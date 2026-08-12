from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class DirectCanonicalPrefilterHandoffStaticTests(unittest.TestCase):
    def test_grouped_preselector_runs_every_canonical_gate_before_selection(self) -> None:
        start = DEVICE.index('enterDirectDetailPhase("PreselectScan")')
        end = DEVICE.index("recordsForBuildCanonicalPrefiltered = true", start)
        block = DEVICE[start:end]
        ordered = (
            "currentFrameDrawTimeProducerOwnsRecord(record)",
            "War3SemanticRawcodeLooksStaticWorldCaster(record.rawcode)",
            "ShadowProducerPolicyAllows(",
            "War3ShadowIsLosBlocker(record)",
            "War3RejectCurrentDrawRecordByUnsafeAlphaVisualPolicy(",
            "preselectedRecords.push_back(",
        )
        cursor = -1
        for token in ordered:
            next_cursor = block.index(token)
            self.assertGreater(next_cursor, cursor)
            cursor = next_cursor

    def test_handoff_is_set_only_after_group_selection(self) -> None:
        declaration = DEVICE.index(
            "bool recordsForBuildCanonicalPrefiltered = false"
        )
        grouped = DEVICE.index("if (useObjectGrouped && directRecordCap != 0u)", declaration)
        handoff = DEVICE.index(
            "recordsForBuildCanonicalPrefiltered = true", grouped
        )
        fallback = DEVICE.index('enterDirectDetailPhase("SnapshotFallbackCopy")', handoff)
        self.assertLess(grouped, handoff)
        self.assertLess(handoff, fallback)
        self.assertNotIn(
            "recordsForBuildCanonicalPrefiltered = true",
            DEVICE[fallback : DEVICE.index("// --- Step 2", fallback)],
        )

    def test_build_reruns_all_gates_only_for_unfiltered_fallback(self) -> None:
        start = DEVICE.index("for (size_t buildIndex = 0u;")
        end = DEVICE.index("uint64_t producerClaimStrictKey", start)
        block = DEVICE[start:end]
        self.assertIn(
            "const bool rerunCanonicalRecordFilters =\n"
            "        !recordsForBuildCanonicalPrefiltered && !useSealedWork",
            block,
        )
        for token in (
            "rerunCanonicalRecordFilters &&\n"
            "        currentFrameDrawTimeProducerOwnsRecord(record)",
            "rerunCanonicalRecordFilters &&\n"
            "        War3SemanticRawcodeLooksStaticWorldCaster(record.rawcode)",
            "if (rerunCanonicalRecordFilters) {",
            "rerunCanonicalRecordFilters &&\n"
            "        dxvk::war3::internal::kPathBlockerHideEnabled",
            "rerunCanonicalRecordFilters &&\n"
            "        War3RejectCurrentDrawRecordByUnsafeAlphaVisualPolicy(",
        ):
            self.assertIn(token, block)
        self.assertIn(
            "if (rerunCanonicalRecordFilters)\n"
            "      bucket.shadowEligibleParts++",
            block,
        )


if __name__ == "__main__":
    unittest.main()
