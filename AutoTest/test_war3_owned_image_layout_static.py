#!/usr/bin/env python3
"""Static contracts for War3-owned Vulkan image layout tracking."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "src/d3d9/war3/render/war3_owned_image_layout.h"
SHADOW_H = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADOW_RESOURCES = ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"
SHADERPACK = ROOT / "src/d3d9/war3_shaderpack.cpp"
TARGET_SOURCES = (
    ROOT / "src/d3d9/d3d9_war3_aa.cpp",
    ROOT / "src/d3d9/d3d9_war3_shadow.cpp",
    SHADOW_RESOURCES,
    ROOT / "src/d3d9/d3d9_war3_ssao.cpp",
    ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp",
    SHADERPACK,
)


class War3OwnedImageLayoutContracts(unittest.TestCase):
    def test_first_use_is_undefined_with_no_source_dependency(self) -> None:
        text = HELPER.read_text(encoding="utf-8")
        self.assertIn("m_layout = VK_IMAGE_LAYOUT_UNDEFINED", text)
        self.assertIn("m_stages = VK_PIPELINE_STAGE_2_NONE", text)
        self.assertIn("m_access = VK_ACCESS_2_NONE", text)
        self.assertIn("barrier.oldLayout = transition.oldLayout", text)
        self.assertIn("barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED", text)
        self.assertIn("image.trackLayout(subresources, transition.newLayout)", text)

    def test_target_paths_do_not_treat_view_layout_as_current_layout(self) -> None:
        view_layout = re.compile(r"\b(?:\w*[Vv]iew|view)\s*->\s*getLayout\s*\(")
        offenders = []
        for path in TARGET_SOURCES:
            for line_no, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1
            ):
                if view_layout.search(line):
                    offenders.append(f"{path.name}:{line_no}: {line.strip()}")
        self.assertEqual([], offenders)

    def test_receiver_tracks_all_default_owned_images(self) -> None:
        text = SHADOW_H.read_text(encoding="utf-8")
        for state in (
            "m_colorCopyLayout",
            "m_depthCopyLayout",
            "m_motionVectorLayout",
            "m_shadowCurrentLayout",
            "m_shadowHistoryLayouts",
            "m_shadowMapLayout",
            "m_shadowCasterMaskLayout",
            "m_volumeSunShadowLayout",
        ):
            self.assertIn(state, text)

    def test_motion_and_taa_current_views_allow_attachment_use(self) -> None:
        text = SHADOW_RESOURCES.read_text(encoding="utf-8")
        for marker in ("m_motionVectorView", "m_shadowCurrentView"):
            position = text.index(marker)
            creation = text[max(0, position - 900) : position + 200]
            self.assertIn("VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT", creation)
            self.assertIn("VK_IMAGE_USAGE_SAMPLED_BIT", creation)

    def test_shaderpack_early_returns_restore_caller_depth(self) -> None:
        text = SHADERPACK.read_text(encoding="utf-8")
        run = text[text.index("void RunShaderPackPasses(") :]
        no_pass = run.index("if (activePasses == 0)")
        depth_transition = run.index("TransitionDepthToReadOnly")
        self.assertLess(no_pass, depth_transition)
        pipeline_failure = run[
            run.index("if (!EnsurePassPipeline") : run.index(
                "VkExtent3D extent", run.index("if (!EnsurePassPipeline")
            )
        ]
        self.assertIn("RestoreDepthLayout", pipeline_failure)


if __name__ == "__main__":
    unittest.main()
