"""Contract for bypassing redundant ShadowObject lookup on complete packets."""

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
    raise AssertionError("unterminated builder")


class DirectCompleteShadowAugmentBypassStaticTests(unittest.TestCase):
    def setUp(self) -> None:
        self.builder = function_body(
            "bool War3TryBuildShadowPacketFromCurrentDrawRecord("
        )

    def test_bypass_requires_both_identity_and_immutable_owner_proofs(self) -> None:
        start = self.builder.index("const bool currentDrawShadowAugmentComplete")
        lookup = self.builder.index("shadowObjectSnapshotCache->find", start)
        proof = self.builder[start:lookup]
        self.assertIn(
            "currentDrawObjectIdentityComplete && ownerSatisfiedByInstance",
            proof,
        )
        self.assertIn("if (!currentDrawShadowAugmentComplete)", proof)

    def test_identity_and_owner_proofs_remain_strict(self) -> None:
        identity_start = self.builder.index("const bool currentDrawObjectIdentityComplete")
        identity_end = self.builder.index("War3PacketBuildNestedPhase::RenderObjectLookup", identity_start)
        identity = self.builder[identity_start:identity_end]
        for token in (
            "record.worldObjectEntry != nullptr",
            "record.sceneNode != nullptr",
            "record.unitPtr != nullptr",
            "record.jHandle != 0u",
            "record.rawcode != 0u",
            "currentDrawHasUnitKind",
            "visibleHit",
            "visibleRecord.identity.groupIdx >= 0",
            "visibleRecord.identity.flags5C != 0u",
        ):
            self.assertIn(token, identity)

        owner_start = self.builder.index("const auto instanceProvesGeosetOwner")
        owner_end = self.builder.index("bool ownerSatisfiedByInstance", owner_start)
        owner = self.builder[owner_start:owner_end]
        for token in (
            "geosetHit",
            "instanceHit",
            "instanceRecord.runtimeModelPtr != nullptr",
            "geoset->modelResourcePtr != nullptr",
            "instanceRecord.modelResourcePtr == geoset->modelResourcePtr",
            "geoset->modelKey != 0u",
            "instanceRecord.modelKey == geoset->modelKey",
        ):
            self.assertIn(token, owner)

    def test_incomplete_path_keeps_full_shadow_lookup_priority(self) -> None:
        start = self.builder.index("if (!currentDrawShadowAugmentComplete)")
        end = self.builder.index("const void* effectiveRuntimeModelPtr", start)
        fallback = self.builder[start:end]
        self.assertIn("shadowObjectSnapshotCache->find(", fallback)
        self.assertIn("shadowRegistry.findFirstForDirectPacketView(", fallback)
        for token in (
            "record.worldObjectEntry",
            "record.sceneNode",
            "record.jHandle",
            "ownerHit ? ownerBinding.runtimeModelPtr : nullptr",
            "instanceHit ? instanceRecord.runtimeModelPtr : nullptr",
        ):
            self.assertIn(token, fallback)


if __name__ == "__main__":
    unittest.main()
