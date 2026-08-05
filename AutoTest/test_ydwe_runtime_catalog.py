from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ydwe_runtime_catalog as runtime_catalog


class YdweRuntimeCatalogTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.instance = self.root / "instance"
        self.ydwe = self.instance / "YDWE"
        self.files = {
            "war3": self.instance / "war3.exe",
            "game": self.instance / "Game.dll",
            "bin": self.ydwe / "bin" / "LuaEngine.dll",
            "common": self.ydwe / "script" / "common" / "common.j",
            "war3_script": self.ydwe / "script" / "war3" / "main.lua",
            "plugin": self.ydwe / "plugin" / "warcraft3" / "yd_loader.dll",
        }
        for index, path in enumerate(self.files.values(), start=1):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((f"fixture-{index}\n").encode("utf-8"))
        nested = self.ydwe / "bin" / "nested" / "runtime.dat"
        nested.parent.mkdir(parents=True)
        nested.write_bytes(b"nested-runtime\n")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def audit(self, **overrides: object) -> dict[str, object]:
        values: dict[str, object] = {
            "instance_root": self.instance,
            "ydwe_root": self.ydwe,
        }
        values.update(overrides)
        return runtime_catalog.audit_ydwe_runtime_catalog(**values)  # type: ignore[arg-type]

    def test_catalog_is_deterministic_and_uses_canonical_relative_paths(self) -> None:
        first = self.audit()
        second = self.audit()

        self.assertTrue(first["ok"])
        self.assertEqual(first["catalogSha256"], second["catalogSha256"])
        self.assertEqual(first["files"], second["files"])
        self.assertEqual(7, first["fileCount"])
        paths = [row["relativePath"] for row in first["files"]]  # type: ignore[index]
        self.assertIn("war3.exe", paths)
        self.assertIn("Game.dll", paths)
        self.assertIn("YDWE/bin/nested/runtime.dat", paths)
        self.assertTrue(all("\\" not in path and not Path(path).is_absolute() for path in paths))

    def test_catalog_changes_when_source_content_changes(self) -> None:
        before = self.audit()
        self.files["common"].write_bytes(b"changed-common-runtime\n")
        after = self.audit()

        self.assertTrue(before["ok"] and after["ok"])
        self.assertNotEqual(before["catalogSha256"], after["catalogSha256"])

    def test_missing_core_file_or_required_tree_is_rejected(self) -> None:
        self.files["game"].unlink()
        missing_core = self.audit()
        self.assertFalse(missing_core["ok"])
        self.assertIn("Game.dll", missing_core["blockers"][0])  # type: ignore[index]

        self.files["game"].write_bytes(b"restored-game\n")
        self.files["plugin"].unlink()
        empty_tree = self.audit()
        self.assertFalse(empty_tree["ok"])
        self.assertIn("不得为空", empty_tree["blockers"][0])  # type: ignore[index]

    def test_ydwe_root_outside_instance_is_rejected(self) -> None:
        outside = self.root / "outside-YDWE"
        outside.mkdir()
        result = self.audit(ydwe_root=outside)

        self.assertFalse(result["ok"])
        self.assertIn("严格位于 instance_root 内", result["blockers"][0])  # type: ignore[index]

    def test_reparse_on_intermediate_component_is_rejected(self) -> None:
        flagged = self.ydwe / "script"
        original = runtime_catalog._path_is_reparse

        def fake_reparse(path: Path) -> bool:
            return path == flagged or original(path)

        with mock.patch.object(runtime_catalog, "_path_is_reparse", side_effect=fake_reparse):
            result = self.audit()

        self.assertFalse(result["ok"])
        self.assertIn("reparse point", result["blockers"][0])  # type: ignore[index]
        self.assertIn(str(flagged), result["blockers"][0])  # type: ignore[index]

    def test_casefold_relative_path_collision_is_rejected(self) -> None:
        alpha = self.ydwe / "bin" / "alpha.dll"
        beta = self.ydwe / "bin" / "beta.dll"
        alpha.write_bytes(b"alpha\n")
        beta.write_bytes(b"beta\n")
        original = runtime_catalog._canonical_relative_path

        def fake_canonical(path: Path, instance_root: Path) -> str:
            if path == alpha:
                return "YDWE/bin/Collision.dll"
            if path == beta:
                return "YDWE/bin/collision.dll"
            return original(path, instance_root)

        with mock.patch.object(
            runtime_catalog,
            "_canonical_relative_path",
            side_effect=fake_canonical,
        ):
            result = self.audit()

        self.assertFalse(result["ok"])
        self.assertIn("大小写重名", result["blockers"][0])  # type: ignore[index]

    def test_result_never_claims_handle_retention_or_launch_authority(self) -> None:
        success = self.audit()
        failure = self.audit(ydwe_root=self.root / "missing")

        for result in (success, failure):
            self.assertFalse(result["handlesRetained"])
            self.assertFalse(result["launchable"])
            self.assertFalse(result["immutabilityGuaranteed"])
            self.assertTrue(result["sourceMayChangeAfterAudit"])
        self.assertEqual("RUNTIME_CATALOG_AUDITED_NOT_LAUNCHABLE", success["code"])


if __name__ == "__main__":
    unittest.main()
