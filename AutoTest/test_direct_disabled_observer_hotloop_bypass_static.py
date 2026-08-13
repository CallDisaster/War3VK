#!/usr/bin/env python3
"""Contracts for bypassing disabled DirectGrouped observers in hot loops."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class DirectDisabledObserverHotloopBypassStaticTest(unittest.TestCase):
    def test_release_completeness_initial_scan_is_gated(self) -> None:
        marker = "for (const auto& record : directRecords)\n      completenessBucketForRecord(record).observedParts++;"
        scan = DEVICE.index(marker)
        prefix = DEVICE[max(0, scan - 80) : scan]
        self.assertIn("if (trackCompletenessBuckets) {", prefix)

    def test_record_loop_does_not_probe_off_compact_table(self) -> None:
        loop = DEVICE.split('enterBuildEligiblePhase("RecordLoop");', 1)[1]
        loop = loop.split("if (traceBuildEligible) {", 1)[0]
        self.assertIn(
            "const bool hasCompactWork = consumeCompactWorkTable &&",
            loop,
        )
        self.assertIn("m_war3CompactWorkTable.load(buildIndex, compactWork)", loop)

    def test_release_record_loop_does_not_write_dummy_bucket(self) -> None:
        loop = DEVICE.split('enterBuildEligiblePhase("RecordLoop");', 1)[1]
        loop = loop.split("if (traceBuildEligible) {", 1)[0]
        self.assertIn(
            "DirectObjectCompletenessBucket* bucket = trackCompletenessBuckets",
            loop,
        )
        self.assertIn("if (bucket != nullptr)", loop)
        self.assertNotIn("auto& bucket = completenessBucketForRecord(record);", loop)

    def test_submit_bookkeeping_is_gated(self) -> None:
        self.assertIn(
            "if (trackCompletenessBuckets) {\n"
            "        completenessBucketForKey(eligible.completenessKey).submittedParts++;",
            DEVICE,
        )


if __name__ == "__main__":
    unittest.main()
