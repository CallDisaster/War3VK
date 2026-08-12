"""Release must not resolve DirectGrouped completeness-only object keys."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="ignore"
)
CONFIG = (
    ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
).read_text(encoding="utf-8", errors="ignore")


class DirectCompletenessKeyReleaseBypassContracts(unittest.TestCase):
    def test_release_tracking_default_remains_disabled(self) -> None:
        block = CONFIG.split(
            "inline constexpr bool kNativeOptimizationPerfTrackingEnabled =",
            1,
        )[1].split(";", 1)[0]
        self.assertIn("kNativeMainLoopCoverageAnalysisMode || false", block)

    def test_completeness_key_is_resolved_only_when_tracking(self) -> None:
        loop = DEVICE.split(
            'enterBuildEligiblePhase("RecordLoop");', 1
        )[1].split("if (traceBuildEligible) {", 1)[0]
        assignment = loop.split(
            "eligible.completenessKey = trackCompletenessBuckets", 1
        )[1].split(";", 1)[0]
        self.assertIn("carriedRecordSelectionKey != 0u", assignment)
        self.assertIn("completenessKeyForRecord(record)", assignment)
        self.assertIn(": 0u", assignment)
        self.assertEqual(
            loop.count("completenessKeyForRecord(record)"), 1
        )

    def test_preselected_identity_is_reused_for_submission(self) -> None:
        loop = DEVICE.split(
            'enterBuildEligiblePhase("RecordLoop");', 1
        )[1].split("if (traceBuildEligible) {", 1)[0]
        self.assertIn(
            "const uint64_t carriedRecordSelectionKey = useSealedWork",
            loop,
        )
        self.assertIn(
            "eligible.recordSelectionKey = carriedRecordSelectionKey != 0u",
            loop,
        )


if __name__ == "__main__":
    unittest.main()
