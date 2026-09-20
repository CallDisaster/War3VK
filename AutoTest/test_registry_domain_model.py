"""Registry domain 隔离（T7 批次 4 / U5）离线生命周期模型（与 C++ 宿主机测试互补）。

被测对象是**域作用域 registry 的生命周期规则**，与生产 d3d9_device.cpp 的
lookup / publish / GC / reset 控制流同构：

  * lookup 先做 domain 归属校验，跨 domain 命中一律拒绝且不触碰槽位；
  * publish 目标槽位属于另一个 domain 时拒绝，且不分配字节、不覆盖槽位；
  * GC 按 geometryId 淘汰常驻条目，槽位只在"仍属于该条目"时才擦除；
  * reset / domain 作用域清理只清本 domain，其他 domain 的条目、槽位与字节账不变。

与 AutoTest/test_persistent_expiry_queue_model.py 的关系：那份模型只建模**过期**
（惰性堆 vs 全扫描），完全没有 domain 维度。本文件补 domain 维度，并把它与
惰性过期堆放在一起做随机等价。

本模型的 key 刻意是 **domain-agnostic** 的：生产里 domain 会混进 hash 输入，
所以真实运行不应发生跨域碰撞；这里的碰撞用来履行 F4.10 反例义务——
必须能构造一个"隔离前会命中、隔离后不会"的跨域碰撞，否则"隔离"不可证伪。
因此：
  * LegacyRegistry（隔离前的形状）必须真的跨域命中（legacy_cross_domain_hits > 0）；
  * IsolatedRegistry 的每一次命中都必须满足 geometry.domain == 请求 domain，
    否则 cross_domain_leaks 计数递增，测试失败。
"""

import heapq
import random
import unittest


GENERIC = "Generic"
S1 = "S1Terrain"
STAGE13 = "Stage13Exact"
ALL_DOMAINS = (GENERIC, S1, STAGE13)


class IsolatedRegistry:
    """隔离后的形状：显式 domain + owner-check（fail-closed）。"""

    def __init__(self, max_age):
        self.max_age = max_age
        self.frame = 0
        # key -> {"geometry_id", "instances", "domain"}
        self.slots = {}
        # geometry_id -> {"key", "domain", "bytes", "last_seen"}
        self.geometries = {}
        self.queue = []
        self.next_id = 1
        # 可观测拒绝计数（与 C++ War3ShadowPersistentDiagnosticsFrame 同名同义）。
        self.domain_lookup_rejects = 0
        self.domain_publish_rejects = 0
        self.domain_gc_erase_rejects = 0
        self.domain_purge_rejects = 0
        self.cross_domain_leaks = 0

    # --- lookup ---
    def find(self, key, domain):
        slot = self.slots.get(key)
        if slot is None:
            return None
        if slot["domain"] != domain:
            # fail-closed：拒绝命中，且不触碰槽位。
            self.domain_lookup_rejects += 1
            return None
        geometry = self.geometries.get(slot["geometry_id"])
        if geometry is None:
            self._erase_slot(key)
            return None
        if geometry["domain"] != domain:
            # 槽位域与常驻条目域不一致：既不暴露几何，也不销毁任何一方。
            self.domain_lookup_rejects += 1
            return None
        slot["instances"] += 1
        geometry["last_seen"] = self.frame
        hit = slot["geometry_id"]
        # 不变量：返回的几何必须是本 domain 的。任何违例都必须显式可见。
        if self.geometries[hit]["domain"] != domain:
            self.cross_domain_leaks += 1
        return hit

    # --- publish ---
    def publish(self, key, domain, bytes_):
        slot = self.slots.get(key)
        if slot is not None and slot["domain"] != domain:
            self.domain_publish_rejects += 1
            return None
        geometry_id = self.next_id
        self.next_id += 1
        self.geometries[geometry_id] = {
            "key": key,
            "domain": domain,
            "bytes": bytes_,
            "last_seen": self.frame,
        }
        self.slots[key] = {
            "geometry_id": geometry_id,
            "instances": 1,
            "domain": domain,
        }
        heapq.heappush(self.queue, (self.frame, geometry_id))
        return geometry_id

    # --- GC ---
    def _erase_slot(self, key):
        self.slots.pop(key, None)

    def _erase_owned_slot(self, key, geometry_id, domain):
        slot = self.slots.get(key)
        if slot is None:
            return
        if slot["geometry_id"] == geometry_id and slot["domain"] == domain:
            self.slots.pop(key, None)
        else:
            self.domain_gc_erase_rejects += 1

    def erase_geometry(self, geometry_id):
        geometry = self.geometries.pop(geometry_id, None)
        if geometry is None:
            return False
        self._erase_owned_slot(geometry["key"], geometry_id, geometry["domain"])
        return True

    def gc(self, frame):
        self.frame = max(self.frame, frame)
        expired = set()
        stats = {"popped": 0, "requeued": 0, "stale": 0, "age": 0}
        while self.queue:
            token_frame, geometry_id = self.queue[0]
            if frame <= token_frame or frame - token_frame <= self.max_age:
                break
            heapq.heappop(self.queue)
            stats["popped"] += 1
            geometry = self.geometries.get(geometry_id)
            if geometry is None:
                stats["stale"] += 1
                continue
            if geometry["last_seen"] != token_frame:
                heapq.heappush(self.queue, (geometry["last_seen"], geometry_id))
                stats["requeued"] += 1
                continue
            self.erase_geometry(geometry_id)
            expired.add(geometry_id)
            stats["age"] += 1
        return expired, stats

    # --- reset / domain 作用域清理 ---
    def purge_domain(self, domain):
        """domain 作用域清理（Stage13 清理形式）：逐槽位校验归属，跨 domain 跳过。"""
        purged = 0
        for key in list(self.slots):
            slot = self.slots[key]
            if slot["domain"] != domain:
                self.domain_purge_rejects += 1
                continue
            self.erase_geometry(slot["geometry_id"])
            purged += 1
        return purged

    def reset_session(self):
        """整会话退役：move 之前做一次归属校验（不一致槽位先丢弃）。"""
        rejected = 0
        for key in list(self.slots):
            slot = self.slots[key]
            geometry = self.geometries.get(slot["geometry_id"])
            if geometry is not None and geometry["domain"] != slot["domain"]:
                self.slots.pop(key)
                rejected += 1
        self.domain_reset_owner_rejects = rejected
        self.slots = {}
        self.geometries = {}
        self.queue = []
        return rejected

    # --- 视图 ---
    def bytes_of(self, domain):
        return sum(g["bytes"] for g in self.geometries.values() if g["domain"] == domain)

    def geometry_count(self, domain):
        return sum(1 for g in self.geometries.values() if g["domain"] == domain)

    def snapshot(self):
        return (
            sorted((k, v["geometry_id"], v["instances"], v["domain"]) for k, v in self.slots.items()),
            sorted((gid, g["key"], g["domain"], g["bytes"], g["last_seen"]) for gid, g in self.geometries.items()),
        )


class LegacyRegistry:
    """隔离前的形状：只有 key，没有 domain ⇒ 跨域碰撞会直接命中。"""

    def __init__(self):
        self.slots = {}
        self.geometries = {}
        self.next_id = 1
        self.legacy_cross_domain_hits = 0

    def publish(self, key, domain, bytes_):
        geometry_id = self.next_id
        self.next_id += 1
        self.geometries[geometry_id] = {"key": key, "domain": domain, "bytes": bytes_}
        self.slots[key] = {"geometry_id": geometry_id, "domain": domain}
        return geometry_id

    def probe(self, key, requested_domain):
        slot = self.slots.get(key)
        if slot is None:
            return None
        geometry = self.geometries.get(slot["geometry_id"])
        if geometry is None:
            return None
        if geometry["domain"] != requested_domain:
            self.legacy_cross_domain_hits += 1
        return slot["geometry_id"]


class FullScanReference:
    """全扫描过期参照（无惰性堆），用于与 IsolatedRegistry 做随机等价。"""

    def __init__(self, max_age):
        self.max_age = max_age
        self.entries = {}
        self.domains = {}

    def create(self, geometry_id, frame, size, domain):
        self.entries[geometry_id] = [frame, size]
        self.domains[geometry_id] = domain

    def hit(self, geometry_id, frame):
        if geometry_id in self.entries:
            self.entries[geometry_id][0] = frame

    def erase(self, geometry_id):
        self.entries.pop(geometry_id, None)
        self.domains.pop(geometry_id, None)

    def gc(self, frame):
        expired = {
            geometry_id
            for geometry_id, (last_seen, _) in self.entries.items()
            if frame > last_seen and frame - last_seen > self.max_age
        }
        for geometry_id in expired:
            self.erase(geometry_id)
        return expired


class RegistryDomainModelTests(unittest.TestCase):
    def test_same_key_different_domain_never_hits(self):
        model = IsolatedRegistry(max_age=240)
        s1_id = model.publish("K", S1, 4096)
        self.assertIsNotNone(s1_id)
        self.assertIsNone(model.find("K", STAGE13))
        self.assertIsNone(model.find("K", GENERIC))
        self.assertEqual(model.domain_lookup_rejects, 2)
        # 槽位必须原封不动（instances 未加、domain 未变）。
        self.assertEqual(model.slots["K"]["instances"], 1)
        self.assertEqual(model.slots["K"]["domain"], S1)
        # 本域查询照常命中。
        self.assertEqual(model.find("K", S1), s1_id)
        self.assertEqual(model.slots["K"]["instances"], 2)
        self.assertEqual(model.cross_domain_leaks, 0)

    def test_owner_check_rejects_publish_gc_and_purge(self):
        model = IsolatedRegistry(max_age=240)
        s1_id = model.publish("K", S1, 2048)
        stage13_id = model.publish("K2", STAGE13, 1024)

        # publish：跨 domain 拒绝，零副作用。
        self.assertIsNone(model.publish("K", STAGE13, 4096))
        self.assertEqual(model.domain_publish_rejects, 1)
        self.assertEqual(model.bytes_of(S1), 2048)
        self.assertEqual(model.bytes_of(STAGE13), 1024)
        self.assertEqual(model.slots["K"]["domain"], S1)

        # GC：淘汰 S1 的条目只擦 S1 的槽位，Stage13 槽位/字节不受影响。
        self.assertTrue(model.erase_geometry(s1_id))
        self.assertNotIn("K", model.slots)
        self.assertIn("K2", model.slots)
        self.assertEqual(model.geometry_count(STAGE13), 1)
        self.assertEqual(model.bytes_of(STAGE13), 1024)
        self.assertEqual(model.domain_gc_erase_rejects, 0)

        # 槽位被新条目接管后，旧 geometry 的 GC 不得盲擦槽位。
        reused = IsolatedRegistry(max_age=240)
        first = reused.publish("K3", GENERIC, 512)
        reused.erase_geometry(first)
        second = reused.publish("K3", GENERIC, 512)
        self.assertNotEqual(first, second)
        reused.geometries[first] = {"key": "K3", "domain": GENERIC, "bytes": 512, "last_seen": 0}
        self.assertTrue(reused.erase_geometry(first))
        self.assertIn("K3", reused.slots)
        self.assertEqual(reused.slots["K3"]["geometry_id"], second)
        self.assertEqual(reused.domain_gc_erase_rejects, 1)

        # domain 作用域清理：只清本 domain，其他 domain 被跳过并计数。
        generic_id = model.publish("K4", GENERIC, 256)
        self.assertEqual(model.purge_domain(STAGE13), 1)
        self.assertEqual(model.geometry_count(STAGE13), 0)
        self.assertIsNone(model.find("K2", STAGE13))
        self.assertEqual(model.domain_purge_rejects, 1)
        self.assertEqual(model.geometry_count(GENERIC), 1)
        self.assertEqual(model.bytes_of(GENERIC), 256)
        self.assertEqual(model.find("K4", GENERIC), generic_id)

    def test_gc_and_reset_only_touch_own_domain(self):
        model = IsolatedRegistry(max_age=240)
        model.frame = 0
        ids = {
            S1: model.publish("KS1", S1, 1000),
            STAGE13: model.publish("K13", STAGE13, 2000),
            GENERIC: model.publish("KG", GENERIC, 3000),
        }
        # 只有 S1 的条目变旧：GC 之后其他 domain 的几何、槽位、字节账都不变。
        model.frame = 1
        model.find("K13", STAGE13)
        model.find("KG", GENERIC)
        expired, _ = model.gc(241)
        self.assertEqual(expired, {ids[S1]})
        self.assertEqual(model.geometry_count(S1), 0)
        self.assertEqual(model.bytes_of(S1), 0)
        self.assertEqual(model.geometry_count(STAGE13), 1)
        self.assertEqual(model.geometry_count(GENERIC), 1)
        self.assertEqual(model.bytes_of(STAGE13), 2000)
        self.assertEqual(model.bytes_of(GENERIC), 3000)
        self.assertEqual(set(model.slots), {"K13", "KG"})
        self.assertEqual(model.domain_gc_erase_rejects, 0)

        # reset：整会话退役按 domain 分账（各 domain 条目数可分别读出）。
        per_domain_before = {d: model.geometry_count(d) for d in ALL_DOMAINS}
        self.assertEqual(per_domain_before, {GENERIC: 1, S1: 0, STAGE13: 1})
        self.assertEqual(model.reset_session(), 0)
        self.assertEqual(model.snapshot(), ([], []))
        self.assertEqual(model.domain_gc_erase_rejects, 0)

    def test_randomized_equivalence_and_no_cross_domain_leak(self):
        rng = random.Random(0x5331_4301)
        reference = FullScanReference(max_age=240)
        model = IsolatedRegistry(max_age=240)
        next_key = 1
        max_live = 120

        for frame in range(1, 6001):
            expected = reference.gc(frame)
            actual, stats = model.gc(frame)
            self.assertEqual(expected, actual)
            self.assertEqual(
                stats["popped"], stats["requeued"] + stats["stale"] + stats["age"]
            )

            # 域内命中：必须返回同一几何，且只增加该槽位的 instances。
            keys = list(model.slots)
            rng.shuffle(keys)
            for key in keys[: rng.randrange(0, 6)]:
                slot = model.slots.get(key)
                if slot is None:
                    continue
                geometry = model.geometries.get(slot["geometry_id"])
                if geometry is None:
                    continue
                before_instances = slot["instances"]
                hit = model.find(key, geometry["domain"])
                self.assertEqual(hit, slot["geometry_id"])
                self.assertEqual(model.slots[key]["instances"], before_instances + 1)
                reference.hit(slot["geometry_id"], frame)

            # 跨 domain 命中：必须被拒绝，且槽位 instances / domain 与全局计数都不变。
            keys = list(model.slots)
            rng.shuffle(keys)
            for key in keys[: rng.randrange(0, 4)]:
                slot = model.slots[key]
                other = next(d for d in ALL_DOMAINS if d != slot["domain"])
                before_instances = slot["instances"]
                before_counts = tuple(model.geometry_count(d) for d in ALL_DOMAINS)
                self.assertIsNone(model.find(key, other))
                self.assertIsNone(model.find(key, other))
                self.assertEqual(model.slots[key]["instances"], before_instances)
                self.assertEqual(model.slots[key]["domain"], slot["domain"])
                self.assertEqual(
                    tuple(model.geometry_count(d) for d in ALL_DOMAINS), before_counts
                )

            for _ in range(rng.randrange(0, 3)):
                key = f"k{next_key}"
                next_key += 1
                domain = ALL_DOMAINS[rng.randrange(0, len(ALL_DOMAINS))]
                size = rng.randrange(64, 1 << 16)
                geometry_id = model.publish(key, domain, size)
                if geometry_id is None:
                    continue
                reference.create(geometry_id, frame, size, domain)

            # 跨 domain publish：必须被拒绝且零副作用。
            if model.slots and frame % 7 == 0:
                key = rng.choice(list(model.slots))
                slot = model.slots[key]
                other = next(d for d in ALL_DOMAINS if d != slot["domain"])
                before = model.snapshot()
                self.assertIsNone(model.publish(key, other, 4096))
                self.assertEqual(model.snapshot(), before)

            if frame % 17 == 0 and model.geometries:
                victims = sorted(
                    model.geometries,
                    key=lambda gid: model.geometries[gid]["last_seen"],
                )[: rng.randrange(0, 3)]
                for geometry_id in victims:
                    model.erase_geometry(geometry_id)
                    reference.erase(geometry_id)

            while len(model.geometries) > max_live:
                victim = min(
                    model.geometries,
                    key=lambda gid: model.geometries[gid]["last_seen"],
                )
                model.erase_geometry(victim)
                reference.erase(victim)

            self.assertEqual(
                {gid: entry[0] for gid, entry in reference.entries.items()},
                {gid: g["last_seen"] for gid, g in model.geometries.items()},
            )
            for domain in ALL_DOMAINS:
                expected_bytes = sum(
                    entry[1]
                    for gid, entry in reference.entries.items()
                    if reference.domains[gid] == domain
                )
                self.assertEqual(model.bytes_of(domain), expected_bytes)

        self.assertEqual(model.cross_domain_leaks, 0)
        # 过期堆的惰性令牌不得少于存活条目。
        self.assertGreaterEqual(len(model.queue), len(model.geometries))

    def test_legacy_model_exposes_the_counterexample(self):
        """F4.10：隔离前的形状必须真的跨域命中，否则"隔离"不可证伪。"""
        key = "collision-key"
        legacy = LegacyRegistry()
        s1_id = legacy.publish(key, S1, 4096)
        self.assertEqual(legacy.probe(key, STAGE13), s1_id)
        self.assertEqual(legacy.legacy_cross_domain_hits, 1)
        self.assertEqual(legacy.probe(key, GENERIC), s1_id)
        self.assertEqual(legacy.legacy_cross_domain_hits, 2)
        # 相同构造在隔离后的模型上不再命中。
        isolated = IsolatedRegistry(max_age=240)
        isolated.publish(key, S1, 4096)
        self.assertIsNone(isolated.find(key, STAGE13))
        self.assertIsNone(isolated.find(key, GENERIC))
        self.assertEqual(isolated.cross_domain_leaks, 0)


if __name__ == "__main__":
    unittest.main()
