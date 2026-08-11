#!/usr/bin/env python3
"""Structural contracts for P0 producer completeness and active cache GC."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
POLICY = (ROOT / "src/d3d9/war3/render/war3_shadow_drawtime_cache_policy.h").read_text(encoding="utf-8")
CAPTURE = (ROOT / "src/d3d9/war3/render/war3_shadow_capture_frontend.cpp").read_text(encoding="utf-8")
CAPTURE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_capture_frontend.h").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")


class ProducerCompletenessContractTest(unittest.TestCase):
    @staticmethod
    def function_body(source: str, signature: str) -> str:
        start = source.index(signature)
        open_brace = source.index("{", start)
        depth = 0
        for index in range(open_brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        raise AssertionError(f"unterminated function: {signature}")

    def test_contract_is_unsealed_by_default_and_stamped(self):
        self.assertIn("bool sealed = false", SCENE)
        self.assertIn("sealFrameSerial", SCENE)
        self.assertIn("requiredCasterOmissionCount", SCENE)
        self.assertIn("exactBudgetDeferredUniqueCasterCount", SCENE)
        self.assertIn("return sealed && !counterOverflow", SCENE)
        self.assertIn("War3SealShadowProducerCompleteness(", DEVICE)
        self.assertGreaterEqual(DEVICE.count("War3SealShadowProducerCompleteness("), 3)

    def test_exact_budget_reasons_do_not_reuse_generic_reject_set(self):
        for name in (
            "PositionAllocBudget",
            "UvAllocBudget",
            "IndexAllocBudget",
            "AllocationFailure",
        ):
            self.assertIn(f"War3RequiredCasterOmissionReason::{name}", DEVICE)
        self.assertIn("m_war3RequiredCasterOmissionKeys", DEVICE)
        self.assertIn("m_war3DrawTimeExactRejectedKeys", DEVICE)
        self.assertIn("mandatoryUvBudgetDeferred", DEVICE)
        self.assertIn("indexBudgetDeferred", DEVICE)

    def test_fallback_and_arena_early_returns_are_accounted(self):
        for name in ("FallbackByteBudget", "ArenaAdmission", "FreezeFailure"):
            self.assertIn(f"War3RequiredCasterOmissionReason::{name}", DEVICE)
        self.assertIn("ShadowArena_BeginBundle", DEVICE)
        self.assertIn("War3RecordRequiredCasterOmission(", DEVICE)

    def test_required_directional_casters_only_use_hard_capacity(self):
        self.assertIn("bool requiredDirectionalCaster = false", CAPTURE_H)
        self.assertIn("RequiredShadowCasterFitsHardBudget", CAPTURE_H)
        required = CAPTURE.index("if (policy.requiredDirectionalCaster)")
        soft = CAPTURE.index("const float softScale", required)
        block = CAPTURE[required:soft]
        self.assertIn("RequiredShadowCasterFitsHardBudget(policy)", block)
        self.assertIn('decision.reason = "required_caster_hard_budget"', block)
        self.assertNotIn("softBudgetBytes", block)
        self.assertIn("budgetPolicy.requiredDirectionalCaster = true", DEVICE)
        self.assertIn("War3RequiredCasterOmissionReason::SoftPriorityBudget", DEVICE)

    def test_active_working_set_is_not_lru_evicted(self):
        self.assertIn("kWar3ShadowDrawTimeVBCacheGcIntervalFrames = 60u", POLICY)
        self.assertIn("kWar3ShadowDrawTimeStaticRecentProtectFrames", POLICY)
        self.assertIn(
            "2u * kWar3ShadowDrawTimeVBCacheGcIntervalFrames", POLICY
        )
        self.assertIn("IsWar3ShadowDrawTimeStaticWorkingSetProtected", POLICY)
        self.assertIn(
            "m_war3DrawTimeVBCacheLastCleanFrame +\n"
            "            war3::render::kWar3ShadowDrawTimeVBCacheGcIntervalFrames",
            DEVICE,
        )
        self.assertNotIn("m_war3DrawTimeVBCacheLastCleanFrame + 60u", DEVICE)
        self.assertIn("while (staticBytesInactive > staticCap)", DEVICE)
        self.assertIn("stableKeyLess", DEVICE)
        self.assertIn("drawTimeVBCacheStaticProtectedBytes", DEVICE)
        self.assertIn("drawTimeVBCacheStaticOverCapBytes", DEVICE)

    def test_consumers_gate_before_new_publication(self):
        self.assertIn("ProducerIncomplete", ROOT.joinpath(
            "src/d3d9/war3/render/war3_shadow_replay_validation.h"
        ).read_text(encoding="utf-8"))
        self.assertIn("ProducerStampMismatch", ROOT.joinpath(
            "src/d3d9/war3/render/war3_shadow_replay_validation.h"
        ).read_text(encoding="utf-8"))
        self.assertGreaterEqual(SHADOW.count("validateShadowProducerCompleteness("), 5)
        self.assertIn("renderedShadowMap && producerComplete", SHADOW)
        self.assertIn("!m_replayValidationFailedThisFrame", SHADOW)
        self.assertIn("!m_workloadGovernorRejectedThisFrame", SHADOW)

    def test_semantic_validation_fallback_seals_after_final_stamp(self):
        body = self.function_body(
            DEVICE,
            "bool D3D9DeviceEx::War3ExecuteSemanticShadowSceneForValidation(",
        )
        moved_scene = body.index("input.scene = std::move(m_war3Scene);")
        frame_stamp = body.index("input.frameSerial = pipelineFrameSerial;")
        device_stamp = body.index(
            "input.deviceEpoch = m_war3GpuSkinDeviceEpoch;", frame_stamp
        )
        seal = body.index("War3SealShadowProducerCompleteness(", device_stamp)
        emit = body.index("EmitCs(", seal)
        self.assertLess(moved_scene, frame_stamp)
        self.assertLess(frame_stamp, device_stamp)
        self.assertLess(device_stamp, seal)
        self.assertLess(seal, emit)
        sealed = body[seal:emit]
        self.assertIn("input.scene, input.frameSerial", sealed)
        self.assertIn("input.mapEpoch, input.deviceEpoch", sealed)

    def test_incomplete_producer_cannot_take_semantic_empty_hold(self):
        body = self.function_body(
            SHADOW,
            "void War3ShadowReceiverPass::Run(",
        )
        producer = body.index("const bool producerCompleteForFrame =")
        ordinary = body.index("const bool ordinaryShadowMapReuseAllowed =", producer)
        hold = body.index("if (ordinaryShadowMapReuseAllowed &&", ordinary)
        recovery = body.index("shadowCandidateRejectedForRecovery()", hold)
        self.assertIn("input.scene.producerCompleteness.accepts(", body[producer:ordinary])
        ordinary_block = body[ordinary:hold]
        for token in (
            "!producerCompleteForFrame",
            "m_replayValidationFailedThisFrame",
            "m_workloadGovernorRejectedThisFrame",
        ):
            self.assertIn(token, body[producer:ordinary])
        self.assertIn("holdForSemanticDynamicEmptyReplay", body[hold:recovery])
        self.assertIn(
            "m_recentSemanticDynamicHoldFramesRemaining",
            body[hold:recovery],
        )
        self.assertIn("m_replayValidationHoldFramesRemaining != 0u", body[recovery:])
        self.assertIn("--m_replayValidationHoldFramesRemaining;", body[recovery:])
        self.assertIn("m_hasCompleteShadowMap = false;", body[recovery:])

    def test_policy_runnable_is_registered(self):
        self.assertIn("war3_shadow_producer_completeness_policy_test", MESON)


if __name__ == "__main__":
    unittest.main()
