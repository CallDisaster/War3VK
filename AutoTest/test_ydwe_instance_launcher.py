from __future__ import annotations

import hashlib
import inspect
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ydwe_instance_launcher as launcher


def _write_pe(path: Path, *, dll: bool, machine: int = 0x014C, magic: int = 0x010B) -> None:
    data = bytearray(0x200)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    characteristics = 0x0002 | (0x2000 if dll else 0)
    struct.pack_into("<HHIIIHH", data, 0x84, machine, 1, 0, 0, 0, 0xE0, characteristics)
    struct.pack_into("<H", data, 0x98, magic)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class YdweInstanceLauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sandbox = self.root / "sandbox"
        self.pool = self.sandbox / "_AutoTestInstances"
        self.instance = self.pool / "run-a" / "client-01" / "root"
        self.ydwe = self.instance / "YDWE"
        self.trusted = self.root / "trusted-helper"
        self.helper = self.trusted / "ydwe_instance_launcher.exe"

        _write_pe(self.helper, dll=False)
        _write_pe(self.instance / "war3.exe", dll=False)
        _write_pe(self.instance / "Game.dll", dll=True)
        _write_pe(self.ydwe / "bin" / "LuaEngine.dll", dll=True)
        _write_pe(self.ydwe / "bin" / "lua53.dll", dll=True)
        _write_pe(self.ydwe / "bin" / "ydbase.dll", dll=True)
        _write_pe(self.ydwe / "plugin" / "warcraft3" / "yd_loader.dll", dll=True)
        main = self.ydwe / "script" / "war3" / "main.lua"
        main.parent.mkdir(parents=True, exist_ok=True)
        main.write_text("return true\n", encoding="utf-8")
        config = self.ydwe / "plugin" / "warcraft3" / "config.cfg"
        config.write_text("[Enable]\n", encoding="utf-8")

        self.hashes = launcher.RuntimeHashes(
            helper=_sha(self.helper),
            war3=_sha(self.instance / "war3.exe"),
            game=_sha(self.instance / "Game.dll"),
            lua_engine=_sha(self.ydwe / "bin" / "LuaEngine.dll"),
            yd_loader=_sha(self.ydwe / "plugin" / "warcraft3" / "yd_loader.dll"),
            lua53=_sha(self.ydwe / "bin" / "lua53.dll"),
            ydbase=_sha(self.ydwe / "bin" / "ydbase.dll"),
            war3_main=_sha(main),
            plugin_config=_sha(config),
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    @staticmethod
    def _binding_ok(job: str, desktop: str) -> dict[str, object]:
        return {
            "ok": True,
            "jobExists": True,
            "jobKillOnClose": True,
            "jobActiveProcesses": 0,
            "desktopExists": True,
            "job": job,
            "desktop": desktop,
        }

    def _prepare(self, **overrides: object) -> dict[str, object]:
        values: dict[str, object] = {
            "sandbox_root": self.sandbox,
            "instance_root": self.instance,
            "ydwe_root": self.ydwe,
            "helper_path": self.helper,
            "trusted_helper_root": self.trusted,
            "hashes": self.hashes,
            "job_name": "Local\\War3AutoTest_run-a_client-01",
            "desktop_name": "War3AutoTest_run-a_client-01",
            "binding_probe": self._binding_ok,
        }
        values.update(overrides)
        with mock.patch.object(launcher, "DEFAULT_SANDBOX_ROOT", self.sandbox):
            return launcher.prepare_ydwe_instance_launch(**values)  # type: ignore[arg-type]

    def test_default_is_dry_run_and_never_launchable(self) -> None:
        result = self._prepare()
        self.assertFalse(result["ok"])
        self.assertFalse(result["launchable"])
        self.assertEqual("DRY_RUN_ONLY", result["code"])
        self.assertFalse(result["realProcessLaunchExecuted"])
        self.assertIn("--windowed", result["command"])
        self.assertTrue(result["commandContract"]["helperInjectsExactlyOneAuto"])

    def test_execute_opt_in_only_generates_audit_command(self) -> None:
        result = self._prepare(execute=True)
        self.assertFalse(result["ok"])
        self.assertFalse(result["launchable"])
        self.assertTrue(result["auditCommandGenerated"])
        self.assertFalse(result["executionRequestGenerated"])
        self.assertFalse(result["realProcessLaunchExecuted"])
        self.assertFalse(result["countsAsLanAcceptance"])
        self.assertEqual("EXECUTION_BRIDGE_NOT_PRODUCTION_READY", result["code"])
        self.assertEqual(3, len(result["blockers"]))
        command = result["command"]
        self.assertEqual(str(self.helper), command[0])
        self.assertNotIn("-auto", command, "-auto 只能由 helper 固定加入")

    def test_forgeable_job_owner_boolean_is_not_part_of_api(self) -> None:
        parameters = inspect.signature(launcher.prepare_ydwe_instance_launch).parameters
        self.assertNotIn("job_handle_retained_by_caller", parameters)

    def test_helper_identity_mismatch_fails_before_request(self) -> None:
        bad = launcher.RuntimeHashes(**{**self.hashes.__dict__, "helper": "0" * 64})
        result = self._prepare(execute=True, hashes=bad)
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertFalse(result["executionRequestGenerated"])
        self.assertIn("helper SHA-256", result["blockers"][0])

    def test_ydwe_root_must_be_inside_instance(self) -> None:
        outside = self.sandbox / "shared-YDWE"
        outside.mkdir()
        result = self._prepare(ydwe_root=outside)
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertIn("越过允许根", result["blockers"][0])

    def test_path_environment_separator_is_rejected(self) -> None:
        result = self._prepare(ydwe_root=Path(str(self.ydwe) + ";shadow"))
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertIn("不安全字符", result["blockers"][0])

    def test_reparse_component_is_rejected(self) -> None:
        original = launcher._path_is_reparse

        def fake(path: Path) -> bool:
            return path == self.ydwe or original(path)

        with mock.patch.object(launcher, "_path_is_reparse", side_effect=fake):
            result = self._prepare()
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertIn("reparse point", result["blockers"][0])

    def test_binding_must_prove_empty_kill_on_close_job_and_desktop(self) -> None:
        def bad_probe(job: str, desktop: str) -> dict[str, object]:
            del job, desktop
            return {"ok": True, "jobExists": True, "jobKillOnClose": False,
                    "jobActiveProcesses": 1, "desktopExists": True}

        result = self._prepare(binding_probe=bad_probe, execute=True)
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertFalse(result["launchable"])

    def test_reserved_lan_arguments_are_rejected(self) -> None:
        for argument in ("-auto", "-loadfile", "-opengl", "-ydwe", "-window"):
            with self.subTest(argument=argument):
                result = self._prepare(passthrough_arguments=[argument])
                self.assertEqual("PREFLIGHT_REJECTED", result["code"])

    def test_passthrough_limits_and_nul_are_mirrored_from_helper(self) -> None:
        for arguments in (["x" * 4097], ["safe\0truncated"], ["x"] * 129):
            with self.subTest(arguments=len(arguments)):
                result = self._prepare(passthrough_arguments=arguments)
                self.assertEqual("PREFLIGHT_REJECTED", result["code"])
                self.assertFalse(result["auditCommandGenerated"])

    def test_x64_runtime_pe_is_rejected(self) -> None:
        _write_pe(self.instance / "Game.dll", dll=True, machine=0x8664, magic=0x020B)
        bad = launcher.RuntimeHashes(**{**self.hashes.__dict__, "game": _sha(self.instance / "Game.dll")})
        result = self._prepare(hashes=bad)
        self.assertEqual("PREFLIGHT_REJECTED", result["code"])
        self.assertIn("x86 PE32", result["blockers"][0])


if __name__ == "__main__":
    unittest.main()
