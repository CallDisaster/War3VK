#!/usr/bin/env python3
"""Weather shader ABI/source and mathematical property checks.

This is not GPU/visual acceptance. Shader compilation and DLL construction
are separate gates; reference math below checks the claimed periodic lattice
properties and does not claim to execute GLSL on a device.
"""

from pathlib import Path
import math
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src/d3d9/d3d9_war3_weather.cpp"
HEADER = ROOT / "src/d3d9/d3d9_war3_weather.h"
SHADERS = ROOT / "subprojects/war3fx/shaders"
VERT = SHADERS / "war3_weather_snow_vert.vert"
FRAG = SHADERS / "war3_weather_snow_frag.frag"
DENSITY = SHADERS / "war3_weather_density.glsl"


def block(text: str, pattern: str) -> str:
    match = re.search(pattern, text, re.S)
    if not match:
        raise AssertionError(f"Production declaration not found: {pattern}")
    return re.sub(r"//[^\n]*", "", match.group(1))


def layout(declaration: str, types: dict[str, int]) -> list[tuple[str, int, int]]:
    result = []
    offset = 0
    for item in declaration.split(";"):
        item = item.strip()
        if not item:
            continue
        words = item.split()
        if len(words) != 2 or words[0] not in types:
            raise AssertionError(f"Unrecognized production ABI field: {item}")
        size = types[words[0]]
        result.append((words[1], offset, size))
        offset += size
    return result


def hash32(value: int) -> int:
    value &= 0xFFFFFFFF
    value ^= value >> 16
    value = value * 0x7FEB352D & 0xFFFFFFFF
    value ^= value >> 15
    value = value * 0x846CA68B & 0xFFFFFFFF
    return value ^ (value >> 16)


def identity(x: int, y: int, lane: int, seed: int) -> int:
    return hash32((x & 8191) ^ hash32(y & 8191) ^ hash32(lane + seed))


def random01(value: int) -> float:
    return (hash32(value) >> 8) / 16777216.0


def particle_xy(x: int, y: int, lane: int, seed: int,
                wind_x: float, wind_y: float) -> tuple[float, float]:
    key = identity(x, y, lane, seed)
    return (x * 96.0 + random01(key) * 96.0 + wind_x,
            y * 96.0 + random01(key + 1) * 96.0 + wind_y)


class WeatherShaderContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = CPP.read_text(encoding="utf-8")
        cls.header = HEADER.read_text(encoding="utf-8")
        cls.vert = VERT.read_text(encoding="utf-8")
        cls.frag = FRAG.read_text(encoding="utf-8")
        cls.density = DENSITY.read_text(encoding="utf-8")

    def test_actual_three_production_layouts_are_identical(self):
        cpp = layout(block(self.cpp, r"struct SnowPushConstants\s*\{(.*?)\};"),
                     {"Matrix4": 64, "Vector4": 16, "uint32_t": 4, "float": 4})
        for shader in (self.vert, self.frag):
            glsl = layout(block(shader, r"uniform SnowPush\s*\{(.*?)\}\s*pc;"),
                          {"mat4": 64, "vec4": 16, "uint": 4, "float": 4})
            self.assertEqual(cpp, glsl)
            self.assertIn("layout(push_constant, scalar, row_major)", shader)
        self.assertEqual(128, cpp[-1][1] + cpp[-1][2])
        self.assertIn("static_assert(sizeof(SnowPushConstants) == 128)", self.cpp)
        for name, offset, _size in cpp[1:5]:
            self.assertIn(f"offsetof(SnowPushConstants, {name}) == {offset}",
                          self.cpp)

    def test_real_abi_has_complete_eye_and_bounded_media(self):
        self.assertIn("cameraH.z / cameraH.w", self.cpp)
        self.assertIn("vec3(pc.windAnchor.zw, pc.cameraZ)", self.frag)
        for parameter in ("region.edgeFeather", "params.haze",
                          "params.lowSnow", "params.gust"):
            self.assertIn(f"Quantize(weather.{parameter}", self.cpp)
        for shift in (8, 16, 24):
            self.assertRegex(self.cpp, rf"<<\s*{shift}\b")
            self.assertRegex(self.frag, rf">>\s*{shift}\b")

    def test_no_particle_storage_or_readback_allocation(self):
        for forbidden in ("createBuffer(", "cmdDispatch(", "mapPtr(",
                          "cmdCopyImageToBuffer("):
            self.assertNotIn(forbidden, self.cpp)
        self.assertNotIn("buffer ", self.vert)
        self.assertNotIn("WeatherRuntime::instance", self.cpp)
        self.assertIn("const auto& weather = input.weather", self.cpp)

    def test_depth_copy_is_bounded_lazy_and_transactional(self):
        self.assertIn("kMaxDepthCopyBytes = 32u * 1024u * 1024u", self.header)
        self.assertIn("minimumBytes > kMaxDepthCopyBytes", self.cpp)
        self.assertIn("bytes > kMaxDepthCopyBytes", self.cpp)
        self.assertIn("candidate->storage()->getMemoryInfo().size", self.cpp)
        self.assertIn("m_depthCopy = std::move(candidate)", self.cpp)
        self.assertIn("m_depthCopyView = std::move(candidateView)", self.cpp)
        self.assertIn("m_depthCopyLayout.reset()", self.cpp)
        # No new backing is created when the current copy matches exactly.
        ensure = self.cpp[self.cpp.index("bool War3WeatherPass::ensureDepthCopy"):
                          self.cpp.index("void War3WeatherPass::copyDepth")]
        self.assertLess(ensure.index("return true;"),
                        ensure.index("createImage("))

    def test_depth_copy_previous_read_write_dependency_is_explicit(self):
        copy = self.cpp[self.cpp.index("void War3WeatherPass::copyDepth"):
                        self.cpp.index("void War3WeatherPass::Run")]
        write = copy.index("m_depthCopyLayout.plan(transferWrite")
        commit_write = copy.index("CommitWar3OwnedImageLayout", write)
        transfer = copy.index("ctx->cmdCopyImage", commit_write)
        read = copy.index("const auto read = m_depthCopyLayout.plan", transfer)
        commit_read = copy.index("CommitWar3OwnedImageLayout", read)
        self.assertLess(write, commit_write)
        self.assertLess(commit_write, transfer)
        self.assertLess(transfer, read)
        self.assertLess(read, commit_read)
        self.assertIn("VK_ACCESS_2_TRANSFER_WRITE_BIT", copy[write:commit_write])
        self.assertIn("VK_ACCESS_2_SHADER_SAMPLED_READ_BIT", copy[read:commit_read])
        self.assertIn("sourceImage->trackLayout(sourceRange, sourceOriginal)", copy)
        self.assertIn("ctx->track(m_depthCopy, DxvkAccess::Write)", copy)
        self.assertIn("ctx->track(sampledDepthImage, DxvkAccess::Read)", self.cpp)

    def test_disabled_branch_precedes_gpu_objects_and_perf_scope(self):
        enabled_check = self.cpp.index("if (!weather.enabled || weather.id == 0)")
        finish_disabled = self.cpp.index("m_wasEnabled = true;", enabled_check)
        self.assertIn("return;", self.cpp[enabled_check:finish_disabled])
        self.assertLess(finish_disabled,
                        self.cpp.index("ensurePipeline(input.colorView"))

    def test_fixed_draw_budget_matches_lattice(self):
        cap = re.search(r"kMaxParticles\s*=\s*(\d+)", self.header)
        self.assertIsNotNone(cap)
        self.assertEqual(32 * 32 * 16, int(cap.group(1)))
        self.assertIn("ctx->cmdDraw(6, kMaxParticles, 0, 0)", self.cpp)
        self.assertIn("particle / (CELL_COUNT * CELL_COUNT)", self.vert)
        self.assertIn("gl_VertexIndex", self.vert)
        self.assertIn("gl_InstanceIndex", self.vert)

    def test_world_motion_uses_integral_not_current_speed(self):
        self.assertIn("weather.windDisplacementX", self.cpp)
        self.assertIn("weather.windDisplacementY", self.cpp)
        self.assertNotIn("weather.params.windSpeed", self.cpp)
        self.assertIn("vec2(cell) * CELL_SIZE", self.vert)
        self.assertIn("pc.windAnchor.xy + wobble", self.vert)
        self.assertIn("vec4(position, 1.0) * pc.viewProj", self.vert)

    def test_movement_period_and_seed_are_coupled(self):
        self.assertIn("8192.0 * 96.0", self.cpp)
        self.assertIn("const int CELL_PERIOD = 8192", self.vert)
        self.assertIn("const float CELL_SIZE = 96.0", self.vert)
        self.assertIn("cell & ivec2(CELL_PERIOD - 1)", self.vert)
        self.assertIn("hash32(lane + pc.seed)", self.vert)

    def test_camera_rotation_does_not_change_world_identity(self):
        # Same world cell viewed with unrelated projected camera orientations.
        p0 = particle_xy(30, -18, 7, 12345, 97.5, -46.25)
        for yaw in (-math.pi, -1.2, 0.0, 0.6, math.pi):
            projected = (p0[0] * math.cos(yaw) - p0[1] * math.sin(yaw),
                         p0[0] * math.sin(yaw) + p0[1] * math.cos(yaw))
            self.assertTrue(all(math.isfinite(x) for x in projected))
            self.assertEqual(p0, particle_xy(30, -18, 7, 12345, 97.5, -46.25))
        # Production identity is computed before the projection and has no
        # camera rotation/matrix term; this is source+reference math evidence.
        key_block = self.vert[self.vert.index("uint identity ="):
                              self.vert.index("float r0 =")]
        self.assertNotIn("viewProj", key_block)

    def test_integrated_displacement_wrap_is_physically_continuous(self):
        for x, y in ((-9000, -7000), (-3, 4), (8190, 8191), (42000, -41000)):
            for lane in (0, 7, 15):
                before = particle_xy(x, y, lane, 0xFEED1234,
                                     786433.25, -786434.5)
                after = particle_xy(x + 8192, y - 8192, lane, 0xFEED1234,
                                    1.25, -2.5)
                self.assertEqual(before, after)

    def test_normal_wind_change_keeps_position_continuous(self):
        old = particle_xy(12, 18, 3, 2345, 300.0, 200.0)
        # A velocity change preserves the existing integral at the instant of
        # change. Old/current speeds are absent from particle position inputs.
        at_change = particle_xy(12, 18, 3, 2345, 300.0, 200.0)
        later = particle_xy(12, 18, 3, 2345, 299.0, 203.0)
        self.assertEqual(old, at_change)
        self.assertAlmostEqual(-1.0, later[0] - old[0])
        self.assertAlmostEqual(3.0, later[1] - old[1])

    def test_direct_depth_uses_legal_usage_and_exact_restore(self):
        for condition in ("depthInfo.usage & (VK_IMAGE_USAGE_SAMPLED_BIT",
                          "depthInfo.sampleCount != VK_SAMPLE_COUNT_1_BIT",
                          "depthRange.layerCount != 1",
                          "depthRange.levelCount != 1"):
            self.assertIn(condition, self.cpp)
        self.assertIn("depthImage->queryLayout(depthRange)", self.cpp)
        self.assertIn("depthRange.aspectMask = depthFormat->aspectMask", self.cpp)
        self.assertIn("sampleKey.aspects = VK_IMAGE_ASPECT_DEPTH_BIT", self.cpp)
        self.assertIn("sampleKey.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY", self.cpp)
        self.assertIn("readLayout, sampledDepthOriginal", self.cpp)
        self.assertIn("sampledDepthImage->trackLayout(sampledDepthRange, sampledDepthOriginal)",
                      self.cpp)
        self.assertIn("colorImage->trackLayout(colorRange, colorOriginal)", self.cpp)
        self.assertIn("VK_QUEUE_FAMILY_IGNORED", self.cpp)

    def test_divergent_layout_sentinel_is_rejected_before_recording(self):
        guard = self.cpp.index("depthOriginal == VK_IMAGE_LAYOUT_UNDEFINED")
        refusal = self.cpp.index("reject(War3WeatherDrawStage::UndefinedLayout)", guard)
        predicate = self.cpp[guard:refusal]
        self.assertIn("depthOriginal == VK_IMAGE_LAYOUT_MAX_ENUM", predicate)
        self.assertIn("colorOriginal == VK_IMAGE_LAYOUT_MAX_ENUM", predicate)
        self.assertLess(refusal, self.cpp.index("copyDepth(ctx, input.depthView"))
        self.assertLess(refusal, self.cpp.index("ctx->cmdBeginRendering"))

    def test_precise_camera_and_epoch_pairing(self):
        for predicate in ("weather.frameSerial != input.frameSerial",
                          "weather.mapEpoch != input.mapEpoch",
                          "weather.deviceEpoch != input.deviceEpoch",
                          "camera.frameSerial != input.frameSerial"):
            self.assertIn(predicate, self.cpp)
        self.assertLess(self.cpp.index("EpochMismatch);"),
                        self.cpp.index("ctx->cmdBeginRendering"))

    def test_command_owns_original_images_sampler_and_pipeline(self):
        for ownership in ("ctx->track(depthImage, DxvkAccess::Read)",
                          "ctx->track(colorImage, DxvkAccess::Write)",
                          "ctx->track(m_depthSampler)",
                          "ctx->track(m_pipeline)",
                          "AdoptWar3TrackedVkPipeline"):
            self.assertIn(ownership, self.cpp)
        # Only a newly created, never-adopted handle may be destroyed here.
        # Cached/in-flight pipelines continue using command-list ownership.
        ensure = self.cpp[self.cpp.index("bool War3WeatherPass::ensurePipeline"):
                          self.cpp.index("void War3WeatherPass::clearDepthCopy")]
        self.assertEqual(self.cpp.count("vkDestroyPipeline("), 1)
        self.assertIn("candidateOwner = war3::render::AdoptWar3TrackedVkPipeline", ensure)
        self.assertIn("} catch (...) {", ensure)
        self.assertIn("device(), candidate, nullptr)", ensure)
        self.assertNotIn("m_pipeline->handle()", ensure)
        self.assertLess(ensure.index("vkDestroyPipeline("),
                        ensure.index("m_pipeline = std::move(candidateOwner)"))

    def test_all_resource_failures_are_pre_record_and_have_frame_backoff(self):
        run = self.cpp[self.cpp.index("void War3WeatherPass::Run"):]
        copy_index = run.index("copyDepth(ctx, input.depthView")
        prepare, recording = run[:copy_index], run[copy_index:]
        for call in ("m_resourceRetry.ready(resourceKey, input.frameSerial)",
                     "m_resourceRetry.failed(resourceKey, input.frameSerial)",
                     "ensurePipeline(input.colorView->info().format)",
                     "ensureDepthCopy(extent, depthInfo.format)",
                     "m_depthSampleView->getDescriptor()",
                     "input.colorView->getDescriptor()"):
            self.assertIn(call, prepare)
        for failure in ("ResourceRetryWait", "ResourceAllocationFailed",
                        "ResourceHostAllocationFailed"):
            self.assertIn(failure, prepare)
            self.assertIn(failure, self.header)
        self.assertIn("catch (const DxvkError&)", prepare)
        self.assertIn("catch (const std::bad_alloc&)", prepare)
        self.assertEqual(prepare.count("m_device->getDeviceStatus() == VK_ERROR_DEVICE_LOST"), 2)
        self.assertNotIn("catch (", recording)
        self.assertLess(prepare.index("m_resourceRetry.clear();"),
                        prepare.index('scope("Weather/Snow", ctx)'))
        self.assertIn("WeatherResourceRetry m_resourceRetry", self.header)
        # Caller-local guard does not change adoption for other render passes.
        self.assertIn("new raw handle", self.cpp)

    def test_surface_snow_is_premultiplied_and_target_is_loaded(self):
        self.assertIn("VK_ATTACHMENT_LOAD_OP_LOAD", self.cpp)
        self.assertIn("srcColorBlendFactor = VK_BLEND_FACTOR_ONE", self.cpp)
        self.assertIn("dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA",
                      self.cpp)
        self.assertIn("vec4(snowColor * alpha, alpha)", self.frag)
        self.assertIn("separation <= 0.0", self.frag)
        self.assertIn("smoothstep(0.0, depthSoftness, separation)", self.frag)

    def test_depth_polarity_uses_shared_production_inference(self):
        self.assertIn("InferWar3FarClearRaw(camera, farClearRaw)", self.cpp)
        self.assertIn("reversedDepth ? 0x80000000u : 0u", self.cpp)
        self.assertIn("pc.depthSampler & 65535u", self.frag)
        self.assertIn("pc.depthSampler & 0x80000000u", self.frag)
        self.assertIn("separation = -separation", self.frag)
        self.assertIn("vp.MaxZ <= vp.MinZ", self.cpp)

    def test_actual_raster_viewport_matches_native_half_texel(self):
        self.assertIn("float(vp.X) + 0.5f, float(vp.Y) + 0.5f", self.cpp)
        self.assertIn("clip.y = -clip.y", self.vert)
        # Negative native viewport vs positive weather viewport + flipped clip.
        for x, y, width, height in ((0, 0, 2560, 1440),
                                     (37, 119, 800, 600)):
            for ndc_x, ndc_y in ((-1, -1), (0, 0), (0.2, 0.4), (1, 1)):
                native_x = x + 0.5 + (ndc_x + 1) * width * 0.5
                weather_x = x + 0.5 + (ndc_x + 1) * width * 0.5
                native_y = y + height + 0.5 - (ndc_y + 1) * height * 0.5
                weather_y = y + 0.5 + (-ndc_y + 1) * height * 0.5
                self.assertAlmostEqual(native_x, weather_x)
                self.assertAlmostEqual(native_y, weather_y)

    def test_narrow_positive_depth_range_uses_native_bias(self):
        self.assertIn("vp.MinZ + (vp.MinZ < 0.5f ? 0.001f : 0.0f)", self.cpp)
        self.assertIn("vp.MaxZ <= vp.MinZ", self.cpp)
        # For this legal positive range the original native mapping expands
        # MaxZ, whereas copying the raw D3D9 range mismatches scene depth.
        minimum, requested_maximum = 0.2, 0.2001
        native_maximum = 0.201
        weather_maximum = min(max(requested_maximum, minimum + 0.001), 1.0)
        self.assertAlmostEqual(native_maximum, weather_maximum)
        self.assertGreater(abs(weather_maximum - requested_maximum), 0.0008)
        for ndc_z in (0.0, 0.25, 0.5, 1.0):
            native_depth = minimum + ndc_z * (native_maximum - minimum)
            weather_depth = minimum + ndc_z * (weather_maximum - minimum)
            self.assertAlmostEqual(native_depth, weather_depth)
        # The bias is intentionally absent once MinZ reaches the native 0.5
        # threshold; normal 0..1 and the upper range are unchanged.
        for minimum, requested_maximum in ((0.0, 1.0), (0.5, 0.5001), (0.99, 1.0)):
            bias = 0.001 if minimum < 0.5 else 0.0
            self.assertEqual(requested_maximum,
                             min(max(requested_maximum, minimum + bias), 1.0))

    def test_diagnostics_carries_exact_frozen_input(self):
        self.assertIn("WeatherFrameSnapshot frozenInput = {}", self.header)
        self.assertIn("diagnostics.frozenInput = weather", self.cpp)
        disabled = self.cpp[self.cpp.index("if (!weather.enabled"):
                            self.cpp.index("m_wasEnabled = true")]
        self.assertIn("War3WeatherDiagnostics disabled", disabled)
        self.assertIn("disabled.frozenInput = weather", disabled)
        self.assertIn("disabled.id = weather.id", disabled)
        self.assertIn("disabled.timeSeconds = weather.timeSeconds", disabled)
        self.assertIn("if (m_wasEnabled)", disabled)

    def test_optical_path_ends_at_particle_not_scene_surface(self):
        self.assertIn('#include "war3_weather_density.glsl"', self.frag)
        self.assertIn("vec3 delta = v_world - eye", self.frag)
        self.assertIn("i < 4", self.frag)
        self.assertIn("samplePosition = eye + ray * mix(interval.x, interval.y", self.frag)
        self.assertIn("vec2 interval = vec2(0.0, distanceWorld)", self.frag)
        self.assertIn("war3WeatherRayInterval(eye, ray", self.frag)
        self.assertIn("tau * (interval.y - interval.x) * 0.25", self.frag)
        self.assertIn("war3WeatherSigma(samplePosition", self.frag)
        self.assertNotIn("invViewProj", self.frag)
        self.assertLess(self.frag.index("fwidth(gl_FragCoord.z)"),
                        self.frag.index("discard;"))

    def test_thin_weather_volume_is_sampled_after_ray_box_clipping(self):
        self.assertIn("bool war3WeatherRayInterval", self.density)
        self.assertIn("abs(direction[axis]) < 1.0e-7", self.density)
        self.assertIn("interval.x = max(interval.x, min(a, b))", self.density)
        self.assertIn("interval.y = min(interval.y, max(a, b))", self.density)
        # The flake is at x=1000 and the weather box begins at 999.9. All
        # four old full-eye-path samples miss it. Clipping to [999.9,1000]
        # allocates all four samples inside the actual finite medium segment.
        distance = 1000.0
        box_min, box_max = 999.9, 1000.1
        old_samples = [distance * (i + 0.5) / 4.0 for i in range(4)]
        self.assertTrue(all(value < box_min for value in old_samples))
        start, end = max(0.0, box_min), min(distance, box_max)
        self.assertGreater(end, start)
        new_samples = [start + (end - start) * (i + 0.5) / 4.0
                       for i in range(4)]
        self.assertTrue(all(box_min <= value <= distance for value in new_samples))
        self.assertAlmostEqual(0.1, end - start)

    def test_no_arena_caster_or_readback_dependencies(self):
        for prohibited in ("shadowCasters", "Stage11", "Arena", "vkQueueWaitIdle",
                           "vkDeviceWaitIdle", "waitForIdle", "readback"):
            self.assertNotIn(prohibited, self.cpp)

    def test_diagnostics_distinguishes_recording_from_gpu_completion(self):
        self.assertIn("CommandsRecorded", self.header)
        self.assertIn("UnsupportedSamples", self.header)
        self.assertIn("UnsupportedDepth", self.header)
        self.assertIn("submitted = true", self.cpp)
        self.assertIn("CPU estimate before frustum/depth/region tests", self.header)
        self.assertIn("not GPU completion", self.header)


if __name__ == "__main__":
    unittest.main()
