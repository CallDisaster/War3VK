#!/usr/bin/env python3
"""Registry domain 隔离（T7 批次 4 / U5）静态门禁（fail-closed）。

判据来源：docs/plan/2026-09-18-t7-registry-stage13-survey.md §4 F1/F2 的可离线部分，
以及主线程裁定（形态 (a)：registry 条目上显式 domain + owner-check，
取值沿用既有隐式 tag，语义不变）。

本门禁钉死的是**代码形状**：
  1. 显式 domain 类型存在，取值逐位等于历史隐式 tag 且互不相同；
  2. 领域常数只允许出现在 war3_shadow_geometry_domain.h，device.cpp 不得再有裸魔法数；
  3. registry 条目 / 常驻条目 / Stage13 条目都带显式 domain 字段；
  4. registry 的三个入口（lookup / find-or-create / create-after-miss）**每一个**
     调用点都携带 domain（F1-3）；
  5. publish / GC / reset / domain 作用域 clear 四条路径都有 owner-check 落点，
     且拒绝计数写入 War3ShadowPersistentDiagnosticsFrame；
  6. Stage13 常驻容器的 domain 作用域清理不得再用无条件 clear()；
  7. 既有 Stage13 准入默认值不变（不改默认值是硬约束）；
  8. 宿主机边界测试与离线模型存在且覆盖 F4.10 反例义务。

fail-closed：任一必需文件缺失、任一锚点文本找不到、或调用点计数与冻结值不符，
直接失败；不允许"解析不出就算通过"。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
DOMAIN_H = ROOT / "src/d3d9/war3/shadow/war3_shadow_geometry_domain.h"
HOST_TEST = ROOT / "src/d3d9/war3/render/tests/war3_shadow_geometry_domain_test.cpp"
MESON = ROOT / "src/d3d9/meson.build"
MODEL = ROOT / "AutoTest/test_registry_domain_model.py"

def strip_cpp_comments(source: str) -> str:
    """去掉 // 行注释与 /* */ 块注释（保留字符串字面量以外的代码文本）。

    只用于"裸魔法数不得出现在调用点"这类检查：注释里为了溯源写出历史常数是
    允许的（例如"数值逐位不变（0x53310001）"），代码里不允许。
    """
    out = []
    index = 0
    size = len(source)
    while index < size:
        char = source[index]
        if char == "/" and index + 1 < size and source[index + 1] == "/":
            newline = source.find("\n", index)
            index = size if newline < 0 else newline
            continue
        if char == "/" and index + 1 < size and source[index + 1] == "*":
            end = source.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


REQUIRED_FILES = (DEVICE_H, DEVICE_CPP, DOMAIN_H, HOST_TEST, MESON, MODEL)
for path in REQUIRED_FILES:
    if not path.is_file():
        raise AssertionError(f"fail-closed: 缺少必需文件 {path}")

DEVICE_H_TEXT = DEVICE_H.read_text(encoding="utf-8", errors="replace")
DEVICE_CPP_TEXT = DEVICE_CPP.read_text(encoding="utf-8", errors="replace")
DEVICE_CPP_CODE = strip_cpp_comments(DEVICE_CPP_TEXT)
DEVICE_H_CODE = strip_cpp_comments(DEVICE_H_TEXT)
DOMAIN_H_TEXT = DOMAIN_H.read_text(encoding="utf-8", errors="replace")
HOST_TEST_TEXT = HOST_TEST.read_text(encoding="utf-8", errors="replace")
MESON_TEXT = MESON.read_text(encoding="utf-8", errors="replace")
MODEL_TEXT = MODEL.read_text(encoding="utf-8", errors="replace")

# 历史隐式 tag（A 树/B 树共用的既有语义，数值不得改变）。
HISTORIC_S1_TAG = 0x53310001
HISTORIC_STAGE13_TAG = 0x53314301

# registry 三个入口在 device.cpp 里的出现次数（1 个定义 + N 个调用点）。
# 该计数是**冻结值**：新增或删除调用点必须同步更新本门禁（见 F1-3）。
FROZEN_ENTRY_SITES = {
    "War3TryFindShadowPersistentGeometry": 5,
    "War3FindOrCreateShadowPersistentGeometry": 2,
    "War3CreateShadowPersistentGeometryAfterMiss": 4,
}

# m_war3ShadowGeometryRegistry 的触碰点计数（find / erase / operator[] / 整体清空）。
FROZEN_REGISTRY_TOUCHES = {
    "m_war3ShadowGeometryRegistry.find(": 3,
    "m_war3ShadowGeometryRegistry.erase(": 3,
    "m_war3ShadowGeometryRegistry[key]": 1,
    "m_war3ShadowGeometryRegistry = {}": 1,
}


def braced_block(source: str, marker: str, start: int = 0) -> str:
    marker_pos = source.index(marker, start)
    open_pos = source.index("{", marker_pos)
    depth = 0
    for pos in range(open_pos, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[marker_pos : pos + 1]
    raise AssertionError(f"unterminated block after {marker}")


def paren_args(source: str, open_paren_pos: int) -> str:
    depth = 0
    for pos in range(open_paren_pos, len(source)):
        if source[pos] == "(":
            depth += 1
        elif source[pos] == ")":
            depth -= 1
            if depth == 0:
                return source[open_paren_pos : pos + 1]
    raise AssertionError("unterminated call arguments")


def entry_call_sites(source: str, name: str) -> list:
    """返回 name( ... ) 的实参文本（含定义/声明站点，用于计数与 domain 检查）。"""
    sites = []
    for match in re.finditer(re.escape(name) + r"\s*\(", source):
        open_paren = source.index("(", match.start())
        sites.append(paren_args(source, open_paren))
    return sites


class RegistryDomainIsolationStaticTest(unittest.TestCase):
    def test_domain_type_and_historic_tag_values(self) -> None:
        self.assertIn("enum class ShadowGeometryDomain : uint32_t", DOMAIN_H_TEXT)
        for token in (
            "Generic = 0u",
            "S1Terrain = 0x53310001u",
            "Stage13Exact = 0x53314301u",
        ):
            self.assertIn(token, DOMAIN_H_TEXT)
        self.assertIn(
            "constexpr uint32_t ShadowGeometryDomainTag(ShadowGeometryDomain domain)",
            DOMAIN_H_TEXT,
        )
        # 取值必须逐位等于历史隐式 tag（"语义不变"的可复核形式）。
        values = {
            name: int(value, 0)
            for name, value in re.findall(
                r"(Generic|S1Terrain|Stage13Exact) = (0x[0-9A-Fa-f]+|\d+)u",
                DOMAIN_H_TEXT,
            )
        }
        self.assertEqual(set(values), {"Generic", "S1Terrain", "Stage13Exact"})
        self.assertEqual(values["S1Terrain"], HISTORIC_S1_TAG)
        self.assertEqual(values["Stage13Exact"], HISTORIC_STAGE13_TAG)
        self.assertEqual(values["Generic"], 0)
        self.assertEqual(
            len({values["Generic"], values["S1Terrain"], values["Stage13Exact"]}), 3
        )

    def test_owner_check_kernel_is_fail_closed(self) -> None:
        kernel = braced_block(DOMAIN_H_TEXT, "inline ShadowGeometryOwnerDecision DecideShadowGeometryOwner(")
        self.assertIn("expectedDomain != entryDomain", kernel)
        self.assertIn("ShadowGeometryOwnerDecision::DomainMismatch", kernel)
        self.assertIn("ShadowGeometryOwnerDecision::GeometryMismatch", kernel)
        # domain 不一致必须在 geometryId 之前判定。
        self.assertLess(
            kernel.index("expectedDomain != entryDomain"),
            kernel.index("expectedGeometryId != 0u"),
        )
        self.assertIn("inline bool ShadowGeometryOwnerAccepts(", DOMAIN_H_TEXT)

    def test_domain_literals_only_live_in_the_domain_header(self) -> None:
        # 只检查**代码**：注释里为了溯源写出历史常数是允许的。
        for raw in ("0x53310001u", "0x53314301u", "0x53310001", "0x53314301"):
            self.assertNotIn(
                raw, DEVICE_CPP_CODE, f"device.cpp 代码里仍出现裸 domain 常数 {raw}"
            )
            self.assertNotIn(
                raw, DEVICE_H_CODE, f"device.h 代码里仍出现裸 domain 常数 {raw}"
            )
        # device.cpp 必须经由显式 domain 取值取 tag（S1 站点 + Stage13 tag 常数）。
        self.assertEqual(
            DEVICE_CPP_CODE.count("dxvk::war3::shadow::ShadowGeometryDomainTag("), 2
        )
        self.assertEqual(DEVICE_CPP_CODE.count("ShadowGeometryDomain::S1Terrain"), 2)
        # Stage13Exact 的代码站点：tag 常数 1 + 两处 domain 作用域清理 2 + 写入条目 1。
        self.assertEqual(
            DEVICE_CPP_CODE.count("ShadowGeometryDomain::Stage13Exact"), 4
        )

    def test_registry_entries_carry_explicit_domain(self) -> None:
        registry_entry = braced_block(DEVICE_H_TEXT, "struct War3ShadowGeometryRegistryEntry {")
        self.assertIn("ShadowGeometryDomain domain", registry_entry)
        persistent_entry = braced_block(DEVICE_H_TEXT, "struct War3ShadowPersistentGeometryEntry {")
        self.assertIn("ShadowGeometryDomain domain", persistent_entry)
        stage13_entry = braced_block(DEVICE_H_TEXT, "struct War3Stage13RetainedCasterEntry {")
        self.assertIn("ShadowGeometryDomain domain", stage13_entry)
        self.assertIn("ShadowGeometryDomain::Stage13Exact", stage13_entry)
        # 发布路径必须同源写入两个 domain 字段。
        self.assertIn("entry.domain = domain;", DEVICE_CPP_TEXT)
        self.assertIn("registryEntry.domain = domain;", DEVICE_CPP_TEXT)
        self.assertIn("retained.domain =", DEVICE_CPP_TEXT)

    def test_every_registry_entry_point_carries_domain(self) -> None:
        for name, expected in FROZEN_ENTRY_SITES.items():
            sites = entry_call_sites(DEVICE_CPP_TEXT, name)
            self.assertEqual(
                len(sites), expected,
                f"{name} 站点数 {len(sites)} != 冻结值 {expected}（新增/删除调用点必须同步本门禁）",
            )
            for index, args in enumerate(sites):
                self.assertIn(
                    "domain", args.lower(),
                    f"{name} 第 {index} 个站点未携带 domain：{args[:160]}",
                )

    def test_registry_container_touch_points_are_frozen(self) -> None:
        for token, expected in FROZEN_REGISTRY_TOUCHES.items():
            found = DEVICE_CPP_TEXT.count(token)
            self.assertEqual(
                found, expected,
                f"{token} 触碰点 {found} != 冻结值 {expected}（新增直连容器必须同步本门禁）",
            )

    def test_lookup_owner_check_rejects_without_touching_the_slot(self) -> None:
        body = braced_block(
            DEVICE_CPP_TEXT,
            "bool D3D9DeviceEx::War3TryFindShadowPersistentGeometry(",
        )
        self.assertIn("DecideShadowGeometryOwner(domain, 0u, regIt->second.domain", body)
        self.assertIn("domainLookupRejects++", body)
        self.assertIn("ShadowGeometryOwnerAccepts(", body)
        # 拒绝分支必须早于 instances++ / lastSeen 刷新。
        self.assertLess(
            body.index("DecideShadowGeometryOwner(domain, 0u, regIt->second.domain"),
            body.index("regIt->second.instances++"),
        )
        self.assertIn("m_war3ShadowGeometryRegistry.find(key)", body)

    def test_publish_owner_check_precedes_allocation_and_gc(self) -> None:
        body = braced_block(
            DEVICE_CPP_TEXT,
            "bool D3D9DeviceEx::War3CreateShadowPersistentGeometryAfterMiss(",
        )
        self.assertIn("existingSlotIt", body)
        self.assertIn("domainPublishRejects++", body)
        self.assertIn("War3ShadowPersistentCreateFailure::DomainConflict", body)
        self.assertLess(
            body.index("domainPublishRejects++"),
            body.index("War3GcShadowPersistentGeometry();"),
        )
        self.assertLess(
            body.index("domainPublishRejects++"),
            body.index("War3CreateShadowPersistentBuffer("),
        )
        # 新失败值必须同时进入 ShadowCapture 的失败分桶 switch（否则 -Wswitch / 计数丢失）。
        self.assertIn("enum class War3ShadowPersistentCreateFailure : uint8_t", DEVICE_H_TEXT)
        failure_enum = braced_block(DEVICE_H_TEXT, "enum class War3ShadowPersistentCreateFailure : uint8_t {")
        self.assertIn("DomainConflict,", failure_enum)
        self.assertIn("case War3ShadowPersistentCreateFailure::DomainConflict:", DEVICE_CPP_TEXT)
        self.assertIn("diagnostics.rejectDomainConflict++;", DEVICE_CPP_TEXT)

    def test_gc_owner_check_gates_every_registry_erase(self) -> None:
        body = braced_block(DEVICE_CPP_TEXT, "void D3D9DeviceEx::War3GcShadowPersistentGeometry()")
        self.assertIn("eraseOwnedRegistrySlot", body)
        self.assertIn("domainGcEraseRejects++", body)
        self.assertIn("DecideShadowGeometryOwner(", body)
        # 两个回收路径（过期队列 + 预算回收）都必须走该 owner-check。
        self.assertEqual(body.count("eraseOwnedRegistrySlot("), 2)  # 2 个回收路径各一次
        self.assertNotIn("m_war3ShadowGeometryRegistry.erase(entry.key)", body)
        self.assertNotIn("m_war3ShadowGeometryRegistry.erase(entryIt->second.key)", body)

    def test_reset_and_domain_scoped_clear_are_owner_checked(self) -> None:
        reset = braced_block(
            DEVICE_CPP_TEXT, "void D3D9DeviceEx::War3ResetShadowSessionState(uint64_t retireSerial)"
        )
        self.assertIn("domainResetOwnerRejects++", reset)
        self.assertIn("ShadowGeometryOwnerAccepts(", reset)
        # move 之前的归属校验必须早于容器 move。
        self.assertLess(
            reset.index("domainResetOwnerRejects++"),
            reset.index("retired.geometryRegistry = std::move(m_war3ShadowGeometryRegistry);"),
        )
        # Stage13 容器不得再用无条件 clear()；两处清理都必须是 domain 作用域。
        self.assertNotIn("m_war3Stage13RetainedCasters.clear()", DEVICE_CPP_TEXT)
        self.assertEqual(DEVICE_CPP_TEXT.count("domainResetPurgeRejects++"), 2)
        self.assertEqual(
            DEVICE_CPP_TEXT.count(
                "dxvk::war3::shadow::ShadowGeometryDomain::Stage13Exact, 0u,"
            ),
            2,
        )

    def test_diagnostics_declare_domain_reject_counters(self) -> None:
        frame = braced_block(DEVICE_H_TEXT, "struct War3ShadowPersistentDiagnosticsFrame {")
        for field in (
            "uint64_t rejectDomainConflict = 0;",
            "uint64_t domainLookupRejects = 0;",
            "uint64_t domainPublishRejects = 0;",
            "uint64_t domainGcEraseRejects = 0;",
            "uint64_t domainResetPurgeRejects = 0;",
            "uint64_t domainResetOwnerRejects = 0;",
        ):
            self.assertIn(field, frame)
        for increment in (
            "domainLookupRejects++",
            "domainPublishRejects++",
            "rejectDomainConflict++",
            "domainGcEraseRejects++",
            "domainResetPurgeRejects++",
            "domainResetOwnerRejects++",
        ):
            self.assertIn(increment, DEVICE_CPP_TEXT)

    def test_existing_stage13_admission_defaults_are_untouched(self) -> None:
        # 硬约束：不改 Stage13 准入默认值、不加 env。
        for token in (
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION", 0u',
            '"DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY", 0u',
            '"DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE", 0u',
            '"DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE", 0u',
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION_FRAMES", 240u',
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION_CAP", 64u',
        ):
            self.assertIn(token, DEVICE_CPP_TEXT)
        # 本块不得引入任何新的 DXVK_WAR3_* env。
        self.assertNotIn("DXVK_WAR3_REGISTRY_DOMAIN", DEVICE_CPP_TEXT)
        self.assertNotIn("DXVK_WAR3_REGISTRY_DOMAIN", DEVICE_H_TEXT)
        self.assertNotIn("DXVK_WAR3_REGISTRY_DOMAIN", DOMAIN_H_TEXT)

    def test_host_test_and_offline_model_exist_and_cover_the_counterexample(self) -> None:
        self.assertIn("war3_shadow_geometry_domain_test = executable(", MESON_TEXT)
        self.assertIn("'war3_shadow_geometry_domain',", MESON_TEXT)
        self.assertIn(
            "war3/render/tests/war3_shadow_geometry_domain_test.cpp", MESON_TEXT
        )
        self.assertIn(
            '#include "../../shadow/war3_shadow_geometry_domain.h"', HOST_TEST_TEXT
        )
        # F4.10：必须同时存在"隔离前会命中"与"隔离后拒绝"两条证据。
        self.assertIn("C4.counterexample.legacy-cross-domain-hit", HOST_TEST_TEXT)
        self.assertIn("C4.counterexample.isolated-cross-domain-rejected", HOST_TEST_TEXT)
        self.assertIn("C6.gc.other-domain-untouched", HOST_TEST_TEXT)
        self.assertIn("C7.purge.only-stage13-removed", HOST_TEST_TEXT)
        self.assertIn("C3.s1-layout-hash-unchanged", HOST_TEST_TEXT)
        self.assertIn("C3.stage13-source-hash-unchanged", HOST_TEST_TEXT)
        # 离线模型必须检查"跨 domain 从不互相命中"这一不变量。
        self.assertIn("cross_domain_leaks", MODEL_TEXT)
        self.assertIn("legacy_cross_domain_hits", MODEL_TEXT)


if __name__ == "__main__":
    unittest.main()
