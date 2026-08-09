from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
HOOK = (ROOT / "src/d3d9/d3d9_war3_hook.cpp").read_text(encoding="utf-8")
WAR3 = (ROOT / "src/d3d9/war3/war3.cpp").read_text(encoding="utf-8")
VISUAL_BRIDGE = (
    ROOT / "src/d3d9/war3/bridge/war3_visual_bridge_v1.cpp"
).read_text(encoding="utf-8")
JAPI = (ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp").read_text(
    encoding="utf-8"
)
INTERNAL_TEST_API = (
    ROOT / "src/d3d9/war3/tools/war3_internal_test_api.cpp"
).read_text(encoding="utf-8")
RENDER_HOOK = (
    ROOT / "src/d3d9/war3/hooks/war3_hook_render.cpp"
).read_text(encoding="utf-8")
BOOTSTRAP = (
    ROOT / "src/d3d9/war3/platform/war3_runtime_bootstrap.cpp"
).read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
SCENE_COLLECTOR = (
    ROOT / "src/d3d9/war3/render/war3_scene_collector.cpp"
).read_text(encoding="utf-8")
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
MODEL_HOOK = (
    ROOT / "src/d3d9/war3/model/war3_model_hook.cpp"
).read_text(encoding="utf-8")
SHADOW_OBJECT = (
    ROOT / "src/d3d9/war3/render/war3_shadow_object_registry.cpp"
).read_text(encoding="utf-8")
CONTRACT_CACHE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp"
).read_text(encoding="utf-8")
RENDERER_CORE = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
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
    def test_render_hook_unit_identity_hot_cache_is_map_scoped(self) -> None:
        identity = body(
            RENDER_HOOK,
            "static void TryFillUnitIdentityFromUnitPtr(",
            "static void ClassifyWorldTagIdentity(",
        )
        self.assertIn("uint64_t mapEpoch", identity)
        self.assertIn("ShadowModelResourceCache::instance().mapEpoch()", identity)
        self.assertIn("cacheEntry.mapEpoch == mapEpoch", identity)
        self.assertIn("cacheEntry.mapEpoch = mapEpoch", identity)

    def test_model_hook_map_reset_preserves_hooks_and_drops_raw_pointer_state(self) -> None:
        reset = body(MODEL_HOOK, "void ResetMapSession()", "void Shutdown()")
        for required in (
            "entry.valid = false",
            "entry.renderablePart.store(0u",
            "entry.paletteWriteSerial.store(0u",
            "s_cachedGlobalPaletteBufBase.store(0u",
            "g_runtimePoseArrayByModel.clear()",
            "g_runtimePoseArrayByMatrixPtr.clear()",
            "g_runtimePoseArrayRegistrySize.store(0u",
            "g_runtimeParentLinks.clear()",
            "g_runtimePaletteTreeDedupeRoots.clear()",
            "g_runtimePaletteTreeDedupeOwnerRoots.clear()",
        ):
            self.assertIn(required, reset)
        for forbidden in (
            "g_active.store(false",
            "g_bootstrapHooksInstalled.store(false",
            "g_fullHooksInstalled.store(false",
            "g_gameBase = 0u",
        ):
            self.assertNotIn(forbidden, reset)

        shutdown = body(MODEL_HOOK, "void Shutdown()", "bool IsActive()")
        self.assertIn("ResetMapSession()", shutdown)
        self.assertIn("g_active.store(false", shutdown)

        epoch_reset = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetGpuSkinMapEpoch()",
            "void D3D9DeviceEx::War3RequestShadowMapEpochReset()",
        )
        self.assertIn(
            "m_war3GpuSkinMapEpoch = War3ResetCpuSemanticMapSession(",
            epoch_reset,
        )

        cpu_reset = body(
            DEVICE,
            "uint64_t D3D9DeviceEx::War3ResetCpuSemanticMapSession(",
            "D3D9DeviceEx::D3D9DeviceEx(",
        )
        self.assertLess(
            cpu_reset.index("ShadowModelResourceCache::instance().resetMapEpoch"),
            cpu_reset.index("war3::model::ResetMapSession()"),
        )
        self.assertIn("war3::model::ResetMapSession()", cpu_reset)

        transition = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ApplyShadowMapEpochResetAtPresent(",
            "bool D3D9DeviceEx::War3ApplyShadowDeviceEpochTransitionAtPresent(",
        )
        self.assertNotIn("war3::model::Shutdown()", transition)

    def test_shadow_runtime_hot_caches_are_map_scoped(self) -> None:
        pointer_cache = body(
            CONTRACT_CACHE,
            "struct PointerBoolCacheEntry",
            "struct ManifestResolveDiagnostics",
        )
        self.assertIn("uint64_t mapEpoch", pointer_cache)

        cached_pointer = body(
            CONTRACT_CACHE,
            "bool CachedPointerBool",
            "bool LooksLikeRuntimeModelPtrForContract",
        )
        self.assertIn("ShadowModelResourceCache::instance().mapEpoch()", cached_pointer)
        self.assertIn("slot.mapEpoch == mapEpoch", cached_pointer)
        self.assertIn("slot.mapEpoch = mapEpoch", cached_pointer)

        legacy_geoset = body(
            CONTRACT_CACHE,
            "void ResolveCurrentRuntimeGeosetFromDataLegacy",
            "void ResolveCurrentRuntimeGeosetFromData(",
        )
        self.assertIn("uint64_t mapEpoch", legacy_geoset)
        self.assertIn("entry.mapEpoch == mapEpoch", legacy_geoset)
        self.assertIn("entry.mapEpoch = mapEpoch", legacy_geoset)

        static_entry = body(
            RENDERER_CORE,
            "struct StaticMeshDataResourceCacheEntry",
            "void ApplyStaticMeshDataResource",
        )
        self.assertIn("uint64_t mapEpoch", static_entry)
        static_lookup = body(
            RENDERER_CORE,
            "const ShadowModelResourceRecord* TryFindStaticMeshDataResource",
            "ShadowPoseRecord ConvertPoseRecord",
        )
        self.assertIn("it->second.mapEpoch == currentMapEpoch", static_lookup)
        self.assertGreaterEqual(static_lookup.count("entry.mapEpoch = currentMapEpoch"), 2)

        validation_reset = body(
            RENDERER_CORE,
            "void ShadowValidationRuntime::reset()",
            "ShadowValidationFrameStats ShadowValidationRuntime::snapshot() const",
        )
        self.assertIn("StaticMeshDataResourceCache().clear()", validation_reset)

    def test_unload_is_request_only_for_render_owned_state(self) -> None:
        reset = body(HOOK, "void ResetWar3RuntimeState()", "bool ValidateGameModule")
        self.assertIn("RequestActiveDeviceShadowMapResetOrCpuFallback", reset)
        self.assertNotIn("ShadowArena_Reset", reset)
        self.assertNotIn("War3ResetGpuSkinMapEpoch", reset)
        self.assertNotIn("GetActiveDevice", reset)
        self.assertNotIn("War3ResetCpuSemanticMapSession", reset)

        fallback = body(
            WAR3,
            "void RequestActiveDeviceShadowMapResetOrCpuFallback()",
            "struct War3SettingsWrite::Impl",
        )
        self.assertIn("s_activePublicationMutex", fallback)
        self.assertIn("device->War3RequestShadowMapEpochReset()", fallback)
        self.assertIn("D3D9DeviceEx::War3ResetCpuSemanticMapSession()", fallback)

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
        map_transition = present.index("War3ApplyShadowMapEpochResetAtPresent")
        device_transition = present.index(
            "War3ApplyShadowDeviceEpochTransitionAtPresent"
        )
        tracking = present.index('War3PresentFrameTransitionScope("TrackingDecision")')
        self.assertLess(before, map_transition)
        self.assertLess(map_transition, device_transition)
        self.assertLess(device_transition, tracking)
        self.assertIn("ShadowArena_QuarantineCurrentGeneration", DEVICE)
        self.assertIn("ctx->signal(cShadowArenaFence", DEVICE)
        allocator_reset = body(
            (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8"),
            "void War3ResetShadowAllocator()",
            "// War3 Shadow",
        )
        self.assertIn("m_war3ShadowSessionReady.load", allocator_reset)
        self.assertIn("m_war3ShadowMapResetRequestedSerial.load", allocator_reset)
        self.assertIn("m_war3ShadowDeviceEpochRequested.load", allocator_reset)
        self.assertIn("m_war3ShadowDeviceRebindPending.load", allocator_reset)

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
            self.assertIn("m_war3ShadowDeviceEpochRequested.load", section)
            self.assertIn("m_war3ShadowDeviceEpochApplied.load", section)
            self.assertIn("m_war3ShadowDeviceRebindPending.load", section)

    def test_device_epoch_commit_is_a_present_owned_receiver_transaction(self) -> None:
        rebind = body(
            DEVICE,
            "void D3D9DeviceEx::War3RetryGpuSkinDeviceRebind()",
            "void D3D9DeviceEx::War3ResetGpuSkinDeviceEpoch()",
        )
        self.assertEqual(
            rebind.count("m_war3GpuSkinDeviceEpoch = candidateEpoch"), 2
        )
        self.assertEqual(
            rebind.count("War3RequestShadowDeviceEpochTransition("), 2
        )
        first_commit = rebind.index("m_war3GpuSkinDeviceEpoch = candidateEpoch")
        first_request = rebind.index("War3RequestShadowDeviceEpochTransition(")
        second_commit = rebind.index(
            "m_war3GpuSkinDeviceEpoch = candidateEpoch", first_commit + 1
        )
        second_request = rebind.index(
            "War3RequestShadowDeviceEpochTransition(", first_request + 1
        )
        self.assertLess(first_commit, first_request)
        self.assertLess(first_request, second_commit)
        self.assertLess(second_commit, second_request)

        invalidate = body(
            DEVICE,
            "void D3D9DeviceEx::War3InvalidateShadowReceiverEpochOnCs(",
            "bool D3D9DeviceEx::War3ApplyShadowMapEpochResetAtPresent(",
        )
        self.assertIn("EmitCs([receiver, mapEpoch, deviceEpoch]", invalidate)
        self.assertIn("receiver->InvalidateMapEpoch(mapEpoch, deviceEpoch)", invalidate)

        transition = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ApplyShadowDeviceEpochTransitionAtPresent(",
            "bool D3D9DeviceEx::War3GpuSkinDeviceReady() const",
        )
        for token in (
            "War3QuarantineShadowSessionAtPresent(retireSerial)",
            "War3ResetShadowSessionState(retireSerial)",
            "War3InvalidateShadowReceiverEpochOnCs(",
            "m_war3ShadowDeviceEpochApplied.store(",
            "m_war3ShadowDeviceRebindPending.store(",
        ):
            self.assertIn(token, transition)
        self.assertNotIn("receiver->InvalidateMapEpoch", transition)

        present = body(
            DEVICE,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx",
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx",
        )
        self.assertGreaterEqual(
            present.count("m_war3ShadowDeviceEpochRequested.load("), 3
        )
        self.assertIn("war3::RunWithActiveDevicePublication(", present)

    def test_failed_device_rebind_keeps_every_shadow_gate_closed(self) -> None:
        rebind = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetGpuSkinDeviceEpoch()",
            "bool D3D9DeviceEx::War3ResetGpuSkinBridgeForTest",
        )
        gate = rebind.index("m_war3ShadowDeviceRebindPending.store(true")
        diagnostics = rebind.index("War3LogGpuSkinDiagnostics(true)")
        retry = rebind.index("War3RetryGpuSkinDeviceRebind()")
        self.assertLess(gate, diagnostics)
        self.assertLess(gate, retry)
        self.assertIn("m_war3ShadowSessionReady.store(false", rebind)
        self.assertNotIn("m_war3ShadowDeviceRebindPending.store(false", rebind)

        transition = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ApplyShadowDeviceEpochTransitionAtPresent(",
            "bool D3D9DeviceEx::War3GpuSkinDeviceReady() const",
        )
        clear = transition.index("m_war3ShadowDeviceRebindPending.store(")
        invalidate = transition.index("War3InvalidateShadowReceiverEpochOnCs(")
        self.assertLess(invalidate, clear)
        self.assertIn("!War3GpuSkinDeviceReady()", transition)

        self.assertIn(
            "std::atomic<uint64_t> m_war3ShadowDeviceEpochApplied",
            DEVICE_H,
        )

    def test_internal_map_reset_probe_only_publishes_a_present_request(self) -> None:
        probe = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ResetGpuSkinBridgeForTest(bool deviceEpoch)",
            "bool D3D9DeviceEx::War3LogGpuSkinDiagnosticsForTest(",
        )
        self.assertIn("War3RequestShadowMapEpochReset()", probe)
        self.assertNotIn("War3ResetGpuSkinMapEpoch()", probe)
        command = body(
            INTERNAL_TEST_API,
            '} else if (state->request.command == "gpu_skin.reset_bridge") {',
            '} else if (state->request.command == "shutdown.session") {',
        )
        self.assertIn("shadowMapResetRequestedBefore", command)
        self.assertIn("shadowMapResetRequestedAfter", command)
        self.assertIn('scope == "map"', command)

    def test_cpu_semantic_reset_is_shared_without_old_gpu_ownership(self) -> None:
        reset = body(
            DEVICE,
            "uint64_t D3D9DeviceEx::War3ResetCpuSemanticMapSession(",
            "D3D9DeviceEx::D3D9DeviceEx(",
        )
        for token in (
            "RenderQueueTracker::instance().Reset()",
            "ExecBatchProcessor::ResetCaches()",
            "War3RenderState::ResetRuntimeState()",
            "ResetShadowRuntimeBridgeState()",
            "ShadowValidationRuntime::instance().reset()",
            "War3Renderer::instance().ResetMapSession()",
            "war3::model::ResetMapSession()",
            "ResetCurrentDrawContractCache()",
            "War3ResetDirectPacketMapCaches()",
            "War3ShadowDrawMetadataStore().clear()",
            "ClearPayloadsForLifecycleOverflow()",
        ):
            self.assertIn(token, reset)
        for forbidden in (
            "ShadowArena_",
            "m_shadowReceiverPass",
            "m_war3GpuSkinManager",
            "War3ResetShadowSessionState",
        ):
            self.assertNotIn(forbidden, reset)
        semantic_lock = reset.index("g_war3CpuSemanticMapSessionMutex")
        mint = reset.index("MintWar3ShadowMapEpoch()")
        registry_reset = reset.index("resetShadowManifestMapEpoch(mapEpoch)")
        self.assertLess(semantic_lock, mint)
        self.assertLess(mint, registry_reset)

        constructor = body(
            DEVICE,
            "D3D9DeviceEx::D3D9DeviceEx(",
            "D3D9DeviceEx::~D3D9DeviceEx()",
        )
        publish_transaction = constructor.index("war3::PublishActiveDeviceAfter(this")
        cpu_reset = constructor.index("War3ResetCpuSemanticMapSession(")
        receiver_init = constructor.index("m_shadowReceiverPass->InvalidateMapEpoch(")
        self.assertLess(publish_transaction, cpu_reset)
        self.assertLess(cpu_reset, receiver_init)
        self.assertLess(constructor.index("RegisterPass("), publish_transaction)
        self.assertNotIn("MintWar3ShadowMapEpoch()", constructor)

    def test_active_device_handoff_and_map_transition_share_one_transaction(self) -> None:
        publish = body(
            WAR3,
            "void PublishActiveDeviceAfter(",
            "bool ClearActiveDeviceIfCurrent(",
        )
        self.assertIn("s_activePublicationMutex", publish)
        self.assertLess(
            publish.index("beforePublish()"),
            publish.index("PublishActiveDeviceLocked(device)"),
        )

        run = body(
            WAR3,
            "bool RunWithActiveDevicePublication(",
            "void RequestActiveDeviceShadowMapResetOrCpuFallback()",
        )
        self.assertIn("s_activePublicationMutex", run)
        self.assertIn("s_activeDevice.load", run)
        self.assertIn("transaction()", run)

        present = body(
            DEVICE,
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::PresentEx",
            "HRESULT STDMETHODCALLTYPE D3D9DeviceEx::CreateRenderTargetEx",
        )
        transaction = present.index("war3::RunWithActiveDevicePublication(")
        map_reset = present.index("War3ApplyShadowMapEpochResetAtPresent(")
        device_reset = present.index("War3ApplyShadowDeviceEpochTransitionAtPresent(")
        self.assertLess(transaction, map_reset)
        self.assertLess(map_reset, device_reset)

    def test_active_device_queries_do_not_export_dereferenceable_raw_pointers(self) -> None:
        run = body(
            WAR3,
            "bool RunWithActiveDevice(",
            "bool HasActivePipeline()",
        )
        self.assertIn("s_activePublicationMutex", run)
        self.assertIn("transaction(*device)", run)

        has_pipeline = body(
            WAR3,
            "bool HasActivePipeline()",
            "bool RunWithActiveDevicePublication(",
        )
        self.assertIn("RunWithActiveDevice", has_pipeline)
        self.assertIn("device.GetWar3Pipeline() != nullptr", has_pipeline)
        self.assertIn("HasActivePipeline()", VISUAL_BRIDGE)
        self.assertIn("HasActivePipeline()", JAPI)

        offenders = []
        for path in (ROOT / "src").rglob("*"):
            if path.suffix not in {".cpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if (
                "GetActiveDevice(" in text
                or "GetActivePipeline(" in text
                or "SetActiveDevice(" in text
            ):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual([], offenders)

    def test_scene_collector_pointer_tls_invalidates_before_empty_early_return(self) -> None:
        tracked_epoch = SCENE_COLLECTOR.index(
            "if (s_trackedPtrMapMapEpoch != mapEpoch)"
        )
        unit_epoch = SCENE_COLLECTOR.index(
            "if (s_unitHandleCacheMapEpoch != mapEpoch)"
        )
        early_return = SCENE_COLLECTOR.index(
            "if (filtered && s_trackedHandles.empty() && !probeEnabled)"
        )
        first_early_return = SCENE_COLLECTOR.index("if (!gameWorldPtr)")
        self.assertLess(tracked_epoch, first_early_return)
        self.assertLess(unit_epoch, first_early_return)
        self.assertLess(tracked_epoch, early_return)
        self.assertLess(unit_epoch, early_return)
        before_return = SCENE_COLLECTOR[tracked_epoch:first_early_return]
        for token in (
            "s_trackedHandles.clear()",
            "s_trackedHandlesCached.clear()",
            "s_trackedPtrMap.clear()",
            "s_cachedResolverReady = false",
            "s_cachedBuildIncomplete = false",
            "s_trackedPtrMapMapEpoch = mapEpoch",
            "s_unitHandleCache.clear()",
            "s_unitHandleCacheMapEpoch = mapEpoch",
        ):
            self.assertIn(token, before_return)

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
            "ResetShadowCasterLifecycleMapEpoch(mapEpoch)", DEVICE
        )
        epoch_reset = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetGpuSkinMapEpoch()",
            "void D3D9DeviceEx::War3RequestShadowMapEpochReset()",
        )
        self.assertIn("&m_war3ShadowTombstoneSerialSeen", epoch_reset)

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

    def test_cpu_hot_caches_are_map_epoch_scoped(self) -> None:
        reset = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetShadowSessionState",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones",
        )
        self.assertIn("War3ResetDirectPacketMapCaches()", reset)

        direct_reset = body(
            DEVICE,
            "void War3ResetDirectPacketMapCaches()",
            "std::shared_ptr<const dxvk::war3::model::ShadowGeosetResourceRecord>\n"
            "War3FindDirectPacketGeosetResource",
        )
        self.assertIn(
            "War3DirectPacketGeosetResourceCache().clear()", direct_reset
        )
        self.assertIn("War3DirectPacketGeosetCacheMutex", direct_reset)

        # Fixed-size TLS caches must reject a recycled Warcraft pointer after
        # A -> B. The index-slice cache also releases its shared CPU vectors
        # lazily on the first Populate in the new map.
        for token in (
            "entry.mapEpoch == mapEpoch",
            "entry.mapEpoch = mapEpoch",
            "resetGenerationEntriesForMapEpoch",
            "generationEntries() = {}",
        ):
            self.assertIn(token, DEVICE)

    def test_semantic_pointer_and_pose_registries_reset_at_present(self) -> None:
        transition = body(
            DEVICE,
            "bool D3D9DeviceEx::War3ApplyShadowMapEpochResetAtPresent",
            "bool D3D9DeviceEx::War3ApplyShadowDeviceEpochTransitionAtPresent",
        )
        self.assertIn("War3ResetGpuSkinMapEpoch()", transition)
        self.assertNotIn("War3Renderer::instance().EndFrame()", transition)

        cpu_reset = body(
            DEVICE,
            "uint64_t D3D9DeviceEx::War3ResetCpuSemanticMapSession(",
            "D3D9DeviceEx::D3D9DeviceEx(",
        )
        self.assertIn("War3Renderer::instance().ResetMapSession()", cpu_reset)

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
            "shadowRetiredSessionEntryCount",
            "shadowRetiredSessionAllocatorBytes",
            "shadowRetiredSessionCachedGpuLogicalBytes",
            "shadowRetiredSessionCpuOwnedBytes",
            "shadowRetiredSessionOldestRetireSerial",
            "shadowRetiredSessionCollectedCount",
            "shadowRetiredLastMapEpoch",
        ):
            self.assertIn(f'"{field}"', DIAGNOSTICS)

    def test_retired_session_census_follows_fence_collection(self) -> None:
        reset = body(
            DEVICE,
            "void D3D9DeviceEx::War3ResetShadowSessionState",
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones",
        )
        for token in (
            "retired.entryCount",
            "retired.allocatorBytes",
            "retired.cachedGpuLogicalBytes",
            "retired.cpuOwnedBytes",
            "entry.ownedGpuBytes",
            "entry.logicalReferencedBytes",
            "War3RefreshRetiredShadowSessionDiagnostics()",
        ):
            self.assertIn(token, reset)

        collect = body(
            DEVICE,
            "void D3D9DeviceEx::War3CollectRetiredShadowSessions",
            "void D3D9DeviceEx::War3RefreshRetiredShadowSessionDiagnostics",
        )
        self.assertIn("session.retireSerial <= completedSerial", collect)
        self.assertIn("m_war3ShadowDiagRetiredSessionCollectedCount.fetch_add", collect)
        self.assertIn("War3RefreshRetiredShadowSessionDiagnostics()", collect)

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
