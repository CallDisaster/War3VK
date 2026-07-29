import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
POLICY_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.h"
).read_text(encoding="utf-8")
POLICY_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_producer_policy.cpp"
).read_text(encoding="utf-8")
LIFECYCLE_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_lifecycle.cpp"
).read_text(encoding="utf-8")
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8"
)
SHADOW_CPP = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(
    encoding="utf-8"
)
PERF_CPP = (
    ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
).read_text(encoding="utf-8")


class ShadowStageLifecycleStaticTests(unittest.TestCase):
    def test_thirty_two_physical_stages_plus_overflow_are_fixed(self):
        self.assertIn(
            "kShadowStageLifecycleStageCount = 32u", POLICY_H
        )
        self.assertIn(
            "kShadowStageLifecycleStageCount + 1u", POLICY_H
        )
        for field in (
            "attempt",
            "policyAccepted",
            "metadataClassified",
            "metadataCaptured",
            "metadataApplied",
            "canonicalPublished",
            "replayPrepared",
            "cascadeDrawn",
            "retiredHidden",
            "retiredRemoved",
            "retiredStageDisabled",
            "retiredReplaced",
            "rejectedOverlay",
            "rejectedStage10Owner",
            "rejectedStage13Owner",
            "rejectedAlphaPayload",
        ):
            self.assertIn(field, POLICY_H)

    def test_hot_path_observer_is_explicitly_opt_in(self):
        self.assertIn(
            'env::getEnvVar("DXVK_WAR3_SHADOW_STAGE_LIFECYCLE") == "1"',
            POLICY_CPP,
        )
        self.assertIn(
            "if (traceLifecycle)\n"
            "    IncrementStageCounter(g_attempt, context.stage);",
            POLICY_CPP,
        )
        self.assertIn(
            '"DXVK_WAR3_SHADOW_STAGE_LIFECYCLE"', PERF_CPP
        )

    def test_accepted_published_prepared_and_drawn_are_distinct(self):
        self.assertIn(
            "IncrementStageCounter(g_policyAccepted, context.stage)",
            POLICY_CPP,
        )
        self.assertIn(
            "NoteShadowStageCanonicalPublished(draw.stage)", DEVICE_CPP
        )
        self.assertIn(
            "NoteShadowStageReplayPrepared(draw.stage)", SHADOW_CPP
        )
        self.assertIn(
            "NoteShadowStageCascadeDrawn(draw.stage, c)", SHADOW_CPP
        )

    def test_retirement_reuses_authoritative_tombstone_reason(self):
        self.assertIn(
            "NoteShadowStageRetired(", LIFECYCLE_CPP
        )
        self.assertIn(
            "static_cast<uint32_t>(reasonIndex)", LIFECYCLE_CPP
        )
        for field in (
            "retiredHidden",
            "retiredRemoved",
            "retiredStageDisabled",
            "retiredReplaced",
        ):
            self.assertIn(field, POLICY_CPP)

    def test_report_contains_the_complete_stage_lifecycle(self):
        self.assertIn(
            'json << "  \\"shadowStageLifecycle\\": {\\n";', PERF_CPP
        )
        for key in (
            "attempt",
            "policyAccepted",
            "metadataClassified",
            "metadataCaptured",
            "metadataApplied",
            "canonicalPublished",
            "replayPrepared",
            "retiredHidden",
            "retiredRemoved",
            "retiredStageDisabled",
            "retiredReplaced",
            "rejectedOverlay",
            "rejectedStage10Owner",
            "rejectedStage13Owner",
            "rejectedAlphaPayload",
        ):
            self.assertIn(f'"{key}"', PERF_CPP)
        self.assertIn('\\"cascadeDrawn\\"', PERF_CPP)


if __name__ == "__main__":
    unittest.main()
