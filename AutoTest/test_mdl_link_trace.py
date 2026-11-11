#!/usr/bin/env python3
"""Adversarial CPU-only wire-reader tests; synthetic fixtures are not game proof.

Set WARVK_MDL_TRACE_CPP_FIXTURE to the file produced by the actual C++ core's
``--emit`` mode to additionally test its production binary roundtrip. An absent
fixture is explicitly skipped, never counted as a production-roundtrip pass.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

import analyze_mdl_link_trace as reader


CREATION = 0x1122334455667788
LOW = 0x7F165820
HIGH = 0x80010090


def event(kind: int = 1, pointer: int = LOW, serial: int = 1,
          token: int = 0, **words: int) -> list[int]:
    value = [0] * reader.EVENT_WORDS
    value[1], value[2], value[3] = kind, 55, 0x128EDD
    value[4], value[6], value[7] = pointer, serial, token
    value[8], value[10] = 22404, 0xB7FF90
    if kind in (1, 6, 8):
        value[12] = pointer
    for key, item in words.items():
        value[int(key.removeprefix("w"))] = item
    return value


def fixture(events: list[list[int]] | None = None, *,
            header: dict[int, int] | None = None,
            preserve_sequence: bool = False) -> bytes:
    values = events if events is not None else [event()]
    result = bytearray(reader.FILE_BYTES)
    head = [0] * reader.HEADER_WORDS
    head[:7] = [reader.MAGIC, 1, 64, 32, reader.CAPACITY, len(values), 1234]
    head[7:13] = [CREATION & 0xFFFFFFFF, CREATION >> 32, 0x6F000000,
                 0x10000000, 2, reader.REQUIRED_HOOK_MASK]
    head[22:27] = [15, 0x56BD0E31, 0xD45000, 0x56BD0E34, 0x61000]
    head_overrides = header if header is not None else {}
    for index, value in head_overrides.items():
        head[index] = value
    struct.pack_into("<64I", result, 0, *head)
    for index, value in enumerate(values):
        value = value.copy()
        if not preserve_sequence:
            value[0] = index + 1
        struct.pack_into("<32I", result, 256 + index * 128, *value)
    return bytes(result)


def analyze(data: bytes | None = None, **kwargs):
    return reader.analyze_bytes(fixture() if data is None else data,
                                process_exited=True, **kwargs)


def reasons(report):
    return {gap["reason"] for gap in report["gaps"]}


def operation_sequence() -> list[list[int]]:
    return [
        event(),
        event(6),
        event(4, token=1),
        event(5, token=1, w5=HIGH, w12=HIGH, w13=16, w24=2),
        event(9, HIGH, 2, w13=1, w14=0x6F961788, w18=0x80015820),
        event(2, HIGH, 2, 2),
        event(3, HIGH, 2, 2, w12=1),
        event(1, HIGH, 3),
    ]


class WireTests(unittest.TestCase):
    def test_complete_bounded_observation_is_never_certified(self):
        report = analyze()
        self.assertTrue(report["observationComplete"])
        self.assertEqual(report["verdict"], "BOUNDED_OBSERVATION_ONLY")
        for field in ("certified", "causeCertified", "lifetimeCertified", "allocatorRouteCertified"):
            self.assertIs(report[field], False)
        self.assertEqual(report["certificationRefusals"], list(reader.COVERAGE_EXCLUSIONS))
        self.assertFalse(report["process"]["normalExitProven"])

    def test_fixed_dimensions_and_exact_size(self):
        self.assertEqual(reader.FILE_BYTES, 2097408)
        for data in (b"", fixture()[:-1], fixture() + b"\0"):
            with self.subTest(size=len(data)), self.assertRaises(reader.TraceFormatError):
                analyze(data)

    def test_schema_fields_reject_unknown(self):
        for index, value in ((0, 0), (1, 2), (2, 63), (3, 31), (4, 1024),
                             (5, reader.CAPACITY + 1), (11, 6), (12, 0x40)):
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture(header={index: value}))

    def test_reserved_header_all_words_rejected(self):
        for index in range(27, 64):
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture(header={index: 1}))

    def test_reserved_event_all_words_rejected(self):
        for index in range(25, 32):
            record = event()
            record[index] = 1
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([record]))

    def test_all_coverage_exclusions_mandatory(self):
        for flags in (0, 1, 3, 7, 14, 16, 31):
            with self.subTest(flags=flags), self.assertRaises(reader.TraceFormatError):
                analyze(fixture(header={22: flags}))

    def test_sticky_flags_are_booleans(self):
        for index in (14, 19):
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture(header={index: 2}))

    def test_unknown_kind_thread_or_fact_mask_rejected(self):
        for index, value in ((1, 0), (1, 11), (2, 0), (13, 32)):
            record = event()
            record[index] = value
            with self.subTest(index=index, value=value), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([record]))

    def test_nonzero_unknown_node_table_head_payload_rejected(self):
        for index in range(14, 25):
            record = event()
            record[index] = 123
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([record]))

    def test_known_payload_may_be_zero(self):
        report = analyze(fixture([event(w13=7)]))
        self.assertTrue(report["observationComplete"])

    def test_known_payload_not_discarded(self):
        record = event(9, w13=7, w14=100, w18=HIGH, w19=25,
                       w22=0x7890, w23=26, w24=27)
        report = analyze(fixture([event(), record]))
        self.assertTrue(report["observations"]["hasPreUnlinkHighBitGlobalBacklinkField"])

    def test_released_old_only_realloc_end(self):
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(w13=16)]))

    def test_return_mirrors_reject_mismatch(self):
        for kind in (1, 6, 8):
            with self.subTest(kind=kind), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([event(kind, w12=LOW + 4)]))
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(5, token=1, w5=HIGH, w12=LOW)]))

    def test_realloc_end_has_new_serial_not_old_head(self):
        events = operation_sequence()
        self.assertTrue(analyze(fixture(events))["observationComplete"])
        for index, value in ((22, 1), (23, 1), (13, 4)):
            changed = [record.copy() for record in events]
            changed[3][index] = value
            with self.subTest(index=index), self.assertRaises(reader.TraceFormatError):
                analyze(fixture(changed))

    def test_failed_returns_cannot_own_serial(self):
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(pointer=0, serial=1)]))
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(5, token=1, w24=2)]))

    def test_alloc_and_boundary_tokens_are_zero(self):
        for kind in (1, 6, 7, 8, 9, 10):
            record = event(kind, pointer=0 if kind == 7 else LOW,
                           serial=0 if kind == 7 else 1, token=1)
            with self.subTest(kind=kind), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([record]))

    def test_begin_end_tokens_cannot_be_zero(self):
        for kind in (2, 3, 4, 5):
            with self.subTest(kind=kind), self.assertRaises(reader.TraceFormatError):
                analyze(fixture([event(kind)]))

    def test_insert_begin_does_not_invent_new_node(self):
        report = analyze(fixture([event(7, pointer=0, serial=0)]))
        self.assertTrue(report["observationComplete"])
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(7)]))


class CoverageTests(unittest.TestCase):
    def test_explicit_post_exit_required(self):
        with self.assertRaises(TypeError):
            reader.analyze_bytes(fixture())
        for value in (False, 1, "true", None):
            with self.subTest(value=value), self.assertRaises(reader.TraceFormatError):
                reader.analyze_bytes(fixture(), process_exited=value)

    def test_process_identity_pair(self):
        report = analyze(expected_pid=1234, expected_creation_time=CREATION)
        self.assertTrue(report["process"]["expectedIdentityChecked"])
        self.assertFalse(analyze(expected_pid=1234)["process"]["expectedIdentityChecked"])
        for kwargs in ({"expected_pid": 1235}, {"expected_creation_time": CREATION + 1},
                       {"expected_pid": True}, {"expected_creation_time": False}):
            with self.subTest(kwargs=kwargs), self.assertRaises(reader.TraceFormatError):
                analyze(**kwargs)

    def test_missing_identity_is_incomplete(self):
        for index in (6, 7, 8, 9, 10, 23, 24, 25, 26):
            changes = {index: 0}
            if index in (7, 8):
                changes = {7: 0, 8: 0}
            report = analyze(fixture(header=changes))
            with self.subTest(index=index):
                self.assertFalse(report["observationComplete"])

    def test_install_failures_are_visible(self):
        for status in (0, 1, 3, 4, 5):
            report = analyze(fixture(header={11: status}))
            with self.subTest(status=status):
                self.assertIn("hooksNotInstalled", reasons(report))
                self.assertFalse(report["certified"])
                self.assertFalse(report["modules"]["expectedProfileVerifiedAtInstall"])

    def test_expected_pe_profile_is_not_rejected_module_observation(self):
        report = analyze(fixture(header={11: 3}))
        self.assertNotIn("gameTimeDateStamp", report["modules"])
        self.assertIn("gameTimeDateStamp", report["modules"]["expectedProfile"])
        self.assertFalse(report["modules"]["expectedProfileVerifiedAtInstall"])
        self.assertTrue(analyze()["modules"]["expectedProfileVerifiedAtInstall"])

    def test_partial_hook_mask_is_visible(self):
        report = analyze(fixture(header={12: 0x1F}))
        self.assertIn("hookCoverageIncomplete", reasons(report))

    def test_all_loss_counters_are_fail_visible(self):
        for index, name in reader.COUNTERS.items():
            report = analyze(fixture(header={index: 1}))
            with self.subTest(name=name):
                self.assertFalse(report["observationComplete"])
                self.assertEqual(report["counters"][name], 1)

    def test_no_events_is_not_vacuously_complete(self):
        report = analyze(fixture([]))
        self.assertEqual(report["journal"]["trustedPrefixEvents"], 0)
        self.assertIn("noEventsObserved", reasons(report))
        self.assertFalse(report["observationComplete"])

    def test_sequence_gap_stops_derivation(self):
        records = [event(), event(pointer=HIGH, serial=2), event(6, HIGH, 2)]
        for index, record in enumerate(records):
            record[0] = index + 1
        records[1][0] = 3
        report = analyze(fixture(records, preserve_sequence=True))
        self.assertIn("commitSequenceMismatch", reasons(report))
        self.assertEqual(report["journal"]["trustedPrefixEvents"], 1)
        self.assertFalse(report["observations"]["hasHighAllocationReturn"])
        self.assertFalse(report["observations"]["hasHighNodeBoundary"])

    def test_declared_but_zero_slot_is_incomplete_not_fake_event(self):
        report = analyze(fixture([event()], header={5: 2}))
        self.assertIn("commitSequenceMismatch", reasons(report))
        self.assertEqual(report["journal"]["trustedPrefixEvents"], 1)

    def test_commit_before_count_is_visible_not_promoted(self):
        report = analyze(fixture([event(), event(pointer=HIGH, serial=2)], header={5: 1}))
        self.assertIn("dataBeyondCommittedCount", reasons(report))
        self.assertEqual(report["journal"]["undeclaredPublishedSlots"], 1)
        self.assertFalse(report["observations"]["hasHighAllocationReturn"])

    def test_partial_tail_without_commit_is_visible(self):
        data = bytearray(fixture())
        struct.pack_into("<I", data, 256 + 128 + 4, 1)
        report = analyze(bytes(data))
        self.assertIn("dataBeyondCommittedCount", reasons(report))
        self.assertEqual(report["journal"]["undeclaredPublishedSlots"], 0)

    def test_malformed_published_tail_is_not_ignored(self):
        record = event(pointer=HIGH, serial=2)
        record[25] = 1
        with self.assertRaises(reader.TraceFormatError):
            analyze(fixture([event(), record], header={5: 1}))

    def test_read_failure_fact_is_visible_even_missing_counter(self):
        report = analyze(fixture([event(w13=8)]))
        self.assertIn("eventReadFailure", reasons(report))

    def test_unknown_allocation_serial_is_not_lifetime_proof(self):
        report = analyze(fixture([event(6, serial=0)], header={18: 1}))
        self.assertEqual(report["observations"]["counts"]["eventsWithUnknownAllocationSerial"], 1)
        self.assertIn("unknownAllocationSerial", reasons(report))
        self.assertFalse(report["lifetimeCertified"])

    def test_unknown_serial_visible_even_without_summary_counter(self):
        report = analyze(fixture([event(6, serial=0)]))
        self.assertFalse(report["observationComplete"])
        self.assertIn("unknownAllocationSerial", reasons(report))


class HistoryTests(unittest.TestCase):
    def test_high_alloc_return_only_needs_actual_return_record(self):
        report = analyze(fixture([event(pointer=HIGH)]))
        self.assertTrue(report["observations"]["hasHighAllocationReturn"])
        self.assertFalse(report["observations"]["hasHighNodeBoundary"])
        self.assertFalse(report["causeCertified"])

    def test_boundary_high_does_not_manufacture_allocation_return(self):
        report = analyze(fixture([event(6, HIGH, 0)], header={18: 1}))
        self.assertTrue(report["observations"]["hasHighNodeBoundary"])
        self.assertFalse(report["observations"]["hasHighAllocationReturn"])
        self.assertFalse(report["observations"]["highNodeBoundaryExamples"][0]["matchedRecordedReturn"])

    def test_global_backlink_offset_matches_independent_game_instructions(self):
        # Exact Game SHA E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A.
        # Independently retained LLVM disassembly, not a reader-derived fixture:
        # +12D951 8D 56 0C => lea edx,[esi+0xC] (esi is original node here)
        # +12D95B 8B 4A 04 => mov ecx,[edx+0x4] (global signed backlink)
        # +12D96D 89 31    => mov [ecx],esi (failed decoded write in old dump).
        global_link_lea = bytes.fromhex("8D 56 0C")
        backlink_load = bytes.fromhex("8B 4A 04")
        node_offset = global_link_lea[2] + backlink_load[2]
        self.assertEqual(node_offset, 16)
        self.assertEqual(reader.GLOBAL_BACKLINK_NODE_OFFSET, node_offset)
        self.assertEqual(reader.GLOBAL_BACKLINK_EVENT_WORD, 14 + node_offset // 4)
        record = event(9, w13=1, w15=0x1000, w16=0x2000, w17=0x3000, w18=HIGH)
        report = analyze(fixture([event(), record]))
        sample = report["observations"]["preUnlinkHighBitGlobalBacklinkExamples"][0]
        self.assertEqual(sample["nodePlus16"], HIGH)
        self.assertNotIn("nodePlus4", sample)

    def test_high_bucket_links_or_global_next_do_not_set_global_backlink_flag(self):
        # node+4=w15 is bucket NEXT; node+8=w16 is bucket BACKLINK;
        # node+12=w17 is global NEXT. None is the failing global backlink.
        for wrong_slot in (15, 16, 17):
            record = event(9, w13=1, w18=0x12345678)
            record[wrong_slot] = HIGH
            report = analyze(fixture([event(), record]))
            with self.subTest(wrong_slot=wrong_slot):
                self.assertFalse(report["observations"]["hasPreUnlinkHighBitGlobalBacklinkField"])
                self.assertEqual(report["observations"]["preUnlinkHighBitGlobalBacklinkExamples"], [])
                self.assertFalse(report["observations"]["hasAnyHighReturnedAddress"])
                self.assertFalse(report["certified"])

    def test_complemented_sentinel_is_not_claimed_as_a_returned_pointer(self):
        # ~(Game+tableAnchor) is a normal signed-link sentinel shape, not an
        # allocation return. This synthetic fixture verifies interpretation;
        # it does not certify a particular game's pointer lifetime.
        sentinel = (~0x6FBC6AC8) & 0xFFFFFFFF
        report = analyze(fixture([event(), event(9, w13=1, w18=sentinel)]))
        observations = report["observations"]
        self.assertTrue(observations["hasPreUnlinkHighBitGlobalBacklinkField"])
        self.assertNotIn("hasPreUnlinkHighBacklink", observations)
        self.assertFalse(observations["hasAnyHighReturnedAddress"])
        sample = observations["preUnlinkHighBitGlobalBacklinkExamples"][0]
        self.assertTrue(sample["rawHighBitFieldNotNecessarilyPointer"])
        self.assertFalse(sample["matchesAnyRecordedReturnedAddress"])
        self.assertFalse(sample["matchesEarlierRecordedReturnedAddress"])
        self.assertFalse(report["certified"])

    def test_high_bit_matches_earlier_return_is_only_numeric_observation(self):
        report = analyze(fixture([event(), event(pointer=HIGH, serial=2),
                                  event(9, w13=1, w18=HIGH)]))
        sample = report["observations"]["preUnlinkHighBitGlobalBacklinkExamples"][0]
        self.assertTrue(sample["matchesAnyRecordedReturnedAddress"])
        self.assertTrue(sample["matchesEarlierRecordedReturnedAddress"])
        self.assertTrue(sample["rawHighBitFieldNotNecessarilyPointer"])
        self.assertFalse(report["lifetimeCertified"])

    def test_high_bit_matches_only_later_return_not_promoted_to_earlier_match(self):
        report = analyze(fixture([event(), event(9, w13=1, w18=HIGH),
                                  event(pointer=HIGH, serial=2)]))
        sample = report["observations"]["preUnlinkHighBitGlobalBacklinkExamples"][0]
        self.assertTrue(sample["matchesAnyRecordedReturnedAddress"])
        self.assertFalse(sample["matchesEarlierRecordedReturnedAddress"])
        self.assertFalse(report["lifetimeCertified"])

    def test_new_serial_realloc_and_address_reuse_remain_distinct(self):
        report = analyze(fixture(operation_sequence()))
        self.assertTrue(report["observationComplete"])
        observations = report["observations"]
        self.assertTrue(observations["hasHighReallocationReturn"])
        self.assertTrue(observations["hasPreUnlinkHighBitGlobalBacklinkField"])
        self.assertEqual(observations["counts"]["matchedOperationPairs"], 2)
        self.assertEqual(observations["highNodeBoundaryExamples"][0]["recordedReturnSequence"], 4)
        self.assertEqual(observations["highNodeBoundaryExamples"][0]["recordedReturnKind"], "Realloc")
        self.assertEqual([item["allocationSerial"] for item in observations["requestIntervalExamples"]], [1, 2, 3])
        self.assertEqual(report["pendingOps"], [])

    def test_in_place_realloc_new_serial(self):
        records = [event(), event(4, token=1),
                   event(5, token=1, w5=LOW, w12=LOW, w13=16, w24=2), event(6, serial=2)]
        report = analyze(fixture(records))
        self.assertTrue(report["observationComplete"])
        self.assertEqual(report["observations"]["counts"]["boundaryEventsMatchedToRecordedReturn"], 1)

    def test_failed_realloc_does_not_invent_new_allocation(self):
        records = [event(), event(4, token=1), event(5, token=1), event(6)]
        report = analyze(fixture(records))
        self.assertTrue(report["observationComplete"])
        self.assertEqual(len(report["observations"]["requestIntervalExamples"]), 1)

    def test_unknown_realloc_return_visible_without_serial(self):
        records = [event(4, serial=0, token=1),
                   event(5, serial=0, token=1, w5=HIGH, w12=HIGH, w13=16)]
        report = analyze(fixture(records, header={18: 1}))
        self.assertIn("returnedAddressWithoutAllocationSerial", reasons(report))
        self.assertTrue(report["observations"]["hasHighReallocationReturn"])
        self.assertFalse(report["lifetimeCertified"])

    def test_serial_reuse_is_not_merged(self):
        report = analyze(fixture([event(), event(pointer=HIGH)]))
        self.assertIn("allocationSerialReused", reasons(report))

    def test_serial_mismatched_pointer_is_visible(self):
        report = analyze(fixture([event(), event(6, HIGH)]))
        self.assertIn("allocationSerialPointerMismatch", reasons(report))

    def test_serial_missing_from_history_is_visible(self):
        report = analyze(fixture([event(6, serial=999)]))
        self.assertIn("allocationSerialNotInRecordedReturnHistory", reasons(report))

    def test_pending_free_or_realloc_is_visible(self):
        for kind in (2, 4):
            report = analyze(fixture([event(), event(kind, token=1)]))
            with self.subTest(kind=kind):
                self.assertIn("pendingOperationsAtExit", reasons(report))
                self.assertEqual(report["pendingOps"][0]["operationToken"], 1)

    def test_end_without_begin_and_repeated_end_are_visible(self):
        for records in ([event(), event(3, token=1)],
                        [event(), event(2, token=1), event(3, token=1), event(3, token=1)]):
            report = analyze(fixture(records))
            self.assertIn("operationEndWithoutBegin", reasons(report))

    def test_wrong_kind_pointer_or_serial_does_not_consume_pending(self):
        ends = [event(5, token=1), event(3, HIGH, 1, 1), event(3, LOW, 2, 1)]
        for end in ends:
            report = analyze(fixture([event(), event(2, token=1), end]))
            with self.subTest(end=end):
                self.assertIn("operationBeginEndMismatch", reasons(report))
                self.assertEqual(len(report["pendingOps"]), 1)

    def test_token_reuse_even_after_complete_is_visible(self):
        report = analyze(fixture([event(), event(2, token=1), event(3, token=1), event(2, token=1)]))
        self.assertIn("operationTokenReused", reasons(report))

    def test_32bit_interval_wrap_not_silently_truncated(self):
        report = analyze(fixture([event(pointer=0xFFFFFFF0, w8=22404)]))
        self.assertIn("requestedIntervalWraps32BitAddressSpace", reasons(report))
        self.assertGreater(report["observations"]["requestIntervalExamples"][0]["exclusiveEnd64"], 1 << 32)

    def test_return_examples_are_bounded(self):
        records = [event(pointer=LOW + index * 0x6000, serial=index + 1) for index in range(100)]
        report = analyze(fixture(records))
        self.assertEqual(len(report["observations"]["requestIntervalExamples"]), reader.EXAMPLE_LIMIT)
        self.assertEqual(report["observations"]["requestIntervalExamplesTruncated"], 36)

    def test_late_high_return_has_separate_sample_budget(self):
        records = [event(pointer=LOW, serial=index + 1) for index in range(100)]
        records.append(event(pointer=HIGH, serial=101))
        report = analyze(fixture(records))
        self.assertEqual(report["observations"]["highReturnedAddressExamples"][0]["sequence"], 101)
        self.assertEqual(report["observations"]["highReturnedAddressExamples"][0]["pointer"], HIGH)

    def test_no_free_never_becomes_current_liveness_claim(self):
        report = analyze()
        serialized = json.dumps(report)
        self.assertNotIn('"live": true', serialized)
        self.assertNotIn('"liveAllocations"', serialized)
        self.assertIn("allocationStillLiveBecauseNoFreeWasRecorded", report["claimsNotSupported"])
        self.assertFalse(report["allocatorRouteCertified"])


class FileAndRoundtripTests(unittest.TestCase):
    def test_file_reader_and_cli_complete_report(self):
        with tempfile.TemporaryDirectory(prefix="mdl-reader-") as folder:
            source = Path(folder) / "journal.bin"
            target = Path(folder) / "report.json"
            source.write_bytes(fixture())
            report = reader.analyze_file(source, process_exited=True, expected_pid=1234,
                                         expected_creation_time=CREATION)
            self.assertTrue(report["observationComplete"])
            with contextlib.redirect_stdout(io.StringIO()):
                status = reader.main(["--input", str(source), "--output", str(target),
                                      "--process-exited", "--expected-pid", "1234",
                                      "--expected-creation-time", hex(CREATION)])
            self.assertEqual(status, 0)
            self.assertIs(json.loads(target.read_text(encoding="utf-8"))["certified"], False)

    def test_cli_malformed_and_incomplete_are_nonzero(self):
        with tempfile.TemporaryDirectory(prefix="mdl-reader-") as folder:
            source, target = Path(folder) / "journal.bin", Path(folder) / "report.json"
            for data, status, verdict in ((b"short", 2, "MALFORMED_OR_UNREADABLE"),
                                           (fixture([]), 1, "INCOMPLETE_OBSERVATION")):
                source.write_bytes(data)
                with contextlib.redirect_stdout(io.StringIO()):
                    result = reader.main(["--input", str(source), "--output", str(target), "--process-exited"])
                self.assertEqual(result, status)
                self.assertEqual(json.loads(target.read_text(encoding="utf-8"))["verdict"], verdict)

    def test_cli_requires_explicit_process_exit(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            reader.main(["--input", "unused", "--output", "unused"])

    def test_file_post_exit_check_precedes_open(self):
        with self.assertRaises(reader.TraceFormatError):
            reader.analyze_file("path-not-touched.bin", process_exited=False)

    def test_cli_cannot_overwrite_raw_input(self):
        with tempfile.TemporaryDirectory(prefix="mdl-reader-") as folder:
            source = Path(folder) / "journal.bin"
            data = fixture()
            source.write_bytes(data)
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                reader.main(["--input", str(source), "--output", str(source), "--process-exited"])
            self.assertEqual(source.read_bytes(), data)

    @unittest.skipUnless(os.environ.get("WARVK_MDL_TRACE_CPP_FIXTURE"),
                         "C++ production --emit fixture not provided; no production-roundtrip claim")
    def test_actual_cpp_fixture_roundtrip(self):
        report = reader.analyze_file(os.environ["WARVK_MDL_TRACE_CPP_FIXTURE"], process_exited=True,
                                     expected_pid=1234, expected_creation_time=CREATION)
        self.assertTrue(report["observationComplete"], report["gaps"])
        self.assertTrue(report["observations"]["hasHighReallocationReturn"])
        self.assertTrue(report["observations"]["hasHighNodeBoundary"])
        self.assertTrue(report["observations"]["hasPreUnlinkHighBitGlobalBacklinkField"])
        self.assertGreaterEqual(report["observations"]["counts"]["matchedOperationPairs"], 2)
        self.assertGreaterEqual(report["observations"]["counts"]["boundaryEventsMatchedToRecordedReturn"], 2)
        self.assertEqual(report["pendingOps"], [])
        self.assertFalse(report["certified"])
        self.assertEqual(report["coverageFlags"], 15)


if __name__ == "__main__":
    unittest.main(verbosity=2)
