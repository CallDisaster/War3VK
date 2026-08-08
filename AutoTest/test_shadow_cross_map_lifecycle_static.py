from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HOOK = (ROOT / "src/d3d9/d3d9_war3_hook.cpp").read_text(encoding="utf-8")
BOOTSTRAP = (
    ROOT / "src/d3d9/war3/platform/war3_runtime_bootstrap.cpp"
).read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
VISIBLE = (
    ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp"
).read_text(encoding="utf-8")
ARENA = (ROOT / "src/d3d9/war3/memory/war3_shadow_arena.cpp").read_text(
    encoding="utf-8"
)
LIFECYCLE = (
    ROOT / "src/d3d9/war3/render/war3_shadow_lifecycle.cpp"
).read_text(encoding="utf-8")
MODEL_REGISTRY = (
    ROOT / "src/d3d9/war3/model/war3_model_registry.cpp"
).read_text(encoding="utf-8")
SHADOW_OBJECT = (
    ROOT / "src/d3d9/war3/render/war3_shadow_object_registry.cpp"
).read_text(encoding="utf-8")
CONTRACT_CACHE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8")
RENDERER = (
    ROOT / "src/d3d9/war3/render/war3_renderer.cpp"
).read_text(encoding="utf-8")
DIAGNOSTICS = (
    ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
).read_text(encoding="utf-8")
RECORDER = (
    ROOT / "AutoTest/run_attach_cross_map_shadow_gate.py"
).read_text(encoding="utf-8")


def body(text: str, signature: str, next_signature: str) -> str:
    start = text.index(signature)
    end = text.index(next_signature, start + len(signature))
    return text[start:end]


class ShadowCrossMapLifecycleStaticTests(unittest.TestCase):
    def test_unload_is_request_only_for_render_owned_state(self) -> None:
        reset = body(HOOK, "void ResetWar3RuntimeState()", "bool ValidateGameModule")
        self.assertIn("War3RequestShadowMapEpochReset", reset)
        self.assertNotIn("ShadowArena_Reset", reset)
        self.assertNotIn("War3ResetGpuSkinMapEpoch", reset)

        cpu_reset = body(BOOTSTRAP, "void ResetRuntimeCore()", "void BindNativeShadowDevice")
        for forbidden in (
            "RenderQueueTracker::instance().Reset",
            "ExecBatchProcessor::ResetCaches",
            "War3RenderState::ResetRuntimeState",
            "war3::model::Shutdown",
            "ResetShadowRuntimeBridgeState",
            "War3LightningRuntime::instance().reset",
            "ShadowValidationRuntime::instance().reset",
            "ShadowArena_Reset",
        ):
            self.assertNotIn(forbidden, cpu_reset)

    def test_present_applies_transition_after_before_ui_before_tracking(self) -> None:
        present = body(
            DEVICE,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx",
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx",
        )
        before = present.index("War3MaybeInsertBeforeUi(true)")
        transition = present.index("War3ApplyShadowMapEpochResetAtPresent")
        tracking = present.index('War3PresentFrameTransitionScope("TrackingDecision")')
        self.assertLess(before, transition)
        self.assertLess(transition, tracking)
        self.assertIn("ShadowArena_QuarantineCurrentGeneration", DEVICE)
        self.assertIn("ctx->signal(cShadowArenaFence", DEVICE)
        allocator_reset = body(
            (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8"),
            "void War3ResetShadowAllocator()",
            "// War3 Shadow",
        )
        self.assertIn("m_war3ShadowSessionReady.load", allocator_reset)
        self.assertIn("m_war3ShadowMapResetRequestedSerial.load", allocator_reset)

    def test_epoch_is_process_monotonic_and_stamped_on_final_draws(self) -> None:
        self.assertIn("MintWar3ShadowMapEpoch", DEVICE)
        self.assertIn("std::numeric_limits<uint64_t>::max()", DEVICE)
        self.assertIn("uint64_t mapEpoch = 0u;", SCENE)
        self.assertIn("uint64_t deviceEpoch = 0u;", SCENE)
        self.assertGreaterEqual(DEVICE.count("draw.mapEpoch = m_war3GpuSkinMapEpoch"), 5)
        self.assertIn("entry.mapEpoch != m_shadowManifestMapEpoch", VISIBLE)

    def test_pending_session_gates_all_shadow_producer_entry_points(self) -> None:
        for signature, next_signature in (
            (
                "bool D3D9DeviceEx::War3TryAppendSemanticShadowPacket(\n",
                "bool D3D9DeviceEx::War3DrainShadowCasterTombstones",
            ),
            (
                "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(\n",
                "bool D3D9DeviceEx::War3ExecuteSemanticShadowSceneForValidation",
            ),
            (
                "void D3D9DeviceEx::War3TryCaptureShadowCaster(\n",
                "} // namespace dxvk",
            ),
        ):
            section = body(DEVICE, signature, next_signature)
            self.assertIn("m_war3ShadowSessionReady.load", section)
            self.assertIn("m_war3ShadowMapResetRequestedSerial.load", section)

    def test_arena_quarantine_never_rewinds_current_generation(self) -> None:
        quarantine = body(
            ARENA,
            "bool ShadowArena_QuarantineCurrentGeneration",
            "ShadowArenaAllocation ShadowArena_Alloc",
        )
        self.assertIn(
            "g_currentFrameIndex.exchange(kInvalidGenerationIndex",
            quarantine,
        )
        self.assertIn("retireSerial", quarantine)
        self.assertNotIn("cursor = 0", quarantine)
        self.assertNotIn("Reset", quarantine)

    def test_replay_validation_precedes_clear_and_blocks_partial_plan(self) -> None:
        render = body(
            SHADOW,
            "bool War3ShadowReceiverPass::renderShadowMap",
            "namespace {\n// A2 Worker_Prepare",
        )
        validate = render.index("validateShadowReplayDraws")
        clear = render.index("VK_ATTACHMENT_LOAD_OP_CLEAR")
        incomplete = render.index("preparedDrawCount != requiredPreparedDrawCount")
        self.assertLess(validate, clear)
        self.assertLess(incomplete, clear)
        self.assertIn("IncompleteReplayPlan", render)
        self.assertIn("m_replayValidationHoldFramesRemaining = 8u", SHADOW)
        self.assertIn("m_hasCompleteShadowMap = false", SHADOW)
        self.assertIn("receiverHasUsableDirectionalShadow\n        ? receiverShadowStrength", SHADOW)

    def test_receiver_rejects_cross_epoch_and_invalidates_publication(self) -> None:
        invalidate = body(
            SHADOW,
            "void War3ShadowReceiverPass::InvalidateMapEpoch",
            "void War3ShadowReceiverPass::renderShadowVisibility",
        )
        for required in (
            "waitPointShadowCpuPrepare",
            "m_pointShadowPrepareFuture.valid",
            "m_pointShadowWorkerCancelCount",
            "shutdown",
            "invalidatePointShadowPublishedState",
            "m_hasCompleteShadowMap = false",
            "m_shadowHistoryValid = false",
            "invalidateVolumeSunShadowPublication",
        ):
            self.assertIn(required, invalidate)
        self.assertIn("input.mapEpoch != m_shadowMapEpoch", SHADOW)

    def test_identity_tombstones_are_map_epoch_scoped(self) -> None:
        self.assertIn("g_mapEpoch", LIFECYCLE)
        self.assertIn("ResetShadowCasterLifecycleMapEpoch", LIFECYCLE)
        self.assertIn("State().activeByIdentity.clear()", LIFECYCLE)
        self.assertIn("ShadowCasterTombstoneBelongsToMap", LIFECYCLE)
        self.assertIn("tombstone.mapEpoch == mapEpoch", LIFECYCLE)
        self.assertIn("ShadowCasterTombstoneReason::StageDisabled", LIFECYCLE)
        self.assertIn(
            "m_war3ShadowTombstoneSerialSeen =\n"
            "      war3::render::ResetShadowCasterLifecycleMapEpoch",
            DEVICE,
        )

    def test_device_frame_fallbacks_do_not_cross_map_epoch(self) -> None:
        reset = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetShadowSessionState",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones",
        )
        for token in (
            "m_war3PerDrawUpload = War3PerDrawUploadInfo{}",
            "m_war3LastGoodCamera = War3WorldCameraState{}",
            "m_war3LastWorldRt0 = nullptr",
            "m_war3LastWorldDs = nullptr",
            "m_war3BestWorldViewportArea = 0u",
            "m_war3BestWorldCameraTier = 0u",
        ):
            self.assertIn(token, reset)

    def test_semantic_pointer_and_pose_registries_reset_at_present(self) -> None:
        transition = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ApplyShadowMapEpochResetAtPresent",
            "bool D3D9DeviceEx::War3GpuSkinDeviceReady",
        )
        self.assertIn("War3Renderer::instance().ResetMapSession()", transition)
        self.assertNotIn("War3Renderer::instance().EndFrame()", transition)

        reset = body(
            RENDERER,
            "void War3Renderer::ResetMapSession()",
            "void War3Renderer::BeginFrame()",
        )
        for token in (
            "RenderObjectRegistry::instance().beginFrame()",
            "VisibleRenderableRegistry::instance().beginFrame()",
            "ModelRegistry::instance().resetMapSession()",
            "ModelInstanceRegistry::instance().resetMapSession()",
            "PoseRegistry::instance().resetMapSession()",
            "AttachmentRigidRegistry::instance().resetMapSession()",
            "ShadowObjectRegistry::instance().resetMapSession()",
            "ShadowRuntimeContractCache::instance().resetMapSession()",
        ):
            self.assertIn(token, reset)

        for signature, next_signature, required in (
            (
                "void ModelRegistry::resetMapSession()",
                "void ModelRegistry::recordSpriteModelPath",
                ("m_bySprite", "m_byRuntimeModel", "m_byPath"),
            ),
            (
                "void ModelInstanceRegistry::resetMapSession()",
                "void ModelInstanceRegistry::storeRuntimeModelRecordLocked",
                ("m_byWorldObjectEntry", "m_byRuntimeModel", "m_byHandle"),
            ),
            (
                "void PoseRegistry::resetMapSession()",
                "void PoseRegistry::storeRuntimeModelRecordLocked",
                ("m_byRuntimeModel", "m_bySceneNode", "m_byUnitPtr"),
            ),
            (
                "void AttachmentRigidRegistry::resetMapSession()",
                "void AttachmentRigidRegistry::storeRecord",
                ("m_byChildRuntimeModel", "m_bySceneNode", "m_byHandle"),
            ),
        ):
            section = body(MODEL_REGISTRY, signature, next_signature)
            for token in required:
                self.assertIn(f"ClearRegistryMap({token})", section)

        shadow_object_reset = body(
            SHADOW_OBJECT,
            "void ShadowObjectRegistry::resetMapSession()",
            "void ShadowObjectRegistry::storeRecord",
        )
        self.assertIn("RegistryMutationGenerationGuard", shadow_object_reset)
        self.assertIn("ClearRegistryMap(m_byRuntimeModel)", shadow_object_reset)
        self.assertIn("ClearRegistryMap(m_byHandle)", shadow_object_reset)

        contract_reset = body(
            CONTRACT_CACHE,
            "void ShadowRuntimeContractCache::resetMapSession()",
            "void ShadowRuntimeContractCache::captureLiveState()",
        )
        self.assertIn("std::make_shared<ShadowFrameManifest>()", contract_reset)
        self.assertIn("manifest->publishRevision = ++m_publishRevision", contract_reset)
        self.assertIn("m_resourceRefreshFrameSerial = 0u", contract_reset)

    def test_runtime_status_exports_transition_and_replay_contract(self) -> None:
        for field in (
            "shadowMapResetRequestedSerial",
            "shadowMapResetAppliedSerial",
            "shadowMapEpoch",
            "shadowArenaQuarantineCount",
            "shadowReplayValidatedCasterCount",
            "shadowReplayPartialPreventedCount",
            "shadowFirstCompleteLatencyFrames",
            "shadowPointLateResultRejectCount",
        ):
            self.assertIn(f'"{field}"', DIAGNOSTICS)

    def test_cross_map_recorder_is_attach_only(self) -> None:
        self.assertIn("--attach-pid", RECORDER)
        self.assertIn("set_shadow_evidence_collector", RECORDER)
        self.assertIn("retain_shadow_evidence", RECORDER)
        for forbidden in (
            "Popen(",
            "start_war3",
            "stop_war3",
            "TerminateProcess",
            "SetPriorityClass",
        ):
            self.assertNotIn(forbidden, RECORDER)


if __name__ == "__main__":
    unittest.main()
