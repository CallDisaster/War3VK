#!/usr/bin/env python3
"""Deterministic, read-only A/B tree integration audit for WarVK v1.22.

The utility never mutates Git state.  It reads two worktrees and writes only
JSON/CSV files under --output-dir.  It is intentionally conservative: paths
that do not match a source/test/doc/keep rule are surfaced for review instead
of being silently merged or deleted.
"""
from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import os
import posixpath
import re
import stat
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

SCHEMA = "warvk-v122-tree-integration-audit/v1"
GITLINK_MODE = "160000"
DEFAULT_A = r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk"
DEFAULT_B = r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914"
DEFAULT_OUT = r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914\AutoTest\artifacts\release-closeout-20260920\integration-audit"
CSV_NAME = "v122-tree-integration-files.csv"
JSON_NAME = "v122-tree-integration-audit.json"
SEMANTIC_CSV_NAME = "r4-b-checkpoint-semantic-review.csv"
SEMANTIC_JSON_NAME = "r4-b-checkpoint-semantic-review.json"
TRANSITION_JSON_NAME = "r4-transition-hazard-manifest.json"

SOURCE_PREFIXES = (
    "src/", "AutoTest/", "docs/", "subprojects/", "tools/", "WarVK/",
    "include/", "shaders/", "smaa/", "LICENSES/", "build-options/",
)
GENERATED_PATTERNS = (
    re.compile(r"(^|/)__pycache__(/|$)"),
    re.compile(r"(^|/)\.pytest_cache(/|$)"),
    re.compile(r"(^|/)\.mypy_cache(/|$)"),
    re.compile(r"^build(?:32|64|-[^/]*)?(/|$)"),
    re.compile(r"^build-options(/|$)"),
    re.compile(r"^(?:[^/]+/)*[^/]+\.(?:log|out|err)$"),
    re.compile(r"^(?:[^/]+/)*[^/]+\.(?:pyc|pyo|obj|pdb|ilk|exp|lib|dll|exe|dmp|etl)$"),
    re.compile(r"^d3d9\.dll(?:\..*)?$"),
)
EVIDENCE_PATTERNS = (
    re.compile(r"^AutoTest/artifacts/"),
    re.compile(r"^PlayerCrash/"),
    re.compile(r"^build32/"),
    re.compile(r"^build-options/"),
    re.compile(r"^d3d9\.dll(?:\..*)?$"),
)
UNSAFE_PATH = re.compile(r"(^/)|(^[A-Za-z]:)|(^|[\\/])\.\.([\\/]|$)")

MESON_PRODUCT_LISTS = {
    "src/d3d9/meson.build": ("src/d3d9", ("d3d9_src", "d3d9_shaders")),
    "src/dxvk/meson.build": ("src/dxvk", ("dxvk_src", "dxvk_shaders")),
    "subprojects/war3fx/meson.build": ("subprojects/war3fx", ("war3fx_shaders",)),
}

BINARY_MAGICS = (
    (b"\x00", "nul_byte"),
    (b"MZ", "dos_pe"),
    (b"\x7fELF", "elf"),
    (b"\x89PNG\r\n\x1a\n", "png"),
    (b"\xff\xd8\xff", "jpeg"),
    (b"PK\x03\x04", "zip"),
    (b"7z\xbc\xaf\x27\x1c", "7z"),
    (b"Rar!\x1a\x07", "rar"),
    (b"MDLX", "mdx"),
    (b"BLP1", "blp1"),
    (b"BLP2", "blp2"),
    (b"DDS ", "dds"),
    (b"RIFF", "riff"),
    (b"ID3", "mp3_id3"),
    (b"OggS", "ogg"),
    (b"%PDF-", "pdf"),
    (b"SQLite format 3\x00", "sqlite"),
    (b"MDMP", "minidump"),
    (b"MSCF", "cab"),
)

GAME_ASSET_EXTENSIONS = {
    ".mdx", ".mdl", ".blp", ".dds", ".tga", ".w3x", ".w3m", ".mpq",
    ".slk", ".wav", ".mp3", ".ogg", ".skin", ".anim",
}

RAW_EVIDENCE_EXTENSIONS = {
    ".log", ".dmp", ".etl", ".zip", ".7z", ".rar", ".png", ".jpg", ".jpeg",
    ".bmp", ".gif", ".exe", ".dll", ".asi", ".pdb", ".obj", ".lib", ".ilk", ".exp",
}

SECRET_PATTERNS = (
    ("private_key", re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("aws_access_key", re.compile(rb"AKIA[0-9A-Z]{16}")),
    ("github_token", re.compile(rb"gh[pousr]_[A-Za-z0-9_]{20,}")),
    ("slack_token", re.compile(rb"xox[baprs]-[A-Za-z0-9-]{10,}")),
    ("openai_key", re.compile(rb"sk-[A-Za-z0-9]{20,}")),
    ("generic_secret_assignment", re.compile(
        rb"(?i)(api[_-]?key|secret|password|passwd|token)\s*[:=]\s*['\"]([^'\"]{8,})['\"]")),
)

REVIEWED_SECRET_FALSE_POSITIVES = {
    ("src/d3d9/war3/tools/war3_frame_evidence.cpp", "generic_secret_assignment"):
        "local C++ token comparison expression; manually reviewed, no credential literal",
    ("AutoTest/live_contrast_palette_objects.py", "generic_secret_assignment"):
        "diagnostic label followed by a local expression; manually reviewed, no credential literal",
    ("AutoTest/test_semantic_build_thread_gate_static.py", "generic_secret_assignment"):
        "function parameter default string literal; manually reviewed, no credential literal",
}

class AuditError(RuntimeError):
    pass

def _norm_path(value: Any) -> str:
    return str(value).replace("\\", "/")

def _safe_rel(path: str) -> str:
    if not path or path in (".", "..") or UNSAFE_PATH.search(path):
        raise AuditError("unsafe or empty repository-relative path: %r" % (path,))
    return path.replace("\\", "/")

def _run_git(root: os.PathLike[str] | str, *args: str, check: bool = True, timeout: int = 180) -> subprocess.CompletedProcess:
    cmd = ["git", "-C", os.fspath(root), *args]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    except OSError as exc:
        raise AuditError("failed to execute %r: %s" % (cmd, exc)) from exc
    if check and proc.returncode != 0:
        err = proc.stderr.decode("utf-8", "surrogateescape").strip()
        raise AuditError("git %s failed with rc=%d: %s" % (" ".join(args), proc.returncode, err))
    return proc

def _git_text(root: os.PathLike[str] | str, *args: str, check: bool = True) -> str:
    return _run_git(root, *args, check=check).stdout.decode("utf-8", "surrogateescape")

def _git_z(root: os.PathLike[str] | str, *args: str) -> List[str]:
    raw = _run_git(root, *args).stdout
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]

def _sanitize_url(url: str) -> str:
    if not url:
        return ""
    url = url.strip()
    if "://" not in url:
        return url.split("@", 1)[1] if "@" in url else url
    try:
        from urllib.parse import urlsplit, urlunsplit
        parts = urlsplit(url)
        host = parts.hostname or ""
        if parts.port:
            host = "%s:%s" % (host, parts.port)
        query = re.sub(r"([?&](?:token|access_token|key|password|pwd|secret)=)[^&]+", r"\1***", parts.query, flags=re.I)
        return urlunsplit((parts.scheme, host, parts.path, "", query))
    except Exception:
        return re.sub(r"^([a-zA-Z][a-zA-Z0-9+.-]*://)[^/@]+@", r"\1", url)

def _read_inventory(root: os.PathLike[str] | str) -> Tuple[set, set, set]:
    tracked = set(_git_z(root, "ls-files", "-z"))
    untracked = set(_git_z(root, "ls-files", "-z", "--others", "--exclude-standard"))
    return tracked | untracked, tracked, untracked

def _read_status(root: os.PathLike[str] | str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for rec in _git_z(root, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--no-renames"):
        if len(rec) >= 4:
            result[rec[3:]] = rec[:2]
    return result

def _read_index(root: os.PathLike[str] | str) -> Dict[str, Dict[str, str]]:
    result: Dict[str, Dict[str, str]] = {}
    for rec in _git_z(root, "ls-files", "-s", "-z"):
        try:
            meta, path = rec.split("\t", 1)
            mode, oid, stage = meta.split()
        except ValueError as exc:
            raise AuditError("unexpected ls-files -s record: %r" % (rec,)) from exc
        result[_safe_rel(path)] = {"mode": mode, "oid": oid, "stage": stage}
    return result

def _read_head_tree(root: os.PathLike[str] | str) -> Dict[str, Dict[str, str]]:
    result: Dict[str, Dict[str, str]] = {}
    for rec in _git_z(root, "ls-tree", "-r", "-z", "HEAD"):
        try:
            meta, path = rec.split("\t", 1)
            mode, typ, oid = meta.split()
        except ValueError as exc:
            raise AuditError("unexpected ls-tree record: %r" % (rec,)) from exc
        result[_safe_rel(path)] = {"mode": mode, "type": typ, "oid": oid}
    return result

def _read_worktrees(root: os.PathLike[str] | str) -> List[Dict[str, str]]:
    text = _git_text(root, "worktree", "list", "--porcelain", check=False)
    items: List[Dict[str, str]] = []
    current: Dict[str, str] = {}
    for line in text.splitlines() + [""]:
        if not line.strip():
            if current:
                items.append(current)
                current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value.strip() if value else "1"
    return sorted(items, key=lambda item: item.get("worktree", ""))

def _read_refs(root: os.PathLike[str] | str) -> List[Dict[str, str]]:
    text = _git_text(root, "for-each-ref", "--format=%(refname)%09%(objectname)%09%(objecttype)", check=False)
    refs: List[Dict[str, str]] = []
    for line in text.splitlines():
        if not line:
            continue
        parts = line.split("\t")
        if len(parts) >= 3:
            refs.append({"name": parts[0], "object": parts[1], "type": parts[2]})
    return sorted(refs, key=lambda item: item["name"])

def _read_remotes(root: os.PathLike[str] | str) -> List[Dict[str, str]]:
    names = _git_text(root, "remote", check=False).splitlines()
    result = []
    for name in sorted(n for n in names if n):
        url = _git_text(root, "remote", "get-url", name, check=False).strip()
        result.append({"name": name, "url": _sanitize_url(url)})
    return result

def _read_gitmodules(root: os.PathLike[str] | str) -> Dict[str, Dict[str, str]]:
    path = Path(root) / ".gitmodules"
    if not path.is_file():
        return {}
    parser = configparser.ConfigParser(interpolation=None)
    parser.read_string(path.read_text(encoding="utf-8-sig"))
    result: Dict[str, Dict[str, str]] = {}
    for section in parser.sections():
        sub = parser[section]
        subpath = sub.get("path", "")
        if subpath:
            result[_safe_rel(subpath)] = {
                "section": section,
                "url": _sanitize_url(sub.get("url", "")),
            }
    return result

def _read_submodules(root: os.PathLike[str] | str, index: Dict[str, Dict[str, str]]) -> Dict[str, Dict[str, str]]:
    result: Dict[str, Dict[str, str]] = {}
    proc = _run_git(root, "submodule", "status", "--recursive", check=False)
    if proc.returncode != 0:
        result["__error__"] = {"line": proc.stderr.decode("utf-8", "surrogateescape").strip()}
    for line in proc.stdout.decode("utf-8", "surrogateescape").splitlines():
        if not line:
            continue
        flag = line[0] if line[0] in " -+U" else " "
        body = line[1:] if line[0] in " -+U" else line
        if len(body) < 41:
            continue
        oid = body[:40]
        rest = body[41:].strip()
        if " (" in rest:
            subpath, desc = rest.rsplit(" (", 1)
            desc = desc[:-1] if desc.endswith(")") else desc
        else:
            subpath, desc = rest, ""
        result[_safe_rel(subpath)] = {"flag": flag.strip() or " ", "worktree_head": oid, "describe": desc}
    for path, meta in index.items():
        if meta.get("mode") != GITLINK_MODE:
            continue
        result.setdefault(path, {})["index_oid"] = meta.get("oid", "")
    return result

def _hash_bytes(raw: bytes) -> Dict[str, Any]:
    git = hashlib.sha1()
    git.update(("blob %d\0" % len(raw)).encode("ascii"))
    git.update(raw)
    return {"size": len(raw), "git_blob_sha1": git.hexdigest(), "sha256": hashlib.sha256(raw).hexdigest()}

def _hash_file(path: Path) -> Dict[str, Any]:
    try:
        st = path.lstat()
    except OSError:
        return {}
    if stat.S_ISLNK(st.st_mode):
        try:
            return _hash_bytes(os.fsencode(os.readlink(path)))
        except OSError:
            return {}
    if not stat.S_ISREG(st.st_mode):
        return {}
    sha1 = hashlib.sha1()
    sha1.update(("blob %d\0" % st.st_size).encode("ascii"))
    sha256 = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                sha1.update(chunk)
                sha256.update(chunk)
    except OSError:
        return {}
    return {"size": st.st_size, "git_blob_sha1": sha1.hexdigest(), "sha256": sha256.hexdigest()}

def _content_flags(path: Path, rel_path: str) -> Dict[str, Any]:
    """Read bounded content flags without returning secret-bearing text."""
    flags: Dict[str, Any] = {
        "exists": path.exists(),
        "regular": False,
        "symlink": False,
        "size": None,
        "sha256": None,
        "git_blob_sha1": None,
        "nul_byte": False,
        "binary_magic": [],
        "game_asset": False,
        "raw_evidence": False,
        "secret_hits": [],
        "reviewed_false_positives": [],
    }
    try:
        st = path.lstat()
    except OSError:
        return flags
    flags["size"] = st.st_size
    flags["symlink"] = stat.S_ISLNK(st.st_mode)
    flags["regular"] = stat.S_ISREG(st.st_mode)
    if not stat.S_ISREG(st.st_mode):
        return flags
    try:
        raw = path.read_bytes()
    except OSError:
        return flags
    sha1 = hashlib.sha1()
    sha1.update(("blob %d\0" % len(raw)).encode("ascii"))
    sha1.update(raw)
    flags["sha256"] = hashlib.sha256(raw).hexdigest()
    flags["git_blob_sha1"] = sha1.hexdigest()
    head = raw[:8192]
    if b"\x00" in head:
        flags["nul_byte"] = True
    for magic, label in BINARY_MAGICS:
        if head.startswith(magic):
            flags["binary_magic"].append(label)
    suffix = Path(rel_path).suffix.lower()
    if suffix in GAME_ASSET_EXTENSIONS:
        flags["game_asset"] = True
    if suffix in RAW_EVIDENCE_EXTENSIONS:
        flags["raw_evidence"] = True
    for label, pattern in SECRET_PATTERNS:
        if not pattern.search(raw):
            continue
        if (rel_path, label) in REVIEWED_SECRET_FALSE_POSITIVES:
            flags["reviewed_false_positives"].append(label)
        else:
            flags["secret_hits"].append(label)
    return flags


def _read_proposal_rows(allowlist_csv: Path, root_owned_csv: Path) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for csv_path, decision in ((Path(allowlist_csv), "b_checkpoint_allowlist"),
                               (Path(root_owned_csv), "root_owned_allowlist")):
        if not csv_path.is_file():
            continue
        with csv_path.open(encoding="utf-8-sig", newline="") as handle:
            for row in csv.DictReader(handle):
                rows.append({
                    "path": _safe_rel(row["path"]),
                    "proposal_decision": decision,
                    "b_status": row.get("b_status", ""),
                    "b_tracked": row.get("b_tracked", ""),
                    "reason": row.get("reason", ""),
                })
    return sorted(rows, key=lambda item: item["path"])


def _path_fold(path: str) -> str:
    return _norm_path(path).casefold()


def _meson_list_items(lines: Sequence[str], wanted: Iterable[str]) -> Dict[str, List[str]]:
    result: Dict[str, List[str]] = {name: [] for name in wanted}
    index = 0
    while index < len(lines):
        match = re.match(r"\s*([A-Za-z0-9_]+)\s*(?:\+?=)\s*(?:files\s*\(\s*)?\[", lines[index])
        if match and match.group(1) in result:
            name = match.group(1)
            index += 1
            while index < len(lines) and not re.match(r"^\s*\]", lines[index]):
                for quoted in re.finditer(r"'([^']+)'", lines[index]):
                    result[name].append(quoted.group(1))
                index += 1
        index += 1
    return result


def _resolve_repo_path(root: Path, basedir: str, item: str) -> Optional[str]:
    if not item or os.path.isabs(item):
        return None
    candidate = (root / basedir / item).resolve()
    try:
        return candidate.relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def _read_meson_product_refs(root: Path, candidates: Iterable[str]) -> Dict[str, List[str]]:
    wanted = set(candidates)
    result: Dict[str, List[str]] = {}
    root = Path(root)
    for meson_rel, (basedir, variables) in MESON_PRODUCT_LISTS.items():
        meson_path = root / meson_rel
        if not meson_path.is_file():
            continue
        lines = meson_path.read_text(encoding="utf-8-sig").splitlines()
        for variable, items in _meson_list_items(lines, variables).items():
            for item in items:
                rel = _resolve_repo_path(root, basedir, item)
                if rel in wanted:
                    result.setdefault(rel, []).append("%s:%s" % (meson_rel, variable))
    return result


def _expand_product_dependencies(root: Path, direct: Iterable[str], candidates: Iterable[str]) -> set:
    root = Path(root)
    wanted = set(candidates)
    deps = set(direct)
    by_base: Dict[str, List[str]] = {}
    for rel in wanted:
        by_base.setdefault(posixpath.basename(rel), []).append(rel)
    changed = True
    while changed:
        changed = False
        for rel in sorted(deps):
            file_path = root / rel
            if not file_path.is_file():
                continue
            try:
                head = file_path.read_bytes()[:4096]
            except OSError:
                continue
            if b"\x00" in head:
                continue
            text = head.decode("utf-8", "ignore")
            for match in re.finditer(r"#\s*include\s+[<\"]([^\">]+)[\">]", text):
                include = match.group(1).replace("\\", "/")
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(rel), include))
                found = set()
                if resolved in wanted:
                    found.add(resolved)
                found.update(by_base.get(posixpath.basename(include), []))
                for item in found:
                    if item not in deps:
                        deps.add(item)
                        changed = True
    return deps


def _looks_source(path: str) -> bool:
    return any(path == prefix or path.startswith(prefix) for prefix in SOURCE_PREFIXES)

def _is_evidence(path: str) -> bool:
    return any(pattern.search(path) for pattern in EVIDENCE_PATTERNS)

def _is_generated(path: str) -> bool:
    return any(pattern.search(path) for pattern in GENERATED_PATTERNS)

def classify_record(rec: Dict[str, Any]) -> Dict[str, Any]:
    path = rec["path"]
    if rec.get("kind") == "gitlink":
        return {"route_hint": "submodule_review", "integration_action": "do_not_integrate_submodule", "exclusion_reason": "gitlink_requires_separate_submodule_transaction", "manual_review": True, "reason": "submodule_or_gitlink"}
    if _is_evidence(path):
        return {"route_hint": "preserve_external", "integration_action": "do_not_integrate", "exclusion_reason": "evidence_or_binary_must_be_preserved_outside_git", "manual_review": True, "reason": "evidence_or_binary"}
    if _is_generated(path):
        return {"route_hint": "exclude_generated", "integration_action": "do_not_integrate", "exclusion_reason": "generated_or_local_runtime_output", "manual_review": False, "reason": "generated_output"}
    a_any = bool(rec.get("a_worktree_present") or rec.get("a_in_index") or rec.get("a_in_head"))
    b_any = bool(rec.get("b_worktree_present") or rec.get("b_in_index") or rec.get("b_in_head"))
    a_dirty = rec.get("a_status") is not None
    b_dirty = rec.get("b_status") is not None
    if not a_any and b_any:
        return {"route_hint": "keep_b", "integration_action": "integrate_b_authoritative", "exclusion_reason": "", "manual_review": False, "reason": "b_only_or_b_authoritative"}
    if a_any and not b_any:
        if _looks_source(path):
            return {"route_hint": "review_a_unique", "integration_action": "transplant_after_review", "exclusion_reason": "", "manual_review": True, "reason": "a_only_source_test_or_doc"}
        return {"route_hint": "review_a_local", "integration_action": "do_not_integrate", "exclusion_reason": "a_only_local_or_non_source", "manual_review": True, "reason": "a_only_local"}
    if a_any and b_any:
        if rec.get("same_worktree_content"):
            return {"route_hint": "identical", "integration_action": "no_change", "exclusion_reason": "", "manual_review": False, "reason": "same_worktree_content"}
        if a_dirty and b_dirty:
            return {"route_hint": "review_conflict", "integration_action": "manual_merge_review", "exclusion_reason": "", "manual_review": True, "reason": "both_status_dirty_and_content_differs"}
        if a_dirty:
            return {"route_hint": "review_a_change", "integration_action": "transplant_after_review", "exclusion_reason": "", "manual_review": True, "reason": "a_dirty_b_clean_content_differs"}
        if b_dirty:
            return {"route_hint": "keep_b", "integration_action": "integrate_b_authoritative", "exclusion_reason": "", "manual_review": False, "reason": "b_dirty_a_clean"}
        if rec.get("a_head_blob") != rec.get("b_head_blob"):
            return {"route_hint": "review_head_divergence", "integration_action": "manual_merge_review", "exclusion_reason": "", "manual_review": True, "reason": "committed_trees_diverge"}
        return {"route_hint": "identical", "integration_action": "no_change", "exclusion_reason": "", "manual_review": False, "reason": "same_index_or_head"}
    return {"route_hint": "deleted_both", "integration_action": "no_change", "exclusion_reason": "absent_both_worktrees", "manual_review": False, "reason": "absent_both"}

def _record_for_path(path: str, a: Dict[str, Any], b: Dict[str, Any]) -> Dict[str, Any]:
    path = _safe_rel(path)
    a_idx = a["index"].get(path)
    b_idx = b["index"].get(path)
    a_head = a["head_tree"].get(path)
    b_head = b["head_tree"].get(path)
    kind = "gitlink" if ((a_idx and a_idx.get("mode") == GITLINK_MODE) or (b_idx and b_idx.get("mode") == GITLINK_MODE)) else "file"
    if kind == "gitlink":
        a_wt = {"present": os.path.isdir(os.path.join(a["root"], path.replace("/", os.sep)))}
        b_wt = {"present": os.path.isdir(os.path.join(b["root"], path.replace("/", os.sep)))}
        a_wt_present = bool(a_wt["present"])
        b_wt_present = bool(b_wt["present"])
        a_blob = b_blob = None
        a_sha = b_sha = None
        a_size = b_size = None
    else:
        a_wt = _hash_file(Path(a["root"]) / path.replace("/", os.sep))
        b_wt = _hash_file(Path(b["root"]) / path.replace("/", os.sep))
        a_wt_present = bool(a_wt)
        b_wt_present = bool(b_wt)
        a_blob = a_wt.get("git_blob_sha1")
        b_blob = b_wt.get("git_blob_sha1")
        a_sha = a_wt.get("sha256")
        b_sha = b_wt.get("sha256")
        a_size = a_wt.get("size")
        b_size = b_wt.get("size")
    rec: Dict[str, Any] = {
        "path": path,
        "kind": kind,
        "a_present": bool(a_wt_present or a_idx or a_head),
        "b_present": bool(b_wt_present or b_idx or b_head),
        "a_tracked": bool(a_idx),
        "b_tracked": bool(b_idx),
        "a_in_index": bool(a_idx),
        "b_in_index": bool(b_idx),
        "a_in_head": bool(a_head),
        "b_in_head": bool(b_head),
        "a_index_mode": a_idx.get("mode") if a_idx else None,
        "b_index_mode": b_idx.get("mode") if b_idx else None,
        "a_head_blob": a_head.get("oid") if a_head else None,
        "b_head_blob": b_head.get("oid") if b_head else None,
        "a_index_blob": a_idx.get("oid") if a_idx else None,
        "b_index_blob": b_idx.get("oid") if b_idx else None,
        "a_worktree_present": a_wt_present,
        "b_worktree_present": b_wt_present,
        "a_worktree_blob": a_blob,
        "b_worktree_blob": b_blob,
        "a_sha256": a_sha,
        "b_sha256": b_sha,
        "a_size": a_size,
        "b_size": b_size,
        "a_status": a["status"].get(path),
        "b_status": b["status"].get(path),
        "same_worktree_content": bool(a_blob is not None and b_blob is not None and a_blob == b_blob),
        "a_equals_a_head": bool(a_blob is not None and a_head is not None and a_blob == a_head.get("oid")),
        "b_equals_b_head": bool(b_blob is not None and b_head is not None and b_blob == b_head.get("oid")),
        "a_equals_b_head": bool(a_blob is not None and b_head is not None and a_blob == b_head.get("oid")),
        "b_equals_a_head": bool(b_blob is not None and a_head is not None and b_blob == a_head.get("oid")),
    }
    rec.update(classify_record(rec))
    return rec

def _read_state(root: Path) -> Dict[str, Any]:
    root = Path(root)
    top = _git_text(root, "rev-parse", "--show-toplevel", check=False).strip()
    if not top:
        raise AuditError("not a git worktree: %s" % root)
    head = _git_text(root, "rev-parse", "HEAD").strip()
    branch = _git_text(root, "symbolic-ref", "-q", "HEAD", check=False).strip() or None
    git_dir = _git_text(root, "rev-parse", "--git-dir", check=False).strip()
    common_dir = _git_text(root, "rev-parse", "--git-common-dir", check=False).strip()
    if git_dir and not os.path.isabs(git_dir):
        git_dir = os.path.normpath(os.path.join(root, git_dir))
    if common_dir and not os.path.isabs(common_dir):
        common_dir = os.path.normpath(os.path.join(root, common_dir))
    status = _read_status(root)
    index = _read_index(root)
    head_tree = _read_head_tree(root)
    inventory, tracked, untracked = _read_inventory(root)
    ignored_proc = _run_git(root, "ls-files", "-z", "--others", "--ignored", "--exclude-standard", check=False)
    if ignored_proc.returncode == 0:
        ignored_count = len([x for x in ignored_proc.stdout.split(b"\0") if x])
        ignored_error = ""
    else:
        ignored_count = -1
        ignored_error = ignored_proc.stderr.decode("utf-8", "surrogateescape").strip()
    gitmodules = _read_gitmodules(root)
    submodules = _read_submodules(root, index)
    return {
        "root": _norm_path(root.resolve()),
        "toplevel": _norm_path(top),
        "head": head,
        "head_short": head[:12],
        "branch": branch,
        "git_dir": _norm_path(git_dir),
        "common_dir": _norm_path(common_dir),
        "status": status,
        "index": index,
        "head_tree": head_tree,
        "inventory": inventory,
        "tracked": tracked,
        "untracked": untracked,
        "ignored_count": ignored_count,
        "ignored_error": ignored_error,
        "worktrees": _read_worktrees(root),
        "refs": _read_refs(root),
        "remotes": _read_remotes(root),
        "gitmodules": gitmodules,
        "submodules": submodules,
    }

def summarize_a_head_provenance(records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    """Distinguish A HEAD-only paths from paths absent from B's worktree.

    The r1 wording treated all A-HEAD-only paths as absent from B.  That is
    not true when B already carries the same path as an untracked file.  This
    helper makes the provenance explicit for reports and tests.
    """
    head_only = [r for r in records if r.get("a_in_head") and not r.get("b_in_head")]
    in_b_worktree = sorted(r["path"] for r in head_only if r.get("b_worktree_present"))
    absent_b_worktree = sorted(r["path"] for r in head_only if not r.get("b_worktree_present"))
    return {
        "a_head_only_count": len(head_only),
        "a_head_only_in_b_worktree_count": len(in_b_worktree),
        "a_head_only_absent_b_worktree_count": len(absent_b_worktree),
        "a_head_only_in_b_worktree_paths": in_b_worktree,
        "a_head_only_absent_b_worktree_paths": absent_b_worktree,
    }


def _summarize(a: Dict[str, Any], b: Dict[str, Any], records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    route_counts = Counter(record["route_hint"] for record in records)
    kind_counts = Counter(record["kind"] for record in records)
    conflict_paths = [record["path"] for record in records if record["route_hint"] == "review_conflict"]
    a_unique = [record["path"] for record in records if record["route_hint"] == "review_a_unique"]
    a_local = [record["path"] for record in records if record["route_hint"] == "review_a_local"]
    a_only = [record["path"] for record in records if record["a_present"] and not record["b_present"]]
    b_only = [record["path"] for record in records if record["b_present"] and not record["a_present"]]
    common = [record for record in records if record["a_present"] and record["b_present"]]
    a_dirty_tracked = [record["path"] for record in records if record["a_status"] is not None and record["a_status"] != "??"]
    b_dirty_tracked = [record["path"] for record in records if record["b_status"] is not None and record["b_status"] != "??"]
    a_untracked = [record["path"] for record in records if record["a_status"] == "??"]
    b_untracked = [record["path"] for record in records if record["b_status"] == "??"]
    both_dirty = [record["path"] for record in records if record["a_status"] is not None and record["b_status"] is not None]
    same_content = [record["path"] for record in common if record["same_worktree_content"]]
    a_head_only = [record["path"] for record in records if record["a_in_head"] and not record["b_in_head"]]
    a_worktree_only = [record["path"] for record in records if record["a_worktree_present"] and not record["b_worktree_present"]]
    b_head_only = [record["path"] for record in records if record["b_in_head"] and not record["a_in_head"]]
    provenance = summarize_a_head_provenance(records)
    top_dir_counts = Counter(record["path"].split("/", 1)[0] if "/" in record["path"] else record["path"] for record in records)
    return {
        "a_inventory_count": len(a["inventory"]),
        "b_inventory_count": len(b["inventory"]),
        "union_inventory_count": len(records),
        "a_only_count": len(a_only),
        "a_worktree_only_count": len(a_worktree_only),
        "b_only_count": len(b_only),
        "common_count": len(common),
        "identical_worktree_count": len(same_content),
        "a_status_count": len(a["status"]),
        "b_status_count": len(b["status"]),
        "a_dirty_tracked_count": len(a_dirty_tracked),
        "b_dirty_tracked_count": len(b_dirty_tracked),
        "a_untracked_count": len(a_untracked),
        "b_untracked_count": len(b_untracked),
        "a_ignored_count": a.get("ignored_count", -1),
        "b_ignored_count": b.get("ignored_count", -1),
        "common_dirty_count": len(both_dirty),
        "conflicting_dirty_count": len(conflict_paths),
        "a_only_committed_count": len(a_head_only),
        "a_head_only_count": provenance["a_head_only_count"],
        "a_head_only_in_b_worktree_count": provenance["a_head_only_in_b_worktree_count"],
        "a_head_only_absent_b_worktree_count": provenance["a_head_only_absent_b_worktree_count"],
        "b_only_committed_count": len(b_head_only),
        "route_counts": {key: route_counts[key] for key in sorted(route_counts)},
        "kind_counts": {key: kind_counts[key] for key in sorted(kind_counts)},
        "top_dir_counts": {key: top_dir_counts[key] for key in sorted(top_dir_counts)},
        "a_only_paths": sorted(a_only),
        "b_only_paths_sample": sorted(b_only)[:200],
        "conflict_paths": sorted(conflict_paths),
        "a_unique_review_paths": sorted(a_unique),
        "a_local_review_paths": sorted(a_local),
        "a_head_only_paths": sorted(a_head_only),
        "a_head_only_in_b_worktree_paths": provenance["a_head_only_in_b_worktree_paths"],
        "a_head_only_absent_b_worktree_paths": provenance["a_head_only_absent_b_worktree_paths"],
        "a_worktree_only_paths": sorted(a_worktree_only),
        "b_head_only_paths_sample": sorted(b_head_only)[:200],
        "a_dirty_tracked_paths": sorted(a_dirty_tracked),
        "b_dirty_tracked_paths_sample": sorted(b_dirty_tracked)[:200],
        "a_untracked_paths": sorted(a_untracked),
        "b_untracked_paths_sample": sorted(b_untracked)[:200],
        "same_worktree_paths_sample": sorted(same_content)[:200],
    }

def _build_blockers(a: Dict[str, Any], b: Dict[str, Any], summary: Dict[str, Any], records: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    merge_base = ""
    try:
        merge_base = _git_text(Path(a["root"]), "merge-base", a["head"], b["head"]).strip()
    except AuditError:
        merge_base = ""
    a_unique_commits = -1
    b_unique_commits = -1
    try:
        counts = _git_text(Path(b["root"]), "rev-list", "--left-right", "--count", a["head"] + "..." + b["head"]).split()
        if len(counts) == 2:
            a_unique_commits, b_unique_commits = int(counts[0]), int(counts[1])
    except (AuditError, ValueError):
        pass
    blockers: List[Dict[str, Any]] = []
    blockers.append({
        "id": "BLOCK-001-history-divergence",
        "severity": "blocker",
        "title": "A and B histories diverged; B is not a descendant of A",
        "evidence": {
            "a_head": a["head"], "b_head": b["head"], "merge_base": merge_base,
            "a_unique_commits": a_unique_commits, "b_unique_commits": b_unique_commits,
        },
        "impact": "A wholesale fast-forward/switch cannot preserve A's unique commit; any ancestry merge needs reviewed conflict resolution.",
        "required_decision": "Use file-reviewed transplant into a B-derived integration branch, or explicitly authorize a reviewed merge commit.",
    })
    blockers.append({
        "id": "BLOCK-002-conflicting-dirty-files",
        "severity": "blocker",
        "title": "Both worktrees contain divergent dirty content",
        "evidence": {
            "conflicting_paths_count": summary["conflicting_dirty_count"],
            "paths": summary["conflict_paths"],
        },
        "impact": "Automatic ours/theirs merge would discard unaudited A or B work.",
        "required_decision": "Resolve all conflict paths file-by-file, default B-primary, and record accepted A hunk hashes.",
    })
    blockers.append({
        "id": "BLOCK-003-a-unique-dirty-and-assets",
        "severity": "blocker",
        "title": "A HEAD-only / A-worktree-only provenance and local assets need explicit disposition",
        "evidence": {
            "a_dirty_tracked_paths": summary["a_dirty_tracked_paths"],
            "a_unique_review_count": len(summary["a_unique_review_paths"]),
            "a_unique_review_sample": summary["a_unique_review_paths"][:100],
            "a_local_review_count": len(summary["a_local_review_paths"]),
            "a_local_review_sample": summary["a_local_review_paths"][:100],
            "a_head_only_count": summary["a_head_only_count"],
            "a_head_only_in_b_worktree_count": summary["a_head_only_in_b_worktree_count"],
            "a_head_only_in_b_worktree_paths": summary["a_head_only_in_b_worktree_paths"],
            "a_head_only_absent_b_worktree_count": summary["a_head_only_absent_b_worktree_count"],
            "a_head_only_absent_b_worktree_paths": summary["a_head_only_absent_b_worktree_paths"],
            "a_worktree_only_count": summary["a_worktree_only_count"],
            "a_untracked_count": summary["a_untracked_count"],
        },
        "impact": "A local evidence, user assets, tests, or dirty source could be lost if the main tree is switched or reset.",
        "required_decision": "Checkpoint/archive A unique trackable changes and export untracked/evidence manifests before any main-tree switch; treat A-HEAD-only paths present in B worktree as already-provenanced B local files, not as missing from B.",
    })
    blockers.append({
        "id": "BLOCK-004-submodule-local-state",
        "severity": "blocker",
        "title": "Submodule local state differs from index/B",
        "evidence": {
            "a_submodules": a.get("submodules", {}),
            "b_submodules": b.get("submodules", {}),
            "a_gitmodules_count": len(a.get("gitmodules", {})),
            "b_gitmodules_count": len(b.get("gitmodules", {})),
        },
        "impact": "A submodule worktree commit/dirty state is outside the superproject file transplant and must be preserved separately.",
        "required_decision": "Keep B index gitlinks authoritative; archive A submodule diff/untracked files and do not update gitlinks in this transaction.",
    })
    blockers.append({
        "id": "BLOCK-005-b-dirty-not-checkpointed",
        "severity": "blocker",
        "title": "B v1.22 source is not represented by B HEAD",
        "evidence": {
            "b_status_count": summary["b_status_count"],
            "b_dirty_tracked_count": summary["b_dirty_tracked_count"],
            "b_untracked_count": summary["b_untracked_count"],
            "b_head": b["head"],
        },
        "impact": "Merging B's current branch alone would not include B's uncommitted v1.22 work.",
        "required_decision": "Create reviewed local checkpoint commit(s) for B source/test/docs changes; leave generated evidence and local binaries outside history.",
    })
    blockers.append({
        "id": "BLOCK-006-historical-inventory",
        "severity": "warning",
        "title": "2026-09-16 merge inventory is historical",
        "evidence": {"historical_path": "docs/plan/2026-09-16-tree-merge-diff-inventory.md"},
        "impact": "Using the old inventory as current would miss 2026-09-20 changes and dirty divergence.",
        "required_decision": "Use this batch's generated JSON/CSV and the current audit run as the only current inventory.",
    })
    blockers.append({
        "id": "BLOCK-007-player-dll-and-release-proof",
        "severity": "warning",
        "title": "Player DLL and FAC feedback are not a release validation",
        "evidence": {
            "player_dll_path": r"E:\Work\Warcraft III\d3d9.dll",
            "observed_size": 36412825,
            "observed_sha256": "FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529",
        },
        "impact": "Tree integration must not modify/redeploy the player DLL or claim stable/FPS/GPU release gates.",
        "required_decision": "Keep DLL identity frozen; any build/deploy/game validation remains a separate leased batch.",
    })
    return blockers

def build_recommendation(a: Dict[str, Any], b: Dict[str, Any], summary: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "choice": "file_reviewed_transplant_into_b_derived_integration_branch_then_switch_original_main_tree",
        "primary_source": "B",
        "why": "B has the authoritative v1.22 source and 221 divergent commits; A has one unique checkpoint and local assets/evidence. A wholesale ancestry merge or copy-over would either discard A local state or import unaudited B dirty/generated content.",
        "required_prerequisites": [
            "Checkpoint/export A dirty tracked, untracked and submodule state before any switch.",
            "Checkpoint only reviewed B source/test/docs as local commits; exclude generated evidence, logs, backups and the player DLL.",
            "Resolve all review_conflict paths file-by-file with B-primary and explicit accepted A hunks.",
            "Keep both existing branch refs and the original A worktree branch reachable.",
            "Keep B index gitlinks authoritative; archive A submodule state separately.",
        ],
        "fallback": "If ancestry is mandatory, create local checkpoint commits on both sides and build a reviewed merge commit with B-primary resolution; this has higher conflict and rollback complexity.",
        "do_not_do": [
            "git reset --hard",
            "git checkout/switch A while A dirty state is unarchived",
            "copy-over the A tree or A submodule worktree",
            "commit generated artifacts or player DLL backups",
            "push/tag/release",
        ],
    }

def audit_checkpoint_semantics(b_root: Path, allowlist_csv: Path, root_owned_csv: Path) -> Dict[str, Any]:
    """Review a proposed checkpoint path list against actual B content.

    The result intentionally never stores secret-bearing file content.  It
    records only pattern names, path identities, content hashes and build
    dependency evidence so a parent can re-run the same audit deterministically.
    """
    root = Path(b_root)
    rows = _read_proposal_rows(Path(allowlist_csv), Path(root_owned_csv))
    actual_status = _read_status(root)
    candidates = {row["path"] for row in rows}
    product_refs = _read_meson_product_refs(root, candidates)
    product_deps = _expand_product_dependencies(root, set(product_refs), candidates)
    records: List[Dict[str, Any]] = []

    for row in rows:
        rel = row["path"]
        flags = _content_flags(root / rel, rel)
        risks: List[str] = []
        refs = product_refs.get(rel, [])
        is_product_dependency = rel in product_deps
        is_native_light = any(token in rel.lower() for token in (
            "native_light", "native_model_light", "autolight", "auto_light"))

        if actual_status.get(rel) != row["b_status"]:
            decision = "blocked_snapshot_changed"
            reason = "actual B status differs from the proposal CSV"
            risks.append("snapshot_changed")
        elif not flags.get("exists"):
            decision = "blocked_missing_path"
            reason = "path no longer exists in current B worktree"
            risks.append("missing_path")
        elif flags.get("symlink") or not flags.get("regular"):
            decision = "reject_non_regular_path"
            reason = "symlink or non-regular path requires separate review"
            risks.append("non_regular_path")
        elif flags.get("nul_byte") or flags.get("binary_magic"):
            decision = "reject_binary_content"
            reason = "binary magic or NUL byte detected"
            risks.append("binary_content")
        elif flags.get("game_asset"):
            decision = "reject_game_asset"
            reason = "game/media asset extension"
            risks.append("game_asset")
        elif flags.get("raw_evidence"):
            decision = "reject_raw_evidence_external_preserve"
            reason = "raw evidence/generated extension must stay external"
            risks.append("raw_evidence")
        elif flags.get("secret_hits"):
            decision = "reject_secret_review"
            reason = "unreviewed credential-pattern hit; value not recorded"
            risks.append("secret_pattern")
        elif row["proposal_decision"] == "root_owned_allowlist":
            decision = "root_owned_do_not_stage"
            reason = "root owns this production/build policy path"
            risks.append("root_owned")
        elif is_product_dependency and is_native_light:
            decision = "hold_autolight_pending_root_meson_decision"
            reason = ("compiled product dependency in current B Meson; runtime "
                      "opt-in must be accepted by root or removed with matching "
                      "root-owned Meson changes")
            risks.append("autolight_compile_in")
        elif is_product_dependency:
            decision = "checkpoint_with_root_owned_build_coordination"
            reason = "product build dependency referenced by root-owned/current Meson"
        else:
            decision = "checkpoint_repo_source_test_or_docs"
            reason = "not referenced as a product dependency by current Meson/include closure"

        if rel in {
            "src/dxvk/dxvk_buffer.cpp",
            "src/dxvk/dxvk_memory.cpp",
            "src/dxvk/dxvk_buffer_allocation_guard.h",
        }:
            risks.append("allocator_failure_semantics_review_required")

        record: Dict[str, Any] = {
            "path": rel,
            "proposal_decision": row["proposal_decision"],
            "b_status": row["b_status"],
            "actual_b_status": actual_status.get(rel, ""),
            "status_match": actual_status.get(rel) == row["b_status"],
            "b_tracked": row["b_tracked"],
            "exists": flags.get("exists", False),
            "regular": flags.get("regular", False),
            "symlink": flags.get("symlink", False),
            "size": flags.get("size"),
            "sha256": flags.get("sha256"),
            "git_blob_sha1": flags.get("git_blob_sha1"),
            "nul_byte": flags.get("nul_byte", False),
            "binary_magic": flags.get("binary_magic", []),
            "game_asset": flags.get("game_asset", False),
            "raw_evidence": flags.get("raw_evidence", False),
            "secret_hits": flags.get("secret_hits", []),
            "reviewed_false_positives": flags.get("reviewed_false_positives", []),
            "product_build_reference": refs,
            "product_dependency": is_product_dependency,
            "recommended_decision": decision,
            "reason": reason,
            "risk_flags": sorted(set(risks)),
        }
        records.append(record)

    decision_counts = Counter(record["recommended_decision"] for record in records)
    risk_counts = Counter(risk for record in records for risk in record["risk_flags"])
    root_owned_paths = sorted(row["path"] for row in rows
                              if row["proposal_decision"] == "root_owned_allowlist")
    proposal_paths = sorted(row["path"] for row in rows
                            if row["proposal_decision"] == "b_checkpoint_allowlist")
    return {
        "schema": "warvk-r4-checkpoint-semantic-review/v1",
        "read_only": True,
        "b_root": _norm_path(root.resolve()),
        "b_head": _git_text(root, "rev-parse", "HEAD").strip(),
        "proposal_csv": _norm_path(Path(allowlist_csv).resolve()),
        "root_owned_csv": _norm_path(Path(root_owned_csv).resolve()),
        "proposal_counts": {
            "b_checkpoint_allowlist": len(proposal_paths),
            "root_owned_allowlist": len(root_owned_paths),
            "total": len(records),
        },
        "product_dependency_count": sum(1 for record in records if record["product_dependency"]),
        "decision_counts": {key: decision_counts[key] for key in sorted(decision_counts)},
        "risk_counts": {key: risk_counts[key] for key in sorted(risk_counts)},
        "proposal_paths": proposal_paths,
        "root_owned_paths": root_owned_paths,
        "records": records,
    }


def _git_z_long(root: os.PathLike[str] | str, *args: str) -> List[str]:
    raw = _run_git(root, "-c", "core.longpaths=true", *args).stdout
    return [item.decode("utf-8", "surrogateescape") for item in raw.split(b"\0") if item]


def _collision_record(a_root: Path, a_path: str, a_kind: str,
                      target_paths: Sequence[str]) -> Dict[str, Any]:
    identity = _hash_file(a_root / a_path)
    return {
        "a_path": a_path,
        "a_kind": a_kind,
        "target_paths": sorted(set(target_paths)),
        "a_size": identity.get("size"),
        "a_sha256": identity.get("sha256"),
        "a_git_blob_sha1": identity.get("git_blob_sha1"),
    }


def audit_transition_hazards(a_root: Path, b_root: Path, proposal_paths: Sequence[str],
                             root_owned_paths: Sequence[str]) -> Dict[str, Any]:
    """Read-only collision and nested-repository review for a future switch."""
    a_root = Path(a_root)
    b_root = Path(b_root)
    target: Dict[str, Dict[str, Any]] = {}
    for rec in _git_z_long(b_root, "ls-tree", "-r", "-z", "HEAD"):
        try:
            meta, path = rec.split("\t", 1)
            mode, typ, oid = meta.split()
        except ValueError:
            continue
        target[path.replace("\\", "/")] = {"source": "b_head", "mode": mode, "oid": oid}
    for rel in proposal_paths:
        target.setdefault(_safe_rel(rel), {})["source"] = "b_checkpoint_allowlist"
    for rel in root_owned_paths:
        target.setdefault(_safe_rel(rel), {})["source"] = "root_owned_allowlist"

    a_status = _read_status(a_root)
    a_untracked = {path for path, status in a_status.items() if status == "??"}
    a_ignored = set(_git_z_long(a_root, "ls-files", "-z", "--others", "--ignored",
                                "--exclude-standard"))
    target_fold: Dict[str, List[str]] = {}
    for path in target:
        target_fold.setdefault(_path_fold(path), []).append(path)

    untracked_exact: List[Dict[str, Any]] = []
    ignored_exact: List[Dict[str, Any]] = []
    untracked_casefold: List[Dict[str, Any]] = []
    ignored_casefold: List[Dict[str, Any]] = []
    for path in sorted(a_untracked):
        if path in target:
            untracked_exact.append(_collision_record(a_root, path, "untracked_nonignored", [path]))
        folded = [item for item in target_fold.get(_path_fold(path), []) if item != path]
        if folded:
            untracked_casefold.append(_collision_record(a_root, path, "untracked_nonignored", folded))
    for path in sorted(a_ignored):
        if path in target:
            ignored_exact.append(_collision_record(a_root, path, "ignored", [path]))
        folded = [item for item in target_fold.get(_path_fold(path), []) if item != path]
        if folded:
            ignored_casefold.append(_collision_record(a_root, path, "ignored", folded))

    reparse: List[Dict[str, Any]] = []
    seen_ancestors = set()
    for target_path in sorted(target):
        parts = target_path.split("/")
        for index in range(1, len(parts) + 1):
            ancestor = "/".join(parts[:index])
            if ancestor in seen_ancestors:
                continue
            seen_ancestors.add(ancestor)
            candidate = a_root / ancestor.replace("/", os.sep)
            try:
                candidate.lstat()
            except OSError:
                continue
            kind = None
            if stat.S_ISLNK(candidate.lstat().st_mode):
                kind = "symlink"
            elif hasattr(os.path, "isjunction") and os.path.isjunction(candidate):
                kind = "junction"
            if kind:
                reparse.append({
                    "a_path": ancestor,
                    "target_path": target_path,
                    "kind": kind,
                    "is_target_path": ancestor == target_path,
                })

    sub_rel = "subprojects/StormBreaker"
    a_sub = a_root / sub_rel
    b_sub = b_root / sub_rel
    a_sub_git = a_sub / ".git"
    b_sub_git = b_sub / ".git"
    a_nested_kind = "missing"
    b_nested_kind = "missing"
    a_nested_target = None
    b_nested_target = None
    if a_sub_git.is_file():
        a_nested_kind = "file"
        a_nested_target = a_sub_git.read_text(encoding="utf-8", errors="surrogateescape").strip()
    elif a_sub_git.is_dir():
        a_nested_kind = "directory"
    if b_sub_git.is_file():
        b_nested_kind = "file"
        b_nested_target = b_sub_git.read_text(encoding="utf-8", errors="surrogateescape").strip()
    elif b_sub_git.is_dir():
        b_nested_kind = "directory"
    a_index = _read_index(a_root).get(sub_rel, {})
    b_index = _read_index(b_root).get(sub_rel, {})
    nested_status_count = -1
    nested_head = None
    try:
        nested_status_count = len(_read_status(a_sub))
        nested_head = _git_text(a_sub, "rev-parse", "HEAD", check=False).strip()
    except AuditError:
        pass
    backup = Path(r"D:/WarVK-Backups/20260920-release-closeout-A-StormBreaker")
    submodule = {
        "path": sub_rel,
        "a_index_gitlink": a_index.get("oid"),
        "b_index_gitlink": b_index.get("oid"),
        "target_gitlink": target.get(sub_rel, {}).get("oid"),
        "a_worktree_head": nested_head,
        "a_worktree_dirty_status_count": nested_status_count,
        "a_nested_git_kind": a_nested_kind,
        "a_nested_git_target": a_nested_target,
        "a_nested_index_lock_present": (a_sub_git / "index.lock").exists() if a_sub_git.is_dir() else False,
        "b_nested_git_kind": b_nested_kind,
        "b_nested_git_target": b_nested_target,
        "root_backup": {
            "path": _norm_path(backup),
            "exists": backup.exists(),
            "expected_head": "375e82e3e4f901c52285c716bd090ea35fe8721c",
            "expected_sha256": "45DD3AF127D2C5475527A7E4558EF963D61494109860584242DE56282E4CB957",
            "do_not_recreate": True,
        },
    }

    untracked_collision_paths = [item["a_path"] for item in untracked_exact]
    untracked_collision_paths += [item["a_path"] for item in untracked_casefold]
    ignored_collision_paths = [item["a_path"] for item in ignored_exact]
    ignored_collision_paths += [item["a_path"] for item in ignored_casefold]
    return {
        "schema": "warvk-r4-transition-hazard-manifest/v1",
        "read_only": True,
        "a_root": _norm_path(a_root.resolve()),
        "b_root": _norm_path(b_root.resolve()),
        "target_path_count": len(target),
        "target_source_counts": {key: sum(1 for value in target.values()
                                          if value.get("source") == key)
                                  for key in sorted({value.get("source", "")
                                                     for value in target.values()})},
        "a_status_counts": {key: sum(1 for value in a_status.values() if value == key)
                            for key in sorted(set(a_status.values()))},
        "a_untracked_count": len(a_untracked),
        "a_ignored_count": len(a_ignored),
        "untracked_exact_collisions": untracked_exact,
        "ignored_exact_collisions": ignored_exact,
        "untracked_casefold_collisions": untracked_casefold,
        "ignored_casefold_collisions": ignored_casefold,
        "ancestor_or_target_reparse_collisions": reparse,
        "preserve_targets": {
            "stash_include_untracked_captures": sorted(set(untracked_collision_paths)),
            "external_create_new_required": sorted(set(
                ignored_collision_paths +
                [item["a_path"] for item in reparse if item.get("is_target_path")])),
            "rule": ("git stash push --include-untracked covers non-ignored untracked collisions; "
                     "ignored exact/case-fold collisions and target reparse points must be "
                     "preserved by exact path outside Git before switch; do not recursive-move "
                     "the workspace"),
        },
        "submodule": submodule,
        "synthetic_evidence": [
            "test_switch_ignored_collision_overwrites",
            "test_switch_untracked_collision_blocks",
            "test_stash_include_untracked_preserves_untracked_not_ignored",
            "test_switch_leaves_dirty_submodule_untouched",
        ],
        "switch_policy": {
            "forbid_force": True,
            "require_stash_or_external_preserve": True,
            "require_reaudit_after_any_a_or_b_write": True,
        },
    }


def audit_repositories(a_root: Path, b_root: Path) -> Dict[str, Any]:
    a = _read_state(Path(a_root))
    b = _read_state(Path(b_root))
    union = sorted(a["inventory"] | b["inventory"])
    records = [_record_for_path(path, a, b) for path in union]
    summary = _summarize(a, b, records)
    blockers = _build_blockers(a, b, summary, records)
    common_dir = a.get("common_dir") or b.get("common_dir")
    result: Dict[str, Any] = {
        "schema": SCHEMA,
        "read_only": True,
        "a": {key: value for key, value in a.items() if key not in {"status", "index", "head_tree", "inventory", "tracked", "untracked", "worktrees", "refs"}},
        "b": {key: value for key, value in b.items() if key not in {"status", "index", "head_tree", "inventory", "tracked", "untracked", "worktrees", "refs"}},
        "common_git": {
            "common_dir": common_dir,
            "a_ref_count": len(a["refs"]),
            "b_ref_count": len(b["refs"]),
            "a_worktree_count": len(a["worktrees"]),
            "b_worktree_count": len(b["worktrees"]),
            "a_remotes": a["remotes"],
            "b_remotes": b["remotes"],
        },
        "comparison": summary,
        "blockers": blockers,
        "recommendation": build_recommendation(a, b, summary),
        "records": records,
    }
    return result

def _writer(path: Path, data: Dict[str, Any]) -> None:
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")

def _csv_row(record: Dict[str, Any]) -> Dict[str, Any]:
    row: Dict[str, Any] = {}
    for key, value in record.items():
        if isinstance(value, bool):
            row[key] = "1" if value else "0"
        elif value is None:
            row[key] = ""
        elif isinstance(value, (dict, list)):
            row[key] = json.dumps(value, ensure_ascii=False, sort_keys=True)
        else:
            row[key] = value
    return row

def write_outputs(output_dir: Path, result: Dict[str, Any]) -> Dict[str, str]:
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    records = result.pop("records")
    csv_path = output_dir / CSV_NAME
    json_path = output_dir / JSON_NAME
    fields = [
        "path", "kind", "route_hint", "integration_action", "exclusion_reason", "manual_review", "reason",
        "a_present", "b_present", "a_tracked", "b_tracked", "a_in_index", "b_in_index", "a_in_head", "b_in_head",
        "a_index_mode", "b_index_mode", "a_head_blob", "b_head_blob", "a_index_blob", "b_index_blob",
        "a_worktree_present", "b_worktree_present", "a_worktree_blob", "b_worktree_blob",
        "same_worktree_content", "a_equals_a_head", "b_equals_b_head", "a_equals_b_head", "b_equals_a_head",
        "a_status", "b_status", "a_sha256", "b_sha256", "a_size", "b_size",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        for record in sorted(records, key=lambda item: item["path"]):
            writer.writerow(_csv_row(record))
    result["generated_outputs"] = {
        "csv": {"name": CSV_NAME, "size": csv_path.stat().st_size, "sha256": hashlib.sha256(csv_path.read_bytes()).hexdigest()},
        "json": {"name": JSON_NAME},
    }
    _writer(json_path, result)
    return {"csv": str(csv_path), "json": str(json_path)}


def write_review_outputs(output_dir: Path, semantic_result: Dict[str, Any],
                         transition_result: Dict[str, Any]) -> Dict[str, str]:
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    records = semantic_result.get("records", [])
    csv_path = output_dir / SEMANTIC_CSV_NAME
    semantic_json_path = output_dir / SEMANTIC_JSON_NAME
    transition_json_path = output_dir / TRANSITION_JSON_NAME
    fields = [
        "path", "proposal_decision", "b_status", "actual_b_status", "status_match",
        "b_tracked", "exists", "regular", "symlink", "size", "sha256",
        "git_blob_sha1", "nul_byte", "binary_magic", "game_asset", "raw_evidence",
        "secret_hits", "reviewed_false_positives", "product_build_reference",
        "product_dependency", "recommended_decision", "reason", "risk_flags",
    ]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore",
                                lineterminator="\n")
        writer.writeheader()
        for record in sorted(records, key=lambda item: item["path"]):
            writer.writerow(_csv_row(record))
    semantic_obj = {key: value for key, value in semantic_result.items() if key != "records"}
    semantic_obj["generated_outputs"] = {
        "csv": {"name": SEMANTIC_CSV_NAME, "size": csv_path.stat().st_size,
                "sha256": hashlib.sha256(csv_path.read_bytes()).hexdigest()},
        "json": {"name": SEMANTIC_JSON_NAME},
    }
    _writer(semantic_json_path, semantic_obj)
    transition_obj = dict(transition_result)
    transition_obj["generated_outputs"] = {"name": TRANSITION_JSON_NAME}
    _writer(transition_json_path, transition_obj)
    return {
        "semantic_csv": str(csv_path),
        "semantic_json": str(semantic_json_path),
        "transition_json": str(transition_json_path),
    }


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Read-only WarVK v1.22 A/B tree integration audit")
    parser.add_argument("--a-root", default=DEFAULT_A, help="legacy A worktree root")
    parser.add_argument("--b-root", default=DEFAULT_B, help="authoritative B worktree root")
    parser.add_argument("--output-dir", default=DEFAULT_OUT, help="JSON/CSV output directory")
    parser.add_argument("--quiet", action="store_true", help="suppress summary on stdout")
    parser.add_argument("--semantic-review", action="store_true",
                        help="also audit r3 checkpoint CSVs and switch hazards")
    parser.add_argument("--semantic-allowlist", default="",
                        help="r3 B checkpoint allowlist CSV")
    parser.add_argument("--semantic-root-owned", default="",
                        help="r3 root-owned allowlist CSV")
    args = parser.parse_args(argv)
    result = audit_repositories(Path(args.a_root), Path(args.b_root))
    outputs = write_outputs(Path(args.output_dir), result)
    review_outputs = None
    if args.semantic_review:
        output_dir = Path(args.output_dir)
        allowlist = Path(args.semantic_allowlist) if args.semantic_allowlist else output_dir / "r3-b-checkpoint-allowlist.csv"
        root_owned = Path(args.semantic_root_owned) if args.semantic_root_owned else output_dir / "r3-root-owned-allowlist.csv"
        semantic = audit_checkpoint_semantics(Path(args.b_root), allowlist, root_owned)
        transition = audit_transition_hazards(
            Path(args.a_root), Path(args.b_root),
            semantic["proposal_paths"], semantic["root_owned_paths"])
        review_outputs = write_review_outputs(output_dir, semantic, transition)
        if not args.quiet:
            print(json.dumps(semantic["decision_counts"], ensure_ascii=False,
                             indent=2, sort_keys=True))
            print("review outputs: %s , %s , %s" % (
                review_outputs["semantic_json"], review_outputs["semantic_csv"],
                review_outputs["transition_json"]))
    if not args.quiet:
        print(json.dumps(result["comparison"], ensure_ascii=False, indent=2, sort_keys=True))
        print("outputs: %s , %s" % (outputs["json"], outputs["csv"]))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
