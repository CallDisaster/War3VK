"""Static contracts for the generation-sealed DirectGrouped work table."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(
    encoding="utf-8", errors="replace"
)
CONTROL = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(
    encoding="utf-8", errors="replace"
)


class CompactWorkTableContracts(unittest.TestCase):
    def test_release_is_off_and_dev_build_is_observe_only(self):
        block = DEVICE[
            DEVICE.index("enum class War3CompactWorkTableMode") :
            DEVICE.index("bool War3PopulateSubmitPermutationViewRuntime")
        ]
        self.assertIn("Off = 0u", block)
        self.assertIn("Observe = 1u", block)
        self.assertIn("Consume = 2u", block)
        self.assertIn('"DXVK_WAR3_SEMANTIC_COMPACT_WORK_TABLE", 0u', block)
        self.assertIn("kDevelopmentShadowObserversEnabled", block)
        self.assertIn("ParseShadowObserverBuildMode", block)
        runtime = block[
            block.index("War3SemanticCompactWorkTableModeRuntime") :
            block.index("War3PersistentPackageStage11EvidenceModeRuntime")
        ]
        self.assertIn("War3CompactWorkTableMode::Observe", runtime)
        self.assertIn("War3CompactWorkTableMode::Off", runtime)
        self.assertNotIn("War3CompactWorkTableMode::Consume", runtime)

    def test_only_exact_current_generation_items_are_sealed(self):
        start = DEVICE.index("// A compact item is consumable only")
        end = DEVICE.index(
            "stats.semanticSceneCompactWorkTableCandidateCount++", start
        )
        block = DEVICE[start:end]
        self.assertIn("record.stage == 11", block)
        self.assertIn("record.producerStage == 11", block)
        self.assertIn("record.producerFreshThisFrame", block)
        self.assertIn("!record.fromGrace", block)
        self.assertIn("record.stagePolicyRevision", block)
        self.assertIn("CurrentShadowStagePolicyRevision()", block)
        self.assertIn("record.frameTag == currentFrameTag", block)
        self.assertIn("record.visibleFrameSerial", block)
        self.assertIn("record.renderFrameIndex == currentFrameTag", block)
        self.assertIn("record.renderablePart != nullptr", block)
        self.assertIn("record.meshPayloadPtr != nullptr", block)
        self.assertIn("record.captureSerial != 0u", block)
        self.assertIn("m_war3ShadowPersistentFrameSerial", block)
        self.assertIn("War3CompactWorkSealed", block)
        self.assertNotIn("record.known &&", block)

    def test_persistent_storage_is_generation_tagged_pod_soa(self):
        block = SCENE[
            SCENE.index("struct War3CompactWorkItem") :
            SCENE.index("struct War3FrameScene")
        ]
        self.assertIn("std::is_standard_layout_v<War3CompactWorkItem>", block)
        self.assertIn("std::is_trivially_copyable_v<War3CompactWorkItem>", block)
        self.assertIn("uint64_t generation", block)
        self.assertIn("std::vector<uint64_t> frameGenerations", block)
        self.assertIn("std::vector<uint64_t> selectionKeys", block)
        self.assertIn("std::vector<uint32_t> priorityScores", block)
        self.assertIn("std::vector<uint8_t> flags", block)

    def test_consume_requires_seal_and_generation_match(self):
        loop = DEVICE.index(
            "for (size_t recordIndex = 0u; recordIndex < recordsForBuild.size();"
        )
        block = DEVICE[loop : DEVICE.index("// --- Step 3: submit ---", loop)]
        self.assertIn("consumeCompactWorkTable", block)
        self.assertIn("m_war3CompactWorkTable.load", block)
        self.assertIn("War3CompactWorkValid", block)
        self.assertIn("War3CompactWorkSealed", block)
        self.assertIn(
            "compactWork.frameGeneration == m_war3ShadowPersistentFrameSerial",
            block,
        )
        self.assertIn("currentFrameDrawTimeProducerOwnsRecord(record)", block)
        self.assertIn("War3ShadowIsLosBlocker(record)", block)
        self.assertIn("ShadowProducerPolicyAllows(", block)

    def test_observe_compares_every_cached_early_gate(self):
        block = DEVICE[
            DEVICE.index("for (const auto& record : directRecords)") :
            DEVICE.index('enterDirectDetailPhase("PreselectRecordSort")')
        ]
        for cached, canonical in (
            ("compactExactOwner", "canonicalExactOwner"),
            ("compactStaticWorld", "canonicalStaticWorld"),
            ("compactProducerAllowed", "canonicalProducerAllowed"),
            ("compactPathBlocker", "canonicalPathBlocker"),
        ):
            self.assertIn(f"{cached} != {canonical}", block)
        self.assertIn("work.selectionKey != selectionKey", block)
        self.assertIn("work.priorityScore != priorityScore", block)
        self.assertIn("semanticSceneCompactWorkTableMismatchCount++", block)

    def test_unsealed_and_early_rejected_items_skip_expensive_identity_work(self):
        start = DEVICE.index("const auto buildCompactWorkEvidence")
        end = DEVICE.index("bool recordsForBuildAlphaPrefiltered", start)
        block = DEVICE[start:end]
        seal_exit = block.index("if (!sealed)\n          return evidence;")
        owner_gate = block.index("currentFrameDrawTimeProducerOwnsRecord", seal_exit)
        static_gate = block.index("War3SemanticRawcodeLooksStaticWorldCaster", owner_gate)
        producer_gate = block.index("ShadowProducerPolicyAllows", static_gate)
        blocker_gate = block.index("War3ShadowIsLosBlocker", producer_gate)
        selection_hash = block.index("War3SemanticDirectRecordSelectionKey", blocker_gate)
        priority_score = block.index(
            "War3SemanticDirectRecordPriorityScore", selection_hash
        )
        sticky_probe = block.index("wasPreviouslySubmitted", priority_score)
        self.assertLess(seal_exit, owner_gate)
        self.assertLess(owner_gate, static_gate)
        self.assertLess(static_gate, producer_gate)
        self.assertLess(producer_gate, blocker_gate)
        self.assertLess(blocker_gate, selection_hash)
        self.assertLess(selection_hash, priority_score)
        self.assertLess(priority_score, sticky_probe)

    def test_diagnostics_are_public_and_mismatch_is_counted(self):
        for field in (
            "semanticSceneCompactWorkTableMode",
            "semanticSceneCompactWorkTableCandidateCount",
            "semanticSceneCompactWorkTableSealedCount",
            "semanticSceneCompactWorkTableConsumedCount",
            "semanticSceneCompactWorkTableFallbackCount",
            "semanticSceneCompactWorkTableRejectStageCount",
            "semanticSceneCompactWorkTableRejectFreshnessCount",
            "semanticSceneCompactWorkTableRejectPolicyCount",
            "semanticSceneCompactWorkTableRejectFrameCount",
            "semanticSceneCompactWorkTableRejectIdentityCount",
            "semanticSceneCompactWorkTableMismatchCount",
        ):
            self.assertIn(field, SCENE)
            self.assertIn(field, CONTROL)

    def test_known_unsafe_cross_frame_shortcuts_remain_fail_closed(self):
        for setting in (
            "DXVK_WAR3_DRAWTIME_VB_CACHE",
            "DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND",
            "DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS",
            "DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE",
        ):
            self.assertIn(f'War3GetEnvU32("{setting}", 0u)', DEVICE)


if __name__ == "__main__":
    unittest.main()
