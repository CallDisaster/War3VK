import json
import os
import pathlib
import time

import war3_autotest_mcp as m


def _shadow_from_probe(res):
    runtime_status = dict(
        ((res.get("result", {}) or {}).get("runtimeStatus", {})) or {}
    )
    shadow = dict(runtime_status.get("shadow", {}) or {})
    shadow.update(dict(((res.get("result", {}) or {}).get("shadowRuntimeSummary", {})) or {}))
    return shadow


def _u(shadow, key):
    try:
        return int(shadow.get(key, 0) or 0)
    except Exception:
        return 0


def main():
    scenario = "dynamic_shadow_pressure"
    preset = m._get_scenario_preset(scenario)
    env = dict(preset.get("envOverrides", {}) or {})
    env["DXVK_WAR3_SCENARIO"] = scenario
    env["DXVK_WAR3_RUNTIME_BENCHMARK"] = "1"
    env["DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC"] = "1"
    env["DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC"] = "35"
    for key in (
        "DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE",
        "DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE_FRAMES",
    ):
        value = os.environ.get(key)
        if value is not None:
            env[key] = value
    output_suffix = os.environ.get("PHASE720_OUTPUT_SUFFIX", "sticky_fill")
    if os.environ.get("PHASE720_OBJECT_FIRST", "0") == "1":
        env["DXVK_WAR3_SEMANTIC_OBJECT_FIRST_SNAPSHOT"] = "1"
        output_suffix += "_objectfirst"
    stable_frames = os.environ.get("PHASE720_STABLE_FRAMES", "").strip()
    if stable_frames:
        env["DXVK_WAR3_SEMANTIC_IDENTITY_STABLE_FRAMES_BEFORE_REDRAW"] = stable_frames
        output_suffix += f"_stable{stable_frames}"
    max_hold = os.environ.get("PHASE720_COVERAGE_MAX_HOLD", "").strip()
    if max_hold:
        env["DXVK_WAR3_SEMANTIC_COVERAGE_DROP_MAX_HOLD_FRAMES"] = max_hold
        output_suffix += f"_maxhold{max_hold}"

    launch = m.launch_war3_test(
        map_path=str(preset.get("mapPath", m.DEFAULT_TEST_MAP)),
        windowed=bool(preset.get("windowed", False)),
        use_isolated_desktop=bool(preset.get("useIsolatedDesktop", True)),
        desktop_name=str(preset.get("desktopName", "")),
        opengl=bool(preset.get("opengl", False)),
        auto_perf_record=True,
        record_after_game_started=True,
        auto_perf_export_sec=8,
        deploy_d3d9_before_launch=True,
        build_d3d9_path="build32/src/d3d9/d3d9.dll",
        enforce_video_baseline=bool(preset.get("enforceVideoBaseline", True)),
        baseline_width=int(preset.get("baselineWidth", m.DEFAULT_BENCHMARK_WIDTH) or m.DEFAULT_BENCHMARK_WIDTH),
        baseline_height=int(preset.get("baselineHeight", m.DEFAULT_BENCHMARK_HEIGHT) or m.DEFAULT_BENCHMARK_HEIGHT),
        baseline_refresh_rate=int(preset.get("baselineRefreshRate", m.DEFAULT_BENCHMARK_REFRESH) or m.DEFAULT_BENCHMARK_REFRESH),
        profile=str(preset.get("profile", "full_default")),
        disable_modules=str(preset.get("disableModules", "")),
        env_overrides_json=json.dumps(env),
    )
    samples = []
    stop = {}
    result = {"scenario": scenario, "launch": launch, "samples": samples}
    try:
        if not launch.get("ok"):
            result["ok"] = False
            result["stage"] = "launch"
            return result
        pid = int(launch["pid"])
        ready = m.wait_for_game_ready(pid=pid, timeout_sec=int(preset.get("readyTimeoutSec", 120) or 120))
        result["ready"] = ready
        if not ready.get("ok"):
            result["ok"] = False
            result["stage"] = "ready"
            return result
        hot = m.wait_for_hot_shadow_frame(pid=pid, timeout_sec=90, prefer_summary_poll=True)
        result["hot"] = hot
        time.sleep(20.0)
        t0 = time.time()
        while time.time() - t0 < 35.0:
            probe = m._control_plane_request(
                pid=pid,
                command="get_hot_shadow_probe",
                payload={},
                timeout_sec=2.0,
            )
            shadow = _shadow_from_probe(probe)
            samples.append({
                "t": round(time.time() - t0, 3),
                "ok": bool(probe.get("ok")),
                "identityHash": _u(shadow, "semanticSceneDirectLastSubmittedIdentityHash"),
                "identityChurn": _u(shadow, "semanticSceneDirectIdentityChurnCount"),
                "replay": _u(shadow, "semanticSceneReplayDrawsCount"),
                "drawn": _u(shadow, "semanticSceneShadowMapDrawnCasters"),
                "submitted": _u(shadow, "semanticSceneSubmitted"),
                "submittedUnit": _u(shadow, "semanticSceneSubmittedUnit"),
                "submittedSkinned": _u(shadow, "semanticSceneSubmittedSkinned"),
                "submittedSkinnedNonUnitResolved": _u(
                    shadow, "semanticSceneSubmittedSkinnedNonUnitResolvedCount"
                ),
                "submittedSkinnedUnknownPacketKind": _u(
                    shadow, "semanticSceneSubmittedSkinnedUnknownPacketKindCount"
                ),
                "submittedSkinnedUnitPtrNonUnitResolved": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount",
                ),
                "submittedSkinnedGroupNonZero": _u(
                    shadow, "semanticSceneSubmittedSkinnedGroupNonZeroCount"
                ),
                "submittedSkinnedTransparentQueue": _u(
                    shadow, "semanticSceneSubmittedSkinnedTransparentQueueCount"
                ),
                "submittedSkinnedMissingUnitPtr": _u(
                    shadow, "semanticSceneSubmittedSkinnedMissingUnitPtrCount"
                ),
                "submittedSkinnedDynamicUnitEvidence": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount",
                ),
                "submittedBuilding": _u(shadow, "semanticSceneSubmittedBuilding"),
                "submittedDestructible": _u(shadow, "semanticSceneSubmittedDestructible"),
                "submittedCutout": _u(shadow, "semanticSceneSubmittedCutout"),
                "submittedAlphaBlend": _u(shadow, "semanticSceneSubmittedAlphaBlend"),
                "reuse": _u(shadow, "semanticSceneReceiverReuseShadowMap"),
                "hold": _u(shadow, "semanticSceneReceiverHoldIdentityChurnCount"),
                "needMap": _u(shadow, "semanticSceneReceiverNeedShadowMap"),
                "completeMap": _u(shadow, "semanticSceneReceiverHasCompleteShadowMap"),
                "usable": _u(shadow, "semanticSceneReceiverHasUsableDirectionalShadow"),
                "capHit": _u(shadow, "semanticSceneDirectRecordCapHitCount"),
                "scanHit": _u(shadow, "semanticSceneDirectScanCapHitCount"),
                "partialObjects": _u(shadow, "semanticSceneDirectSubmittedPartialObjectCount"),
                "stickyFillBudget": _u(shadow, "semanticSceneDirectStickyFillBudgetRecordCount"),
                "stickyFillAppended": _u(shadow, "semanticSceneDirectStickyFillAppendedCount"),
                "stickyFillSubmitted": _u(shadow, "semanticSceneDirectStickyFillSubmittedCount"),
                "stickyFillMissed": _u(shadow, "semanticSceneDirectStickyFillMissedCount"),
                "partLeaseRestored": _u(shadow, "semanticSceneDirectPartLeaseRestoredCount"),
                "partLeaseUpdated": _u(shadow, "semanticSceneDirectPartLeaseUpdatedCount"),
                "partLeaseExpired": _u(shadow, "semanticSceneDirectPartLeaseExpiredCount"),
                "partLeaseRejectDynamicMesh": _u(
                    shadow, "semanticSceneDirectPartLeaseRejectedDynamicMeshCount"
                ),
                "partLeaseRejectNotSelfContained": _u(
                    shadow,
                    "semanticSceneDirectPartLeaseRejectedNotSelfContainedCount",
                ),
                "partLeaseRejectUnsafeBacking": _u(
                    shadow, "semanticSceneDirectPartLeaseRejectedUnsafeBackingCount"
                ),
                "partLeaseRejectSelfRenew": _u(
                    shadow, "semanticSceneDirectPartLeaseRejectedSelfRenewCount"
                ),
                "partLeaseBudgetLimit": _u(
                    shadow, "semanticSceneDirectPartLeaseBudgetLimitCount"
                ),
                "manifestPartLeaseRestored": _u(
                    shadow, "semanticSceneShadowManifestPartLeaseRestoredCount"
                ),
                "manifestPartLeaseUpdatedFromLive": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount",
                ),
                "manifestPartLeaseExpired": _u(
                    shadow, "semanticSceneShadowManifestPartLeaseExpiredCount"
                ),
                "manifestPartLeaseRejectPoseStale": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount",
                ),
                "manifestPartLeaseRejectSliceStale": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount",
                ),
                "manifestPartLeaseRejectUnsafeBacking": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount",
                ),
                "manifestPartLeaseRejectNotSelfContained": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount",
                ),
                "manifestPartLeaseRejectSelfRenew": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount",
                ),
                "manifestPartLeaseBudgetLimit": _u(
                    shadow, "semanticSceneShadowManifestPartLeaseBudgetLimitCount"
                ),
                "manifestPartLeaseRestoredPoseStaleCore": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount",
                ),
                "manifestPartLeasePoseFreshenedFromCModel": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount",
                ),
                "manifestPartLeasePoseCModelRefreshMiss": _u(
                    shadow,
                    "semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount",
                ),
                "manifestObjectCoreComplete": _u(
                    shadow, "semanticSceneShadowManifestObjectCoreCompleteCount"
                ),
                "manifestObjectCoreIncompleteSkip": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreIncompleteSkipCount",
                ),
                "manifestPartOmittedIncompleteCore": _u(
                    shadow,
                    "semanticSceneShadowManifestPartOmittedIncompleteCoreCount",
                ),
                "manifestCoreEpochUpdatedFromLive": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount",
                ),
                "manifestCoreEpochRestoredComplete": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount",
                ),
                "manifestCoreEpochSkippedIncomplete": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount",
                ),
                "manifestCoreEpochMissingPart": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreEpochMissingPartCount",
                ),
                "manifestCoreEpochSelfRenewReject": _u(
                    shadow,
                    "semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount",
                ),
                # Phase 7.28：skinned palette content stability probe。
                "paletteSourceNone": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceNoneCount",
                ),
                "paletteSourceDrawTime": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
                ),
                "paletteSourceGlobalSlot": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount",
                ),
                "paletteSourceBlendedCache": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount",
                ),
                "paletteSourcePublishedRegistry": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount",
                ),
                "paletteSourceCModelFallback": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount",
                ),
                "paletteStablePartSample": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStablePartSampleCount",
                ),
                "paletteHashChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteHashChurnCount",
                ),
                "paletteSourceChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSourceChurnCount",
                ),
                "paletteSlotIndexChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount",
                ),
                "paletteHashUniqueInWindow": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax",
                ),
                "paletteSlotIndexUniqueInWindow": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax",
                ),
                "paletteFirstMatrixSmallDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount",
                ),
                "paletteFirstMatrixMediumDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount",
                ),
                "paletteFirstMatrixLargeDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount",
                ),
                "paletteCountChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteCountChurnCount",
                ),
                # Phase 7.29：差分探针字段。
                "paletteLeaseKeyPayload11CMultiValue": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount",
                ),
                "paletteLeaseKeyPaletteCountMultiValue": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount",
                ),
                "paletteStrictSliceSample": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount",
                ),
                "paletteStrictSliceHashChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount",
                ),
                "paletteStrictSliceCountChurn": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount",
                ),
                "paletteStrictSliceFirstMatrixSmallDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount",
                ),
                "paletteStrictSliceFirstMatrixMediumDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount",
                ),
                "paletteStrictSliceFirstMatrixLargeDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount",
                ),
                "paletteAttributionSnapshotHit": _u(
                    shadow,
                    "semanticSceneDirectPaletteAttributionSnapshotHitCount",
                ),
                "paletteCaptureTrustedSourceHit": _u(
                    shadow,
                    "semanticSceneDirectPaletteCaptureTrustedSourceHitCount",
                ),
                "paletteCaptureTrustedSourceMiss": _u(
                    shadow,
                    "semanticSceneDirectPaletteCaptureTrustedSourceMissCount",
                ),
                # Phase 7.30 Step A：stale→live 过渡归因
                "paletteStaleRestoreSubmitted": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount",
                ),
                "paletteAfterStaleRestoreLargeDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount",
                ),
                "paletteLiveToLiveLargeDelta": _u(
                    shadow,
                    "semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount",
                ),
                "raw": _u(shadow, "semanticSceneDirectLastRawRecordCount"),
                "eligible": _u(shadow, "semanticSceneDirectLastEligibleRecordCount"),
                "uniqueObjects": _u(shadow, "semanticSceneDirectLastUniqueObjectCount"),
                "submittedObjects": _u(shadow, "semanticSceneDirectLastSubmittedObjectCount"),
                "manifestObjects": _u(shadow, "semanticSceneDirectManifestObjectCount"),
                "manifestParts": _u(shadow, "semanticSceneDirectManifestObservedPartCount"),
                "eligibleParts": _u(shadow, "semanticSceneDirectManifestShadowEligiblePartCount"),
                "shadowManifestObjects": _u(shadow, "semanticSceneShadowManifestObjectCount"),
                "shadowManifestParts": _u(shadow, "semanticSceneShadowManifestPartCount"),
                "shadowManifestStableObjects": _u(shadow, "semanticSceneShadowManifestStableObjectCount"),
                "shadowManifestNewObjects": _u(shadow, "semanticSceneShadowManifestNewObjectCount"),
                "shadowManifestExpiredObjects": _u(shadow, "semanticSceneShadowManifestExpiredObjectCount"),
                "shadowManifestFreshParts": _u(shadow, "semanticSceneShadowManifestFreshPartCount"),
                "shadowManifestLeaseableParts": _u(shadow, "semanticSceneShadowManifestLeaseablePartCount"),
                "shadowManifestPoseStaleParts": _u(shadow, "semanticSceneShadowManifestPoseStalePartCount"),
                "shadowManifestSliceStaleParts": _u(shadow, "semanticSceneShadowManifestSliceStalePartCount"),
                "shadowManifestExpiredParts": _u(shadow, "semanticSceneShadowManifestExpiredPartCount"),
                "shadowManifestMultiSliceParts": _u(shadow, "semanticSceneShadowManifestMultiSlicePartCount"),
                "shadowManifestPayload11CChurn": _u(shadow, "semanticSceneShadowManifestPayload11CChurnCount"),
                "shadowManifestRenderablePartChurn": _u(shadow, "semanticSceneShadowManifestRenderablePartChurnCount"),
                "shadowManifestCModelPoseHit": _u(
                    shadow, "semanticSceneShadowManifestCModelPoseHitCount"
                ),
                "shadowManifestCModelPoseMiss": _u(
                    shadow, "semanticSceneShadowManifestCModelPoseMissCount"
                ),
                "shadowManifestCModelPoseNoRuntime": _u(
                    shadow, "semanticSceneShadowManifestCModelPoseNoRuntimeCount"
                ),
                "shadowManifestCModelPoseLastMatrixCount": _u(
                    shadow, "semanticSceneShadowManifestCModelPoseLastMatrixCount"
                ),
                "shadowManifestCModelPoseLastMatrixHash": _u(
                    shadow, "semanticSceneShadowManifestCModelPoseLastMatrixHash"
                ),
                "submittedObjectJaccard": _u(shadow, "semanticSceneSubmittedObjectJaccardMilli"),
                "submittedPartJaccard": _u(shadow, "semanticSceneSubmittedPartJaccardMilli"),
                "visibleLookupPartLayerHit": _u(shadow, "semanticSceneVisibleLookupPartLayerHitCount"),
                "visibleLookupSingleFallback": _u(shadow, "semanticSceneVisibleLookupSingleFallbackCount"),
                "visibleLookupMiss": _u(shadow, "semanticSceneVisibleLookupMissCount"),
                "mainWorldBackingNotChecked": _u(
                    shadow, "semanticSceneDirectMainWorldBackingNotCheckedCount"
                ),
                "mainWorldBackingPass": _u(
                    shadow, "semanticSceneDirectMainWorldBackingPassCount"
                ),
                "mainWorldBackingFailNoRenderablePart": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailNoRenderablePartCount",
                ),
                "mainWorldBackingFailLookupMiss": _u(
                    shadow, "semanticSceneDirectMainWorldBackingFailLookupMissCount"
                ),
                "mainWorldBackingFailNonMainQueue": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailNonMainQueueCount",
                ),
                "mainWorldBackingFailNonWorldGroup": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailNonWorldGroupCount",
                ),
                "mainWorldBackingFailIdentityMismatch": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailIdentityMismatchCount",
                ),
                "mainWorldBackingFailSceneNodeMismatch": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount",
                ),
                "mainWorldBackingFailMeshDataMismatch": _u(
                    shadow,
                    "semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount",
                ),
                "selectionKeyUnitPtr": _u(shadow, "semanticSceneDirectSelectionKeyUnitPtrCount"),
                "selectionKeyJHandle": _u(shadow, "semanticSceneDirectSelectionKeyJHandleCount"),
                "selectionKeyRuntimeModel": _u(shadow, "semanticSceneDirectSelectionKeyRuntimeModelCount"),
                "selectionKeyWorldObject": _u(shadow, "semanticSceneDirectSelectionKeyWorldObjectCount"),
                "selectionKeySceneNode": _u(shadow, "semanticSceneDirectSelectionKeySceneNodeCount"),
                "selectionKeyModelMesh": _u(shadow, "semanticSceneDirectSelectionKeyModelMeshCount"),
                "selectionKeyRenderablePart": _u(shadow, "semanticSceneDirectSelectionKeyRenderablePartCount"),
                "unitsFilterRejectNonSkinned": _u(shadow, "semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount"),
                "unitsFilterRejectNoIdentity": _u(shadow, "semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount"),
                "unitsFilterRejectNoStableResource": _u(shadow, "semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount"),
                "preparedFallback": _u(shadow, "semanticSceneDirectPreparedSliceFallbackLayerIndexCount"),
            })
            time.sleep(0.25)
        result["ok"] = True
        result["stage"] = "poll"
        return result
    finally:
        if launch.get("ok"):
            stop = m.stop_war3(
                pid=int(launch.get("pid", 0) or 0),
                graceful_wait_sec=2,
                force=True,
                avoid_foreground_switch=True,
            )
        result["stop"] = stop


if __name__ == "__main__":
    data = main()
    samples = data.get("samples", [])
    hashes = [s["identityHash"] for s in samples if s.get("identityHash")]
    # Jaccard Min 对场景切换瞬间敏感，Max/Mean/Median 更能反映稳定状态。
    object_jaccards = [s.get("submittedObjectJaccard", 0) for s in samples]
    part_jaccards = [s.get("submittedPartJaccard", 0) for s in samples]

    def _mean(values):
        return round(sum(values) / max(1, len(values)), 2)

    def _median(values):
        if not values:
            return 0
        values_sorted = sorted(values)
        mid = len(values_sorted) // 2
        if len(values_sorted) % 2 == 1:
            return values_sorted[mid]
        return round((values_sorted[mid - 1] + values_sorted[mid]) / 2, 2)

    data["summary"] = {
        "sampleCount": len(samples),
        "identityHashUniqueCount": len(set(hashes)),
        "identityChurnSamples": sum(1 for s in samples if s.get("identityChurn")),
        "reuseSamples": sum(1 for s in samples if s.get("reuse")),
        "holdSamples": sum(1 for s in samples if s.get("hold")),
        "redrawSamples": sum(1 for s in samples if s.get("needMap") and not s.get("reuse")),
        "badNonReuseReceiverCount": sum(
            1
            for s in samples
            if s.get("needMap")
            and not s.get("reuse")
            and (not s.get("completeMap") or not s.get("usable"))
        ),
        "replayUnique": sorted(set(s.get("replay", 0) for s in samples)),
        "drawnUnique": sorted(set(s.get("drawn", 0) for s in samples)),
        "stickyFillSubmittedMax": max((s.get("stickyFillSubmitted", 0) for s in samples), default=0),
        "stickyFillMissedMax": max((s.get("stickyFillMissed", 0) for s in samples), default=0),
        "partLeaseRestoredMax": max((s.get("partLeaseRestored", 0) for s in samples), default=0),
        "partLeaseUpdatedMax": max((s.get("partLeaseUpdated", 0) for s in samples), default=0),
        "partLeaseExpiredMax": max((s.get("partLeaseExpired", 0) for s in samples), default=0),
        "partLeaseRejectDynamicMeshMax": max(
            (s.get("partLeaseRejectDynamicMesh", 0) for s in samples),
            default=0,
        ),
        "partLeaseRejectNotSelfContainedMax": max(
            (s.get("partLeaseRejectNotSelfContained", 0) for s in samples),
            default=0,
        ),
        "partLeaseRejectUnsafeBackingMax": max(
            (s.get("partLeaseRejectUnsafeBacking", 0) for s in samples),
            default=0,
        ),
        "partLeaseRejectSelfRenewMax": max(
            (s.get("partLeaseRejectSelfRenew", 0) for s in samples),
            default=0,
        ),
        "partLeaseBudgetLimitMax": max(
            (s.get("partLeaseBudgetLimit", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRestoredMax": max(
            (s.get("manifestPartLeaseRestored", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseUpdatedFromLiveMax": max(
            (s.get("manifestPartLeaseUpdatedFromLive", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseExpiredMax": max(
            (s.get("manifestPartLeaseExpired", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRejectPoseStaleMax": max(
            (s.get("manifestPartLeaseRejectPoseStale", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRejectSliceStaleMax": max(
            (s.get("manifestPartLeaseRejectSliceStale", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRejectUnsafeBackingMax": max(
            (s.get("manifestPartLeaseRejectUnsafeBacking", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRejectNotSelfContainedMax": max(
            (s.get("manifestPartLeaseRejectNotSelfContained", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRejectSelfRenewMax": max(
            (s.get("manifestPartLeaseRejectSelfRenew", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseBudgetLimitMax": max(
            (s.get("manifestPartLeaseBudgetLimit", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeaseRestoredPoseStaleCoreMax": max(
            (s.get("manifestPartLeaseRestoredPoseStaleCore", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeasePoseFreshenedFromCModelMax": max(
            (s.get("manifestPartLeasePoseFreshenedFromCModel", 0) for s in samples),
            default=0,
        ),
        "manifestPartLeasePoseCModelRefreshMissMax": max(
            (s.get("manifestPartLeasePoseCModelRefreshMiss", 0) for s in samples),
            default=0,
        ),
        "manifestObjectCoreCompleteMax": max(
            (s.get("manifestObjectCoreComplete", 0) for s in samples),
            default=0,
        ),
        "manifestObjectCoreIncompleteSkipMax": max(
            (s.get("manifestObjectCoreIncompleteSkip", 0) for s in samples),
            default=0,
        ),
        "manifestPartOmittedIncompleteCoreMax": max(
            (s.get("manifestPartOmittedIncompleteCore", 0) for s in samples),
            default=0,
        ),
        "manifestCoreEpochUpdatedFromLiveMax": max(
            (s.get("manifestCoreEpochUpdatedFromLive", 0) for s in samples),
            default=0,
        ),
        "manifestCoreEpochRestoredCompleteMax": max(
            (s.get("manifestCoreEpochRestoredComplete", 0) for s in samples),
            default=0,
        ),
        "manifestCoreEpochSkippedIncompleteMax": max(
            (s.get("manifestCoreEpochSkippedIncomplete", 0) for s in samples),
            default=0,
        ),
        "manifestCoreEpochMissingPartMax": max(
            (s.get("manifestCoreEpochMissingPart", 0) for s in samples),
            default=0,
        ),
        "manifestCoreEpochSelfRenewRejectMax": max(
            (s.get("manifestCoreEpochSelfRenewReject", 0) for s in samples),
            default=0,
        ),
        # Phase 7.28：skinned palette content stability probe 摘要。
        "paletteSourceNoneMax": max(
            (s.get("paletteSourceNone", 0) for s in samples), default=0
        ),
        "paletteSourceDrawTimeMax": max(
            (s.get("paletteSourceDrawTime", 0) for s in samples), default=0
        ),
        "paletteSourceGlobalSlotMax": max(
            (s.get("paletteSourceGlobalSlot", 0) for s in samples), default=0
        ),
        "paletteSourceBlendedCacheMax": max(
            (s.get("paletteSourceBlendedCache", 0) for s in samples), default=0
        ),
        "paletteSourcePublishedRegistryMax": max(
            (s.get("paletteSourcePublishedRegistry", 0) for s in samples),
            default=0,
        ),
        "paletteSourceCModelFallbackMax": max(
            (s.get("paletteSourceCModelFallback", 0) for s in samples),
            default=0,
        ),
        "paletteStablePartSampleMax": max(
            (s.get("paletteStablePartSample", 0) for s in samples), default=0
        ),
        "paletteHashChurnMax": max(
            (s.get("paletteHashChurn", 0) for s in samples), default=0
        ),
        "paletteSourceChurnMax": max(
            (s.get("paletteSourceChurn", 0) for s in samples), default=0
        ),
        "paletteSlotIndexChurnMax": max(
            (s.get("paletteSlotIndexChurn", 0) for s in samples), default=0
        ),
        "paletteHashUniqueInWindowMax": max(
            (s.get("paletteHashUniqueInWindow", 0) for s in samples), default=0
        ),
        "paletteSlotIndexUniqueInWindowMax": max(
            (s.get("paletteSlotIndexUniqueInWindow", 0) for s in samples),
            default=0,
        ),
        "paletteFirstMatrixSmallDeltaMax": max(
            (s.get("paletteFirstMatrixSmallDelta", 0) for s in samples),
            default=0,
        ),
        "paletteFirstMatrixMediumDeltaMax": max(
            (s.get("paletteFirstMatrixMediumDelta", 0) for s in samples),
            default=0,
        ),
        "paletteFirstMatrixLargeDeltaMax": max(
            (s.get("paletteFirstMatrixLargeDelta", 0) for s in samples),
            default=0,
        ),
        "paletteCountChurnMax": max(
            (s.get("paletteCountChurn", 0) for s in samples), default=0
        ),
        "paletteLeaseKeyPayload11CMultiValueMax": max(
            (s.get("paletteLeaseKeyPayload11CMultiValue", 0) for s in samples),
            default=0,
        ),
        "paletteLeaseKeyPaletteCountMultiValueMax": max(
            (
                s.get("paletteLeaseKeyPaletteCountMultiValue", 0)
                for s in samples
            ),
            default=0,
        ),
        "paletteStrictSliceSampleMax": max(
            (s.get("paletteStrictSliceSample", 0) for s in samples), default=0
        ),
        "paletteStrictSliceHashChurnMax": max(
            (s.get("paletteStrictSliceHashChurn", 0) for s in samples),
            default=0,
        ),
        "paletteStrictSliceCountChurnMax": max(
            (s.get("paletteStrictSliceCountChurn", 0) for s in samples),
            default=0,
        ),
        "paletteStrictSliceFirstMatrixSmallDeltaMax": max(
            (
                s.get("paletteStrictSliceFirstMatrixSmallDelta", 0)
                for s in samples
            ),
            default=0,
        ),
        "paletteStrictSliceFirstMatrixMediumDeltaMax": max(
            (
                s.get("paletteStrictSliceFirstMatrixMediumDelta", 0)
                for s in samples
            ),
            default=0,
        ),
        "paletteStrictSliceFirstMatrixLargeDeltaMax": max(
            (
                s.get("paletteStrictSliceFirstMatrixLargeDelta", 0)
                for s in samples
            ),
            default=0,
        ),
        "paletteAttributionSnapshotHitMax": max(
            (s.get("paletteAttributionSnapshotHit", 0) for s in samples),
            default=0,
        ),
        "paletteCaptureTrustedSourceHitMax": max(
            (s.get("paletteCaptureTrustedSourceHit", 0) for s in samples),
            default=0,
        ),
        "paletteCaptureTrustedSourceMissMax": max(
            (s.get("paletteCaptureTrustedSourceMiss", 0) for s in samples),
            default=0,
        ),
        # Phase 7.30 Step A：stale→live 过渡归因
        "paletteStaleRestoreSubmittedMax": max(
            (s.get("paletteStaleRestoreSubmitted", 0) for s in samples),
            default=0,
        ),
        "paletteAfterStaleRestoreLargeDeltaMax": max(
            (s.get("paletteAfterStaleRestoreLargeDelta", 0) for s in samples),
            default=0,
        ),
        "paletteLiveToLiveLargeDeltaMax": max(
            (s.get("paletteLiveToLiveLargeDelta", 0) for s in samples),
            default=0,
        ),
        "partialObjectsMax": max((s.get("partialObjects", 0) for s in samples), default=0),
        "submittedObjectJaccardMin": min((s.get("submittedObjectJaccard", 0) for s in samples), default=0),
        "submittedObjectJaccardMax": max((s.get("submittedObjectJaccard", 0) for s in samples), default=0),
        "submittedObjectJaccardMean": _mean(object_jaccards),
        "submittedObjectJaccardMedian": _median(object_jaccards),
        "submittedPartJaccardMin": min((s.get("submittedPartJaccard", 0) for s in samples), default=0),
        "submittedPartJaccardMax": max((s.get("submittedPartJaccard", 0) for s in samples), default=0),
        "submittedPartJaccardMean": _mean(part_jaccards),
        "submittedPartJaccardMedian": _median(part_jaccards),
        "shadowManifestStableObjectMax": max((s.get("shadowManifestStableObjects", 0) for s in samples), default=0),
        "shadowManifestNewObjectMax": max((s.get("shadowManifestNewObjects", 0) for s in samples), default=0),
        "shadowManifestExpiredObjectMax": max((s.get("shadowManifestExpiredObjects", 0) for s in samples), default=0),
        "shadowManifestFreshPartMax": max((s.get("shadowManifestFreshParts", 0) for s in samples), default=0),
        "shadowManifestLeaseablePartMax": max((s.get("shadowManifestLeaseableParts", 0) for s in samples), default=0),
        "shadowManifestPoseStalePartMax": max((s.get("shadowManifestPoseStaleParts", 0) for s in samples), default=0),
        "shadowManifestSliceStalePartMax": max((s.get("shadowManifestSliceStaleParts", 0) for s in samples), default=0),
        "shadowManifestExpiredPartMax": max((s.get("shadowManifestExpiredParts", 0) for s in samples), default=0),
        "shadowManifestRenderablePartChurnMax": max((s.get("shadowManifestRenderablePartChurn", 0) for s in samples), default=0),
        "shadowManifestCModelPoseHitMax": max((s.get("shadowManifestCModelPoseHit", 0) for s in samples), default=0),
        "shadowManifestCModelPoseMissMax": max((s.get("shadowManifestCModelPoseMiss", 0) for s in samples), default=0),
        "shadowManifestCModelPoseNoRuntimeMax": max((s.get("shadowManifestCModelPoseNoRuntime", 0) for s in samples), default=0),
        "shadowManifestCModelPoseLastMatrixCountMax": max(
            (s.get("shadowManifestCModelPoseLastMatrixCount", 0) for s in samples),
            default=0,
        ),
        "visibleLookupMissMax": max((s.get("visibleLookupMiss", 0) for s in samples), default=0),
        "submittedSkinnedNonUnitResolvedMax": max(
            (s.get("submittedSkinnedNonUnitResolved", 0) for s in samples),
            default=0,
        ),
        "submittedSkinnedUnknownPacketKindMax": max(
            (s.get("submittedSkinnedUnknownPacketKind", 0) for s in samples),
            default=0,
        ),
        "submittedSkinnedUnitPtrNonUnitResolvedMax": max(
            (
                s.get("submittedSkinnedUnitPtrNonUnitResolved", 0)
                for s in samples
            ),
            default=0,
        ),
        "submittedSkinnedGroupNonZeroMax": max(
            (s.get("submittedSkinnedGroupNonZero", 0) for s in samples),
            default=0,
        ),
        "submittedSkinnedTransparentQueueMax": max(
            (s.get("submittedSkinnedTransparentQueue", 0) for s in samples),
            default=0,
        ),
        "submittedSkinnedMissingUnitPtrMax": max(
            (s.get("submittedSkinnedMissingUnitPtr", 0) for s in samples),
            default=0,
        ),
        "submittedSkinnedDynamicUnitEvidenceMax": max(
            (s.get("submittedSkinnedDynamicUnitEvidence", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingNotCheckedMax": max(
            (s.get("mainWorldBackingNotChecked", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingPassMax": max(
            (s.get("mainWorldBackingPass", 0) for s in samples), default=0
        ),
        "mainWorldBackingFailNoRenderablePartMax": max(
            (s.get("mainWorldBackingFailNoRenderablePart", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailLookupMissMax": max(
            (s.get("mainWorldBackingFailLookupMiss", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailNonMainQueueMax": max(
            (s.get("mainWorldBackingFailNonMainQueue", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailNonWorldGroupMax": max(
            (s.get("mainWorldBackingFailNonWorldGroup", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailIdentityMismatchMax": max(
            (s.get("mainWorldBackingFailIdentityMismatch", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailSceneNodeMismatchMax": max(
            (s.get("mainWorldBackingFailSceneNodeMismatch", 0) for s in samples),
            default=0,
        ),
        "mainWorldBackingFailMeshDataMismatchMax": max(
            (s.get("mainWorldBackingFailMeshDataMismatch", 0) for s in samples),
            default=0,
        ),
    }
    suffix = os.environ.get("PHASE720_OUTPUT_SUFFIX", "sticky_fill")
    if os.environ.get("PHASE720_OBJECT_FIRST", "0") == "1":
        suffix += "_objectfirst"
    stable_frames = os.environ.get("PHASE720_STABLE_FRAMES", "").strip()
    if stable_frames:
        suffix += f"_stable{stable_frames}"
    max_hold = os.environ.get("PHASE720_COVERAGE_MAX_HOLD", "").strip()
    if max_hold:
        suffix += f"_maxhold{max_hold}"
    out = pathlib.Path(f"AutoTest/artifacts/phase720_{suffix}_hot_shadow_poll_20260509.json")
    out.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"path": str(out), "summary": data["summary"]}, ensure_ascii=False))
