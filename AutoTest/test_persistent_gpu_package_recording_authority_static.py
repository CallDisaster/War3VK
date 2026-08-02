#!/usr/bin/env python3
"""Static contracts for the isolated persistent-package recording authority."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUTHORITY_H = (
    ROOT
    / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_recording_authority.h"
)
AUTHORITY_CPP = (
    ROOT
    / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_recording_authority.cpp"
)
AUTHORITY_TEST = (
    ROOT
    / "src/d3d9/war3/gpu_skin/tests/war3_persistent_gpu_package_recording_authority_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"


class PersistentPackageRecordingAuthorityContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = AUTHORITY_H.read_text(encoding="utf-8")
        cls.source = AUTHORITY_CPP.read_text(encoding="utf-8")
        cls.test = AUTHORITY_TEST.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_remains_isolated_from_production_and_meson(self) -> None:
        self.assertNotIn(
            "war3_persistent_gpu_package_recording_authority.cpp",
            self.meson,
        )
        self.assertNotIn(
            "war3_persistent_gpu_package_recording_authority_test.cpp",
            self.meson,
        )
        for forbidden in (
            "Rc<",
            "DxvkCommandList",
            "DxvkBufferSlice",
            "VkBuffer",
            "ShadowArena",
            "EmitCs(",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.header + self.source)

    def test_current_stage_source_retains_unforgeable_p1_provenance(self) -> None:
        current_source = self.header.split("struct CurrentStageSource", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("ProofCatalog::SharedSnapshot catalogSnapshot", current_source)
        self.assertIn(
            "ProofCatalog::PackageContentDecision packageContentDecision",
            current_source,
        )
        validator = self.source.split("validCurrentStageSource(", 1)[1].split(
            "sameCurrentStageSource(", 1
        )[0]
        for token in (
            "decision.ready()",
            "PackageContentDecision::Reason::Ready",
            "decision.matches(",
            "source.catalogSnapshot->find(key)",
            "PublicationState::UploadCompleted",
            "entry->value.publicationRevision",
            "entry->value.canonicalDigest",
        ):
            with self.subTest(token=token):
                self.assertIn(token, validator)

    def test_every_authority_boundary_revalidates_the_retained_plan(self) -> None:
        self.assertIn("validExpectedPlan(input)", self.source)
        self.assertIn("validCurrentStageSource(input.source, m_context)", self.source)
        self.assertGreaterEqual(self.source.count("validRetainedPlanLocked("), 3)
        retained = self.source.split("validRetainedPlanLocked(", 1)[1].split(
            "clearPlanLocked(", 1
        )[0]
        self.assertIn("validCurrentStageSource(record.source, currentContext)", retained)
        self.assertIn("digest = appendRecordDigest(digest, record)", retained)
        self.assertIn(
            "sameRecordInput(input, m_expectedRecords[m_recordCount])",
            self.source,
        )

    def test_emit_uses_authority_owned_const_plan_and_catches_exceptions(self) -> None:
        view = self.header.split("class ImmutableCommandPlanView final", 1)[1].split(
            "struct SealedBatchView", 1
        )[0]
        self.assertIn("const RecordInput* data() const noexcept", view)
        self.assertNotIn("\n    RecordInput* data()", view)
        self.assertNotIn("\n    RecordInput* record(", view)
        emit = self.source.split("::emitSealed(", 1)[1].split("::abortLocked(", 1)[0]
        self.assertIn("m_state = State::Emitting", emit)
        self.assertIn("m_expectedRecords.data()", emit)
        self.assertIn("try {", emit)
        self.assertIn("catch (...) {", emit)
        self.assertIn("return EmitResult::CallbackFailed", emit)
        callback = self.header.split("using EmitCallback", 1)[1].split(";", 1)[0]
        self.assertNotIn("noexcept", callback)

    def test_capacity_and_one_shot_failure_paths_are_explicit(self) -> None:
        self.assertIn("kMaxRecords = 4096u", self.header)
        for token in (
            "expectedRecordCount > kMaxRecords",
            "m_recordCount >= kMaxRecords",
            "State::Emitting",
            "EmitResult::AlreadyTerminal",
            "EmitResult::EmitInProgress",
            "m_expectedRecords.clear()",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.source)

    def test_runnable_covers_forgery_mutation_faults_and_races(self) -> None:
        for token in (
            "Authority::kMaxRecords == 4096u",
            "Authority::kMaxRecords + 1u",
            "std::bad_alloc",
            "source.packageContentDecision = Decision{}",
            "other.records[0].source.packageContentDecision",
            "throwingEmit",
            "testPostSealMutationAndDuplicateSealInvalidate",
            "kRounds = 200u",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.test)


if __name__ == "__main__":
    unittest.main()
