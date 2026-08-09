import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
SHADOW_H = ROOT / "src/d3d9/d3d9_war3_shadow.h"
SHADERPACK = ROOT / "src/d3d9/war3_shaderpack.cpp"
POLICY = ROOT / "src/d3d9/war3_shaderpack_policy.h"
UI = ROOT / "src/d3d9/war3/ui/war3_imgui.cpp"
MESON = ROOT / "src/d3d9/meson.build"


def body(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


class War3PipelineLifetimeStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.shadow = SHADOW.read_text(encoding="utf-8")
        cls.shadow_h = SHADOW_H.read_text(encoding="utf-8")
        cls.shaderpack = SHADERPACK.read_text(encoding="utf-8")
        cls.policy = POLICY.read_text(encoding="utf-8")
        cls.ui = UI.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_shadow_bias_invalidation_releases_only_cache_owner(self):
        bias = body(
            self.shadow,
            "if (casterBias != m_shadowCasterBiasConstant",
            "const bool shadowsEnabled",
        )
        self.assertIn("m_shadowCasterPipelines.clear()", bias)
        self.assertNotIn("vk->vkDestroyPipeline", bias)
        self.assertIn("War3TrackedVkPipeline> lifetime", self.shadow_h)
        self.assertIn("AdoptWar3TrackedVkPipeline", self.shadow)
        self.assertEqual(self.shadow.count("ctx->track(pipeline.lifetime)"), 2)
        self.assertGreaterEqual(self.shadow.count("trackCasterPipeline("), 3)

    def test_shaderpack_reload_and_format_change_are_command_list_owned(self):
        clear_pack = body(self.shaderpack, "void ClearPack()", "void InvalidatePassPipelines")
        invalidate = body(self.shaderpack, "void InvalidatePassPipelines", "void EnsureSampler")
        ensure = body(self.shaderpack, "bool EnsurePassPipeline", "void TransitionDepthToReadOnly")
        for section in (clear_pack, invalidate, ensure):
            self.assertNotIn("vkDestroyPipeline", section)
            self.assertIn("pipelineLifetime", section)
        self.assertIn("ctx->track(pass.pipelineLifetime)", self.shaderpack)

    def test_raw_shaderpack_is_compile_time_developer_only(self):
        self.assertIn("WARVK_ENABLE_RAW_SHADERPACK_DEV", self.policy)
        self.assertIn("kRawShaderPackEnabled = false", self.policy)
        self.assertNotIn("getenv", self.policy.lower())
        self.assertIn("RejectRawShaderPackPolicy", self.shaderpack)
        self.assertIn("POLICY_DISABLED", self.shaderpack)
        self.assertIn("IsRawShaderPackLoadingEnabled", self.ui)
        self.assertIn("BeginDisabled(!rawShaderPackEnabled)", self.ui)

        self.assertEqual(
            self.meson.count("-DWARVK_ENABLE_RAW_SHADERPACK_DEV=1"), 1
        )
        production = self.meson[: self.meson.index(
            "war3_shaderpack_policy_dev_test"
        )]
        self.assertNotIn("-DWARVK_ENABLE_RAW_SHADERPACK_DEV=1", production)


if __name__ == "__main__":
    unittest.main()
