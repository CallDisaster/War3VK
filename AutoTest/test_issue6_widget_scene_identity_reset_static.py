#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Issue6WidgetSceneIdentityResetStaticTest(unittest.TestCase):
    def test_widget_pointer_and_handle_cache_has_explicit_map_reset(self):
        header = (ROOT / "src/d3d9/war3/hooks/war3_hook_widget_identity.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "src/d3d9/war3/hooks/war3_hook_widget_identity.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("void ResetWidgetIdentityMapSession();", header)
        self.assertIn("void ResetWidgetIdentityMapSession()", source)
        self.assertIn("cache().byPtr.clear();", source)
        self.assertIn("cache().byHandle.clear();", source)

    def test_present_owned_renderer_reset_invalidates_widget_identity(self):
        source = (ROOT / "src/d3d9/war3/render/war3_renderer.cpp").read_text(
            encoding="utf-8"
        )
        reset = source.split("void War3Renderer::ResetMapSession()", 1)[1]
        reset = reset.split("void War3Renderer::BeginFrame()", 1)[0]
        self.assertIn("hooks::ResetWidgetIdentityMapSession();", reset)

    def test_scene_collector_tls_handle_cache_is_map_epoch_scoped(self):
        source = (ROOT / "src/d3d9/war3/render/war3_scene_collector.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("s_unitHandleCacheMapEpoch", source)
        self.assertIn(
            "model::ShadowModelResourceCache::instance().mapEpoch()", source
        )
        self.assertIn("s_unitHandleCacheMapEpoch != mapEpoch", source)
        self.assertIn("s_unitHandleCache.clear();", source)


if __name__ == "__main__":
    unittest.main()
