import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_shadow_observer_build_policy.h"
).read_text(encoding="utf-8", errors="replace")
OPTIONS = (ROOT / "meson_options.txt").read_text(
    encoding="utf-8", errors="replace"
)


class ShadowCapturePostBreakdownDevStaticTests(unittest.TestCase):
    def test_release_default_has_no_post_breakdown_capability(self):
        self.assertIn("kDevelopmentShadowObserversEnabled = false", POLICY)
        option = OPTIONS[OPTIONS.index("option('warvk_shadow_observers_dev'") :]
        self.assertIn("value : false", option[:240])

    def test_post_breakdown_uses_the_dev_observer_compile_gate(self):
        start = DEVICE.index("inline bool War3ShadowCapturePostBreakdownRuntime()")
        body = DEVICE[start : DEVICE.index("inline dxvk::war3::render::", start)]
        self.assertIn("kDevelopmentShadowObserversEnabled", body)
        self.assertIn("DXVK_WAR3_SHADOW_CAPTURE_BREAKDOWN", body)
        self.assertNotIn("War3TerrainBoundsCullMode::Consume", body)
        self.assertNotIn("War3UnionCullMode::Consume", body)


if __name__ == "__main__":
    unittest.main()
