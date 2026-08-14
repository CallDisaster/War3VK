#!/usr/bin/env python3
"""Release-review contracts for Vulkan ownership and fail-stop behavior."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def body(source: str, signature: str, next_signature: str | None = None) -> str:
    begin = source.index(signature)
    end = source.index(next_signature, begin) if next_signature else len(source)
    return source[begin:end]


class VulkanReleaseReviewStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.aa = read("src/d3d9/d3d9_war3_aa.cpp")
        cls.cs = read("src/dxvk/dxvk_cs.cpp")
        cls.device = read("src/dxvk/dxvk_device.h")
        cls.presenter = read("src/dxvk/dxvk_presenter.cpp")
        cls.outline = read("src/d3d9/d3d9_war3_shadow_outline.cpp")
        cls.shadow = read("src/d3d9/d3d9_war3_shadow.cpp")
        cls.replay = read(
            "src/d3d9/war3/render/war3_shadow_replay_validation.cpp"
        )
        cls.arena = read("src/d3d9/war3/memory/war3_shadow_arena.cpp")
        cls.d3d_device = read("src/d3d9/d3d9_device.cpp")
        cls.volumetric = read("src/d3d9/d3d9_war3_volumetric_light.cpp")
        cls.governor_h = read(
            "src/d3d9/war3/render/war3_gpu_workload_governor.h"
        )
        cls.governor_cpp = read(
            "src/d3d9/war3/render/war3_gpu_workload_governor.cpp"
        )
        cls.diagnostics = read(
            "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
        )

    def test_smaa_upload_and_publication_are_transactional(self) -> None:
        upload = body(self.aa, "bool UploadLookupTexture(", "} // namespace")
        self.assertIn("ctx->track(uploadBuffer, DxvkAccess::Read)", upload)
        create = body(
            self.aa,
            "bool War3AAPass::createSmaaLookupTextures(",
            "void War3AAPass::createFxaaPipeline",
        )
        self.assertIn("candidateAreaTex", create)
        self.assertIn("candidateSearchTex", create)
        reject = create.index("if (!areaUploaded || !searchUploaded")
        publish = create.index("m_lookupTablesCreated = true")
        self.assertLess(reject, publish)
        self.assertIn("return false", create[:publish])

    def test_device_loss_is_terminal_before_wsi_recreation(self) -> None:
        self.assertIn("std::atomic<VkResult>       m_terminalStatus", self.device)
        self.assertIn("compare_exchange_strong", self.device)
        acquire = body(
            self.presenter,
            "VkResult Presenter::acquireNextImage(",
            "VkResult Presenter::presentImage(",
        )
        self.assertLess(
            acquire.index("m_acquireStatus == VK_ERROR_DEVICE_LOST"),
            acquire.index("recreateSwapChain()"),
        )
        recreate = body(
            self.presenter,
            "VkResult Presenter::recreateSwapChain()",
            "VkResult Presenter::createSwapChain()",
        )
        self.assertIn("getDeviceStatus() == VK_ERROR_DEVICE_LOST", recreate)

    def test_cs_exception_destroys_once_and_still_advances_sequence(self) -> None:
        execute = body(
            self.cs,
            "void DxvkCsChunk::executeAll(",
            "void DxvkCsChunk::reset()",
        )
        self.assertLess(execute.index("m_head = cmd->next()"),
                        execute.index("cmd->exec(ctx)"))
        self.assertIn("cmd->~DxvkCsCmd();", execute)
        worker = body(self.cs, "void DxvkCsThread::threadFunc()")
        self.assertLess(worker.index("catch (const DxvkError&"),
                        worker.index("if (entry.seq)"))
        self.assertIn("m_aborted.store(true", worker)
        self.assertIn("counter.store(entry.seq", worker)

    def test_outline_preflights_pipelines_and_tracks_real_layout(self) -> None:
        screen = body(
            self.outline,
            "void War3ShadowReceiverPass::renderUnitOutlineScreenSpace(",
            "void War3ShadowReceiverPass::renderUnitOutline(",
        )
        geometry = body(
            self.outline,
            "void War3ShadowReceiverPass::renderUnitOutline(",
        )
        self.assertLess(screen.index("std::vector<VkPipeline> maskPipelines"),
                        screen.index("cmdBeginRendering"))
        self.assertLess(screen.index("createOutlineMaskPipeline(key)"),
                        screen.index("cmdBeginRendering"))
        self.assertLess(geometry.index("getOrCreateUnitOutlinePipeline"),
                        geometry.index("cmdBeginRendering"))
        self.assertGreaterEqual(screen.count("trackLayout("), 2)
        self.assertIn("VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT", self.shadow)
        self.assertIn("VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT", self.shadow)

    def test_replay_validates_palette_domain_before_draw(self) -> None:
        for token in (
            "paletteRequired",
            "paletteIndex",
            "paletteCount",
            "paletteMatricesPerEntry",
            "InvalidPaletteIndex",
            "PaletteRangeOverflow",
        ):
            self.assertIn(token, self.replay)

    def test_arena_has_exact_device_owner_and_shutdown(self) -> None:
        self.assertIn("DxvkDevice* g_ownerDevice", self.arena)
        init = body(
            self.arena,
            "bool ShadowArena_Init(DxvkDevice* device)",
            "bool ShadowArena_IsInitialized()",
        )
        self.assertIn("g_ownerDevice != device", init)
        shutdown = body(
            self.arena,
            "void ShadowArena_Shutdown(DxvkDevice* device)",
            "bool ShadowArena_BeginFrame(",
        )
        self.assertIn("g_ownerDevice != device", shutdown)
        self.assertIn("g_frameStates.clear()", shutdown)
        self.assertIn("g_ownerDevice = nullptr", shutdown)
        destructor = self.d3d_device[
            self.d3d_device.index("D3D9DeviceEx::~D3D9DeviceEx()") :
            self.d3d_device.index("D3D9DeviceEx::~D3D9DeviceEx()") + 12000
        ]
        self.assertLess(destructor.index("SynchronizeCsThread"),
                        destructor.index("ShadowArena_Shutdown"))

    def test_volumetric_resize_publishes_complete_candidate_set(self) -> None:
        ensure = body(
            self.volumetric,
            "void War3VolumetricLightPass::ensureResources(",
            "void War3VolumetricLightPass::ensurePointShadowFallbackResources(",
        )
        for token in (
            "candidateColor",
            "candidateColorView",
            "candidateDepth",
            "candidateDepthView",
            "candidateEffect",
            "candidateEffectView",
            "candidateGuideBase",
            "candidateGuideBaseView",
            "candidateGuideRefined",
            "candidateGuideRefinedView",
        ):
            self.assertIn(token, ensure)
        first_publish = ensure.index("m_colorCopy = std::move(candidateColor)")
        for candidate_create in (
            'createGuide("War3DirectionalVolumeGuideBase"',
            'createGuide("War3DirectionalVolumeGuideRefined"',
        ):
            self.assertLess(ensure.index(candidate_create), first_publish)
        self.assertGreater(
            ensure.index("m_cachedExtent = extent"), first_publish
        )
        self.assertGreater(
            ensure.index("m_cachedDepthExtent = extent"), first_publish
        )
        self.assertGreater(
            ensure.index("m_cachedEffectExtent = effectExtent"), first_publish
        )

    def test_shadow_workload_reservations_roll_back_before_recording(self) -> None:
        self.assertIn("cancelReservation", self.governor_h)
        self.assertIn("SubtractCost", self.governor_cpp)
        self.assertIn("rolledBackReservations", self.governor_cpp)
        directional = body(
            self.shadow,
            "bool War3ShadowReceiverPass::renderShadowMap(",
            "void War3ShadowReceiverPass::resetPointShadowCpuPlanPreservingCapacity(",
        )
        self.assertIn("ScopedWar3GpuWorkloadReservation", directional)
        first_render = directional.index("ctx->cmdBeginRendering")
        for commit in (
            "volumeWorkloadReservation.commit()",
            "directionalWorkloadReservation.commit()",
        ):
            self.assertLess(directional.index(commit), first_render)
        point = body(
            self.shadow,
            "void War3ShadowReceiverPass::renderPointShadow(",
            "void War3ShadowReceiverPass::drawReceiver(",
        )
        self.assertIn("ScopedWar3GpuWorkloadReservation", point)
        self.assertLess(
            point.index("pointWorkloadReservation.commit()"),
            point.index("ctx->cmdBeginRendering"),
        )

    def test_device_loss_incident_has_a_terminal_one_shot_gate(self) -> None:
        self.assertIn("s_gpuIncidentLatched", self.diagnostics)
        self.assertIn("s_gpuDeviceLostIncidentLatched", self.diagnostics)
        notify = body(
            self.diagnostics,
            "void NotifyGpuDeviceLostFailStop(",
        )
        self.assertIn("if (!s_gpuDeviceLostIncidentLatched)", notify)
        self.assertIn("s_gpuDeviceLostIncidentLatched = true", notify)
        self.assertIn("s_gpuDeviceLostDeviceFaultEnrichmentLatched", notify)
        self.assertIn("queue-error-device-lost-device-fault-enrichment", notify)
        self.assertNotIn("if (s_gpuIncidentLatched)", notify)


if __name__ == "__main__":
    unittest.main()
