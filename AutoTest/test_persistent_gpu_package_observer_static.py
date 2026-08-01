#!/usr/bin/env python3
"""Static contracts for the value-only persistent package observer P1."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OBSERVER_H = (
    ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_observer.h"
)
OBSERVER_CPP = (
    ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_observer.cpp"
)
TEST_CPP = (
    ROOT
    / "src/d3d9/war3/gpu_skin/tests/war3_persistent_gpu_package_observer_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
STORE = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
RESOURCES = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp"


class PersistentPackageObserverContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = OBSERVER_H.read_text(encoding="utf-8")
        cls.source = OBSERVER_CPP.read_text(encoding="utf-8")
        cls.test = TEST_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")
        cls.live_sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in (DEVICE_CPP, STORE, RESOURCES)
        )

    def test_modes_exist_but_consume_is_hard_disabled(self) -> None:
        for token in (
            "Off = 0u",
            "Observe = 1u",
            "Consume = 2u",
            "kRuntimeInstantiated = false",
            "kObserveOnly = true",
            "kObserveBindsAtlas = false",
            "kObserveWritesConsumerLastUse = false",
            "kConsumeAdmissionGranted = false",
            "kRecordingThreadOwnershipIntegrated = false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header)
        self.assertIn("decision.effectiveMode = Mode::Off", self.source)
        self.assertIn("Disposition::ConsumeNotAdmitted", self.source)

    def test_fixed_capacity_pod_table_has_no_hot_path_allocator(self) -> None:
        self.assertIn("std::array<Entry, kCapacity> m_entries", self.header)
        self.assertIn("kCapacity = 4096u", self.header)
        implementation = self.header + self.source
        for forbidden in (
            "unordered_map",
            "std::vector",
            "std::string",
            "push_back",
            "emplace",
            "reserve(",
            "resize(",
            "new ",
            "malloc(",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, implementation)
        self.assertIn("std::is_trivially_copyable_v", self.header)

    def test_consumer_and_proof_domains_are_complete(self) -> None:
        for token in (
            "ConsumerMain",
            "ConsumerCsm0",
            "ConsumerCsm1",
            "ConsumerCsm2",
            "ConsumerCsm3",
            "ConsumerPointShadow",
            "ConsumerGeometryOutline",
            "IdentityOnly",
            "ContentPending",
            "PackageInputReady",
            "FullyEquivalent",
            "Rejected",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header)

    def test_exact_seal_requires_every_current_domain(self) -> None:
        exact = self.source.split("exactFrameKey(", 1)[1].split(
            "bool War3PersistentGpuPackageObserver::exactPackageDecision", 1
        )[0]
        for token in (
            "input.frameSerial == context.frameSerial",
            "input.policyRevision == context.policyRevision",
            "input.stage == context.stage",
            "input.mapEpoch == context.mapEpoch",
            "input.deviceEpoch == context.deviceEpoch",
            "input.catalogInstanceGeneration ==",
            "context.catalogInstanceGeneration",
            "input.catalogSnapshotRevision == context.catalogSnapshotRevision",
        ):
            with self.subTest(token=token):
                self.assertIn(token, exact)
        for flag in (
            "identityExact",
            "sourceExact",
            "materialExact",
            "alphaExact",
            "worldExact",
            "boundsExact",
        ):
            with self.subTest(flag=flag):
                self.assertIn(flag, self.source)

    def test_package_and_source_generations_are_per_entry(self) -> None:
        frame = self.header.split("struct FrameContext", 1)[1].split(
            "struct SealInput", 1
        )[0]
        seal = self.header.split("struct SealInput", 1)[1].split(
            "struct Entry", 1
        )[0]
        self.assertNotIn("packageGeneration", frame)
        self.assertNotIn("currentDrawSourceGeneration", frame)
        self.assertNotIn("immutableModelGeneration", frame)
        self.assertIn("catalogSnapshotRevision", frame)
        self.assertIn("catalogInstanceGeneration", frame)
        self.assertIn("packageGeneration", seal)
        self.assertIn("currentDrawSourceGeneration", seal)
        self.assertIn("immutableModelGeneration", seal)
        exact = self.source.split("exactFrameKey(", 1)[1].split(
            "bool War3PersistentGpuPackageObserver::exactPackageDecision", 1
        )[0]
        self.assertNotIn("packageGeneration", exact)
        self.assertNotIn("currentDrawSourceGeneration", exact)
        self.assertNotIn("immutableModelGeneration", exact)

    def test_ready_is_catalog_decision_bound_to_full_row(self) -> None:
        seal = self.header.split("struct SealInput", 1)[1].split(
            "struct Entry", 1
        )[0]
        self.assertIn("PackageProofCatalog::Key packageKey", seal)
        self.assertIn(
            "PackageProofCatalog::PackageContentDecision packageContentDecision",
            seal,
        )
        self.assertNotIn("packageContentReady", self.header + self.source)
        exact_package = self.source.split("exactPackageDecision(\n", 1)[1].split(
            "isKnownSingleConsumerBit", 1
        )[0]
        for token in (
            "input.packageKey.mapEpoch == input.mapEpoch",
            "input.packageKey.deviceEpoch == input.deviceEpoch",
            "input.packageKey.packageGeneration == input.packageGeneration",
            "input.packageKey.immutableModelGeneration ==",
            "input.immutableModelGeneration",
            "input.currentDrawSourceGeneration",
            "input.packageContentDecision.matches(",
            "input.catalogInstanceGeneration",
            "input.catalogSnapshotRevision",
            "input.identityToken",
            "input.sourceToken",
            "input.materialToken",
            "input.alphaToken",
            "input.worldToken",
            "input.boundsToken",
        ):
            with self.subTest(token=token):
                self.assertIn(token, exact_package)

    def test_dynamic_unknown_and_skinned_cannot_be_fully_equivalent(self) -> None:
        classify = self.source.split("classify(\n", 1)[1].split(
            "War3PersistentGpuPackageObserver::BeginDecision", 1
        )[0]
        fully = classify.index("return ProofState::FullyEquivalent")
        for gate in (
            "!input.identityKnown",
            "!input.sourceKnown",
            "!exactPackageDecision(input)",
            "input.dynamic || input.skinned || !input.staticRigidProven",
            "!exactMaterial || !exactAlpha || !exactWorld || !exactBounds",
        ):
            with self.subTest(gate=gate):
                self.assertIn(gate, classify)
                self.assertLess(classify.index(gate), fully)

    def test_observe_is_diagnostic_only_and_passes_work_through(self) -> None:
        self.assertIn(
            "decision.effectiveConsumerMask = input.requestedConsumerMask",
            self.source,
        )
        self.assertIn(
            "return m_context.canonicalWorkload;",
            self.header,
        )
        self.assertNotIn("consumerLastUse", self.source)
        self.assertNotIn("War3PersistentGpuPackageObserver", self.live_sources)
        for token in (
            "drawMutationAllowed = false",
            "atlasBindingAllowed = false",
            "writesConsumerLastUse = false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header)

    def test_eligibility_and_actual_consumers_are_separate(self) -> None:
        entry = self.header.split("struct Entry", 1)[1].split(
            "struct ActualConsumerNote", 1
        )[0]
        self.assertIn("eligibleConsumerMask", entry)
        self.assertIn("wouldUseConsumerMask", entry)
        self.assertIn("actualConsumerMask", entry)
        diagnostics = self.header.split("struct Diagnostics", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("eligibleConsumerMaskOr", diagnostics)
        self.assertIn("wouldUseConsumerMaskOr", diagnostics)
        self.assertIn("actualConsumerMaskOr", diagnostics)
        append = self.source.split("append(\n", 1)[1].split(
            "War3PersistentGpuPackageObserver::SealDecision\n"
            "War3PersistentGpuPackageObserver::seal",
            1,
        )[0]
        self.assertIn("target.actualConsumerMask = 0u", append)
        self.assertIn("target.eligibleConsumerMask =", append)
        self.assertIn("state == ProofState::FullyEquivalent", append)
        self.assertIn(
            "target.eligibleConsumerMask & target.actualConsumerMask", append
        )
        self.assertNotIn(
            "target.actualConsumerMask = target.wouldUseConsumerMask", append
        )

    def test_main_has_no_forgeable_pre_submitted_shortcut(self) -> None:
        implementation = self.header + self.source
        self.assertNotIn("preSubmittedConsumerMask", implementation)
        self.assertNotIn("preSubmittedAccepted", implementation)
        self.assertNotIn("kPreSubmittedConsumerMask", implementation)
        self.assertIn("TestMainAlsoRequiresExplicitBoundDrawEvidence", self.test)

    def test_actual_note_requires_exact_current_entry_and_known_bit(self) -> None:
        note = self.source.split("noteActualConsumer(\n", 1)[1]
        for token in (
            "m_diagnostics.effectiveMode != Mode::Observe",
            "isKnownSingleConsumerBit(note.consumerBit)",
            "note.tableIndex >= m_size",
            "note.frameSerial != m_context.frameSerial",
            "note.policyRevision != m_context.policyRevision",
            "note.mapEpoch != m_context.mapEpoch",
            "note.deviceEpoch != m_context.deviceEpoch",
            "note.catalogInstanceGeneration !=",
            "m_context.catalogInstanceGeneration",
            "note.catalogSnapshotRevision !=",
            "m_context.catalogSnapshotRevision",
            "note.packageGeneration != target.packageGeneration",
            "note.immutableModelGeneration != target.immutableModelGeneration",
            "note.currentDrawSourceGeneration !=",
            "target.currentDrawSourceGeneration",
            "PackageProofCatalog::sameKey(note.packageKey, target.packageKey)",
            "note.sourceToken != target.sourceToken",
            "note.materialToken != target.materialToken",
            "note.alphaToken != target.alphaToken",
            "note.worldToken != target.worldToken",
            "note.boundsToken != target.boundsToken",
            "note.identityToken != target.identityToken",
            "target.requestedConsumerMask & note.consumerBit",
            "target.actualConsumerMask |= note.consumerBit",
            "target.eligibleConsumerMask & target.actualConsumerMask",
        ):
            with self.subTest(token=token):
                self.assertIn(token, note)
        self.assertLess(
            note.index("isKnownSingleConsumerBit(note.consumerBit)"),
            note.index("target.actualConsumerMask |= note.consumerBit"),
        )
        self.assertLess(
            note.index("note.packageGeneration != target.packageGeneration"),
            note.index("target.actualConsumerMask |= note.consumerBit"),
        )

    def test_nonfinite_capacity_and_budget_fail_closed(self) -> None:
        self.assertIn("std::isfinite(bounds.minX)", self.source)
        self.assertIn("Disposition::NonFiniteBounds", self.source)
        self.assertIn("m_size >= kCapacity", self.source)
        self.assertIn("Disposition::DeferredCapacity", self.source)
        self.assertIn("m_size >= m_budget", self.source)
        self.assertIn("Disposition::DeferredBudget", self.source)

    def test_production_build_compiles_but_does_not_instantiate_observer(self) -> None:
        self.assertIn(
            "'war3/gpu_skin/war3_persistent_gpu_package_observer.cpp'",
            self.meson,
        )
        self.assertIn(
            "'war3_persistent_gpu_package_observer_test'",
            self.meson,
        )
        for source in (
            "'war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp'",
            "'war3/gpu_skin/tests/war3_persistent_gpu_package_observer_test.cpp'",
        ):
            with self.subTest(source=source):
                self.assertIn(source, self.meson)


if __name__ == "__main__":
    unittest.main(verbosity=2)
