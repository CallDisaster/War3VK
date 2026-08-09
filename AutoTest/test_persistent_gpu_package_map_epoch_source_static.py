#!/usr/bin/env python3
"""Static contracts for map-scoped immutable model source authority."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODEL_H = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
MODEL_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
ADAPTER_H = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.h"
ADAPTER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_persistent_gpu_package_stage11_observe_adapter.cpp"


class PersistentPackageMapEpochSourceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.model_h = MODEL_H.read_text(encoding="utf-8")
        cls.model_cpp = MODEL_CPP.read_text(encoding="utf-8")
        cls.device = DEVICE_CPP.read_text(encoding="utf-8")
        cls.adapter_h = ADAPTER_H.read_text(encoding="utf-8")
        cls.adapter_cpp = ADAPTER_CPP.read_text(encoding="utf-8")

    def test_map_reset_clears_every_address_alias(self) -> None:
        reset = self.model_cpp.split(
            "bool ShadowModelResourceCache::resetMapEpoch", 1
        )[1].split("void ShadowModelResourceCache::beginFrame", 1)[0]
        for table in (
            "m_byGeoset",
            "m_byGeosetData",
            "m_geosetDataObservation",
            "m_byModelResource",
            "m_byRuntimeModel",
            "m_runtimeOwnerByGeoset",
            "m_runtimeOwnerByGeosetData",
        ):
            self.assertIn(f"{table}.clear()", reset)
        self.assertIn("nextMapEpoch == 0u", reset)
        self.assertIn("m_mapEpoch.store(nextMapEpoch", reset)
        self.assertNotIn("m_immutableModelGenerations", reset)

    def test_published_snapshot_is_stamped_by_cache_owner(self) -> None:
        store = self.model_cpp.split(
            "ShadowModelResourceCache::storeGeosetRecord", 1
        )[1].split("void ShadowModelResourceCache::storeModelRecord", 1)[0]
        self.assertIn("ShadowGeosetResourceRecord record = incomingRecord", store)
        self.assertIn("record.mapEpoch = m_mapEpoch.load", store)
        self.assertIn("uint64_t mapEpoch = 0", self.model_h)

    def test_epoch_qualified_lookup_rechecks_under_lock(self) -> None:
        lookup = self.model_cpp.split(
            "findGeosetStampByDataForEpoch", 1
        )[1].split("hydrateGeosetByKnownPtrs", 1)[0]
        self.assertIn("std::shared_lock<std::shared_mutex> lock", lookup)
        self.assertIn("m_mapEpoch.load", lookup)
        self.assertIn("it->second->mapEpoch != expectedMapEpoch", lookup)
        self.assertIn("out.mapEpoch = it->second->mapEpoch", lookup)

    def test_device_owner_resets_cache_on_creation_and_map_exit(self) -> None:
        ctor = self.device.split("D3D9DeviceEx::D3D9DeviceEx(", 1)[1].split(
            "D3D9DeviceEx::~D3D9DeviceEx", 1
        )[0]
        reset = self.device.split(
            "void D3D9DeviceEx::War3ResetGpuSkinMapEpoch()", 1
        )[1].split("void D3D9DeviceEx::War3RequestShadowMapEpochReset", 1)[0]
        semantic_reset = self.device.split(
            "uint64_t D3D9DeviceEx::War3ResetCpuSemanticMapSession(", 1
        )[1].split("D3D9DeviceEx::D3D9DeviceEx(", 1)[0]
        self.assertIn("War3ResetCpuSemanticMapSession(", ctor)
        self.assertIn("War3ResetCpuSemanticMapSession(", reset)
        self.assertIn(
            "ShadowModelResourceCache::instance().resetMapEpoch",
            semantic_reset,
        )
        self.assertLess(
            semantic_reset.index("MintWar3ShadowMapEpoch()"),
            semantic_reset.index("resetMapEpoch"),
        )
        self.assertNotIn("MintWar3ShadowMapEpoch()", ctor)
        self.assertNotIn("MintWar3ShadowMapEpoch()", reset)
        self.assertIn("g_war3ShadowMapEpochIssuer", self.device)

    def test_stage11_requires_exact_map_epoch_but_consume_stays_closed(self) -> None:
        self.assertIn("sidecar.mapEpoch == witness.mapEpoch", self.adapter_cpp)
        self.assertIn("RecordedCurrentMapSource", self.adapter_h)
        self.assertIn("StaleMapEpochExplicitGeosetDataSidecar", self.adapter_h)
        self.assertIn("kProvesCurrentGameMemory = true", self.adapter_h)
        for hard_gate in (
            "kConsumeAdmissionGranted = false",
            "kBindsGpuResources = false",
            "kRecordsCommands = false",
            "kMutatesCanonicalDraw = false",
            "kPublishesPackage = false",
        ):
            self.assertIn(hard_gate, self.adapter_h)


if __name__ == "__main__":
    unittest.main(verbosity=2)
