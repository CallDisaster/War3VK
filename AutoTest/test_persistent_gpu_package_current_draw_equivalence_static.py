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
            "proof.zeroBasedVertexRange",
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

    def test_owner_revalidates_frozen_payload_and_only_reports_would_use(self) -> None:
        observe = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner::observe(", 1
        )[1].split("takeSubmission()", 1)[0]
        for token in (
            "frozen->snapshotIdentity() == currentSnapshot.get()",
            "EvaluatePersistentGpuPackageCurrentDrawEquivalence",
            "ValidateGpuSkinStaticFrozenPayload",
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
            "persistentPackageCurrentDrawCpuSourceUnavailable",
            "persistentPackageCurrentDrawMultiPrimitiveRejected",
            "persistentPackageCurrentDrawPositionMismatch",
            "persistentPackageCurrentDrawIndexMismatch",
            "persistentPackageCurrentDrawLastDisposition",
        ):
            self.assertIn(token, self.diag_h)
            self.assertIn(token, self.diag_cpp)
            self.assertIn(token, self.control_cpp)

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
