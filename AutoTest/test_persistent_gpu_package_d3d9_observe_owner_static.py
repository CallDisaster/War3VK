#!/usr/bin/env python3
"""Contracts for the real but consumer-inert D3D9 package Observe owner."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
OWNER_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.h"
OWNER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_d3d9_observe_owner.cpp"
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
DEVICE_H = ROOT / "src/d3d9/d3d9_device.h"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"


class PersistentPackageD3D9ObserveOwnerContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.owner_h = OWNER_H.read_text(encoding="utf-8")
        cls.owner_cpp = OWNER_CPP.read_text(encoding="utf-8")
        cls.store_h = STORE_H.read_text(encoding="utf-8")
        cls.store_cpp = STORE_CPP.read_text(encoding="utf-8")
        cls.device_h = DEVICE_H.read_text(encoding="utf-8")
        cls.device_cpp = DEVICE_CPP.read_text(encoding="utf-8")
        cls.meson = MESON.read_text(encoding="utf-8")

    def test_owner_is_runtime_built_but_consumer_inert(self) -> None:
        for token in (
            "kRuntimeObserveUploadIntegrated = true",
            "kObserveOnly = true",
            "kRequiresNativeBridge = false",
            "kBindsGpuResources = false",
            "kMutatesCanonicalDraw = false",
            "kPublishesConsumerAuthority = false",
            "kPublishesConsumerLastUseFence = false",
        ):
            self.assertIn(token, self.owner_h)
        self.assertIn(
            "war3_persistent_gpu_package_d3d9_observe_owner.cpp",
            self.meson,
        )

    def test_runtime_gate_is_separate_and_defaults_off(self) -> None:
        mode = self.device_cpp.split(
            "War3PersistentPackageD3D9OwnerModeRuntime()", 1
        )[1].split("bool War3PopulateSubmitPermutationViewRuntime", 1)[0]
        self.assertIn(
            "DXVK_WAR3_PERSISTENT_GPU_PACKAGE_D3D9_OWNER_MODE", mode
        )
        self.assertIn(", 0u", mode)
        self.assertIn("Consume denied", self.device_cpp)

    def test_only_current_stage11_source_can_queue_snapshot(self) -> None:
        validation = self.owner_cpp.split(
            "validEvidenceAndSnapshot", 1
        )[1].split("bool War3PersistentGpuPackageD3D9ObserveOwner::beginFrame", 1)[0]
        for token in (
            "RecordedCurrentMapSource",
            "evidence.provesCurrentGameMemory",
            "snapshot->mapEpoch == evidence.mapEpoch",
            "snapshot->contentHash == evidence.immutableContentHash",
            "snapshot->immutableModelGeneration",
            "evidence.immutableModelGeneration",
            "!evidence.gpuBindingAllowed",
            "!evidence.commandRecordingAllowed",
            "!evidence.packagePublicationAllowed",
        ):
            self.assertIn(token, validation)
        observe = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner::observe(", 1
        )[1].split("takeSubmission()", 1)[0]
        self.assertIn("currentSnapshot.get() != snapshot.get()", observe)
        self.assertIn("findOrQueueStatic", observe)
        self.assertIn("kMaxPreparesPerFrame", observe)

    def test_owner_has_dedicated_fence_and_exact_submission_identity(self) -> None:
        constructor = self.owner_cpp.split(
            "War3PersistentGpuPackageD3D9ObserveOwner(", 1
        )[1].split("~War3PersistentGpuPackageD3D9ObserveOwner", 1)[0]
        self.assertIn("m_device->createFence", constructor)
        self.assertNotIn("War3GpuSkinManager", self.owner_h + self.owner_cpp)
        exact = self.owner_cpp.split("validSubmission(", 1)[1].split(
            "commitSubmission", 1
        )[0]
        for token in (
            "ownerAuthority",
            "submission.serial == m_openSubmission.serial",
            "submission.mapEpoch == m_openSubmission.mapEpoch",
            "submission.deviceEpoch == m_openSubmission.deviceEpoch",
            "submission.fenceValue == m_openSubmission.fenceValue",
            "submission.uploads.get() == m_openSubmission.uploads.get()",
        ):
            self.assertIn(token, exact)

    def test_copy_signal_then_atomic_store_retirement(self) -> None:
        integration = self.device_cpp.split(
            "War3PersistentPackageD3D9OwnerModeRuntime();", 1
        )[1].split("const auto& diagnostics", 1)[0]
        self.assertLess(
            integration.index("ctx->copyBuffer"),
            integration.index("ctx->signalFence"),
        )
        self.assertLess(
            integration.index("ctx->signalFence"),
            integration.index("commitSubmission"),
        )
        self.assertIn("VK_ACCESS_INDEX_READ_BIT", integration)

        batch = self.store_cpp.split("retireStaticUploads(", 1)[1].split(
            "completeRetiredStaticUpload", 1
        )[0]
        self.assertIn("prepared.reserve(uploads.size())", batch)
        self.assertIn("m_retiredStaticUploads.reserve", batch)
        self.assertLess(
            batch.index("m_retiredStaticUploads.reserve"),
            batch.index("GpuSkinStaticResourceState::UploadSubmitted"),
        )
        self.assertIn("uploads[earlier].key == upload.key", batch)

    def test_map_device_and_destruction_lifecycle_are_explicit(self) -> None:
        reset = self.device_cpp.split(
            "void D3D9DeviceEx::War3ResetGpuSkinMapEpoch()", 1
        )[1].split("bool D3D9DeviceEx::War3GpuSkinDeviceReady", 1)[0]
        self.assertIn("invalidateMapEpoch", reset)
        rebind = self.device_cpp.split(
            "void D3D9DeviceEx::War3RetryGpuSkinDeviceRebind()", 1
        )[1].split("void D3D9DeviceEx::War3ResetGpuSkinDeviceEpoch", 1)[0]
        self.assertGreaterEqual(rebind.count("invalidateDeviceEpoch"), 2)
        destructor = self.device_cpp.split("D3D9DeviceEx::~D3D9DeviceEx", 1)[1].split(
            "void D3D9DeviceEx::War3AttachGpuSkinNativeBridge", 1
        )[0]
        self.assertLess(
            destructor.index("waitForIdle"),
            destructor.index("pollProducerCompletions"),
        )
        self.assertIn("m_war3PersistentPackageD3D9ObserveOwner", self.device_h)


if __name__ == "__main__":
    unittest.main(verbosity=2)
