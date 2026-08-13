"""Ensure DirectGrouped classifies each exact Stage11 owner with one key/probe."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def block(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    return text[begin : text.index(end, begin)]


class DirectExactOwnerSingleProbeStaticTests(unittest.TestCase):
    def test_owner_classification_builds_one_key_and_one_cache_probe(self) -> None:
        grouped = block(
            DEVICE,
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(",
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
        )
        owner = block(
            grouped,
            "const auto currentFrameDrawTimeProducerOwnsRecord =",
            "struct DirectObjectCompletenessBucket",
        )
        self.assertEqual(owner.count("War3MakeDrawTimeVBCacheKey("), 1)
        self.assertEqual(owner.count("m_war3DrawTimeVBCache.find(cacheKey)"), 1)
        self.assertNotIn("currentFrameDrawTimeProducerEntry", owner)

    def test_rejection_and_owner_order_remains_fail_closed(self) -> None:
        owner = block(
            DEVICE,
            "const auto currentFrameDrawTimeProducerOwnsRecord =",
            "struct DirectObjectCompletenessBucket",
        )
        anonymous = owner.index("War3DrawTimeAnonymousMarkerRejectionActive(")
        exact = owner.index("War3CurrentDrawContractNamesExactSlice(")
        key = owner.index("War3MakeDrawTimeVBCacheKey(")
        rejected = owner.index("War3DrawTimeExactRejectedCurrentFrame(cacheKey)")
        lookup = owner.index("m_war3DrawTimeVBCache.find(cacheKey)")
        self.assertLess(anonymous, exact)
        self.assertLess(exact, key)
        self.assertLess(key, rejected)
        self.assertLess(rejected, lookup)
        for token in (
            "vbIt->second.MatchesKey(cacheKey)",
            "vbIt->second.frameSerial == m_war3ShadowPersistentFrameSerial",
            "vbIt->second.exactOwnerFrameSerial ==",
        ):
            self.assertIn(token, owner)


if __name__ == "__main__":
    unittest.main()
