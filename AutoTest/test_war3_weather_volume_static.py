"""Source-contract checks for the independent weather volume integration.

These guard wiring counterexamples (mixed owner/UBO/history, light gates,
legacy no-op). GPU execution, visual quality and timings are separate gates.
"""
import math
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def float32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def smoothstep(edge0, edge1, value):
    x = min(1.0, max(0.0, (value - edge0) / (edge1 - edge0)))
    return x * x * (3.0 - 2.0 * x)


class DensityMath:
    """Evaluate the production shader's extracted constants in counterexamples.

    Formula wiring is checked below; coefficients are read from GLSL rather
    than duplicated as Python expectations. Float32 arithmetic covers modulo
    wrap precision, but is not shader/GPU execution or visual acceptance.
    """

    def __init__(self, source):
        self.period = float(re.search(r"WEATHER_WIND_PERIOD = ([0-9.]+)", source).group(1))
        self.two_pi = float(re.search(r"WEATHER_TWO_PI = ([0-9.]+)", source).group(1))
        self.waves = [tuple(map(float, pair)) for pair in re.findall(
            r"sin\(dot\(q, vec2\((-?[0-9.]+), (-?[0-9.]+)\)\)", source)]
        self.low_height = float(re.search(r"float low = exp\(-max\(p.z - regionMin.z, 0.0\) / ([0-9.]+)\)", source).group(1))
        self.haze_sigma = float(re.search(r"controls.x, 0.0, 1.0\) \* ([0-9.]+)", source).group(1))
        self.low_sigma = float(re.search(r"controls.y, 0.0, 1.0\) \* low \* ([0-9.]+)", source).group(1))

    def band(self, position, wind, time, fp32=False):
        cast = float32 if fp32 else float
        scale = cast(cast(self.two_pi) / cast(self.period))
        q = [cast(cast(cast(p) - cast(w)) * scale) for p, w in zip(position, wind)]
        phases = [cast(cast(q[0] * wave[0]) + cast(q[1] * wave[1])) for wave in self.waves]
        phases[1] = cast(phases[1] + cast(cast(time) * cast(0.4)))
        return 0.65 + 0.20 * math.sin(phases[0]) + 0.15 * math.sin(phases[1])

    def sigma(self, position, region_min, region_max, feather, haze, low_snow):
        if any(not math.isfinite(x) for x in (*position, *region_min, *region_max)):
            return 0.0
        if any(hi <= lo for lo, hi in zip(region_min, region_max)):
            return 0.0
        if any(p < lo or p > hi for p, lo, hi in zip(position, region_min, region_max)):
            return 0.0
        edges = [min(position[i] - region_min[i], region_max[i] - position[i]) for i in range(2)]
        edges.append(region_max[2] - position[2])
        mask = 1.0
        if feather > 0.0:
            for edge, lo, hi in zip(edges, region_min, region_max):
                mask *= smoothstep(0.0, max(max(hi - lo, 1.0) * feather, 1.0), edge)
        low = math.exp(-max(position[2] - region_min[2], 0.0) / self.low_height)
        return mask * (haze * self.haze_sigma + low_snow * low * self.low_sigma)


class WeatherVolumeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = read("src/d3d9/d3d9_war3_volumetric_light.cpp")
        cls.header = read("src/d3d9/d3d9_war3_volumetric_light.h")
        cls.inject = read("subprojects/war3fx/shaders/war3_volumetric_froxel_inject.comp")
        cls.integrate = read("subprojects/war3fx/shaders/war3_volumetric_froxel_integrate.comp")
        cls.temporal = read("subprojects/war3fx/shaders/war3_volumetric_froxel_temporal.comp")
        cls.legacy = read("subprojects/war3fx/shaders/war3_volumetric_light.frag")
        cls.density = read("subprojects/war3fx/shaders/war3_weather_density.glsl")
        cls.density_math = DensityMath(cls.density)
        cls.run_body = cls.cpp.split("void War3VolumetricLightPass::Run(", 1)[1].split("uint64_t War3VolumetricLightPass::resourceBytes", 1)[0]
        cls.medium = cls.cpp.split("void War3VolumetricLightPass::RunMedium(", 1)[1]

    def test_froxel_lookup_uses_the_same_viewport_ray_as_depth(self):
        self.assertIn("out float distanceLimit, out vec2 volumeUv)", self.integrate)
        self.assertIn("volumeUv = uvLocal;", self.integrate)
        self.assertIn("reconstructRayAndLimit(uv, rayDir, distanceLimit, volumeUv)", self.integrate)
        self.assertIn("vec3(volumeUv, (float(z) + 0.5)", self.integrate)
        self.assertNotIn("vec3(uv, (float(z) + 0.5)", self.integrate)
        # Both producers are projection/viewport local. Do not "fix" them to
        # full-target coordinates while preserving a local consumer contract.
        for shader in (self.inject, self.temporal):
            self.assertIn("1.0 - uv.y * 2.0", shader)
            self.assertNotIn("p_viewport", shader)

    @staticmethod
    def viewport_uv(output_uv, depth_size, viewport):
        # Independent numeric model of the production pixel transform, not a
        # shader execution/GPU certificate. Wiring to GLSL is checked above.
        minimum = viewport[:2]
        extent = viewport[2:]
        full = [u * size - 0.5 for u, size in zip(output_uv, depth_size)]
        if any(p < lo or p >= lo + width for p, lo, width in zip(full, minimum, extent)):
            return None
        pixel = [min(max(math.floor(p + 0.5), lo), lo + width - 1)
                 for p, lo, width in zip(full, minimum, extent)]
        pixel = [min(max(p, 0), size - 1) for p, size in zip(pixel, depth_size)]
        return tuple((p - lo) / width for p, lo, width in zip(pixel, minimum, extent))

    def test_inset_viewport_center_does_not_read_another_column(self):
        depth = (2560, 1440)
        viewport = (160, 80, 2240, 1040)
        full_uv = ((1280 + 0.5) / depth[0], (600 + 0.5) / depth[1])
        local_uv = self.viewport_uv(full_uv, depth, viewport)
        self.assertEqual(local_uv, (0.5, 0.5))
        self.assertGreater(abs(full_uv[1] - local_uv[1]), 0.08)
        # A sloped thin layer belongs to the local camera column. The old
        # lookup demonstrably accesses a different density for this sample.
        density = lambda uv: math.exp(-abs(uv[1] - 0.5) * 80.0)
        self.assertEqual(density(local_uv), 1.0)
        self.assertLess(density(full_uv), 0.01)

    def test_full_viewport_preserves_the_existing_d3d_half_pixel_contract(self):
        depth = (2560, 1440)
        uv = ((1000 + 0.5) / depth[0], (600 + 0.5) / depth[1])
        self.assertEqual(self.viewport_uv(uv, depth, (0, 0, *depth)),
                         (1000 / depth[0], 600 / depth[1]))

    def test_ui_and_outside_viewport_do_not_sample_weather_volume(self):
        depth = (2560, 1440)
        viewport = (0, 0, 2560, 1100)
        self.assertIsNone(self.viewport_uv((0.5, 0.9), depth, viewport))
        self.assertIsNone(self.viewport_uv((0.0, 0.0), depth, viewport))
        self.assertIsNotNone(self.viewport_uv((0.5, 0.5), depth, viewport))

    def test_original_settings_and_scene_are_not_rewritten(self):
        self.assertIn("RunMedium(ctx, input, input.settings->postFx.volumetricLight);", self.run_body)
        self.assertIn("War3VolumetricLightSettings weatherSettings = {};", self.run_body)
        for bad in ("auto settings = input.settings->postFx.volumetricLight", "settings.intensity = std::max", "War3PipelineInput weatherInput", "input.scene ="):
            self.assertNotIn(bad, self.cpp)
        self.assertIn("m_weatherOwner->RunMedium(ctx, input, weatherSettings);", self.run_body)

    def test_weather_has_a_lazy_independent_resource_owner(self):
        self.assertIn("std::unique_ptr<War3VolumetricLightPass> m_weatherOwner", self.header)
        self.assertIn("std::make_unique<War3VolumetricLightPass>(m_parent, true)", self.run_body)
        create = self.run_body.index("std::make_unique")
        self.assertLess(self.run_body.index("if (!weatherMedium && !m_weatherOwner)"), create)
        self.assertIn("if (!m_weatherOwner && weatherMedium && ctx", self.run_body)
        self.assertIn("m_weatherOnly(weatherOnly)", self.cpp)
        self.assertIn("std::array<Rc<DxvkBuffer>, kUboRingSlots> m_fogVolumeBuffers", self.header)
        self.assertIn("m_lastLoggedBackend", self.header)
        self.assertNotIn("static int32_t s_lastLoggedBackend", self.cpp)

    def test_weather_medium_does_not_consume_authored_fog_or_light_policy(self):
        for item in (
            "weatherSettings.quality = War3VolumetricQuality::FroxelMedium;",
            "weatherSettings.globalMediumEnabled = false;",
            "weatherSettings.density = 0.0f;",
            "weatherSettings.heightFogEnabled = false;",
            "weatherSettings.extinctionStrength = 1.0f;",
            "weatherSettings.includePointLights = false;",
            "weatherSettings.requireCsmSnapshot = false;",
            "weatherSettings.volumeSunShadowEnabled = false;",
            "weatherSettings.froxelTemporalEnabled = false;",
        ):
            self.assertIn(item, self.run_body)
        self.assertIn("const bool authoredMediumEnabled = !m_weatherOnly && settings.enabled;", self.medium)
        self.assertIn("authoredMediumEnabled && War3FogVolumeManager", self.medium)
        self.assertIn("!m_weatherOnly && settings.includePointLights", self.medium)
        self.assertIn("if (!hasSunVolume && !pointFeatureRequested && !weatherMedium)", self.medium)
        self.assertIn("if (!hasSunVolume && !hasPointVolume && !weatherMedium)", self.medium)

    def test_weather_ubo_is_zero_in_original_owner_and_matches_both_shaders(self):
        self.assertIn("if (m_weatherOnly && input.weather.enabled", self.cpp)
        self.assertIn("input.weather.frameSerial == input.frameSerial", self.cpp)
        self.assertIn("input.weather.mapEpoch == input.mapEpoch", self.cpp)
        self.assertIn("input.weather.deviceEpoch == input.deviceEpoch", self.cpp)
        for shader in (self.inject, self.legacy):
            fields = re.search(r"uniform FogVolumeBlock \{(.*?)\} fogVolumes;", shader, re.S).group(1)
            self.assertEqual(re.findall(r"vec4 (u_weather\w+);", fields), ["u_weatherMin", "u_weatherMax", "u_weatherControls", "u_weatherMotion"])
        self.assertIn("sizeof(VolumetricFogVolumeUniform) == 592u", self.cpp)

    def test_seek_disable_and_duplicate_frame_cannot_reuse_unsafe_history_or_ubo(self):
        self.assertIn("input.weather.historyRevision ==", self.cpp)
        self.assertIn("m_froxelHistoryWeatherRevision", self.header)
        self.assertIn("if (m_weatherOnly)\n      invalidateFroxelHistory();", self.medium)
        duplicate = self.medium.index('execution.stage = "duplicate-weather-frame";')
        self.assertLess(duplicate, self.medium.index("copyColor(ctx"))
        self.assertIn("m_weatherRecordedMapEpoch == input.mapEpoch", self.medium)
        self.assertIn("m_weatherRecordedDeviceEpoch == input.deviceEpoch", self.medium)

    def test_weather_fallback_is_a_real_bounded_integral(self):
        branch = self.legacy.split("if (hasWeatherMedium) {", 1)[1].split("float maxDistScale", 1)[0]
        self.assertIn("war3WeatherRayInterval", branch)
        self.assertIn("sampleIndex < 16", branch)
        self.assertIn("war3WeatherSigma", branch)
        self.assertIn("weatherOpticalDepth +=", branch)
        self.assertIn("weatherTransmittance = exp", branch)
        self.assertIn("vec3(0.30, 0.34, 0.40) * (1.0 - weatherTransmittance)", branch)
        self.assertNotIn("hasSunVolume", branch)
        self.assertIn("!hasSunVolume && !hasPointVolume && !hasWeatherMedium", self.legacy)

    def test_weather_account_and_diagnostics_do_not_overwrite_original(self):
        self.assertIn("QueryWar3WeatherVolumeDiagnostics", self.cpp)
        self.assertIn("m_weatherOnly ? g_weatherVolumeDiagnostics", self.cpp)
        self.assertIn('cpuScope("WeatherVolume")', self.run_body)
        self.assertIn("if (!m_weatherOnly)\n    PublishVolumetricShaderWorkDiagnostics", self.medium)
        self.assertIn("execution.resourceBytes = resourceBytes();", self.medium)
        self.assertIn("image->getMemoryInfo().size", self.cpp)
        gate = self.medium.index("nominalBytes > kWeatherVolumeResourceBudget")
        self.assertLess(gate, self.medium.index("ensureResources(extent"))
        self.assertIn("extent.width > 8192u || extent.height > 8192u", self.medium)
        self.assertIn("if (froxelAdmitted && !m_weatherOnly)", self.medium)

    def test_density_and_ray_parallel_axes_fail_closed(self):
        self.assertIn("abs(direction[axis]) < 1.0e-7", self.density)
        self.assertIn("any(isnan(regionMin))", self.density)
        self.assertIn("any(isinf(motion))", self.density)
        self.assertIn("any(lessThanEqual(regionMax.xyz, regionMin.xyz))", self.density)
        self.assertIn("interval.y <= interval.x", self.density)
        self.assertIn("source += weatherSigma * vec3(0.30, 0.34, 0.40)", self.inject)

    def test_low_snow_survives_bottom_feather_and_fades_only_at_top(self):
        self.assertIn("vec3 edge = vec3(min(p.xy - regionMin.xy, regionMax.xy - p.xy),", self.density)
        self.assertIn("regionMax.z - p.z);", self.density)
        self.assertNotIn("vec3 edge = min(p - regionMin.xyz", self.density)
        model = self.density_math
        lo, hi = (-1000.0, -1000.0, 0.0), (1000.0, 1000.0, 1024.0)
        ground = model.sigma((0.0, 0.0, 0.0), lo, hi, 0.5, 0.0, 1.0)
        self.assertAlmostEqual(ground, model.low_sigma)
        self.assertGreater(ground, 0.001)
        self.assertGreater(model.sigma((0.0, 0.0, 24.0), lo, hi, 0.5, 0.0, 1.0), ground * 0.75)
        self.assertEqual(model.sigma((0.0, 0.0, 1024.0), lo, hi, 0.5, 1.0, 1.0), 0.0)
        self.assertEqual(model.sigma((0.0, 0.0, -0.01), lo, hi, 0.5, 1.0, 1.0), 0.0)
        self.assertEqual(model.sigma((1000.0, 0.0, 1.0), lo, hi, 0.5, 1.0, 1.0), 0.0)
        # The former symmetric feather would suppress all density at Z=0.
        self.assertEqual(ground * smoothstep(0.0, 512.0, 0.0), 0.0)

    def test_density_wind_is_periodic_in_either_axis_and_signed_wrap(self):
        self.assertIn("vec2 q = (p.xy - motion.xy) * (WEATHER_TWO_PI / WEATHER_WIND_PERIOD);", self.density)
        self.assertIn("float(std::fmod(w.windDisplacementX, 786432.0))", self.cpp)
        self.assertIn("float(std::fmod(w.windDisplacementY, 786432.0))", self.cpp)
        model = self.density_math
        self.assertEqual(len(model.waves), 2)
        self.assertEqual(model.period, 8192.0 * 96.0)
        self.assertTrue(all(x.is_integer() for pair in model.waves for x in pair))
        for p in ((0.0, 0.0), (256.0, -1024.0), (-50000.0, 80000.0)):
            for t in (0.0, 25.0, 86400.0):
                baseline = model.band(p, (123.25, -987.5), t)
                for axis in (0, 1):
                    for turns in (-2.0, -1.0, 1.0, 2.0):
                        wind = [123.25, -987.5]
                        wind[axis] += turns * model.period
                        self.assertAlmostEqual(model.band(p, wind, t), baseline, places=10)
        # Examine the exact CPU fmod boundary, including negative travel.
        # Physical wind moved just 0.5 unit, not an entire world pattern.
        p, t = (256.0, -1024.0), 25.0
        for axis in (0, 1):
            for sign in (-1.0, 1.0):
                winds = []
                for offset in (-0.25, 0.25):
                    value = sign * (model.period + offset)
                    wind = [0.0, 0.0]
                    wind[axis] = math.fmod(value, model.period)
                    winds.append(wind)
                self.assertLess(abs(model.band(p, winds[0], t, True) - model.band(p, winds[1], t, True)), 0.003)
        old = lambda wind: 0.65 + 0.20 * math.sin((p[0] - wind) * 0.003 + p[1] * 0.003 * 0.7) + 0.15 * math.sin(p[1] * 0.003 * 1.9 - (p[0] - wind) * 0.003 * 0.4 + t * 0.4)
        self.assertGreater(abs(old(model.period - 0.25) - old(0.25)), 0.05)

    def test_weather_resource_failures_are_not_shared_pass_exceptions(self):
        ordinary = self.run_body.index("RunMedium(ctx, input, input.settings->postFx.volumetricLight);")
        weather_try = self.run_body.index("try {")
        self.assertLess(ordinary, weather_try)
        self.assertIn("m_weatherResourceRetry.ready(resourceKey, input.frameSerial)", self.run_body)
        self.assertIn("m_weatherResourceRetry.failed(resourceKey, input.frameSerial)", self.run_body)
        self.assertIn('noteCreationFailure("weather-volume-owner-retry-wait")', self.run_body)
        self.assertIn("catch (const DxvkError&)", self.run_body)
        self.assertIn("catch (const std::bad_alloc&)", self.run_body)
        self.assertNotIn("catch (...)", self.run_body)
        self.assertEqual(self.run_body.count("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"), 2)
        before_copy, after_copy = self.medium.split("copyColor(ctx, input.colorView);", 1)
        for stage in ("weather-volume-resource-retry-wait", "weather-volume-resource-allocation-failed", "weather-volume-resource-host-allocation-failed"):
            self.assertIn(stage, before_copy)
        self.assertEqual(before_copy.count("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"), 2)
        self.assertNotIn("catch (", after_copy)
        self.assertNotIn("entry.enabled", self.cpp)

    def test_weather_preparation_cannot_allocate_late_pipeline_or_fallback_views(self):
        before_copy = self.medium.split("copyColor(ctx, input.colorView);", 1)[0]
        for call in ("ensureFroxelResources(extent, settings.quality)", "preparePointShadowFallbackResources();", "getPipeline(effectKey)", "getCompositePipeline(compositeKey)", "prepareWeatherDescriptors(input);"):
            self.assertIn(call, before_copy)
        prepare_views = self.cpp.split("void War3VolumetricLightPass::prepareWeatherDescriptors(", 1)[1].split("void War3VolumetricLightPass::copyColor", 1)[0]
        self.assertIn("prepare(m_pointShadowFallbackCubeView);", prepare_views)
        self.assertIn("view->getDescriptor();", prepare_views)
        self.assertNotIn("ctx->", prepare_views)
        self.assertIn("m_weatherOnly ? m_weatherFroxelPrepared", self.cpp)
        self.assertIn("const auto prepared = m_pipelines.find(key);", self.cpp)
        self.assertIn("const auto prepared = m_compositePipelines.find(key);", self.cpp)
        self.assertIn("if (!m_weatherOnly)\n    preparePointShadowFallbackResources();", self.cpp)

    def test_partial_constructor_and_froxel_allocations_have_owned_cleanup(self):
        constructor = self.cpp.split("War3VolumetricLightPass::War3VolumetricLightPass(", 1)[1].split("War3VolumetricLightPass::~War3VolumetricLightPass", 1)[0]
        self.assertIn("try {", constructor)
        self.assertIn("} catch (...) {", constructor)
        self.assertIn("releasePipelines();\n    throw;", constructor)
        self.assertIn("void War3VolumetricLightPass::releasePipelines() noexcept", self.cpp)
        self.assertIn("m_froxelInjectPipeline = VK_NULL_HANDLE;", self.cpp)
        self.assertIn("m_pipelines.clear();", self.cpp)
        froxel = self.cpp.split("bool War3VolumetricLightPass::ensureFroxelResources(", 1)[1].split("void War3VolumetricLightPass::preparePointShadowFallbackResources", 1)[0]
        self.assertLess(froxel.index("candidateHistoryViews[i] ="), froxel.index("m_froxelCurrentImage = std::move"))
        self.assertIn("m_froxelHistoryImages = std::move(candidateHistory);", froxel)
        for field, offset in (("weatherMin", 528), ("weatherMax", 544), ("weatherControls", 560), ("weatherMotion", 576)):
            self.assertIn(f"offsetof(VolumetricFogVolumeUniform, {field}) == {offset}u", self.cpp)


if __name__ == "__main__":
    unittest.main()
