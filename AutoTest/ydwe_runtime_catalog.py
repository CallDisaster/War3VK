#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成 YDWE instance 的只读晚加载运行时目录。

本模块只在调用瞬间枚举和散列普通文件，不打开或启动 Warcraft/YDWE，也不持有
任何用于阻止文件替换的 HANDLE。因此目录只能作为后续启动桥的输入证据，不能
单独证明运行期间的闭包不变，更不能作为生产启动授权。
"""

from __future__ import annotations

import hashlib
import json
import os
import stat
import unicodedata
from pathlib import Path
from typing import Any, Iterator


SCHEMA_VERSION = "ydwe-runtime-catalog-v1"
_REQUIRED_TREES = (
    ("ydwe-bin", Path("bin")),
    ("ydwe-script-common", Path("script") / "common"),
    ("ydwe-script-war3", Path("script") / "war3"),
    ("ydwe-plugin-warcraft3", Path("plugin") / "warcraft3"),
)


class RuntimeCatalogError(ValueError):
    """目录不能安全、完整地建立时的 fail-closed 错误。"""


def _sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def _path_is_reparse(path: Path) -> bool:
    """同时识别 Windows reparse/junction 与跨平台 symlink。"""

    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    attributes = int(getattr(info, "st_file_attributes", 0))
    is_junction = getattr(path, "is_junction", None)
    return (
        bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))
        or stat.S_ISLNK(info.st_mode)
        or bool(callable(is_junction) and is_junction())
    )


def _lexical_absolute(path: Path | str, label: str) -> Path:
    text = os.fspath(path)
    if not text or "\0" in text or any(ord(character) < 0x20 for character in text):
        raise RuntimeCatalogError(f"{label} 包含空路径、NUL 或控制字符")
    return Path(os.path.abspath(os.path.expanduser(text)))


def _assert_no_reparse_chain(path: Path | str, label: str) -> Path:
    absolute = _lexical_absolute(path, label)
    # 从卷根到叶节点逐组件检查，不能只检查最终目录。
    for component in reversed((absolute, *absolute.parents)):
        if _path_is_reparse(component):
            raise RuntimeCatalogError(
                f"{label} 路径链包含 symlink/junction/reparse point: {component}"
            )
    return absolute


def _path_key(path: Path) -> str:
    return os.path.normcase(str(path)).casefold()


def _require_within(path: Path | str, root: Path | str, label: str) -> Path:
    candidate = _assert_no_reparse_chain(path, label)
    boundary = _assert_no_reparse_chain(root, "instance_root")
    try:
        common = Path(os.path.commonpath([str(candidate), str(boundary)]))
    except ValueError as exc:
        raise RuntimeCatalogError(f"{label} 与 instance_root 不在同一卷") from exc
    if _path_key(common) != _path_key(boundary) or _path_key(candidate) == _path_key(boundary):
        raise RuntimeCatalogError(f"{label} 必须严格位于 instance_root 内: {candidate}")
    return candidate


def _require_plain_directory(path: Path, label: str) -> Path:
    safe = _assert_no_reparse_chain(path, label)
    try:
        info = os.lstat(safe)
    except FileNotFoundError as exc:
        raise RuntimeCatalogError(f"缺少必需目录 {label}: {safe}") from exc
    if not stat.S_ISDIR(info.st_mode):
        raise RuntimeCatalogError(f"{label} 不是普通目录: {safe}")
    return safe


def _iter_tree_files(root: Path, label: str) -> Iterator[Path]:
    """不跟随链接地递归枚举；树内任何特殊节点都会拒绝整个目录。"""

    with os.scandir(root) as entries:
        ordered = sorted(entries, key=lambda entry: (entry.name.casefold(), entry.name))
    for entry in ordered:
        path = Path(entry.path)
        if _path_is_reparse(path):
            raise RuntimeCatalogError(
                f"{label} 包含 symlink/junction/reparse point: {path}"
            )
        info = entry.stat(follow_symlinks=False)
        if stat.S_ISDIR(info.st_mode):
            yield from _iter_tree_files(path, label)
        elif stat.S_ISREG(info.st_mode):
            yield path
        else:
            raise RuntimeCatalogError(f"{label} 包含非普通文件节点: {path}")


def _canonical_relative_path(path: Path, instance_root: Path) -> str:
    try:
        relative = path.relative_to(instance_root)
    except ValueError as exc:
        raise RuntimeCatalogError(f"文件越过 instance_root: {path}") from exc
    parts = relative.parts
    if not parts or any(part in ("", ".", "..") for part in parts):
        raise RuntimeCatalogError(f"无法形成规范相对路径: {path}")
    return unicodedata.normalize("NFC", "/".join(parts))


def _audit_plain_file(
    path: Path,
    instance_root: Path,
    source_group: str,
) -> dict[str, Any]:
    safe = _assert_no_reparse_chain(path, source_group)
    try:
        before = os.lstat(safe)
    except FileNotFoundError as exc:
        raise RuntimeCatalogError(f"缺少必需文件 {source_group}: {safe}") from exc
    if not stat.S_ISREG(before.st_mode):
        raise RuntimeCatalogError(f"{source_group} 不是普通文件: {safe}")

    digest = _sha256_file(safe)
    after = os.lstat(safe)
    identity_before = (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    identity_after = (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    if not stat.S_ISREG(after.st_mode) or _path_is_reparse(safe) or identity_before != identity_after:
        raise RuntimeCatalogError(f"散列期间文件发生替换或修改: {safe}")

    return {
        "relativePath": _canonical_relative_path(safe, instance_root),
        "size": int(after.st_size),
        "sha256": digest,
        "sourceGroup": source_group,
    }


def _catalog_digest(files: list[dict[str, Any]]) -> str:
    canonical = {
        "schemaVersion": SCHEMA_VERSION,
        "files": files,
    }
    payload = json.dumps(
        canonical,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def audit_ydwe_runtime_catalog(
    *,
    instance_root: Path | str,
    ydwe_root: Path | str,
) -> dict[str, Any]:
    """审计一个 instance 内的 YDWE 晚加载文件集合。

    返回值无论成功或失败都明确声明不持有句柄、不可启动。成功只表示在本次读取
    瞬间满足目录契约；后续生产桥仍须绝对路径加载、句柄保留和 ready watchdog。
    """

    base: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "ok": False,
        "code": "RUNTIME_CATALOG_REJECTED",
        "handlesRetained": False,
        "launchable": False,
        "immutabilityGuaranteed": False,
        "sourceMayChangeAfterAudit": True,
        "catalogSha256": None,
        "files": [],
        "fileCount": 0,
        "totalBytes": 0,
        "blockers": [],
    }
    try:
        instance = _require_plain_directory(
            _assert_no_reparse_chain(instance_root, "instance_root"),
            "instance_root",
        )
        ydwe = _require_within(ydwe_root, instance, "ydwe_root")
        ydwe = _require_plain_directory(ydwe, "ydwe_root")

        rows = [
            _audit_plain_file(instance / "war3.exe", instance, "instance-core"),
            _audit_plain_file(instance / "Game.dll", instance, "instance-core"),
        ]
        for source_group, relative_tree in _REQUIRED_TREES:
            tree = _require_plain_directory(ydwe / relative_tree, source_group)
            tree_files = list(_iter_tree_files(tree, source_group))
            if not tree_files:
                raise RuntimeCatalogError(f"必需目录不得为空: {source_group} ({tree})")
            rows.extend(
                _audit_plain_file(path, instance, source_group)
                for path in tree_files
            )

        rows.sort(key=lambda row: (row["relativePath"].casefold(), row["relativePath"]))
        seen: dict[str, str] = {}
        for row in rows:
            relative = row["relativePath"]
            collision_key = relative.casefold()
            previous = seen.get(collision_key)
            if previous is not None:
                raise RuntimeCatalogError(
                    f"规范相对路径发生大小写重名: {previous!r} 与 {relative!r}"
                )
            seen[collision_key] = relative

        base.update({
            "ok": True,
            "code": "RUNTIME_CATALOG_AUDITED_NOT_LAUNCHABLE",
            "instanceRoot": str(instance),
            "ydweRoot": str(ydwe),
            "ydweRelativeRoot": _canonical_relative_path(ydwe, instance),
            "files": rows,
            "fileCount": len(rows),
            "totalBytes": sum(row["size"] for row in rows),
            "catalogSha256": _catalog_digest(rows),
        })
    except (OSError, RuntimeCatalogError, ValueError) as exc:
        base["blockers"] = [f"{type(exc).__name__}: {exc}"]
    return base


__all__ = [
    "SCHEMA_VERSION",
    "RuntimeCatalogError",
    "audit_ydwe_runtime_catalog",
]
