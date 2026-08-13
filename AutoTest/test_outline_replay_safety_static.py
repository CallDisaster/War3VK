#!/usr/bin/env python3
"""Static contracts for fail-closed outline replay and mask layouts."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OUTLINE = ROOT / "src/d3d9/d3d9_war3_shadow_outline.cpp"
RESOURCES = ROOT / "src/d3d9/d3d9_war3_shadow_resources.cpp"
HEADER = ROOT / "src/d3d9/d3d9_war3_shadow.h"


def function_body(text: str, signature: str, next_signature: str | None) -> str:
    begin = text.index(signature)
    end = text.index(next_signature, begin) if next_signature else len(text)
    return text[begin:end]


class OutlineReplaySafetyStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.outline = OUTLINE.read_text(encoding="utf-8")
        cls.resources = RESOURCES.read_text(encoding="utf-8")
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.screen = function_body(
            cls.outline,
            "void War3ShadowReceiverPass::renderUnitOutlineScreenSpace(",
            "void War3ShadowReceiverPass::renderUnitOutline(",
        )
        cls.geometry = function_body(
            cls.outline,
            "void War3ShadowReceiverPass::renderUnitOutline(",
            None,
        )

    def test_both_outline_routes_validate_complete_target_batch_first(self) -> None:
        for body, consumer in (
            (self.screen, '"outline-screen-space"'),
            (self.geometry, '"outline-geometry"'),
        ):
            validate = body.index("validateShadowReplayDraws")
            begin = body.index("cmdBeginRendering")
            draw = body.index("cmdDraw")
            self.assertLess(validate, begin)
            self.assertLess(validate, draw)
            self.assertIn(consumer, body)
            self.assertIn("return;", body[validate:begin])

    def test_screen_space_draws_only_the_prevalidated_targets(self) -> None:
        self.assertIn("std::vector<const War3ShadowCasterDraw*> outlineDraws", self.screen)
        self.assertIn("outlineDrawIndices", self.screen)
        loop = self.screen.index("for (uint32_t targetIndex")
        begin = self.screen.index("ctx->cmdBeginRendering")
        self.assertGreater(loop, begin)
        self.assertNotIn(
            "const auto &draw = input.scene.shadowCasters[drawIdx]",
            self.screen[loop:],
        )

    def test_mask_view_advertises_both_actual_usages(self) -> None:
        start = self.resources.index("void War3ShadowReceiverPass::ensureOutlineMaskResources")
        body = self.resources[start:]
        self.assertIn(
            "VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT",
            body,
        )

    def test_mask_layout_is_renderer_owned_not_inferred_from_view(self) -> None:
        self.assertIn("m_outlineMaskLayoutState", self.header)
        self.assertIn("PlanWar3OutlineMaskBegin", self.screen)
        self.assertIn("PlanWar3OutlineMaskEnd", self.screen)
        self.assertIn("VK_IMAGE_LAYOUT_UNDEFINED", self.screen)
        self.assertIn("VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", self.screen)
        self.assertNotIn("getLayout()", self.screen)


if __name__ == "__main__":
    unittest.main()
