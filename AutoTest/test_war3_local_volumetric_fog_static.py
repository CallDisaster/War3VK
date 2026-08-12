import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER_H = (ROOT / "src/d3d9/d3d9_war3_fog_volume.h").read_text(
    encoding="utf-8"
)
MANAGER_CPP = (ROOT / "src/d3d9/d3d9_war3_fog_volume.cpp").read_text(
    encoding="utf-8"
)
PASS_H = (ROOT / "src/d3d9/d3d9_war3_volumetric_light.h").read_text(
    encoding="utf-8"
)
PASS_CPP = (ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp").read_text(
    encoding="utf-8"
)
SHADER = (
    ROOT / "subprojects/war3fx/shaders/war3_volumetric_light.frag"
).read_text(encoding="utf-8")
SETTINGS = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
    encoding="utf-8"
)
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
JAPI = (ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp").read_text(
    encoding="utf-8"
)
JASS = (ROOT / "WarVK/jass/warvk_api.j").read_text(encoding="utf-8")
RESEARCH = (
    ROOT / "docs/research/2026-08-12-local-volumetric-fog-and-clear-air-shafts.md"
).read_text(encoding="utf-8")


class War3LocalVolumetricFogStaticTests(unittest.TestCase):
    def test_manager_is_bounded_and_validates_direct_api_inputs(self):
        self.assertIn("static constexpr uint32_t kMaxVolumes = 8u", MANAGER_H)
        for contract in (
            "ValidPosition(position)",
            "ValidHalfSize(halfSize)",
            "ValidDensity(density)",
            "ValidFeather(edgeFeather)",
            "m_volumes.size() >= War3FogVolumeFrameSnapshot::kMaxVolumes",
        ):
            self.assertIn(contract, MANAGER_CPP)
        self.assertIn("std::lock_guard<std::mutex> lock(m_mutex)", MANAGER_CPP)
        self.assertIn("War3FogVolumeFrameSnapshot GetFrameSnapshot", MANAGER_H)

    def test_gpu_contract_uses_fixed_ring_ubo_and_matching_binding(self):
        self.assertIn("VolumetricFogVolumeUniform", PASS_CPP)
        self.assertIn("sizeof(VolumetricFogVolumeUniform) == 528u", PASS_CPP)
        self.assertIn(
            "std::array<Rc<DxvkBuffer>, kUboRingSlots> m_fogVolumeBuffers",
            PASS_H,
        )
        self.assertIn("std::array<DxvkDescriptorSetLayoutBinding, 7>", PASS_CPP)
        self.assertIn("descriptors[6].buffer = fogVolumeInfo", PASS_CPP)
        self.assertRegex(SHADER, r"layout\(set = 1, binding = 6, scalar\)")
        self.assertIn("FogVolume u_volumes[8]", SHADER)

    def test_global_density_zero_keeps_authored_local_medium_alive(self):
        run = PASS_CPP[PASS_CPP.index("void War3VolumetricLightPass::Run"):]
        self.assertIn("localMediumRequested", run)
        self.assertIn("settings.globalMediumEnabled && safeDensity", run)
        self.assertIn("(!hasConfiguredGlobalMedium && !localMediumRequested)", run)
        self.assertIn("if (!hasConfiguredGlobalMedium && !hasLocalMedium)", run)
        self.assertIn("SetVolumetricGlobalMediumEnabled", JAPI)
        self.assertIn("WarVKSetGlobalVolumetricMediumEnabled", JASS)
        self.assertIn("(density <= 1e-6 && !hasFogRayInterval)", SHADER)
        self.assertIn("hasFogRayInterval ? 0.0", SHADER)

    def test_shadow_column_readability_is_view_neutral_and_evidence_gated(self):
        self.assertIn("const float directionalProbeSpacing = 10.0", SHADER)
        self.assertIn("sunShadowEvidenceOptical +=", SHADER)
        self.assertIn("blockerVisibilityLoss", SHADER)
        self.assertIn("peakSunLocalOcclusion", SHADER)
        self.assertIn("columnEvidenceGate", SHADER)
        self.assertNotIn("rayTopDownFactor", SHADER)

    def test_small_clear_air_local_volume_can_use_budgeted_half_res_roi(self):
        run = PASS_CPP[PASS_CPP.index("void War3VolumetricLightPass::Run"):]
        self.assertIn("kVolumetricLocalRoiResolutionDivisor = 2u", PASS_CPP)
        self.assertIn("!hasConfiguredGlobalMedium && hasLocalMedium", run)
        self.assertIn("requestedSamples", run)
        self.assertIn("rayBudgetFits && fogBudgetFits", run)
        self.assertIn("HalfResRegionRect(localRegion)", run)
        self.assertIn("uint64_t(effectScissor.extent.width)", PASS_CPP)

    def test_shapes_use_analytic_ray_support_before_fixed_step_integration(self):
        intersection = SHADER[
            SHADER.index("bool intersectFogVolume"):
            SHADER.index("float fogVolumeWeight")
        ]
        self.assertIn("discriminant", intersection)
        self.assertIn("clipFogSlab(origin.x", intersection)
        self.assertIn("dot(direction.xy, direction.xy)", intersection)
        interval_setup = SHADER.index("vec2 fogRayIntervals[8]")
        march_loop = SHADER.index("for (int i = 0; i < 16; i++)")
        self.assertLess(interval_setup, march_loop)
        march = SHADER[march_loop:]
        self.assertIn("max(segmentStart, fogRayIntervals[vi].x)", march)
        self.assertIn("min(segmentEnd, fogRayIntervals[vi].y)", march)
        self.assertIn("fogSegmentSigmaT", march)

    def test_watchdogs_count_volume_and_point_interactions(self):
        self.assertIn("kVolumetricRaySegmentBudget = 4'000'000ull", PASS_CPP)
        self.assertIn(
            "kVolumetricFogSegmentTestBudget = 96'000'000ull", PASS_CPP
        )
        self.assertIn("uint64_t(fogVolumes.count)", PASS_CPP)
        self.assertIn("(1u + uint64_t(pointSelection.count))", PASS_CPP)
        self.assertIn("minimumFogSegmentTests(effectExtent)", PASS_CPP)

    def test_map_reset_and_public_managed_lifetime_are_closed(self):
        reset_start = DEVICE.index("D3D9DeviceEx::War3ResetCpuSemanticMapSession")
        reset = DEVICE[reset_start: reset_start + 8000]
        self.assertIn("War3FogVolumeManager::Instance().Clear()", reset)
        self.assertIn("ManagedType::LocalFog", JAPI)
        self.assertIn("RegisterObject(ManagedType::LocalFog", JAPI)
        self.assertIn("war3shader::RemoveFogVolume", JAPI)
        for function in (
            "WarVKCreateSphereFogVolume",
            "WarVKCreateBoxFogVolume",
            "WarVKCreateCylinderFogVolume",
            "WarVKDestroyFogVolume",
            "WarVKIsFogVolumeAlive",
        ):
            self.assertIn(f"function {function}", JASS)

    def test_clear_air_defaults_are_low_extinction_without_height_fog(self):
        defaults = re.search(
            r"struct War3VolumetricLightSettings \{(.*?)\n\};",
            SETTINGS,
            re.DOTALL,
        ).group(1)
        self.assertIn("float intensity = 1.20f", defaults)
        self.assertIn("float density = 0.22f", defaults)
        self.assertIn("float weight = 2.10f", defaults)
        self.assertIn("bool globalMediumEnabled = true", defaults)
        self.assertIn("bool heightFogEnabled = false", defaults)
        self.assertIn("float extinctionStrength = 0.05f", defaults)
        self.assertIn("float unshadowedScattering = 0.03f", defaults)
        self.assertIn("mix(0.03, 1.0, distanceFade)", SHADER)

    def test_primary_research_and_scope_limit_are_recorded(self):
        for source in (
            "wronski",
            "ea.com/news/physically-based-unified-volumetric-rendering",
            "developer.nvidia.com",
            "registry.khronos.org/vulkan/specs",
        ):
            self.assertIn(source, RESEARCH.lower())
        self.assertIn("不会\n遮挡另一个雾体积", RESEARCH)
        self.assertIn("玩家前台 A/B", RESEARCH)


if __name__ == "__main__":
    unittest.main()
