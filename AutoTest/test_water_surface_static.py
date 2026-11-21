"""Guard the water consumer's build, frame admission, and composite contracts."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def code(path):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", source(path), flags=re.S)


CPP = code("src/d3d9/d3d9_war3_water.cpp")
SHADER = code("subprojects/war3fx/shaders/war3_water_surface.frag")


class WaterSurfaceContracts(unittest.TestCase):
    def test_post_pass_keeps_its_shared_lighting_controls_and_its_own_clarity(self):
        push = CPP.split("WaterPush pc = {", 1)[1].split("};", 1)[0]
        for field, maximum in (("reflection", 1), ("specular", 2), ("clarity", 1)):
            self.assertIn(f"setting(water.{field}, defaults.{field}, {maximum}.0f)", push)
            self.assertIn(f"pc.{field}", SHADER)
        self.assertNotIn("caustics", push)
        self.assertNotIn("clarity", code("subprojects/war3fx/shaders/war3_water_in_place.frag.in"))

    def test_only_a_drawn_in_place_frame_suppresses_the_host_composite(self):
        host = code("tools/render_host/dxvk_core/shadow_before_ui.cpp")
        run = host.split("ShadowOutcome ShadowBeforeUi::run(", 1)[1]
        self.assertIn("if (waterSurface && inPlaceWater) ++skippedWaterPasses;", run)
        guarded = re.search(r"if \(waterSurface && !inPlaceWater\)\s*\{([^}]+)\}", run)
        self.assertIsNotNone(guarded)
        self.assertIn("m_state->water->Run(list, input);", guarded[1])
        self.assertEqual(1, run.count("m_state->water->Run("))
        self.assertLess(run.index("m_state->pass->Run(list, input);"), guarded.start())
        self.assertLess(guarded.end(), run.index("m_state->volumetric->Run(list, input);"))

    def test_both_shader_builds(self):
        meson = source("subprojects/war3fx/meson.build")
        for name in ("war3fx_shaders", "war3fx_executor_shaders"):
            with self.subTest(name=name):
                block = re.search(rf"^{name}\s*=\s*files\(\[(.*?)\]\)", meson, re.M | re.S)
                # Dropping this target's list fails even if the other list survives.
                self.assertIsNotNone(block)
                # Removing or duplicating this entry in either build fails its own count.
                self.assertEqual(block[1].count("'shaders/war3_water_surface.frag'"), 1)

    def test_both_cpp_builds(self):
        for path in ("src/d3d9/meson.build", "tools/render_host/dxvk_core/meson.build"):
            with self.subTest(path=path):
                # Removing water from either target could leave a host-only link failure.
                self.assertEqual(source(path).count("d3d9_war3_water.cpp'"), 1)

    def test_disabled_and_dry_frames_return_before_work(self):
        run = CPP.split("void War3WaterSurfacePass::Run(", 1)[1].split("{", 1)[1]
        # Anchoring both guards to entry catches recording, allocation, or a helper
        # moved ahead of either return, as well as removing either guard itself.
        self.assertRegex(run, r"^\s*if\s*\(!input\.settings\s*\|\|\s*!input\.settings->water\.surfaceEnabled"
                             r"\s*\|\|\s*!m_shadow\)\s*return;\s*const auto& frame = m_shadow->waterFrame\(\);"
                             r"\s*if\s*\(!frame\.valid\s*\|\|\s*!ctx\s*\|\|\s*!input\.colorView\)\s*return;")

    def test_premultiplied_rgb_over_loaded_target(self):
        for field in (
            "blend.blendEnable = VK_TRUE;",
            "blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;",
            "blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;",
            "blend.colorBlendOp = VK_BLEND_OP_ADD;",
            "blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;",
            "state.cbAttachment = &blend;",
            "attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;",
            "attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;",
        ):
            with self.subTest(field=field):
                # Changing a factor/op, writing alpha, detaching the state, or discarding
                # the previous attachment contents breaks the corresponding requirement.
                self.assertIn(field, CPP)

    def test_only_visible_water_writes(self):
        main = SHADER.split("void main()", 1)[1]
        output = re.search(r"\bo_color\s*=", main)
        # The accepted water path must still reach a real fragment output.
        self.assertIsNotNone(output)
        before_output = main[:output.start()]
        # Removing the water clear sentinel or replacing discard with a colour write fails.
        self.assertRegex(before_output, r"if\s*\(surfaceRaw == 1\.0 \|\| !usableDepth\(surfaceRaw, epsilon\)\)\s*discard;")
        # Accepting foreground or missing ground, or moving the test after output, fails.
        self.assertRegex(before_output, r"if\s*\(!usableDepth\(groundRaw, epsilon\) \|\|\s*"
                         r"abs\(surfaceRaw - u\.depthContract\.x\) <= abs\(groundRaw - u\.depthContract\.x\)"
                         r" \+ epsilon\)\s*discard;")
        # A simplified depth helper that admits the inferred far clear fails here.
        self.assertIn("abs(raw - u.depthContract.x) > epsilon", SHADER)
        # Removing the last nonfinite guard could write NaNs to the loaded target.
        self.assertRegex(before_output, r"if\s*\(!finite\(contribution\)\)\s*discard;\s*$")


if __name__ == "__main__":
    unittest.main()
