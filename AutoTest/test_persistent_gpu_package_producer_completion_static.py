#!/usr/bin/env python3
"""Static contracts for fence-proven persistent package publication."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
TYPES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_types.h"
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
MANAGER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.cpp"
PERF_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"


class PersistentPackageProducerCompletionContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.types = TYPES_H.read_text(encoding="utf-8")
        cls.store_h = STORE_H.read_text(encoding="utf-8")
        cls.store_cpp = STORE_CPP.read_text(encoding="utf-8")
        cls.manager = MANAGER_CPP.read_text(encoding="utf-8")
        cls.perf = PERF_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")

    def test_submission_is_a_distinct_non_ready_state(self) -> None:
        state = self.types.split(
            "enum class GpuSkinStaticResourceState", 1
        )[1].split("};", 1)[0]
        self.assertIn("PendingUpload = 0", state)
        self.assertIn("UploadSubmitted = 1", state)
        self.assertIn("Ready = 2", state)
        self.assertIn("Invalid = 3", state)

        retire = self.store_cpp.split("retireStaticUpload(", 1)[1].split(
            "completeRetiredStaticUpload", 1
        )[0]
        self.assertIn("GpuSkinStaticResourceState::UploadSubmitted", retire)
        self.assertNotIn("GpuSkinStaticResourceState::Ready", retire)
        self.assertLess(
            retire.index("m_retiredStaticUploads.push_back"),
            retire.index("GpuSkinStaticResourceState::UploadSubmitted"),
        )

    def test_only_exact_retirement_owns_ready_publication(self) -> None:
        retirement = self.store_h.split(
            "struct RetiredStaticUpload", 1
        )[1].split("};", 1)[0]
        self.assertIn("publicationResource", retirement)
        self.assertIn("publishesReady = false", retirement)

        retire = self.store_cpp.split("retireStaticUpload(", 1)[1].split(
            "completeRetiredStaticUpload", 1
        )[0]
        for token in (
            "found->second->key == upload.key",
            "pending.key == upload.key",
            "pending.residencyCensus == upload.residencyCensus",
            "pending.source.buffer() == upload.source.buffer()",
            "pending.destination.buffer() == upload.destination.buffer()",
            "ValidateGpuSkinStaticFrozenPayload",
            "retirement->publicationResource = found->second",
            "retirement->publishesReady = true",
        ):
            with self.subTest(token=token):
                self.assertIn(token, retire)

    def test_fence_poll_precedes_completion_and_ready_transition(self) -> None:
        poll = self.store_h.split("void pollRetired", 1)[1].split(
            "void fillResidencySnapshot", 1
        )[0]
        self.assertLess(
            poll.index("getFenceValue((*it)->fence) >= (*it)->retireValue"),
            poll.index("completeRetiredStaticUpload"),
        )

        complete = self.store_cpp.split(
            "completeRetiredStaticUpload(", 1
        )[1].split("staticAtlasSlice()", 1)[0]
        for token in (
            "m_mapEpoch == retirement.key.mapEpoch",
            "m_deviceEpoch == retirement.key.deviceEpoch",
            "active->second == resource",
            "GpuSkinStaticResourceState::UploadSubmitted",
            "retirement.destination.buffer() == resource->packageSlice.buffer()",
            "ValidateGpuSkinStaticFrozenPayload",
            "GpuSkinStaticResourceState::Ready",
            "ValidateGpuSkinStaticPackage",
        ):
            with self.subTest(token=token):
                self.assertIn(token, complete)
        self.assertLess(
            complete.index("exactSubmittedPayload"),
            complete.index("GpuSkinStaticResourceState::Ready"),
        )

    def test_epoch_clear_cannot_late_publish_old_resource(self) -> None:
        complete = self.store_cpp.split(
            "completeRetiredStaticUpload(", 1
        )[1].split("staticAtlasSlice()", 1)[0]
        self.assertIn("active != m_staticResources.end()", complete)
        self.assertIn("resource->state = GpuSkinStaticResourceState::Invalid", complete)
        clear = self.store_cpp.split("clearEpochResources()", 2)[2]
        self.assertIn("m_staticResources.clear()", clear)
        self.assertNotIn("m_retiredStaticUploads.clear()", clear)

    def test_consumers_accept_ready_only_and_submitted_is_observable(self) -> None:
        self.assertGreaterEqual(
            self.manager.count("GpuSkinStaticResourceState::Ready"), 5
        )
        self.assertIn("staticSubmittedRecords", self.store_cpp)
        self.assertIn('\\"staticSubmittedRecords\\"', self.perf)
        self.assertIn("staticUploadsCompleted", self.types)
        self.assertIn("staticUploadCompletionsRejected", self.types)
        lifetime = self.device.split(
            '"DXVK War3GpuSkin: diag lifetime', 1
        )[1].split('war3dbg::Print("%s\\n", diagLine)', 1)[0]
        self.assertIn('"completion=%llu/%llu "', lifetime)
        self.assertIn("manager.resources.staticUploadsCompleted", lifetime)
        self.assertIn(
            "manager.resources.staticUploadCompletionsRejected", lifetime
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
