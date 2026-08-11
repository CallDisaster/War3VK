import os
import pathlib
import sys
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUTOTEST = ROOT / "AutoTest"
sys.path.insert(0, str(AUTOTEST))

import war3_autotest_mcp as autotest  # noqa: E402


class IsolatedDesktopNonInteractiveTests(unittest.TestCase):
    def test_owned_scenario_d3d9_backup_restore_is_hash_exact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            war3_dir = root / "war3"
            artifact_dir = root / "artifacts"
            war3_dir.mkdir()
            target = war3_dir / "d3d9.dll"
            original = b"user-release-dll\x00\x01"
            candidate = b"autotest-candidate-dll\x02\x03"
            target.write_bytes(original)

            backup = autotest._backup_scenario_d3d9(
                war3_dir, artifact_dir
            )
            self.assertTrue(backup.get("ok"), backup)
            self.assertTrue(backup.get("originalExisted"), backup)
            self.assertEqual(
                backup.get("sha256"), autotest.sha256_file(target)
            )

            target.write_bytes(candidate)
            restore = autotest._restore_scenario_d3d9(
                backup, str(target)
            )
            self.assertTrue(restore.get("ok"), restore)
            self.assertEqual(target.read_bytes(), original)
            self.assertEqual(
                restore.get("sha256"), backup.get("sha256")
            )

    def test_owned_scenario_restore_wraps_launch_and_result(self) -> None:
        runner = (AUTOTEST / "war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        body = runner.split(
            "def run_life_and_death_tdr_scenario", 1
        )[1].split("def run_cross_map_shadow_scenario", 1)[0]
        backup_index = body.index("_backup_scenario_d3d9(")
        launch_index = body.index("_launch_suite_map_until_ready(")
        stop_index = body.index("stop_war3(", launch_index)
        restore_index = body.index("_restore_scenario_d3d9(", stop_index)
        self.assertLess(backup_index, launch_index)
        self.assertLess(stop_index, restore_index)
        self.assertIn("owned War3 process still alive; DLL restore refused", body)
        self.assertIn("and restore_ok", body)

    def test_strict_stability_hook_classifier_is_bounded_and_case_insensitive(
        self,
    ) -> None:
        modules = [
            {"name": "War3.exe", "path": r"C:\Game\War3.exe", "size": 1},
            {
                "name": "GRAPHICS-HOOK32.DLL",
                "path": r"C:\ProgramData\obs-studio-hook\graphics-hook32.dll",
                "size": 2,
            },
            {
                "name": "ReShade32.dll",
                "path": r"C:\Game\ReShade32.dll",
                "size": 3,
            },
            {"name": "nvoglv32.dll", "path": r"C:\Windows\nvoglv32.dll", "size": 4},
        ]
        evidence = autotest._external_graphics_hook_evidence(modules)
        self.assertEqual(
            [row["name"] for row in evidence],
            ["GRAPHICS-HOOK32.DLL", "ReShade32.dll"],
        )
        self.assertTrue(
            all(len(row["sha256"]) in (0, 64) for row in evidence)
        )

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

    def test_isolated_input_is_hwnd_scoped_without_global_helper(self) -> None:
        runner = (AUTOTEST / "war3_autotest_mcp.py").read_text(
            encoding="utf-8"
        )
        key_body = runner.split("def _post_war3_key_pulse", 1)[1].split(
            "def _run_war3_input_plan", 1
        )[0]
        isolated_branch = key_body.split(
            'if desktop_mode == "isolated":', 1
        )[1].split("return result", 1)[0]
        self.assertIn("_post_war3_virtual_key_message", isolated_branch)
        self.assertNotIn("_launch_process_on_desktop", isolated_branch)
        self.assertNotIn("SetForegroundWindow", isolated_branch)

        plan_body = runner.split("def _run_war3_input_plan", 1)[1].split(
            "def _wait_for_window_ready", 1
        )[0]
        self.assertIn('if desktop_mode == "isolated":', plan_body)
        self.assertIn("_post_war3_virtual_key_message", plan_body)
        self.assertIn("ISOLATED_DESKTOP_INPUT_UNSUPPORTED", plan_body)
        self.assertIn("不允许鼠标注入", plan_body)
        self.assertIn('mode": "isolated-window-message-plan"', plan_body)

        message_body = runner.split(
            "def _post_war3_virtual_key_message", 1
        )[1].split("def _post_war3_key_pulse", 1)[0]
        self.assertIn("PostMessageW", message_body)
        self.assertIn("_query_input_desktop_name", message_body)
        self.assertIn('"foregroundChanged": False', message_body)
        for forbidden in (
            "SwitchDesktop",
            "SetForegroundWindow",
            "keybd_event",
            "mouse_event",
            "SendInput",
        ):
            self.assertNotIn(forbidden, message_body)

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
