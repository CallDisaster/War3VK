#!/usr/bin/env python3
"""Registry domain 拒绝计数**对外出口**静态门禁（T7 批次 4 / U5 缺口修补，fail-closed）。

判据来源：
  * docs/plan/2026-09-18-registry-domain-isolation-record.md §8-2 / §9-1
    （6 个 domain 拒绝计数只写进 War3ShadowPersistentDiagnosticsFrame，
     没有接进 War3PerfMonitor 的对外 dump）；
  * docs/plan/2026-09-18-m2-5-taxonomy-and-remaining-scope.md §2.2
    （照既有 persistent-geometry 拒绝计数族的读取路径接，不新创通道）。

本门禁钉死的是**出口链的形状与口径**，每段都给出**允许处数**（恰好一处，
或明确登记的合法处数）：
  1. 当帧定义：d3d9_device.h 的 War3ShadowPersistentDiagnosticsFrame（每字段 1）；
     device.cpp **不得**重复定义（每字段 0）；
  2. 累加点：device.cpp 的 `NAME++`（lookup/purge 各 2，其余各 1），
     并且每个累加点的**所属函数**被钉死（口径：哪条路径记哪个桶）；
  3. bridge 传递：device.cpp 把 completed 区间拷进
     War3PerfMonitor::PersistentGeometryFrameStats（每字段 1）；
  4. perf monitor：perf_monitor.h 的 FrameWorkloadSnapshot /
     PersistentGeometryFrameStats / ShadowBudgetAggregate 各 1；
     perf_monitor.cpp 的 per-frame 拷贝 1 + 区间累加 1；
  5. JSON 写手：shadowBudgetSummary 与 shadowRuntimeV2Summary 两个段
     各恰好 1 个键、1 个取值表达式；workloadSeriesColumns 1 列 +
     workloadSeries 行 1 个值；
  6. 口径：rejectDomainConflict 属于 legacy persistentRejectCreateOrBudget 的
     ShadowCapture 失败分桶，必须进入 persistentRejectCreateOrBudgetDetailedTotal；
     另外 5 个**不得**进入该求和；
  7. 硬约束：本块不得引入任何新 env（冻结被触碰文件的 DXVK_WAR3_ 字面量数）。

fail-closed：任一必需文件缺失、任一锚点找不到、任一处数与冻结值不符即失败。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
PM_H = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
PM_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"

REQUIRED = (DEVICE_H, DEVICE_CPP, PM_H, PM_CPP)
for path in REQUIRED:
    if not path.is_file():
        raise AssertionError(f"fail-closed: 缺少必需文件 {path}")

DEVICE_H_TEXT = DEVICE_H.read_text(encoding="utf-8", errors="replace")
DEVICE_CPP_TEXT = DEVICE_CPP.read_text(encoding="utf-8", errors="replace")
PM_H_TEXT = PM_H.read_text(encoding="utf-8", errors="replace")
PM_CPP_TEXT = PM_CPP.read_text(encoding="utf-8", errors="replace")

# 多行赋值按空白归一化后再数（换行只影响可读性，不影响处数）。
DEVICE_CPP_NORM = re.sub(r"\s+", " ", DEVICE_CPP_TEXT)
PM_CPP_NORM = re.sub(r"\s+", " ", PM_CPP_TEXT)

# (device 侧字段名, perf monitor 聚合字段名, device.cpp 允许的 `++` 处数)
FIELDS = (
    ("rejectDomainConflict", "persistentRejectDomainConflict", 1),
    ("domainLookupRejects", "persistentDomainLookupRejects", 2),
    ("domainPublishRejects", "persistentDomainPublishRejects", 1),
    ("domainGcEraseRejects", "persistentDomainGcEraseRejects", 1),
    ("domainResetPurgeRejects", "persistentDomainResetPurgeRejects", 2),
    ("domainResetOwnerRejects", "persistentDomainResetOwnerRejects", 1),
)

# 口径冻结：每个累加点必须落在这些函数里（所属函数 -> 该字段在该函数内的处数）。
FROZEN_ACCUMULATION_FUNCTIONS = {
    "rejectDomainConflict": {"War3TryCaptureShadowCaster": 1},
    "domainLookupRejects": {"War3TryFindShadowPersistentGeometry": 2},
    "domainPublishRejects": {"War3CreateShadowPersistentGeometryAfterMiss": 1},
    "domainGcEraseRejects": {"War3GcShadowPersistentGeometry": 1},
    "domainResetPurgeRejects": {"War3MaybeInsertBeforeUi": 1,
                                "War3DrainShadowCasterTombstones": 1},
    "domainResetOwnerRejects": {"War3ResetShadowSessionState": 1},
}

# Registry counters still introduce no env. The independent 2026-09-19
# Stage11 census/upload-range candidates add precisely two report env keys.
# The 2026-09-20 frozen run matrix additionally exports 19 existing War3 env
# names and the OBS opt-out. This is report metadata, not new registry policy.
# Keep exact cardinality AND exact single-occurrence names, not a wildcard skip.
FROZEN_ENV_LITERALS = {str(DEVICE_CPP): 138, str(PM_H): 2, str(PM_CPP): 129}
STAGE11_REPORT_ENV_KEYS = (
    "DXVK_WAR3_STAGE11_BUDGET_CENSUS",
    "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE",
)
NIGHT_RUN_REPORT_ENV_KEYS = (
    "DXVK_WAR3_INTERNAL_TEST_API",
    "DXVK_WAR3_INTERNAL_EXIT_TEST",
    "DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE",
    "DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE",
    "DXVK_WAR3_ASYNC_SCREENSHOT",
    "DXVK_WAR3_PERF_HISTORY_FRAMES",
    "DXVK_WAR3_PERF_AUTO_EXPORT_SEC",
    "DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB",
    "DXVK_WAR3_SCENARIO",
    "DISABLE_VULKAN_OBS_CAPTURE",
    "DXVK_WAR3_FRAME_EVIDENCE",
    "DXVK_WAR3_FRAME_EVIDENCE_INPUTS",
    "DXVK_WAR3_FRAME_EVIDENCE_DRAWS",
    "DXVK_WAR3_FRAME_EVIDENCE_CASTERS",
    "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT",
    "DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED",
    "DXVK_WAR3_NATIVE_MODEL_LIGHTS",
    "DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER",
    "DXVK_WAR3_DEBUG_CONSOLE",
    "DXVK_WAR3_RENDER_LOG",
)

# 出口链每段允许处数（对每个字段都一样）。
FROZEN_SITES = {
    "device.h War3ShadowPersistentDiagnosticsFrame 定义": 1,
    "device.cpp 重复定义": 0,
    "device.cpp bridge 传递": 1,
    "perf_monitor.h FrameWorkloadSnapshot": 1,
    "perf_monitor.h PersistentGeometryFrameStats": 1,
    "perf_monitor.h ShadowBudgetAggregate": 1,
    "perf_monitor.cpp per-frame 拷贝": 1,
    "perf_monitor.cpp 区间累加": 1,
    "perf_monitor.cpp JSON 键（每个写手）": 1,
    "perf_monitor.cpp JSON 取值（每个写手）": 1,
    "perf_monitor.cpp workloadSeriesColumns": 1,
    "perf_monitor.cpp workloadSeries 行": 1,
}

WRITER_A_START = 'json << "  \\"shadowBudgetSummary\\": {\\n";'
WRITER_A_END = 'json << "  \\"topShadowOffenders\\": [\\n";'
WRITER_B_START = 'json << "  \\"shadowRuntimeV2Summary\\": {\\n";'
WRITER_B_END = 'json << "  \\"publishVisibleSafeCopyVerifier\\": {\\n";'


def count(haystack, needle):
    return haystack.count(needle)


def braced_block(source, marker, start=0):
    marker_pos = source.index(marker, start)
    open_pos = source.index("{", marker_pos)
    depth = 0
    for pos in range(open_pos, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[marker_pos:pos + 1]
    raise AssertionError(f"unterminated block after {marker}")


def writer_region(source, start_marker, end_marker):
    start = source.index(start_marker)
    end = source.index(end_marker, start)
    return source[start:end]


def enclosing_function(source, position):
    """返回 position 之前最近的 `Word::Name(...){ ` 定义所在的函数名。"""
    head = source[:position]
    pattern = re.compile(
        r"^(?:[A-Za-z_][\w:<>,*&\s]*?\s+)?(\w+)::(\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{",
        re.M,
    )
    last = None
    for match in pattern.finditer(head):
        last = match
    if last is None:
        return None
    return last.group(2)


class RegistryDomainCountExportStaticTest(unittest.TestCase):
    # ---- 1. 当帧定义与"不重复定义" ----
    def test_frame_definition_is_unique_in_device_h(self):
        frame = braced_block(
            DEVICE_H_TEXT, "struct War3ShadowPersistentDiagnosticsFrame {")
        for name, _agg, _inc in FIELDS:
            self.assertEqual(
                count(frame, f"uint64_t {name} = 0;"),
                FROZEN_SITES["device.h War3ShadowPersistentDiagnosticsFrame 定义"],
                f"{name} 在 War3ShadowPersistentDiagnosticsFrame 的定义数不是 1")

    def test_device_cpp_does_not_redefine_the_counters(self):
        for name, _agg, _inc in FIELDS:
            self.assertEqual(
                count(DEVICE_CPP_TEXT, f"uint64_t {name} = 0;"),
                FROZEN_SITES["device.cpp 重复定义"],
                f"device.cpp 不得重复定义 {name}（定义只允许在 d3d9_device.h）")
            self.assertEqual(
                count(DEVICE_CPP_TEXT, f"uint64_t {name} = 0u;"),
                FROZEN_SITES["device.cpp 重复定义"],
                f"device.cpp 不得重复定义 {name}（u 后缀变体）")

    # ---- 2. 累加点处数 + 所属函数（口径） ----
    def test_accumulation_site_counts_are_frozen(self):
        for name, _agg, expected in FIELDS:
            self.assertEqual(
                count(DEVICE_CPP_TEXT, f"{name}++"), expected,
                f"device.cpp 中 {name}++ 的处数不是冻结值 {expected}")

    def test_accumulation_sites_are_attributed_to_the_expected_paths(self):
        for name, _agg, expected in FIELDS:
            found = {}
            for match in re.finditer(re.escape(name) + r"\+\+", DEVICE_CPP_TEXT):
                function = enclosing_function(DEVICE_CPP_TEXT, match.start())
                found[function] = found.get(function, 0) + 1
            self.assertEqual(
                found, FROZEN_ACCUMULATION_FUNCTIONS[name],
                f"{name} 的累加点所属函数与冻结口径不符：{found}")
            self.assertEqual(sum(found.values()), expected)

    # ---- 3. bridge 传递 ----
    def test_bridge_passes_every_counter_into_the_perf_monitor_stats(self):
        region = DEVICE_CPP_TEXT[
            DEVICE_CPP_TEXT.index(
                "stats.rejectCapacity = completed.rejectCapacity;"):]
        region = region[:region.index(
            "war3::War3PerfMonitor::instance().notePersistentGeometryFrame(stats);")]
        region = re.sub(r"\s+", " ", region)
        for name, _agg, _inc in FIELDS:
            self.assertEqual(
                count(region, f"stats.{name} = completed.{name};"),
                FROZEN_SITES["device.cpp bridge 传递"],
                f"bridge 传递缺失或重复：stats.{name} = completed.{name};")

    def test_bridge_sources_from_the_completed_interval(self):
        # 区间边界必须仍是 std::exchange(..., {})（每 Present 区间清零）。
        self.assertIn(
            "std::exchange(m_war3ShadowPersistentDiagnosticsFrame, {})",
            DEVICE_CPP_NORM)

    # ---- 4. perf monitor 定义与累加 ----
    def test_perf_monitor_header_declares_every_stage(self):
        workload = braced_block(PM_H_TEXT, "struct FrameWorkloadSnapshot {")
        frame_stats = braced_block(
            PM_H_TEXT, "struct PersistentGeometryFrameStats {")
        aggregate = braced_block(
            PM_H_TEXT, "struct ShadowBudgetAggregate {")
        for name, agg, _inc in FIELDS:
            self.assertEqual(
                count(workload, f"uint64_t {name} = 0;"),
                FROZEN_SITES["perf_monitor.h FrameWorkloadSnapshot"], name)
            self.assertEqual(
                count(frame_stats, f"uint64_t {name} = 0;"),
                FROZEN_SITES["perf_monitor.h PersistentGeometryFrameStats"], name)
            self.assertEqual(
                count(aggregate, f"uint64_t {agg} = 0;"),
                FROZEN_SITES["perf_monitor.h ShadowBudgetAggregate"], name)

    def test_perf_monitor_cpp_copies_and_accumulates_once_each(self):
        for name, agg, _inc in FIELDS:
            self.assertEqual(
                count(PM_CPP_NORM,
                      f"m_currentFrameWorkload.{name} = stats.{name};"),
                FROZEN_SITES["perf_monitor.cpp per-frame 拷贝"], name)
            self.assertEqual(
                count(PM_CPP_NORM, f"agg.{agg} += stats.{name};"),
                FROZEN_SITES["perf_monitor.cpp 区间累加"], name)
        # 累加仍只在 notePersistentGeometryFrame 里。
        for name, agg, _inc in FIELDS:
            position = PM_CPP_TEXT.index(f"agg.{agg} += stats.{name};")
            self.assertEqual(
                enclosing_function(PM_CPP_TEXT, position),
                "notePersistentGeometryFrame", name)

    # ---- 5. 两个 JSON 写手 + workloadSeries ----
    def test_both_json_writers_export_every_counter(self):
        region_a = writer_region(PM_CPP_TEXT, WRITER_A_START, WRITER_A_END)
        region_b = writer_region(PM_CPP_TEXT, WRITER_B_START, WRITER_B_END)
        for label, region in (("shadowBudgetSummary", region_a),
                              ("shadowRuntimeV2Summary", region_b)):
            for name, agg, _inc in FIELDS:
                self.assertEqual(
                    count(region, f'\\"{agg}\\": '),
                    FROZEN_SITES["perf_monitor.cpp JSON 键（每个写手）"],
                    f"{label} 缺 {agg} 的 JSON 键或写重")
                self.assertEqual(
                    count(region, f"shadowAgg.{agg} <<"),
                    FROZEN_SITES["perf_monitor.cpp JSON 取值（每个写手）"],
                    f"{label} 的 {agg} 取值表达式缺失或写重")

    def test_workload_series_has_one_column_and_one_value_per_counter(self):
        columns = PM_CPP_TEXT[
            PM_CPP_TEXT.index('\\"workloadSeriesColumns\\": ['):
            PM_CPP_TEXT.index('\\"workloadSeries\\": [')]
        for name, _agg, _inc in FIELDS:
            self.assertEqual(
                count(columns, f'\\"{name}\\\"'),
                FROZEN_SITES["perf_monitor.cpp workloadSeriesColumns"], name)
            self.assertEqual(
                count(PM_CPP_NORM, f"w.{name}"),
                FROZEN_SITES["perf_monitor.cpp workloadSeries 行"], name)

    # ---- 6. 口径：legacy 失败分桶求和 ----
    def test_only_reject_domain_conflict_joins_the_legacy_detailed_total(self):
        region_a = writer_region(PM_CPP_TEXT, WRITER_A_START, WRITER_A_END)
        region_b = writer_region(PM_CPP_TEXT, WRITER_B_START, WRITER_B_END)
        for label, region in (("shadowBudgetSummary", region_a),
                              ("shadowRuntimeV2Summary", region_b)):
            total = region[region.index(
                "persistentRejectCreateOrBudgetDetailedTotal"):]
            total = total[:total.index('<< ",\\n";')]
            self.assertIn("shadowAgg.persistentRejectDomainConflict", total,
                          f"{label}: rejectDomainConflict 未进入 legacy 明细求和")
            for name, agg, _inc in FIELDS:
                if name == "rejectDomainConflict":
                    continue
                self.assertNotIn(
                    f"shadowAgg.{agg}", total,
                    f"{label}: {agg} 不是 create 失败分桶，不得进入 legacy 明细求和")

    def test_counters_are_documented_as_observation_only(self):
        region_a = writer_region(PM_CPP_TEXT, WRITER_A_START, WRITER_A_END)
        self.assertIn("persistentDomainCounterContract", region_a)
        self.assertIn("steady state is 0", region_a)

    # ---- 7. 不加 env ----
    def test_no_new_environment_variable_is_introduced(self):
        report_env = braced_block(PM_CPP_TEXT, "std::string BuildPerfEnvJson()")
        for key in STAGE11_REPORT_ENV_KEYS + NIGHT_RUN_REPORT_ENV_KEYS:
            # These two existing configuration readers remain alongside the
            # newly exported metadata. Do not mistake them for duplicate keys.
            reader_count = int(key in ("DXVK_WAR3_PERF_HISTORY_FRAMES",
                                       "DXVK_WAR3_PERF_AUTO_EXPORT_SEC"))
            self.assertEqual(PM_CPP_TEXT.count('"' + key + '"'),
                             1 + reader_count, key)
            if reader_count:
                self.assertEqual(PM_CPP_TEXT.count('env::getEnvVar("' + key + '")'),
                                 1, key)
            self.assertEqual(report_env.count('"' + key + '"'), 1, key)
        self.assertEqual(sum(key.startswith("DXVK_WAR3_")
                             for key in NIGHT_RUN_REPORT_ENV_KEYS), 19)
        for path, expected in FROZEN_ENV_LITERALS.items():
            text = Path(path).read_text(encoding="utf-8", errors="replace")
            self.assertEqual(
                len(re.findall(r"DXVK_WAR3_", text)), expected,
                f"{path}: DXVK_WAR3_ 字面量数变化（本块不得加 env）")

    def test_no_domain_count_export_env_switch_exists(self):
        for forbidden in ("DXVK_WAR3_REGISTRY_DOMAIN_COUNT",
                          "DXVK_WAR3_DOMAIN_COUNT_EXPORT"):
            for path in REQUIRED:
                self.assertNotIn(
                    forbidden,
                    path.read_text(encoding="utf-8", errors="replace"), str(path))


if __name__ == "__main__":
    unittest.main()
