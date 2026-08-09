"""Static Vulkan synchronization contracts for WarVK shadow depth images."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
SHADERPACK = ROOT / "src/d3d9/war3_shaderpack.cpp"


class ShadowDepthSynchronizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shadow = SHADOW.read_text(encoding="utf-8")
        cls.shaderpack = SHADERPACK.read_text(encoding="utf-8")

    def _requires_complete_depth_source_scope(self, block: str) -> None:
        self.assertIn("VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT", block)
        self.assertIn("VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT", block)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT", block)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT", block)

    def test_csm_final_transition_waits_for_early_and_late_depth(self) -> None:
        start = self.shadow.index("// 3) Transition shadow map back to read-only")
        end = self.shadow.index("if (terrainCasterMaskEnabled)", start)
        final_transition = self.shadow[start:end]
        if "MakeWar3OwnedImageBarrier" in final_transition:
            attach_start = self.shadow.index(
                "// 2) Transition shadow map to depth attachment"
            )
            attach_end = self.shadow.index(
                "if (terrainCasterMaskEnabled)", attach_start
            )
            self._requires_complete_depth_source_scope(
                self.shadow[attach_start:attach_end]
            )
            self.assertIn("shadowMapLayout.plan", final_transition)
            self.assertIn("CommitWar3OwnedImageLayout", final_transition)
        else:
            self._requires_complete_depth_source_scope(final_transition)

    def test_point_shadow_normal_and_exception_transitions_match(self) -> None:
        point = self.shadow[self.shadow.index(
            "void War3ShadowReceiverPass::renderPointShadow(") :]
        normal_start = point.index("VkImageMemoryBarrier2 toRead")
        normal_end = point.index("pointShadowLayersInAttachmentLayout = false", normal_start)
        self._requires_complete_depth_source_scope(point[normal_start:normal_end])
        restore_start = point.index("VkImageMemoryBarrier2 restoreRead")
        restore_end = point.index("throw;", restore_start)
        self._requires_complete_depth_source_scope(point[restore_start:restore_end])

    def test_terrain_mask_has_explicit_depth_write_to_read_dependency(self) -> None:
        start = self.shadow.index("VkImageMemoryBarrier2 mainDepthToTerrainMask")
        end = self.shadow.index("uint32_t terrainMaskDraws", start)
        block = self.shadow[start:end]
        self.assertIn("VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT", block)
        self.assertIn("VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT", block)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT", block)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT", block)
        self.assertGreaterEqual(
            block.count("VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL"), 2
        )

    def test_shaderpack_depth_transition_uses_complete_scope_both_ways(self) -> None:
        start = self.shaderpack.index("void TransitionDepthToReadOnly(")
        end = self.shaderpack.index("void RestoreDepthLayout(", start)
        self._requires_complete_depth_source_scope(self.shaderpack[start:end])
        restore_end = self.shaderpack.index("void TransitionImageToReadOnly(", end)
        restore = self.shaderpack[end:restore_end]
        self.assertIn("VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT", restore)
        self.assertIn("VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT", restore)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT", restore)
        self.assertIn("VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT", restore)


if __name__ == "__main__":
    unittest.main()
