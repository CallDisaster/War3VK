import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / "src" / "d3d9" / "d3d9_device.cpp"


class ShadowReplayDomainPropagationStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = DEVICE.read_text(encoding="utf-8")

    def test_shared_helper_copies_complete_rebased_domain_contract(self) -> None:
        helper = re.search(
            r"void War3CopyDrawTimeReplayDomain\(.*?\n\}",
            self.source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(helper)
        body = helper.group(0)
        for field in (
            "shadowActualIndexMin",
            "shadowActualIndexMax",
            "shadowActualIndexDomainKnown",
            "shadowFullVertexDomainFallback",
            "shadowIndexHintMismatch",
        ):
            self.assertIn(field, body)

    def test_all_three_draw_time_entry_consumers_apply_domain_contract(self) -> None:
        # DirectGrouped override, the exact producer, and FastAppend must all
        # carry the same addressing proof into the final caster.
        calls = re.findall(
            r"War3CopyDrawTimeReplayDomain\(draw, entry\);", self.source
        )
        self.assertEqual(len(calls), 3)

    def test_negative_offset_remains_guarded_by_exact_domain(self) -> None:
        self.assertIn("consumeVertexOffset = -int32_t(actualIndexMin);", self.source)
        self.assertIn("actualIndexDomainKnown = actualIndexMin <= actualIndexMax;", self.source)

    def test_legacy_exact_trim_publishes_scanned_domain(self) -> None:
        # The legacy current-frame/Arena lane independently compacts dynamic
        # Stage10 terrain-decoration streams. It must rewrite the exact IB
        # range to a zero-based domain rather than pair the compact VB with a
        # negative vertexOffset.
        for token in (
            "exactIndexedFreezeMinIndex = exactDomain.minIndex;",
            "exactIndexedFreezeMaxIndex = exactDomain.maxIndex;",
            "RebaseWar3ExactIndexDomain(",
            "draw.firstIndex = exactIndexedFreezeRebased ? 0u : StartVal;",
            "? 0u : exactIndexedFreezeMinIndex;",
            "? 0\n             : int32_t(int64_t(BaseVertexIndex)",
            "draw.shadowActualIndexDomainKnown = true;",
        ):
            self.assertIn(token, self.source)

    def test_exact_trim_has_single_variable_tdr_ab_switch(self) -> None:
        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_SHADOW_EXACT_INDEX_TRIM", 0u)',
            self.source,
        )
        self.assertIn(
            "War3ExactIndexedFreezeTrimRuntime() && indexed &&",
            self.source,
        )


if __name__ == "__main__":
    unittest.main()
