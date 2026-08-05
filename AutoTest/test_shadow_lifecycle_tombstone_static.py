import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIFECYCLE_H = (
    ROOT / "src/d3d9/war3/render/war3_shadow_lifecycle.h"
).read_text(encoding="utf-8")
LIFECYCLE_CPP = (
    ROOT / "src/d3d9/war3/render/war3_shadow_lifecycle.cpp"
).read_text(encoding="utf-8")
VISIBLE_CPP = (
    ROOT / "src/d3d9/war3/render/war3_visible_renderables.cpp"
).read_text(encoding="utf-8")
VISIBLE_H = (
    ROOT / "src/d3d9/war3/render/war3_visible_renderables.h"
).read_text(encoding="utf-8")
CURRENT_DRAW_CPP = (
    ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp"
).read_text(encoding="utf-8")
WIDGET_CPP = (
    ROOT / "src/d3d9/war3/hooks/war3_hook_widget_identity.cpp"
).read_text(encoding="utf-8")
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class ShadowLifecycleTombstoneStaticTest(unittest.TestCase):
    def test_all_authoritative_reasons_exist(self):
        for reason in ("Hidden", "Removed", "StageDisabled", "Replaced"):
            self.assertIn(reason, LIFECYCLE_H)

    def test_stage_disable_is_a_global_stage_tombstone(self):
        self.assertIn("activeDisabledStages", LIFECYCLE_CPP)
        self.assertIn(
            "tombstone.reason == ShadowCasterTombstoneReason::StageDisabled",
            LIFECYCLE_CPP,
        )
        self.assertIn(
            "State().activeDisabledStages.find(identity.producerStage)",
            LIFECYCLE_CPP,
        )

    def test_fresh_ack_requires_newer_policy_revision(self):
        self.assertIn(
            "stagePolicyRevision > stageIt->second.stagePolicyRevision",
            LIFECYCLE_CPP,
        )
        self.assertIn(
            "visibleFrameSerial <= tombstone.visibleFrameSerial",
            LIFECYCLE_CPP,
        )

    def test_manifest_retirement_removes_object_and_part_records(self):
        self.assertIn("retireShadowManifest(", VISIBLE_CPP)
        self.assertIn("m_shadowManifestObjects.erase", VISIBLE_CPP)
        self.assertIn("m_shadowManifestParts.erase", VISIBLE_CPP)
        self.assertIn("directPointerMatch", VISIBLE_CPP)

    def test_manifest_part_contract_keeps_producer_and_lifecycle_identity(self):
        for field in (
            "producerStage",
            "producerGroup",
            "sourceKind",
            "producerFreshThisFrame",
            "visibleFrameSerial",
            "stagePolicyRevision",
            "fromGrace",
            "graceAge",
            "alphaPayloadComplete",
        ):
            self.assertGreaterEqual(VISIBLE_H.count(field), 2, field)
        self.assertIn(
            "part.producerStage == tombstone.identity.producerStage",
            VISIBLE_CPP,
        )

    def test_grace_never_refreshes_manifest_or_lease_epochs(self):
        self.assertIn("if (record.fromGrace)\n      continue;", VISIBLE_CPP)
        self.assertIn(
            "record.producerFreshThisFrame && !snapshot.fromGrace",
            CURRENT_DRAW_CPP,
        )
        self.assertIn(
            "if (!eligible.sample.contract.fromGrace)\n"
            "      shadowEligibleManifestRecords.push_back(manifestRecord);",
            DEVICE_CPP,
        )
        self.assertIn(
            "!eligible.sample.contract.fromGrace &&\n"
            "        packetSafeForDirectPartLease(eligible, true)",
            DEVICE_CPP,
        )
        self.assertIn(
            "eligible.sample.contract.fromGrace)\n"
            "        continue;",
            DEVICE_CPP,
        )

    def test_unverified_widget_callers_are_observer_only(self):
        self.assertIn("ObserveWidgetLifecycleCaller(", WIDGET_CPP)
        self.assertNotIn("PublishShadowCasterTombstone(", WIDGET_CPP)
        self.assertIn("lastCallerRva", WIDGET_CPP)
        self.assertIn("lastArgumentMask", WIDGET_CPP)

    def test_tombstone_clears_drawtime_alpha_and_stage_retention(self):
        self.assertIn("m_war3DrawTimeVBCache.erase", DEVICE_CPP)
        self.assertIn("RetirePayloadsForTombstone(", DEVICE_CPP)
        self.assertIn("ClearPayloadsForLifecycleOverflow()", DEVICE_CPP)
        self.assertIn("m_war3Stage13RetainedCasters.clear()", DEVICE_CPP)
        self.assertIn("m_war3S1TerrainCasterStash.clear()", DEVICE_CPP)


if __name__ == "__main__":
    unittest.main()
