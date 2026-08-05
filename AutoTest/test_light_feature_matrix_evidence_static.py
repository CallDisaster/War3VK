#!/usr/bin/env python3
"""Static contracts for isolated light-feature execution evidence."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "AutoTest/run_light_feature_matrix.py").read_text(
    encoding="utf-8", errors="ignore"
)


class LightFeatureMatrixEvidenceStaticTests(unittest.TestCase):
    def test_point_shadow_accepts_fresh_active_gpu_section(self) -> None:
        self.assertIn("current-report-gpu-section:PointShadow", RUNNER)
        self.assertIn("elif active_point_sections:", RUNNER)
        self.assertIn("report_file_within_case", RUNNER)
        self.assertIn("float(row.get(\"avgGpuMs\"", RUNNER)

    def test_controlled_forced_stop_is_not_a_runtime_failure(self) -> None:
        failure = RUNNER[
            RUNNER.index("runtime_process_failure = bool(") :
            RUNNER.index("return {", RUNNER.index("runtime_process_failure = bool("))
        ]
        self.assertNotIn("or forced_stop", failure)
        self.assertIn("or device_lost_count > 0", failure)
        self.assertIn('"controlledForcedStop"', RUNNER)


if __name__ == "__main__":
    unittest.main()
