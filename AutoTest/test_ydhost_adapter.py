#!/usr/bin/env python
# -*- coding: utf-8 -*-

import hashlib
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import ydhost_adapter
from ydhost_adapter import (
    MAPDUMP_SOURCE_SHA256,
    audit_map_metadata,
    audit_ydhost_bundle,
    discover_archived_ydhost_bundles,
    generate_ydhost_map_metadata,
    inspect_map_container,
    parse_map_cfg,
    provision_ydhost_assets,
    sha256_file,
)


VALID_MAP_CFG = """\
map_size = 1 2 3 4
map_info = 5 6 7 8
map_crc = 9 10 11 12
map_sha1 = 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
map_options = 0
map_width = 52 0
map_height = 52 0
map_slot1 = 0 255 0 0 0 0 1 1 100
map_slot2 = 0 255 0 0 0 1 2 1 100
"""


def _fake_profile() -> tuple[dict[str, bytes], dict[str, str]]:
    payloads = {
        "ydhost.exe": b"fake-ydhost",
        "msvcp140.dll": b"fake-msvcp",
        "vcruntime140.dll": b"fake-vcruntime",
    }
    hashes = {name: hashlib.sha256(payload).hexdigest() for name, payload in payloads.items()}
    return payloads, hashes


class BundleAuditTests(unittest.TestCase):
    def test_exact_bundle_is_hash_locked(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            bundle = sandbox / "plugin" / "ydhost"
            bundle.mkdir(parents=True)
            for name, payload in payloads.items():
                (bundle / name).write_bytes(payload)
            result = audit_ydhost_bundle(bundle, sandbox, hashes)
            self.assertTrue(result["ok"], result)
            (bundle / "ydhost.exe").write_bytes(b"drift")
            drift = audit_ydhost_bundle(bundle, sandbox, hashes)
            self.assertFalse(drift["ok"])
            self.assertTrue(any("SHA-256" in error for error in drift["errors"]))

    def test_required_binary_reparse_file_is_rejected_before_hash(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            bundle = sandbox / "plugin" / "ydhost"
            bundle.mkdir(parents=True)
            for name, payload in payloads.items():
                (bundle / name).write_bytes(payload)
            real_probe = ydhost_adapter._is_reparse_point

            def fake_probe(path: Path) -> bool:
                return Path(path).name == "ydhost.exe" or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=fake_probe):
                result = audit_ydhost_bundle(bundle, sandbox, hashes)
            self.assertFalse(result["ok"])
            self.assertTrue(any("reparse" in error for error in result["errors"]))

    def test_archived_assets_are_detected_without_extraction(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp)
            archive_path = sandbox / "YDWE-test.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                for name, payload in payloads.items():
                    archive.writestr(f"YDWE/plugin/ydhost/{name}", payload)
            result = discover_archived_ydhost_bundles(sandbox, hashes)
            self.assertEqual(1, len(result))
            self.assertTrue(result[0]["complete"], result)
            self.assertFalse((sandbox / "plugin").exists())

    def test_archive_zip_bomb_shape_is_rejected_before_bulk_read(self) -> None:
        payload = b"0" * (1024 * 1024 + 1)
        expected = {"ydhost.exe": hashlib.sha256(payload).hexdigest()}
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp)
            archive_path = sandbox / "YDWE-bomb.zip"
            with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("YDWE/plugin/ydhost/ydhost.exe", payload)
            result = discover_archived_ydhost_bundles(sandbox, expected)
            self.assertEqual(1, len(result))
            self.assertFalse(result[0]["complete"])
            self.assertTrue(
                any("上限" in error or "ZIP bomb" in error for error in result[0]["errors"]),
                result,
            )

    def test_provision_manifest_short_circuits_after_bundle_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "ydhost"
            root.mkdir()
            (root / ydhost_adapter.PROVISION_MANIFEST_NAME).write_text(
                "this must not be parsed", encoding="utf-8"
            )
            result = ydhost_adapter.audit_provision_manifest(
                {"ok": False, "root": str(root), "files": []}
            )
            self.assertFalse(result["ok"])
            self.assertEqual(["ydhost 二进制资产校验未通过"], result["errors"])
            self.assertNotIn("data", result)

    def test_provision_manifest_and_bundle_reparse_are_rejected(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "ydhost"
            root.mkdir()
            files = []
            for name, payload in payloads.items():
                (root / name).write_bytes(payload)
                files.append({"name": name, "size": len(payload), "sha256": hashes[name]})
            manifest_path = root / ydhost_adapter.PROVISION_MANIFEST_NAME
            manifest_path.write_text(
                json.dumps(
                    {
                        "schemaVersion": ydhost_adapter.PROVISION_SCHEMA_VERSION,
                        "profileId": ydhost_adapter.YDHOST_PROFILE_ID,
                        "targetRoot": str(root),
                        "files": files,
                    }
                ),
                encoding="utf-8",
            )
            bundle = {"ok": True, "root": str(root), "files": files}
            real_probe = ydhost_adapter._is_reparse_point

            def manifest_reparse(path: Path) -> bool:
                return Path(path).name == ydhost_adapter.PROVISION_MANIFEST_NAME or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=manifest_reparse):
                manifest_result = ydhost_adapter.audit_provision_manifest(bundle)
            self.assertFalse(manifest_result["ok"])
            self.assertTrue(any("reparse" in error for error in manifest_result["errors"]))

            def bundle_reparse(path: Path) -> bool:
                return Path(path) == root or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=bundle_reparse):
                bundle_result = ydhost_adapter.audit_provision_manifest(bundle)
            self.assertFalse(bundle_result["ok"])
            self.assertTrue(any("reparse" in error for error in bundle_result["errors"]))


class ProvisionTests(unittest.TestCase):
    def test_default_is_dry_run_apply_is_idempotent_and_drift_is_rejected(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sandbox = root / "sandbox"
            source = root / "source"
            sandbox.mkdir()
            source.mkdir()
            for name, payload in payloads.items():
                (source / name).write_bytes(payload)

            dry_run = provision_ydhost_assets(source, sandbox, required_files=hashes)
            self.assertTrue(dry_run["ok"], dry_run)
            self.assertTrue(dry_run["dryRun"])
            self.assertFalse((sandbox / "plugin" / "ydhost").exists())

            applied = provision_ydhost_assets(source, sandbox, apply=True, required_files=hashes)
            self.assertTrue(applied["ok"], applied)
            self.assertTrue(applied["applied"])
            target = sandbox / "plugin" / "ydhost"
            self.assertTrue((target / "provision-manifest.json").is_file())

            second = provision_ydhost_assets(source, sandbox, apply=True, required_files=hashes)
            self.assertTrue(second["ok"], second)
            self.assertTrue(all(row["action"].startswith("keep") for row in second["operations"]))

            (target / "ydhost.exe").write_bytes(b"target-drift")
            drift = provision_ydhost_assets(source, sandbox, required_files=hashes)
            self.assertFalse(drift["ok"])
            self.assertTrue(any("漂移" in error for error in drift["errors"]))

    def test_target_outside_sandbox_is_rejected(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sandbox = root / "sandbox"
            source = root / "source"
            sandbox.mkdir()
            source.mkdir()
            for name, payload in payloads.items():
                (source / name).write_bytes(payload)
            result = provision_ydhost_assets(
                source,
                sandbox,
                root / "outside",
                required_files=hashes,
            )
            self.assertFalse(result["ok"])
            self.assertTrue(any("目标只能" in error for error in result["errors"]))

    def test_intermediate_reparse_component_is_rejected(self) -> None:
        payloads, hashes = _fake_profile()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sandbox = root / "sandbox"
            source = root / "source"
            (sandbox / "plugin").mkdir(parents=True)
            source.mkdir()
            for name, payload in payloads.items():
                (source / name).write_bytes(payload)

            real_probe = ydhost_adapter._is_reparse_point

            def fake_probe(path: Path) -> bool:
                return Path(path).name == "plugin" or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=fake_probe):
                result = provision_ydhost_assets(source, sandbox, required_files=hashes)
            self.assertFalse(result["ok"])
            self.assertTrue(any("reparse point" in error for error in result["errors"]))

    def test_snapshot_copy_detects_source_change_after_audit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sandbox = root / "sandbox"
            source = root / "ydwe"
            sandbox.mkdir()
            file_path = source / "bin" / "lua.exe"
            file_path.parent.mkdir(parents=True)
            file_path.write_bytes(b"audited")
            row = {
                "relativePath": "bin/lua.exe",
                "size": 7,
                "sha256": hashlib.sha256(b"audited").hexdigest(),
            }
            catalog_sha = ydhost_adapter._catalog_digest([row])
            audited = {"ok": True, "files": [row]}
            file_path.write_bytes(b"changed-after-audit")
            with mock.patch.object(ydhost_adapter, "MAPDUMP_CATALOG_FILE_COUNT", 1), mock.patch.object(
                ydhost_adapter, "MAPDUMP_CATALOG_SHA256", catalog_sha
            ):
                with self.assertRaises(OSError):
                    ydhost_adapter._materialize_mapdump_snapshot(
                        source,
                        sandbox / "snapshot",
                        sandbox,
                        audited,
                    )


class MapMetadataTests(unittest.TestCase):
    def test_trusted_system_directory_rejects_reparse_component(self) -> None:
        with self.assertRaisesRegex(OSError, "绝对路径"):
            ydhost_adapter._validate_trusted_system_directory(Path("relative"), "test system")
        with tempfile.TemporaryDirectory() as temp:
            system = Path(temp) / "Windows" / "System32"
            system.mkdir(parents=True)
            self.assertEqual(
                system.absolute(),
                ydhost_adapter._validate_trusted_system_directory(system, "test system"),
            )
            real_probe = ydhost_adapter._is_reparse_point

            def fake_probe(path: Path) -> bool:
                return Path(path) == system or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=fake_probe):
                with self.assertRaisesRegex(ValueError, "reparse"):
                    ydhost_adapter._validate_trusted_system_directory(system, "test system")

    def test_container_distinguishes_mpq_and_w2l_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            mpq = root / "packed.w3x"
            mpq.write_bytes(b"HM3W" + b"\x00" * 508 + b"MPQ\x1a" + b"payload")
            marker = root / "source.w3x"
            marker.write_bytes(b"\x00" * 8 + b"W2L\x01")
            self.assertEqual("mpq", inspect_map_container(mpq)["kind"])
            self.assertEqual("w3x2lni-directory-marker", inspect_map_container(marker)["kind"])

    def test_map_cfg_schema_and_sha_bound_manifest(self) -> None:
        parsed = parse_map_cfg(VALID_MAP_CFG)
        self.assertEqual([0, 255, 0, 0, 0, 0, 1, 1, 100], parsed["map_slot1"])
        with self.assertRaises(ValueError):
            parse_map_cfg(VALID_MAP_CFG.replace("map_slot2", "map_slot3"))

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            map_path = root / "packed.w3x"
            map_path.write_bytes(b"HM3W" + b"\x00" * 508 + b"MPQ\x1a" + b"payload")
            metadata = root / "metadata"
            metadata.mkdir()
            cfg = metadata / "map.cfg"
            cfg.write_text(VALID_MAP_CFG, encoding="utf-8")
            manifest = {
                "schemaVersion": ydhost_adapter.METADATA_SCHEMA_VERSION,
                "generator": "YDWE mapdump",
                "generatorSha256": MAPDUMP_SOURCE_SHA256,
                "toolchainProfileId": "test-snapshot",
                "mapSha256": sha256_file(map_path),
                "mapSize": map_path.stat().st_size,
                "mapCfgSha256": sha256_file(cfg),
                "slotCount": 2,
            }
            toolchain_row = {
                "relativePath": "bin/lua.exe",
                "size": 3,
                "sha256": hashlib.sha256(b"lua").hexdigest(),
            }
            catalog_sha = ydhost_adapter._catalog_digest([toolchain_row])
            manifest.update(
                {
                    "toolchainCatalogSha256": catalog_sha,
                    "toolchainFileCount": 1,
                    "toolchainFiles": [toolchain_row],
                }
            )
            with mock.patch.object(ydhost_adapter, "MAPDUMP_PROFILE_ID", "test-snapshot"), mock.patch.object(
                ydhost_adapter, "MAPDUMP_CATALOG_SHA256", catalog_sha
            ), mock.patch.object(ydhost_adapter, "MAPDUMP_CATALOG_FILE_COUNT", 1):
                (metadata / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
                result = audit_map_metadata(map_path, metadata)
                self.assertTrue(result["ok"], result)
                manifest["mapSha256"] = "0" * 64
                (metadata / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
                self.assertFalse(audit_map_metadata(map_path, metadata)["ok"])

    def test_metadata_child_reparse_file_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            sandbox = Path(temp) / "sandbox"
            sandbox.mkdir()
            map_path = sandbox / "packed.w3x"
            map_path.write_bytes(b"HM3W" + b"\x00" * 508 + b"MPQ\x1a" + b"payload")
            metadata = sandbox / "metadata"
            metadata.mkdir()
            (metadata / "map.cfg").write_text(VALID_MAP_CFG, encoding="utf-8")
            (metadata / "manifest.json").write_text("{}", encoding="utf-8")
            real_probe = ydhost_adapter._is_reparse_point

            def fake_probe(path: Path) -> bool:
                return Path(path).name == "map.cfg" or real_probe(path)

            with mock.patch.object(ydhost_adapter, "_is_reparse_point", side_effect=fake_probe):
                result = audit_map_metadata(map_path, metadata, sandbox)
            self.assertFalse(result["ok"])
            self.assertTrue(any("reparse" in error for error in result["errors"]))
            self.assertNotIn("mapCfgSha256", result)

    def test_mapdump_generator_is_temporary_by_default_and_sha_bound_when_applied(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            sandbox = root / "sandbox"
            ydwe = root / "ydwe"
            sandbox.mkdir()
            ydwe.mkdir()
            lua = ydwe / "bin" / "lua.exe"
            lua.parent.mkdir()
            lua.write_bytes(b"lua")
            map_path = sandbox / "packed.w3x"
            map_path.write_bytes(b"HM3W" + b"\x00" * 508 + b"MPQ\x1a" + b"payload")
            toolchain_row = {
                "relativePath": "bin/lua.exe",
                "size": 3,
                "sha256": hashlib.sha256(b"lua").hexdigest(),
            }
            catalog_sha = ydhost_adapter._catalog_digest([toolchain_row])
            fake_toolchain = {
                "ok": True,
                "profileId": "test",
                "root": str(ydwe),
                "errors": [],
                "files": [toolchain_row],
            }

            launched_commands = []
            launched_environments = []
            launched_cwds = []

            def fake_run(command, **kwargs):
                launched_commands.append(command)
                launched_environments.append(dict(kwargs["env"]))
                launched_cwds.append(Path(kwargs["cwd"]))
                Path(command[-1]).write_text(VALID_MAP_CFG, encoding="utf-8")
                return __import__("subprocess").CompletedProcess(command, 0, "ok", "")

            with mock.patch.dict(
                ydhost_adapter.os.environ,
                {
                    "LUA_INIT": "@evil.lua",
                    "LUA_INIT_5_3": "@evil53.lua",
                    "LUA_PATH": "evil-path",
                    "LUA_CPATH": "evil-cpath",
                    "SystemRoot": str(root / "evil-windows"),
                    "WINDIR": str(root / "evil-windir"),
                },
                clear=False,
            ), mock.patch.object(ydhost_adapter, "audit_mapdump_toolchain", return_value=fake_toolchain), mock.patch.object(
                ydhost_adapter.subprocess, "run", side_effect=fake_run
            ), mock.patch.object(ydhost_adapter, "MAPDUMP_PROFILE_ID", "test-snapshot"), mock.patch.object(
                ydhost_adapter, "MAPDUMP_CATALOG_SHA256", catalog_sha
            ), mock.patch.object(ydhost_adapter, "MAPDUMP_CATALOG_FILE_COUNT", 1):
                dry_run = generate_ydhost_map_metadata(ydwe, sandbox, map_path)
                self.assertTrue(dry_run["ok"], dry_run)
                self.assertTrue(dry_run["dryRun"])
                self.assertFalse(Path(dry_run["targetRoot"]).exists())
                self.assertIn("toolchain-snapshot", launched_commands[0][0])
                self.assertEqual("-E", launched_commands[0][1])
                self.assertNotEqual(str(lua), launched_commands[0][0])
                self.assertIn("map-snapshot", launched_commands[0][-3])
                self.assertNotEqual(str(map_path), launched_commands[0][-3])
                environment = launched_environments[0]
                self.assertEqual("1", environment["LUA_NOENV"])
                self.assertFalse(
                    any(
                        key.upper().startswith(("LUA_INIT", "LUA_PATH", "LUA_CPATH"))
                        for key in environment
                    )
                )
                self.assertIn("toolchain-snapshot", environment["PATH"])
                policy = dry_run["execution"]["environmentPolicy"]
                self.assertFalse(policy["parentEnvironmentInherited"])
                self.assertEqual(environment["SystemRoot"], policy["trustedWindowsDirectory"])
                self.assertEqual(environment["WINDIR"], policy["trustedWindowsDirectory"])
                self.assertEqual(environment["PATH"].split(os.pathsep), policy["pathEntries"])
                self.assertEqual(launched_cwds[0], Path(policy["workingDirectory"]))
                self.assertEqual("bin", launched_cwds[0].name)
                self.assertNotIn(str(root / "evil-windows"), environment.values())

                applied = generate_ydhost_map_metadata(ydwe, sandbox, map_path, apply=True)
                self.assertTrue(applied["ok"], applied)
                self.assertTrue(applied["applied"])
                target = Path(applied["targetRoot"])
                self.assertTrue((target / "map.cfg").is_file())
                self.assertTrue(audit_map_metadata(map_path, target)["ok"])

                manifest_path = target / "manifest.json"
                legacy_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                legacy_manifest["schemaVersion"] = 1
                legacy_manifest["toolchainProfileId"] = "legacy-profile"
                legacy_manifest.pop("toolchainCatalogSha256", None)
                legacy_manifest.pop("toolchainFileCount", None)
                legacy_manifest.pop("toolchainFiles", None)
                manifest_path.write_text(json.dumps(legacy_manifest), encoding="utf-8")
                refused = generate_ydhost_map_metadata(ydwe, sandbox, map_path, apply=True)
                self.assertFalse(refused["ok"], refused)
                upgraded = generate_ydhost_map_metadata(
                    ydwe,
                    sandbox,
                    map_path,
                    apply=True,
                    replace_existing=True,
                )
                self.assertTrue(upgraded["ok"], upgraded)
                self.assertTrue(upgraded["applied"])
                self.assertTrue(Path(upgraded["backupRoot"]).is_dir())
                self.assertTrue(audit_map_metadata(map_path, target)["ok"])


if __name__ == "__main__":
    unittest.main()
