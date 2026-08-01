#!/usr/bin/env python3
"""P0 ownership contracts for the extracted persistent GPU package store."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
STORE_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.h"
STORE_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_store.cpp"
RES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h"
RES_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MANAGER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.cpp"


class PersistentPackageOwnerContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.store_h = STORE_H.read_text(encoding="utf-8")
        cls.store_cpp = STORE_CPP.read_text(encoding="utf-8")
        cls.resources_h = RES_H.read_text(encoding="utf-8")
        cls.resources_cpp = RES_CPP.read_text(encoding="utf-8")
        cls.device_cpp = DEVICE_CPP.read_text(encoding="utf-8")
        cls.manager_cpp = MANAGER_CPP.read_text(encoding="utf-8")

    def test_store_is_the_only_static_package_state_owner(self) -> None:
        for token in (
            "m_staticResources",
            "m_staticMisses",
            "m_readyStaticUploads",
            "m_staticAtlas",
            "m_staticCursor",
            "m_retiredStaticUploads",
            "m_nextStaticPackageGeneration",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.store_h)
                self.assertNotIn(token, self.resources_h)

    def test_resources_preserves_public_api_and_delegates(self) -> None:
        self.assertIn(
            "std::unique_ptr<War3PersistentGpuPackageStore> "
            "m_persistentPackages;",
            self.resources_h,
        )
        for method in (
            "findOrQueueStatic",
            "probeStatic",
            "prepareQueuedStaticResources",
            "takeStaticUploads",
            "retireStaticUpload",
            "staticAtlasSlice",
        ):
            with self.subTest(method=method):
                self.assertIn(
                    f"m_persistentPackages->{method}", self.resources_cpp
                )

    def test_p0_store_has_no_manager_or_cross_frame_route_authority(self) -> None:
        store_implementation = self.store_h + self.store_cpp
        for forbidden in (
            "std::mutex",
            "War3GpuSkinManager",
            "allocateOutput",
            "OutputLeaseDesc",
            "War3GpuSkinCompute",
            "War3GpuSkinNativeBridge",
            "native kernel",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, store_implementation)
        self.assertIn("kD3D9SharedOwnerEnabled = false", self.store_h)
        self.assertIn("kRequiresNativeBridge = false", self.store_h)
        self.assertIn(
            "kProducerRetirementSurvivesEpochClear = true", self.store_h
        )
        self.assertIn("kCrossEpochRetirementSafe = false", self.store_h)

    def test_producer_retirement_survives_epoch_clear(self) -> None:
        retired = self.store_h.split("struct RetiredStaticUpload", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("DxvkBufferSlice source;", retired)
        self.assertIn("DxvkBufferSlice destination;", retired)
        self.assertIn("destinationResidencyCensus", retired)

        retire = self.store_cpp.split("retireStaticUpload(", 1)[1].split(
            "staticAtlasSlice()", 1
        )[0]
        self.assertIn("upload.source, upload.destination", retire)
        self.assertIn("m_staticAtlasCensus", retire)

        clear = self.store_cpp.split(
            "void War3PersistentGpuPackageStore::clearEpochResources()", 1
        )[1]
        self.assertNotIn("m_retiredStaticUploads.clear()", clear)

        destructor = self.store_cpp.split(
            "War3PersistentGpuPackageStore::~War3PersistentGpuPackageStore()",
            1,
        )[1].split("bool War3PersistentGpuPackageStore::beginFrame", 1)[0]
        self.assertIn("deferOutstandingProducerRetirements()", destructor)

        deferred = self.store_cpp.split(
            "deferOutstandingProducerRetirements()", 2
        )[2]
        self.assertIn("fence->enqueueWait(retireValue", deferred)
        self.assertIn("delete payload", deferred)
        self.assertIn("Intentionally leak", deferred)

    def test_shared_owner_remains_blocked_without_consumer_use_fence(self) -> None:
        self.assertIn(
            "static_assert(!War3PersistentGpuPackageStore::"
            "kCrossEpochRetirementSafe)",
            self.store_h,
        )
        self.assertNotIn("retireStaticAtlasUse", self.store_h)

    def test_d3d9_does_not_construct_or_name_the_store(self) -> None:
        self.assertNotIn("War3PersistentGpuPackageStore", self.device_cpp)

    def test_disabled_gate_still_precedes_manager_construction(self) -> None:
        attach = self.device_cpp.split(
            "void D3D9DeviceEx::War3AttachGpuSkinNativeBridge", 1
        )[1].split("\nvoid D3D9DeviceEx::", 1)[0]
        disabled = attach.index(
            "if (config.mode == GpuSkinMode::Disabled)\n    return;"
        )
        manager = attach.index(
            "m_war3GpuSkinManager = std::make_unique<War3GpuSkinManager>"
        )
        self.assertLess(disabled, manager)

    def test_resources_remain_manager_owned_and_lazily_created(self) -> None:
        ensure_epoch = self.manager_cpp.split("bool ensureEpoch(", 1)[1].split(
            "bool canRetireCurrentEpoch(", 1
        )[0]
        self.assertIn("if (m_resources == nullptr)", ensure_epoch)
        self.assertIn(
            "m_resources = std::make_shared<War3GpuSkinResources>",
            ensure_epoch,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
