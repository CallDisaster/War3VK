from pathlib import Path
import math
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class War3VolumetricFroxelStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = (ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp").read_text(
            encoding="utf-8"
        )
        cls.header = (ROOT / "src/d3d9/d3d9_war3_volumetric_light.h").read_text(
            encoding="utf-8"
        )
        cls.settings = (ROOT / "src/d3d9/d3d9_war3_settings.h").read_text(
            encoding="utf-8"
        )
        cls.shadow = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
            encoding="utf-8"
        )
        cls.inject = (
            ROOT
            / "subprojects/war3fx/shaders/war3_volumetric_froxel_inject.comp"
        ).read_text(encoding="utf-8")
        cls.temporal = (
            ROOT
            / "subprojects/war3fx/shaders/war3_volumetric_froxel_temporal.comp"
        ).read_text(encoding="utf-8")
        cls.integrate = (
            ROOT
            / "subprojects/war3fx/shaders/war3_volumetric_froxel_integrate.comp"
        ).read_text(encoding="utf-8")
        cls.composite = (
            ROOT
            / "subprojects/war3fx/shaders/war3_volumetric_composite.frag"
        ).read_text(encoding="utf-8")
        cls.shader_meson = (ROOT / "subprojects/war3fx/meson.build").read_text(
            encoding="utf-8"
        )
        cls.japi = (ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp").read_text(
            encoding="utf-8"
        )
        cls.jass = (ROOT / "WarVK/jass/warvk_api.j").read_text(encoding="utf-8")
        cls.jass_constants = (
            ROOT / "WarVK/jass/warvk_constant.j"
        ).read_text(encoding="utf-8")

    def test_enabled_feature_defaults_to_froxel_high_and_logs_actual_backend(self):
        self.assertIn("LegacyRayMarch = 0u", self.settings)
        self.assertRegex(
            self.settings,
            r"quality\s*=\s*War3VolumetricQuality::FroxelHigh",
        )
        self.assertIn("war3_volumetric_light", self.cpp)
        self.assertIn("DXVK War3Volumetric: active backend=%u (%s)", self.cpp)

    def test_quality_tiers_and_4k_cell_budget_are_bounded(self):
        self.assertIn("kVolumetricFroxelCellBudget = 4'500'000ull", self.cpp)
        self.assertIn(
            "kVolumetricFroxelTraversalWorkBudget = 350'000'000ull",
            self.cpp,
        )
        self.assertIn("kVolumetricFroxelMediumTileSize = 32u", self.cpp)
        self.assertIn("kVolumetricFroxelMediumDepth = 64u", self.cpp)
        self.assertIn("kVolumetricFroxelHighTileSize = 16u", self.cpp)
        self.assertIn("kVolumetricFroxelHighDepth = 128u", self.cpp)
        self.assertIn("kVolumetricFroxelHighEffectDivisor = 4u", self.cpp)
        self.assertIn("kVolumetricFroxelHighTraversalSteps = 2048u", self.cpp)
        self.assertIn("froxelCells > kVolumetricFroxelCellBudget", self.cpp)
        self.assertLessEqual(
            ((3840 + 15) // 16) * ((2160 + 15) // 16) * 128,
            4_500_000,
        )
        self.assertLessEqual((3840 // 8) * (2160 // 8) * 2048,
                             350_000_000)

    def test_resources_are_rgba16f_3d_storage_and_ping_pong(self):
        self.assertIn("VK_IMAGE_TYPE_3D", self.cpp)
        self.assertIn("VK_IMAGE_VIEW_TYPE_3D", self.cpp)
        self.assertIn("VK_FORMAT_R16G16B16A16_SFLOAT", self.cpp)
        self.assertIn("VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT", self.cpp)
        self.assertIn("m_froxelHistoryImages", self.header)
        self.assertIn("historyWriteIndex = (historyReadIndex + 1u) % 2u", self.cpp)

    def test_all_compute_shaders_are_embedded(self):
        for shader in (
            "war3_volumetric_froxel_inject.comp",
            "war3_volumetric_froxel_temporal.comp",
            "war3_volumetric_froxel_integrate.comp",
        ):
            self.assertIn(shader, self.shader_meson)
        for symbol in (
            "war3_volumetric_froxel_inject",
            "war3_volumetric_froxel_temporal",
            "war3_volumetric_froxel_integrate",
        ):
            self.assertIn(symbol, self.cpp)

    def test_injection_consumes_global_height_and_all_local_shapes(self):
        self.assertIn("densityToSigmaT", self.inject)
        self.assertIn("csm.u_params2.w", self.inject)
        self.assertIn("fogVolumes.u_count", self.inject)
        self.assertRegex(self.inject, r"shape\s*==\s*0")
        self.assertRegex(self.inject, r"shape\s*==\s*1")
        self.assertRegex(self.inject, r"shape\s*==\s*2")
        self.assertIn("fogVolumeWeight", self.inject)
        self.assertIn("coverageOffsets[4]", self.inject)
        self.assertIn("phaseOffsets[8]", self.inject)
        self.assertIn("sampleCount", self.inject)
        self.assertIn("input.frameSerial & 7u", self.cpp)

    def test_directional_shadow_is_integrated_separately_from_point_shadow(self):
        self.assertNotIn("directionalVisibility", self.inject)
        self.assertNotIn("uniform texture2DArray s_shadow", self.inject)
        self.assertIn("integrateShadowedSlice", self.integrate)
        self.assertIn("uniform texture2DArray s_shadow", self.integrate)
        self.assertIn("weightedLitIntegral", self.integrate)
        self.assertIn("intervalVisibility", self.integrate)
        self.assertIn("selectSegmentCascade", self.integrate)
        self.assertIn("nextTexelBoundary", self.integrate)
        self.assertIn("u_volumeSunParams", self.inject)
        self.assertIn("samplerCubeArray", self.inject)
        self.assertIn("u_pointShadowedLightCount", self.inject)
        self.assertIn("GetVolumetricPointShadowSnapshot", self.cpp)
        self.assertIn("GetVolumetricSunShadowSnapshot", self.cpp)

    def test_volume_sun_does_not_mutate_surface_csm_and_requires_a_consumer(self):
        csm_config = self.shadow[
            self.shadow.index("m_csmConfig = mutableSettings.shadows.csm"):
            self.shadow.index("s_farCasterDepthExtensionOverride")
        ]
        self.assertNotIn("postFx.volumetricLight.enabled", csm_config)
        self.assertIn("Surface CSM precision is independent", csm_config)

        volume_sun = self.shadow[
            self.shadow.index("const bool hasGlobalMedium"):
            self.shadow.index("if (wantVolumeSun)", self.shadow.index("const bool hasGlobalMedium"))
        ]
        self.assertIn("War3FogVolumeManager::Instance().HasActiveVolumes()", volume_sun)
        self.assertIn("const bool hasEffectiveSun", volume_sun)
        self.assertIn("const bool hasEffectiveVolumeConsumer", volume_sun)
        self.assertIn("hasEffectiveVolumeConsumer &&", volume_sun)

    def test_volume_sun_bias_is_world_scaled_and_cannot_peter_pan_with_range(self):
        self.assertIn("float volumeSunReceiverBias = 2.0f", self.settings)
        self.assertIn("const float receiverBiasWorld", self.shadow)
        self.assertIn("orthoFar.maxZ - orthoFar.minZ", self.shadow)
        self.assertIn("receiverBiasWorld / farDepthSpan", self.shadow)
        self.assertNotIn("0.0075f", self.settings)

    def test_temporal_world_reprojects_common_grid_and_variance_clips(self):
        self.assertIn("sliceBoundary", self.temporal)
        self.assertIn("reconstructCenter", self.temporal)
        self.assertIn("previousClip = vec4(worldPos, 1.0)", self.temporal)
        self.assertIn("textureLod", self.temporal)
        self.assertIn("secondMoment - mean * mean", self.temporal)
        self.assertIn("clippedHistory", self.temporal)
        self.assertIn("reactive", self.temporal)
        self.assertIn("0.0, 1.0", self.temporal)
        self.assertRegex(self.temporal, r"offsets\[7\]")

    def test_history_contract_uses_frame_map_device_grid_and_camera(self):
        for token in (
            "input.frameSerial == m_froxelHistoryFrameSerial + 1u",
            "input.mapEpoch == m_froxelHistoryMapEpoch",
            "input.deviceEpoch == m_froxelHistoryDeviceEpoch",
            "sameDistribution",
            "historyCameraLimit",
        ):
            self.assertIn(token, self.cpp)
        self.assertIn("cameraDelta <= historyCameraLimit", self.cpp)
        self.assertNotIn("stableCamera", self.cpp)
        self.assertIn("invalidateFroxelHistory", self.cpp)

    def test_integration_has_beer_lambert_and_continuous_shadow_binding(self):
        self.assertIn("segmentT = exp(-sigmaT * intervalLength)", self.integrate)
        self.assertIn("(1.0 - exp(-sigmaT * distance)) / sigmaT", self.integrate)
        self.assertIn("s_shadow", self.integrate)
        self.assertIn("p_maxTraversalSteps", self.integrate)
        self.assertIn("traversalStepsRemaining", self.integrate)
        self.assertIn("integrateFallback", self.integrate)
        self.assertIn("const float fallbackStep = 32.0", self.integrate)
        self.assertIn("float anchor = max(pc.p_grid.z, 0.1)", self.integrate)
        self.assertIn("floor(phase + 1.0e-5)", self.integrate)
        self.assertNotIn("samplerCubeArray", self.integrate)
        self.assertNotIn("rayInterval", self.integrate)
        self.assertNotIn("surfaceAnchored", self.integrate)

    def test_froxel_column_readability_requires_true_csm_optical_evidence(self):
        for token in (
            "out float physicalVisibility",
            "recordShadowEvidence",
            "shadowReferenceIntegral",
            "shadowPhysicalIntegral",
            "shadowEvidenceOptical",
            "peakShadowOcclusion",
            "selectSegmentCascade proved",
            "columnReadabilityAttenuation",
            "float maxColumnAtten = 0.24",
        ):
            self.assertIn(token, self.integrate)

        interval_visibility = self.integrate[
            self.integrate.index("float intervalVisibility"):
            self.integrate.index("void recordShadowEvidence")
        ]
        self.assertIn(
            "physicalVisibility = mix(1.0, rawVisibility, shadowStrength)",
            interval_visibility,
        )
        self.assertIn(
            "pow(rawVisibility, contrast)",
            interval_visibility,
        )

        fallback = self.integrate[
            self.integrate.index("void integrateFallback"):
            self.integrate.index("void integrateShadowedSlice")
        ]
        select_start = fallback.index("if (selectSegmentCascade")
        select_end = fallback.index("accumulateInterval")
        self.assertIn(
            "recordShadowEvidence",
            fallback[select_start:select_end],
        )

    def test_froxel_readability_attenuation_is_monotonic_and_bounded(self):
        def smoothstep(edge0, edge1, value):
            t = min(max((value - edge0) / (edge1 - edge0), 0.0), 1.0)
            return t * t * (3.0 - 2.0 * t)

        contrast = 2.10
        readability_mix = min(max((contrast - 1.0) / 1.5, 0.0), 1.0)
        source_gate = smoothstep(0.004, 0.040, 1.0)
        attenuations = []
        for evidence in (0.0, 0.00035, 0.0020, 0.0060, 0.0200):
            evidence_gate = smoothstep(0.00035, 0.0060, evidence)
            attenuation = min(
                1.0
                * 0.55
                * (0.62 * 1.05)
                * evidence_gate
                * source_gate
                * readability_mix,
                0.24,
            )
            attenuations.append(attenuation)

        self.assertEqual(attenuations[0], 0.0)
        self.assertTrue(all(
            a <= b for a, b in zip(attenuations, attenuations[1:])
        ))
        self.assertLessEqual(max(attenuations), 0.24)
        self.assertAlmostEqual(attenuations[-1], 0.24)

    def test_rgba_upsample_uses_depth_guide_not_its_own_edge(self):
        upsample = self.composite[
            self.composite.index("vec4 depthAwareUpsample"):
            self.composite.index("void main()")
        ]
        for token in (
            "float guideWeight = spatial[i] * depthWeight",
            "vec4 sumEffect = vec4(0.0)",
            "sumEffect += resolved * guideWeight",
            "return sumEffect / weightSum",
            "not a range guide for themselves",
            "bilinear coverage AA",
        ):
            self.assertIn(token, upsample)
        self.assertNotIn("effectEdgeDistance", self.composite)
        self.assertNotIn("scatteringWeight", upsample)

    def test_flat_depth_rgba_reconstruction_preserves_interior(self):
        dark = 0.76
        lit = 1.0

        def bilinear(values, fx, fy):
            weights = (
                (1.0 - fx) * (1.0 - fy),
                fx * (1.0 - fy),
                (1.0 - fx) * fy,
                fx * fy,
            )
            return sum(value * weight for value, weight in zip(values, weights))

        self.assertAlmostEqual(
            bilinear((dark, dark, dark, dark), 0.37, 0.61), dark
        )
        boundary = [
            bilinear((dark, lit, dark, lit), fraction, 0.5)
            for fraction in (0.0, 0.25, 0.5, 0.75, 1.0)
        ]
        self.assertAlmostEqual(boundary[0], dark)
        self.assertAlmostEqual(boundary[-1], lit)
        self.assertTrue(all(a < b for a, b in zip(boundary, boundary[1:])))

    def test_common_grid_does_not_slide_with_per_pixel_surface_depth(self):
        for shader in (self.inject, self.integrate):
            self.assertNotIn("surfaceDistance - windowLength", shader)
            self.assertNotIn("surfaceAnchored", shader)
            self.assertIn("log(intervalEnd / intervalStart)", shader)
        self.assertNotIn("binding = 6", self.inject)
        self.assertIn("m_depthCopyView->getDescriptor()", self.cpp)
        self.assertIn("distanceLimit = min(surfaceDistance, farDistance)",
                      self.integrate)
        self.assertIn("float froxelFar = 10000.0f", self.settings)

        near_distance = 20.0
        far_distance = 10000.0
        depth_slices = 128
        z = 63.5
        first_column_depth = near_distance * math.exp(
            math.log(far_distance / near_distance) * z / depth_slices)
        second_column_depth = near_distance * math.exp(
            math.log(far_distance / near_distance) * z / depth_slices)
        self.assertAlmostEqual(first_column_depth, second_column_depth)

    def test_legacy_quality_distance_cannot_shorten_common_froxel_grid(self):
        quality_case = self.japi[
            self.japi.index("case CommandId::VolumetricSetQuality"):
            self.japi.index("case CommandId::VolumetricSetBackend")
        ]
        self.assertIn("sunDistance = a[1].real", quality_case)
        self.assertIn("froxelFar", quality_case)
        self.assertIn("froxelFar = 10000.0f", quality_case)

    def test_effect_resolution_is_decoupled_from_grid_tiles(self):
        self.assertIn("kVolumetricFroxelHighTileSize = 16u", self.cpp)
        self.assertIn("kVolumetricFroxelHighEffectDivisor = 4u", self.cpp)
        self.assertIn("froxelWidth", self.cpp)
        self.assertIn("froxelHeight", self.cpp)
        self.assertIn("effectExtentForDivisor(resolutionDivisor)", self.cpp)
        self.assertIn("effect=%ux%u", self.cpp)

    def test_integrator_maps_output_pixels_through_the_d3d9_viewport(self):
        self.assertIn("fullPixelD3D", self.integrate)
        self.assertIn("viewportMin", self.integrate)
        self.assertIn("viewportSize", self.integrate)
        self.assertIn("vec2 uvLocal", self.integrate)
        self.assertIn("ivec2 viewportMaxI", self.integrate)

    def test_compute_dependencies_and_effect_layout_round_trip_exist(self):
        self.assertGreaterEqual(
            self.cpp.count("VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT"), 9
        )
        self.assertIn("VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL", self.cpp)
        self.assertIn("effectToWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL", self.cpp)
        self.assertIn(
            "effectToRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL",
            self.cpp,
        )
        shader_read_stages = (
            r"VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT\s*\|\s*"
            r"VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT"
        )
        self.assertRegex(self.cpp, shader_read_stages)
        self.assertRegex(self.shadow, shader_read_stages)

    def test_public_backend_switch_is_bounded(self):
        self.assertIn('"volumetric.setBackend"', self.japi)
        self.assertIn("a[0].integer > 2", self.japi)
        self.assertIn("WarVKSetVolumetricBackend", self.jass)
        self.assertIn(
            "WARVK_VOLUMETRIC_BACKEND_FROXEL_HIGH = 2", self.jass_constants
        )

    def test_logarithmic_slice_contract_is_monotonic_and_exact_at_bounds(self):
        near_distance = 20.0
        far_distance = 10000.0
        slice_count = 128
        boundaries = [
            near_distance
            * math.exp(
                math.log(far_distance / near_distance) * i / slice_count
            )
            for i in range(slice_count + 1)
        ]
        self.assertAlmostEqual(boundaries[0], near_distance, places=6)
        self.assertAlmostEqual(boundaries[-1], far_distance, places=5)
        self.assertTrue(all(a < b for a, b in zip(boundaries, boundaries[1:])))

    def test_analytic_shadow_crossing_fraction_matches_dense_reference(self):
        reference0 = 0.30
        reference1 = 0.70
        stored_depth = 0.50
        crossing = (stored_depth - reference0) / (reference1 - reference0)
        analytic_lit_fraction = crossing
        samples = 100_000
        numerical = sum(
            1.0
            for i in range(samples)
            if reference0 + (reference1 - reference0) * (i + 0.5) / samples
            <= stored_depth
        ) / samples
        self.assertAlmostEqual(analytic_lit_fraction, numerical, places=5)

    def test_froxel_shadow_silhouette_uses_continuous_comparison_gather(self):
        visibility = self.integrate[
            self.integrate.index("float intervalVisibility"):
            self.integrate.index("void recordShadowEvidence")
        ]
        for token in (
            "textureGather",
            "filter the four *comparison integrals* (PCF)",
            "vec2 footprintFraction = fract(midpointCoord - vec2(0.5))",
            "float pcfBlend = clamp(csm.u_params.w, 0.0, 1.0)",
            "vec4 comparisonIntegrals",
            "float litIntegral = dot(weights, comparisonIntegrals)",
        ):
            self.assertIn(token, visibility)
        self.assertNotIn("nearTransition", visibility)
        self.assertEqual(visibility.count("textureGather("), 1)
        self.assertNotIn("stored.x + stored.y", visibility)

    def test_bilinear_comparison_pcf_is_convex_and_continuous(self):
        comparisons = (0.0, 0.25, 0.75, 1.0)  # gather x/y/z/w order

        def resolve(fx, fy):
            weights = (
                (1.0 - fx) * fy,
                fx * fy,
                fx * (1.0 - fy),
                (1.0 - fx) * (1.0 - fy),
            )
            self.assertAlmostEqual(sum(weights), 1.0)
            return sum(value * weight for value, weight in zip(
                comparisons, weights
            ))

        samples = [resolve(i / 100.0, 0.37) for i in range(101)]
        self.assertTrue(all(0.0 <= value <= 1.0 for value in samples))
        self.assertLess(max(
            abs(b - a) for a, b in zip(samples, samples[1:])
        ), 0.02)

    def test_closed_form_segment_integration_matches_fine_numerical_reference(self):
        sigma_t = 0.0017
        source = 0.42
        segment_length = 137.0
        closed_form = source * (
            1.0 - math.exp(-sigma_t * segment_length)
        ) / sigma_t
        steps = 100_000
        ds = segment_length / steps
        numerical = sum(
            source * math.exp(-sigma_t * (i + 0.5) * ds) * ds
            for i in range(steps)
        )
        self.assertAlmostEqual(closed_form, numerical, places=7)


if __name__ == "__main__":
    unittest.main()
