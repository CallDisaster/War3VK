#!/usr/bin/env python3
"""Static contracts for immutable CurrentDraw index-slice hash reuse."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


class CurrentDrawIndexSliceHashCacheContracts(unittest.TestCase):
    def test_cache_owns_and_returns_the_immutable_content_hash(self) -> None:
        start = DEVICE.index("class War3CurrentDrawVisibleIndexSliceCache final")
        end = DEVICE.index("class War3CurrentDrawGeosetSnapshotCache final", start)
        cache = DEVICE[start:end]
        self.assertIn("uint64_t& outContentHash", cache)
        self.assertGreaterEqual(cache.count("outContentHash ="), 2)
        self.assertIn("uint64_t contentHash = 0u", cache)
        self.assertIn("contentHash};", cache)

    def test_index_bytes_are_hashed_only_when_the_slice_misses(self) -> None:
        start = DEVICE.index("bool War3TryAttachCurrentDrawVisibleIndexSlice(")
        end = DEVICE.index(
            "inline bool War3CasterIsAnonymousSmallPathBlockerMarker", start
        )
        attach = DEVICE[start:end]
        miss = attach.index("if (!cacheHit)")
        immutable_hash = attach.index(
            "War3ComputeCurrentDrawVisibleIndexSliceContentHash", miss
        )
        store = attach.index("sliceCache->store", immutable_hash)
        self.assertLess(miss, immutable_hash)
        self.assertLess(immutable_hash, store)
        self.assertEqual(
            attach.count("War3ComputeCurrentDrawVisibleIndexSliceContentHash"), 1
        )

    def test_draw_specific_identity_is_still_mixed_each_time(self) -> None:
        start = DEVICE.index("uint64_t War3ComputeCurrentDrawVisibleIndexSliceHash(")
        end = DEVICE.index("class War3CurrentDrawVisibleIndexSliceCache final", start)
        helper = DEVICE[start:end]
        for field in (
            "renderable.layerIndex",
            "renderable.subIndex",
            "record.layerIndex",
            "record.payloadWord108",
            "record.payloadWord11C",
            "immutableSliceContentHash",
        ):
            self.assertIn(field, helper)
        self.assertNotIn("indices[i]", helper)

    def test_cache_authority_remains_generation_backed(self) -> None:
        start = DEVICE.index("class War3CurrentDrawVisibleIndexSliceCache final")
        end = DEVICE.index("class War3CurrentDrawGeosetSnapshotCache final", start)
        cache = DEVICE[start:end]
        self.assertIn("entry.mapEpoch == geoset->mapEpoch", cache)
        self.assertIn(
            "entry.immutableModelGeneration == geoset->immutableModelGeneration",
            cache,
        )
        self.assertIn("geoset->readyForShadowConsumer()", cache)


if __name__ == "__main__":
    unittest.main()
