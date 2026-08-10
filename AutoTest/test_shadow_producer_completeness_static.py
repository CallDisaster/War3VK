#!/usr/bin/env python3
"""Structural contracts for P0 producer completeness and active cache GC."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
POLICY = (ROOT / "src/d3d9/war3/render/war3_shadow_drawtime_cache_policy.h").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")


class ProducerCompletenessContractTest(unittest.TestCase):
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

    def test_active_working_set_is_not_lru_evicted(self):
        self.assertIn("kWar3ShadowDrawTimeStaticRecentProtectFrames", POLICY)
        self.assertIn("IsWar3ShadowDrawTimeStaticWorkingSetProtected", POLICY)
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

    def test_policy_runnable_is_registered(self):
        self.assertIn("war3_shadow_producer_completeness_policy_test", MESON)


if __name__ == "__main__":
    unittest.main()
