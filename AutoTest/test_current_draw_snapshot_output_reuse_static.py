import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="ignore"
)
CONTRACT_CPP = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
).read_text(encoding="utf-8", errors="ignore")
CONTRACT_H = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"
).read_text(encoding="utf-8", errors="ignore")


class CurrentDrawSnapshotOutputReuseContracts(unittest.TestCase):
    def test_public_value_returning_api_keeps_historical_semantics(self) -> None:
        wrapper = CONTRACT_CPP.index(
            "std::vector<CurrentDrawContractRecord> "
            "SnapshotPublishedCurrentDrawContracts(\n"
            "    const CurrentDrawContractSnapshotOptions& options)"
        )
        implementation = CONTRACT_CPP.index(
            "void SnapshotPublishedCurrentDrawContracts(\n"
            "    const CurrentDrawContractSnapshotOptions& options,",
            wrapper,
        )
        block = CONTRACT_CPP[wrapper:implementation]
        self.assertIn("std::vector<CurrentDrawContractRecord> out", block)
        self.assertIn("SnapshotPublishedCurrentDrawContracts(options, out)", block)
        self.assertIn("return out", block)

    def test_caller_owned_output_is_cleared_before_policy_or_scan(self) -> None:
        start = CONTRACT_CPP.index(
            "void SnapshotPublishedCurrentDrawContracts(\n"
            "    const CurrentDrawContractSnapshotOptions& options,"
        )
        scan = CONTRACT_CPP.index(
            'enterSnapshotPhase("SnapshotActiveSlotScan")', start
        )
        block = CONTRACT_CPP[start:scan]
        self.assertIn("out.clear()", block)
        self.assertLess(block.index("out.clear()"), block.index("GlobalCurrentDrawPublishEnabled()"))
        self.assertIn("std::vector<CurrentDrawContractRecord>& out", CONTRACT_H)

    def test_direct_grouped_reuses_only_non_owning_contract_values(self) -> None:
        start = DEVICE.index(
            "static thread_local std::vector<\n"
            "      dxvk::war3::render::CurrentDrawContractRecord> s_directRecords"
        )
        end = DEVICE.index('enterDirectDetailPhase("SnapshotDiagnostics")', start)
        block = DEVICE[start:end]
        self.assertIn("auto& directRecords = s_directRecords", block)
        self.assertIn("struct DirectRecordScratchReset", block)
        self.assertIn("records.clear()", block)
        self.assertIn(
            "const DirectRecordScratchReset directRecordScratchReset { directRecords }",
            block,
        )
        self.assertIn("snapshotOptions, directRecords", block)
        self.assertNotIn("ShadowDrawPacket", block)
        self.assertNotIn("CurrentDrawAuthoritativeSample", block)
        self.assertNotIn("Rc<", block)
        self.assertNotIn("shared_ptr", block)

    def test_snapshot_record_remains_non_owning_value_contract(self) -> None:
        start = CONTRACT_H.index("struct CurrentDrawContractRecord")
        end = CONTRACT_H.index("struct CurrentDrawDispatchContext", start)
        record = CONTRACT_H[start:end]
        for owner in ("Rc<", "shared_ptr", "unique_ptr", "std::vector"):
            self.assertNotIn(owner, record)


if __name__ == "__main__":
    unittest.main()
