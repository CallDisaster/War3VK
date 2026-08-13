"""Contracts for canonical CurrentDraw identity before packet construction."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
CONTRACT_H = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"
).read_text(encoding="utf-8")
CONTRACT_CPP = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
).read_text(encoding="utf-8")


def block(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    return text[begin : text.index(end, begin)]


class CurrentDrawExactOwnerPrebuildNormalizationTests(unittest.TestCase):
    def test_canonical_identity_is_one_pure_exact_rule(self) -> None:
        helper = block(
            CONTRACT_H,
            "inline bool CurrentDrawContractHasCanonicalIdentity(",
            "struct CurrentDrawDispatchContext",
        )
        self.assertIn("record.known ||", helper)
        self.assertIn("record.renderablePart != nullptr", helper)
        self.assertIn("record.meshPayloadPtr != nullptr", helper)
        self.assertIn("noexcept", helper)

        resolver = block(
            CONTRACT_CPP,
            "CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSampleFromRecord(",
            "// Phase 7.35",
        )
        self.assertIn(
            "out.contract.known = CurrentDrawContractHasCanonicalIdentity(record);",
            resolver,
        )

    def test_cache_key_and_slice_gate_share_normalization(self) -> None:
        key = block(
            DEVICE,
            "War3DrawTimeVBCacheKey War3MakeDrawTimeVBCacheKey(",
            "bool War3CurrentDrawContractNamesExactSlice(",
        )
        gate = block(
            DEVICE,
            "bool War3CurrentDrawContractNamesExactSlice(",
            "War3ShadowDrawMetadataQuery War3MakeShadowMetadataQuery(",
        )
        self.assertIn(
            "CurrentDrawContractHasCanonicalIdentity(*contract)", key
        )
        self.assertIn("contract->renderablePart == renderablePart", key)
        self.assertIn("contract->layerIndex == layerIndex", key)
        self.assertIn("CurrentDrawContractHasCanonicalIdentity(contract)", gate)
        for token in (
            "contract.renderablePart == renderablePart",
            "contract.layerIndex == layerIndex",
            "contract.meshPayloadPtr != nullptr",
            "War3CurrentDrawInstanceIdentity(contract) != nullptr",
            "contract.jHandle != 0u",
        ):
            self.assertIn(token, gate)

    def test_exact_owner_preselection_precedes_packet_build(self) -> None:
        grouped = block(
            DEVICE,
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(",
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
        )
        preselect = grouped.index("currentFrameDrawTimeProducerOwnsRecord(record)")
        packet_build = grouped.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
        self.assertLess(preselect, packet_build)
        self.assertIn("exactOwner", grouped[preselect:packet_build])

    def test_final_gate_remains_defensive_and_exact(self) -> None:
        append = block(
            DEVICE,
            "// Defensive final ownership gate.",
            "War3FallbackAppendRawTiming fallbackAppendTiming;",
        )
        self.assertIn("War3CurrentDrawContractNamesExactSlice(", append)
        self.assertIn("War3MakeDrawTimeVBCacheKey(", append)
        self.assertIn("War3DrawTimeExactRejectedCurrentFrame(cacheKey)", append)
        self.assertIn("exactOwnerFrameSerial", append)


if __name__ == "__main__":
    unittest.main()
