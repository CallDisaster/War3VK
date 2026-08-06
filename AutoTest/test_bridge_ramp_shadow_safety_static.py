"""Static contracts for the bridge/ramp shadow safety repair."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
COMMON_BUFFER_H = ROOT / "src/d3d9/d3d9_common_buffer.h"
SHADOW_CPP = ROOT / "src/d3d9/d3d9_war3_shadow.cpp"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
CONTROL_PLANE_CPP = ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp"
RUNTIME_BRIDGE_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp"
)
PRODUCER_POLICY_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.h"
)
PRODUCER_POLICY_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.cpp"
)
CURRENT_DRAW_CPP = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
)


class BridgeRampShadowSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.header = DEVICE_H.read_text(encoding="utf-8")
        cls.common_buffer = COMMON_BUFFER_H.read_text(encoding="utf-8")
        cls.shadow = SHADOW_CPP.read_text(encoding="utf-8")
        cls.monitor = MONITOR_CPP.read_text(encoding="utf-8")
        cls.control_plane = CONTROL_PLANE_CPP.read_text(encoding="utf-8")
        cls.runtime_bridge = RUNTIME_BRIDGE_CPP.read_text(encoding="utf-8")
        cls.producer_policy_h = PRODUCER_POLICY_H.read_text(encoding="utf-8")
        cls.producer_policy = PRODUCER_POLICY_CPP.read_text(encoding="utf-8")
        cls.current_draw = CURRENT_DRAW_CPP.read_text(encoding="utf-8")

    def test_incomplete_fingerprint_reuse_fails_closed(self) -> None:
        name = "DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE"
        self.assertIn(f'"{name}", 0u', self.device)
        self.assertIn(f'"{name}"', self.monitor)
        reuse = self.device.index(
            "War3DrawTimeSourceFingerprintReuseRuntime() &&"
        )
        break_call = self.device.index("break;", reuse)
        block = self.device[reuse:break_call]
        self.assertIn("sameFrame && fingerprintMatch", block)
        self.assertIn("staticCrossFrameReuse", block)

    def test_drawtime_snapshot_cache_uses_current_draw_slice_identity(self) -> None:
        self.assertIn("struct War3DrawTimeVBCacheKey", self.header)
        self.assertIn("void* instanceIdentity = nullptr;", self.header)
        self.assertIn("void* meshPayloadPtr = nullptr;", self.header)
        self.assertIn("void* renderablePart = nullptr;", self.header)
        self.assertIn("uint32_t jHandle = 0u;", self.header)
        self.assertIn("uint32_t layerIndex = 0u;", self.header)
        self.assertIn("uint32_t payloadWord108 = 0u;", self.header)
        self.assertIn("uint32_t payloadWord11C = 0u;", self.header)
        key_start = self.header.index("struct War3DrawTimeVBCacheKey")
        key_end = self.header.index("class D3D9InterfaceEx", key_start)
        key_block = self.header[key_start:key_end]
        self.assertGreaterEqual(key_block.count("instanceIdentity"), 3)
        self.assertGreaterEqual(key_block.count("meshPayloadPtr"), 3)
        self.assertGreaterEqual(key_block.count("jHandle"), 3)
        self.assertGreaterEqual(key_block.count("payloadWord108"), 3)
        self.assertGreaterEqual(key_block.count("payloadWord11C"), 3)
        self.assertIn(
            "std::unordered_map<War3DrawTimeVBCacheKey, War3DrawTimeVBEntry,",
            self.header,
        )

        capture = self.device.index("const auto drawDispatchContext =")
        cache_setup = self.device.index(
            "const War3DrawTimeVBCacheKey vbCacheKey =",
            capture,
        )
        capture_block = self.device[capture:cache_setup + 260]
        self.assertIn("drawDispatchContext.layerIndex", capture_block)
        self.assertIn("gpuSkinResolved->key.layerIndex", capture_block)
        self.assertIn("QueryCurrentDrawContract(", capture_block)
        self.assertIn("QueryCurrentDrawGeometryContract(", capture_block)
        self.assertIn("vbCacheContract.producerFreshThisFrame", capture_block)
        self.assertIn("vbCacheContract.fromGrace", capture_block)
        self.assertIn("vbCacheContract.producerStage != 11", capture_block)
        self.assertIn("vbCacheContract.renderFrameIndex", capture_block)
        self.assertIn("War3CurrentDrawContractNamesExactSlice(", capture_block)
        self.assertIn(
            "War3CurrentDrawContractMatchesSemanticInstance(", capture_block
        )
        self.assertIn(
            "if (!dispatchPartMatches && !gpuSkinLayerMatches)", capture_block
        )
        self.assertNotIn("vbCacheLayerIndex = 0", capture_block)
        self.assertIn("War3MakeDrawTimeVBCacheKey(", capture_block)
        self.assertIn(
            "drawTimeVBCacheRejectNoLayerContext++", capture_block
        )

        self.assertIn("kGeometryContractLedgerWays = 4u", self.current_draw)
        self.assertIn("g_currentDrawGeometryLedger", self.current_draw)
        self.assertIn("StoreCurrentDrawGeometryLedgerRecord(record)", self.current_draw)
        geometry_query = self.current_draw.index(
            "bool QueryCurrentDrawGeometryContract("
        )
        geometry_query_end = self.current_draw.index(
            "bool QueryCurrentDrawContractCapturedPalette(", geometry_query
        )
        geometry_query_block = self.current_draw[
            geometry_query:geometry_query_end
        ]
        self.assertIn("expectedRenderFrameIndex", geometry_query_block)
        self.assertIn("record.producerStage != 11", geometry_query_block)
        self.assertIn("record.fromGrace", geometry_query_block)
        self.assertIn(
            "CurrentDrawGeometryRecordMatchesIdentity(", geometry_query_block
        )

        consume = self.device.index("// key 与 CurrentDraw 的 canonical slice")
        consume_end = self.device.index(
            "drawTimeVBOverrideApplied = true;", consume
        )
        consume_block = self.device[consume:consume_end]
        self.assertIn("packet.renderable.layerIndex", consume_block)
        self.assertIn("authoritativeDrawContract", consume_block)
        self.assertIn("War3MakeDrawTimeVBCacheKey(", consume_block)
        self.assertIn("vbIt->second.MatchesKey(cacheKey)", consume_block)

        producer = self.device.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = self.device.index(
            "War3ShadowCasterDraw draw = {};", producer
        )
        producer_block = self.device[producer:producer_end]
        self.assertIn("queryByRenderablePartAndLayer(", producer_block)
        self.assertIn("cacheKey.layerIndex", producer_block)
        self.assertIn("entry.MatchesKey(cacheKey)", producer_block)
        self.assertIn(
            "War3VisibleRenderableMatchesDrawTimeKey(record, cacheKey)",
            producer_block,
        )

        setup = self.device.index("entry.instanceIdentity = vbCacheKey.instanceIdentity")
        setup_block = self.device[setup:setup + 300]
        self.assertIn("entry.meshPayloadPtr = vbCacheKey.meshPayloadPtr", setup_block)
        self.assertIn("entry.contractJHandle = vbCacheKey.jHandle", setup_block)

    def test_drawtime_cache_has_same_dll_fail_closed_gate(self) -> None:
        name = "DXVK_WAR3_DRAWTIME_VB_CACHE"
        self.assertIn(f'"{name}", 0u', self.device)
        self.assertIn(f'"{name}"', self.monitor)
        capture_gate = self.device.index(
            "const bool currentFrameStage11Geometry ="
        )
        capture_identity = self.device.index(
            "War3ShadowDrawTimeCapturePhase::IdentityResolve",
            capture_gate,
        )
        capture_gate_block = self.device[capture_gate:capture_identity]
        self.assertIn(
            "stage == 11 && War3DrawTimeCurrentFrameGeometryRuntime()",
            capture_gate_block,
        )
        self.assertIn(
            "if (!currentFrameStage11Geometry && !War3DrawTimeVBCacheRuntime())",
            capture_gate_block,
        )
        producer = self.device.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_block = self.device[producer:producer + 500]
        self.assertIn("War3DrawTimeCurrentFrameGeometryRuntime()", producer_block)
        self.assertIn("!War3DrawTimeVBCacheRuntime()", producer_block)

    def test_fast_append_preserves_gpu_skin_input_contract(self) -> None:
        start = self.device.index(
            "auto tryAppendDrawTimeFastEligible ="
        )
        end = self.device.index(
            "return finishFastAppend(FastAppendOutcome::Success, true);",
            start,
        )
        block = self.device[start:end]
        self.assertIn("draw.gpuSkinInput = entry.gpuSkinInput;", block)
        self.assertLess(
            block.index("draw.gpuSkinInput = entry.gpuSkinInput;"),
            block.index("m_war3Scene.shadowCasters.emplace_back"),
        )

    def test_unsafe_fast_append_defaults_fail_closed(self) -> None:
        fast = "DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND"
        prebuild = "DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS"
        self.assertIn(f'"{fast}", 0u', self.device)
        self.assertIn(f'"{prebuild}", 0u', self.device)
        self.assertIn(f'"{fast}"', self.monitor)
        self.assertIn(f'"{prebuild}"', self.monitor)

    def test_dynamic_drawtime_snapshots_are_current_frame_only(self) -> None:
        start = self.device.index(
            "// Dynamic/pre-skinned snapshots are exact-frame resources."
        )
        end = self.device.index("if (entryFresh)", start)
        block = self.device[start:end]
        self.assertIn(
            "vbIt->second.frameSerial == m_war3ShadowPersistentFrameSerial",
            block,
        )
        self.assertIn("vbIt->second.isStaticGeometry", block)
        self.assertIn("!vbIt->second.gpuSkinLeaseBacked", block)

    def test_position_copy_requires_current_capacity(self) -> None:
        start = self.device.index("// 分配/复用 device-local position buffer")
        end = self.device.index(
            "War3ShadowDrawTimeCapturePhase::UvBacking", start
        )
        block = self.device[start:end]
        guard = block.index("entry.positionCapacity < posBytes")
        copy = block.index("ctx->copyBuffer")
        self.assertLess(guard, copy)
        self.assertIn("entry.positionInfo = {};", block[:copy])

    def test_uv_and_index_copies_require_current_capacity(self) -> None:
        uv_start = self.device.index(
            "War3ShadowDrawTimeCapturePhase::UvBacking"
        )
        idx_start = self.device.index(
            "War3ShadowDrawTimeCapturePhase::IndexBacking", uv_start
        )
        uv_block = self.device[uv_start:idx_start]
        self.assertIn("entry.uvCapacity >= uvBytes", uv_block)
        self.assertLess(
            uv_block.index("entry.uvCapacity >= uvBytes"),
            uv_block.index("ctx->copyBuffer"),
        )

        idx_end = self.device.index(
            "War3ShadowDrawTimeCapturePhase::FinalizeAccounting", idx_start
        )
        idx_block = self.device[idx_start:idx_end]
        self.assertIn("entry.indexCapacity >= idxBytes", idx_block)
        self.assertLess(
            idx_block.index("entry.indexCapacity >= idxBytes"),
            idx_block.index("ctx->copyBuffer"),
        )

    def test_incomplete_index_capture_cannot_fall_back_to_nonindexed(self) -> None:
        self.assertIn("bool captureComplete = false;", self.header)
        self.assertIn("bool HasCompleteBacking() const", self.header)

        idx_start = self.device.index(
            "War3ShadowDrawTimeCapturePhase::IndexBacking"
        )
        finalize = self.device.index(
            "War3ShadowDrawTimeCapturePhase::FinalizeAccounting", idx_start
        )
        idx_block = self.device[idx_start:finalize]
        reject = idx_block.index("if (indexed && !idxOk)")
        complete = idx_block.index("entry.captureComplete = true;")
        self.assertLess(reject, complete)
        self.assertIn("entry.indexed = true;", idx_block[reject:complete])
        self.assertIn("entry.indexInfo = {};", idx_block[reject:complete])
        self.assertIn("entry.indexCount = 0u;", idx_block[reject:complete])
        self.assertIn("break;", idx_block[reject:complete])
        self.assertNotIn(
            "if (!idxOk) {\n          entry.indexed = false;",
            idx_block,
        )

        # Every draw-time consumer must pass the complete-backing contract.
        self.assertGreaterEqual(
            self.device.count(".HasCompleteBacking()"),
            5,
        )

    def test_shadow_taa_history_advances_only_after_receiver_draw(self) -> None:
        start = self.shadow.index(
            "const bool shadowHistoryWriteComplete ="
        )
        end = self.shadow.index(
            "reconciliation.shadowHistoryValidAfter", start
        )
        block = self.shadow[start:end]
        self.assertIn(
            "reconciliation.receiverDrawExecutedThisFrame != 0u",
            block,
        )
        self.assertIn(
            "reconciliation.shadowVisibilityExecutedThisFrame != 0u",
            block,
        )
        self.assertIn(
            "reconciliation.shadowMotionVectorExecutedThisFrame != 0u",
            block,
        )
        self.assertIn("shadowHistoryWriteExecuted", block)
        self.assertIn(
            "if (shadowHistoryWriteComplete)",
            block,
        )
        self.assertIn(
            "reconciliation.shadowHistoryAdvancedThisFrame = 1u;",
            block,
        )
        self.assertIn(
            "reconciliation.shadowHistoryAdvanceSkippedIncomplete = 1u;",
            block,
        )
        self.assertIn(
            "m_shadowTaaWasActiveLastFrame = shadowHistoryWriteComplete;",
            self.shadow[end:],
        )
        self.assertIn(
            '{"shadowHistoryAdvancedThisFrame",',
            self.control_plane,
        )
        self.assertIn(
            '{"shadowHistoryAdvanceSkippedIncomplete",',
            self.control_plane,
        )
        merge_start = self.runtime_bridge.find(
            "merged.semanticSceneShadowHistoryAdvancedThisFrame"
        )
        self.assertGreaterEqual(merge_start, 0)
        self.assertIn(
            "merged.semanticSceneShadowHistoryAdvanceSkippedIncomplete",
            self.runtime_bridge[merge_start:],
        )
        self.assertIn(
            "const bool missingTaaHistoryOutcome =",
            self.runtime_bridge,
        )

    def test_terminal_receiver_publication_survives_prepass_republish(self) -> None:
        self.assertIn(
            "NoteShadowSceneTerminalStats(stats);",
            self.shadow,
        )
        self.assertIn(
            "void NoteShadowSceneStatsImpl(const War3ShadowCaptureStats& stats,",
            self.runtime_bridge,
        )
        self.assertIn(
            "(!terminalReceiverPublication && g_shadowSceneTerminalPublished)",
            self.runtime_bridge,
        )
        self.assertIn(
            "if (terminalReceiverPublication)\n"
            "    g_shadowSceneTerminalPublished = true;",
            self.runtime_bridge,
        )
        reset_start = self.runtime_bridge.index(
            "void ResetShadowRuntimeBridgeState()"
        )
        reset_end = self.runtime_bridge.index(
            "bool AugmentShadowSemanticContext", reset_start
        )
        reset = self.runtime_bridge[reset_start:reset_end]
        self.assertIn("g_shadowSceneStats = {};", reset)
        self.assertIn("g_shadowSceneTerminalPublished = false;", reset)

    def test_static_s1_source_is_retained_without_pool_charge(self) -> None:
        name = "DXVK_WAR3_S1_PERSISTENT_BORROW_STATIC"
        self.assertIn(f'"{name}", 1u', self.device)
        self.assertIn(f'"{name}"', self.monitor)
        self.assertIn("bool retainSource = false;", self.header)
        self.assertIn("outStorage = upload.slice.buffer();", self.device)
        self.assertIn("if (!upload.retainSource)", self.device)
        self.assertIn(
            "uploads[0].retainSource = borrowS1StaticBacking;",
            self.device,
        )
        self.assertIn(
            "uploads[1].retainSource = borrowS1StaticBacking;",
            self.device,
        )

    def test_unstable_s1_sources_are_not_promoted_by_default(self) -> None:
        name = "DXVK_WAR3_S1_PERSISTENT_UNSTABLE_SOURCE"
        self.assertIn(f'"{name}", 0u', self.device)
        self.assertIn(f'"{name}"', self.monitor)
        start = self.device.index(
            "const bool s1StableBackingEligible ="
        )
        end = self.device.index(
            "const bool persistentEligible =", start
        )
        block = self.device[start:end]
        self.assertIn("!DynamicSysmemVBOs", self.device[start - 300:start])
        self.assertIn("!DynamicSysmemIBO && !ibDynamic", block)
        self.assertIn(
            "s1StableBackingEligible || s1UnstableBackingEligible",
            block,
        )
        self.assertIn(
            "War3S1PersistentUnstableSourceRuntime() &&",
            block,
        )

    def test_s1_early_hit_publishes_a_canonical_replay_record(self) -> None:
        start = self.device.index(
            "// BuildShadowReplayDraws does not consume bare shadowCasters"
        )
        end = self.device.index(
            "m_war3Scene.shadowStats.captured++;", start
        )
        block = self.device[start:end]

        # Persistent-backed entries restore the instance contract; fallback-
        # backed entries restore the snapshot contract. Both are published
        # before the compatibility caster and before the early return.
        self.assertIn(
            "if (earlyIt->second.persistentGeometryId != 0u)", block
        )
        self.assertIn(
            "m_war3Scene.shadowInstances.emplace_back", block
        )
        self.assertIn(
            '"s1-early-cache"', block
        )
        self.assertIn(
            "m_war3Scene.shadowFallbacks.push_back", block
        )
        caster_publish = block.index(
            "m_war3Scene.shadowCasters.emplace_back"
        )
        self.assertLess(
            block.index("m_war3Scene.shadowInstances.emplace_back"),
            caster_publish,
        )
        self.assertLess(
            block.index("m_war3Scene.shadowFallbacks.push_back"),
            caster_publish,
        )
        self.assertLess(
            caster_publish,
            block.index("s1EarlyReplayPublishedCount++"),
        )

    def test_s1_early_replay_publication_has_frame_closure_telemetry(self) -> None:
        for field in (
            "s1EarlyAcceptedHitCount",
            "s1EarlyReplayPublishedCount",
            "s1EarlyReplayInstanceCount",
            "s1EarlyReplayFallbackCount",
            "s1EarlyReplayClosureMismatch",
        ):
            self.assertIn(field, self.header + self.monitor)

        self.assertIn(
            "completed.s1EarlyAcceptedHitCount !=",
            self.device,
        )
        self.assertIn(
            "persistentS1EarlyReplayClosureMismatchFrames",
            self.monitor,
        )
        self.assertIn(
            "s1EarlyReplayClosureMismatch",
            self.monitor,
        )

    def test_s1_early_fallback_backing_is_fail_closed_by_default(self) -> None:
        name = "DXVK_WAR3_S1_EARLY_FALLBACK_BACKING"
        self.assertIn(f'"{name}", 0u', self.device)
        self.assertIn(f'"{name}"', self.monitor)

        store_start = self.device.index(
            "void D3D9DeviceEx::War3StoreS1TerrainEarlyCacheEntry("
        )
        store_insert = self.device.index(
            "m_war3S1TerrainEarlyCache.try_emplace", store_start
        )
        store_guard = self.device[store_start:store_insert]
        self.assertIn("persistentGeometryId == 0u", store_guard)
        self.assertIn("!War3S1EarlyFallbackBackingRuntime()", store_guard)
        self.assertIn("return;", store_guard)

        hit_start = self.device.index(
            "auto earlyIt = m_war3S1TerrainEarlyCache.find"
        )
        hit_publish = self.device.index(
            "// BuildShadowReplayDraws does not consume bare shadowCasters",
            hit_start,
        )
        hit_guard = self.device[hit_start:hit_publish]
        self.assertIn("persistentGeometryId == 0u", hit_guard)
        self.assertIn("War3EraseS1TerrainEarlyCacheEntry", hit_guard)
        self.assertIn("backingValid = false;", hit_guard)

        fallback_store = self.device.index(
            "War3StoreS1TerrainEarlyCacheEntry(\n        s1EarlyKey, draw, 0u,"
        )
        fallback_guard = self.device[fallback_store - 500:fallback_store]
        self.assertIn(
            "War3S1EarlyFallbackBackingRuntime() &&",
            fallback_guard,
        )

    def test_s1_early_hit_validates_source_fingerprint(self) -> None:
        # early key 只含 worldMatrix+几何规模，identity world + 相同顶点数的
        # 不同 tile 会 key 碰撞。命中路径必须先比对冻结时的源身份指纹，
        # 不匹配即淘汰（fail-closed），绝不重放异源冻结几何。
        hit_start = self.device.index(
            "auto earlyIt = m_war3S1TerrainEarlyCache.find"
        )
        hit_publish = self.device.index(
            "// BuildShadowReplayDraws does not consume bare shadowCasters",
            hit_start,
        )
        hit_guard = self.device[hit_start:hit_publish]
        self.assertIn(
            "sourceFingerprint != s1SourceFingerprint", hit_guard
        )
        self.assertIn("s1EarlySourceMismatchEvictCount", hit_guard)
        # 指纹必须在查找之前从当前 draw 状态计算，两个 store 站点都必须记录。
        self.assertIn(
            "War3ComputeS1TerrainSourceFingerprint(indexed, BaseVertexIndex,",
            self.device,
        )
        self.assertIn("entry.sourceFingerprint = sourceFingerprint;", self.device)
        self.assertIn("s1EarlySourceMismatchEvictCount", self.monitor)

    def test_stage13_world_objects_have_one_continuous_shadow_owner(self) -> None:
        name = "DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE"
        self.assertIn(f'"{name}"', self.producer_policy)
        self.assertIn(
            "IsStage13ImmediateLegacyShadowOwnerEnabled()",
            self.device,
        )

        # The semantic early-return must not consume Stage13 when its immediate
        # same-draw capture lane owns shadow publication.
        semantic_gate = self.device.index(
            "IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled() &&"
        )
        legacy_gate = self.device.index(
            "War3ShadowCaptureGatePhase::LegacyGate", semantic_gate
        )
        self.assertIn(
            "!keepStage13WorldObjectLegacyCapture",
            self.device[semantic_gate:legacy_gate],
        )

        # Stage13 is explicitly classified and allowed through the VS caster
        # gate; relying on a transient TLS batch tag caused the old flicker.
        classify = self.device.index(
            "const bool objectCasterByStage =", legacy_gate
        )
        considered = self.device.index(
            "stage13CaptureConsideredCount++", classify
        )
        classify_block = self.device[classify:considered]
        self.assertIn("stage == 13", classify_block)
        self.assertNotIn("stage == 12", classify_block)
        vertex_gate = self.device.index(
            "const bool allowVertexShaderCaster =", considered
        )
        vertex_gate_end = self.device.index(
            "const bool allowRuntimePoseFallbackCaster", vertex_gate
        )
        self.assertIn(
            "keepStage13WorldObjectLegacyCapture",
            self.device[vertex_gate:vertex_gate_end],
        )

        # DirectGrouped, packet-build and append all share one policy instead
        # of repeating local Stage13 conditionals.
        self.assertGreaterEqual(
            self.device.count(
                "ShadowProducerKind::SemanticDirectGrouped"
            ),
            4,
        )
        self.assertIn(
            "RejectStage13OwnedByImmediateLegacy",
            self.producer_policy,
        )

    def test_stage13_cost_ledger_is_complete_and_safe_defaults_stay_off(
        self,
    ) -> None:
        for unsafe_switch in (
            "DXVK_WAR3_STAGE13_STATIC_RETENTION",
            "DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE",
            "DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE",
        ):
            self.assertIn(f'"{unsafe_switch}", 0u', self.device)

        # The owner decision precedes semantic-context construction and all
        # retained-source scans. CurrentDraw independently asks the same policy
        # before it materializes/publishes a contract.
        owner = self.device.index(
            "IsStage13ImmediateLegacyShadowOwnerEnabled()"
        )
        semantic_context = self.device.index(
            "BuildShadowSemanticContext", owner
        )
        retention = self.device.index(
            "const bool stage13BaseRetentionEligible =", owner
        )
        self.assertLess(owner, semantic_context)
        self.assertLess(owner, retention)
        self.assertIn(
            "ShadowProducerKind::CurrentDrawContract",
            self.current_draw,
        )

        for field in (
            "stage13CaptureAttemptCount",
            "stage13CaptureConsideredCount",
            "stage13FreezeCopyBytes",
            "stage13CpuSnapshotCopyBytes",
            "stage13RetentionSnapshotBytes",
            "stage13ReplayDrawCount",
        ):
            self.assertIn(field, self.device + self.monitor)
            self.assertIn(field, self.control_plane)

    def test_s12_never_publishes_shadow_side_contracts(self) -> None:
        self.assertIn("context.stage == 12", self.producer_policy)
        self.assertIn(
            "War3BatchTag::RangeIndicatorTarget",
            self.producer_policy,
        )
        self.assertNotIn(
            "stage == 11 || stage == 12 || stage == 13",
            self.current_draw,
        )
        self.assertIn(
            "ShadowProducerKind::CurrentDrawContract",
            self.current_draw,
        )
        self.assertIn(
            "ShadowProducerKind::DrawTimePose",
            self.device,
        )
        self.assertIn(
            "ShadowProducerKind::ImmediateLegacyCapture",
            self.device,
        )

    def test_alpha_payload_is_complete_before_publication(self) -> None:
        self.assertIn(
            "RejectIncompleteAlphaPayload",
            self.producer_policy,
        )
        alpha_policy = self.device.index(
            "ShadowProducerKind::\n"
            "                        ImmediateLegacyAlphaPayload"
        )
        insert = self.device.index("InsertPayloadLocked(", alpha_policy)
        self.assertIn(
            "const bool alphaPayloadComplete = payload.valid();",
            self.device[alpha_policy - 700:insert],
        )

    def test_stage13_retention_uses_bounded_cpu_snapshot_and_frame_arena(
        self,
    ) -> None:
        self.assertIn(
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION", 0u',
            self.device,
        )
        self.assertIn(
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION_FRAMES", 240u',
            self.device,
        )
        self.assertIn(
            '"DXVK_WAR3_STAGE13_STATIC_RETENTION_CAP", 64u',
            self.device,
        )
        self.assertIn(
            '"DXVK_WAR3_STAGE13_COMPONENT_DIAGNOSTICS", 0u',
            self.device,
        )
        base_gate = self.device.index(
            "const bool stage13BaseRetentionEligible ="
        )
        cpu_gate = self.device.index(
            "const bool stage13CpuRetentionPath =", base_gate
        )
        persistent_gate = self.device.index(
            "const bool persistentEligible =", cpu_gate
        )
        gate_end = self.device.index(
            "const uint32_t stage13PositionElementSize", base_gate
        )
        gate = self.device[base_gate:gate_end]
        self.assertIn("replayMode == War3ShadowReplayMode::FixedWorld", gate)
        self.assertIn("!skinnedOrPoseDrivenGeometry", gate)
        self.assertIn("isStage13WorldObjectDraw && indexed", gate)
        self.assertIn(
            "stage13FastIdentityHit ||",
            self.device[cpu_gate:persistent_gate],
        )
        persistent_end = self.device.index(
            "if (!persistentEligible", persistent_gate
        )
        self.assertNotIn(
            "stage13CpuRetentionPath",
            self.device[persistent_gate:persistent_end],
        )
        self.assertIn("stage13MappedIndexEligible", self.device)
        self.assertIn("stage13IndexRangeBytes", self.device)
        self.assertIn("stage13IndexRangeInBounds", self.device)
        self.assertIn(
            "kStage13MaxRetainedSnapshotBytes = 1u << 20",
            self.device,
        )
        self.assertIn(
            '"DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY", 0u',
            self.device,
        )
        self.assertIn(
            "const bool stage13SourceGenerationIdentityValid =",
            self.device,
        )
        self.assertIn(
            "m_war3PerDrawUpload.vbSourceContentGeneration[i] =",
            self.device,
        )
        self.assertIn(
            "m_war3PerDrawUpload.ibSourceContentGeneration",
            self.device,
        )
        self.assertIn(
            "War3ContentGeneration() const",
            self.common_buffer,
        )
        self.assertIn(
            "War3MarkContentMutation()",
            self.common_buffer,
        )
        self.assertIn(
            "pResource->War3MarkContentMutation();",
            self.device,
        )
        self.assertIn(
            "!stage13FastIdentityHit ||",
            self.device,
        )
        self.assertIn(
            "stage13RetainedIt->second.contentHash !=",
            self.device,
        )
        self.assertIn(
            "for (uint32_t indexOrdinal = 0u; indexOrdinal < CountVal;",
            self.device,
        )
        self.assertIn(
            "int64_t(rawIndex) + int64_t(BaseVertexIndex)",
            self.device,
        )
        self.assertIn(
            "GetDecltypeSize(declInfo.posType)",
            self.device,
        )
        self.assertNotIn("posSlice.mapPtr(0u)", self.device)
        self.assertIn(
            "BuildWar3CpuReadableBufferSpan({",
            self.device,
        )
        self.assertIn(
            "stage13PositionMappedAllocation->getBufferInfo()",
            self.device,
        )
        self.assertIn(
            "stage13PositionReadableSpan.data",
            self.device,
        )
        self.assertIn(
            "positionBytes + vertexByteOffset +",
            self.device,
        )
        referenced_begin = self.device.index(
            "const auto computeStage13ReferencedContentHash ="
        )
        referenced_end = self.device.index(
            "const bool stage13CanonicalReferencedContentHashValid",
            referenced_begin,
        )
        referenced_block = self.device[referenced_begin:referenced_end]
        self.assertNotIn(
            "fnv1a_iter(positionHash, rawIndex)",
            referenced_block,
        )
        self.assertNotIn(
            "positionHash, uint32_t(BaseVertexIndex)",
            referenced_block,
        )
        self.assertIn(
            "kStage13ReferencedContentTag = 0x53314301u",
            self.device,
        )
        self.assertIn(
            "const auto* stage13CanonicalPositionBytes =",
            self.device,
        )
        for token in (
            "s_stage13UniqueVertices.push_back(vertexOrdinal);",
            "War3ShadowStage13StrongPhase::IndexParse",
            "War3ShadowStage13StrongPhase::UniqueLeafRead",
            "War3ShadowStage13StrongPhase::HashReplay",
            '"DXVK_WAR3_STAGE13_SORT_UNIQUE_READS", 0u',
            "std::sort(s_stage13UniqueVertices.begin(),",
            '"DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE", 0u',
            '"DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE", 0u',
            '"DXVK_WAR3_STAGE13_LATE_SAMPLE_COUNT", 32u',
            '"DXVK_WAR3_STAGE13_LATE_FULL_INDEX_FINGERPRINT", 1u',
            "stage13LateSampleLimit",
            "stage13UniqueSemanticHit",
            "stage13LateDescriptorIndexBytes",
            "stage13LateDescriptorPositionBytes",
            "War3ShadowStage13SourcePhase::IndexFingerprint",
            "War3ShadowStage13SourcePhase::SampleLeaves",
            "War3ShadowStage13SourcePhase::RetainedLookup",
            '"Stage13StrongIdentity/SourceMapAndSetup"',
            "const bool stage13FastIdentityHit =",
            '"ShadowCapture/PostGate/PersistentLookup/"',
            '"Stage13StrongIdentity"',
        ):
            self.assertIn(token, self.device)
        self.assertIn(
            "stage13CanonicalReferencedContentHash",
            self.device,
        )
        self.assertIn(
            "Stage13 referenced-set hash verifier mismatch",
            self.device,
        )
        self.assertIn(
            "stage13RetentionPositionBytes.resize(",
            self.device,
        )
        self.assertIn(
            "stage13RetentionPositionBytes.data() +",
            self.device,
        )
        snapshot_begin = self.device.index(
            "stage13RetentionPositionBytes.resize("
        )
        snapshot_end = self.device.index(
            "const bool persistentEligible =", snapshot_begin
        )
        snapshot_block = self.device[snapshot_begin:snapshot_end]
        self.assertIn(
            "s_stage13ExpandedIndices[indexOrdinal]",
            snapshot_block,
        )
        self.assertNotIn("uint32_t rawIndex", snapshot_block)
        self.assertIn(
            "stage13SnapshotContentRekeyCount++",
            snapshot_block,
        )
        self.assertNotIn(
            "bit::fnv1a_hash(stage13MappedPositionBytes,",
            self.device,
        )

        # The retained template is written only after the normal fallback draw
        # has passed finalization. All frame-owned Rc backing is stripped.
        finalize = self.device.index(
            "if (!finalizeShadowDrawCommon(draw))",
            cpu_gate,
        )
        retain = self.device.index(
            "m_war3Stage13RetainedCasters.find(stage13RetentionKey)",
            finalize,
        )
        submit = self.device.index(
            "m_war3Scene.shadowFallbacks.push_back(",
            retain,
        )
        self.assertLess(finalize, retain)
        self.assertLess(retain, submit)
        self.assertIn(
            "retained.positionBytes =",
            self.device[retain:submit],
        )
        self.assertIn(
            "retained.contentHash =",
            self.device[retain:submit],
        )
        self.assertIn(
            "retainedDraw.positionStorage = nullptr;",
            self.device[finalize:retain],
        )
        self.assertIn(
            "retainedDraw.indexed = false;",
            self.device[finalize:retain],
        )
        self.assertIn(
            "std::vector<unsigned char> positionBytes",
            self.header,
        )
        self.assertNotIn("uint32_t geometryId", self.header[
            self.header.index("struct War3Stage13RetainedCasterEntry"):
            self.header.index("using War3Stage13RetainedCasterMap")
        ])

        # BeforeUi skips current-frame entries, drains stale entries, and
        # clears across invalid-camera loading transitions. Replay backing is
        # allocated only from the current frame's shared host-mapped arena.
        replay = self.device.index(
            "// Native Stage13 visibility is receiver-view based"
        )
        replay_end = self.device.index(
            "War3UpdateSemanticReplayInputDiagnostics", replay
        )
        replay_block = self.device[replay:replay_end]
        self.assertIn("const bool hasUsableCamera =", replay_block)
        self.assertIn(
            "War3WorldCameraIsFreshForFrame(m_war3LastGoodCamera",
            replay_block,
        )
        self.assertIn("age > maxAge", replay_block)
        self.assertIn(
            "retained.lastSeenFrame != currentFrame",
            replay_block,
        )
        self.assertIn(
            "War3AllocFreezeBuffer(",
            replay_block,
        )
        self.assertIn(
            "replayBytes, replayOffset, true, &replayMapPtr",
            replay_block,
        )
        self.assertIn(
            "std::memcpy(replayMapPtr, retained.positionBytes.data()",
            replay_block,
        )
        self.assertIn(
            "m_war3Scene.shadowCasters.emplace_back(std::move(replay))",
            replay_block,
        )


if __name__ == "__main__":
    unittest.main()
