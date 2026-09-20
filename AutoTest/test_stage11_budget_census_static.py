"""Production collector -> real JSON emitter -> strict reader, plus wiring guards."""
import copy
import json
from pathlib import Path
import subprocess
import unittest

from analyze_stage11_budget_census import REASONS, TAG_DIMENSIONS, unique_pairs, validate

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build32/src/d3d9/war3_stage11_budget_census_test.exe"


def _v2_sample():
    return {
        "deviceIdentity": 1,
        "mapEpoch": 2,
        "deviceEpoch": 3,
        "frame": 42,
        "stage": 0,
        "errors": 0,
        "cap": 4096,
        "activeResident": 4096,
        "capacityRejects": 0,
        "uploadRangeHits": 0,
        "sampleCpuUs": 0,
        "entryCount": 1,
        "sliceCount": 2,
        "complete": True,
        "unknownCounts": [0] * len(REASONS),
        "unknownPositionBytes": [0] * len(REASONS),
        "pages": [{
            "id": 1,
            "active": True,
            "capacity": 4096,
            "used": 2048,
            "notReferencedByCacheBytes": 512,
            "tail": 2048,
            "references": 2,
            "staticReferences": 1,
            "tagUnknownReferences": 0,
            "staticTaggedUnionBytes": 1024,
            "notStaticTaggedUnionBytes": 1024,
            "staticTagOnlyBytes": 512,
            "notStaticTagOnlyBytes": 512,
            "mixedTagOverlapBytes": 512,
            "failedRetainedUnionBytes": 1024,
            "touchedUnionBytes": 1024,
            "touchedAndFailedUnionBytes": 512,
            "touchedOrFailedUnionBytes": 1536,
            "tagUnknownUnionBytes": 0,
            "pageReferences": 0,
            "knownAgeReferences": 0,
            "minAccessAge": 0,
            "maxAccessAge": 0,
            "owned": [1024, 0, 0, 512, 0],
        }],
    }


def _v2_data():
    return {
        "schema": 2,
        "enabled": True,
        "scope": "retained-page-and-cache-slices",
        "physicalBackingComplete": False,
        "gpuCompletionKnown": False,
        "ownerCategories": ["touched", "failed", "recentCache", "coldCache", "retired"],
        "unknownReasons": REASONS,
        "tagDimensions": TAG_DIMENSIONS,
        "samples": [_v2_sample()],
    }


def _v2_two_page_data():
    data = _v2_data()
    sample = data["samples"][0]
    sample["sliceCount"] = 3
    sample["entryCount"] = 2
    sample["pages"].append({
        "id": 2,
        "active": False,
        "capacity": 4096,
        "used": 256,
        "notReferencedByCacheBytes": 0,
        "tail": 3840,
        "references": 1,
        "staticReferences": 1,
        "tagUnknownReferences": 0,
        "staticTaggedUnionBytes": 256,
        "notStaticTaggedUnionBytes": 0,
        "staticTagOnlyBytes": 256,
        "notStaticTagOnlyBytes": 0,
        "mixedTagOverlapBytes": 0,
        "failedRetainedUnionBytes": 0,
        "touchedUnionBytes": 0,
        "touchedAndFailedUnionBytes": 0,
        "touchedOrFailedUnionBytes": 0,
        "tagUnknownUnionBytes": 0,
        "pageReferences": 0,
        "knownAgeReferences": 0,
        "minAccessAge": 0,
        "maxAccessAge": 0,
        "owned": [0, 0, 0, 256, 0],
    })
    return data


def _v1_data():
    sample = _v2_sample()
    page = sample["pages"][0]
    for key in list(page):
        if key not in {
            "id", "active", "capacity", "used", "notReferencedByCacheBytes",
            "tail", "references", "staticReferences", "pageReferences",
            "knownAgeReferences", "minAccessAge", "maxAccessAge", "owned",
        }:
            del page[key]
    return {
        "schema": 1,
        "enabled": True,
        "scope": "retained-page-and-cache-slices",
        "physicalBackingComplete": False,
        "gpuCompletionKnown": False,
        "ownerCategories": ["touched", "failed", "recentCache", "coldCache", "retired"],
        "unknownReasons": REASONS,
        "samples": [sample],
    }


class CensusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Missing executable is a failed gate, never a synthetic substitute/skip.
        cls.value = json.loads(
            subprocess.check_output([str(EXE), "--json"], text=True),
            object_pairs_hook=unique_pairs,
        )

    def test_production_roundtrip(self):
        result = validate(self.value)
        self.assertTrue(result["covered"])
        self.assertTrue(result["tagDimensionsAvailable"])
        s = result["samples"][0]
        self.assertEqual(
            (s["activeResident"], s["cacheReferencedCapacity"],
             s["cacheUnattributedUsed"], s["tail"]),
            (4096, 1536, 512, 2048),
        )
        self.assertEqual(s["cacheReferencedCapacityActive"], 1536)
        self.assertEqual(s["cacheReferencedCapacityRetired"], 0)
        self.assertEqual(s["cacheReferencedCapacityAll"], 1536)
        self.assertEqual(s["tagBytesAll"]["staticTagOnly"], 512)
        self.assertEqual(s["tagBytesAll"]["notStaticTagOnly"], 512)
        self.assertEqual(s["tagBytesAll"]["mixedTagOverlap"], 512)
        self.assertEqual(s["tagBytesAll"]["failedRetainedUnion"], 1024)
        self.assertEqual(s["tagBytesAll"]["touchedUnion"], 1024)
        self.assertEqual(s["tagBytesAll"]["touchedAndFailedUnion"], 512)
        self.assertEqual(s["tagBytesAll"]["touchedOrFailedUnion"], 1536)
        self.assertEqual(s["tagBytesAll"]["tagUnknownUnion"], 0)
        self.assertEqual(s["tagUnknownRatioActive"], 0.0)
        self.assertEqual(s["tagUnknownRatioRetired"], None)
        self.assertEqual(s["tagUnknownRatioAll"], 0.0)
        self.assertIsNone(s["gpuSafeReclaimableBytes"])
        self.assertFalse(result["shadowRecoveryProven"])

    def test_bad_cases(self):
        changes = [
            lambda d: d.update(enabled=False),
            lambda d: d.update(physicalBackingComplete=True),
            lambda d: d.update(tagDimensions=[]),
            lambda d: d.update(schema=True),
            lambda d: d["samples"][0].update(complete=False),
            lambda d: d["samples"][0].update(activeResident=0),
            lambda d: d["samples"][0].update(unknownCounts=[]),
            lambda d: d["samples"][0].update(frame=True),
            lambda d: d["samples"][0].update(stage=4),
            lambda d: d["samples"][0].update(sliceCount=1),
            lambda d: d["samples"][0].update(deviceIdentity=0),
            lambda d: d["samples"][0]["pages"][0].update(used=4096),
            lambda d: d["samples"][0]["pages"][0].update(capacity=0),
            lambda d: d["samples"][0]["pages"][0].update(staticTagOnlyBytes=0),
            lambda d: d["samples"][0]["pages"][0].update(mixedTagOverlapBytes=1024),
            lambda d: d["samples"][0]["pages"][0].update(failedRetainedUnionBytes=2048),
            lambda d: d["samples"][0]["pages"][0].update(touchedAndFailedUnionBytes=1024),
            lambda d: d["samples"][0]["pages"][0].update(touchedOrFailedUnionBytes=0),
            lambda d: d["samples"][0]["pages"][0].update(tagUnknownUnionBytes=1),
            lambda d: d["samples"][0]["pages"].append(copy.deepcopy(d["samples"][0]["pages"][0])),
            lambda d: d["samples"].append(copy.deepcopy(d["samples"][0])),
        ]
        for mutation in changes:
            with self.subTest(mutation=mutation):
                data = copy.deepcopy(self.value)
                mutation(data)
                with self.assertRaises(ValueError):
                    validate(data)

    def test_empty_and_incomplete(self):
        data = copy.deepcopy(self.value)
        data["samples"] = []
        self.assertFalse(validate(data)["covered"])
        data = copy.deepcopy(self.value)
        data["samples"][0].update(errors=4, complete=False)
        self.assertFalse(validate(data)["covered"])

    def test_duplicates(self):
        with self.assertRaises(ValueError):
            json.loads('{"x":{"id":1,"id":2}}', object_pairs_hook=unique_pairs)

    def test_owner_and_export_boundary(self):
        device = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
        method = device.split(
            "void D3D9DeviceEx::War3SampleStage11BudgetAtPresent() {", 1
        )[1].split("void D3D9DeviceEx::War3ResetShadowSessionState", 1)[0]
        self.assertLess(method.index("if (!m_war3Stage11CensusEnabled)"), method.index("GetMainLoopThreadId"))
        self.assertLess(method.index("owner != GetCurrentThreadId()"), method.index("schedule.take("))
        self.assertLess(method.index("owner != GetCurrentThreadId()"), method.index("censusLock = LockDevice()"))
        self.assertLess(method.index("censusLock = LockDevice()"), method.index("auto& schedule"))
        failure = method.split("if (!collector) {", 1)[1].split("return;", 1)[0]
        self.assertLess(failure.index("censusLock = D3D9DeviceLock()"), failure.index("noteStage11BudgetSample"))
        finish = method.split("const uint64_t expectedResident", 1)[1]
        self.assertLess(finish.index("censusLock = D3D9DeviceLock()"), finish.index("collector->finish("))
        self.assertLess(finish.index("collector->finish("), finish.index("noteStage11BudgetSample"))
        self.assertLess(method.index("EntryLimit"), method.index("for (const auto& pair : m_war3DrawTimeVBCache)"))
        for forbidden in ("createBuffer(", "EmitCs(", ".erase(", "mapPtr(", "WriteFile("):
            self.assertNotIn(forbidden, method)
        self.assertEqual(device.count("War3SampleStage11BudgetAtPresent();"), 1)
        monitor = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
        self.assertIn("snapshot.stage11BudgetCensus = m_stage11BudgetCensus;", monitor)
        self.assertIn("stage11_census::WriteJson(json, snapshot.stage11BudgetCensus);", monitor)


class UvCensusWiringStaticTests(unittest.TestCase):
    """Source-only checks for the b06 exact shared-UV census resolver."""

    def test_device_calls_resolver_and_fails_invalid_visible(self):
        device = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
        method = device.split(
            "void D3D9DeviceEx::War3SampleStage11BudgetAtPresent() {", 1
        )[1].split("void D3D9DeviceEx::War3ResetShadowSessionState", 1)[0]
        self.assertIn("War3DrawTimeUvCensusSpan uvSpan{};", method)
        self.assertIn("War3ResolveDrawTimeUvCensusSpan(entry, uvSpan)", method)
        self.assertIn(
            "case war3::render::War3DrawTimeUvCensusSpanStatus::PositionAlias:",
            method,
        )
        self.assertIn(
            "case war3::render::War3DrawTimeUvCensusSpanStatus::Independent:",
            method,
        )
        invalid = method.split(
            "case war3::render::War3DrawTimeUvCensusSpanStatus::Invalid:", 1
        )[1].split("break;", 1)[0]
        self.assertIn("collector->result.errors |= InvalidRange;", invalid)

    def test_resolver_uses_page_offset_proof_and_position_capacity(self):
        header = (
            ROOT / "src/d3d9/war3/render/war3_draw_time_snapshot_lifetime.h"
        ).read_text(encoding="utf-8")
        self.assertIn("enum class War3DrawTimeUvCensusSpanStatus : uint8_t", header)
        self.assertIn("struct War3DrawTimeUvCensusSpan", header)
        self.assertIn("War3ResolveDrawTimeUvCensusSpan(", header)
        self.assertIn("entry.uvSnapshotPage != entry.positionSnapshotPage", header)
        self.assertIn("entry.uvSnapshotOffset != entry.positionSnapshotOffset", header)
        self.assertIn("out.capacity = entry.positionCapacity;", header)


class SchemaReaderNegativeTests(unittest.TestCase):
    """Offline negatives for the strict reader; the real C++ writer is in CensusTests."""

    def test_v2_valid_positive(self):
        result = validate(_v2_data())
        self.assertTrue(result["tagDimensionsAvailable"])
        s = result["samples"][0]
        self.assertEqual(s["tagBytesAll"]["staticTaggedUnion"], 1024)
        self.assertEqual(s["tagBytesAll"]["notStaticTaggedUnion"], 1024)
        self.assertEqual(s["tagUnknownRatioActive"], 0.0)
        self.assertEqual(s["tagUnknownRatioAll"], 0.0)

    def test_scope_active_retired_all_are_separate(self):
        result = validate(_v2_two_page_data())
        s = result["samples"][0]
        self.assertEqual(s["cacheReferencedCapacity"], 1536)
        self.assertEqual(s["cacheReferencedCapacityActive"], 1536)
        self.assertEqual(s["cacheReferencedCapacityRetired"], 256)
        self.assertEqual(s["cacheReferencedCapacityAll"], 1792)
        self.assertEqual(s["tagBytesActive"]["staticTagOnly"], 512)
        self.assertEqual(s["tagBytesRetired"]["staticTagOnly"], 256)
        self.assertEqual(s["tagBytesAll"]["staticTagOnly"], 768)
        self.assertEqual(
            s["tagBytesAll"]["staticTaggedUnion"], 1280)
        self.assertIsNone(s["gpuSafeReclaimableBytes"])

    def test_v1_is_read_but_dimensions_unavailable(self):
        result = validate(_v1_data())
        self.assertFalse(result["tagDimensionsAvailable"])
        self.assertIsNone(result["samples"][0]["tagBytesAll"])
        self.assertIsNone(result["samples"][0]["tagUnknownRatioAll"])

    def test_v2_negative_closure_and_schema(self):
        mutations = [
            lambda d: d.update(schema=3),
            lambda d: d.update(schema=True),
            lambda d: d.update(tagDimensions=TAG_DIMENSIONS[:-1]),
            lambda d: d["samples"][0].update(stage=4),
            lambda d: d["samples"][0].update(deviceIdentity=0),
            lambda d: d["samples"][0].update(sliceCount=1),
            lambda d: d["samples"][0]["pages"][0].update(capacity=0),
            lambda d: d["samples"][0]["pages"][0].pop("staticTagOnlyBytes"),
            lambda d: d["samples"][0]["pages"][0].update(staticTagOnlyBytes=1),
            lambda d: d["samples"][0]["pages"][0].update(staticTaggedUnionBytes=1),
            lambda d: d["samples"][0]["pages"][0].update(failedRetainedUnionBytes=2048),
            lambda d: d["samples"][0]["pages"][0].update(touchedAndFailedUnionBytes=1024),
            lambda d: d["samples"][0]["pages"][0].update(touchedOrFailedUnionBytes=0),
            lambda d: d["samples"][0]["pages"][0].update(tagUnknownUnionBytes=1),
            lambda d: d["samples"][0]["pages"][0].update(tagUnknownReferences=3),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                data = copy.deepcopy(_v2_data())
                mutation(data)
                with self.assertRaises(ValueError):
                    validate(data)

    def test_nan_values_are_not_unsigned_ints(self):
        data = copy.deepcopy(_v2_data())
        data["samples"][0]["cap"] = float("nan")
        with self.assertRaises(ValueError):
            validate(data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
