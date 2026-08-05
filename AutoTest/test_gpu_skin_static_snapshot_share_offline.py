#!/usr/bin/env python3
"""针对 GPU 蒙皮不可变 geoset 共享机制的离线契约检查。

本测试不模拟 Vulkan 执行。它用于闭合以下所有权规则：缓存、排队中的未命中项与静态资源
共同持有同一份不可变消费者载荷，同时观察时间戳仍可独立刷新。
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from pathlib import Path
import random
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODEL_H = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.h"
MODEL_CPP = ROOT / "src/d3d9/war3/model/war3_model_resource_cache.cpp"
RES_H = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h"
RES_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp"
MANAGER_CPP = ROOT / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.cpp"


@dataclass(frozen=True)
class Payload:
    geoset_data: int
    content_hash: int
    vertex_count: int
    vectors: tuple[int, ...]


@dataclass
class Observation:
    first_seen: int = 0
    last_seen: int = 0
    last_refresh: int = 0


class SnapshotModel:
    def __init__(self) -> None:
        self.snapshot: Payload | None = None
        self.observation = Observation()

    def publish(self, incoming: Payload, frame: int, refresh: bool = False) -> Payload:
        if self.snapshot != incoming:
            self.snapshot = replace(incoming)
        if self.observation.first_seen == 0:
            self.observation.first_seen = frame
        self.observation.last_seen = max(self.observation.last_seen, frame)
        if refresh:
            self.observation.last_refresh = max(
                self.observation.last_refresh, frame
            )
        return self.snapshot


class SnapshotOwnershipTests(unittest.TestCase):
    def test_metadata_refresh_reuses_payload_identity(self) -> None:
        model = SnapshotModel()
        payload = Payload(0x1234, 0x5678, 3, (1, 2, 3))
        first = model.publish(payload, 10)
        queued_miss = first
        static_resource = queued_miss
        second = model.publish(payload, 11, refresh=True)
        self.assertIs(first, second)
        self.assertIs(queued_miss, static_resource)
        self.assertEqual(model.observation, Observation(10, 11, 11))

    def test_content_change_is_copy_on_publish(self) -> None:
        model = SnapshotModel()
        old = model.publish(Payload(1, 7, 2, (4, 5)), 1)
        new = model.publish(Payload(1, 8, 2, (4, 6)), 2)
        self.assertIsNot(old, new)
        self.assertEqual(old.content_hash, 7)
        self.assertEqual(old.vectors, (4, 5))
        self.assertEqual(new.content_hash, 8)
        self.assertEqual(new.vectors, (4, 6))

    def test_deterministic_fuzz_identity_partition(self) -> None:
        rng = random.Random(0x57415233)
        model = SnapshotModel()
        current: Payload | None = None
        current_identity: Payload | None = None
        for frame in range(1, 20001):
            if current is None or rng.randrange(19) == 0:
                version = frame
                current = Payload(
                    0x2000,
                    version,
                    1 + version % 128,
                    tuple(rng.randrange(256) for _ in range(8)),
                )
                previous_identity = current_identity
                current_identity = model.publish(current, frame, True)
                if previous_identity is not None:
                    self.assertIsNot(previous_identity, current_identity)
            else:
                same = model.publish(current, frame, rng.randrange(3) == 0)
                self.assertIs(same, current_identity)
        self.assertEqual(model.observation.first_seen, 1)
        self.assertEqual(model.observation.last_seen, 20000)


class SourceContractTests(unittest.TestCase):
    def test_cache_uses_owning_const_snapshot(self) -> None:
        header = MODEL_H.read_text(encoding="utf-8")
        source = MODEL_CPP.read_text(encoding="utf-8")
        self.assertIn("std::shared_ptr<const ShadowGeosetResourceRecord>", header)
        self.assertIn(
            "std::unordered_map<void *, ShadowGeosetResourceSnapshot> m_byGeosetData",
            header,
        )
        self.assertIn("findGeosetSnapshotByData", header)
        self.assertIn("findGeosetSnapshotByData", source)
        self.assertIn("materializeGeosetDataRecordLocked", source)
        self.assertIn("HasSameGeosetConsumerContent(*current->second, merged)", source)
        self.assertIn("record.contentHash != 0 &&", source)
        self.assertIn(
            "noteGeosetDataMetadataLocked(it->second.geosetDataPtr, it->second);",
            source,
        )
        self.assertNotIn("findGeosetByPtrRef", header)
        self.assertNotIn("findGeosetByDataRef", header)

    def test_gpu_queue_and_resource_share_snapshot(self) -> None:
        header = RES_H.read_text(encoding="utf-8")
        source = RES_CPP.read_text(encoding="utf-8")
        manager = MANAGER_CPP.read_text(encoding="utf-8")
        self.assertGreaterEqual(
            header.count("model::ShadowGeosetResourceSnapshot record;"), 2
        )
        self.assertIn("findGeosetSnapshotByData", manager)
        self.assertIn("m_staticMisses.push_back({ key, std::move(record) })", source)
        self.assertIn("resource->record = miss.record", source)
        self.assertNotIn("model::ShadowGeosetResourceRecord record;", header)

    def test_census_counts_no_independent_host_copy(self) -> None:
        source = RES_CPP.read_text(encoding="utf-8")
        self.assertIn("census.hostBackingLogicalBytes = 0u", source)
        self.assertIn("census.hostMappedLogicalBytes = 0u", source)
        self.assertIn("census.duplicateHostBackingLogicalBytes = 0u", source)
        self.assertIn(
            "found->second->residencyCensus, 0u, 0u, 0u", source
        )
        self.assertNotIn("GeosetRecordCapacityBytes", source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
