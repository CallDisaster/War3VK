"""Pure reader/argument tests: no Desktop, native API, process, window or GPU."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from isolated_render_desktop import (DesktopSafetyError, IsolatedRenderDesktop,
                                     _launch_arguments, validate_probe_receipt)


def sample(bits=32, pid=101, tid=301):
    main = dict(threadId=tid, station="WinSta0", desktop="WarVK_Test_0123",
                inputBefore="Default", inputAfter="Default")
    pump = dict(main, threadId=tid + 1, windowCreated=True, windowHidden=True,
                windowDestroyed=True, classUnregistered=True, width=64, height=64,
                pings=1, messagesDispatched=2)
    return dict(schema=1, scope="NON_INPUT_DESKTOP_PROBE", bits=bits, ok=True,
                pid=pid, creation100ns=10001, main=main, pump=pump,
                childRequested=False, childDesktopExplicit=False,
                childPid=0, childCreation=0, childExit=0, gpuModulesAbsent=True,
                gameTouched=False, globalInputUsed=False, desktopSwitchUsed=False,
                normalTeardown=True)


def validate(row, child=None):
    validate_probe_receipt(row, desktop="WarVK_Test_0123", input_desktop="Default",
                           station="WinSta0", bits=32, child=child)


class DesktopReaderTests(unittest.TestCase):
    def test_valid_main_and_independent_pump(self):
        validate(sample())

    def test_valid_explicit_cross_bitness_child(self):
        row, child = sample(), sample(64, 102, 401)
        row.update(childRequested=True, childDesktopExplicit=True,
                   childPid=child["pid"], childCreation=child["creation100ns"])
        validate(row, child)

    def test_probe_must_match_exact_launch_authority(self):
        row = sample()
        launch = dict(ok=True, processSettled=True, jobEmpty=True, jobAtomic=True,
                      handleListApplied=True, timedOut=False, forcedTermination=False,
                      pid=row["pid"], creation100ns=row["creation100ns"],
                      desktop="WarVK_Test_0123", inputBefore="Default", inputAfter="Default")
        kwargs = dict(desktop="WarVK_Test_0123", input_desktop="Default", station="WinSta0", bits=32)
        validate_probe_receipt(row, launch=launch, **kwargs)
        for key, value in (("pid", 102), ("pid", True), ("creation100ns", 10002),
                           ("jobAtomic", False), ("processSettled", False),
                           ("timedOut", True), ("forcedTermination", True),
                           ("inputAfter", "Other"), ("desktop", "Default")):
            wrong = dict(launch)
            wrong[key] = value
            with self.subTest(key=key), self.assertRaises(DesktopSafetyError):
                validate_probe_receipt(row, launch=wrong, **kwargs)

    def test_missing_extra_fields_fail_closed(self):
        for role in (None, "main", "pump"):
            for extra in (False, True):
                row = sample()
                target = row if role is None else row[role]
                if extra:
                    target["notInContract"] = 1
                else:
                    target.pop(next(iter(target)))
                with self.subTest(role=role, extra=extra), self.assertRaises(DesktopSafetyError):
                    validate(row)

    def test_no_bool_or_float_as_identity(self):
        for key in ("schema", "bits", "pid", "creation100ns"):
            for value in (True, 0, -1, 1.0):
                row = sample()
                row[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(DesktopSafetyError):
                    validate(row)

    def test_identity_ranges(self):
        for key, value in (("pid", 1 << 32), ("creation100ns", 1 << 64)):
            row = sample()
            row[key] = value
            with self.subTest(key=key), self.assertRaises(DesktopSafetyError):
                validate(row)

    def test_real_desktop_names_required_not_metadata(self):
        for role in ("main", "pump"):
            for key in ("station", "desktop", "inputBefore", "inputAfter"):
                row = sample()
                row[role][key] = "Other"
                with self.subTest(role=role, key=key), self.assertRaises(DesktopSafetyError):
                    validate(row)
        row = sample()
        row["pump"]["threadId"] = row["main"]["threadId"]
        with self.assertRaises(DesktopSafetyError):
            validate(row)

    def test_input_desktop_cannot_be_target(self):
        with self.assertRaises(DesktopSafetyError):
            validate_probe_receipt(sample(), desktop="default", input_desktop="Default",
                                   station="WinSta0", bits=32)

    def test_visibility_teardown_and_side_effect_flags(self):
        for key in ("ok", "normalTeardown", "gpuModulesAbsent", "gameTouched",
                    "globalInputUsed", "desktopSwitchUsed"):
            row = sample()
            row[key] = not row[key]
            with self.subTest(key=key), self.assertRaises(DesktopSafetyError):
                validate(row)
        for key in ("windowCreated", "windowHidden", "windowDestroyed", "classUnregistered"):
            row = sample()
            row["pump"][key] = False
            with self.subTest(key=key), self.assertRaises(DesktopSafetyError):
                validate(row)

    def test_pump_counter_contract(self):
        for key in ("threadId", "width", "height", "messagesDispatched", "pings"):
            for value in (True, 0, 1.0):
                row = sample()
                row["pump"][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(DesktopSafetyError):
                    validate(row)

    def test_child_must_have_own_full_matching_witness(self):
        good, child = sample(), sample(64, 102, 401)
        good.update(childRequested=True, childDesktopExplicit=True,
                    childPid=102, childCreation=10001)
        with self.assertRaises(DesktopSafetyError):
            validate(good)
        for key, value in (("childRequested", False), ("childDesktopExplicit", False),
                           ("childPid", 103), ("childPid", True),
                           ("childCreation", 10002), ("childExit", 2), ("childExit", False)):
            row = copy.deepcopy(good)
            row[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(DesktopSafetyError):
                validate(row, child)
        bad_child = copy.deepcopy(child)
        bad_child["pump"]["desktop"] = "Default"
        with self.assertRaises(DesktopSafetyError):
            validate(good, bad_child)

    def test_context_construction_has_no_native_side_effect(self):
        desktop = IsolatedRenderDesktop("PureTest")
        self.assertEqual(desktop._desktop, 0)
        self.assertIsNone(desktop._api)
        self.assertEqual(desktop.receipt["launches"], [])
        self.assertFalse(desktop.receipt["ok"])
        for prefix in ("", "Default\\other", "spaces here", "x" * 65):
            with self.subTest(prefix=prefix), self.assertRaises(DesktopSafetyError):
                IsolatedRenderDesktop(prefix)

    def test_argv_rejects_invalid_timeouts_without_launch(self):
        executable = Path(sys.executable).resolve()
        directory = Path(__file__).resolve().parent
        for timeout in (True, 0, -1, float("inf"), float("nan"), 301):
            with self.subTest(timeout=timeout), self.assertRaises(DesktopSafetyError):
                _launch_arguments([str(executable)], directory, timeout)
        args, cwd, timeout = _launch_arguments([str(executable), "space arg"], directory, 30)
        self.assertEqual(args[1], "space arg")
        self.assertEqual(cwd, directory)
        self.assertEqual(timeout, 30)


if __name__ == "__main__":
    unittest.main()
