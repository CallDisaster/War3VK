#!/usr/bin/env python
# -*- coding: utf-8 -*-

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import war3_autotest_mcp as autotest


class YdweLaunchContractTests(unittest.TestCase):
    def _make_runtime_roots(self, base: Path) -> tuple[Path, Path]:
        war3 = base / "War3_AutoTestSandbox"
        ydwe = base / "YDWE1.32.13 - MemoryHack"
        war3.mkdir(parents=True)
        ydwe.mkdir(parents=True)
        (ydwe / "bin").mkdir()
        (ydwe / "logs").mkdir()
        (ydwe / "plugin" / "warcraft3").mkdir(parents=True)
        for name in ("war3.exe", "Game.dll", "war3.mpq", "war3patch.mpq", "Storm.dll"):
            (war3 / name).write_bytes(name.encode("ascii"))
        (ydwe / "YDWE.exe").write_bytes(b"ydwe")
        (ydwe / "bin" / "LuaEngine.dll").write_bytes(b"lua")
        (ydwe / "logs" / "war3.log").write_bytes(b"")
        (ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll").write_bytes(b"japi")
        return war3, ydwe

    def tearDown(self) -> None:
        if int(autotest.STATE.ydwe_lock_handle or 0) != 0:
            autotest._release_ydwe_root_lock(
                {
                    "handle": int(autotest.STATE.ydwe_lock_handle),
                    "name": str(autotest.STATE.ydwe_lock_name),
                    "ydweRoot": str(autotest.STATE.ydwe_lock_root),
                }
            )
        autotest.STATE.war3_proc = None
        autotest.STATE.war3_pid = None
        autotest.STATE.launcher_pid = None
        autotest.STATE.launcher_created_at_ms = 0
        autotest.STATE.launcher_mode = autotest.YDWE_LAUNCHER_MODE_DIRECT
        autotest.STATE.launcher_exe = ""
        autotest.STATE.ydwe_lock_handle = 0
        autotest.STATE.ydwe_lock_name = ""
        autotest.STATE.ydwe_lock_root = ""
        autotest.STATE.desktop_name = ""
        autotest.STATE.desktop_handle = 0
        autotest.STATE.desktop_mode = "default"
        autotest.STATE.video_restore_snapshot = {}

    def test_launcher_mode_aliases_and_unknown_value(self) -> None:
        self.assertEqual(autotest._normalize_launcher_mode("war3.exe"), "direct")
        self.assertEqual(autotest._normalize_launcher_mode("YDWE.EXE"), "ydwe")
        with self.assertRaises(ValueError):
            autotest._normalize_launcher_mode("guess")

    def test_preflight_requires_exact_hkcu_install_path_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            war3, ydwe = self._make_runtime_roots(Path(temp))
            with (
                mock.patch.object(autotest, "_require_multi_instance_sandbox", return_value=war3),
                mock.patch.object(autotest, "_find_user_ydwe_process_conflicts", return_value=[]),
                mock.patch.object(
                    autotest,
                    "_read_hkcu_war3_install_path",
                    return_value={"ok": True, "path": str(war3.parent / "wrong")},
                ),
                mock.patch.object(autotest.winreg, "SetValueEx") as registry_write,
            ):
                result = autotest._preflight_ydwe_single_launch(war3, ydwe)
            self.assertFalse(result["ok"])
            self.assertEqual(result["code"], "YDWE_INSTALL_PATH_MISMATCH")
            registry_write.assert_not_called()

    def test_preflight_accepts_complete_ydwe_japi_chain(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            war3, ydwe = self._make_runtime_roots(Path(temp))
            with (
                mock.patch.object(autotest, "_require_multi_instance_sandbox", return_value=war3),
                mock.patch.object(autotest, "_find_user_ydwe_process_conflicts", return_value=[]),
                mock.patch.object(
                    autotest,
                    "_read_hkcu_war3_install_path",
                    return_value={"ok": True, "path": str(war3), "keyPath": "test"},
                ),
            ):
                result = autotest._preflight_ydwe_single_launch(war3, ydwe)
            self.assertTrue(result["ok"], result)
            self.assertFalse(result["registryModified"])
            self.assertEqual(Path(result["luaEngine"]), ydwe / "bin" / "LuaEngine.dll")
            self.assertEqual(Path(result["ydJassApi"]), ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll")

    def test_preflight_fails_closed_when_user_editor_is_running(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            war3, ydwe = self._make_runtime_roots(Path(temp))
            with (
                mock.patch.object(autotest, "_require_multi_instance_sandbox", return_value=war3),
                mock.patch.object(
                    autotest,
                    "_find_user_ydwe_process_conflicts",
                    return_value=[{"pid": 32268, "exeName": "worldeditydwe.exe"}],
                ),
            ):
                result = autotest._preflight_ydwe_single_launch(war3, ydwe)
            self.assertFalse(result["ok"])
            self.assertEqual(result["code"], "USER_YDWE_PROCESS_CONFLICT")
            self.assertEqual(result["conflicts"][0]["pid"], 32268)

    def test_preflight_fails_before_launch_when_war3_log_is_not_writable(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            war3, ydwe = self._make_runtime_roots(Path(temp))
            denied = {
                "ok": False,
                "code": "YDWE_LOG_NOT_WRITABLE",
                "error": "locked",
                "win32Error": 32,
            }
            with (
                mock.patch.object(autotest, "_require_multi_instance_sandbox", return_value=war3),
                mock.patch.object(autotest, "_probe_ydwe_log_writable", return_value=denied),
            ):
                result = autotest._preflight_ydwe_single_launch(war3, ydwe)
            self.assertFalse(result["ok"])
            self.assertEqual(result["code"], "YDWE_LOG_NOT_WRITABLE")

    def test_ydwe_root_lock_rejects_concurrent_owner(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            ydwe = Path(temp) / "YDWE"
            ydwe.mkdir()
            first = autotest._acquire_ydwe_root_lock(ydwe)
            self.assertTrue(first["ok"], first)
            try:
                second = autotest._acquire_ydwe_root_lock(ydwe)
                self.assertFalse(second["ok"], second)
                self.assertEqual(second["code"], "YDWE_ROOT_BUSY")
            finally:
                released = autotest._release_ydwe_root_lock(first)
                self.assertTrue(released["ok"], released)

    def test_child_discovery_requires_lineage_and_exact_sandbox_image(self) -> None:
        expected = Path(r"E:\Work\War3_AutoTestSandbox\war3.exe")
        rows = [
            {"pid": 900, "parentPid": 1, "exeName": "YDWE.exe"},
            {"pid": 901, "parentPid": 900, "exeName": "war3.exe"},
            {"pid": 902, "parentPid": 2, "exeName": "war3.exe"},
        ]
        with (
            mock.patch.object(autotest, "_snapshot_process_entries", return_value=rows),
            mock.patch.object(
                autotest,
                "_query_process_image_path",
                side_effect=lambda pid: str(expected) if pid == 901 else r"E:\Other\war3.exe",
            ),
            mock.patch.object(autotest, "_pid_alive", return_value=True),
        ):
            result = autotest._wait_for_ydwe_war3_child(
                launcher_pid=900,
                expected_war3_exe=expected,
                preexisting_pids=set(),
                timeout_sec=1,
            )
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["pid"], 901)
        self.assertEqual(result["discovery"], "toolhelp-descendant-and-exact-image")

    def test_child_discovery_keeps_short_lived_helper_lineage_across_snapshots(self) -> None:
        expected = Path(r"E:\Work\War3_AutoTestSandbox\war3.exe")
        snapshots = [
            [
                {"pid": 900, "parentPid": 1, "exeName": "YDWE.exe"},
                {"pid": 901, "parentPid": 900, "exeName": "helper.exe"},
            ],
            [
                {"pid": 902, "parentPid": 901, "exeName": "war3.exe"},
            ],
        ]
        with (
            mock.patch.object(autotest, "_snapshot_process_entries", side_effect=snapshots),
            mock.patch.object(autotest, "_query_process_image_path", return_value=str(expected)),
            mock.patch.object(autotest.time, "sleep"),
        ):
            result = autotest._wait_for_ydwe_war3_child(
                launcher_pid=900,
                expected_war3_exe=expected,
                preexisting_pids=set(),
                timeout_sec=1,
            )
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["pid"], 902)

    def test_loaded_japi_modules_require_exact_path_and_sha(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            _, ydwe = self._make_runtime_roots(Path(temp))
            lua = ydwe / "bin" / "LuaEngine.dll"
            japi = ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll"
            modules = [
                {"name": "LuaEngine.dll", "path": str(lua)},
                {"name": "yd_jass_api.dll", "path": str(japi)},
            ]
            expected_sha = {
                "LuaEngine.dll": autotest.sha256_file(lua),
                "yd_jass_api.dll": autotest.sha256_file(japi),
            }
            with (
                mock.patch.object(autotest, "_snapshot_process_modules", return_value=modules),
                mock.patch.object(autotest, "_pid_alive", return_value=True),
            ):
                result = autotest._wait_for_ydwe_runtime_modules(
                    pid=701,
                    expected_paths={"LuaEngine.dll": lua, "yd_jass_api.dll": japi},
                    expected_sha256=expected_sha,
                    timeout_sec=1,
                )
            self.assertTrue(result["ok"], result)
            self.assertTrue(all(row["shaMatches"] for row in result["modules"]))

    def test_locked_stale_world_edit_map_is_never_silently_reused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            war3 = base / "sandbox"
            source = base / "candidate.w3x"
            target = war3 / autotest.DEFAULT_TEST_MAP_REL
            target.parent.mkdir(parents=True)
            source.write_bytes(b"new-candidate")
            target.write_bytes(b"stale-map")
            with mock.patch.object(autotest.os, "replace", side_effect=PermissionError("locked")):
                with self.assertRaises(PermissionError):
                    autotest._prepare_test_map_copy(war3, source, autotest.DEFAULT_TEST_MAP_REL)
            self.assertEqual(target.read_bytes(), b"stale-map")

    def test_readonly_stale_world_edit_map_is_atomically_replaced(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            war3 = base / "sandbox"
            source = base / "candidate.w3x"
            target = war3 / autotest.DEFAULT_TEST_MAP_REL
            target.parent.mkdir(parents=True)
            source.write_bytes(b"new-candidate")
            target.write_bytes(b"stale-map")
            target.chmod(0o444)

            deployed = autotest._prepare_test_map_copy(
                war3, source, autotest.DEFAULT_TEST_MAP_REL
            )

            self.assertEqual(deployed, target)
            self.assertEqual(target.read_bytes(), b"new-candidate")
            self.assertEqual(autotest.sha256_file(target), autotest.sha256_file(source))

    def test_launch_uses_ydwe_wrapper_but_tracks_child_game_pid(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            war3, ydwe = self._make_runtime_roots(base)
            source_map = base / "input.w3x"
            source_map.write_bytes(b"map")
            captured = {}

            def fake_launch(args, cwd, env, desktop_name):
                captured["args"] = list(args)
                captured["cwd"] = Path(cwd)
                captured["desktop"] = desktop_name
                return {"ok": True, "pid": 700}

            preflight = {
                "ok": True,
                "war3Dir": str(war3),
                "ydweRoot": str(ydwe),
                "ydweExe": str(ydwe / "YDWE.exe"),
                "luaEngine": str(ydwe / "bin" / "LuaEngine.dll"),
                "ydJassApi": str(ydwe / "plugin" / "warcraft3" / "yd_jass_api.dll"),
                "runtimeSha256": {
                    "LuaEngine.dll": "lua-sha",
                    "yd_jass_api.dll": "japi-sha",
                },
                "registryModified": False,
            }
            with (
                mock.patch.object(autotest, "_preflight_ydwe_single_launch", return_value=preflight),
                mock.patch.object(
                    autotest,
                    "_create_isolated_desktop",
                    return_value={"ok": True, "name": "UnitDesktop", "handle": 123},
                ),
                mock.patch.object(autotest, "_launch_process_on_desktop", side_effect=fake_launch),
                mock.patch.object(autotest, "_snapshot_process_entries", return_value=[]),
                mock.patch.object(
                    autotest,
                    "_wait_for_ydwe_war3_child",
                    return_value={"ok": True, "pid": 701, "launcherPid": 700},
                ),
                mock.patch.object(
                    autotest,
                    "_wait_for_ydwe_runtime_modules",
                    return_value={"ok": True, "pid": 701},
                ),
                mock.patch.object(autotest, "_set_process_priority_high", return_value={"ok": True}),
                mock.patch.object(autotest, "_get_process_creation_epoch_ms", return_value=1000),
                mock.patch.object(autotest, "_start_debug_monitor", return_value={"ok": True}),
                mock.patch.object(autotest.time, "sleep"),
            ):
                result = autotest.launch_war3_test(
                    war3_dir=str(war3),
                    map_path=str(source_map),
                    launcher_mode="ydwe",
                    ydwe_root=str(ydwe),
                    use_isolated_desktop=True,
                    deploy_d3d9_before_launch=False,
                    enforce_video_baseline=False,
                    auto_perf_record=False,
                )

            self.assertTrue(result["ok"], result)
            self.assertEqual(result["launcherPid"], 700)
            self.assertEqual(result["gamePid"], 701)
            self.assertEqual(autotest.STATE.war3_pid, 701)
            self.assertEqual(autotest.STATE.launcher_pid, 700)
            self.assertEqual(captured["cwd"], ydwe)
            self.assertEqual(
                captured["args"][:4],
                [str(ydwe / "YDWE.exe"), "-war3", "-loadfile", r"Maps\Test\WorldEditTestMap.w3x"],
            )
            self.assertEqual(captured["args"].count("-loadfile"), 1)
            self.assertIn("-closew2l", captured["args"])
            self.assertEqual(result["sourceMapSha256"], result["targetMapSha256"])

    def test_quick_autotest_forwards_explicit_ydwe_contract(self) -> None:
        with mock.patch.object(
            autotest,
            "launch_war3_test",
            return_value={"ok": False, "error": "expected-stop"},
        ) as launch:
            result = autotest.run_quick_autotest(
                launcher_mode="ydwe",
                ydwe_root=r"E:\Work\War3\YDWE1.32.13 - MemoryHack",
                ydwe_child_timeout_sec=33,
                ydwe_module_timeout_sec=44,
            )
        self.assertFalse(result["ok"])
        kwargs = launch.call_args.kwargs
        self.assertEqual(kwargs["launcher_mode"], "ydwe")
        self.assertEqual(kwargs["ydwe_child_timeout_sec"], 33)
        self.assertEqual(kwargs["ydwe_module_timeout_sec"], 44)
        self.assertTrue(kwargs["ydwe_root"].endswith("MemoryHack"))


if __name__ == "__main__":
    unittest.main()
