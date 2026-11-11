"""Reader contract negatives plus optional ACTUAL production C++ fixture.

Set WARVK_STARTUP_WITNESS_FIXTURE to test_startup_witness_core --emit output.
Without that path the production roundtrip is explicitly skipped, not passed.
No process, game, desktop, DBWIN listener or compilation is started here.
"""
from __future__ import annotations

from copy import deepcopy
import os
from pathlib import Path
import re
import struct
import unittest
from unittest.mock import patch

import analyze_startup_witness as reader

PID = 1234
CREATION = 0x1122334455667788


def event(stage=1, phase=3, tid=7, token=0, ticks=10, detail=0, extra=0):
    return [0, stage, phase, tid, token, ticks & 0xFFFFFFFF, ticks >> 32,
            detail, extra, 0, 0, 0, 0, 0, 0, 0]


def binary(events=(), mutate_header=None, mutate_words=None):
    h = [0] * 64
    h[:10] = [reader.MAGIC, 1, 64, 16, 256, len(events), PID,
              CREATION & 0xFFFFFFFF, CREATION >> 32, 1]
    h[16] = 10000000
    rows = [list(row) for row in events]
    for i, row in enumerate(rows):
        row[0] = i + 1
    rows += [[0] * 16 for _ in range(256 - len(rows))]
    if mutate_header:
        mutate_header(h)
    if mutate_words:
        mutate_words(rows)
    return struct.pack("<64I", *h) + b"".join(struct.pack("<16I", *r) for r in rows)


def analyze(blob, **kwargs):
    args = dict(process_exited=True, expected_pid=PID, expected_creation_time=CREATION)
    args.update(kwargs)
    return reader.analyze_bytes(blob, **args)


class StartupReader(unittest.TestCase):
    def test_empty_is_uncovered(self):
        result = analyze(binary())
        self.assertFalse(result["recordingComplete"])
        self.assertFalse(result["stageObservationAvailable"])
        self.assertFalse(result["runtimeReadyObserved"])
        self.assertIsNone(result["latestCommittedEvent"])

    def test_one_note(self):
        result = analyze(binary([event()]))
        self.assertTrue(result["recordingComplete"])
        self.assertEqual(result["eventCount"], 1)
        self.assertFalse(result["causeCertified"])
        self.assertFalse(result["deadlockCertified"])
        self.assertFalse(result["normalExitProven"])
        self.assertFalse(result["gameplayAccepted"])

    def test_ready_is_only_note(self):
        self.assertFalse(analyze(binary([event(34, 1, token=1)]))["runtimeReadyObserved"])
        self.assertTrue(analyze(binary([event(34)]))["runtimeReadyObserved"])

    def test_exact_owner(self):
        for changes in ({"expected_pid": PID + 1}, {"expected_creation_time": CREATION + 1},
                        {"expected_pid": True}, {"expected_creation_time": True},
                        {"expected_pid": 0}, {"expected_creation_time": 0},
                        {"expected_pid": 1 << 32}, {"expected_creation_time": 1 << 64}):
            with self.subTest(changes=changes), self.assertRaises(reader.WitnessFormatError):
                analyze(binary(), **changes)

    def test_exit_is_mandatory(self):
        for proof in (False, 1, None, "true"):
            with self.subTest(proof=proof), self.assertRaises(reader.WitnessFormatError):
                analyze(binary(), process_exited=proof)
        with patch.object(Path, "open", side_effect=AssertionError("file opened")):
            with self.assertRaises(reader.WitnessFormatError):
                reader.analyze_file("nonexistent", process_exited=False,
                                    expected_pid=PID, expected_creation_time=CREATION)

    def test_size(self):
        for blob in (b"", binary()[:-4], binary() + b"\0"):
            with self.subTest(length=len(blob)), self.assertRaises(reader.WitnessFormatError):
                analyze(blob)

    def test_header_shape(self):
        for slot, value in ((0, 0), (1, 2), (2, 63), (3, 15), (4, 255), (5, 257),
                            (9, 0), (11, 2), (14, 2), (18, 1), (63, 1)):
            with self.subTest(slot=slot), self.assertRaises(reader.WitnessFormatError):
                analyze(binary(mutate_header=lambda h: h.__setitem__(slot, value)))

    def test_clock_unknown_accounted(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary(mutate_header=lambda h: h.__setitem__(16, 0)))
        result = analyze(binary([event()], mutate_header=lambda h:
                         (h.__setitem__(16, 0), h.__setitem__(15, 1))))
        self.assertFalse(result["recordingComplete"])
        self.assertEqual(result["qpcFrequency"], 0)

    def test_every_counter_is_loss(self):
        for slot in reader.COUNTERS:
            result = analyze(binary([event()], mutate_header=lambda h: h.__setitem__(slot, 1)))
            with self.subTest(slot=slot):
                self.assertFalse(result["recordingComplete"])
                self.assertEqual(result["counters"][reader.COUNTERS[slot]], 1)

    def test_invalid_event(self):
        for slot, value in ((1, 0), (1, 37), (2, 0), (2, 4), (3, 0), (4, 1),
                            (9, 1), (15, 1)):
            with self.subTest(slot=slot, value=value), self.assertRaises(reader.WitnessFormatError):
                analyze(binary([event()], mutate_words=lambda rows: rows[0].__setitem__(slot, value)))

    def test_all_stages_have_known_names(self):
        result = analyze(binary([event(stage=s) for s in reader.STAGES]))
        self.assertEqual([x["stage"] for x in result["events"]], list(reader.STAGES.values()))

    def test_stage_ids_match_cpp(self):
        header = Path(__file__).resolve().parents[1] / "src/d3d9/war3/diagnostics/war3_startup_witness.h"
        text = header.read_text(encoding="utf-8")
        body = text.split("enum class StartupStage : uint32_t {", 1)[1].split("};", 1)[0]
        names = re.findall(r"^\s*(\w+)(?:\s*=\s*1)?,\s*$", body, re.MULTILINE)
        self.assertEqual(dict(enumerate(names, 1)), reader.STAGES)

    def test_begin_token_is_own_sequence(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(1, 1, token=2)]))

    def test_mainrunner_early_is_not_original(self):
        result = analyze(binary([event(11, 1, token=1)]))
        self.assertEqual(result["mainRunnerOriginalCalls"], [])
        self.assertEqual(result["openSpans"][0]["stage"], "MainRunnerEarlyHooks")

    def test_open_original_is_not_deadlock(self):
        result = analyze(binary([event(12, 1, token=1)]))
        self.assertFalse(result["mainRunnerOriginalCalls"][0]["returnObserved"])
        self.assertFalse(result["deadlockCertified"])
        self.assertFalse(result["causeCertified"])

    def test_original_return(self):
        rows = [event(12, 1, token=1), event(12, 2, token=1, ticks=20, detail=123)]
        result = analyze(binary(rows))
        self.assertTrue(result["mainRunnerOriginalCalls"][0]["returnObserved"])
        self.assertEqual(result["completedSpans"][0]["end"]["detail"], 123)
        self.assertEqual(result["openSpans"], [])

    def test_nested_and_alt_are_distinct(self):
        rows = [event(12, 1, token=1), event(15, 1, token=2),
                event(15, 2, token=2), event(12, 2, token=1)]
        result = analyze(binary(rows))
        self.assertEqual([x["stage"] for x in result["mainRunnerOriginalCalls"]],
                         ["MainRunnerOriginal", "MainRunnerAltOriginal"])
        self.assertTrue(all(x["returnObserved"] for x in result["mainRunnerOriginalCalls"]))

    def test_end_wrong_thread(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(12, 1, token=1), event(12, 2, tid=8, token=1)]))

    def test_end_wrong_stage(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(12, 1, token=1), event(15, 2, token=1)]))

    def test_end_without_begin(self):
        for rows in ([event(12, 2, token=1)],
                     [event(12), event(12, 2, token=1)],
                     [event(12, 1, token=1), event(12, 2, token=0)]):
            with self.subTest(rows=rows), self.assertRaises(reader.WitnessFormatError):
                analyze(binary(rows))

    def test_duplicate_end(self):
        rows = [event(12, 1, token=1), event(12, 2, token=1), event(12, 2, token=1)]
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary(rows))

    def test_same_span_clock_backwards(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(1, 1, token=1, ticks=100), event(1, 2, token=1, ticks=99)]))

    def test_cross_thread_clock_order_not_commit_order(self):
        result = analyze(binary([event(tid=1, ticks=100), event(tid=2, ticks=99)]))
        self.assertTrue(result["recordingComplete"])

    def test_control_thread_not_pipe(self):
        result = analyze(binary([event(24, 1, token=1)]))
        self.assertTrue(result["controlThreadEntryObserved"])
        self.assertFalse(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [])

    def test_pipe_success_only_completed_create(self):
        result = analyze(binary([event(25, 1, token=1), event(25, 2, token=1, detail=1)]))
        self.assertTrue(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [])
        self.assertFalse(analyze(binary([event(25, 1, token=1, detail=1)]))["pipeCreateSuccessObserved"])
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(25, detail=1)]))

    def test_pipe_failure_retains_error(self):
        result = analyze(binary([event(25, 1, token=1), event(25, 2, token=1, extra=5)]))
        self.assertFalse(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [{"sequence": 2, "win32Error": 5}])

    def test_later_pipe_failure_note(self):
        rows = [event(25, 1, token=1), event(25, 2, token=1, detail=1),
                event(25, detail=0, extra=231)]
        result = analyze(binary(rows))
        self.assertTrue(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [{"sequence": 3, "win32Error": 231}])

    def test_pipe_failure_note_does_not_require_previous_success(self):
        result = analyze(binary([event(25, extra=5)]))
        self.assertFalse(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [{"sequence": 1, "win32Error": 5}])

    def test_pipe_success_note_is_forbidden(self):
        for stage in (25, 26):
            for detail in (1, 2):
                with self.subTest(stage=stage, detail=detail), self.assertRaises(reader.WitnessFormatError):
                    analyze(binary([event(stage, detail=detail)]))

    def test_pipe_bad_result(self):
        for stage in (25, 26):
            for detail, extra in ((2, 0), (1, 5)):
                with self.subTest(stage=stage, detail=detail, extra=extra), \
                     self.assertRaises(reader.WitnessFormatError):
                    analyze(binary([event(stage, 1, token=1),
                                    event(stage, 2, token=1, detail=detail, extra=extra)]))

    def test_duplicate_sequence(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event(), event()], mutate_words=lambda rows: rows[1].__setitem__(0, 1)))

    def test_future_sequence(self):
        with self.assertRaises(reader.WitnessFormatError):
            analyze(binary([event()], mutate_words=lambda rows: rows[0].__setitem__(0, 2)))

    def test_partial_publication_is_not_negative_evidence(self):
        result = analyze(binary([event(), event(), event()],
                         mutate_words=lambda rows: rows[1].__setitem__(0, 0)))
        self.assertEqual(result["declaredCount"], 3)
        self.assertEqual(result["eventCount"], 1)
        self.assertFalse(result["recordingComplete"])
        self.assertEqual([x["reason"] for x in result["gaps"]],
                         ["partialPublication", "afterBrokenPrefix"])

    def test_count_lag_and_uncommitted_tail(self):
        for commit in (0, 2):
            result = analyze(binary([event()], mutate_words=lambda rows:
                                    (rows[1].__setitem__(0, commit), rows[1].__setitem__(1, 12))))
            with self.subTest(commit=commit):
                self.assertFalse(result["recordingComplete"])
                self.assertEqual(result["eventCount"], 1)
                self.assertEqual(result["gaps"][0]["reason"], "undeclaredEventData")

    def test_input_preserved(self):
        blob = binary([event()])
        copy = deepcopy(blob)
        analyze(blob)
        self.assertEqual(blob, copy)

    def test_real_production_core_fixture(self):
        path = os.environ.get("WARVK_STARTUP_WITNESS_FIXTURE", "")
        if not path:
            self.skipTest("actual C++ --emit fixture not supplied; roundtrip NOT run")
        result = reader.analyze_file(path, process_exited=True,
                                     expected_pid=PID, expected_creation_time=CREATION)
        self.assertTrue(result["recordingComplete"])
        self.assertEqual(result["eventCount"], 12)
        self.assertTrue(result["mainRunnerOriginalCalls"][0]["returnObserved"])
        self.assertTrue(result["controlThreadEntryObserved"])
        self.assertFalse(result["pipeCreateSuccessObserved"])
        self.assertEqual(result["pipeCreateFailures"], [{"sequence": 9, "win32Error": 5}])
        self.assertEqual(result["openSpans"], [])
        self.assertFalse(result["runtimeReadyObserved"])


if __name__ == "__main__":
    unittest.main()
