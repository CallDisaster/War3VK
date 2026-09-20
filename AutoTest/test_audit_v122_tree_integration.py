#!/usr/bin/env python3
"""Temp fixture tests for the read-only v1.22 A/B integration audit."""
from __future__ import annotations

import copy
import csv
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audit_v122_tree_integration as audit


def _git_test(root, *args):
    command = [
        "git", "-C", str(root),
        "-c", "user.name=test",
        "-c", "user.email=test@example.invalid",
        "-c", "commit.gpgsign=false",
        "-c", "protocol.file.allow=always",
        "-c", "core.autocrlf=false",
        *args,
    ]
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8", errors="replace")


def record(path: str, **overrides):
    base = {
        "path": path,
        "kind": "file",
        "a_present": False,
        "b_present": False,
        "a_tracked": False,
        "b_tracked": False,
        "a_in_index": False,
        "b_in_index": False,
        "a_in_head": False,
        "b_in_head": False,
        "a_index_mode": None,
        "b_index_mode": None,
        "a_head_blob": None,
        "b_head_blob": None,
        "a_index_blob": None,
        "b_index_blob": None,
        "a_worktree_present": False,
        "b_worktree_present": False,
        "a_worktree_blob": None,
        "b_worktree_blob": None,
        "a_sha256": None,
        "b_sha256": None,
        "a_size": None,
        "b_size": None,
        "a_status": None,
        "b_status": None,
        "same_worktree_content": False,
        "a_equals_a_head": False,
        "b_equals_b_head": False,
        "a_equals_b_head": False,
        "b_equals_a_head": False,
    }
    base.update(overrides)
    if base.get("a_present") or base.get("a_tracked"):
        base["a_worktree_present"] = True
    if base.get("b_present") or base.get("b_tracked"):
        base["b_worktree_present"] = True
    if base.get("a_tracked"):
        base["a_in_index"] = True
    if base.get("b_tracked"):
        base["b_in_index"] = True
    base.update(audit.classify_record(base))
    return base


class AuditFixtureTests(unittest.TestCase):
    def test_git_blob_sha1_bytes(self):
        hashed = audit._hash_bytes(b"hello\n")
        self.assertEqual(hashed["git_blob_sha1"], "ce013625030ba8dba906f756967f9e9ca394464a")
        self.assertEqual(hashed["sha256"], hashlib.sha256(b"hello\n").hexdigest())

    def test_safe_rel_rejects_absolute_and_parent(self):
        for bad in ("/etc/passwd", r"C:\Windows\x.txt", "../x", "a/../../b"):
            with self.assertRaises(audit.AuditError):
                audit._safe_rel(bad)

    def test_classify_identical(self):
        rec = record("src/a.cpp", a_present=True, b_present=True, same_worktree_content=True)
        self.assertEqual(rec["route_hint"], "identical")
        self.assertFalse(rec["manual_review"])

    def test_classify_keep_b_only(self):
        rec = record("src/new.cpp", b_present=True)
        self.assertEqual(rec["route_hint"], "keep_b")

    def test_classify_a_only_source(self):
        rec = record("src/a.cpp", a_present=True)
        self.assertEqual(rec["route_hint"], "review_a_unique")
        self.assertTrue(rec["manual_review"])

    def test_classify_a_only_local(self):
        rec = record("_run_static_all.ps1", a_present=True)
        self.assertEqual(rec["route_hint"], "review_a_local")

    def test_classify_evidence_preserved(self):
        rec = record("PlayerCrash/Crash/latest.dmp", a_present=True)
        self.assertEqual(rec["route_hint"], "preserve_external")

    def test_classify_generated_excluded(self):
        rec = record("m40_race_run1.log", a_present=True)
        self.assertEqual(rec["route_hint"], "exclude_generated")

    def test_classify_tracked_conflict(self):
        rec = record("src/a.cpp", a_present=True, b_present=True, a_tracked=True, b_tracked=True,
                     a_status=" M", b_status=" M")
        self.assertEqual(rec["route_hint"], "review_conflict")
        self.assertTrue(rec["manual_review"])

    def test_classify_a_dirty_b_clean(self):
        rec = record("src/a.cpp", a_present=True, b_present=True, a_status=" M")
        self.assertEqual(rec["route_hint"], "review_a_change")

    def test_classify_b_dirty_a_clean(self):
        rec = record("src/a.cpp", a_present=True, b_present=True, b_status=" M")
        self.assertEqual(rec["route_hint"], "keep_b")

    def test_classify_submodule(self):
        rec = record("subprojects/StormBreaker", kind="gitlink", a_present=True, b_present=True)
        self.assertEqual(rec["route_hint"], "submodule_review")
        self.assertEqual(rec["integration_action"], "do_not_integrate_submodule")

    def test_classify_head_divergence(self):
        rec = record("src/a.cpp", a_present=True, b_present=True, a_in_head=True, b_in_head=True,
                     a_head_blob="a" * 40, b_head_blob="b" * 40)
        self.assertEqual(rec["route_hint"], "review_head_divergence")

    def test_write_outputs_is_deterministic_and_limited(self):
        fixture = {
            "schema": audit.SCHEMA,
            "read_only": True,
            "comparison": {"b_only_count": 1},
            "blockers": [],
            "recommendation": {"choice": "test"},
            "records": [
                record("src/z.cpp", b_present=True),
                record("src/a.cpp", a_present=True, b_present=True, same_worktree_content=True),
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            one = Path(tmp) / "one"
            two = Path(tmp) / "two"
            audit.write_outputs(one, copy.deepcopy(fixture))
            audit.write_outputs(two, copy.deepcopy(fixture))
            self.assertEqual(sorted(p.name for p in one.iterdir()), sorted([audit.CSV_NAME, audit.JSON_NAME]))
            self.assertEqual((one / audit.CSV_NAME).read_bytes(), (two / audit.CSV_NAME).read_bytes())
            self.assertEqual((one / audit.JSON_NAME).read_bytes(), (two / audit.JSON_NAME).read_bytes())
            with (one / audit.CSV_NAME).open(encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual([row["path"] for row in rows], ["src/a.cpp", "src/z.cpp"])

    def test_summarize_a_head_provenance(self):
        records = [
            record("docs/present-in-b-worktree.md", a_present=True, a_in_head=True,
                   b_in_head=False, b_worktree_present=True),
            record("AutoTest/absent-from-b-worktree.py", a_present=True, a_in_head=True,
                   b_in_head=False, b_worktree_present=False),
        ]
        summary = audit.summarize_a_head_provenance(records)
        self.assertEqual(summary["a_head_only_count"], 2)
        self.assertEqual(summary["a_head_only_in_b_worktree_count"], 1)
        self.assertEqual(summary["a_head_only_absent_b_worktree_count"], 1)
        self.assertEqual(summary["a_head_only_in_b_worktree_paths"],
                         ["docs/present-in-b-worktree.md"])
        self.assertEqual(summary["a_head_only_absent_b_worktree_paths"],
                         ["AutoTest/absent-from-b-worktree.py"])

    def test_content_flags_detect_binary_game_asset(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "bad.mdx"
            path.write_bytes(b"MDLX" + b"\x00" * 64)
            flags = audit._content_flags(path, "bad.mdx")
            self.assertTrue(flags["game_asset"])
            self.assertIn("mdx", flags["binary_magic"])
            self.assertTrue(flags["nul_byte"])

    def test_content_flags_record_secret_pattern_not_value(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "looks_secret.txt"
            payload = b"AK" + b"IA" + (b"A" * 16)
            path.write_bytes(payload)
            flags = audit._content_flags(path, "looks_secret.txt")
            self.assertIn("aws_access_key", flags["secret_hits"])
            self.assertNotIn("value", flags)

    def test_reviewed_secret_false_positive_is_cleared(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "source.cpp"
            payload = b"tok" + b"en=" + b"'" + b"freeze before export" + b"'"
            path.write_bytes(payload)
            flags = audit._content_flags(path, "AutoTest/test_semantic_build_thread_gate_static.py")
            self.assertEqual(flags["secret_hits"], [])
            self.assertIn("generic_secret_assignment", flags["reviewed_false_positives"])

    def test_meson_product_reference_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source_dir = root / "src" / "d3d9"
            source_dir.mkdir(parents=True)
            (source_dir / "foo.cpp").write_text("", encoding="utf-8")
            (source_dir / "bar.h").write_text("", encoding="utf-8")
            (source_dir / "meson.build").write_text(
                "d3d9_src = [\n  'foo.cpp',\n  'bar.h',\n]\n", encoding="utf-8")
            refs = audit._read_meson_product_refs(
                root, {"src/d3d9/foo.cpp", "src/d3d9/bar.h"})
            self.assertEqual(sorted(refs), ["src/d3d9/bar.h", "src/d3d9/foo.cpp"])
            self.assertTrue(all(any("d3d9_src" in item for item in value)
                                for value in refs.values()))

    def test_include_dependency_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source_dir = root / "src" / "dxvk"
            source_dir.mkdir(parents=True)
            (source_dir / "guard.h").write_text("#pragma once\n", encoding="utf-8")
            (source_dir / "a.cpp").write_text('#include "guard.h"\n', encoding="utf-8")
            deps = audit._expand_product_dependencies(
                root, {"src/dxvk/a.cpp"}, {"src/dxvk/a.cpp", "src/dxvk/guard.h"})
            self.assertEqual(deps, {"src/dxvk/a.cpp", "src/dxvk/guard.h"})

    def test_path_fold_normalizes_separators_and_case(self):
        self.assertEqual(audit._path_fold("A\\B\\C"), audit._path_fold("a/b/c"))

    def test_switch_ignored_collision_overwrites(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(_git_test(root, "init", "-q", "-b", "main").returncode, 0)
            (root / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
            (root / "base.txt").write_text("base\n", encoding="utf-8")
            _git_test(root, "add", ".gitignore", "base.txt")
            _git_test(root, "commit", "-q", "-m", "base")
            _git_test(root, "branch", "target")
            _git_test(root, "switch", "-q", "target")
            (root / "ignored.txt").write_text("target content\n", encoding="utf-8")
            _git_test(root, "add", "-f", "ignored.txt")
            _git_test(root, "commit", "-q", "-m", "tracked collision")
            _git_test(root, "switch", "-q", "main")
            (root / "ignored.txt").write_text("user content\n", encoding="utf-8")
            result = _git_test(root, "switch", "target")
            self.assertEqual(result.returncode, 0)
            self.assertEqual((root / "ignored.txt").read_text(encoding="utf-8").strip(),
                             "target content")

    def test_switch_untracked_collision_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _git_test(root, "init", "-q", "-b", "main")
            (root / "base.txt").write_text("base\n", encoding="utf-8")
            _git_test(root, "add", "base.txt")
            _git_test(root, "commit", "-q", "-m", "base")
            _git_test(root, "branch", "target")
            _git_test(root, "switch", "-q", "target")
            (root / "tracked.txt").write_text("target content\n", encoding="utf-8")
            _git_test(root, "add", "tracked.txt")
            _git_test(root, "commit", "-q", "-m", "tracked collision")
            _git_test(root, "switch", "-q", "main")
            (root / "tracked.txt").write_text("user content\n", encoding="utf-8")
            result = _git_test(root, "switch", "target")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((root / "tracked.txt").read_text(encoding="utf-8").strip(),
                             "user content")

    def test_stash_include_untracked_preserves_untracked_not_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _git_test(root, "init", "-q", "-b", "main")
            (root / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
            (root / "tracked.txt").write_text("base\n", encoding="utf-8")
            _git_test(root, "add", ".gitignore", "tracked.txt")
            _git_test(root, "commit", "-q", "-m", "base")
            (root / "tracked.txt").write_text("dirty\n", encoding="utf-8")
            (root / "untracked.txt").write_text("untracked\n", encoding="utf-8")
            (root / "ignored.txt").write_text("ignored\n", encoding="utf-8")
            result = _git_test(root, "stash", "push", "--include-untracked", "-m", "r4-stash")
            self.assertEqual(result.returncode, 0)
            self.assertFalse((root / "untracked.txt").exists())
            self.assertTrue((root / "ignored.txt").exists())
            self.assertEqual((root / "ignored.txt").read_text(encoding="utf-8").strip(),
                             "ignored")
            _git_test(root, "stash", "apply", "stash@{0}")
            self.assertEqual((root / "untracked.txt").read_text(encoding="utf-8").strip(),
                             "untracked")

    def test_switch_leaves_dirty_submodule_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            remote = base / "remote.git"
            work = base / "work"
            parent = base / "parent"
            self.assertEqual(_git_test(base, "init", "--bare", "-q", str(remote)).returncode, 0)
            self.assertEqual(_git_test(base, "clone", "-q", str(remote), str(work)).returncode, 0)
            (work / "file.txt").write_text("C0\n", encoding="utf-8")
            _git_test(work, "add", "file.txt")
            _git_test(work, "commit", "-q", "-m", "C0")
            _git_test(work, "push", "-q", "origin", "HEAD:refs/heads/master")
            self.assertEqual(_git_test(base, "init", "-q", "-b", "main", str(parent)).returncode, 0)
            (parent / "base.txt").write_text("base\n", encoding="utf-8")
            _git_test(parent, "add", "base.txt")
            _git_test(parent, "commit", "-q", "-m", "base")
            add = _git_test(parent, "submodule", "add", "-q", str(remote), "sub")
            self.assertEqual(add.returncode, 0, add.stderr)
            _git_test(parent, "commit", "-q", "-m", "add submodule")
            _git_test(parent, "branch", "target")
            (parent / "sub" / "dirty.txt").write_text("dirty\n", encoding="utf-8")
            before = _git_test(parent, "status", "--porcelain=v1",
                               "--untracked-files=all").stdout
            self.assertIn("sub", before)
            switch = _git_test(parent, "switch", "target")
            self.assertEqual(switch.returncode, 0)
            self.assertTrue((parent / "sub" / "dirty.txt").exists())
            self.assertIn("sub", _git_test(parent, "status", "--porcelain=v1",
                                            "--untracked-files=all").stdout)
            stash = _git_test(parent, "stash", "push", "--include-untracked",
                              "-m", "r4-submodule")
            self.assertEqual(stash.returncode, 0)
            self.assertIn("No local changes to save", stash.stdout)
            self.assertTrue((parent / "sub" / "dirty.txt").exists())


if __name__ == "__main__":
    unittest.main()
