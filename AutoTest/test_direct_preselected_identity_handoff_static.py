from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class DirectPreselectedIdentityHandoffStaticTests(unittest.TestCase):
    def test_scalar_selection_key_scratch_tracks_record_indices(self) -> None:
        start = DEVICE.index(
            "static thread_local std::vector<uint32_t> s_recordIndicesForBuild"
        )
        end = DEVICE.index("// --- Step 2: build eligible record list", start)
        block = DEVICE[start:end]
        for token in (
            "static thread_local std::vector<uint64_t> s_recordSelectionKeysForBuild",
            "recordIndicesForBuild.clear()",
            "recordSelectionKeysForBuild.clear()",
            "recordIndicesForBuild.reserve(directStickyRecordBudget)",
            "recordSelectionKeysForBuild.reserve(directStickyRecordBudget)",
            "recordIndicesForBuild.push_back(preselectedRecords[i].recordIndex)",
            "recordSelectionKeysForBuild.push_back(\n"
            "            preselectedRecords[i].selectionKey)",
        ):
            self.assertIn(token, block)
        self.assertNotIn("CurrentDrawContractRecord> s_recordSelection", block)

    def test_uncapped_path_keeps_historical_on_demand_resolution(self) -> None:
        start = DEVICE.index('enterDirectDetailPhase("SnapshotFallbackCopy")')
        end = DEVICE.index("// --- Step 2: build eligible record list", start)
        block = DEVICE[start:end]
        self.assertIn(
            "recordSelectionKeysForBuild.assign(directRecords.size(), 0u)",
            block,
        )

    def test_build_uses_preselected_key_and_falls_back_only_on_zero(self) -> None:
        start = DEVICE.index("for (size_t buildIndex = 0u;")
        end = DEVICE.index("eligible.selectionKey =", start)
        block = DEVICE[start:end]
        self.assertIn("const uint64_t preselectedRecordSelectionKey", block)
        self.assertIn("buildIndex < recordSelectionKeysForBuild.size()", block)
        self.assertIn(
            "const uint64_t carriedRecordSelectionKey = useSealedWork\n"
            "        ? compactWork.selectionKey\n"
            "        : preselectedRecordSelectionKey",
            block,
        )
        self.assertIn(
            "eligible.recordSelectionKey = carriedRecordSelectionKey != 0u\n"
            "        ? carriedRecordSelectionKey\n"
            "        : War3SemanticDirectRecordSelectionKey(record)",
            block,
        )


if __name__ == "__main__":
    unittest.main()
