import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
    encoding="utf-8"
)
SHADOW_H = (ROOT / "src/d3d9/d3d9_war3_shadow.h").read_text(
    encoding="utf-8"
)
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
PIPELINE = (ROOT / "src/d3d9/d3d9_war3_pipeline.cpp").read_text(
    encoding="utf-8"
)
PERF_H = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
).read_text(encoding="utf-8")
PERF_CPP = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")
INTERNAL_CONFIG = (
    ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
).read_text(encoding="utf-8")


class ShadowTaaStage0StaticTests(unittest.TestCase):
    def test_three_modes_default_to_direct_inline(self):
        self.assertIn("enum class War3ShadowTaaMode", SETTINGS)
        self.assertIn("DirectInline = 0", SETTINGS)
        self.assertIn("PrepassCurrentOnly = 1", SETTINGS)
        self.assertIn("Temporal = 2", SETTINGS)
        self.assertIn(
            "shadowTaaMode = War3ShadowTaaMode::DirectInline", SETTINGS
        )
        self.assertIn("bool shadowTaaEnabled = false", SETTINGS)

    def test_new_mode_switch_precedes_legacy_switch(self):
        mode_pos = PIPELINE.index("DXVK_WAR3_SHADOW_TAA_MODE")
        legacy_pos = PIPELINE.index("DXVK_WAR3_SHADOW_TAA\"")
        self.assertLess(mode_pos, legacy_pos)
        self.assertIn(
            "ParseInitialShadowTaaMode(initialShadowTaaMode)", PIPELINE
        )
        resolve_start = SHADOW_CPP.index("ResolveShadowTaaRequestedMode")
        resolve_end = SHADOW_CPP.index(
            "ShadowTaaDisableForSemanticDynamicEnabled", resolve_start
        )
        self.assertNotIn("getEnvVar", SHADOW_CPP[resolve_start:resolve_end])

    def test_runtime_module_gates_all_auxiliary_work(self):
        self.assertIn(
            "IsWar3RuntimeModuleEnabled(\n"
            "          war3::runtime::War3RuntimeModule::ShadowTaa)",
            SHADOW_CPP,
        )
        self.assertIn(
            "allowShadowTaaAuxiliaryPasses &&\n"
            "        (shadowTaaTemporalActive || debugMotionVector)",
            SHADOW_CPP,
        )
        self.assertIn(
            "allowShadowTaaAuxiliaryPasses &&\n"
            "        (shadowTaaActive || debugShadowCurrent",
            SHADOW_CPP,
        )
        self.assertIn(
            "if (allowShadowTaaAuxiliaryPasses &&\n"
            "        (shadowTaaTemporalActive || debugShadowHistory)",
            SHADOW_CPP,
        )

    def test_history_advance_requires_four_recorded_stages(self):
        self.assertIn(
            "reconciliation.shadowMotionVectorExecutedThisFrame = 1u",
            SHADOW_CPP,
        )
        self.assertIn(
            "reconciliation.shadowHistoryWriteExecutedThisFrame =",
            SHADOW_CPP,
        )
        closure = (
            "shadowTaaTemporalActive &&\n"
            "        reconciliation.shadowVisibilityExecutedThisFrame != 0u &&\n"
            "        reconciliation.shadowMotionVectorExecutedThisFrame != 0u &&\n"
            "        reconciliation.receiverDrawExecutedThisFrame != 0u &&\n"
            "        shadowHistoryWriteExecuted"
        )
        self.assertIn(closure, SHADOW_CPP)

    def test_dynamic_and_sun_policies_use_compile_time_defaults(self):
        self.assertIn(
            "war3::internal::kShadowDisableTaaForSemanticDynamicCasters",
            SHADOW_CPP,
        )
        self.assertIn(
            "war3::internal::kShadowSunMotionAwareTaaDisable", SHADOW_CPP
        )
        self.assertIn("shadowTaaBlockedForSunMotion", SHADOW_CPP)
        self.assertIn("shadowTaaBlockedForSemanticDynamic", SHADOW_CPP)
        self.assertIn(
            "kShadowDisableTaaForSemanticDynamicCasters = false",
            INTERNAL_CONFIG,
        )

    def test_fixed_frame_telemetry_and_report_snapshot_are_wired(self):
        self.assertIn("struct ShadowTaaFrameTelemetry", PERF_H)
        self.assertIn("void noteShadowTaaFrame(", PERF_H)
        self.assertIn("noteShadowTaaFrame(telemetry)", SHADOW_CPP)
        self.assertIn('"DXVK_WAR3_SHADOW_TAA_MODE"', PERF_CPP)
        self.assertIn(
            '"DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC"',
            PERF_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_SHADOW_TAA_DISABLE_ON_SUN_MOTION"', PERF_CPP
        )
        self.assertIn('\\"hasShadowTaaTelemetry\\"', PERF_CPP)
        self.assertGreaterEqual(PERF_CPP.count('\\"shadowTaaFramesObserved\\"'), 2)
        self.assertIn("shadowMotionVectorExecutedThisFrame", SHADOW_H)


if __name__ == "__main__":
    unittest.main()
