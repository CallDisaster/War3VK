#!/usr/bin/env python3
"""Contracts for same-frame Visible main-world backing proof handoff."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = DEVICE.index(signature)
    brace = DEVICE.index("{", start)
    depth = 0
    for pos in range(brace, len(DEVICE)):
        if DEVICE[pos] == "{":
            depth += 1
        elif DEVICE[pos] == "}":
            depth -= 1
            if depth == 0:
                return DEVICE[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


class DirectMainWorldBackingHandoffStaticTests(unittest.TestCase):
    def test_value_validator_retains_all_canonical_checks(self) -> None:
        validator = function_body(
            "bool War3SemanticDirectPacketMatchesMainWorldVisibleRecord("
        )
        for token in (
            "VisibleRenderableQueueKind::MainQueue",
            "visible.identity.groupIdx > 0",
            "renderable.unitPtr == identity.unitPtr",
            "renderable.worldObjectEntry == identity.worldObjectEntry",
            "identity.jHandle == renderable.jHandle",
            "identity.handleId == renderable.jHandle",
            "visible.sceneNode != renderable.sceneNode",
            "visible.meshData != renderable.meshData",
            "War3SemanticDirectMainWorldBackingStatus::Pass",
        ):
            self.assertIn(token, validator)

    def test_canonical_fallback_still_queries_current_snapshot(self) -> None:
        fallback = function_body(
            "bool War3SemanticDirectPacketHasMainWorldVisibleBacking("
        )
        self.assertIn("queryByRenderablePartAndLayer(", fallback)
        self.assertIn(
            "War3SemanticDirectPacketMatchesMainWorldVisibleRecord(", fallback
        )

    def test_builder_only_prevalidates_an_exact_preselected_value(self) -> None:
        builder = function_body(
            "bool War3TryBuildShadowPacketFromCurrentDrawRecord("
        )
        gate = builder.index("const bool preselectedVisibleMatches")
        validate = builder.index(
            "War3SemanticDirectPacketMatchesMainWorldVisibleRecord(", gate
        )
        resource = builder.index("War3PacketBuildPhase::ResourceSetup", gate)
        self.assertLess(validate, resource)
        block = builder[gate:validate]
        for token in (
            "record.renderablePart != nullptr",
            "preselectedVisibleRecord != nullptr",
            "preselectedVisibleRecord->renderablePart == record.renderablePart",
            "preselectedVisibleRecord->layerIndex == record.layerIndex",
            "outPrevalidatedMainWorldBackingStatus != nullptr",
        ):
            self.assertIn(token, block)

    def test_eligibility_and_live_lease_reuse_the_value_or_fallback(self) -> None:
        eligibility = function_body(
            "bool War3LooksSubmitEligibleForDirectCurrentDrawFast("
        )
        self.assertIn("prevalidatedMainWorldBackingStatus", eligibility)
        self.assertIn(
            "War3SemanticDirectPacketHasMainWorldVisibleBacking(", eligibility
        )
        self.assertIn(
            "backingStatus == War3SemanticDirectMainWorldBackingStatus::Pass",
            eligibility,
        )

        populate = function_body(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        self.assertIn(
            "War3SemanticDirectMainWorldBackingStatus mainWorldBackingStatus",
            populate,
        )
        self.assertIn("&eligible.mainWorldBackingStatus", populate)
        lease = populate[
            populate.index("auto packetSafeForDirectPartLease") :
            populate.index("// Both lists are bounded", populate.index("auto packetSafeForDirectPartLease"))
        ]
        self.assertIn("eligible.mainWorldBackingStatus", lease)
        self.assertIn("War3SemanticDirectPacketHasMainWorldVisibleBacking(", lease)


if __name__ == "__main__":
    unittest.main()
