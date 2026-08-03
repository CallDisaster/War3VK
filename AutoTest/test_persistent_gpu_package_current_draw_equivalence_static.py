#!/usr/bin/env python3
"""Contracts for the exact current-draw/package Observe equivalence gate."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
IMM_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_immutable.h"
IMM_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_immutable.cpp"
OWNER_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.h"
OWNER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.cpp"
RESOURCES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
DIAG_H = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h"
DIAG_CPP = ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp"
CONTROL_CPP = ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class CurrentDrawPackageEquivalenceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.device_h = DEVICE_H.read_text(encoding="utf-8")
        cls.device_cpp = DEVICE_CPP.read_text(encoding="utf-8")
        cls.imm_h = IMM_H.read_text(encoding="utf-8")
        cls.imm_cpp = IMM_CPP.read_text(encoding="utf-8")
        cls.owner_h = OWNER_H.read_text(encoding="utf-8")
        cls.owner_cpp = OWNER_CPP.read_text(encoding="utf-8")
        cls.resources_h = RESOURCES_H.read_text(encoding="utf-8")
        cls.store_cpp = STORE_CPP.read_text(encoding="utf-8")
        cls.diag_h = DIAG_H.read_text(encoding="utf-8")
        cls.diag_cpp = DIAG_CPP.read_text(encoding="utf-8")
        cls.control_cpp = CONTROL_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_gate_is_independent_default_off_and_consume_denied(self) -> None:
        gate = self.device_cpp.split(
            "War3PersistentPackageCurrentDrawEquivalenceModeRuntime()", 1
        )[1].split("bool War3PopulateSubmitPermutationViewRuntime", 1)[0]
        self.assertIn(
            "DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_", gate
        )
        self.assertIn("EQUIVALENCE_MODE", gate)
        self.assertIn(", 0u", gate)
        self.assertIn("PackageCurrentDraw: Consume denied", self.device_cpp)

    def test_shared_byte_hash_and_strided_float3_use_one_domain(self) -> None:
        for token in (
            "kPersistentGpuPackageContentHashSeed",
            "ContinuePersistentGpuPackageContentHash",
            "HashPersistentGpuPackageContent",
            "HashPersistentGpuPackageStridedFloat3",
        ):
            self.assertIn(token, self.imm_h)
            self.assertIn(token, self.imm_cpp)
        immutable_builder = self.imm_cpp.split(
            "BuildPersistentGpuPackageImmutableProof", 1
        )[1]
        self.assertIn("HashBytes", immutable_builder)
        self.assertIn("HashPersistentGpuPackageContent", self.imm_cpp)

    def test_frame_entry_proof_is_invalidated_and_generation_sealed(self) -> None:
        self.assertIn("persistentPackageCurrentDrawProof = {}", self.device_h)
        self.assertGreaterEqual(
            self.device_cpp.count("persistentPackageCurrentDrawProof = {}"), 2
        )
        capture = self.device_cpp.split(
            "DXVK War3PackageCurrentDraw: Consume denied", 1
        )[1].split("War3ShadowDrawTimeCapturePhase::GpuSkinSettlement", 1)[0]
        for token in (
            "proof.frameSerial = m_war3ShadowPersistentFrameSerial",
            "proof.mapEpoch = m_war3GpuSkinMapEpoch",
            "proof.deviceEpoch = m_war3GpuSkinDeviceEpoch",
            "proof.positionIdentityGeneration",
            "proof.positionAllocationGeneration",
            "proof.positionContentGeneration",
            "proof.indexIdentityGeneration",
            "proof.indexAllocationGeneration",
            "proof.indexContentGeneration",
            "proof.sealed = true",
        ):
            self.assertIn(token, capture)

    def test_hash_walk_is_host_cached_and_after_cheap_rejects(self) -> None:
        capture = self.device_cpp.split(
            "const bool hashCandidate = proof.rigidStatic", 1
        )[1].split("War3ShadowDrawTimeCapturePhase::GpuSkinSettlement", 1)[0]
        for token in (
            "proof.opaqueMaterial",
            "!proof.gpuSkinBacked",
            "!proof.vertexBlendEnabled",
            "proof.uint16Indices",
            "proof.exactContiguousVertexRange",
            "proof.positionHostCached",
            "proof.indexHostCached",
            "HashPersistentGpuPackageStridedFloat3",
            "HashPersistentGpuPackageContent",
        ):
            self.assertIn(token, capture)
        self.assertLess(
            capture.index("if (hashCandidate)"),
            capture.index("HashPersistentGpuPackageStridedFloat3"),
        )
        self.assertGreaterEqual(
            self.device_cpp.count("VK_MEMORY_PROPERTY_HOST_CACHED_BIT"), 3
        )

    def test_write_combined_index_proof_is_observe_only_and_bounded(self) -> None:
        bounded = self.device_cpp.split(
            "Observe-only proof for Warcraft's WRITEONLY index buffers", 1
        )[1].split("// 计算这个 draw 实际使用的 vertex range", 1)[0]
        self.assertIn("persistentPackageObserveEnabled", self.device_cpp)
        for token in (
            "kMaxIndexBytesPerDraw",
            "kMaxIndexBytesPerFrame",
            "proofTickBudget",
            "VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT",
            "BuildWar3CpuReadableBufferSpan",
            "s_packageIndexScratch",
            "packageObserveUncachedIndexScan = true",
        ):
            self.assertIn(token, bounded)
        self.assertIn(
            "never fed back into vRangeStart, Arena allocation or command",
            bounded,
        )
        proof = self.device_cpp.split(
            "proof.indexBoundedObserveReadable =", 1
        )[1].split("War3ShadowDrawTimeCapturePhase::GpuSkinSettlement", 1)[0]
        self.assertIn("proof.boundedIndexScanBytes", proof)
        self.assertIn("proof.boundedIndexScanTicks", proof)
        self.assertIn("proof.positionBoundedObserveReadable", proof)
        self.assertIn("s_packagePositionScratch", proof)
        self.assertIn("proof.boundedPositionCopyBytes", proof)
        self.assertIn("proof.boundedPositionCopyTicks", proof)
        self.assertIn("kMaxPositionHashBytesPerFrame", proof)
        self.assertIn("m_war3PersistentPackageProofTicksThisFrame", proof)
        self.assertIn("!entry.pathBlocker", self.device_cpp)
        self.assertIn("draw.indexBoundedObserveReadable", self.imm_cpp)
        self.assertIn("draw.positionBoundedObserveReadable", self.imm_cpp)

    def test_bounded_proof_targets_previous_final_capture_ordinal(self) -> None:
        for token in (
            "packageCaptureOrdinal",
            "packageLastSubmittedCaptureOrdinal",
        ):
            self.assertIn(token, self.device_h)
            self.assertIn(token, self.device_cpp)
        bounded = self.device_cpp.split(
            "Observe-only proof for Warcraft's WRITEONLY index buffers", 1
        )[1].split("// 计算这个 draw 实际使用的 vertex range", 1)[0]
        self.assertIn("predictedFinalCapture", bounded)
        self.assertIn(
            "previousPackageEntry->second.exactSubmittedFrameSerial", bounded
        )
        self.assertIn(
            "packageCurrentCaptureOrdinal ==", bounded
        )
        self.assertIn(
            "entry.packageLastSubmittedCaptureOrdinal = entry.packageCaptureOrdinal",
            self.device_cpp,
        )

    def test_owner_revalidates_store_publication_and_only_reports_would_use(self) -> None:
        observe = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner::observe(", 1
        )[1].split("takeSubmission()", 1)[0]
        for token in (
            "frozen->snapshotIdentity() == currentSnapshot.get()",
            "EvaluatePersistentGpuPackageCurrentDrawEquivalence",
            "resource.readyValidationAuthority",
            "result.fullyEquivalent = true",
            "result.wouldUseConsumerMask = result.eligibleConsumerMask",
        ):
            self.assertIn(token, observe)
        self.assertIn("entry.persistentPackageCurrentDrawProof", self.device_cpp)
        for token in (
            "bool gpuBindingAllowed = false",
            "bool drawMutationAllowed = false",
            "bool consumerAuthorityPublished = false",
        ):
            self.assertIn(token, self.owner_h)
        self.assertNotIn("packageSlice()", self.device_cpp)

    def test_multi_primitive_subrange_is_selected_by_exact_content(self) -> None:
        capture = self.device_cpp.split(
            "proof.sourceVertexFirst = packageVertexRangeValid", 1
        )[1].split("War3ShadowDrawTimeCapturePhase::GpuSkinSettlement", 1)[0]
        self.assertIn("proof.sourceFirstIndex = StartVal", capture)
        self.assertIn("positionRangeOffset", capture)
        self.assertNotIn("StartVal == 0u", capture)
        observe = self.owner_cpp.split(
            "auto selected = primitiveProofs.end()", 1
        )[1].split("EvaluatePersistentGpuPackageCurrentDrawEquivalence", 1)[0]
        for token in (
            "it->indexContentHash == currentDraw.indexContentHash",
            "ambiguousSelection",
            "record.positions.data() + size_t(firstFloat)",
            "currentPackage.primitiveSelected",
        ):
            self.assertIn(token, observe)

    def test_rejected_draws_bypass_store_and_snapshot_lookup(self) -> None:
        device_owner = self.device_cpp.split(
            "void D3D9DeviceEx::War3ObservePersistentPackageD3D9Owner", 1
        )[1].split("void D3D9DeviceEx::War3Invalidate", 1)[0]
        self.assertIn("currentDrawRejectedBeforePackage", device_owner)
        self.assertIn("ShadowGeosetResourceSnapshot{}", device_owner)
        observe = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner::observe(", 1
        )[1].split("takeSubmission()", 1)[0]
        self.assertLess(
            observe.index("currentDrawPreflight"),
            observe.index("findGeosetSnapshotByData"),
        )
        self.assertLess(
            observe.index("CurrentDrawRejected"),
            observe.index("m_store->findOrQueueStatic"),
        )

    def test_ready_validation_authority_removes_full_hot_path_rewalk(self) -> None:
        self.assertIn("readyValidationAuthority", self.resources_h)
        hot_lookup = self.store_cpp.split("findOrQueueStatic(", 1)[1].split(
            "prepareQueuedStaticResources", 1
        )[0]
        self.assertIn(
            "readyValidationAuthority == m_instanceAuthority", hot_lookup
        )
        self.assertNotIn("ValidateGpuSkinStaticPackage", hot_lookup)
        completion = self.store_cpp.split(
            "completeRetiredStaticUpload(", 1
        )[1].split("staticAtlasSlice()", 1)[0]
        self.assertLess(
            completion.index("ValidateGpuSkinStaticFrozenPayload"),
            completion.index("readyValidationAuthority = m_instanceAuthority"),
        )
        self.assertLess(
            completion.index("ValidateGpuSkinStaticPackage"),
            completion.index("readyValidationAuthority = m_instanceAuthority"),
        )
        observe = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner::observe(", 1
        )[1].split("takeSubmission()", 1)[0]
        self.assertIn("resource.readyValidationAuthority", observe)
        self.assertNotIn("ValidateGpuSkinStaticPackage(resource)", observe)

    def test_reject_taxonomy_and_runtime_status_are_complete(self) -> None:
        for token in (
            "NotRigidStatic",
            "MaterialRejected",
            "SkinningRouteRejected",
            "GeometryContractRejected",
            "CpuSourceUnavailable",
            "SourceGenerationMissing",
            "PackageNotReady",
            "PackageInvalid",
            "SnapshotMismatch",
            "PrimitiveSelectionRejected",
            "MultiPrimitiveRejected",
            "PackageLayoutMismatch",
            "PositionContentMismatch",
            "IndexContentMismatch",
            "PrimitiveMismatch",
            "ExactMatch",
        ):
            self.assertIn(token, self.imm_h)
            self.assertIn(token, self.owner_cpp)
        for token in (
            "persistentPackageCurrentDrawConfiguredMode",
            "persistentPackageCurrentDrawEffectiveMode",
            "persistentPackageCurrentDrawObservations",
            "persistentPackageCurrentDrawExactMatches",
            "persistentPackageCurrentDrawWouldUseCsm",
            "persistentPackageCurrentDrawRejected",
            "persistentPackageCurrentDrawBoundedIndexScans",
            "persistentPackageCurrentDrawBoundedIndexScanBytes",
            "persistentPackageCurrentDrawBoundedIndexScanTicks",
            "persistentPackageCurrentDrawBoundedPositionCopies",
            "persistentPackageCurrentDrawBoundedPositionCopyBytes",
            "persistentPackageCurrentDrawBoundedPositionCopyTicks",
            "persistentPackageCurrentDrawContentHashBytes",
            "persistentPackageCurrentDrawContentHashTicks",
            "persistentPackageCurrentDrawProofBudgetRejected",
            "persistentPackageCaptureBoundedIndexScans",
            "persistentPackageCaptureContentHashTicks",
            "persistentPackageCaptureTimerFrequency",
            "persistentPackageCurrentDrawCpuSourceUnavailable",
            "persistentPackageCurrentDrawMultiPrimitiveRejected",
            "persistentPackageCurrentDrawPositionMismatch",
            "persistentPackageCurrentDrawIndexMismatch",
            "persistentPackageCurrentDrawLastDisposition",
        ):
            self.assertIn(token, self.diag_h)
            self.assertIn(token, self.diag_cpp)
            self.assertIn(token, self.control_cpp)

    def test_geometry_rejection_has_non_overlapping_runtime_breakdown(self) -> None:
        for token in (
            "currentDrawGeometryNonIndexed",
            "currentDrawGeometryNonTriangleList",
            "currentDrawGeometryNonUint16",
            "currentDrawGeometryIndexDomainUnknown",
            "currentDrawGeometryFullDomainFallback",
            "currentDrawGeometryNonContiguousRange",
            "currentDrawGeometryPositionNotFloat3",
            "currentDrawGeometryVertexCountMismatch",
        ):
            self.assertIn(token, self.owner_h)
            self.assertIn(token, self.owner_cpp)
            self.assertIn(token, self.device_cpp)

    def test_value_runnable_is_registered_without_store_or_renderer(self) -> None:
        target = self.meson.split(
            "war3_persistent_gpu_package_current_draw_equivalence_test = executable",
            1,
        )[1].split("test(", 1)[0]
        self.assertIn("war3_persistent_gpu_package_immutable.cpp", target)
        self.assertIn(
            "war3_persistent_gpu_package_current_draw_equivalence_test.cpp",
            target,
        )
        self.assertNotIn("war3_persistent_gpu_package_store.cpp", target)
        self.assertNotIn("d3d9_device.cpp", target)


if __name__ == "__main__":
    unittest.main(verbosity=2)
