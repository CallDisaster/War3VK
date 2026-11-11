"""Pure low-VA census tests. No WinAPI, process, game, build or payload reads."""
import ast
import copy
from pathlib import Path
import unittest

import native_game_va_sample as gate


class Native:
    def __init__(self):
        self.process = 44
        self.expected = dict(pid=123, creation100ns=456,
                             actualExecutable=r"E:\sandbox\war3.exe", desktop="Isolated")
        self.pending = []
        self.verifications = 0
        self.failure_at = None

    def verify_process(self):
        self.verifications += 1
        if self.failure_at == self.verifications:
            raise ValueError("injected identity verification failure")


class Api:
    def __init__(self, rows=None):
        self.rows = rows or [dict(base=0, size=gate.LOW_END, state=gate.MEM_FREE, type=0)]
        self.calls = []
        self.process_id = 123
        self.after_query = None
        self.error_at = None
        self.query_count = 0

    def pid(self, process):
        self.calls.append(("pid", process))
        return self.process_id

    def query(self, process, address):
        self.calls.append(("query", process, address))
        self.query_count += 1
        if self.error_at == self.query_count:
            raise ValueError("injected query failure")
        row = next((row for row in self.rows if row["base"] <= address < row["base"] + row["size"]), None)
        if row is None:
            raise ValueError("no fixture region")
        if self.after_query:
            self.after_query()
        return copy.deepcopy(row)


class Contracts(unittest.TestCase):
    def setUp(self):
        self.native, self.api = Native(), Api()
        self.now = 0.0

    def sample(self, **kwargs):
        return gate.sample(self.native, _api=self.api, _clock=lambda: self.now, **kwargs)

    def partial(self, result, reason):
        self.assertFalse(result["complete"], result)
        self.assertTrue(result["totalsArePartial"], result)
        self.assertEqual(result["stopReason"], reason)
        self.assertFalse(result["snapshotAtomic"])
        self.assertFalse(result["allocationFailureCauseProven"])
        self.assertFalse(result["payloadRead"])
        self.assertFalse(result["targetSuspended"])
        self.assertFalse(result["processReopened"])

    def test_complete_single_free_span_clips_null_range_and_upper_limit(self):
        self.api.rows[0]["size"] = 1 << 32
        result = self.sample()
        self.assertTrue(result["complete"], result)
        self.assertEqual(result["freeBytes"], gate.LOW_END - gate.LOW_BEGIN)
        self.assertEqual(result["largestAlignedFreeHoleBytes"], result["freeBytes"])
        self.assertEqual(result["largestAlignedFreeHoleStart"], gate.LOW_BEGIN)
        self.assertEqual(result["largestAlignedFreeHoleEnd"], gate.LOW_END)
        self.assertEqual(result["queries"], 1)
        self.assertEqual(self.api.calls, [("pid", 44), ("query", 44, 0x10000), ("pid", 44)])
        self.assertEqual(self.native.verifications, 2)

    def test_state_and_mapping_type_are_independent_accounting_axes(self):
        self.api.rows = [
            dict(base=0x10000, size=0x10000, state=gate.MEM_COMMIT, type=gate.MEM_IMAGE),
            dict(base=0x20000, size=0x10000, state=gate.MEM_RESERVE, type=gate.MEM_PRIVATE),
            dict(base=0x30000, size=0x10000, state=gate.MEM_COMMIT, type=gate.MEM_MAPPED),
            dict(base=0x40000, size=gate.LOW_END - 0x40000, state=gate.MEM_FREE, type=0)]
        result = self.sample()
        self.assertTrue(result["complete"], result)
        self.assertEqual(result["states"]["committed"], dict(bytes=0x20000, regions=2))
        self.assertEqual(result["states"]["reserved"], dict(bytes=0x10000, regions=1))
        self.assertEqual(result["types"]["private"],
                         dict(bytes=0x10000, regions=1, committedBytes=0, reservedBytes=0x10000))
        self.assertEqual(result["types"]["image"]["committedBytes"], 0x10000)
        self.assertEqual(result["types"]["mapped"]["committedBytes"], 0x10000)

    def test_hole_alignment_never_rounds_available_length_up(self):
        result = dict(largestAlignedFreeHoleBytes=0)
        gate._account_hole(result, 0x11000, 0x51000)
        self.assertEqual(result["largestAlignedFreeHoleStart"], 0x20000)
        self.assertEqual(result["largestAlignedFreeHoleBytes"], 0x31000)
        gate._account_hole(result, 0x61000, 0x62000)
        self.assertEqual(result["largestAlignedFreeHoleBytes"], 0x31000)

    def test_adjacent_free_rows_merge_without_crossing_allocation(self):
        self.api.rows = [
            dict(base=0x10000, size=0x1000, state=gate.MEM_COMMIT, type=gate.MEM_PRIVATE),
            dict(base=0x11000, size=0x10000, state=gate.MEM_FREE, type=0),
            dict(base=0x21000, size=0x20000, state=gate.MEM_FREE, type=0),
            dict(base=0x41000, size=gate.LOW_END - 0x41000, state=gate.MEM_RESERVE, type=gate.MEM_PRIVATE)]
        result = self.sample()
        self.assertTrue(result["complete"], result)
        self.assertEqual(result["largestAlignedFreeHoleStart"], 0x20000)
        self.assertEqual(result["largestAlignedFreeHoleBytes"], 0x21000)

    def test_query_failure_keeps_partial_totals_and_revalidates(self):
        self.api.rows = [dict(base=0x10000, size=0x10000, state=gate.MEM_FREE, type=0)]
        result = self.sample()
        self.partial(result, "error")
        self.assertEqual(result["queries"], 2)
        self.assertEqual(result["coveredBytes"], 0x10000)
        self.assertEqual(result["largestAlignedFreeHoleBytes"], 0x10000)
        self.assertTrue(result["identityVerifiedAfter"])

    def test_deadline_is_incomplete_even_if_last_query_covers_all_space(self):
        self.api.after_query = lambda: setattr(self, "now", 1.0)
        result = self.sample()
        self.partial(result, "deadline")
        self.assertEqual(result["queries"], 1)
        self.assertEqual(result["nextAddress"], gate.LOW_END)
        self.assertTrue(result["identityVerifiedAfter"])

    def test_query_limit_cannot_expand(self):
        class PageApi(Api):
            def query(self, process, address):
                self.query_count += 1
                return dict(base=address, size=4096, state=gate.MEM_COMMIT, type=gate.MEM_PRIVATE)
        self.api = PageApi()
        result = self.sample()
        self.partial(result, "query-limit")
        self.assertEqual(result["queries"], 32768)
        self.assertEqual(self.api.query_count, 32768)
        self.assertEqual(result["coveredBytes"], 32768 * 4096)

    def test_foreign_pid_refused_before_query(self):
        self.api.process_id = 124
        self.partial(self.sample(), "error")
        self.assertEqual(self.api.query_count, 0)

    def test_pending_or_invalid_authority_refused(self):
        for field, value in (("process", 0), ("process", True), ("expected", None),
                             ("expected", {}), ("pending", [object()])):
            with self.subTest(field=field):
                self.native = Native()
                setattr(self.native, field, value)
                self.partial(self.sample(), "error")
                self.assertEqual(self.api.query_count, 0)

    def test_identity_change_after_query_invalidates_full_scan(self):
        for change in (
                lambda: setattr(self.native, "process", 45),
                lambda: self.native.expected.update(creation100ns=457),
                lambda: setattr(self.api, "process_id", 124),
                lambda: self.native.pending.append(object())):
            with self.subTest(change=change):
                self.native, self.api = Native(), Api()
                self.api.after_query = change
                result = self.sample()
                self.partial(result, "identity-changed")
                self.assertFalse(result["identityVerifiedAfter"])

    def test_existing_verify_process_failures_are_not_bypassed(self):
        for number, reason in ((1, "error"), (2, "identity-changed")):
            with self.subTest(number=number):
                self.native, self.api = Native(), Api()
                self.native.failure_at = number
                self.partial(self.sample(), reason)

    def test_second_attempt_never_queries_again(self):
        self.assertTrue(self.sample()["complete"])
        before = list(self.api.calls)
        self.partial(self.sample(), "error")
        self.assertEqual(self.api.calls, before)

    def test_deadline_values_reject_boolean_nan_infinity_or_expansion(self):
        for budget in (True, False, 0, -1, float("nan"), float("inf"), 1.00001, "1"):
            with self.subTest(budget=budget):
                self.partial(self.sample(budget_seconds=budget), "error")
                self.assertFalse(self.api.calls)

    def test_malformed_nonprogressing_regions_fail_closed(self):
        base = dict(base=0, size=gate.LOW_END, state=gate.MEM_FREE, type=0)
        for field, value in (("base", True), ("base", -1), ("base", 0x20000),
                ("size", 0), ("size", 1), ("size", 1 << 64), ("state", 0),
                ("state", True), ("type", gate.MEM_PRIVATE)):
            with self.subTest(field=field, value=value):
                row = dict(base)
                row[field] = value
                with self.assertRaises(ValueError):
                    gate._region(row, gate.LOW_BEGIN)
        with self.assertRaises(ValueError):
            gate._region(dict(base, state=gate.MEM_COMMIT, type=0), gate.LOW_BEGIN)

    def test_module_has_only_metadata_native_calls_and_no_file_io(self):
        source = Path(gate.__file__).read_text("utf-8")
        tree = ast.parse(source)
        calls = {node.func.attr for node in ast.walk(tree)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)}
        forbidden = {"OpenProcess", "OpenThread", "ReadProcessMemory", "WriteProcessMemory",
            "VirtualAllocEx", "VirtualFreeEx", "SuspendThread", "ResumeThread", "DebugActiveProcess",
            "CreateProcessW", "TerminateProcess", "TerminateJobObject", "SwitchDesktop", "SendInput",
            "Popen", "run", "write_text", "write_bytes", "open"}
        self.assertFalse(forbidden & calls)
        self.assertIn("VirtualQueryEx", calls)
        self.assertNotIn("__main__", source)


if __name__ == "__main__":
    unittest.main()
