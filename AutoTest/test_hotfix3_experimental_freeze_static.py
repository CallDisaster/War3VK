"""Static contracts for the Hotfix3 correctness-only release freeze."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
DEVICE = ROOT / "src/d3d9/d3d9_device.cpp"
SHADOW = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
HOOKS = ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp"
CENSUS = ROOT / "src/d3d9/war3/tools/war3_resource_residency_census.cpp"
GPU_SKIN = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp"


class Hotfix3ExperimentalFreezeContracts(unittest.TestCase):
    def test_release_freeze_is_compile_time_enabled(self) -> None:
        text = CONFIG.read_text(encoding="utf-8")
        self.assertIn(
            "kReleaseFreezeExperimentalShadowRoutes = true", text
        )

    def test_shadow_optimization_modes_are_forced_off(self) -> None:
        device = DEVICE.read_text(encoding="utf-8")
        shadow = SHADOW.read_text(encoding="utf-8")
        for token in (
            "War3CompactWorkTableMode::Off",
            "War3ProducerClaimObserveMode::Off",
            "War3TerrainBoundsCullMode::Off",
            "Mode::Off",
        ):
            self.assertIn(token, device)
        self.assertIn("War3UnionVisibilityMode::Off", shadow)
        self.assertIn("War3TerrainBoundsCullMode::Off", shadow)
        self.assertIn(
            "return War3PointShadowPersistentMode::Off;", shadow
        )
        self.assertIn(
            "!war3::internal::kReleaseFreezeExperimentalShadowRoutes",
            shadow,
        )

    def test_diagnostic_observers_cannot_be_reenabled_by_environment(self) -> None:
        for path in (HOOKS, CENSUS):
            text = path.read_text(encoding="utf-8")
            self.assertIn("kReleaseFreezeExperimentalShadowRoutes", text)
            self.assertIn("return false;", text)
        gpu_skin = GPU_SKIN.read_text(encoding="utf-8")
        self.assertIn("config.mode = GpuSkinMode::Disabled;", gpu_skin)
        self.assertIn("config.diffSamplePeriod = 0u;", gpu_skin)
        self.assertIn("config.fullDiagnostics = false;", gpu_skin)

    def test_type0_correctness_and_current_frame_capture_remain_enabled(self) -> None:
        hooks = HOOKS.read_text(encoding="utf-8")
        device = DEVICE.read_text(encoding="utf-8")
        self.assertIn("Hook_TransparentDispatchType0Semantic", hooks)
        self.assertIn("Type0 is a correctness boundary", hooks)
        self.assertIn(
            'War3GetEnvU32("DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY", 1u)',
            device,
        )


if __name__ == "__main__":
    unittest.main()
