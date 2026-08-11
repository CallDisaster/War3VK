import os
import pathlib
import sys
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUTOTEST = ROOT / "AutoTest"
sys.path.insert(0, str(AUTOTEST))

import war3_autotest_mcp as autotest  # noqa: E402


class IsolatedDesktopNonInteractiveTests(unittest.TestCase):
    def test_active_autotest_has_no_desktop_switch_capability(self) -> None:
        runner = (AUTOTEST / "war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        input_plan = (AUTOTEST / "send_input_plan_same_desktop.ps1").read_text(
            encoding="utf-8"
        )
        input_key = (AUTOTEST / "send_key_same_desktop.ps1").read_text(
            encoding="utf-8"
        )
        combined = "\n".join((runner, input_plan, input_key))
        self.assertNotIn("SwitchDesktop", combined)
        self.assertNotIn("DESKTOP_SWITCHDESKTOP", runner)
        self.assertIn("ISOLATED_DESKTOP_NONINTERACTIVE_ONLY = True", runner)

        mask = runner.split("def _desktop_access_mask", 1)[1].split(
            "def _desktop_name_from_handle", 1
        )[0]
        self.assertNotIn("0x0100", mask)

    def test_isolated_input_paths_fail_before_window_or_helper_work(self) -> None:
        runner = (AUTOTEST / "war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        key_body = runner.split("def _post_war3_key_pulse", 1)[1].split(
            "def _run_war3_input_plan", 1
        )[0]
        key_gate = key_body.index("ISOLATED_DESKTOP_INPUT_UNSUPPORTED")
        self.assertLess(key_gate, key_body.index("_wait_for_main_window_hwnd"))
        self.assertLess(key_gate, key_body.index("_launch_process_on_desktop"))

        plan_body = runner.split("def _run_war3_input_plan", 1)[1].split(
            "def _wait_for_window_ready", 1
        )[0]
        plan_gate = plan_body.index("ISOLATED_DESKTOP_INPUT_UNSUPPORTED")
        self.assertLess(plan_gate, plan_body.index("_wait_for_main_window_hwnd"))
        self.assertLess(plan_gate, plan_body.index("_launch_process_on_desktop"))

        input_plan = (AUTOTEST / "send_input_plan_same_desktop.ps1").read_text(
            encoding="utf-8"
        ).split(
            "[void][War3DesktopInputPlan]::SetProcessDPIAware()", 1
        )[1]
        plan_proof = input_plan.index(
            "non-input desktop injection is forbidden"
        )
        self.assertLess(plan_proof, input_plan.index("::PulseKey("))
        self.assertLess(plan_proof, input_plan.index("::ClickClient("))

        key_pulse = (AUTOTEST / "send_key_same_desktop.ps1").read_text(
            encoding="utf-8"
        ).split("public static string Pulse", 1)[1]
        key_proof = key_pulse.index(
            "non-input desktop injection is forbidden"
        )
        self.assertLess(key_proof, key_pulse.index("keybd_event("))

    @unittest.skipUnless(os.name == "nt", "Win32 Desktop probe")
    def test_process_launch_does_not_change_input_desktop(self) -> None:
        input_before = autotest._query_input_desktop_name()
        self.assertTrue(input_before)
        desktop_name = (
            f"War3AutoTestSafety_{os.getpid()}_{time.time_ns()}"
        )
        desktop = autotest._create_isolated_desktop(desktop_name)
        self.assertTrue(desktop.get("ok"), desktop)
        self.assertEqual(desktop.get("inputDesktopBefore"), input_before)
        self.assertEqual(desktop.get("inputDesktopAfter"), input_before)
        self.assertTrue(desktop.get("nonInteractiveOnly"))

        witness = None
        launch = {}
        try:
            system_root = pathlib.Path(os.environ.get("SystemRoot", r"C:\Windows"))
            cmd = system_root / "System32" / "cmd.exe"
            launch = autotest._launch_process_on_desktop(
                [str(cmd), "/d", "/c", "exit", "0"],
                AUTOTEST,
                os.environ.copy(),
                desktop_name,
            )
            self.assertTrue(launch.get("ok"), launch)
            self.assertEqual(launch.get("inputDesktopBefore"), input_before)
            self.assertEqual(launch.get("inputDesktopAfter"), input_before)
            self.assertTrue(launch.get("nonInteractiveOnly"))
            witness = launch.pop("_nativeProcessWitness", None)
            self.assertIsNotNone(witness)
            self.assertEqual(witness.wait(timeout=5.0), 0)
        finally:
            if witness is not None:
                witness.close()
            self.assertTrue(
                autotest._close_desktop_handle(
                    int(desktop.get("handle", 0) or 0)
                )
            )

        self.assertEqual(autotest._query_input_desktop_name(), input_before)


if __name__ == "__main__":
    unittest.main()
