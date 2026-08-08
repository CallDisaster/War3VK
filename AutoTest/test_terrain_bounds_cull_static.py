"""Static contracts for the generation-backed terrain cascade observer."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "src/d3d9/war3/render/war3_shadow_bounds_policy.h"
SCENE = ROOT / "src/d3d9/d3d9_war3_scene.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
CONTROL = ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp"


class TerrainBoundsCullContracts(unittest.TestCase):
    def test_mode_is_explicit_and_defaults_to_off(self) -> None:
        policy = POLICY.read_text(encoding="utf-8")
        for token in ("Off = 0u", "Observe = 1u", "Consume = 2u"):
            self.assertIn(token, policy)
        device = DEVICE.read_text(encoding="utf-8")
        shadow = SHADOW.read_text(encoding="utf-8")
        for content in (device, shadow):
            self.assertIn("DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE", content)
            self.assertIn("DXVK_WAR3_CSM_TERRAIN_BOUNDS_CULL", content)
            self.assertIn("War3TerrainBoundsCullMode::Off", content)

    def test_only_exact_current_bounds_can_authorize_culling(self) -> None:
        policy = POLICY.read_text(encoding="utf-8")
        for token in (
            "GenericDiagnostic",
            "ExactCurrentWorld",
            "SourceGenerationUnknown",
            "FrameGenerationStale",
            "IdentityUnproven",
            "DynamicOrSkinned",
            "AnimatedAttachment",
            "NonFiniteBounds",
            "InvalidRadius",
        ):
            self.assertIn(token, policy)
        scene = SCENE.read_text(encoding="utf-8")
        for token in (
            "boundsProvenance",
            "boundsSourceGeneration",
            "boundsFrameSerial",
            "boundsSourceWasSkinned",
            "boundsFrameLocalDynamic",
            "boundsAnimatedAttachment",
        ):
            self.assertIn(token, scene)

    def test_capture_uses_validated_span_generation(self) -> None:
        device = DEVICE.read_text(encoding="utf-8")
        self.assertIn(
            "terrainPositionReadableSpan.contentGeneration", device
        )
        self.assertIn(
            "War3ShadowBoundsProvenance::ExactCurrentWorld", device
        )
        self.assertIn(
            "m_war3ShadowPersistentFrameSerial + 1u", device
        )

    def test_observe_never_changes_the_draw_mask(self) -> None:
        shadow = SHADOW.read_text(encoding="utf-8")
        self.assertIn("terrainBoundsWouldCullCount", shadow)
        self.assertIn("terrainBoundsAppliedCullCount", shadow)
        self.assertIn("const bool consumeTerrainCascade", shadow)
        self.assertIn("c >= 2u", shadow)
        self.assertIn("? !consumeTerrainCascade || terrainWouldBeVisible", shadow)
        self.assertIn("m_shadowCascadeVisibilityMasksScratch", shadow)

    def test_runtime_status_exposes_observe_and_consume_counts(self) -> None:
        control = CONTROL.read_text(encoding="utf-8")
        for token in (
            "semanticSceneTerrainBoundsCullMode",
            "semanticSceneTerrainBoundsCandidateCount",
            "semanticSceneTerrainBoundsProofAcceptedCount",
            "semanticSceneTerrainBoundsFailVisibleCount",
            "semanticSceneTerrainBoundsWouldCullCount",
            "semanticSceneTerrainBoundsAppliedCullCount",
        ):
            self.assertIn(token, control)


if __name__ == "__main__":
    unittest.main()
