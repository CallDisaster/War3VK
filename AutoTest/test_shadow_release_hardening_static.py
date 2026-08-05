import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


SETTINGS = read("src/d3d9/d3d9_war3_settings.h")
PIPELINE = read("src/d3d9/d3d9_war3_pipeline.cpp")
SHADOW_H = read("src/d3d9/d3d9_war3_shadow.h")
SHADOW_CPP = read("src/d3d9/d3d9_war3_shadow.cpp")
RESOURCES = read("src/d3d9/d3d9_war3_shadow_resources.cpp")
CSM = read("src/d3d9/d3d9_war3_csm.h")
INTERNAL = read("src/d3d9/war3/core/war3_internal_test_config.h")
ARENA_H = read("src/d3d9/war3/memory/war3_shadow_arena.h")
ARENA_CPP = read("src/d3d9/war3/memory/war3_shadow_arena.cpp")
DEVICE_H = read("src/d3d9/d3d9_device.h")
DEVICE_CPP = read("src/d3d9/d3d9_device.cpp")
UI = read("src/d3d9/war3/ui/war3_imgui.cpp")
DIAG_H = read("src/d3d9/war3/tools/war3_diagnostics_hub.h")
DIAG_CPP = read("src/d3d9/war3/tools/war3_diagnostics_hub.cpp")
PROBE = read("AutoTest/run_bridge_ramp_visual_probe.py")
SHADER = read("subprojects/war3fx/shaders/war3_shadow_receiver.frag")


class ShadowReleaseHardeningStaticTests(unittest.TestCase):
    def test_taa_environment_is_constructor_only_and_ui_has_revision(self):
        self.assertIn('getEnvVar("DXVK_WAR3_SHADOW_TAA_MODE")', PIPELINE)
        self.assertIn('ParseEnvInt("DXVK_WAR3_SHADOW_TAA"', PIPELINE)
        resolve_start = SHADOW_CPP.index("ResolveShadowTaaRequestedMode")
        resolve_end = SHADOW_CPP.index(
            "ShadowTaaDisableForSemanticDynamicEnabled", resolve_start
        )
        resolver = SHADOW_CPP[resolve_start:resolve_end]
        self.assertNotIn("getEnvVar", resolver)
        self.assertIn("shadowTaaSettingsRevision", SETTINGS)
        self.assertIn("++settings.shadows.shadowTaaSettingsRevision", UI)

    def test_taa_diagnostics_prove_requested_effective_shader_state(self):
        self.assertIn("struct ShadowTaaDiagnostics", SHADOW_H)
        for field in (
            "requestedMode",
            "effectiveMode",
            "shaderMode",
            "historyValid",
            "historyReadable",
            "historyGeneration",
            "lastInvalidationReason",
            "fixedWallBypassCount",
        ):
            self.assertIn(field, SHADOW_H)
            self.assertIn(field, DIAG_CPP)
        self.assertIn("TAA requested/effective/shader", UI)

    def test_csm_defaults_to_fixed_4096_and_tighter_filter(self):
        self.assertIn("uint32_t shadowResolution = 4096", CSM)
        self.assertIn("float pcfRadius = 0.70f", SETTINGS)
        self.assertIn("kShadowAdaptiveResolutionEnabled = false", INTERNAL)
        self.assertIn("1.50", SHADER)

    def test_csm_fallback_is_latched_and_resource_creation_is_transactional(self):
        self.assertIn("struct CsmResolutionDiagnostics", SHADOW_H)
        self.assertIn("m_csmResolutionFallbackLatched", SHADOW_H)
        self.assertIn("kCsmFallbackReserveBytes", RESOURCES)
        self.assertIn("VK_EXT_memory_budget", RESOURCES)
        self.assertIn("candidateShadowMap", RESOURCES)
        self.assertIn("candidateShadowCasterMask", RESOURCES)
        self.assertIn("std::swap", RESOURCES)
        self.assertIn("m_shadowMapResolution == candidateResolution", RESOURCES)
        self.assertNotIn(
            "m_csmConfig.shadowResolution = m_csmEffectiveResolution",
            SHADOW_CPP,
        )
        self.assertIn(
            "War3CsmConfig effectiveCsmConfig = m_csmConfig",
            SHADOW_CPP,
        )
        self.assertIn("return false", RESOURCES)

    def test_shadow_arena_is_bounded_and_fence_completed_before_reuse(self):
        self.assertIn("64 * 1024 * 1024", ARENA_CPP)
        self.assertIn("384 * 1024 * 1024", ARENA_CPP)
        self.assertIn("1152ull * 1024ull * 1024ull", ARENA_CPP)
        self.assertIn("completedSerial", ARENA_H)
        self.assertIn("retireSerial", ARENA_CPP)
        self.assertIn("busyReuseRejectCount", ARENA_H)
        self.assertIn("m_war3ShadowArenaFence", DEVICE_H)
        self.assertIn("ctx->signal(cShadowArenaFence", DEVICE_CPP)
        self.assertNotIn("回退 LegacyFreeze", ARENA_CPP)

    def test_runtime_status_exposes_taa_csm_arena_and_queue_contract(self):
        for field in (
            "shadowTaaRequestedMode",
            "shadowTaaEffectiveMode",
            "shadowTaaShaderMode",
            "csmRequestedResolution",
            "csmEffectiveResolution",
            "csmFallbackReason",
            "shadowArenaUsedBytes",
            "shadowArenaResidentBytes",
            "shadowArenaBusyReuseRejectCount",
            "shadowArenaOverflowCount",
            "queueSubmittedSerial",
            "queueCompletedSerial",
            "queueLastResult",
        ):
            self.assertIn(field, DIAG_H)
            self.assertIn(field, DIAG_CPP)

    def test_gpu_flight_recorder_and_incident_are_bounded_and_atomic(self):
        self.assertIn("struct GpuIncidentSnapshot", DIAG_H)
        self.assertIn("struct GpuFlightFrame", DIAG_H)
        self.assertIn("s_gpuFlightFrames.size() > 240u", DIAG_CPP)
        self.assertIn("stalledMs >= 10000u", DIAG_CPP)
        self.assertIn("lastRenderStage", DIAG_H)
        self.assertIn("csmMemoryAvailableBytes", DIAG_H)
        self.assertIn("arenaBusyReuseRejectCount", DIAG_H)
        self.assertIn("MoveFileExA", DIAG_CPP)
        self.assertIn("MOVEFILE_REPLACE_EXISTING", DIAG_CPP)
        self.assertIn("while (incidents.size() > 4u)", DIAG_CPP)

    def test_attach_only_probe_never_owns_or_stops_the_game(self):
        self.assertIn('"--attach-only"', PROBE)
        self.assertIn('"--attach-pid"', PROBE)
        self.assertIn("attach_only", PROBE)
        self.assertIn("trace_ring_max_disk_mb", PROBE)
        self.assertIn("default=512", PROBE)
        self.assertIn("default=3", PROBE)
        self.assertIn("owns_process = False", PROBE)
        self.assertIn("if owns_process:", PROBE)
        self.assertIn("args.capture_retain_count = 16", PROBE)
        self.assertIn("evidenceRetainedBytes", PROBE)
        self.assertIn("hard-evidence-disk-cap", PROBE)

    def test_manual_evidence_retention_is_visible_and_bounded(self):
        self.assertIn("RequestShadowEvidenceRetention", DIAG_H)
        self.assertIn("shadowEvidenceRetentionRevision", DIAG_H)
        self.assertIn("shadowEvidenceCollectorAttached", DIAG_H)
        self.assertIn("保留最近阴影证据", UI)
        self.assertIn("512", PROBE)


if __name__ == "__main__":
    unittest.main()
