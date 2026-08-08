"""Contracts for the Issue #5 non-mutating Observe report gate."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUTOTEST = ROOT / "AutoTest"
PERF_MONITOR = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
sys.path.insert(0, str(AUTOTEST))

import analyze_issue5_shadow_observe as analyzer  # noqa: E402
import war3_autotest_mcp as war3  # noqa: E402


def valid_data() -> dict:
    return {
        "frameCount": 10000,
        "avgFps": 120.0,
        "semanticSceneTerrainBoundsCullMode": 1,
        "semanticSceneTerrainBoundsCandidateCount": 40000,
        "semanticSceneTerrainBoundsProofAcceptedCount": 40000,
        "semanticSceneTerrainBoundsWouldCullCount": 24000,
        "semanticSceneTerrainBoundsAppliedCullCount": 0,
        "semanticSceneObjectBoundsAppliedCullCount": 0,
        "semanticSceneUnionCullMode": 1,
        "semanticSceneUnionCullObserveFrameCount": 10000,
        "semanticSceneUnionCullCandidateCount": 20000,
        "semanticSceneUnionCullProofAcceptedCount": 40000,
        "semanticSceneUnionCullBothFarWouldCullCount": 8000,
        "semanticSceneUnionCullFalseNegativeCount": 0,
        "semanticSceneUnionCullFalsePositiveCount": 0,
        "shadowBudgetSummary": {
            "framesIncomplete": 0,
            "framesBudgetExceeded": 0,
        },
    }


class Issue5ShadowObserveAnalysisContracts(unittest.TestCase):
    def test_report_metadata_records_the_exact_observer_environment(self) -> None:
        source = PERF_MONITOR.read_text(encoding="utf-8")
        self.assertIn("DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE", source)
        self.assertIn("DXVK_WAR3_UNION_CONSUMER_CULL_MODE", source)

    def test_complete_observe_recording_can_close_the_analysis_gate(self) -> None:
        result = war3._extract_shadow_cull_observe_summary(valid_data())
        self.assertTrue(result["gate"]["consumeAdmissionReady"])
        self.assertEqual(result["terrain"]["farCascadeWouldCullPct"], 30.0)
        self.assertEqual(result["union"]["bothFarWouldCullPct"], 40.0)

    def test_consume_or_output_mutation_is_never_accepted_as_observe(self) -> None:
        data = valid_data()
        data["semanticSceneTerrainBoundsCullMode"] = 2
        data["semanticSceneTerrainBoundsAppliedCullCount"] = 1
        result = war3._extract_shadow_cull_observe_summary(data)
        self.assertFalse(result["gate"]["modeIsObserve"])
        self.assertFalse(result["gate"]["observeIsNonMutating"])
        self.assertFalse(result["gate"]["consumeAdmissionReady"])

    def test_one_frame_false_negative_blocks_admission(self) -> None:
        data = valid_data()
        data["semanticSceneUnionCullFalseNegativeCount"] = 1
        result = war3._extract_shadow_cull_observe_summary(data)
        self.assertFalse(result["gate"]["correctnessClosed"])
        self.assertFalse(result["gate"]["consumeAdmissionReady"])

    def test_short_or_incomplete_capture_blocks_admission(self) -> None:
        data = valid_data()
        data["frameCount"] = 9999
        data["shadowBudgetSummary"]["framesIncomplete"] = 1
        result = war3._extract_shadow_cull_observe_summary(data)
        self.assertFalse(result["gate"]["enoughFrames"])
        self.assertFalse(result["gate"]["correctnessClosed"])

    def test_cli_reader_extracts_embedded_perf_json_without_game_control(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            report = Path(temp_dir) / "observe.html"
            report.write_text(
                "<script>const data = " + json.dumps(valid_data()) +
                ";</script>",
                encoding="utf-8",
            )
            result = analyzer.analyze_report(report)
        self.assertTrue(result["attachOnlyAnalysis"])
        self.assertTrue(result["neverLaunchesOrStopsGame"])
        self.assertTrue(
            result["shadowCullObserveSummary"]["gate"][
                "consumeAdmissionReady"
            ]
        )


if __name__ == "__main__":
    unittest.main()
