#!/usr/bin/env python3
"""Registry domain 拒绝计数出口**定向测试**（增量正确性 + 稳态为 0）。

性质（如实声明，不得误读）：
  * 被测的形状来自**生产源文本**：device.cpp 的累加点及其所属函数、
    device.cpp -> PersistentGeometryFrameStats 的 bridge 拷贝、
    perf_monitor.cpp 的 per-frame 拷贝与区间累加、两个 JSON 写手的键/取值。
    任何一处被删除、改名或写成**另一个字段**（串线），本测试即红。
  * 本测试**不执行** C++ 出口代码：War3PerfMonitor 依赖 d3d9/DXVK 设备侧类型，
    不是宿主机可链接目标（见 docs/plan/2026-09-18-registry-domain-count-export-record.md
    §未覆盖项）。因此这里是对同一份映射做**按源码锚定的增量模型**，
    与静态门禁 test_registry_domain_count_export_static.py 互补：
    静态门禁钉"处数"，本测试钉"每条路径 -> 每个桶 -> 导出值"的增量与恒等映射。

覆盖的定向场景（lookup / publish / GC / reset 各一条路径 + 稳态）：
  S1 纯域内稳态                     -> 6 个计数全部为 0（无越权即稳态 0）
  S2 lookup 跨域命中拒绝            -> domainLookupRejects +1，其余 0
  S3 publish 跨域拒绝（ShadowCapture 调用方）
                                    -> domainPublishRejects +1 且 rejectDomainConflict +1
  S4 publish 跨域拒绝（Semantic/UpperLayer 调用方）
                                    -> 只有 domainPublishRejects +1
  S5 GC / 预算回收跨域拒绝          -> domainGcEraseRejects +1
  S6 reset 整会话归属校验           -> domainResetOwnerRejects +1
  S7 Stage13 domain 作用域 clear 两个站点
                                    -> domainResetPurgeRejects +2
并且校验 legacy persistentRejectCreateOrBudgetDetailedTotal 只吸收
rejectDomainConflict（create 失败分桶），不吸收另外 5 个。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
PM_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"

for path in (DEVICE_CPP, PM_CPP):
    if not path.is_file():
        raise AssertionError(f"fail-closed: 缺少必需文件 {path}")

DEVICE_CPP_TEXT = DEVICE_CPP.read_text(encoding="utf-8", errors="replace")
PM_CPP_TEXT = PM_CPP.read_text(encoding="utf-8", errors="replace")
DEVICE_NORM = re.sub(r"\s+", " ", DEVICE_CPP_TEXT)
PM_NORM = re.sub(r"\s+", " ", PM_CPP_TEXT)

FIELDS = (
    "rejectDomainConflict",
    "domainLookupRejects",
    "domainPublishRejects",
    "domainGcEraseRejects",
    "domainResetPurgeRejects",
    "domainResetOwnerRejects",
)
AGGREGATE = {
    "rejectDomainConflict": "persistentRejectDomainConflict",
    "domainLookupRejects": "persistentDomainLookupRejects",
    "domainPublishRejects": "persistentDomainPublishRejects",
    "domainGcEraseRejects": "persistentDomainGcEraseRejects",
    "domainResetPurgeRejects": "persistentDomainResetPurgeRejects",
    "domainResetOwnerRejects": "persistentDomainResetOwnerRejects",
}
# 冻结的**静态处数**口径（字段 -> 所属函数 -> 该函数内 `++` 处数）。
# 同一函数内的多处是**互斥分支**（同一次事件只会命中一处），
# 因此"每次事件增量"恒为 1；不同函数各自代表一个独立站点（如两处 purge）。
SITE_COUNTS = {
    "War3TryFindShadowPersistentGeometry": {"domainLookupRejects": 2},
    "War3CreateShadowPersistentGeometryAfterMiss": {"domainPublishRejects": 1},
    "War3TryCaptureShadowCaster": {"rejectDomainConflict": 1},
    "War3GcShadowPersistentGeometry": {"domainGcEraseRejects": 1},
    "War3ResetShadowSessionState": {"domainResetOwnerRejects": 1},
    "War3MaybeInsertBeforeUi": {"domainResetPurgeRejects": 1},
    "War3DrainShadowCasterTombstones": {"domainResetPurgeRejects": 1},
}

WRITER_A_START = 'json << "  \\"shadowBudgetSummary\\": {\\n";'
WRITER_A_END = 'json << "  \\"topShadowOffenders\\": [\\n";'
WRITER_B_START = 'json << "  \\"shadowRuntimeV2Summary\\": {\\n";'
WRITER_B_END = 'json << "  \\"publishVisibleSafeCopyVerifier\\": {\\n";'


def enclosing_function(source, position):
    head = source[:position]
    pattern = re.compile(
        r"^(?:[A-Za-z_][\w:<>,*&\s]*?\s+)?(\w+)::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{",
        re.M,
    )
    last = None
    for match in pattern.finditer(head):
        last = match
    if last is None:
        raise AssertionError("no enclosing function found")
    return last.group(2)


def source_increments():
    """从 device.cpp 读出 `字段++` 的所属函数 -> 处数（生产口径）。"""
    result = {}
    for field in FIELDS:
        for match in re.finditer(re.escape(field) + r"\+\+", DEVICE_CPP_TEXT):
            function = enclosing_function(DEVICE_CPP_TEXT, match.start())
            result.setdefault(function, {}).setdefault(field, 0)
            result[function][field] += 1
    return result


def bridge_mapping():
    pattern = re.compile(r"stats\.(\w+) = completed\.(\w+);")
    return {m.group(1): m.group(2)
            for m in pattern.finditer(DEVICE_NORM) if m.group(1) in FIELDS}


def per_frame_mapping():
    pattern = re.compile(r"m_currentFrameWorkload\.(\w+) = stats\.(\w+);")
    return {m.group(1): m.group(2)
            for m in pattern.finditer(PM_NORM) if m.group(1) in FIELDS}


def aggregate_mapping():
    pattern = re.compile(r"agg\.(\w+) \+= stats\.(\w+);")
    return {m.group(2): m.group(1)
            for m in pattern.finditer(PM_NORM) if m.group(2) in FIELDS}


def writer_regions():
    a = PM_CPP_TEXT[PM_CPP_TEXT.index(WRITER_A_START):
                    PM_CPP_TEXT.index(WRITER_A_END)]
    b = PM_CPP_TEXT[PM_CPP_TEXT.index(WRITER_B_START):
                    PM_CPP_TEXT.index(WRITER_B_END)]
    return {"shadowBudgetSummary": a, "shadowRuntimeV2Summary": b}


def assert_json_export(region, aggregate_name):
    key = f'\\"{aggregate_name}\\": '
    if region.count(key) != 1:
        raise AssertionError(f"{aggregate_name}: JSON 键数 != 1")
    if region.count(f"shadowAgg.{aggregate_name} <<") != 1:
        raise AssertionError(f"{aggregate_name}: JSON 取值表达式数 != 1")


def detailed_total_fields(region):
    total = region[region.index("persistentRejectCreateOrBudgetDetailedTotal"):]
    total = total[:total.index('<< ",\\n";')]
    return re.findall(r"shadowAgg\.(\w+)", total)


def apply_interval(events):
    """events: {计数字段所属函数: 触发次数} -> 当帧 War3ShadowPersistentDiagnosticsFrame。

    每处 `++` 每次事件恰好 +1；同一函数内的互斥分支因此只 +1。
    """
    frame = {field: 0 for field in FIELDS}
    for function, times in events.items():
        if function not in SITE_COUNTS:
            raise AssertionError(f"未登记的累加函数 {function}")
        for field in SITE_COUNTS[function]:
            frame[field] += times
    return frame


def export_interval(frame, aggregate, json_ok):
    """按生产源的恒等映射把当帧帧结构导出成聚合累加量。"""
    bridge = bridge_mapping()
    per_frame = per_frame_mapping()
    for field in FIELDS:
        if bridge.get(field) != field:
            raise AssertionError(f"bridge 未把 {field} 恒等传出：{bridge.get(field)}")
        if per_frame.get(field) != field:
            raise AssertionError(
                f"per-frame 未把 {field} 恒等传出：{per_frame.get(field)}")
        if field not in aggregate:
            raise AssertionError(f"{field} 没有区间累加")
        if not json_ok.get(field):
            raise AssertionError(f"{field} 没有 JSON 出口")
    return {aggregate[field]: frame[field] for field in FIELDS}


class RegistryDomainCountExportTest(unittest.TestCase):
    def test_increment_site_counts_match_source(self):
        self.assertEqual(source_increments(), SITE_COUNTS,
                         "device.cpp 的累加站点/所属函数与冻结口径不符")

    def test_export_wiring_is_an_identity_mapping(self):
        bridge = bridge_mapping()
        per_frame = per_frame_mapping()
        aggregate = aggregate_mapping()
        regions = writer_regions()
        self.assertEqual(set(bridge), set(FIELDS), "bridge 拷贝集合不完整")
        self.assertEqual(set(per_frame), set(FIELDS), "per-frame 拷贝集合不完整")
        self.assertEqual(set(aggregate), set(FIELDS), "区间累加集合不完整")
        for field in FIELDS:
            self.assertEqual(bridge[field], field, "bridge 字段串线")
            self.assertEqual(per_frame[field], field, "per-frame 字段串线")
            self.assertEqual(aggregate[field], AGGREGATE[field], "聚合字段名不符")
            for label, region in regions.items():
                assert_json_export(region, AGGREGATE[field])
                del label

    def test_legacy_detailed_total_only_absorbs_reject_domain_conflict(self):
        for label, region in writer_regions().items():
            names = detailed_total_fields(region)
            self.assertIn("persistentRejectDomainConflict", names, label)
            for field in FIELDS:
                if field == "rejectDomainConflict":
                    continue
                self.assertNotIn(
                    AGGREGATE[field], names,
                    f"{label}: {field} 不得进入 create 失败明细求和")

    def test_directed_scenarios_increment_exactly(self):
        aggregate = aggregate_mapping()
        regions = writer_regions()
        json_ok = {}
        for field in FIELDS:
            assert_json_export(regions["shadowBudgetSummary"], AGGREGATE[field])
            assert_json_export(regions["shadowRuntimeV2Summary"], AGGREGATE[field])
            json_ok[field] = True

        zero = {AGGREGATE[field]: 0 for field in FIELDS}
        scenarios = (
            ("S1-steady-state-no-cross-domain-touch", {}, {}),
            ("S2-lookup-cross-domain-hit",
             {"War3TryFindShadowPersistentGeometry": 1},
             {"domainLookupRejects": 1}),
            ("S3-publish-cross-domain-shadowcapture",
             {"War3CreateShadowPersistentGeometryAfterMiss": 1,
              "War3TryCaptureShadowCaster": 1},
             {"domainPublishRejects": 1, "rejectDomainConflict": 1}),
            ("S4-publish-cross-domain-semantic-caller",
             {"War3CreateShadowPersistentGeometryAfterMiss": 1},
             {"domainPublishRejects": 1}),
            ("S5-gc-budget-reclaim-cross-domain",
             {"War3GcShadowPersistentGeometry": 1},
             {"domainGcEraseRejects": 1}),
            ("S6-reset-session-owner-mismatch",
             {"War3ResetShadowSessionState": 1},
             {"domainResetOwnerRejects": 1}),
            ("S7-stage13-domain-scope-purge",
             {"War3MaybeInsertBeforeUi": 1,
              "War3DrainShadowCasterTombstones": 1},
             {"domainResetPurgeRejects": 2}),
        )
        for label, events, expected in scenarios:
            first_frame = export_interval(apply_interval(events), aggregate, json_ok)
            second_frame = export_interval(apply_interval({}), aggregate, json_ok)
            want = {AGGREGATE[field]: expected.get(field, 0) for field in FIELDS}
            # 区间累计 = 发生区间 + 无越权区间（后者必须全 0）。
            self.assertEqual(second_frame, zero, f"{label}: 无越权区间未归零")
            self.assertEqual(first_frame, want, f"{label}: 区间增量不符")
            accumulated = {k: first_frame[k] + second_frame[k] for k in zero}
            self.assertEqual(accumulated, want, f"{label}: 区间累计不符")

    def test_legacy_detailed_total_arithmetic_reproduces_the_bucket_sum(self):
        aggregate = aggregate_mapping()
        json_ok = {field: True for field in FIELDS}
        for label, region in writer_regions().items():
            names = detailed_total_fields(region)
            conflicts = export_interval(
                apply_interval({"War3TryCaptureShadowCaster": 3}),
                aggregate, json_ok)
            self.assertEqual(sum(conflicts.get(name, 0) for name in names), 3,
                             f"{label}: create 失败明细求和不等于域冲突数")
            others = export_interval(
                apply_interval({
                    "War3TryFindShadowPersistentGeometry": 5,
                    "War3GcShadowPersistentGeometry": 5,
                    "War3ResetShadowSessionState": 5,
                    "War3MaybeInsertBeforeUi": 5,
                    "War3DrainShadowCasterTombstones": 5,
                }),
                aggregate, json_ok)
            self.assertEqual(
                sum(others.get(name, 0) for name in names), 0,
                f"{label}: 非 create 桶污染了 legacy 明细求和")


if __name__ == "__main__":
    unittest.main()
