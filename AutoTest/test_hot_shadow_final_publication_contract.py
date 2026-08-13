import copy
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "AutoTest"))

import war3_autotest_mcp as autotest  # noqa: E402
import run_unattended_perf_gate as perf_gate  # noqa: E402


def valid_summary():
    return {
        "drawTimeSemanticProducerOwnedDirectGroupedSkipCount": 96,
        "producerCompletenessSealed": 1,
        "producerSealFrameSerial": 100,
        "producerSealMapEpoch": 2,
        "producerSealDeviceEpoch": 3,
        "producerRequiredCasterOmissionCount": 0,
        "producerCompletenessReasonMask": 0,
        "producerCompletenessCounterOverflow": 0,
        "producerExactBudgetDeferredUniqueCasterCount": 0,
        "producerPositionAllocBudgetCount": 0,
        "producerUvAllocBudgetCount": 0,
        "producerIndexAllocBudgetCount": 0,
        "producerAllocationFailureCount": 0,
        "producerFallbackByteBudgetCount": 0,
        "producerArenaAdmissionCount": 0,
        "producerFreezeFailureCount": 0,
        "semanticSceneDirectLastRecordCapPartialObjectCount": 0,
        "semanticSceneDirectLastScanCapPartialObjectCount": 0,
        "semanticSceneDirectSubmittedPartialObjectCount": 0,
        "semanticSceneShadowCastersCount": 262,
        "semanticSceneReplayDrawsCount": 262,
        "semanticSceneShadowMapDrawnCasters": 1048,
        "semanticSceneReceiverCsmCascadeCount": 4,
        "semanticSceneShadowMapExecutedThisFrame": 1,
        "semanticSceneReceiverSettingsShadowsEnabled": 1,
        "semanticSceneReceiverNeedShadowMap": 1,
        "semanticSceneReceiverInputValid": 1,
        "semanticSceneReceiverInputRejectReason": 0,
        "semanticSceneReceiverRunEarlyReturnReason": 0,
        "semanticSceneReceiverHasCompleteShadowMap": 1,
        "semanticSceneReceiverHasUsableDirectionalShadow": 1,
        "semanticSceneReceiverHasSunShadow": 1,
        "semanticSceneReceiverReuseShadowMap": 0,
        "semanticSceneReceiverActiveStrengthMilli": 608,
        "semanticSceneReceiverCsmFallbackToLastGoodCount": 0,
        "semanticSceneReceiverHoldEmptyReplayCount": 0,
        "semanticSceneReceiverHoldIdentityChurnCount": 0,
        "semanticSceneReceiverHoldInvalidCsmCount": 0,
        "semanticSceneSubmittedObjectJaccardMilli": 1000,
        "semanticSceneSubmittedPartJaccardMilli": 1000,
    }


def main():
    base = valid_summary()
    result = autotest._final_shadow_publication_status(base)
    assert result["finalShadowPublicationComplete"]

    failures = {
        "no exact owner": ("drawTimeSemanticProducerOwnedDirectGroupedSkipCount", 0),
        "unsealed": ("producerCompletenessSealed", 0),
        "missing stamp": ("producerSealFrameSerial", 0),
        "caster omitted": ("producerRequiredCasterOmissionCount", 1),
        "counter overflow": ("producerCompletenessCounterOverflow", 1),
        "partial direct object": ("semanticSceneDirectSubmittedPartialObjectCount", 1),
        "replay mismatch": ("semanticSceneReplayDrawsCount", 261),
        "partial cascade draw": ("semanticSceneShadowMapDrawnCasters", 1047),
        "receiver input rejected": ("semanticSceneReceiverInputValid", 0),
        "last-good reuse": ("semanticSceneReceiverReuseShadowMap", 1),
        "zero strength": ("semanticSceneReceiverActiveStrengthMilli", 0),
        "identity mismatch": ("semanticSceneSubmittedPartJaccardMilli", 999),
    }
    for name, (field, value) in failures.items():
        candidate = copy.deepcopy(base)
        candidate[field] = value
        rejected = autotest._final_shadow_publication_status(candidate)
        assert not rejected["finalShadowPublicationComplete"], name

    launch_result = {
        "launch": {
            "priority": {"ok": True, "pid": 1234, "priority": "HIGH"},
        },
    }
    reconciled = perf_gate._reconcile_priority_evidence(
        "high",
        {"ok": False, "error": "process did not publish a retained witness"},
        launch_result,
    )
    assert reconciled["ok"]
    assert reconciled["source"] == "launcher-owned-process"
    assert reconciled["outerWitnessRace"]
    assert not perf_gate._reconcile_priority_evidence(
        "normal", {"ok": False}, launch_result
    )["ok"]
    observed = {"ok": True, "source": "retained-witness"}
    assert perf_gate._reconcile_priority_evidence(
        "high", observed, launch_result
    ) is observed

    total = 4 + len(failures)
    print(f"PASS: {total}/{total}")


if __name__ == "__main__":
    main()
