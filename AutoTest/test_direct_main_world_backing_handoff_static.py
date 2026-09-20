#!/usr/bin/env python3
"""Contracts for same-frame Visible main-world backing proof handoff."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


# M1 迁移：记录级 selector 已迁到语义模块；默认仍按 device.cpp 解析，
# 需要时显式把 source 传成 SEMANTIC。
SEMANTIC = (
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp"
).read_text(encoding="utf-8")


def function_body(signature: str, source: str = DEVICE) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


class DirectMainWorldBackingHandoffStaticTests(unittest.TestCase):
    def test_value_validator_retains_all_canonical_checks(self) -> None:
        validator = function_body(
            "bool War3SemanticDirectPacketMatchesMainWorldVisibleRecord(", SEMANTIC
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
            "bool War3SemanticDirectPacketHasMainWorldVisibleBacking(", SEMANTIC
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
