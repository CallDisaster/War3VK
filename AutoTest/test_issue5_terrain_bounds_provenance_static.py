"""Static contracts for Issue #5 exact terrain bounds provenance."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "src/d3d9/war3/render/war3_terrain_bounds_provenance.h"
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"


class TerrainBoundsProvenanceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = HELPER.read_text(encoding="utf-8")
        cls.scene = SCENE.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")

    def test_indexed_hints_cannot_authorize_bounds(self) -> None:
        self.assertIn("War3ResolveTerrainBoundsVertexRange", self.helper)
        self.assertIn("if (!exactIndexedDomainKnown", self.helper)
        self.assertIn("return {};", self.helper)
        self.assertIn("exactIndexedDomainKnown = true", self.device)
        scan = self.device.index("ComputeWar3ExactIndexVertexDomainPrepared")
        known = self.device.index("exactIndexedDomainKnown = true", scan)
        self.assertGreater(known, scan)

    def test_persistent_geometry_owns_exact_local_bounds(self) -> None:
        for token in (
            "localBoundsSourceGeneration",
            "localBoundsIdentityProven",
        ):
            self.assertIn(token, self.scene)
        miss = self.device.index("S1 can return through the persistent path")
        create = self.device.index(
            "War3CreateShadowPersistentGeometryAfterMiss", miss
        )
        body = self.device[miss:create]
        for token in (
            "War3ResolveTerrainBoundsVertexRange",
            "BuildWar3CpuReadableBufferSpan",
            "War3ComputeCachedMappedTerrainBoundsFromSpan",
            "candidate.localBoundsIdentityProven",
        ):
            self.assertIn(token, body)
        self.assertIn(
            "stored.localBoundsSourceGeneration = uint64_t(geometryId)",
            self.device,
        )

    def test_persistent_and_early_hits_refresh_current_frame_bounds(self) -> None:
        compat = self.device.index(
            "terrainS1Caster && geometry->localBoundsIdentityProven"
        )
        finalize = self.device.index("finalizeShadowDrawCommon(compatDraw)", compat)
        self.assertLess(compat, finalize)
        early = self.device.index(
            "earlyPersistentGeometry->localBoundsIdentityProven"
        )
        publish = self.device.index(
            "m_war3Scene.shadowCasters.emplace_back", early
        )
        early_body = self.device[early:publish]
        for token in (
            "War3ApplySemanticBoundsFromMatrix",
            "ExactLocalGeoset",
            "m_war3ShadowPersistentFrameSerial + 1u",
            "War3ShadowBoundsProvenance::Unknown",
        ):
            self.assertIn(token, early_body)

    def test_stage10_and_fallback_use_same_exact_range_policy(self) -> None:
        fallback = self.device.index("else if (terrainCaster &&")
        apply = self.device.index(
            "if (terrainCaster && estimatedBoundsRadius > 0.0f)", fallback
        )
        body = self.device[fallback:apply]
        self.assertIn("War3ResolveTerrainBoundsVertexRange", body)
        self.assertIn("terrainVertexRange.exact", body)
        self.assertNotIn(
            "int64_t(BaseVertexIndex) + int64_t(MinVertexIndex)", body
        )

    def test_release_path_still_requires_observer_mode(self) -> None:
        miss = self.device.index("S1 can return through the persistent path")
        create = self.device.index(
            "War3CreateShadowPersistentGeometryAfterMiss", miss
        )
        body = self.device[miss:create]
        self.assertIn("War3TerrainBoundsCullModeRuntime()", body)
        self.assertIn("War3TerrainBoundsCullMode::Off", body)


if __name__ == "__main__":
    unittest.main()
