"""Pure validation of the CPU gate's receipts; no subprocess/build/game calls."""
import json
import unittest

import run_storm_low_domain_gate as gate


def output(*, native=True, disabled=False, injected=False, option="plain", repair="off"):
    result = dict(checks=100, failures=0, native=native, ok=True)
    details = dict(enabled=0 if disabled else 1, scopes=0 if disabled else 10,
                   reserves=0 if disabled else 3, highRejected=1 if injected else 0,
                   searchQueries=3 if injected else 0, failures=0,
                   preexisting=0 if disabled else 1, creates=0 if disabled else 2,
                   createdLow=0 if disabled else 2, injections=1 if injected else 0,
                   option=option, repair=repair)
    zero_rows = []
    if native:
        for phase in ("unhooked-reference", "disabled-target" if disabled else "hooked-target"):
            for old_size in (4096, 0x20010):
                external = option == "protect" or old_size > 0xFE7B
                for flags in (0x10, 0):
                    refused = external and flags == 0x10
                    zero_rows.append("ZERO_REALLOC " + json.dumps(dict(phase=phase, oldSize=old_size,
                        flags=flags, external=external, resultNull=refused, samePointer=not external,
                        liveSize=old_size if refused else 0)) + "\n")
    return "".join(zero_rows) + ("NATIVE " + json.dumps(details) + "\n" if native else "") + "RESULT " + json.dumps(result) + "\n"


class GateReceipts(unittest.TestCase):
    def test_targets_are_cpu_leafs(self):
        self.assertEqual(gate.native_target(gate.DEFAULT_NATIVE_TARGET), gate.DEFAULT_NATIVE_TARGET)
        for value in ("src/d3d9/d3d9.dll", "all", "../test.exe", "src/d3d9/warvk-x-test.exe other",
                      "src/d3d9/warvk-x-test.exe;echo", "src/d3d9/warvk-x-test.exe/../foo"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                gate.native_target(value)

    def test_runtime_exact_identity_and_mode(self):
        text = "startup runtime checks=12 failures=0 pid=10 creation100ns=99 mode=--emit\n"
        self.assertEqual(gate.verify_runtime_stdout(text, {"pid": 10, "creation100ns": 99}, "--emit")["checks"], 12)
        for launch, mode in (({"pid": 11, "creation100ns": 99}, "--emit"),
                             ({"pid": 10, "creation100ns": 100}, "--emit"),
                             ({"pid": 10, "creation100ns": 99}, "--off")):
            with self.subTest(launch=launch, mode=mode), self.assertRaises(ValueError):
                gate.verify_runtime_stdout(text, launch, mode)
        for bad in (text.replace("failures=0", "failures=1"), text.replace("checks=12", "checks=0"),
                    text + text, text + "unexpected"):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.verify_runtime_stdout(bad, {"pid": 10, "creation100ns": 99}, "--emit")

    def test_native_matrix_exact_modes(self):
        for label, repair, option, injected, disabled in gate.NATIVE_MATRIX:
            with self.subTest(label=label):
                parsed = gate.verify_native_stdout(output(repair=repair, option=option,
                            injected=injected, disabled=disabled), native=True,
                            repair=repair, option=option, injected=injected, disabled=disabled)
                self.assertTrue(parsed["RESULT"]["ok"])

    def test_core_is_not_native(self):
        self.assertFalse(gate.verify_native_stdout(output(native=False), native=False)["RESULT"]["native"])
        with self.assertRaises(ValueError):
            gate.verify_native_stdout(output(native=False), native=True)
        with self.assertRaises(ValueError):
            gate.verify_native_stdout(output(), native=False)

    def test_native_modes_cannot_be_substituted(self):
        for opts in ({"repair": "search"}, {"option": "protect"}, {"disabled": True}, {"injected": True}):
            with self.subTest(opts=opts), self.assertRaises(ValueError):
                gate.verify_native_stdout(output(), native=True, **opts)

    def test_missing_checks_failures_or_nonboolean_verdict(self):
        text = output()
        for bad in (text.replace('"checks": 100', '"checks": 0'),
                    text.replace('"failures": 0', '"failures": 1'),
                    text.replace('"ok": true', '"ok": 1'),
                    text.replace('"native": true', '"native": 1')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True)

    def test_no_native_work_is_not_pass(self):
        text = output()
        for name, old in (("scopes", 10), ("reserves", 3), ("creates", 2), ("createdLow", 2)):
            bad = text.replace(f'"{name}": {old}', f'"{name}": 0')
            with self.subTest(name=name), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True)

    def test_disabled_path_cannot_hide_work(self):
        text = output(disabled=True)
        for name in ("scopes", "reserves", "creates", "createdLow"):
            bad = text.replace(f'"{name}": 0', f'"{name}": 1')
            with self.subTest(name=name), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True, disabled=True)

    def test_injection_requires_actual_rejection_search(self):
        text = output(injected=True)
        for name, old in (("injections", 1), ("highRejected", 1), ("searchQueries", 3)):
            bad = text.replace(f'"{name}": {old}', f'"{name}": 0')
            with self.subTest(name=name), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True, injected=True)

    def test_unknown_duplicate_and_malformed_json(self):
        text = output()
        for bad in ("garbage\n" + text, text + text, "NATIVE {}\n" + text,
                    text.replace('"checks": 100', '"checks": 100, "checks": 1'),
                    text.replace('"checks": 100', '"checks": NaN')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True)

    def test_zero_realloc_needs_exact_reference_and_live_ownership(self):
        text = output()
        lines = text.splitlines(keepends=True)
        for bad in ("".join(lines[1:]), lines[0] + text,
                    text.replace('"samePointer": true', '"samePointer": false', 1),
                    text.replace('"liveSize": 0', '"liveSize": 4096', 1),
                    text.replace('"resultNull": false', '"resultNull": 0', 1),
                    text.replace('unhooked-reference', 'hooked-target', 1)):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                gate.verify_native_stdout(bad, native=True)


if __name__ == "__main__":
    unittest.main()
