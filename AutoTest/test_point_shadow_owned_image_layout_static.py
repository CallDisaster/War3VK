#!/usr/bin/env python3
"""Static contracts for point-shadow cube image ownership."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/d3d9_war3_shadow.h").read_text(
    encoding="utf-8", errors="replace"
)
SOURCE = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8", errors="replace"
)
RESOURCES = (ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp").read_text(
    encoding="utf-8", errors="replace"
)


class PointShadowOwnedImageLayoutStaticTests(unittest.TestCase):
    def test_cube_tracks_each_face_and_neutral_resource_explicitly(self):
        self.assertIn("m_pointShadowFaceLayouts", HEADER)
        self.assertIn("m_pointShadowNeutralLayout", HEADER)
        self.assertNotIn("m_pointShadowCubeLayoutInitialized", HEADER)
        self.assertNotIn("m_pointShadowNeutralReady", HEADER)

    def test_recreated_resources_reset_owned_layout_state(self):
        self.assertIn("m_pointShadowNeutralLayout.reset();", RESOURCES)
        self.assertRegex(
            RESOURCES,
            r"for \(auto& layout : m_pointShadowFaceLayouts\)\s+layout\.reset\(\);",
        )

    def test_cube_and_views_declare_their_real_usages(self):
        cube_block = RESOURCES[
            RESOURCES.index('info.debugName = "War3PointShadowCubeArray"') - 900 :
            RESOURCES.index('info.debugName = "War3PointShadowCubeArray"') + 1800
        ]
        self.assertIn("VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT", cube_block)
        self.assertIn("VK_IMAGE_USAGE_SAMPLED_BIT", cube_block)
        self.assertIn("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT", cube_block)
        self.assertIn("VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT", cube_block)
        self.assertRegex(
            cube_block,
            r"viewInfo\.usage\s*=\s*VK_IMAGE_USAGE_SAMPLED_BIT",
        )
        self.assertRegex(
            cube_block,
            r"faceViewInfo\.usage\s*=\s*VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT",
        )

        neutral_block = RESOURCES[
            RESOURCES.index("void War3ShadowReceiverPass::ensurePointShadowNeutralResources") :
            RESOURCES.index("void War3ShadowReceiverPass::ensurePointShadowResources")
        ]
        self.assertIn("VK_IMAGE_USAGE_TRANSFER_DST_BIT", neutral_block)
        self.assertIn("VK_IMAGE_USAGE_SAMPLED_BIT", neutral_block)

    def test_face_write_and_sample_transitions_use_owned_state(self):
        point_block = SOURCE[
            SOURCE.index("void War3ShadowReceiverPass::renderPointShadow(") :
            SOURCE.index("void War3ShadowReceiverPass::drawReceiver(")
        ]
        self.assertIn("transitionPointShadowWriteLayers", point_block)
        self.assertIn("m_pointShadowFaceLayouts[layer].plan", point_block)
        self.assertIn("CommitWar3OwnedImageLayout", point_block)
        self.assertIn("VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL", point_block)
        self.assertIn("VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL", point_block)
        self.assertNotRegex(
            point_block,
            r"(?:toWrite|toRead|restoreRead)\.oldLayout\s*=",
        )

    def test_neutral_cube_is_cleared_before_sampling(self):
        receiver_prefix = SOURCE[
            SOURCE.index("void War3ShadowReceiverPass::drawReceiver(") :
            SOURCE.index("const bool pointShadowPublicationCurrent")
        ]
        self.assertIn("VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL", receiver_prefix)
        self.assertIn("cmdClearDepthStencilImage", receiver_prefix)
        self.assertIn("const VkClearDepthStencilValue neutralDepth = {1.0f, 0u}", receiver_prefix)
        self.assertIn("m_pointShadowNeutralLayout.plan", receiver_prefix)
        self.assertIn("CommitWar3OwnedImageLayout", receiver_prefix)


if __name__ == "__main__":
    unittest.main()
