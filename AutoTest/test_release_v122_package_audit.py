"""Strict regression tests for the v1.22 package manifest audit."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import release_v122_package_audit as audit


def pe_bytes(machine=0x14C, magic=0x10B, characteristics=0x2102, size=0x2000):
    data = bytearray(size)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x40)
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", data, 0x44, machine)
    struct.pack_into("<H", data, 0x56, characteristics)
    struct.pack_into("<H", data, 0x58, magic)
    return bytes(data)


def digest(data):
    return hashlib.sha256(data).hexdigest().upper()


def write_file(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def make_package(root, dll_data=None, scope="player", extra_files=None, mutate=None):
    package_dir = Path(root) / "package"
    package_dir.mkdir()
    dll_data = dll_data if dll_data is not None else pe_bytes()
    payload = {"d3d9.dll": dll_data, "README.md": b"readme"}
    payload.update(extra_files or {})
    for relative, data in payload.items():
        write_file(package_dir / relative, data)
    files = {
        relative: {"sha256": digest(data), "size": len(data)}
        for relative, data in payload.items()
    }
    manifest = {
        "schema": 1,
        "kind": "warvk-offline-candidate",
        "scope": scope,
        "target": {"path": "d3d9.dll", **files["d3d9.dll"]},
        "files": files,
        "flags": {key: False for key in audit.REQUIRED_FLAGS},
        "buildConfig": dict(audit.BUILD_CONFIG_EXPECTED),
        "limits": [
            "offline candidate only",
            "no runtime, GPU, or visual acceptance",
        ],
    }
    if mutate is not None:
        mutate(manifest)
    (package_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    return package_dir, manifest, payload


def make_zip(package_dir, archive, mutate=None):
    with zipfile.ZipFile(archive, "w") as zipped:
        for path in sorted(package_dir.rglob("*")):
            if not path.is_file():
                continue
            payload = path.read_bytes()
            if mutate is not None:
                payload = mutate(path, payload)
            zipped.writestr(path.relative_to(package_dir).as_posix(), payload)


class ReleaseV122PackageAuditTest(unittest.TestCase):
    def test_valid_player_package_with_license_and_catalog_passes(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                extra_files={"LICENSE.txt": b"license", "pack.json": b"{}"},
            )
            result = audit.audit_package(package_dir)
            self.assertTrue(result["ok"], result["errors"])
            self.assertEqual(result["scope"], "player")
            self.assertEqual(result["target"]["largeAddressAware"], False)
            self.assertEqual(result["target"]["x64"], False)

    def test_valid_author_water_and_catalog_assets_pass(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                scope="author",
                extra_files={
                    "assets/water_v121.png": b"png",
                    "WarVK/water/legacy.json": b"{}",
                    "docs/catalog.json": b"{}",
                },
            )
            result = audit.audit_package(package_dir)
            self.assertTrue(result["ok"], result["errors"])

    def test_missing_target_sha_size_only_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].pop("sha256"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("target", result["errors"][0])

    def test_missing_target_size_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].pop("size"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("target", result["errors"][0])

    def test_parent_path_target_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("path", "../d3d9.dll"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("parent path components", result["errors"][0])

    def test_target_path_must_be_exactly_d3d9_dll(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("path", "assets/other.dll"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("exactly d3d9.dll", result["errors"][0])

    def test_string_bool_flag_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["flags"].__setitem__("productAccepted", "true"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("must be a JSON boolean", result["errors"][0])

    def test_top_level_acceptance_alias_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m.__setitem__("productAccepted", "true"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("keys mismatch", result["errors"][0])

    def test_unlisted_extra_python_file_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            write_file(package_dir / "extra.py", b"pass")
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("membership mismatch", result["errors"][0])
            self.assertIn("extra.py", result["errors"][0])

    def test_target_sha_must_be_hex64(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("sha256", "ABC"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("64 hex", result["errors"][0])

    def test_target_size_must_be_integer(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("size", "123"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("JSON integer", result["errors"][0])

    def test_schema_and_kind_are_strict(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root, mutate=lambda m: m.__setitem__("schema", 2))
            self.assertFalse(audit.audit_package(package_dir)["ok"])
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root, mutate=lambda m: m.__setitem__("kind", "warvk-release-candidate"))
            self.assertFalse(audit.audit_package(package_dir)["ok"])

    def test_declared_file_hash_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["files"]["README.md"].__setitem__("sha256", "A" * 64),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("declared file bytes", result["errors"][0])

    def test_duplicate_json_key_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            (package_dir / "manifest.json").write_text(
                '{"kind":"warvk-offline-candidate","kind":"warvk-offline-candidate"}',
                encoding="utf-8",
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("duplicate JSON key", result["errors"][0])

    def test_x64_candidate_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root, dll_data=pe_bytes(machine=0x8664, magic=0x20B))
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("PE32/i386", result["errors"][0])

    def test_laa_is_parsed_from_coff_not_equated_with_x64(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root, dll_data=pe_bytes(characteristics=0x2122)
            )
            result = audit.audit_package(package_dir)
            self.assertTrue(result["ok"], result["errors"])
            self.assertTrue(result["target"]["largeAddressAware"])
            self.assertFalse(result["target"]["x64"])
            self.assertEqual(result["target"]["pe"], "PE32")

    def test_build_config_provenance_flags_must_be_false(self):
        for key in ("waterExperimental", "nativeAutoLights", "x64Product"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as root:
                package_dir, _, _ = make_package(
                    root,
                    mutate=lambda m, k=key: m["buildConfig"].__setitem__(k, True),
                )
                result = audit.audit_package(package_dir)
                self.assertFalse(result["ok"])
                self.assertIn("config provenance", result["errors"][0])

    def test_product_scope_rejects_game_asset_and_evidence_paths(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root, extra_files={"game.w3x": b"asset"})
            self.assertFalse(audit.audit_package(package_dir)["ok"])
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root, extra_files={"evidence/raw.txt": b"raw"})
            self.assertFalse(audit.audit_package(package_dir)["ok"])

    def test_author_scope_rejects_nested_binary_game_and_evidence(self):
        cases = (
            ("assets/helper.exe", b"MZ"),
            ("WarVK/game.mpq", b"mpq"),
            ("docs/artifact.dmp", b"dump"),
            ("docs/evidence/raw.json", b"{}"),
        )
        for relative, payload in cases:
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as root:
                package_dir, _, _ = make_package(
                    root, scope="author", extra_files={relative: payload}
                )
                result = audit.audit_package(package_dir)
                self.assertFalse(result["ok"], relative)

    def test_colon_drive_and_windows_alias_paths_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("path", "C:/d3d9.dll"),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("colon", result["errors"][0])
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                mutate=lambda m: m["target"].__setitem__("path", "d3d9.dll."),
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("space or dot", result["errors"][0])
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(
                root,
                scope="author",
                extra_files={"docs/NUL.txt": b"reserved"},
            )
            result = audit.audit_package(package_dir)
            self.assertFalse(result["ok"])
            self.assertIn("reserved", result["errors"][0])

    def test_malformed_scope_rejected_without_type_error(self):
        for bad_scope in ([], {}, 7, None):
            with self.subTest(scope=bad_scope), tempfile.TemporaryDirectory() as root:
                package_dir, _, _ = make_package(
                    root,
                    mutate=lambda m, value=bad_scope: m.__setitem__("scope", value),
                )
                result = audit.audit_package(package_dir)
                self.assertFalse(result["ok"])
                self.assertIn("scope", result["errors"][0])

    def test_build_config_is_self_declared_not_independent(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            result = audit.audit_package(package_dir)
            self.assertTrue(result["ok"], result["errors"])
            self.assertEqual(
                result["buildConfigProvenance"], "self-declared-not-independent"
            )
            self.assertFalse(result["buildEvidenceVerified"])

    def test_archive_identity_matches_package(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            archive = Path(root) / "candidate.zip"
            make_zip(package_dir, archive)
            result = audit.audit_package(package_dir, archive_path=archive)
            self.assertTrue(result["ok"], result["errors"])

    def test_archive_extra_member_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            archive = Path(root) / "candidate.zip"
            make_zip(package_dir, archive)
            with zipfile.ZipFile(archive, "a") as zipped:
                zipped.writestr("extra.bin", b"extra")
            result = audit.audit_package(package_dir, archive_path=archive)
            self.assertFalse(result["ok"])
            self.assertIn("archive member set", result["errors"][0])

    def test_archive_member_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            package_dir, _, _ = make_package(root)
            archive = Path(root) / "candidate.zip"
            make_zip(
                package_dir,
                archive,
                mutate=lambda path, payload: (
                    payload[:-1] + bytes([payload[-1] ^ 0xFF])
                    if path.name == "d3d9.dll" else payload
                ),
            )
            result = audit.audit_package(package_dir, archive_path=archive)
            self.assertFalse(result["ok"])
            self.assertIn("archive member differs", result["errors"][0])


if __name__ == "__main__":
    unittest.main()
