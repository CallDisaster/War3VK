import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
SHADOW_H = (ROOT / "src/d3d9/d3d9_war3_shadow.h").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
OUTLINE = (ROOT / "src/d3d9/d3d9_war3_shadow_outline.cpp").read_text(encoding="utf-8")


class RelocatedAlphaDescriptorContract(unittest.TestCase):
    def test_capture_snapshot_is_not_a_replay_binding(self):
        self.assertIn("CurrentTextureDescriptor() const", SCENE)
        helper = SCENE[SCENE.index("CurrentTextureDescriptor() const"):]
        helper = helper[:helper.index("\n        }")]
        self.assertIn("diffuseTexture->getDescriptor()", helper)
        self.assertIn("image.imageView != VK_NULL_HANDLE", helper)
        self.assertNotIn("&draw.textureDescriptor", SHADOW)
        self.assertNotIn("&draw.textureDescriptor", OUTLINE)

    def test_directional_prepare_and_both_replays_share_current_descriptor(self):
        self.assertIn("const DxvkDescriptor* alphaDescriptor = nullptr", SHADOW_H)
        self.assertIn("draw.CurrentTextureDescriptor()", SHADOW)
        self.assertIn("out.alphaDescriptor = effectiveAlphaTestShadow", SHADOW)
        self.assertGreaterEqual(
            SHADOW.count("descriptors[1].descriptor = prep.alphaDescriptor"), 2)
        self.assertNotIn("draw.textureDescriptor.legacy.image.imageView", SHADOW)

    def test_point_shadow_preflight_owns_the_current_descriptor_choice(self):
        preflight = SHADOW[SHADOW.index("pointShadowAlphaDescriptors.assign"):]
        bind = preflight.index(
            "descriptors[1].descriptor = currentAlphaDescriptor")
        self.assertLess(preflight.index("draw.CurrentTextureDescriptor()"), bind)
        self.assertLess(
            preflight.index("pointShadowAlphaDescriptors[drawIdx] ="), bind)

    def test_outline_preflights_before_binding(self):
        for name in ("maskAlphaDescriptors", "outlineAlphaDescriptors"):
            self.assertIn(f"std::vector<const DxvkDescriptor*> {name}", OUTLINE)
            first = OUTLINE.index(f"{name}.push_back(alphaDescriptor)")
            bind = OUTLINE.index(f"descriptors[1].descriptor = {name}[", first)
            self.assertLess(first, bind)
        self.assertGreaterEqual(OUTLINE.count("draw->CurrentTextureDescriptor()"), 2)

    def test_no_graphics_replay_uses_capture_time_descriptor_snapshot(self):
        replay_files = SHADOW + OUTLINE
        self.assertIsNone(re.search(r"draw\.textureDescriptor", replay_files))


if __name__ == "__main__":
    unittest.main()
