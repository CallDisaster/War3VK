#!/usr/bin/env python3
"""Static safety contracts for the value-only package proof catalog."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CATALOG_H = (
    ROOT
    / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.h"
)
CATALOG_CPP = (
    ROOT
    / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp"
)
CATALOG_TEST = (
    ROOT
    / "src/d3d9/war3/gpu_skin/tests/war3_persistent_gpu_package_proof_catalog_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
PRODUCTION_ROOT = ROOT / "src/d3d9"
CATALOG_FILENAMES = {
    "war3_persistent_gpu_package_proof_catalog.h",
    "war3_persistent_gpu_package_proof_catalog.cpp",
    "war3_persistent_gpu_package_observer.h",
    "war3_persistent_gpu_package_observer.cpp",
    # The isolated recording-authority value contract deliberately consumes
    # the catalog's private Ready decision, but is not part of d3d9_src and
    # cannot grant a production/runtime admission by itself.
    "war3_persistent_gpu_package_recording_authority.h",
    "war3_persistent_gpu_package_recording_authority.cpp",
}
LIVE_SOURCES = tuple(
    path
    for path in PRODUCTION_ROOT.rglob("*")
    if path.suffix in {".h", ".cpp"}
    and "tests" not in path.parts
    and path.name not in CATALOG_FILENAMES
)


class PersistentPackageProofCatalogContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.header = CATALOG_H.read_text(encoding="utf-8")
        cls.source = CATALOG_CPP.read_text(encoding="utf-8")
        cls.test = CATALOG_TEST.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")
        cls.live = "\n".join(
            source.read_text(encoding="utf-8") for source in LIVE_SOURCES
        )

    def test_key_is_complete_and_schema_versioned(self) -> None:
        key = self.header.split("struct Key", 1)[1].split("};", 1)[0]
        for token in (
            "schema",
            "mapEpoch",
            "deviceEpoch",
            "packageGeneration",
            "immutableModelGeneration",
            "geosetData",
            "contentHash",
            "layoutGeneration",
            "primitiveOrdinal",
        ):
            with self.subTest(token=token):
                self.assertIn(token, key)
        self.assertIn("kSchemaVersion = 1u", self.header)
        same_key = self.source.split("sameKey(\n", 1)[1].split(
            "keyLess(\n", 1
        )[0]
        for token in (
            "lhs.schema == rhs.schema",
            "lhs.mapEpoch == rhs.mapEpoch",
            "lhs.deviceEpoch == rhs.deviceEpoch",
            "lhs.packageGeneration == rhs.packageGeneration",
            "lhs.immutableModelGeneration == rhs.immutableModelGeneration",
            "lhs.geosetData == rhs.geosetData",
            "lhs.contentHash == rhs.contentHash",
            "lhs.layoutGeneration == rhs.layoutGeneration",
            "lhs.primitiveOrdinal == rhs.primitiveOrdinal",
        ):
            with self.subTest(token=token):
                self.assertIn(token, same_key)

    def test_snapshot_is_sorted_immutable_and_shared(self) -> None:
        self.assertIn("using SharedSnapshot = std::shared_ptr<const Snapshot>", self.header)
        self.assertIn("instanceGeneration() const noexcept", self.header)
        self.assertIn("std::vector<Entry> m_entries", self.header)
        self.assertIn("std::lower_bound", self.source)
        self.assertIn("std::atomic_load_explicit", self.source)
        self.assertIn("std::atomic_store_explicit", self.source)
        self.assertIn("std::vector<Entry> entries = current->m_entries", self.source)
        snapshot = self.header.split("class Snapshot final", 1)[1].split(
            "using SharedSnapshot", 1
        )[0]
        self.assertIn("friend class War3PersistentGpuPackageProofCatalog", snapshot)
        self.assertEqual(snapshot.count("std::vector<Entry> m_entries"), 1)
        self.assertNotIn("std::vector<Entry>& entries()", snapshot)

    def test_single_writer_publishes_all_four_states(self) -> None:
        for token in (
            "Prepared = 0u",
            "UploadSubmitted",
            "UploadCompleted",
            "Invalidated",
            "publishPrepared",
            "publishUploadSubmitted",
            "publishUploadCompleted",
            "publishInvalidated",
            "WrongWriter",
            "bindOrCheckWriter",
            "std::thread::id m_writerThread",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.header + self.source)

    def test_value_contains_full_proofs_digest_revision_and_value_fence(self) -> None:
        value = self.header.split("struct Value", 1)[1].split("};", 1)[0]
        for token in (
            "GpuSkinStaticPackageProof packageProof",
            "GpuSkinStaticPrimitiveProof primitiveProof",
            "canonicalDigest",
            "publicationRevision",
            "ProducerFencePoint producerFence",
            "PublicationState state",
        ):
            with self.subTest(token=token):
                self.assertIn(token, value)
        fence = self.header.split("struct ProducerFencePoint", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("identity", fence)
        self.assertIn("value", fence)
        observation = self.header.split(
            "struct ProducerFenceObservation", 1
        )[1].split("};", 1)[0]
        for token in ("identity", "completedValue", "querySucceeded"):
            self.assertIn(token, observation)

    def test_catalog_is_cpu_value_only_and_not_integrated(self) -> None:
        implementation = self.header + self.source
        for forbidden in (
            "Rc<",
            "DxvkBufferSlice",
            "DxvkFence",
            "VkBuffer",
            "staticAtlasSlice",
            "bindVertex",
            "bindIndex",
            "ShadowArena",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, implementation)
        self.assertNotIn("War3PersistentGpuPackageProofCatalog", self.live)
        self.assertIn("kRuntimeInstantiated = false", self.header)
        self.assertIn("kBindsAtlas = false", self.header)
        self.assertIn("kConsumeAdmissionGranted = false", self.header)
        self.assertIn(
            "kStorePublicationAuthorityIntegrated = false", self.header
        )

    def test_ready_decision_is_private_and_validator_only(self) -> None:
        decision = self.header.split(
            "class PackageContentDecision final", 1
        )[1].split("enum class MutationResult", 1)[0]
        self.assertIn("private:", decision)
        self.assertIn("friend class War3PersistentGpuPackageProofCatalog", decision)
        private = decision.split("private:", 1)[1]
        self.assertIn("PackageContentDecision(\n", private)
        public = decision.split("private:", 1)[0]
        self.assertNotIn("PackageContentDecision(\n", public)
        self.assertIn("This is the only operation capable of constructing a ready decision", self.header)
        validator = self.source.split("validateDrawEvidence(\n", 1)[1]
        self.assertEqual(
            validator.count("true, Reason::Ready"),
            1,
        )
        self.assertIn("bool matches(", decision)
        self.assertIn("sameKey(m_key, key)", self.source)

    def test_validator_requires_exact_stage_route_tokens_and_completed_upload(self) -> None:
        validator = self.source.split("validateDrawEvidence(\n", 1)[1]
        for token in (
            "context.frameSerial == 0u",
            "context.policyRevision == 0u",
            "context.stage != kRequiredStage",
            "evidence.frameSerial != context.frameSerial",
            "evidence.policyRevision != context.policyRevision",
            "evidence.stage != context.stage",
            "evidence.catalogSnapshotRevision != catalog->revision()",
            "evidence.catalogInstanceGeneration != catalog->instanceGeneration()",
            "PublicationState::UploadCompleted",
            "validFence(value.producerFence)",
            "SameGpuSkinStaticPackageProof",
            "SameGpuSkinStaticPrimitiveProof",
            "sameStreamHashes",
            "samePrimitiveDomain",
            "validRoute(evidence)",
            "validExactToken(evidence.material)",
            "validExactToken(evidence.alpha)",
            "validExactToken(evidence.world)",
            "validExactToken(evidence.bounds)",
        ):
            with self.subTest(token=token):
                self.assertIn(token, validator)
        route = self.source.split("validRoute(\n", 1)[1].split(
            "sameStreamHashes(\n", 1
        )[0]
        for token in (
            "identityExact",
            "sourceExact",
            "validExactToken(evidence.identity)",
            "validExactToken(evidence.source)",
            "evidence.currentDrawSourceGeneration != 0u",
            "staticRigid",
            "!evidence.dynamic",
            "!evidence.skinned",
            "evidence.fresh",
            "!evidence.grace",
            "!evidence.blocker",
            "!evidence.rejected",
        ):
            with self.subTest(token=token):
                self.assertIn(token, route)

    def test_completion_requires_observed_fence_progress(self) -> None:
        transition = self.source.split("publishTransition(\n", 1)[1].split(
            "publishUploadSubmitted", 1
        )[0]
        for token in (
            "observation->querySucceeded",
            "observation->completedValue < value.producerFence.value",
            "CompletionNotObserved",
            "value.producerFence.identity != observation->identity",
        ):
            with self.subTest(token=token):
                self.assertIn(token, transition)
        self.assertNotIn(
            "publishUploadCompleted(\n    const Key& key, "
            "const ProducerFencePoint&",
            self.header,
        )

    def test_multi_primitive_publication_is_hard_blocked(self) -> None:
        self.assertIn("kMultiPrimitivePublicationGranted = false", self.header)
        prepared = self.source.split("validPreparedPackage(\n", 1)[1].split(
            "validExactToken", 1
        )[0]
        self.assertIn("proof.primitiveProofCount != 1u", prepared)
        self.assertIn(
            "PrimitiveProofAggregate(primitive) != proof.primitiveProofHash",
            prepared,
        )
        self.assertIn(
            "primitive.indexContentHash != proof.indexContentHash", prepared
        )
        self.assertIn("contradictoryIndex", self.test)
        self.assertIn("proof.staticByteLength != expectedStaticBytes", prepared)
        self.assertIn("staticEnd > proof.indexByteOffset", prepared)

    def test_ready_decision_is_bound_to_current_draw_context(self) -> None:
        matches = self.source.split(
            "PackageContentDecision::matches(\n", 1
        )[1].split("bindOrCheckWriter", 1)[0]
        for token in (
            "frameSerial == m_frameSerial",
            "catalogInstanceGeneration == m_catalogInstanceGeneration",
            "policyRevision == m_policyRevision",
            "stage == m_stage",
            "currentDrawSourceGeneration == m_currentDrawSourceGeneration",
            "identityToken == m_identityToken",
            "materialToken == m_materialToken",
            "worldToken == m_worldToken",
        ):
            with self.subTest(token=token):
                self.assertIn(token, matches)

    def test_digest_is_only_an_acceleration_and_full_compare_follows(self) -> None:
        validator = self.source.split("validateDrawEvidence(\n", 1)[1]
        digest = validator.index("evidence.canonicalDigest != value.canonicalDigest")
        package = validator.index("SameGpuSkinStaticPackageProof")
        primitive = validator.index("SameGpuSkinStaticPrimitiveProof")
        self.assertLess(digest, package)
        self.assertLess(digest, primitive)
        self.assertIn("digest-only validator", self.test)
        self.assertIn("PackageProofMismatch", self.test)

    def test_snapshot_revision_is_not_entry_publication_revision(self) -> None:
        self.assertIn("publicationRevision", self.header)
        self.assertIn("catalogSnapshotRevision", self.header)
        validator = self.source.split("validateDrawEvidence(\n", 1)[1]
        self.assertNotIn(
            "evidence.catalogSnapshotRevision != value.publicationRevision",
            validator,
        )
        self.assertIn(
            "TestOneSnapshotValidatesMultiplePublicationRevisions", self.test
        )
        self.assertIn(
            "firstEntry->value.publicationRevision !=", self.test
        )

    def test_meson_compiles_production_module_and_runnable_test(self) -> None:
        self.assertIn(
            "'war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp'",
            self.meson,
        )
        self.assertIn("'war3_persistent_gpu_package_proof_catalog_test'", self.meson)
        self.assertIn(
            "'war3/gpu_skin/tests/war3_persistent_gpu_package_proof_catalog_test.cpp'",
            self.meson,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
