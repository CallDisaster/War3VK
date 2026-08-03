from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODEL_H = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
MODEL_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
GEN_H = ROOT / "src/d3d9/war3/model/war3_immutable_model_generation.h"
IMM_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_immutable.h"
IMM_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_immutable.cpp"
RES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h"
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
MANAGER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.cpp"
CATALOG_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_proof_catalog.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class ImmutablePublicationAuthorityContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.model_h = MODEL_H.read_text(encoding="utf-8")
        cls.model_cpp = MODEL_CPP.read_text(encoding="utf-8")
        cls.gen_h = GEN_H.read_text(encoding="utf-8")
        cls.imm_h = IMM_H.read_text(encoding="utf-8")
        cls.imm_cpp = IMM_CPP.read_text(encoding="utf-8")
        cls.res_h = RES_H.read_text(encoding="utf-8")
        cls.store_h = STORE_H.read_text(encoding="utf-8")
        cls.store_cpp = STORE_CPP.read_text(encoding="utf-8")
        cls.manager_cpp = MANAGER_CPP.read_text(encoding="utf-8")
        cls.catalog_cpp = CATALOG_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_cache_generation_is_process_monotonic_and_never_wraps(self) -> None:
        self.assertIn("ImmutableModelGenerationIssuer m_immutableModelGenerations", self.model_h)
        self.assertIn("std::numeric_limits<uint64_t>::max()", self.gen_h)
        self.assertIn("return 0u", self.gen_h)
        self.assertNotIn("m_immutableModelGenerations =", self.model_cpp)
        self.assertIn("m_immutableModelGenerations.issue()", self.model_cpp)

    def test_float_payload_identity_is_bit_exact(self) -> None:
        comparator = self.model_h.split(
            "SameShadowGeosetFloatPayloadBytes", 1
        )[1].split("// Exact immutable payload", 1)[0]
        self.assertIn("std::memcmp", comparator)
        payload = self.model_h.split(
            "SameShadowGeosetImmutableConsumerPayload", 1
        )[1].split("using ShadowGeosetResourceSnapshot", 1)[0]
        for token in ("lhs.positions", "lhs.normals", "uvPairs"):
            self.assertIn(token, payload)

    def test_capture_has_failure_tombstone_and_alias_fast_path_is_exact(self) -> None:
        for state in ("NotAttempted", "Complete", "AttemptedFailed"):
            self.assertIn(state, self.model_h)
        runtime = self.model_cpp.split("noteRuntimeGeosetBinding(", 1)[1].split(
            "findGeosetByPtr", 1
        )[0]
        self.assertIn("geosetRecord.geosetDataPtr == runtimeGeosetDataPtr", runtime)
        create = self.model_cpp.split("recordGeosetCreate(", 1)[1].split(
            "noteModelResourceBinding", 1
        )[0]
        self.assertIn("aliasIdentityTransition", create)
        self.assertGreaterEqual(create.count("!aliasIdentityTransition"), 2)
        store = self.model_cpp.split("storeGeosetRecord(", 1)[1].split(
            "storeModelRecord", 1
        )[0]
        self.assertIn("sourceIdentityTransition", store)
        self.assertIn("forceFreshGeneration = sourceIdentityTransition", store)
        self.assertIn("pendingAliasResolution = !sourceIdentityTransition", store)
        self.assertIn("sourceIdentityTransition || pendingAliasResolution", store)
        self.assertIn("existingByGeoset->immutableModelGeneration == 0u", store)
        self.assertGreaterEqual(store.count("!forceFreshGeneration"), 2)
        self.assertIn("AttemptedFailed", store)
        self.assertIn("ReplaceGeosetImmutablePayload", store)

    def test_capture_requires_every_count_read_before_complete(self) -> None:
        capture_region = self.model_cpp.split("bool CaptureGeosetRecord(", 1)[1].split(
            "bool TryReadGeosetDataPtr", 1
        )[0]
        for token in (
            "vertexCountRead", "normalCountRead", "vertexGroupCountRead",
            "uvLayerCountRead", "primitiveCountRead", "indexCountRead",
            "matrixGroupCountRead", "matrixIndexCountRead",
        ):
            self.assertEqual(capture_region.count(token), 4)
        self.assertEqual(capture_region.count("rawCountsReadable"), 4)

    def test_model_readiness_uses_published_complete_generation(self) -> None:
        payload = self.model_h.split(
            "bool hasCompleteImmutableConsumerPayload() const", 1
        )[1].split("bool readyForShadowConsumer() const", 1)[0]
        ready = self.model_h.split("bool readyForShadowConsumer() const", 1)[1].split(
            "bool hasSkinningData", 1
        )[0]
        self.assertIn("ShadowGeosetImmutableCaptureStatus::Complete", payload)
        self.assertIn("immutableModelGeneration != 0u", ready)
        hydrate = self.model_cpp.split("hydrateGeosetByKnownPtrs(", 1)[1].split(
            "findModelGeoset", 1
        )[0]
        self.assertIn("hydrated.hasCompleteImmutableConsumerPayload()", hydrate)
        self.assertIn("published->geosetDataPtr", hydrate)
        self.assertIn("aliasRequiresCapture", hydrate)
        self.assertIn("!aliasIdentityTransition && !aliasRequiresCapture", hydrate)
        self.assertGreaterEqual(self.model_cpp.count(
            "const ShadowGeosetResourceSnapshot published ="), 3)
        self.assertIn("published->readyForShadowConsumer()", self.model_cpp)

    def test_alias_readers_materialize_current_canonical_publication(self) -> None:
        helper = self.model_cpp.split(
            "materializeGeosetAliasRecordLocked(", 1
        )[1].split("ShadowModelResourceCache::storeGeosetRecord", 1)[0]
        self.assertIn("m_byGeosetData.find(alias.geosetDataPtr)", helper)
        self.assertIn("!alias.readyForShadowConsumer()", helper)
        self.assertIn("materializeGeosetDataRecordLocked", helper)
        self.assertIn("MergeGeosetMetadata(result, alias)", helper)
        self.assertIn("immutableModelGeneration = 0u", helper)
        for function, end in (
            ("findGeosetByPtr(", "findGeosetByData("),
            ("findModelGeoset(", "findRuntimeModelGeoset("),
            ("findRuntimeModelGeoset(", "findRuntimeModelOwner("),
            ("snapshotGeosets() const", "snapshotModels() const"),
        ):
            body = self.model_cpp.split(function, 1)[1].split(end, 1)[0]
            self.assertIn("materializeGeosetAliasRecordLocked", body)
        count_ready = self.model_cpp.split("uint32_t CountReadyGeosets", 1)[1].split(
            "int ScoreRuntimeOwnerCandidate", 1
        )[0]
        self.assertIn("byGeosetData.find(alias.geosetDataPtr)", count_ready)
        self.assertIn("canonical->second->readyForShadowConsumer()", count_ready)

    def test_store_accepts_only_cache_snapshot_and_fixed_layout(self) -> None:
        self.assertIn(
            "kPersistentGpuPackageStaticLayoutGeneration = 1u", self.imm_h
        )
        queue = self.store_cpp.split("findOrQueueStatic(", 1)[1].split(
            "probeStatic", 1
        )[0]
        self.assertIn("cacheSnapshot.get() != record.get()", queue)
        self.assertIn("layoutGeneration != kStaticPackingLayoutGeneration", queue)
        self.assertIn("BuildPersistentGpuPackageImmutableProof", queue)

    def test_frozen_descriptor_is_private_and_store_instance_is_uint64(self) -> None:
        frozen = self.res_h.split("class GpuSkinStaticFrozenPackage", 1)[1].split(
            "struct GpuSkinStaticResource", 1
        )[0]
        self.assertIn("private:", frozen)
        self.assertIn("friend class War3PersistentGpuPackageStore", frozen)
        self.assertIn("uint64_t m_storeInstanceAuthority", frozen)
        self.assertIn("m_snapshotIdentity", frozen)
        self.assertIn("m_immutableProof", frozen)
        self.assertIn("!std::is_default_constructible_v", self.res_h)

    def test_cpu_and_staging_proofs_cover_all_payload_classes(self) -> None:
        for token in (
            "positionContentHash", "normalContentHash",
            "vertexGroupContentHash", "uv0ContentHash", "uv1ContentHash",
            "indexContentHash", "matrixGroupContentHash",
            "matrixIndexContentHash", "primitiveProofHash", "localBoundsHash",
        ):
            self.assertIn(token, self.imm_h)
            self.assertIn(token, self.imm_cpp)
        self.assertIn("ValidatePersistentGpuPackagePackedBytes", self.store_cpp)
        self.assertIn("ValidateGpuSkinStaticFrozenPayload", self.store_cpp)

    def test_unfinished_authorities_remain_unintegrated_declarations(self) -> None:
        for token in (
            "kCurrentStageSourceAuthorityIntegrated = false",
            "kRecordingThreadOwnershipIntegrated = false",
            "kProducerCompletionAuthorityIntegrated = false",
            "kFrozenGpuSliceRunnableIntegrated = false",
        ):
            self.assertIn(token, self.store_h)
        retire = self.store_cpp.split("retireStaticUpload(", 1)[1].split(
            "completeRetiredStaticUpload", 1
        )[0]
        self.assertIn("return exactPendingUpload", retire)
        self.assertIn(
            "GpuSkinStaticResourceState::UploadSubmitted", retire
        )
        self.assertNotIn("GpuSkinStaticResourceState::Ready", retire)

    def test_manager_hint_revalidates_current_cache_and_store_probe(self) -> None:
        hint = self.manager_cpp.split("bypassStaticHint", 1)[1].split(
            "if (!usedBypassStaticHint)", 1
        )[0]
        self.assertIn("findGeosetStampByData", hint)
        self.assertIn("probeStatic", hint)
        self.assertIn("immutableModelGeneration", hint)

    def test_catalog_digest_and_validator_bind_model_generation(self) -> None:
        self.assertIn("HashU64(hash, proof.immutableModelGeneration)", self.catalog_cpp)
        self.assertIn(
            "proof.immutableModelGeneration != key.immutableModelGeneration",
            self.catalog_cpp,
        )

    def test_runnable_is_registered_without_store_or_device(self) -> None:
        target = self.meson.split(
            "war3_persistent_gpu_package_immutable_test = executable", 1
        )[1].split("test(", 1)[0]
        self.assertIn("war3_persistent_gpu_package_immutable.cpp", target)
        self.assertIn("war3_persistent_gpu_package_immutable_test.cpp", target)
        self.assertNotIn("war3_persistent_gpu_package_store.cpp", target)


if __name__ == "__main__":
    unittest.main(verbosity=2)
