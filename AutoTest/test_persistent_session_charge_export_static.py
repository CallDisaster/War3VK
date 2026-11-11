"""Source-only gates for same-Present live/retired logical charge exports.

These fields are logical admission charges, not physical VRAM measurements.
This checks production wiring and source emitter keys, not real JSON execution,
GPU retirement completion, or a successful map-switch runtime recovery.
"""

from collections import Counter
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(encoding="utf-8")
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def code_only(source):
    pattern = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda match: re.sub(r"[^\n]", " ", match.group()),
                  source, flags=re.DOTALL)


def body(source, marker):
    match = re.search(marker, source)
    if match is None:
        raise AssertionError(f"Missing production boundary: {marker}")
    start = source.index("{", match.end())
    depth = 0
    for end in range(start, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:end]
    raise AssertionError(f"Unterminated production boundary: {marker}")


HEADER_CODE = code_only(HEADER)
PERF_CODE = code_only(PERF)
DEVICE_CODE = code_only(DEVICE)
NOTE = body(PERF_CODE, r"\bWar3PerfMonitor::notePersistentGeometryFrame\s*\(")
FIELDS = {"Used": "bytesUsed", "Live": "bytesLive", "Retired": "bytesRetired"}


class PersistentSessionChargeExportStaticTests(unittest.TestCase):
    def test_new_frame_and_last_gauges_default_to_zero(self):
        for struct_name in ("PersistentGeometryFrameStats", "FrameWorkloadSnapshot"):
            fields = body(HEADER_CODE, rf"\bstruct\s+{struct_name}\b")
            for field in FIELDS.values():
                self.assertEqual(len(re.findall(rf"\buint64_t\s+{field}\s*=\s*0\s*;", fields)), 1)
        aggregate = body(HEADER_CODE, r"\bstruct\s+ShadowBudgetAggregate\b")
        for suffix in FIELDS:
            self.assertEqual(len(re.findall(
                rf"\buint64_t\s+persistentPoolBytes{suffix}Last\s*=\s*0\s*;", aggregate)), 1)

    def test_device_publishes_one_present_snapshot_from_shared_charges(self):
        present = body(DEVICE_CODE, r"\bD3D9DeviceEx::PresentEx\s*\(")
        start = re.search(r"PersistentGeometryFrameStats\s+stats\s*=\s*\{\s*\}\s*;", present)
        self.assertIsNotNone(start)
        publish = re.search(r"\bnotePersistentGeometryFrame\s*\(\s*stats\s*\)\s*;", present)
        self.assertIsNotNone(publish)
        self.assertEqual(len(re.findall(r"\bnotePersistentGeometryFrame\s*\(", DEVICE_CODE)), 1)
        self.assertLess(start.start(), publish.start())
        handoff = present[start.end():publish.start()]
        for field, source in (("bytesUsed", r"total\s*\(\s*\)"),
                              ("bytesLive", "live"), ("bytesRetired", "retired")):
            self.assertEqual(len(re.findall(
                rf"\bstats\.{field}\s*=\s*m_war3ShadowPersistentCharges\.{source}\s*;", handoff)), 1)
            self.assertEqual(len(re.findall(rf"\bstats\.{field}\s*=", DEVICE_CODE)), 1)

    def test_one_locked_note_copies_frame_and_last_values_without_recomputing_total(self):
        lock = re.search(r"std::lock_guard\s+lock\s*\(\s*m_mutex\s*\)\s*;", NOTE)
        self.assertIsNotNone(lock)
        self.assertEqual(len(re.findall(r"std::lock_guard", NOTE)), 1)
        for suffix, field in FIELDS.items():
            for target in (f"m_currentFrameWorkload.{field}", f"agg.persistentPoolBytes{suffix}Last"):
                matches = list(re.finditer(rf"\b{re.escape(target)}\s*=\s*stats\.{field}\s*;", NOTE))
                self.assertEqual(len(matches), 1, target)
                self.assertGreater(matches[0].start(), lock.start())
        export = body(PERF_CODE, r"\bWar3PerfMonitor::captureExportSnapshotLocked\s*\(")
        self.assertRegex(export, r"snapshot\.shadowBudgetAggregate\s*=\s*m_shadowBudgetAggregate\s*;")
        self.assertNotRegex(NOTE, r"stats\.bytes(?:Used|Live|Retired)\s*=")

    def test_each_summary_emits_unique_charge_keys_from_corresponding_last_values(self):
        for start_key, end_key in (("shadowBudgetSummary", "topShadowOffenders"),
                                   ("shadowRuntimeV2Summary", "publishVisibleSafeCopyVerifier")):
            start = PERF.index(r'\"' + start_key + r'\":')
            summary = PERF[start:PERF.index(r'\"' + end_key + r'\":', start)]
            pool_keys = re.findall(r'\\"(persistentPool\w+)\\"\s*:', summary)
            duplicates = {key: count for key, count in Counter(pool_keys).items() if count != 1}
            self.assertFalse(duplicates, (start_key, duplicates))
            for suffix in FIELDS:
                # Keep the existing total key unchanged; the two additions are
                # explicit Last gauges, with no new max/mean family.
                key = "persistentPoolBytesUsed" if suffix == "Used" else f"persistentPoolBytes{suffix}Last"
                self.assertEqual(pool_keys.count(key), 1, (start_key, key))
                expected = (re.escape(r'\"' + key + r'\": "') + r'\s*<<\s*shadowAgg\.'
                            + f"persistentPoolBytes{suffix}Last" + r'\s*<<\s*'
                            + re.escape(r'",\n";'))
                self.assertIsNotNone(re.search(expected, summary), (start_key, key))
        for suffix in ("Live", "Retired"):
            self.assertEqual(PERF.count(r'\"persistentPoolBytes' + suffix + r'Last\":'), 2)
            self.assertNotRegex(HEADER_CODE + PERF_CODE,
                                rf"\bpersistentPoolBytes{suffix}(?:Max|Mean|Average|GaugeSum)\b")

    def test_frame_columns_and_values_append_without_shifting_existing_columns(self):
        start = PERF.index(r'\"workloadSeriesColumns\":')
        column_block = PERF[start:PERF.index(r'\"workloadSeries\":', start)]
        columns = re.findall(r'\\"(\w+)\\"', column_block)[1:]
        self.assertEqual(len(columns), len(set(columns)))
        self.assertEqual(columns[-3:], ["s1EarlyReplayClosureMismatch", "poolBytesLive", "poolBytesRetired"])
        self.assertEqual(columns.count("poolBytesUsed"), 1)
        rows_start = PERF.index(r'\"workloadSeries\":', start)
        rows = PERF[rows_start:PERF.index("struct SectionAggregate", rows_start)]
        self.assertRegex(rows, r"\(w\.s1EarlyReplayClosureMismatch\s*\?\s*1\s*:\s*0\)"
                         r'\s*<<\s*", "\s*<<\s*w\.bytesLive\s*<<\s*", "'
                         r'\s*<<\s*w\.bytesRetired\s*<<\s*"\]"\s*;')
        for field in ("bytesUsed", "bytesLive", "bytesRetired"):
            self.assertEqual(len(re.findall(rf"\bw\.{field}\b", rows)), 1)

    def test_comments_distinguish_logical_admission_charge_from_physical_vram(self):
        self.assertIn("bytesUsed = bytesLive + bytesRetired", HEADER)
        self.assertIn("Same-Present logical admission charge, not physical VRAM", HEADER)
        self.assertIn("Last same-Present logical admission charges, not physical VRAM usage", HEADER)


if __name__ == "__main__":
    unittest.main()
