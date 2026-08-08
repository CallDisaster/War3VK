#!/usr/bin/env python3
"""Static contracts for generation-backed non-terrain CSM bounds."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODEL_H = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
MODEL_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
RUNTIME_H = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.h"
RUNTIME_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
RENDERER_H = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.h"
RENDERER_CPP = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
CANONICAL_H = ROOT / "src/d3d9/war3/render/war3_canonical_draw.h"
CANONICAL_CPP = ROOT / "src/d3d9/war3/render/war3_canonical_draw.cpp"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"


class GenerationBackedObjectBoundsContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.model_h = MODEL_H.read_text(encoding="utf-8")
        cls.model_cpp = MODEL_CPP.read_text(encoding="utf-8")
        cls.runtime_h = RUNTIME_H.read_text(encoding="utf-8")
        cls.runtime_cpp = RUNTIME_CPP.read_text(encoding="utf-8")
        cls.renderer_h = RENDERER_H.read_text(encoding="utf-8")
        cls.renderer_cpp = RENDERER_CPP.read_text(encoding="utf-8")
        cls.canonical_h = CANONICAL_H.read_text(encoding="utf-8")
        cls.canonical_cpp = CANONICAL_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE.read_text(encoding="utf-8")
        cls.scene = SCENE.read_text(encoding="utf-8")
        cls.shadow = SHADOW.read_text(encoding="utf-8")

    def test_cache_derives_bounds_only_after_generation_assignment(self) -> None:
        store = self.model_cpp.split(
            "ShadowModelResourceCache::storeGeosetRecord", 1
        )[1].split("void ShadowModelResourceCache::storeModelRecord", 1)[0]
        generation = store.index("merged.immutableModelGeneration = generation")
        bounds = store.index("ComputeShadowGeosetLocalBounds")
        self.assertLess(generation, bounds)
        self.assertIn("generation != 0u", store[generation:bounds + 200])
        self.assertIn("ShadowGeosetLocalBounds localBounds", self.model_h)

    def test_unresolved_and_failed_publications_clear_bounds(self) -> None:
        replace = self.model_cpp.split(
            "void ReplaceGeosetImmutablePayload", 1
        )[1].split("void ShadowModelResourceCache::endFrame", 1)[0]
        self.assertIn("dst.localBounds = {}", replace)
        store = self.model_cpp.split(
            "ShadowModelResourceCache::storeGeosetRecord", 1
        )[1].split("void ShadowModelResourceCache::storeModelRecord", 1)[0]
        self.assertGreaterEqual(store.count("localBounds = {}"), 2)

    def test_authority_is_carried_through_all_value_layers(self) -> None:
        for text in (self.runtime_h, self.renderer_h, self.canonical_h):
            self.assertIn("uint64_t mapEpoch", text)
            self.assertIn("uint64_t immutableModelGeneration", text)
            self.assertIn("ShadowGeosetLocalBounds localBounds", text)
        for text in (self.runtime_cpp, self.renderer_cpp, self.canonical_cpp):
            self.assertIn("immutableModelGeneration", text)
            self.assertIn("localBounds", text)

    def test_static_cull_gate_is_narrow_and_identity_explicit(self) -> None:
        for token in (
            "exactStaticKind",
            "!skinned",
            "!frameLocalDynamicGeometry",
            "CanonicalGeometrySource::StaticResource",
            "canonicalMesh.mapEpoch == m_war3GpuSkinMapEpoch",
            "canonicalMesh.immutableModelGeneration != 0u",
            "canonicalMesh.localBounds.valid",
            "canonicalIdentity.meshDataPtr ==",
            "canonicalIdentity.runtimeGeosetDataPtr",
            "CanonicalWorldTransformSource::SceneNode",
            "War3ShadowBoundsProvenance::ExactLocalGeoset",
            "draw.boundsIdentityProven = true",
        ):
            self.assertIn(token, self.device)

    def test_draw_time_override_cannot_inherit_static_authority(self) -> None:
        override = self.device.split(
            "if (drawTimeVBOverrideApplied) {", 1
        )[1].split("instance.paletteIndex", 1)[0]
        self.assertIn("War3ShadowBoundsProvenance::Unknown", override)
        self.assertIn("draw.boundsSourceGeneration = 0u", override)
        self.assertIn("draw.boundsIdentityProven = false", override)
        self.assertIn("draw.boundsFrameLocalDynamic = true", override)

    def test_final_policy_uses_explicit_identity_bit(self) -> None:
        self.assertIn("bool boundsIdentityProven = false", self.scene)
        policy = self.shadow.split("const auto evaluateBoundsPolicy", 1)[1].split(
            "auto intersectsCascadeUnchecked", 1
        )[0]
        self.assertIn("draw.boundsIdentityProven", policy)
        self.assertNotIn("draw.boundsSourceGeneration != 0u", policy)

    def test_runtime_consumption_is_explicit_and_default_off(self) -> None:
        self.assertIn(
            'EnvFlagDefault("DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME", false)',
            self.shadow,
        )
        self.assertIn("s_objectBoundsCullConsume && c >= 2u", self.shadow)
        self.assertIn("!consumeObjectCascade || objectWouldBeVisible", self.shadow)


if __name__ == "__main__":
    unittest.main(verbosity=2)
