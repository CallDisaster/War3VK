"""Positive and negative tests for the targeted Python checks entry (M2).

Standard library only (unittest + unittest.mock). Every test here exercises
the real functions of AutoTest/run_architecture_python_checks.py; nothing
re-implements the entry's model. All subprocess launches are mocked, all
files live in temporary directories: this file never runs native code, never
invokes a run_* native gate, never starts a game or touches a GPU.

The 64 KiB stream clipping verified below is only a cap on what the summary
records; it is not a subprocess memory limit and not process-level memory
protection.
"""

import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import run_architecture_python_checks as runner

EXPECTED_RECORDER = (
    "test_analyze_frame_evidence.py",
    "test_analyze_frame_history.py",
    "test_frame_inputs.py",
    "test_frame_evidence_control_static.py",
    "test_frame_history_static.py",
    "test_frame_history_shortcut_static.py",
    "test_frame_recorder_self_contained_static.py",
    "test_frame_recorder_memory_static.py",
    "test_frame_recorder_default_policy_static.py",
    "test_self_contained_recorder_gate.py",
    "test_recorder_control_lease_gate.py",
)

EXPECTED_RENDER_HOST = (
    "test_render_host_transport_gate.py",
    "test_render_host_shared_slots_gate.py",
    "test_render_host_producer_inbox_gate.py",
    "test_render_host_sample_worker_gate.py",
)

EXPECTED_ALL = EXPECTED_RECORDER + EXPECTED_RENDER_HOST


class GroupSelectionTests(unittest.TestCase):
    def test_recorder_group_exact_frozen_order(self):
        self.assertEqual(runner.select_group("recorder"), EXPECTED_RECORDER)

    def test_render_host_group_exact_frozen_order(self):
        self.assertEqual(runner.select_group("render-host"), EXPECTED_RENDER_HOST)

    def test_all_group_exact_frozen_order(self):
        self.assertEqual(runner.select_group("all"), EXPECTED_ALL)
        self.assertEqual(len(EXPECTED_ALL), 15)

    def test_all_is_recorder_then_render_host(self):
        self.assertEqual(
            runner.select_group("all"),
            runner.select_group("recorder") + runner.select_group("render-host"),
        )

    def test_groups_have_no_duplicates(self):
        for group in runner.GROUP_CHOICES:
            names = runner.select_group(group)
            self.assertEqual(len(names), len(set(names)), group)

    def test_select_group_rejects_unknown_group(self):
        with self.assertRaises(ValueError):
            runner.select_group("everything")

    def test_cli_rejects_unknown_group(self):
        err = io.StringIO()
        with redirect_stderr(err):
            with self.assertRaises(SystemExit) as ctx:
                runner.main(["--group", "not-a-group"])
        self.assertEqual(ctx.exception.code, 2)

    def test_whitelist_excludes_entry_and_its_own_test(self):
        names = runner.select_group("all")
        self.assertNotIn("run_architecture_python_checks.py", names)
        self.assertNotIn("test_architecture_python_checks_runner.py", names)

    def test_real_whitelist_files_exist(self):
        for name in runner.select_group("all"):
            self.assertTrue((runner.AUTO_TEST_DIR / name).is_file(), name)


class ListingModeTests(unittest.TestCase):
    def test_list_prints_selection_without_launching_or_importing(self):
        module_names = [name[:-3] for name in runner.select_group("all")]
        for name in module_names:
            self.assertNotIn(name, sys.modules)
        out = io.StringIO()
        err = io.StringIO()
        with mock.patch.object(runner.subprocess, "run") as run_mock:
            with redirect_stdout(out), redirect_stderr(err):
                code = runner.main(["--list", "--group", "all"])
            run_mock.assert_not_called()
        self.assertEqual(code, 0)
        payload = json.loads(out.getvalue())
        self.assertEqual(payload["mode"], "list")
        self.assertEqual(payload["group"], "all")
        self.assertEqual(payload["count"], 15)
        self.assertEqual(payload["scripts"], list(EXPECTED_ALL))
        self.assertFalse(payload["verified"])
        self.assertEqual(payload["attempted"], 0)
        self.assertEqual(payload["executed"], 0)
        self.assertNotIn("ok", payload)
        for name in module_names:
            self.assertNotIn(name, sys.modules)

    def test_list_with_output_is_rejected(self):
        err = io.StringIO()
        with redirect_stderr(err):
            with self.assertRaises(SystemExit) as ctx:
                runner.main(["--list", "--output", "never-created.json"])
        self.assertEqual(ctx.exception.code, 2)


class PreflightTests(unittest.TestCase):
    def test_verify_selection_reports_missing_files(self):
        problems = runner.verify_selection(
            [
                runner.AUTO_TEST_DIR / "definitely_missing_a.py",
                runner.AUTO_TEST_DIR / "definitely_missing_b.py",
            ]
        )
        entries = {(p["kind"], p["path"]) for p in problems}
        self.assertIn(("missing", "AutoTest/definitely_missing_a.py"), entries)
        self.assertIn(("missing", "AutoTest/definitely_missing_b.py"), entries)

    def test_verify_selection_reports_duplicates(self):
        path = runner.AUTO_TEST_DIR / "test_frame_inputs.py"
        problems = runner.verify_selection([path, path])
        self.assertTrue(any(p["kind"] == "duplicate" for p in problems))
        self.assertFalse(any(p["kind"] == "missing" for p in problems))

    def test_missing_file_refuses_entire_run_before_any_launch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "present_a.py").write_bytes(b"# synthetic\n")
            (root / "present_b.py").write_bytes(b"# synthetic\n")
            names = ("present_a.py", "absent.py", "present_b.py")
            out = io.StringIO()
            err = io.StringIO()
            with mock.patch.object(runner, "GROUPS", {"all": names}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(runner.subprocess, "run") as run_mock:
                        with redirect_stdout(out), redirect_stderr(err):
                            code = runner.main(["--group", "all"])
                        run_mock.assert_not_called()
            self.assertNotEqual(code, 0)
            summary = json.loads(out.getvalue())
            self.assertFalse(summary["ok"])
            self.assertEqual(summary["counts"]["selected"], 3)
            self.assertEqual(summary["counts"]["executed"], 0)
            self.assertTrue(any(p["kind"] == "missing" for p in summary["problems"]))

    def test_duplicate_selection_refuses_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "dup.py").write_bytes(b"# synthetic\n")
            out = io.StringIO()
            err = io.StringIO()
            with mock.patch.object(runner, "GROUPS", {"all": ("dup.py", "dup.py")}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(runner.subprocess, "run") as run_mock:
                        with redirect_stdout(out), redirect_stderr(err):
                            code = runner.main(["--group", "all"])
                        run_mock.assert_not_called()
            self.assertNotEqual(code, 0)
            summary = json.loads(out.getvalue())
            self.assertFalse(summary["ok"])
            self.assertTrue(any(p["kind"] == "duplicate" for p in summary["problems"]))

    def test_zero_execution_is_not_ok(self):
        out = io.StringIO()
        err = io.StringIO()
        with mock.patch.object(runner, "GROUPS", {"all": ()}):
            with mock.patch.object(runner.subprocess, "run") as run_mock:
                with redirect_stdout(out), redirect_stderr(err):
                    code = runner.main(["--group", "all"])
                run_mock.assert_not_called()
        self.assertNotEqual(code, 0)
        summary = json.loads(out.getvalue())
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["counts"]["selected"], 0)
        self.assertEqual(summary["counts"]["executed"], 0)


class ClipStreamTests(unittest.TestCase):
    """Real-function negative cases for clip_stream.

    The 64 KiB cap applies to what is recorded: the returned text must be
    valid Unicode whose UTF-8 encoding fits in 64 KiB, with the truncation
    flag set whenever content was dropped and the original raw byte count
    preserved. This is not a subprocess memory limit and not process-level
    memory protection.
    """

    def test_invalid_utf8_cannot_expand_past_the_byte_cap(self):
        text, truncated, raw = runner.clip_stream(b"\xff" * 70000)
        self.assertEqual(raw, 70000)
        self.assertTrue(truncated)
        self.assertLessEqual(len(text.encode("utf-8")), 65536)
        # 70000 invalid bytes: the cap admits floor(65536/3) U+FFFD chars.
        self.assertEqual(text, "\ufffd" * 21845)

    def test_multibyte_char_at_the_clip_boundary_is_dropped_whole(self):
        data = b"A" * 65535 + b"\xe4\xb8\xad" + b"X" * 100
        text, truncated, raw = runner.clip_stream(data)
        self.assertEqual(raw, 65638)
        self.assertTrue(truncated)
        self.assertLessEqual(len(text.encode("utf-8")), 65536)
        # The 0xe4 lead byte at the clip boundary becomes one U+FFFD (3
        # bytes), which no longer fits, so the record ends at the A's.
        self.assertEqual(text, "A" * 65535)

    def test_multibyte_char_exactly_filling_the_cap_is_kept(self):
        data = b"A" * 65533 + b"\xe4\xb8\xad"
        text, truncated, raw = runner.clip_stream(data)
        self.assertEqual(raw, 65536)
        self.assertFalse(truncated)
        self.assertEqual(text, "A" * 65533 + "\u4e2d")
        self.assertEqual(len(text.encode("utf-8")), 65536)

    def test_expansion_below_raw_cap_still_marks_truncation(self):
        # Even when the raw stream is under the cap, replacement expansion
        # can force a trim; the flag must then report dropped content.
        text, truncated, raw = runner.clip_stream(b"\xff" * 60000)
        self.assertEqual(raw, 60000)
        self.assertTrue(truncated)
        self.assertLessEqual(len(text.encode("utf-8")), 65536)
        self.assertEqual(text, "\ufffd" * 21845)


class OutputFileTests(unittest.TestCase):
    def test_existing_output_refused_before_any_launch_and_bytes_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "summary.json"
            sentinel = b'{"pre-existing": true}\n'
            target.write_bytes(sentinel)
            out = io.StringIO()
            err = io.StringIO()
            with mock.patch.object(runner.subprocess, "run") as run_mock:
                with redirect_stdout(out), redirect_stderr(err):
                    code = runner.main(
                        ["--group", "recorder", "--output", str(target)]
                    )
                run_mock.assert_not_called()
            self.assertNotEqual(code, 0)
            self.assertEqual(target.read_bytes(), sentinel)
            self.assertNotIn('"ok"', out.getvalue())
            self.assertIn("refusing", err.getvalue())

    def test_output_written_on_success_matches_stdout_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ("a.py", "b.py")
            for name in names:
                (root / name).write_bytes(b"# synthetic\n")
            target = Path(tmp) / "summary.json"
            out = io.StringIO()
            err = io.StringIO()

            def fake_run(command, **kwargs):
                return subprocess.CompletedProcess(command, 0, stdout=b"ok", stderr=b"")

            with mock.patch.object(runner, "GROUPS", {"all": tuple(names)}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(
                        runner.subprocess, "run", side_effect=fake_run
                    ):
                        with redirect_stdout(out), redirect_stderr(err):
                            code = runner.main(
                                ["--group", "all", "--output", str(target)]
                            )
            self.assertEqual(code, 0)
            printed = json.loads(out.getvalue())
            written = json.loads(target.read_text(encoding="utf-8"))
            self.assertEqual(printed, written)
            self.assertTrue(printed["ok"])
            self.assertEqual(printed["counts"]["executed"], 2)
            self.assertTrue(printed["output"]["requested"])
            self.assertIsNone(printed["output"]["error"])

    def test_output_create_failure_is_not_reported_as_success(self):
        # Natural CreateNew failure without any open/write mock: the target
        # itself does not exist, but its parent directory does not either, so
        # opening with "x" raises OSError after the tests already ran. The
        # final stdout JSON must still carry ok=false with an explicit output
        # error and the exit code must be non-zero.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.py").write_bytes(b"# synthetic\n")
            target = Path(tmp) / "no_such_dir" / "summary.json"
            out = io.StringIO()
            err = io.StringIO()

            def fake_run(command, **kwargs):
                return subprocess.CompletedProcess(command, 0, stdout=b"ok", stderr=b"")

            with mock.patch.object(runner, "GROUPS", {"all": ("a.py",)}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(
                        runner.subprocess, "run", side_effect=fake_run
                    ) as run_mock:
                        with redirect_stdout(out), redirect_stderr(err):
                            code = runner.main(
                                ["--group", "all", "--output", str(target)]
                            )
                        self.assertEqual(run_mock.call_count, 1)
            self.assertNotEqual(code, 0)
            summary = json.loads(out.getvalue())
            self.assertFalse(summary["ok"])
            self.assertTrue(summary["output"]["requested"])
            self.assertIsNotNone(summary["output"]["error"])
            self.assertEqual(summary["counts"]["passed"], 1)
            self.assertIn("output publication failed", err.getvalue())
            self.assertFalse(target.exists())

    def test_output_open_failure_makes_ok_false_in_stdout_json(self):
        # Direct negative case against the real main(): every test passes and
        # only the CreateNew open of --output raises OSError. The final stdout
        # JSON must carry ok=false with an explicit output error and the exit
        # code must be non-zero.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ("a.py", "b.py")
            for name in names:
                (root / name).write_bytes(b"# synthetic\n")
            target = Path(tmp) / "summary.json"
            out = io.StringIO()
            err = io.StringIO()
            real_open = open

            def selective_open(file, mode="r", *args, **kwargs):
                if mode == "x" and Path(file).resolve() == target.resolve():
                    raise OSError(13, "simulated CreateNew failure")
                return real_open(file, mode, *args, **kwargs)

            def fake_run(command, **kwargs):
                return subprocess.CompletedProcess(command, 0, stdout=b"ok", stderr=b"")

            with mock.patch.object(runner, "GROUPS", {"all": tuple(names)}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(
                        runner.subprocess, "run", side_effect=fake_run
                    ) as run_mock:
                        with mock.patch("builtins.open", side_effect=selective_open):
                            with redirect_stdout(out), redirect_stderr(err):
                                code = runner.main(
                                    ["--group", "all", "--output", str(target)]
                                )
                        self.assertEqual(run_mock.call_count, 2)
            self.assertNotEqual(code, 0)
            summary = json.loads(out.getvalue())
            self.assertFalse(summary["ok"])
            self.assertTrue(summary["output"]["requested"])
            self.assertIn(
                "simulated CreateNew failure", summary["output"]["error"]
            )
            self.assertEqual(summary["counts"]["passed"], 2)
            self.assertFalse(target.exists())
            self.assertIn("output publication failed", err.getvalue())

    def test_output_write_failure_makes_ok_false_in_stdout_json(self):
        # Same contract when the file opens but the write itself raises.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            names = ("a.py", "b.py")
            for name in names:
                (root / name).write_bytes(b"# synthetic\n")
            target = Path(tmp) / "summary.json"
            out = io.StringIO()
            err = io.StringIO()
            real_open = open

            class ExplodingWriter:
                def __enter__(self):
                    return self

                def write(self, data):
                    raise OSError(28, "simulated disk full")

                def __exit__(self, *exc_info):
                    return False

            def selective_open(file, mode="r", *args, **kwargs):
                if mode == "x" and Path(file).resolve() == target.resolve():
                    return ExplodingWriter()
                return real_open(file, mode, *args, **kwargs)

            def fake_run(command, **kwargs):
                return subprocess.CompletedProcess(command, 0, stdout=b"ok", stderr=b"")

            with mock.patch.object(runner, "GROUPS", {"all": tuple(names)}):
                with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                    with mock.patch.object(
                        runner.subprocess, "run", side_effect=fake_run
                    ):
                        with mock.patch("builtins.open", side_effect=selective_open):
                            with redirect_stdout(out), redirect_stderr(err):
                                code = runner.main(
                                    ["--group", "all", "--output", str(target)]
                                )
            self.assertNotEqual(code, 0)
            summary = json.loads(out.getvalue())
            self.assertFalse(summary["ok"])
            self.assertIn("simulated disk full", summary["output"]["error"])
            self.assertEqual(summary["counts"]["passed"], 2)
            self.assertIn("output publication failed", err.getvalue())


class RunBehaviorTests(unittest.TestCase):
    def fake_tree(self, names):
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        root = Path(holder.name)
        for name in names:
            (root / name).write_bytes(b"# synthetic whitelist script\n")
        return root

    def run_main_with_fake_tree(self, names, fake_run, group="all"):
        root = self.fake_tree(names)
        out = io.StringIO()
        err = io.StringIO()
        with mock.patch.object(runner, "GROUPS", {group: tuple(names)}):
            with mock.patch.object(runner, "AUTO_TEST_DIR", root):
                with mock.patch.object(
                    runner.subprocess, "run", side_effect=fake_run
                ) as run_mock:
                    with redirect_stdout(out), redirect_stderr(err):
                        code = runner.main(["--group", group])
        return code, json.loads(out.getvalue()), run_mock, err.getvalue()

    @staticmethod
    def passing_run(command, **kwargs):
        return subprocess.CompletedProcess(command, 0, stdout=b"ran", stderr=b"")

    def test_success_run_is_ok_with_per_script_identity_records(self):
        names = ("a.py", "b.py", "c.py")
        code, summary, run_mock, _ = self.run_main_with_fake_tree(
            names, self.passing_run
        )
        self.assertEqual(code, 0)
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["counts"]["selected"], 3)
        self.assertEqual(summary["counts"]["attempted"], 3)
        self.assertEqual(summary["counts"]["executed"], 3)
        self.assertEqual(summary["counts"]["passed"], 3)
        self.assertEqual(summary["counts"]["failed"], 0)
        self.assertEqual(summary["counts"]["timed_out"], 0)
        self.assertEqual(summary["counts"]["launch_errors"], 0)
        self.assertEqual(summary["counts"]["identity_changes"], 0)
        self.assertEqual(run_mock.call_count, 3)
        for record in summary["tests"]:
            self.assertIsNotNone(record["size_before"])
            self.assertIsNotNone(record["sha256_before"])
            self.assertEqual(record["size_before"], record["size_after"])
            self.assertEqual(record["sha256_before"], record["sha256_after"])
            self.assertFalse(record["identity_changed"])
            self.assertEqual(record["returncode"], 0)
            self.assertFalse(record["timed_out"])
            self.assertIsNone(record["launch_error"])
            self.assertEqual(record["stdout"], "ran")

    def test_invocation_contract_python_absolute_path_no_shell_pinned_cwd(self):
        names = ("a.py", "b.py")
        code, summary, run_mock, _ = self.run_main_with_fake_tree(
            names, self.passing_run
        )
        self.assertEqual(code, 0)
        self.assertEqual(run_mock.call_count, 2)
        for call in run_mock.call_args_list:
            args, kwargs = call
            command = args[0]
            self.assertIsInstance(command, list)
            self.assertEqual(command[0], sys.executable)
            self.assertEqual(command[1], "-B")
            self.assertTrue(Path(command[2]).is_absolute())
            self.assertEqual(kwargs.get("cwd"), str(runner.ROOT))
            self.assertIs(kwargs.get("shell"), False)
            self.assertEqual(kwargs.get("timeout"), 120)
            self.assertEqual(kwargs.get("timeout"), runner.SCRIPT_TIMEOUT_SECONDS)
            self.assertTrue(kwargs.get("capture_output"))
            if sys.platform == "win32":
                flags = kwargs.get("creationflags", 0)
                self.assertEqual(flags, runner.CREATION_FLAGS)
                self.assertNotEqual(flags & 0x08000000, 0)  # CREATE_NO_WINDOW
                self.assertNotEqual(flags & 0x00004000, 0)  # BELOW_NORMAL_PRIORITY
            else:
                self.assertEqual(kwargs.get("creationflags", 0), 0)

    def test_scripts_run_serially_in_whitelist_order(self):
        names = ("s1.py", "s2.py", "s3.py", "s4.py")
        state = {"active": 0}
        order = []

        def serial_run(command, **kwargs):
            self.assertEqual(state["active"], 0, "concurrent launch detected")
            state["active"] += 1
            try:
                order.append(Path(command[2]).name)
                return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")
            finally:
                state["active"] -= 1

        code, summary, run_mock, _ = self.run_main_with_fake_tree(names, serial_run)
        self.assertEqual(code, 0)
        self.assertEqual(order, list(names))
        self.assertEqual(run_mock.call_count, 4)

    def test_failure_continues_with_later_scripts_and_summary_is_failure(self):
        names = ("f1.py", "f2.py", "f3.py")

        def fake_run(command, **kwargs):
            returncode = 1 if Path(command[2]).name == "f1.py" else 0
            return subprocess.CompletedProcess(
                command, returncode, stdout=b"out", stderr=b"err"
            )

        code, summary, run_mock, _ = self.run_main_with_fake_tree(names, fake_run)
        self.assertNotEqual(code, 0)
        self.assertFalse(summary["ok"])
        self.assertEqual(run_mock.call_count, 3)
        self.assertEqual(summary["counts"]["executed"], 3)
        self.assertEqual(summary["counts"]["passed"], 2)
        self.assertEqual(summary["counts"]["failed"], 1)
        by_name = {Path(t["path"]).name: t for t in summary["tests"]}
        self.assertEqual(by_name["f1.py"]["returncode"], 1)
        self.assertEqual(by_name["f2.py"]["returncode"], 0)
        self.assertEqual(by_name["f3.py"]["returncode"], 0)

    def test_timeout_is_recorded_and_later_scripts_still_run(self):
        names = ("t1.py", "t2.py", "t3.py")

        def fake_run(command, **kwargs):
            if Path(command[2]).name == "t2.py":
                raise subprocess.TimeoutExpired(
                    command,
                    runner.SCRIPT_TIMEOUT_SECONDS,
                    output=b"partial-out",
                    stderr=b"partial-err",
                )
            return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")

        code, summary, run_mock, _ = self.run_main_with_fake_tree(names, fake_run)
        self.assertNotEqual(code, 0)
        self.assertFalse(summary["ok"])
        self.assertEqual(run_mock.call_count, 3)
        self.assertEqual(summary["counts"]["timed_out"], 1)
        self.assertEqual(summary["counts"]["passed"], 2)
        by_name = {Path(t["path"]).name: t for t in summary["tests"]}
        self.assertTrue(by_name["t2.py"]["timed_out"])
        self.assertIsNone(by_name["t2.py"]["returncode"])
        self.assertEqual(by_name["t2.py"]["stdout"], "partial-out")
        self.assertEqual(by_name["t2.py"]["stderr"], "partial-err")
        self.assertEqual(by_name["t3.py"]["returncode"], 0)

    def test_launch_exception_is_recorded_and_later_scripts_still_run(self):
        names = ("l1.py", "l2.py")

        def fake_run(command, **kwargs):
            if Path(command[2]).name == "l1.py":
                raise OSError("cannot spawn")
            return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")

        code, summary, run_mock, _ = self.run_main_with_fake_tree(names, fake_run)
        self.assertNotEqual(code, 0)
        self.assertFalse(summary["ok"])
        self.assertEqual(run_mock.call_count, 2)
        self.assertEqual(summary["counts"]["launch_errors"], 1)
        self.assertEqual(summary["counts"]["executed"], 1)
        self.assertEqual(summary["counts"]["passed"], 1)
        by_name = {Path(t["path"]).name: t for t in summary["tests"]}
        self.assertIn("OSError", by_name["l1.py"]["launch_error"])
        self.assertTrue(by_name["l1.py"]["attempted"])
        self.assertFalse(by_name["l1.py"]["executed"])
        self.assertEqual(by_name["l2.py"]["returncode"], 0)

    def test_source_identity_change_detected_even_with_zero_returncode(self):
        names = ("m1.py", "m2.py")

        def fake_run(command, **kwargs):
            if Path(command[2]).name == "m1.py":
                Path(command[2]).write_bytes(b"# mutated while running\n")
            return subprocess.CompletedProcess(command, 0, stdout=b"", stderr=b"")

        code, summary, run_mock, _ = self.run_main_with_fake_tree(names, fake_run)
        self.assertNotEqual(code, 0)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["counts"]["identity_changes"], 1)
        self.assertEqual(summary["counts"]["passed"], 1)
        by_name = {Path(t["path"]).name: t for t in summary["tests"]}
        self.assertEqual(by_name["m1.py"]["returncode"], 0)
        self.assertTrue(by_name["m1.py"]["identity_changed"])
        self.assertNotEqual(
            by_name["m1.py"]["sha256_before"], by_name["m1.py"]["sha256_after"]
        )
        self.assertFalse(by_name["m2.py"]["identity_changed"])

    def test_stream_clipping_64kib_with_explicit_truncation_flag(self):
        # 64 KiB only caps what is recorded in the summary; it is not a
        # subprocess memory limit or process-level memory protection.
        def fake_run(command, **kwargs):
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=b"A" * (64 * 1024 + 1000),
                stderr=b"B" * (64 * 1024 + 1),
            )

        code, summary, _, _ = self.run_main_with_fake_tree(("big.py",), fake_run)
        self.assertEqual(code, 0)
        self.assertTrue(summary["ok"])
        record = summary["tests"][0]
        self.assertTrue(record["stdout_truncated"])
        self.assertTrue(record["stderr_truncated"])
        self.assertEqual(record["stdout"], "A" * 65536)
        self.assertEqual(record["stderr"], "B" * 65536)
        self.assertEqual(record["stdout_bytes"], 65536 + 1000)
        self.assertEqual(record["stderr_bytes"], 65536 + 1)

    def test_stream_exactly_at_limit_is_not_truncated(self):
        def fake_run(command, **kwargs):
            return subprocess.CompletedProcess(
                command, 0, stdout=b"C" * 65536, stderr=b""
            )

        code, summary, _, _ = self.run_main_with_fake_tree(("edge.py",), fake_run)
        self.assertEqual(code, 0)
        record = summary["tests"][0]
        self.assertFalse(record["stdout_truncated"])
        self.assertEqual(record["stdout"], "C" * 65536)
        self.assertEqual(record["stdout_bytes"], 65536)

    def test_summary_declares_schema_scope_and_not_run_claims(self):
        code, summary, _, _ = self.run_main_with_fake_tree(
            ("a.py",), self.passing_run
        )
        self.assertEqual(code, 0)
        self.assertEqual(summary["schema"], 1)
        self.assertEqual(summary["scope"], "TARGETED_PYTHON_STATIC_AND_SYNTHETIC_ONLY")
        self.assertEqual(summary["counts_unit"], "scripts")
        for token in ("native", "gpu", "game", "dll_build"):
            self.assertIn(token, summary["not_run"])
        self.assertIn("indirect", summary["coverage_note"])
        self.assertEqual(summary["limits"]["timeout_seconds_per_script"], 120)
        self.assertIs(summary["limits"]["shell"], False)
        self.assertIs(summary["limits"]["parallel"], False)
        self.assertIn("not a hard memory limit", summary["limits"]["limits_note"])
        self.assertFalse(summary["output"]["requested"])
        self.assertIsNone(summary["output"]["error"])
        self.assertEqual(
            summary["runner"]["entry_path"],
            "AutoTest/run_architecture_python_checks.py",
        )
        self.assertIsNotNone(summary["runner"]["entry_sha256"])
        self.assertIsNotNone(summary["runner"]["entry_size"])


if __name__ == "__main__":
    unittest.main()