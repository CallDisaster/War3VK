"""Static contracts for Issue #5 exact terrain bounds provenance."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "src/d3d9/war3/render/war3_terrain_bounds_provenance.h"
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
CONFIG = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"


class TerrainBoundsProvenanceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = HELPER.read_text(encoding="utf-8")
        cls.scene = SCENE.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.config = CONFIG.read_text(encoding="utf-8")

    def test_indexed_hint_is_observer_only_and_trim_stays_exact(self) -> None:
        self.assertIn("War3ResolveTerrainBoundsVertexRange", self.helper)
        self.assertIn("if (!exactIndexedDomainKnown", self.helper)
        self.assertIn(
            "War3ResolveConservativeTerrainIndexedHintRange", self.helper
        )
        self.assertIn("exactIndexedTerrainBoundsAuditSample", self.device)
        self.assertIn("exactIndexedDomainKnown = true", self.device)
        scan = self.device.index("ComputeWar3ExactIndexVertexDomainPrepared")
        known = self.device.index("exactIndexedDomainKnown = true", scan)
        self.assertGreater(known, scan)
        trim = self.device.index(
            "if (exactIndexedFreezeTrimCandidate && allStreamsFit", known
        )
        self.assertGreater(trim, known)

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
            "resolveTerrainBoundsVertexRange",
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
        self.assertIn("resolveTerrainBoundsVertexRange", body)
        self.assertIn("terrainVertexRange.exact", body)

    def test_release_path_still_requires_observer_mode(self) -> None:
        miss = self.device.index("S1 can return through the persistent path")
        create = self.device.index(
            "War3CreateShadowPersistentGeometryAfterMiss", miss
        )
        body = self.device[miss:create]
        self.assertIn("War3TerrainBoundsCullModeRuntime()", body)
        self.assertIn("War3TerrainBoundsCullMode::Off", body)

    def test_dynamic_mapped_bounds_cache_is_development_observe_only(self) -> None:
        self.assertIn(
            "kShadowS1TerrainBoundsCacheCurrentGenerationObserverEnabled",
            self.config,
        )
        dev = self.config.index("WARVK_ENABLE_SHADOW_OBSERVERS_DEV")
        enabled = self.config.index(
            "kShadowS1TerrainBoundsCacheCurrentGenerationObserverEnabled = true",
            dev,
        )
        disabled = self.config.index(
            "kShadowS1TerrainBoundsCacheCurrentGenerationObserverEnabled = false",
            enabled,
        )
        self.assertLess(enabled, disabled)
        cache_gate = self.device.index("const bool allowTerrainBoundsCache")
        cache_call = self.device.index(
            "War3ComputeCachedMappedTerrainBoundsFromSpan(", cache_gate
        )
        body = self.device[cache_gate:cache_call]
        self.assertIn(
            "kShadowS1TerrainBoundsCacheCurrentGenerationObserverEnabled",
            body,
        )
        self.assertIn("terrainPositionReadableSpan", body)
        self.assertIn("keyIdentityGeneration", self.device)
        self.assertIn("keyAllocationGeneration", self.device)
        self.assertIn("keyContentGeneration", self.device)
        self.assertIn(
            "entry.stableSourceKey == useStableSourceKey", self.device
        )
        self.assertNotIn(
            "positionSpan.identityGeneration ^",
            self.device,
            "generation components must be compared exactly, not XOR-folded",
        )



if __name__ == "__main__":
    unittest.main()
