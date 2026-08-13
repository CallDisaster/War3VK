"""Static contract checks for the CurrentDraw redundant-atomic rollback."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTRACT_CPP = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
)
MODEL_HOOK_CPP = ROOT / "src/d3d9/war3/model/war3_model_hook.cpp"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"


class CurrentDrawRedundantAtomicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.contract = CONTRACT_CPP.read_text(encoding="utf-8")
        cls.model_hook = MODEL_HOOK_CPP.read_text(encoding="utf-8")
        cls.monitor = MONITOR_CPP.read_text(encoding="utf-8")

    def test_rollback_environment_is_reported(self) -> None:
        name = "DXVK_WAR3_CURRENT_DRAW_REDUNDANT_ATOMICS"
        self.assertIn(f'"{name}"', self.contract)
        self.assertIn(f'"{name}"', self.monitor)
        self.assertRegex(
            self.contract,
            rf'ReadEnvU32\("{name}",\s*0u\)',
        )

    def test_duplicate_trusted_counter_is_legacy_only(self) -> None:
        writes = [
            match.start()
            for match in re.finditer(
                r"g_publishTrustedHitCumulative\.fetch_add", self.contract
            )
        ]
        self.assertEqual(1, len(writes))
        gate = self.contract.rfind(
            "if (CurrentDrawRedundantAtomicsLegacyRuntime())",
            0,
            writes[0],
        )
        self.assertGreater(gate, writes[0] - 240)
        summary = self.contract.index(
            "summary.publishTrustedHitCumulative ="
        )
        summary_tail = self.contract[summary : summary + 360]
        self.assertIn(
            "QueryBlendedPaletteExactHitCount()", summary_tail
        )
        self.assertIn(
            "g_publishTrustedHitCumulative.load", summary_tail
        )

    def test_unread_provenance_rmw_is_inside_legacy_gate(self) -> None:
        block_start = self.contract.index(
            "// Legacy-only diagnostic buckets"
        )
        block_end = self.contract.index(
            "const bool snapshotWasTrusted", block_start
        )
        block = self.contract[block_start:block_end]
        self.assertEqual(3, block.count(".fetch_add("))
        self.assertIn(
            "if (CurrentDrawRedundantAtomicsLegacyRuntime())",
            self.contract[block_start - 100 : block_start],
        )

    def test_canonical_and_legacy_counters_have_one_relaxed_writer_and_no_reset(
        self,
    ) -> None:
        duplicate = "g_publishTrustedHitCumulative"
        self.assertNotIn("g_paletteCaptureTrustedSourceHitCount", self.contract)
        canonical = "g_queryBlendedPaletteExactHitCount"
        self.assertEqual(2, self.model_hook.count(f"{canonical}.fetch_add("))
        self.assertEqual(
            1, self.contract.count(f"{duplicate}.fetch_add(")
        )
        for name, source in ((canonical, self.model_hook),
                             (duplicate, self.contract)):
            write = source.index(f"{name}.fetch_add(")
            self.assertIn(
                "std::memory_order_relaxed",
                source[write : write + 140],
            )
        reset_start = self.contract.index(
            "void ResetCurrentDrawContractCache()"
        )
        reset_end = self.contract.index(
            "void PublishCurrentDrawContract(", reset_start
        )
        reset = self.contract[reset_start:reset_end]
        self.assertNotIn("g_paletteCaptureTrustedSourceHitCount", reset)
        self.assertNotIn(duplicate, reset)

    def test_legacy_provenance_buckets_have_no_reader(self) -> None:
        names = (
            "g_paletteProvenanceTrustedBlendedWriterCount",
            "g_paletteProvenanceRawGlobalArenaCount",
            "g_paletteProvenanceUnknownCount",
        )
        for name in names:
            self.assertEqual(2, self.contract.count(name))
            self.assertNotIn(f"{name}.load(", self.contract)


if __name__ == "__main__":
    unittest.main()
