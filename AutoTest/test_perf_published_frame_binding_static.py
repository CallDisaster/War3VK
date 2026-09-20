#!/usr/bin/env python3
"""Static guards for the published perf frame binding producer contract.

This is source structure evidence only. It does not compile or run the producer.
"""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]

PERF_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(encoding="utf-8")
PERF_CPP = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
HUB_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(encoding="utf-8")
HUB_CPP = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(encoding="utf-8")
CONTROL = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(encoding="utf-8")
RUNNER = (ROOT / "AutoTest/run_snapshot_pressure_canary.py").read_text(encoding="utf-8")
ANALYZER = (ROOT / "AutoTest/analyze_night_pressure_recovery.py").read_text(encoding="utf-8")


def between(text: str, start: str, end: str) -> str:
    start_index = text.index(start)
    return text[start_index:text.index(end, start_index)]


class PerfPublishedFrameBindingStaticTests(unittest.TestCase):
    def test_query_uses_completed_history_under_one_lock_without_last_note_shortcut(self):
        query = between(
            PERF_CPP,
            "War3PerfMonitor::queryPublishedPerfState() {",
            "void War3PerfMonitor::resetShadowBudgetAggregateLocked",
        )
        self.assertEqual(query.count("std::lock_guard lock(m_mutex);"), 1)
        self.assertIn("m_frameHistory.back()", query)
        self.assertIn("completed.workload.businessFrameSerial", query)
        self.assertIn("completed.frameEpoch", query)
        self.assertNotIn("m_lastBusinessFrameSerial", query)
        self.assertIn(
            "state.valid = state.businessFrameSerial != 0u && state.frameEpoch != 0u;",
            query,
        )

    def test_status_hub_queries_once_and_both_serializers_share_published_fields(self):
        self.assertEqual(HUB_CPP.count("perf.queryPublishedPerfState()"), 1)
        self.assertEqual(HUB_CPP.count("queryPublishedPerfState"), 1)
        self.assertEqual(CONTROL.count("queryPublishedPerfState"), 0)
        self.assertIn(
            "const auto publishedPerf = perf.queryPublishedPerfState();",
            HUB_CPP,
        )
        for field in (
            "frameAnchorValid",
            "businessFrameSerial",
            "perfFrameEpoch",
            "producerAccumulationEpoch",
        ):
            needle = '{"%s", snapshot.perf.%s}' % (field, field)
            self.assertIn(needle, CONTROL)
            self.assertIn(needle, HUB_CPP)

    def test_reset_helper_is_only_aggregate_clear_and_two_sites_use_it(self):
        self.assertEqual(PERF_CPP.count("m_shadowBudgetAggregate = {};"), 1)
        helper = between(
            PERF_CPP,
            "void War3PerfMonitor::resetShadowBudgetAggregateLocked() {",
            "void War3PerfMonitor::noteShadowMetadataFrame",
        )
        self.assertIn("m_shadowBudgetAggregate = {};", helper)
        self.assertIn("if (m_producerAccumulationEpoch != 0u)", helper)
        self.assertIn("++m_producerAccumulationEpoch;", helper)
        self.assertNotRegex(helper, r"m_producerAccumulationEpoch\s*=\s*1u?;")

        shutdown = between(
            PERF_CPP,
            "void War3PerfMonitor::shutdown() {",
            "void War3PerfMonitor::beginFrame",
        )
        reset_history = between(
            PERF_CPP,
            "void War3PerfMonitor::resetHistory() {",
            "uint32_t War3PerfMonitor::getPendingExportCount",
        )
        self.assertIn("resetShadowBudgetAggregateLocked();", shutdown)
        self.assertIn("resetShadowBudgetAggregateLocked();", reset_history)
        self.assertEqual(PERF_CPP.count("resetShadowBudgetAggregateLocked();"), 2)

    def test_export_pairs_epoch_with_aggregate_and_serializes_summary(self):
        export = between(
            PERF_CPP,
            "War3PerfMonitor::captureExportSnapshotLocked() const {",
            "std::string War3PerfMonitor::resolveExportPath",
        )
        self.assertRegex(
            export,
            r"snapshot\.shadowBudgetAggregate = m_shadowBudgetAggregate;\s*"
            r"snapshot\.producerAccumulationEpoch = m_producerAccumulationEpoch;",
        )
        self.assertIn('\\"producerAccumulationEpoch\\":', PERF_CPP)

    def test_epoch_wrap_is_exhausted_not_reused_as_one(self):
        helper = between(
            PERF_CPP,
            "void War3PerfMonitor::resetShadowBudgetAggregateLocked() {",
            "void War3PerfMonitor::noteShadowMetadataFrame",
        )
        self.assertNotRegex(helper, r"m_producerAccumulationEpoch\s*=\s*1u?;")
        self.assertIn("m_producerAccumulationEpoch = 1;", PERF_H)

    def test_env_key_list_is_unique_and_covers_frozen_canary_required_matrix(self):
        names_match = re.search(
            r"static constexpr const char \*kNames\[\] = \{(.*?)\n\s*\};",
            PERF_CPP,
            re.S,
        )
        self.assertIsNotNone(names_match)
        names = re.findall(r'"([A-Z0-9_]+)"', names_match.group(1))
        self.assertEqual(len(names), len(set(names)))

        heavy_match = re.search(r"HEAVY_FORENSICS_OFF = \((.*?)\n\)", RUNNER, re.S)
        required_match = re.search(
            r"REQUIRED_RUN_ENV_KEYS = \((.*?)\n\) \+ HEAVY_FORENSICS_OFF",
            RUNNER,
            re.S,
        )
        self.assertIsNotNone(heavy_match)
        self.assertIsNotNone(required_match)
        heavy = set(re.findall(r"'([A-Z0-9_]+)'", heavy_match.group(1)))
        required = set(re.findall(r"'([A-Z0-9_]+)'", required_match.group(1))) | heavy
        self.assertTrue(heavy <= set(names))
        self.assertTrue(required <= set(names))

    def test_runner_consumes_producer_fields_and_analyzer_keeps_exclusive_start(self):
        perf_fields = between(RUNNER, "def _perf_phase_fields(status):", "def phase_marker")
        for key in ("enabled", "recording", "frameAnchorValid"):
            self.assertIn("'" + key + "'", perf_fields)
        for key in (
            "businessFrameSerial",
            "perfFrameEpoch",
            "producerAccumulationEpoch",
        ):
            self.assertIn("'" + key + "'", perf_fields)
        self.assertIn(
            "frameDomain='workload.businessFrameSerial'",
            RUNNER,
        )

        phases = between(
            ANALYZER,
            "def validate_phase_markers(",
            "def _load_phases",
        )
        self.assertIn(
            'pressure_start = int(normalized["pressure-start"]["businessFrameSerial"]) + 1',
            phases,
        )
        self.assertIn(
            'relief_start = int(normalized["relief-start"]["businessFrameSerial"]) + 1',
            phases,
        )
        self.assertIn(
            'pressure_end = int(normalized["pressure-end"]["businessFrameSerial"])',
            phases,
        )
        self.assertIn("not a completed-frame cut", ANALYZER)


if __name__ == "__main__":
    unittest.main()