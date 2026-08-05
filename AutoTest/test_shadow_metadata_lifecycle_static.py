"""Static contracts for the current-frame shadow metadata/lifecycle repair."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
METADATA_H = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_alpha_test_payload.h"
).read_text(encoding="utf-8")
METADATA_CPP = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_alpha_test_payload.cpp"
).read_text(encoding="utf-8")
MONITOR = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")
BRIDGE = (
    ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp"
).read_text(encoding="utf-8")
CURRENT_DRAW = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
).read_text(encoding="utf-8")
CURRENT_DRAW_H = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.h"
).read_text(encoding="utf-8")
ANALYZER = (
    ROOT / "AutoTest/analyze_shadow_final_caster_trace.py"
).read_text(encoding="utf-8")


class ShadowMetadataLifecycleStaticTests(unittest.TestCase):
    def test_unsafe_geometry_shortcuts_stay_fail_closed(self) -> None:
        defaults = (
            'War3GetEnvU32("DXVK_WAR3_DRAWTIME_VB_CACHE", 0u)',
            'War3GetEnvU32("DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND", 0u)',
            'War3GetEnvU32("DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS", 0u)',
        )
        for default in defaults:
            self.assertIn(default, DEVICE)
        self.assertIn(
            "if (War3DrawTimeVBCacheRuntime() && captureAlphaTest &&",
            DEVICE,
        )

    def test_safe_current_frame_geometry_is_independent_and_reported(self) -> None:
        name = "DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY"
        self.assertIn(f'War3GetEnvU32("{name}", 1u)', DEVICE)
        self.assertIn(f'"{name}"', MONITOR)
        self.assertIn(
            "stage == 11 && War3DrawTimeCurrentFrameGeometryRuntime()",
            DEVICE,
        )
        capture = DEVICE.index("const auto drawDispatchContext =")
        capture_end = DEVICE.index(
            "const War3DrawTimeVBCacheKey vbCacheKey =", capture
        )
        capture_block = DEVICE[capture:capture_end]
        for contract in (
            "QueryCurrentDrawGeometryContract(",
            "vbCacheContract.producerFreshThisFrame",
            "vbCacheContract.fromGrace",
            "vbCacheContract.producerStage != 11",
            "vbCacheContract.renderFrameIndex",
            "War3CurrentDrawContractMatchesSemanticInstance(",
            "if (!dispatchPartMatches && !gpuSkinLayerMatches)",
        ):
            self.assertIn(contract, capture_block, contract)
        self.assertNotIn("vbCacheLayerIndex = 0", capture_block)
        producer = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = DEVICE.index("War3ShadowCasterDraw draw = {};", producer)
        producer_block = DEVICE[producer:producer_end]
        self.assertIn("War3DrawTimeCurrentFrameGeometryRuntime()", producer_block)
        self.assertIn(
            "entry.frameSerial != m_war3ShadowPersistentFrameSerial",
            producer_block,
        )
        draw_start = DEVICE.index("War3ShadowCasterDraw draw = {};", producer)
        draw_end = DEVICE.index("draw.indexed = entry.indexed;", draw_start)
        draw_contract = DEVICE[draw_start:draw_end]
        for field in (
            "draw.shadowRenderablePart = renderablePart;",
            "draw.shadowLayerIndex = cacheKey.layerIndex;",
            "War3DrawTimeVBCacheKeyHash{}(cacheKey)",
            "draw.alphaMetadataFrameSerial =",
            "entry.frameSerial",
            "War3ShadowPartLifecycleState::RequiredCurrent",
            "entry.HasCompleteAlphaPayload()",
        ):
            self.assertIn(field, draw_contract, field)

    def test_stage11_exact_owner_precedes_and_suppresses_grouped_fallback(self) -> None:
        direct_only = DEVICE.index("if (War3SemanticDirectOnlyRuntime())")
        exact_call = DEVICE.index(
            "War3TryPopulateDrawTimeSemanticProducer();", direct_only
        )
        grouped_call = DEVICE.index(
            "War3TryPopulateDirectCurrentDrawGrouped(", exact_call
        )
        self.assertLess(exact_call, grouped_call)
        self.assertIn("uint64_t exactOwnerFrameSerial = 0u;", DEVICE_H)

        producer = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = DEVICE.index(
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()", producer
        )
        producer_block = DEVICE[producer:producer_end]
        claim = producer_block.index(
            "entry.exactOwnerFrameSerial = m_war3ShadowPersistentFrameSerial;"
        )
        blocker = producer_block.index(
            "dxvk::war3::internal::kPathBlockerHideEnabled", claim
        )
        alpha = producer_block.index(
            "entry.alphaBlendEnabled && !entry.alphaTestEnabled", blocker
        )
        self.assertLess(claim, blocker)
        self.assertLess(claim, alpha)
        self.assertEqual(
            DEVICE.count(
                "entry.exactOwnerFrameSerial = "
                "m_war3ShadowPersistentFrameSerial;"
            ),
            1,
        )

        grouped = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        grouped_end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
            grouped,
        )
        grouped_block = DEVICE[grouped:grouped_end]
        self.assertIn("currentFrameDrawTimeProducerOwnsRecord", grouped_block)
        self.assertGreaterEqual(
            grouped_block.count("currentFrameDrawTimeProducerOwnsRecord(record)"),
            2,
        )
        self.assertIn("Defensive final ownership gate", DEVICE)

    def test_exact_submission_refreshes_manifest_selection_and_core_without_lease(self) -> None:
        self.assertIn("uint64_t exactSubmittedFrameSerial = 0u;", DEVICE_H)
        producer = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = DEVICE.index(
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()", producer
        )
        producer_block = DEVICE[producer:producer_end]
        publish = producer_block.index(
            "m_war3Scene.shadowCasters.emplace_back(std::move(draw))"
        )
        submitted_marker = producer_block.index(
            "entry.exactSubmittedFrameSerial = "
            "m_war3ShadowPersistentFrameSerial;"
        )
        self.assertLess(publish, submitted_marker)

        grouped = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        grouped_end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
            grouped,
        )
        grouped_block = DEVICE[grouped:grouped_end]
        exact_records = grouped_block.index("exactSubmittedManifestRecords")
        manifest_publish = grouped_block.index(
            "publishShadowManifestSummary(shadowEligibleManifestRecords)"
        )
        self.assertLess(exact_records, manifest_publish)
        for contract in (
            "for (const auto& [cacheKey, entry] : m_war3DrawTimeVBCache)",
            "entry.exactSubmittedFrameSerial",
            "exactRecord.producerFreshThisFrame = true",
            "ShadowProducerKind::DrawTimeGeometry",
            "shadowEligibleManifestRecords.insert(",
            "for (const auto& exactRecord : exactSubmittedManifestRecords)",
            "currentPartKeys.insert(partKey)",
            "eligibleRecordCount == 0u && exactSubmittedManifestRecords.empty()",
            "drawTimeSemanticProducerLifecycleMergedCount++",
            "liveSubmittedCorePartsByObject[selectionKey].push_back(",
            "exactCorePartCount",
        ):
            self.assertIn(contract, grouped_block, contract)

        merge = grouped_block.index(
            "The exact producer already published these casters"
        )
        merge_end = grouped_block.index(
            "IsReadableRangeFast", merge
        )
        self.assertNotIn(
            "submittedPartPacketLeaseRecords.push_back", grouped_block[merge:merge_end]
        )

        direct_only = DEVICE.index("if (War3SemanticDirectOnlyRuntime())")
        direct_only_end = DEVICE.index(
            "const bool visibleFrameAheadOfLastCapture", direct_only
        )
        direct_only_block = DEVICE[direct_only:direct_only_end]
        self.assertIn("drawTimeClaimedBefore", direct_only_block)
        self.assertIn(
            "drawTimeSemanticProducerClaimedCount ==", direct_only_block
        )

        for counter in (
            "drawTimeSemanticProducerClaimedCount",
            "drawTimeSemanticProducerSubmittedCount",
            "drawTimeSemanticProducerOwnedDirectGroupedSkipCount",
            "drawTimeSemanticProducerLifecycleMergedCount",
        ):
            self.assertIn(counter, BRIDGE)
            self.assertIn(counter, ANALYZER)

    def test_static_world_cards_use_exact_native_owner_not_unit_skinning(self) -> None:
        producer = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = DEVICE.index(
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()", producer
        )
        producer_block = DEVICE[producer:producer_end]
        for token in (
            "staticWorldCasterRawcode",
            "War3SemanticRawcodeLooksStaticWorldCaster(exactOwnerRawcode)",
            "visibleRecordExact ? record.identity.rawcode : 0u",
            "objectKind = dxvk::war3::render::ObjectKind::Destructible",
            "exactNativeObjectSupported",
            "semanticSceneSubmittedDestructible++",
            "semanticSceneSubmittedCutout++",
        ):
            self.assertIn(token, producer_block, token)

        grouped = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped("
        )
        grouped_end = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(",
            grouped,
        )
        grouped_block = DEVICE[grouped:grouped_end]
        reject = grouped_block.index(
            "LT/YT world objects are known to masquerade as Unit"
        )
        units_only = grouped_block.index(
            "if (unitsOnly && !drawTimePrebuildBypassed)", reject
        )
        self.assertLess(reject, units_only)

    def test_buildings_use_exact_current_frame_owner(self) -> None:
        producer = DEVICE.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer()"
        )
        producer_end = DEVICE.index(
            "bool D3D9DeviceEx::War3DrainShadowCasterTombstones()", producer
        )
        producer_block = DEVICE[producer:producer_end]
        support = producer_block.index("const bool exactNativeObjectSupported")
        reject = producer_block.index(
            "if (!exactNativeObjectSupported && !drawTimeEntryIsPathBlocker)"
        )
        building_support = producer_block.index(
            "objectKind == dxvk::war3::render::ObjectKind::Building", support
        )
        self.assertLess(support, building_support)
        self.assertLess(building_support, reject)
        self.assertIn(
            "m_war3Scene.shadowStats.semanticSceneSubmittedBuilding++;",
            producer_block,
        )

    def test_geometry_ledger_identity_material_and_blocker_are_contract_first(self) -> None:
        metadata = DEVICE.index(
            "bool D3D9DeviceEx::War3CaptureShadowDrawMetadata("
        )
        metadata_end = DEVICE.index(
            "void D3D9DeviceEx::War3TryCaptureShadowCaster(", metadata
        )
        metadata_block = DEVICE[metadata:metadata_end]
        ledger = metadata_block.index("QueryCurrentDrawGeometryContract(")
        legacy = metadata_block.index("QueryCurrentDrawContract(", ledger)
        self.assertLess(ledger, legacy)
        for freshness_contract in (
            "legacyContractHit",
            "!contract.known",
            "contract.renderFrameIndex != currentRenderFrameIndex",
            "contract.producerStage != 11",
            "!contract.producerFreshThisFrame",
            "contract.fromGrace",
        ):
            self.assertIn(freshness_contract, metadata_block)
        self.assertIn("contract.pathBlocker", metadata_block)
        self.assertIn("contract.worldObjectEntry", metadata_block)
        self.assertIn("contract.unitPtr", metadata_block)

        material = DEVICE.index(
            "bool War3TryBuildCurrentDrawRecordMaterialSignature("
        )
        material_end = DEVICE.index(
            "bool War3CurrentDrawRecordIsUnsafeAlphaCaster(", material
        )
        material_block = DEVICE[material:material_end]
        self.assertIn("visibleMatchesMaterialInstance", material_block)
        for identity in (
            "record.sceneNode",
            "record.worldObjectEntry",
            "record.unitPtr",
            "recordHandle",
        ):
            self.assertIn(identity, material_block)
        self.assertIn(
            "candidate.meshData != record.meshPayloadPtr", material_block
        )
        self.assertNotIn(
            "queryByPayload(record.meshPayloadPtr", material_block
        )

    def test_only_exact_contract_backed_units_bypass_anonymous_blocker_heuristics(self) -> None:
        for source, helper in (
            (DEVICE, "War3CasterIsAnonymousSmallPathBlockerMarker"),
            (SHADOW, "War3ReplayDrawIsAnonymousStage11Marker"),
            (SHADOW, "War3ReplayDrawIsAnonymousSmallMarker"),
        ):
            start = source.index(helper)
            block = source[start : start + 2200]
            self.assertIn("exactCurrentDrawContractBacked", block)
            self.assertIn("War3ShadowPartLifecycleState::RequiredCurrent", block)
            exact_start = block.index("const bool exactCurrentDrawContractBacked")
            exact_end = block.index("if (exactCurrentDrawContractBacked)", exact_start)
            self.assertIn("ObjectKind::Unit", block[exact_start:exact_end])
            self.assertIn("return false;", block)

    def test_leased_skinned_pose_requires_actual_current_frame_rebuild(self) -> None:
        self.assertIn(
            '"DXVK_WAR3_SEMANTIC_MANIFEST_CORE_STALE_POSE_ONE_FRAME_RESTORE",\n'
            "          0u",
            DEVICE,
        )
        lease = DEVICE.index(
            "An exact current-frame Stage11 decision outranks every historical"
        )
        lease_end = DEVICE.index(
            'enterBuildEligibleLeasePhase("LeaseMerge")', lease
        )
        lease_block = DEVICE[lease:lease_end]
        self.assertIn("QueryCurrentDrawGeometryContract(", lease_block)
        self.assertIn("exactOwnerFrameSerial", lease_block)
        self.assertIn(
            "paletteFreshenedFromProducer || poseFreshenedFromCModel",
            lease_block,
        )
        safety = lease_block.index("const bool poseFreshForLease")
        safety_end = lease_block.index("const bool allowStalePoseForCore", safety)
        self.assertNotIn("leaseInfo.poseFresh", lease_block[safety:safety_end])

    def test_lease_palette_refresh_requires_exact_current_frame_tag(self) -> None:
        refresh = DEVICE.index("auto tryRefreshLeasedPaletteFromProducerFacts")
        refresh_end = DEVICE.index("auto tryFreshenLeasedPoseFromCModel", refresh)
        refresh_block = DEVICE[refresh:refresh_end]
        proof = refresh_block.index("producerPaletteCurrentFrameProven")
        apply_palette = refresh_block.index(
            "leased.packet.runtimeGroupPalette =", proof
        )
        proof_block = refresh_block[proof:apply_palette]
        for contract in (
            "QueryCurrentPaletteFrameTag(",
            "currentPaletteFrameTag != 0u",
            "livePaletteMinFrameTag != 0u",
            "livePaletteMinFrameTag == livePaletteMaxFrameTag",
            "livePaletteMinFrameTag == currentPaletteFrameTag",
            "!producerPaletteCurrentFrameProven",
            "semanticSceneShadowManifestPartLeasePaletteRefreshMissCount++",
            "return false;",
        ):
            self.assertIn(contract, proof_block, contract)

    def test_unsafe_drawtime_consumers_still_require_legacy_master(self) -> None:
        self.assertIn(
            "War3DrawTimeVBCacheRuntime() && geometry->vertexBlendEnabled",
            DEVICE,
        )
        self.assertIn(
            "const auto cached = War3DrawTimeVBCacheRuntime() && exactLogicalSlice",
            DEVICE,
        )
        self.assertIn(
            "if (!War3DrawTimeVBCacheRuntime() ||\n"
            "        !War3SemanticDrawTimePrebuildBypassRuntime()",
            DEVICE,
        )
        self.assertIn(
            "if (!War3DrawTimeVBCacheRuntime() ||\n"
            "        !War3SemanticDrawTimeFastAppendRuntime())",
            DEVICE,
        )

    def test_metadata_runtime_gates_default_on_and_are_reported(self) -> None:
        for name in (
            "DXVK_WAR3_SHADOW_METADATA_CAPTURE",
            "DXVK_WAR3_SHADOW_METADATA_ALPHA",
            "DXVK_WAR3_SHADOW_METADATA_BLOCKER",
        ):
            self.assertIn(f'War3GetEnvU32("{name}", 1u)', DEVICE)
            self.assertIn(f'"{name}"', MONITOR)
        for field in (
            "shadowMetadataClassifiedCount",
            "shadowMetadataCapturedCount",
            "shadowMetadataAppliedCount",
            "shadowMetadataRejectedNoMaterialCount",
            "shadowMetadataRejectedOpaqueCount",
            "shadowMetadataRejectedNoUvCount",
            "shadowMetadataRejectedNoDiffuseCount",
            "shadowMetadataRejectedUploadCount",
            "shadowMetadataRejectedDuplicateCount",
            "shadowMetadataRejectedByReasonCount",
        ):
            self.assertEqual(MONITOR.count(f'\\"{field}\\"'), 2, field)

    def test_stage11_capture_precedes_semantic_early_return(self) -> None:
        capture = DEVICE.index(
            "const bool metadataRejectedBlocker = War3CaptureShadowDrawMetadata("
        )
        early_gate = DEVICE.index("earlySemanticSceneUnitLikeCandidate", capture)
        early_return = DEVICE.index("return;", early_gate)
        self.assertLess(capture, early_gate)
        self.assertLess(capture, early_return)

    def test_metadata_self_timing_is_sampled_and_flushed_once_per_frame(self) -> None:
        self.assertIn("kShadowMetadataTimingSamplePeriod = 16u", DEVICE)
        self.assertIn("shadowMetadataCaptureTicks +=", DEVICE)
        self.assertIn("elapsed - overhead", DEVICE)
        self.assertIn("SemanticDataPerfTag::ShadowMetadataCapture", DEVICE)
        self.assertIn('"ShadowMetadata", "MetadataCapture"', MONITOR)
        self.assertNotIn(
            'cpuScope("ShadowMetadata', DEVICE
        )

    def test_metadata_store_has_no_geometry_or_replay_contract(self) -> None:
        record_start = METADATA_H.index("struct War3ShadowDrawMetadata {")
        record_end = METADATA_H.index("class War3ShadowDrawMetadataFrameStore", record_start)
        record = METADATA_H[record_start:record_end]
        self.assertNotIn("positionStorage", record)
        self.assertNotIn("indexStorage", record)
        self.assertNotIn("ShadowCasterDraw", record)
        store = METADATA_H[record_end:METADATA_H.index(
            "War3ShadowDrawMetadataFrameStore&", record_end
        )]
        self.assertNotIn("replay", store.lower())
        self.assertNotIn("draw(", store.lower())

    def test_metadata_key_matches_every_identity_and_generation_field(self) -> None:
        fields = (
            "instanceIdentity",
            "sceneNode",
            "renderablePart",
            "meshPayloadPtr",
            "worldObjectEntry",
            "unitPtr",
            "jHandle",
            "layerIndex",
            "producerStage",
            "payloadWord108",
            "payloadWord11C",
            "materialSignatureHash",
            "textureIdentity",
            "textureGeneration",
        )
        equal_start = METADATA_CPP.index("War3ShadowDrawMetadataKey::operator==")
        hash_start = METADATA_CPP.index("War3ShadowDrawMetadataKey::stableHash", equal_start)
        texture_start = METADATA_CPP.index("War3ShadowTextureGeneration", hash_start)
        equal_block = METADATA_CPP[equal_start:hash_start]
        hash_block = METADATA_CPP[hash_start:texture_start]
        for field in fields:
            self.assertIn(field, equal_block, field)
            self.assertIn(field, hash_block, field)

    def test_metadata_is_exact_frame_and_three_slot_generation_checked(self) -> None:
        self.assertIn("std::array<FrameSlot, 3u>", METADATA_H)
        self.assertIn("slot.frameSerial != frameSerial", METADATA_CPP)
        self.assertIn("item.uvPageGeneration != slot.pageGeneration", METADATA_CPP)
        self.assertIn("item.alpha.uvPageGeneration != slot.pageGeneration", METADATA_CPP)
        lookup_start = METADATA_CPP.index("lookupAlpha(")
        lookup_end = METADATA_CPP.index("lookupBlocker(", lookup_start)
        self.assertNotIn("+ 8u", METADATA_CPP[lookup_start:lookup_end])

    def test_metadata_small_store_remains_exact_matched(self) -> None:
        self.assertIn("slot.records.reserve(256u)", METADATA_CPP)
        self.assertIn("std::find_if(", METADATA_CPP)
        self.assertIn("MatchesQuery(item.key, query, true)", METADATA_CPP)
        self.assertIn("MatchesQuery(item.key, query, false)", METADATA_CPP)
        self.assertIn("MetadataTextureGenerationMatches(item)", METADATA_CPP)
        self.assertNotIn("unordered_multimap", METADATA_H)

    def test_known_non_blocker_skips_anonymous_geometry_heuristic(self) -> None:
        self.assertIn("const bool anonymousBlockerProbe =", DEVICE)
        self.assertIn("blockerRawcode == 0u", DEVICE)
        self.assertIn("if (anonymousBlockerProbe) {", DEVICE)
        identity = DEVICE.index(
            "const auto identityQuery = War3MakeShadowMetadataQuery(contract, 0u)"
        )
        material = DEVICE.index(
            "War3TryBuildCurrentDrawRecordMaterialSignature(contract, material)",
            identity,
        )
        alpha_gate = DEVICE.index(
            "if (!War3ShadowMetadataAlphaRuntime() || !nativeAlphaTest)",
            identity,
        )
        self.assertLess(alpha_gate, material)
        self.assertIn(
            "material.alphaMode ==\n"
            "      dxvk::war3::shadow::ShadowAlphaMode::Opaque",
            DEVICE,
        )
        self.assertIn("metadataRejectedOpaqueCount.fetch_add", DEVICE)

    def test_metadata_uv_uses_frame_ring_and_never_persistent_geometry(self) -> None:
        capture_start = DEVICE.index(
            "bool D3D9DeviceEx::War3CaptureShadowDrawMetadata("
        )
        capture_end = DEVICE.index(
            "void D3D9DeviceEx::War3TryCaptureShadowCaster(", capture_start
        )
        capture = DEVICE[capture_start:capture_end]
        self.assertIn("War3AllocFreezeBuffer(uvBytes, uvStorageOffset)", capture)
        self.assertIn("ctx->copyBuffer(cDst, cDstOffset, cSrc", capture)
        self.assertNotIn("War3CreateShadowPersistentBuffer", capture)

        alpha_key_start = DEVICE.index(
            "const bool semanticAlphaPayloadClassified ="
        )
        alpha_key_end = DEVICE.index("key.mode =", alpha_key_start)
        self.assertNotIn("semanticMetadata.uvPageGeneration",
                         DEVICE[alpha_key_start:alpha_key_end])

        apply_start = DEVICE.index("candidate.alphaTestEnabled = false;")
        apply_end = DEVICE.index("draw.category =", apply_start)
        apply = DEVICE[apply_start:apply_end]
        self.assertNotIn(
            "candidate.uvStorage = semanticAlphaPayload.uvStorage", apply
        )
        self.assertIn("draw.uvStorage = semanticAlphaPayload.uvStorage", apply)
        self.assertIn(
            "draw.diffuseTexture = semanticAlphaPayload.diffuseTexture", apply
        )

    def test_alpha_payload_is_fail_closed_and_cannot_enter_lease(self) -> None:
        self.assertIn(
            "if (semanticAlphaPayloadClassified && semanticAlphaPayloadState != 5u)",
            DEVICE,
        )
        self.assertIn(
            "packet.material.alphaMode !=\n"
            "        dxvk::war3::shadow::ShadowAlphaMode::Opaque",
            DEVICE,
        )

    def test_unreadable_blocker_is_diagnostic_not_rejected(self) -> None:
        self.assertIn(
            "blockerReason != War3ShadowMetadataBlockerReason::Unreadable",
            METADATA_H,
        )
        unreadable = DEVICE.index(
            "publishBlocker(War3ShadowMetadataBlockerReason::Unreadable)"
        )
        below_ground = DEVICE.index(
            "publishBlocker(War3ShadowMetadataBlockerReason::BelowGround)",
            unreadable,
        )
        self.assertNotIn("return true;", DEVICE[unreadable:below_ground])

    def test_lease_expiry_does_not_mutate_authoritative_core(self) -> None:
        helper = DEVICE.index("const auto noteLeaseExpiredBackingOnly")
        first_call = DEVICE.index("noteLeaseExpiredBackingOnly();", helper)
        block = DEVICE[helper:first_call]
        self.assertIn(
            "semanticSceneShadowManifestLeaseExpiredBackingOnlyCount++", block
        )
        self.assertNotIn("committedPartKeys", block)
        self.assertNotIn("observationPartKeys", block)
        self.assertIn("constexpr uint64_t kPacketGraceFrames = 1u", DEVICE)
        self.assertIn("authoritativeAbsenceStreak", DEVICE)
        self.assertIn("if (streak < 2u)", DEVICE)

    def test_palette_cache_contract_stays_historical_and_ready_gate_is_fail_closed(self) -> None:
        self.assertIn("kContractCacheSize = 4096u", CURRENT_DRAW)
        self.assertIn("kContractCacheWays = 1u", CURRENT_DRAW)
        self.assertIn("kPaletteSnapshotCacheSize = 512u", CURRENT_DRAW)
        self.assertIn("kPaletteSnapshotCacheWays = 1u", CURRENT_DRAW)
        self.assertIn("FindLocalCurrentDrawContract(renderablePart)", CURRENT_DRAW)
        self.assertIn("FindLocalPaletteSnapshot(record.renderablePart)", CURRENT_DRAW)
        self.assertIn("SelectLocalCurrentDrawContractSlot(record.renderablePart)", CURRENT_DRAW)
        self.assertIn("SelectLocalPaletteSnapshotSlot(record.renderablePart)", CURRENT_DRAW)
        self.assertIn(
            "return (uintptr_t(renderablePart) >> 4u) % setCount;",
            CURRENT_DRAW,
        )
        ready_gate = CURRENT_DRAW[
            CURRENT_DRAW.index("CurrentDrawSnapshotRecordPhase::ReadyGate") :
            CURRENT_DRAW.index("CurrentDrawSnapshotRecordPhase::PriorityGate")
        ]
        self.assertIn("!RecordHasLocalPaletteSnapshot(record)", ready_gate)
        self.assertIn("return;", ready_gate)
        rebuild = DEVICE[
            DEVICE.index("if (authoritativeSkinnedRequired &&") :
            DEVICE.index("War3PacketBuildPhase::PaletteInstall")
        ]
        self.assertIn("War3TryBuildLiveRuntimeGroupPalette", rebuild)
        self.assertIn("/*allowCModelFallbackForCall=*/false", rebuild)
        self.assertIn("return false;", rebuild)
        for rejected_symbol in (
            "TrustedPaletteCollisionOverflow",
            "TRUSTED_PALETTE_COLLISION_OVERFLOW",
            "includePreferredWithoutPalette",
            "CurrentDrawMissingPaletteRecoveryKey",
            "PREFERRED_MISSING_PALETTE_REBUILD",
        ):
            self.assertNotIn(rejected_symbol, CURRENT_DRAW)
            self.assertNotIn(rejected_symbol, CURRENT_DRAW_H)
            self.assertNotIn(rejected_symbol, DEVICE)
            self.assertNotIn(rejected_symbol, MONITOR)

    def test_missing_part_is_skipped_without_rejecting_whole_object(self) -> None:
        missing = DEVICE.index(
            "semanticSceneShadowManifestMissingRequiredPartCount +="
        )
        next_group = DEVICE.index(
            "semanticSceneShadowManifestObjectCoreCompleteCount++", missing
        )
        self.assertNotIn("continue;", DEVICE[missing:next_group])

    def test_final_caster_trace_has_metadata_and_lifecycle_identity(self) -> None:
        for field in (
            "metadataKeyHash",
            "alphaMetadataFrameSerial",
            "metadataBlockerReason",
            "partLifecycleState",
            "alphaPayloadComplete",
            "positionStorageGeneration",
            "indexStorageGeneration",
            "renderablePart",
            "layerIndex",
        ):
            self.assertIn(field, BRIDGE)

    def test_analyzer_tracks_part_layer_metadata_and_blocker_failures(self) -> None:
        for token in (
            "part_layer_key",
            "if is_zero_hex(part)",
            "unexplainedPartDisappearanceCount",
            "backingGenerationChangeCount",
            "alphaPayloadGapCount",
            "blockerLeakCount",
            "largeGeometryAnchoredNearOrigin",
        ):
            self.assertIn(token, ANALYZER)

    def test_final_blocker_boundary_fails_closed(self) -> None:
        gate = DEVICE.index("const bool metadataBlocker =")
        publish = DEVICE.index(
            "m_war3Scene.shadowCasters.emplace_back(std::move(draw))", gate
        )
        block = DEVICE[gate:publish]
        self.assertIn("blockerFinalLeakCount.fetch_add", block)
        self.assertIn("return false;", block)

    def test_stage_lifecycle_closes_metadata_pipeline(self) -> None:
        policy_h = (
            ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.h"
        ).read_text(encoding="utf-8", errors="ignore")
        policy_cpp = (
            ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.cpp"
        ).read_text(encoding="utf-8", errors="ignore")
        perf_cpp = (
            ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
        ).read_text(encoding="utf-8", errors="ignore")
        for name in (
            "metadataClassified",
            "metadataCaptured",
            "metadataApplied",
        ):
            self.assertIn(name, policy_h)
            self.assertIn(name, policy_cpp)
            self.assertIn(f'"{name}"', perf_cpp)
        self.assertGreaterEqual(
            DEVICE.count("NoteShadowStageMetadataClassified(int(stage));"),
            2,
        )
        self.assertGreaterEqual(
            DEVICE.count("NoteShadowStageMetadataCaptured(int(stage));"),
            2,
        )
        self.assertIn("NoteShadowStageMetadataApplied(", DEVICE)


if __name__ == "__main__":
    unittest.main()
