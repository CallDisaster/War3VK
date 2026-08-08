"""Cross-map invalidation contracts for raw identity and palette TLS caches."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OBJECTS_H = ROOT / "src/d3d9/war3/render/war3_render_objects.h"
OBJECTS_CPP = ROOT / "src/d3d9/war3/render/war3_render_objects.cpp"
RENDERER = ROOT / "src/d3d9/war3/render/war3_renderer.cpp"
MODEL_HOOK = ROOT / "src/d3d9/war3/model/war3_model_hook.cpp"
SHADOW_CORE = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"


class Issue6RenderIdentityCacheResetContracts(unittest.TestCase):
    def test_render_object_raw_pointer_and_handle_aliases_are_reset(self) -> None:
        header = OBJECTS_H.read_text(encoding="utf-8")
        source = OBJECTS_CPP.read_text(encoding="utf-8")
        renderer = RENDERER.read_text(encoding="utf-8")
        self.assertIn("ResetRenderObjectMapSessionCaches", header)
        self.assertIn("ResetRenderObjectMapSessionCaches();", renderer)
        start = source.index("void ResetRenderObjectMapSessionCaches()")
        end = source.index("// ============================================================================", start)
        block = source[start:end]
        for token in (
            "handleToRawcode.clear()",
            "handleToUnitPtr.clear()",
            "failedHandles.clear()",
            "loggedRawcodes.clear()",
            "GetUnitMetaCache().clear()",
            "g_tlsCurrentBatchHandle = 0u",
            "g_tlsCurrentBatchObject = nullptr",
        ):
            self.assertIn(token, block)

    def test_runtime_model_positive_tls_cache_is_session_generation_scoped(self) -> None:
        source = MODEL_HOOK.read_text(encoding="utf-8")
        self.assertIn("s_runtimeModelValidationSessionGeneration{1u}", source)
        lookup = source.index("bool LooksLikeRuntimeModelPtrCached")
        hit = source.index("s_validRuntimeModels.find(candidate)", lookup)
        clear = source.index("s_validRuntimeModels.clear()", lookup)
        self.assertLess(clear, hit)
        reset = source.index("void ResetMapSession()")
        self.assertIn(
            "s_runtimeModelValidationSessionGeneration.fetch_add",
            source[reset:],
        )

    def test_shadow_palette_slot_tls_cache_is_session_generation_scoped(self) -> None:
        source = SHADOW_CORE.read_text(encoding="utf-8")
        self.assertIn("g_paletteSlotCacheSessionGeneration{1u}", source)
        lookup = source.index("FindOrUpdatePaletteSlotCache")
        scan = source.index("// 查找缓存", lookup)
        clear = source.index("for (auto& entry : s_paletteSlotCache)", lookup)
        self.assertLess(clear, scan)
        reset = source.index("void ShadowValidationRuntime::reset()")
        self.assertIn(
            "g_paletteSlotCacheSessionGeneration.fetch_add",
            source[reset:],
        )

    def test_map_reset_keeps_model_hooks_installed(self) -> None:
        source = MODEL_HOOK.read_text(encoding="utf-8")
        start = source.index("void ResetMapSession()")
        end = source.index("void Shutdown()", start)
        block = source[start:end]
        self.assertNotIn("g_active.store(false", block)
        self.assertNotIn("g_fullHooksInstalled.store(false", block)
        self.assertNotIn("MH_DisableHook", block)


if __name__ == "__main__":
    unittest.main()
