#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""YDWE 1.32.13 ``ydhost`` 的只读资产与地图元数据门禁。

本模块刻意不启动 ``ydhost.exe`` 或 Warcraft III。它只确认：

* ydhost 二进制和两个随附 VC 运行库与已审计版本完全一致；
* 地图是实际 MPQ，而不是 w3x2lni 的 ``W2L\x01`` 目录占位文件；
* ``map.cfg`` 具有完整字段，并由绑定到地图 SHA-256 的清单声明。

真正的建房/入房流程仍须由多实例调度器显式实现并验收；在此之前保持
fail-closed，绝不把多个 ``-loadfile`` 进程当作局域网联机测试。
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import ctypes.wintypes as wintypes
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping, Sequence


YDHOST_PROFILE_ID = "ydwe-1.32.13-memoryhack-ydhost-20180617"
YDHOST_REQUIRED_FILES: Mapping[str, str] = {
    "ydhost.exe": "cb1f6e1fb1e3844400381302c2f0488d091fcddab6537523e289d9a638c34dd0",
    "msvcp140.dll": "2db7fd3c9c3c4b67f2d50a5a50e8c69154dc859780dd487c28a4e6ed1af90d01",
    "vcruntime140.dll": "9d02e952396bdff3abfe5654e07b7a713c84268a225e11ed9a3bf338ed1e424c",
}
YDHOST_REQUIRED_SIZES: Mapping[str, int] = {
    "ydhost.exe": 138752,
    "msvcp140.dll": 449280,
    "vcruntime140.dll": 80128,
}
YDHOST_ZIP_MAX_ENTRY_BYTES = 1024 * 1024
YDHOST_ZIP_MAX_COMPRESSION_RATIO = 200.0
MAPDUMP_SOURCE_SHA256 = "f67d4685032168b45a804c82e3cc62d0ed7d373ade5146242ea92e45ae75acbc"
METADATA_SCHEMA_VERSION = 2
PROVISION_SCHEMA_VERSION = 1
PROVISION_MANIFEST_NAME = "provision-manifest.json"
MAPDUMP_PROFILE_ID = "ydwe-1.32.13-mapdump-runtime24-snapshot-v2"
MAPDUMP_CATALOG_SHA256 = "9c0afb43451537c5e9e939663e3ca52f407cacc8537a024de7d1301e0c39362b"
MAPDUMP_CATALOG_FILE_COUNT = 434
MAPDUMP_REQUIRED_FILES: Mapping[str, str] = {
    "bin/lua.exe": "2237ce0004d7b8ded7804bf78206fb72988c7149b3d38e0f7469142d2fb19208",
    "bin/lua53.dll": "27269d53120bb25ee8210bb6de453d0ba3795015ff46332ded79d4531d1b5e77",
    "bin/vcruntime140.dll": "9d02e952396bdff3abfe5654e07b7a713c84268a225e11ed9a3bf338ed1e424c",
    "bin/ucrtbase.dll": "6f8f05993b8a25cadf5e301e58194c4d23402e467229b12e40956e4f128588b3",
    "bin/StormLib.dll": "8886ee26e9356f3a9580c64c0ba21b21f70cda9ae5e5974134cae4b1f4f1a0cc",
    "bin/modules/filesystem.dll": "0ec0bd94a3b3c2a99209570a5f64676320c330740defd21e9eaa6d383c23f68c",
    "bin/modules/ffi.dll": "9adedb0deda0b888c605910ad7cf57eff3402d4bf9b61364e2c18549489f9f3f",
    "bin/modules/maphash.dll": "f54164fee7b0bb5cd24ddbcfda0f83673c279bfd5652f74a3e2d48c5ce5616cf",
    "bin/modules/w3xparser.dll": "95c092ebcbae69c35d617191f730b1e7f40ef17498ff20b310a7fcda18c1f0dd",
    "bin/modules/lni.dll": "84cebb73e348c457c88a24a4bcc3a9f037cfc699429533558b498291a44f0e54",
    "bin/modules/lpeg.dll": "8183fb83d22aadb8721cbca45e16820ea859606f456b41496ef8fb4be6e20a61",
    "bin/modules/lml.dll": "b5319ad3fb15305c1315532229fa6b426346ab870a876d9f4919b2ac9d4b0b2b",
    "bin/modules/i18n.dll": "054b43198662fab0c3c75946238d202ba95d82b25a0f119c645f0074205ae7d8",
    "bin/modules/process.dll": "032082212f1c636f1f78253ca3441724bdf27e0007034659a19031b03cfcfa97",
    "script/common/utiliy.lua": "8c74864d83a3c3fc39ea35c8f29299a8aae4e6e3a4aedbdc4f9aa629ee17c89b",
    "script/common/localization.lua": "9329aafa1f248f491e790a5bb377a14dcc075e21aba6a9ed66257cc3a8a7032a",
    "script/ydwe/mapdump.lua": MAPDUMP_SOURCE_SHA256,
    "compiler/script/w3x2lni/init.lua": "f979a46d1d3c22b5e00f2c03b6e450d968cba79294775f399a7a45d1456dc5cb",
    "compiler/script/w3x2lni/data_load.lua": "667a35388167c6fead9e204af47ef32e41e69a474cf4619fdd85bc92a956d7c0",
    "plugin/w3x2lni/script/backend/sandbox.lua": "69b780623edc5f13c6ef0b0fcafabcc6a27d61143810bc19199f879507f99673",
    "plugin/w3x2lni/script/core/slk/frontend_w3i.lua": "afa352fafaf5b88967e52d9610d45db436a5f6f9d2487e302774759252090232",
    "compiler/jass/24/common.j": "9cb04da1c75dd30025d96fec12b4302c2f6a1f1a8c864b66d6cf52c65ecde924",
    "compiler/jass/24/Blizzard.j": "d290865bd673f8394f4e6fb7b4d6720412fed3a447877523cec9e54e65b8e62c",
}

_FIXED_MAP_FIELDS = {
    "map_size": 4,
    "map_info": 4,
    "map_crc": 4,
    "map_sha1": 20,
    "map_width": 2,
    "map_height": 2,
}
_SLOT_RE = re.compile(r"^map_slot([1-9][0-9]*)$")


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _lexical_absolute(path: Path) -> Path:
    """返回不解析 symlink/junction 的绝对规范路径。"""

    return Path(os.path.abspath(os.path.normpath(os.fspath(path))))


def _is_within(path: Path, root: Path) -> bool:
    """只做 lexical containment；绝不以 resolve 后的位置代替安全边界。"""

    try:
        candidate = os.path.normcase(os.fspath(_lexical_absolute(path)))
        boundary = os.path.normcase(os.fspath(_lexical_absolute(root)))
        return os.path.commonpath((candidate, boundary)) == boundary
    except (OSError, ValueError):
        return False


def _is_reparse_point(path: Path) -> bool:
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    attributes = int(getattr(info, "st_file_attributes", 0) or 0)
    return stat.S_ISLNK(info.st_mode) or bool(attributes & 0x00000400)


def _existing_components(path: Path) -> list[Path]:
    absolute = _lexical_absolute(path)
    anchor = Path(absolute.anchor)
    current = anchor
    rows: list[Path] = [anchor]
    for part in absolute.parts[1:]:
        current = current / part
        if os.path.lexists(current):
            rows.append(current)
        else:
            break
    return rows


def assert_safe_path(path: Path, boundary: Path, label: str) -> Path:
    """lexical 边界 + 从卷根到目标的逐组件 reparse 拒绝。"""

    target = _lexical_absolute(path)
    root = _lexical_absolute(boundary)
    if not _is_within(target, root):
        raise ValueError(f"{label} 不在允许边界内: {target}")
    for component in _existing_components(target):
        if _is_reparse_point(component):
            raise ValueError(f"{label} 路径含 reparse point/junction: {component}")
    return target


def _assert_safe_external_tree(path: Path, label: str) -> Path:
    """外部只读源没有 sandbox 边界，但仍拒绝任一 reparse 组件。"""

    target = _lexical_absolute(path)
    for component in _existing_components(target):
        if _is_reparse_point(component):
            raise ValueError(f"{label} 路径含 reparse point/junction: {component}")
    return target


def _safe_tree_files(directory: Path, label: str) -> list[Path]:
    """不跟随 symlink/junction 的递归枚举；发现 reparse 立即拒绝整棵树。"""

    root = _assert_safe_external_tree(directory, label)
    if not root.is_dir():
        raise FileNotFoundError(root)
    rows: list[Path] = []
    for current_name, directory_names, file_names in os.walk(root, followlinks=False):
        current = Path(current_name)
        _assert_safe_external_tree(current, label)
        safe_directories: list[str] = []
        for name in directory_names:
            candidate = current / name
            if _is_reparse_point(candidate):
                raise ValueError(f"{label} 含 reparse point/junction: {candidate}")
            safe_directories.append(name)
        directory_names[:] = safe_directories
        for name in file_names:
            candidate = current / name
            if _is_reparse_point(candidate):
                raise ValueError(f"{label} 含 reparse 文件: {candidate}")
            if candidate.is_file():
                rows.append(candidate)
    return sorted(rows, key=lambda item: item.as_posix())


@contextlib.contextmanager
def pin_directory(path: Path, boundary: Path, label: str):
    """持有无 SHARE_DELETE 的目录句柄，覆盖最终复查到 replace/rmtree 的竞态窗口。"""

    directory = assert_safe_path(path, boundary, label)
    if not directory.is_dir():
        raise NotADirectoryError(directory)
    if os.name != "nt":
        descriptor = os.open(directory, os.O_RDONLY)
        identity = (os.fstat(descriptor).st_dev, os.fstat(descriptor).st_ino)
        try:
            yield identity
            current = os.stat(directory, follow_symlinks=False)
            if (current.st_dev, current.st_ino) != identity:
                raise OSError(f"{label} identity 在操作期间发生变化")
            assert_safe_path(directory, boundary, label)
        finally:
            os.close(descriptor)
        return

    FILE_LIST_DIRECTORY = 0x0001
    FILE_SHARE_READ = 0x00000001
    FILE_SHARE_WRITE = 0x00000002
    OPEN_EXISTING = 3
    FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
    FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    class ByHandleFileInformation(ctypes.Structure):
        _fields_ = [
            ("dwFileAttributes", wintypes.DWORD),
            ("ftCreationTime", wintypes.FILETIME),
            ("ftLastAccessTime", wintypes.FILETIME),
            ("ftLastWriteTime", wintypes.FILETIME),
            ("dwVolumeSerialNumber", wintypes.DWORD),
            ("nFileSizeHigh", wintypes.DWORD),
            ("nFileSizeLow", wintypes.DWORD),
            ("nNumberOfLinks", wintypes.DWORD),
            ("nFileIndexHigh", wintypes.DWORD),
            ("nFileIndexLow", wintypes.DWORD),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.LPVOID,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.GetFileInformationByHandle.argtypes = [
        wintypes.HANDLE,
        ctypes.POINTER(ByHandleFileInformation),
    ]
    kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.GetFinalPathNameByHandleW.argtypes = [
        wintypes.HANDLE,
        wintypes.LPWSTR,
        wintypes.DWORD,
        wintypes.DWORD,
    ]
    kernel32.GetFinalPathNameByHandleW.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL

    handle = kernel32.CreateFileW(
        str(directory),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        None,
    )
    if handle in (None, 0, INVALID_HANDLE_VALUE):
        raise OSError(ctypes.get_last_error(), f"无法 pin {label}: {directory}")
    information = ByHandleFileInformation()

    def final_path_from_handle() -> Path:
        size = kernel32.GetFinalPathNameByHandleW(handle, None, 0, 0)
        if size <= 0:
            raise OSError(ctypes.get_last_error(), f"无法读取 {label} final path")
        buffer = ctypes.create_unicode_buffer(size + 1)
        written = kernel32.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
        if written <= 0 or written >= len(buffer):
            raise OSError(ctypes.get_last_error(), f"无法读取完整 {label} final path")
        text = buffer.value
        if text.startswith("\\\\?\\UNC\\"):
            text = "\\\\" + text[8:]
        elif text.startswith("\\\\?\\"):
            text = text[4:]
        return _lexical_absolute(Path(text))

    try:
        if not kernel32.GetFileInformationByHandle(handle, ctypes.byref(information)):
            raise OSError(ctypes.get_last_error(), f"无法读取 {label} identity")
        if int(information.dwFileAttributes) & 0x00000400:
            raise ValueError(f"{label} 是 reparse point/junction: {directory}")
        opened_path = final_path_from_handle()
        if os.path.normcase(str(opened_path)) != os.path.normcase(str(directory)):
            raise ValueError(
                f"{label} handle final path 逃逸/漂移: expected={directory}, actual={opened_path}"
            )
        identity = (
            int(information.dwVolumeSerialNumber),
            int(information.nFileIndexHigh),
            int(information.nFileIndexLow),
        )
        yield identity
        assert_safe_path(directory, boundary, label)
        if os.path.normcase(str(final_path_from_handle())) != os.path.normcase(str(directory)):
            raise OSError(f"{label} final path 在操作期间发生变化")
        after = ByHandleFileInformation()
        if not kernel32.GetFileInformationByHandle(handle, ctypes.byref(after)):
            raise OSError(ctypes.get_last_error(), f"无法复查 {label} identity")
        after_identity = (
            int(after.dwVolumeSerialNumber),
            int(after.nFileIndexHigh),
            int(after.nFileIndexLow),
        )
        if after_identity != identity:
            raise OSError(f"{label} identity 在操作期间发生变化")
    finally:
        kernel32.CloseHandle(handle)


@contextlib.contextmanager
def pin_nearest_existing_parent(path: Path, boundary: Path, label: str):
    candidate = _lexical_absolute(path).parent
    root = _lexical_absolute(boundary)
    while not candidate.exists():
        if candidate == root or candidate.parent == candidate:
            break
        candidate = candidate.parent
    candidate = assert_safe_path(candidate, root, f"{label} 最近现存父目录")
    with pin_directory(candidate, root, f"{label} 最近现存父目录") as identity:
        yield identity


def secure_makedirs(path: Path, boundary: Path, label: str) -> Path:
    """逐层 pin 父目录创建子目录；并发插入 junction 会失败或在复查时被拒绝。"""

    target = _lexical_absolute(path)
    root = assert_safe_path(boundary, boundary, f"{label} 边界")
    if not _is_within(target, root):
        raise ValueError(f"{label} 目标越界: {target}")
    relative = target.relative_to(root)
    current = root
    for part in relative.parts:
        with pin_directory(current, root, f"{label} 父目录 {current}"):
            child = current / part
            assert_safe_path(child, root, f"{label} 子目录 {child}")
            try:
                child.mkdir()
            except FileExistsError:
                pass
            assert_safe_path(child, root, f"{label} 子目录复查 {child}")
            if not child.is_dir():
                raise NotADirectoryError(child)
        current = child
    return current


def audit_ydhost_bundle(
    bundle_root: Path,
    sandbox_root: Path,
    required_files: Mapping[str, str] = YDHOST_REQUIRED_FILES,
) -> dict[str, Any]:
    """校验一个已解压资产目录；目录必须位于专用 AutoTest 沙盒内。"""

    root = _lexical_absolute(bundle_root)
    sandbox = _lexical_absolute(sandbox_root)
    result: dict[str, Any] = {
        "profileId": YDHOST_PROFILE_ID,
        "root": str(root),
        "withinSandbox": _is_within(root, sandbox),
        "files": [],
        "errors": [],
    }
    if not result["withinSandbox"]:
        result["errors"].append("ydhost 资产目录不在专用 AutoTest 沙盒内")
    else:
        try:
            assert_safe_path(root, sandbox, "ydhost 资产目录")
        except ValueError as exc:
            result["errors"].append(str(exc))
    if not root.is_dir():
        result["errors"].append("ydhost 资产目录不存在")
    for name, expected_sha in required_files.items():
        path = root / name
        row: dict[str, Any] = {
            "name": name,
            "path": str(path),
            "expectedSha256": expected_sha,
            "exists": path.is_file(),
        }
        if path.is_file():
            try:
                assert_safe_path(path, sandbox, f"ydhost 资产文件 {name}")
                row["size"] = path.stat().st_size
                row["sha256"] = sha256_file(path)
                row["shaMatches"] = row["sha256"].casefold() == expected_sha.casefold()
                if not row["shaMatches"]:
                    result["errors"].append(f"{name} SHA-256 与审计版本不符")
            except ValueError as exc:
                row["shaMatches"] = False
                result["errors"].append(str(exc))
        else:
            row["shaMatches"] = False
            result["errors"].append(f"缺少 {name}")
        result["files"].append(row)
    result["ok"] = not result["errors"]
    return result


def _allowed_provision_targets(sandbox: Path) -> tuple[Path, Path]:
    return (
        _lexical_absolute(sandbox / "plugin" / "ydhost"),
        _lexical_absolute(sandbox / "KKWE插件" / "plugin" / "ydhost"),
    )


def _provision_manifest_payload(source: Path, target: Path, files: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schemaVersion": PROVISION_SCHEMA_VERSION,
        "profileId": YDHOST_PROFILE_ID,
        "sourceRoot": str(source),
        "targetRoot": str(target),
        "files": [
            {"name": row["name"], "size": row["size"], "sha256": row["sha256"]}
            for row in files
        ],
    }


def _validate_existing_provision_manifest(path: Path, expected: Mapping[str, Any]) -> list[str]:
    if not path.exists():
        return []
    if not path.is_file():
        return [f"{PROVISION_MANIFEST_NAME} 不是普通文件"]
    try:
        current = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return [f"现有 {PROVISION_MANIFEST_NAME} 无法解析: {exc}"]
    if not isinstance(current, dict):
        return [f"现有 {PROVISION_MANIFEST_NAME} 顶层必须是对象"]
    errors: list[str] = []
    for key in ("schemaVersion", "profileId", "targetRoot", "files"):
        if current.get(key) != expected.get(key):
            errors.append(f"现有 {PROVISION_MANIFEST_NAME} 的 {key} 与本次计划不符")
    return errors


def provision_ydhost_assets(
    source_root: Path,
    sandbox_root: Path,
    target_root: Path | None = None,
    *,
    apply: bool = False,
    required_files: Mapping[str, str] = YDHOST_REQUIRED_FILES,
) -> dict[str, Any]:
    """将已审计运行时资产显式配置到沙盒；默认只返回 dry-run 计划。

    ``map.cfg`` 与 ``ydhost.cfg`` 不属于不可变资产：二者必须在选定地图后由
    YDWE ``mapdump`` 与版本化配置步骤生成，所以这里不会复制 YDWE 目录里
    可能已经过期的示例配置。
    """

    source = _lexical_absolute(source_root)
    sandbox = _lexical_absolute(sandbox_root)
    target = _lexical_absolute(target_root) if target_root else _lexical_absolute(
        sandbox / "plugin" / "ydhost"
    )
    result: dict[str, Any] = {
        "ok": False,
        "dryRun": not apply,
        "applied": False,
        "profileId": YDHOST_PROFILE_ID,
        "sourceRoot": str(source),
        "sandboxRoot": str(sandbox),
        "targetRoot": str(target),
        "operations": [],
        "errors": [],
    }
    if not sandbox.is_dir():
        result["errors"].append("专用 AutoTest 沙盒不存在")
    else:
        try:
            assert_safe_path(sandbox, sandbox, "专用 AutoTest 沙盒")
        except ValueError as exc:
            result["errors"].append(str(exc))
    if target not in _allowed_provision_targets(sandbox):
        result["errors"].append("目标只能是专用沙盒的 plugin/ydhost 或 KKWE插件/plugin/ydhost")
    else:
        try:
            assert_safe_path(target, sandbox, "ydhost provision 目标")
        except ValueError as exc:
            result["errors"].append(str(exc))
    if not source.is_dir():
        result["errors"].append("ydhost 源资产目录不存在")
    else:
        try:
            _assert_safe_external_tree(source, "ydhost 源资产目录")
        except ValueError as exc:
            result["errors"].append(str(exc))

    source_rows: list[dict[str, Any]] = []
    for name, expected_sha in required_files.items():
        source_path = source / name
        if not source_path.is_file():
            result["errors"].append(f"源资产缺少 {name}")
            continue
        try:
            _assert_safe_external_tree(source_path, f"ydhost 源资产 {name}")
        except ValueError as exc:
            result["errors"].append(str(exc))
            continue
        actual_sha = sha256_file(source_path)
        row = {
            "name": name,
            "source": str(source_path),
            "target": str(target / name),
            "size": source_path.stat().st_size,
            "sha256": actual_sha,
        }
        source_rows.append(row)
        if actual_sha.casefold() != expected_sha.casefold():
            result["errors"].append(f"源资产 {name} SHA-256 与审计版本不符")

    expected_manifest = _provision_manifest_payload(source, target, source_rows)
    manifest_path = target / PROVISION_MANIFEST_NAME
    if target.exists() and not target.is_dir():
        result["errors"].append("ydhost 目标路径已存在但不是目录")
    if target.is_dir():
        for candidate in target.iterdir():
            if candidate.is_file() and candidate.suffix.casefold() in {".exe", ".dll"}:
                if candidate.name not in required_files:
                    result["errors"].append(f"目标含未审计二进制，拒绝覆盖: {candidate.name}")
        try:
            assert_safe_path(manifest_path, sandbox, "ydhost provision manifest")
            result["errors"].extend(_validate_existing_provision_manifest(manifest_path, expected_manifest))
        except ValueError as exc:
            result["errors"].append(str(exc))

    for row in source_rows:
        destination = Path(row["target"])
        try:
            assert_safe_path(destination, sandbox, f"ydhost 目标文件 {row['name']}")
        except ValueError as exc:
            result["errors"].append(str(exc))
            continue
        if destination.exists() and not destination.is_file():
            result["errors"].append(f"目标 {row['name']} 已存在但不是普通文件")
            continue
        if destination.is_file():
            target_sha = sha256_file(destination)
            if target_sha.casefold() != row["sha256"].casefold():
                result["errors"].append(f"目标 {row['name']} 已漂移，拒绝覆盖")
                continue
            result["operations"].append({"action": "keep", **row})
        else:
            result["operations"].append({"action": "copy", **row})
    if manifest_path.is_file():
        result["operations"].append({"action": "keep-manifest", "target": str(manifest_path)})
    else:
        result["operations"].append({"action": "write-manifest", "target": str(manifest_path)})

    if result["errors"]:
        return result
    result["ok"] = True
    result["manifestPreview"] = expected_manifest
    if not apply:
        return result

    with pin_nearest_existing_parent(target, sandbox, "ydhost provision 目标创建"):
        assert_safe_path(target, sandbox, "ydhost provision 目标（mkdir 前复查）")
        secure_makedirs(target, sandbox, "ydhost provision 目标")
        assert_safe_path(target, sandbox, "ydhost provision 目标（mkdir 后复查）")
    for row in source_rows:
        destination = Path(row["target"])
        if destination.is_file():
            continue
        _assert_safe_external_tree(Path(row["source"]), f"ydhost 源文件 {row['name']}")
        assert_safe_path(destination, sandbox, f"ydhost 目标文件 {row['name']}")
        with pin_directory(target, sandbox, "ydhost provision 目标目录"):
            fd, temporary_name = tempfile.mkstemp(prefix=f".{row['name']}.", suffix=".tmp", dir=target)
            os.close(fd)
            temporary = Path(temporary_name)
            try:
                shutil.copy2(Path(row["source"]), temporary)
                if sha256_file(temporary).casefold() != row["sha256"].casefold():
                    raise OSError(f"临时副本 {row['name']} SHA-256 校验失败")
                assert_safe_path(target, sandbox, "ydhost provision 目标（replace 前复查）")
                assert_safe_path(temporary, sandbox, f"ydhost 临时文件 {row['name']}")
                assert_safe_path(destination, sandbox, f"ydhost 目标文件 {row['name']}（replace 前复查）")
                os.replace(temporary, destination)
            finally:
                if temporary.exists():
                    temporary.unlink()

    manifest_to_write = dict(expected_manifest)
    manifest_to_write["provisionedAtUtc"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    if not manifest_path.exists():
        with pin_directory(target, sandbox, "ydhost manifest 目标目录"):
            fd, temporary_name = tempfile.mkstemp(prefix=".provision-manifest.", suffix=".tmp", dir=target)
            os.close(fd)
            temporary = Path(temporary_name)
            try:
                temporary.write_text(
                    json.dumps(manifest_to_write, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                assert_safe_path(target, sandbox, "ydhost manifest 目标（replace 前复查）")
                assert_safe_path(temporary, sandbox, "ydhost manifest 临时文件")
                assert_safe_path(manifest_path, sandbox, "ydhost manifest 目标文件")
                os.replace(temporary, manifest_path)
            finally:
                if temporary.exists():
                    temporary.unlink()

    post = audit_ydhost_bundle(target, sandbox, required_files)
    if not post.get("ok"):
        raise OSError(f"ydhost provision 后校验失败: {post.get('errors')}")
    result.update({"applied": True, "postAudit": post})
    return result


def _mapdump_snapshot_paths(root: Path) -> list[Path]:
    """列出隔离执行所需的封闭工具树；宽包含以覆盖 sandbox 内部动态 require。"""

    rows: list[Path] = [root / "bin" / "lua.exe"]
    rows.extend(sorted((root / "bin").glob("*.dll"), key=lambda item: item.name.casefold()))
    for directory in (
        root / "bin" / "modules",
        root / "compiler" / "script" / "w3x2lni",
        root / "plugin" / "w3x2lni",
        root / "share",
    ):
        if directory.is_dir():
            rows.extend(_safe_tree_files(directory, f"mapdump snapshot tree {directory.name}"))
    rows.extend(
        (
            root / "script" / "ydwe" / "mapdump.lua",
            root / "compiler" / "jass" / "24" / "common.j",
            root / "compiler" / "jass" / "24" / "Blizzard.j",
        )
    )
    unique: dict[str, Path] = {}
    for path in rows:
        relative = path.relative_to(root).as_posix()
        unique[relative] = path
    return [unique[key] for key in sorted(unique)]


def _catalog_digest(rows: Sequence[Mapping[str, Any]]) -> str:
    digest = hashlib.sha256()
    for row in sorted(rows, key=lambda item: str(item["relativePath"])):
        digest.update(str(row["relativePath"]).encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(int(row["size"])).encode("ascii"))
        digest.update(b"\0")
        digest.update(str(row["sha256"]).casefold().encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def _catalog_rows(root: Path, relative_paths: Sequence[str] | None = None) -> list[dict[str, Any]]:
    paths = (
        [root / Path(relative) for relative in relative_paths]
        if relative_paths is not None
        else _mapdump_snapshot_paths(root)
    )
    rows: list[dict[str, Any]] = []
    for path in paths:
        _assert_safe_external_tree(path, "mapdump 工具依赖")
        if not path.is_file():
            raise FileNotFoundError(path)
        relative = path.relative_to(root).as_posix()
        rows.append(
            {
                "relativePath": relative,
                "path": str(path),
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    return sorted(rows, key=lambda item: item["relativePath"])


def audit_mapdump_toolchain(ydwe_root: Path) -> dict[str, Any]:
    """校验 434 文件封闭 catalog，而非只检查入口二进制。"""

    root = _lexical_absolute(ydwe_root)
    result: dict[str, Any] = {
        "ok": False,
        "profileId": MAPDUMP_PROFILE_ID,
        "root": str(root),
        "expectedCatalogSha256": MAPDUMP_CATALOG_SHA256,
        "expectedFileCount": MAPDUMP_CATALOG_FILE_COUNT,
        "files": [],
        "errors": [],
    }
    try:
        _assert_safe_external_tree(root, "mapdump YDWE 工具树")
        rows = _catalog_rows(root)
    except (OSError, ValueError) as exc:
        result["errors"].append(str(exc))
        return result
    catalog_sha = _catalog_digest(rows)
    result.update(
        {
            "files": rows,
            "fileCount": len(rows),
            "catalogSha256": catalog_sha,
        }
    )
    if len(rows) != MAPDUMP_CATALOG_FILE_COUNT:
        result["errors"].append(
            f"mapdump catalog 文件数漂移: {len(rows)} != {MAPDUMP_CATALOG_FILE_COUNT}"
        )
    if catalog_sha.casefold() != MAPDUMP_CATALOG_SHA256:
        result["errors"].append("mapdump catalog SHA-256 与审计版本不符")
    result["ok"] = not result["errors"]
    return result


def _materialize_mapdump_snapshot(
    source_root: Path,
    snapshot_root: Path,
    sandbox_root: Path,
    audited: Mapping[str, Any],
) -> dict[str, Any]:
    """从活树复制到沙盒快照并逐文件复核，执行阶段只接触该快照。"""

    source = _lexical_absolute(source_root)
    sandbox = _lexical_absolute(sandbox_root)
    snapshot = assert_safe_path(snapshot_root, sandbox, "mapdump 隔离快照")
    if snapshot.exists() and any(snapshot.iterdir()):
        raise ValueError(f"mapdump 隔离快照非空: {snapshot}")
    with pin_nearest_existing_parent(snapshot, sandbox, "mapdump 隔离快照创建"):
        secure_makedirs(snapshot, sandbox, "mapdump 隔离快照")
        assert_safe_path(snapshot, sandbox, "mapdump 隔离快照（mkdir 后复查）")
    copied: list[dict[str, Any]] = []
    with pin_directory(snapshot, sandbox, "mapdump 隔离快照目录"):
        for row in audited.get("files", []):
            relative = str(row["relativePath"])
            source_path = source / Path(relative)
            destination = snapshot / Path(relative)
            _assert_safe_external_tree(source_path, f"mapdump 源依赖 {relative}")
            assert_safe_path(destination, sandbox, f"mapdump 快照依赖 {relative}")
            with pin_nearest_existing_parent(
                destination.parent,
                sandbox,
                f"mapdump 快照父目录创建 {relative}",
            ):
                secure_makedirs(destination.parent, sandbox, f"mapdump 快照父目录 {relative}")
                assert_safe_path(destination.parent, sandbox, f"mapdump 快照父目录 {relative}")
            with pin_directory(destination.parent, sandbox, f"mapdump 快照固定父目录 {relative}"):
                shutil.copy2(source_path, destination)
                assert_safe_path(destination, sandbox, f"mapdump 快照依赖 {relative}（复制后）")
                actual_sha = sha256_file(destination)
            if actual_sha.casefold() != str(row["sha256"]).casefold():
                raise OSError(f"mapdump 快照复制后 SHA-256 不符: {relative}")
            copied.append(
                {
                    "relativePath": relative,
                    "path": str(destination),
                    "size": destination.stat().st_size,
                    "sha256": actual_sha,
                }
            )
    snapshot_sha = _catalog_digest(copied)
    if len(copied) != MAPDUMP_CATALOG_FILE_COUNT or snapshot_sha.casefold() != MAPDUMP_CATALOG_SHA256:
        raise OSError("mapdump 隔离快照 catalog 校验失败")
    return {
        "ok": True,
        "root": str(snapshot),
        "fileCount": len(copied),
        "catalogSha256": snapshot_sha,
        "files": copied,
    }


_MAPDUMP_RUNNER = r'''local ydwe = arg[1]
local sandbox = arg[2]
local input = arg[3]
local jass = arg[4]
local output = arg[5]

package.cpath = ydwe .. [[\bin\modules\?.dll;]] .. ydwe .. [[\bin\?.dll]]
package.path = table.concat({
    ydwe .. [[\script\?.lua]],
    ydwe .. [[\script\?\init.lua]],
    ydwe .. [[\compiler\script\?.lua]],
    ydwe .. [[\compiler\script\?\init.lua]],
    ydwe .. [[\plugin\w3x2lni\script\?.lua]],
    ydwe .. [[\plugin\w3x2lni\script\?\init.lua]],
}, ';')

require 'filesystem'
fs.__ydwe_path = fs.path(ydwe)
fs.__ydwe_devpath = fs.path(ydwe)
fs.__war3_path = fs.path(sandbox)
function fs.ydwe_path()
    return fs.__ydwe_path
end
function fs.ydwe_devpath()
    return fs.__ydwe_devpath
end
function fs.war3_path()
    return fs.__war3_path
end

io.__open = io.open
local function path_string(path)
    if type(path) == 'string' then
        return path
    end
    return path:string()
end
function io.open(path, mode)
    return io.__open(path_string(path), mode)
end
function io.load(path)
    local stream, open_error = io.open(path, 'rb')
    if not stream then
        return nil, open_error
    end
    local content = stream:read('*a')
    stream:close()
    return content
end

package.preload['compiler.w3x2lni.data_load'] = function ()
    return assert(loadfile(ydwe .. [[\compiler\script\w3x2lni\data_load.lua]]))()
end
package.preload['compiler.w3x2lni.init'] = function ()
    return assert(loadfile(ydwe .. [[\compiler\script\w3x2lni\init.lua]]))()
end

local mapdump_chunk, load_error = loadfile(ydwe .. [[\script\ydwe\mapdump.lua]])
if not mapdump_chunk then
    io.stderr:write(tostring(load_error) .. '\n')
    os.exit(2)
end
local mapdump = mapdump_chunk()
local stream, open_error = io.__open(output, 'wb')
if not stream then
    io.stderr:write(tostring(open_error) .. '\n')
    os.exit(2)
end
local ok, dump_error = xpcall(function ()
    mapdump(fs.path(input), fs.path(jass), function (line)
        stream:write(line .. '\n')
    end)
end, debug.traceback)
stream:close()
if not ok then
    io.stderr:write(tostring(dump_error) .. '\n')
    os.exit(3)
end
io.stdout:write('YDWE mapdump: success\n')
'''


def _get_win32_system_directory(api_name: str) -> Path:
    """通过 Win32 API 获取系统目录，不信任父进程环境变量。"""

    if os.name != "nt":
        raise OSError("ydhost mapdump 只支持 Windows")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    function = getattr(kernel32, api_name)
    function.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    function.restype = wintypes.UINT
    capacity = 260
    while capacity <= 32768:
        buffer = ctypes.create_unicode_buffer(capacity)
        written = int(function(buffer, capacity))
        if written == 0:
            raise OSError(ctypes.get_last_error(), f"{api_name} 失败")
        if written < capacity:
            return _lexical_absolute(Path(buffer.value))
        capacity = written + 1
    raise OSError(f"{api_name} 返回路径过长")


def _validate_trusted_system_directory(path: Path, label: str) -> Path:
    if not Path(path).is_absolute():
        raise OSError(f"{label} 不是绝对路径: {path}")
    directory = _assert_safe_external_tree(path, label)
    if not directory.is_dir():
        raise OSError(f"{label} 不存在或不是目录: {directory}")
    return directory


def _trusted_windows_directories() -> tuple[Path, Path]:
    windows = _validate_trusted_system_directory(
        _get_win32_system_directory("GetWindowsDirectoryW"),
        "Windows 目录",
    )
    system = _validate_trusted_system_directory(
        _get_win32_system_directory("GetSystemDirectoryW"),
        "Windows System 目录",
    )
    if not _is_within(system, windows):
        raise OSError(f"Windows System 目录不在 Windows 目录内: {system}")
    return windows, system


def _mapdump_subprocess_environment(
    isolated_ydwe: Path,
    temporary: Path,
) -> tuple[dict[str, str], dict[str, Any]]:
    """构造不继承用户 Lua 注入变量的最小 Windows 子进程环境。"""

    windows, system = _trusted_windows_directories()
    path_entries = (str(isolated_ydwe / "bin"), str(system))
    environment = {
        "SystemRoot": str(windows),
        "WINDIR": str(windows),
        "PATH": os.pathsep.join(path_entries),
        "TEMP": str(temporary),
        "TMP": str(temporary),
        # Lua 5.3 的 -E 是主要门禁；此变量为不支持 -E 的异常运行时提供第二层保护。
        "LUA_NOENV": "1",
    }
    policy = {
        "luaNoEnvFlag": True,
        "parentEnvironmentInherited": False,
        "trustedWindowsDirectory": str(windows),
        "trustedSystemDirectory": str(system),
        "pathEntries": list(path_entries),
        "workingDirectory": str(isolated_ydwe / "bin"),
    }
    return environment, policy


def generate_ydhost_map_metadata(
    ydwe_root: Path,
    sandbox_root: Path,
    map_path: Path,
    *,
    apply: bool = False,
    replace_existing: bool = False,
    timeout: int = 180,
) -> dict[str, Any]:
    """用独立 YDWE Lua/mapdump 读取 MPQ 并生成 SHA 绑定元数据；默认不保留。"""

    ydwe = _lexical_absolute(ydwe_root)
    sandbox = _lexical_absolute(sandbox_root)
    map_file = _lexical_absolute(map_path)
    result: dict[str, Any] = {
        "ok": False,
        "dryRun": not apply,
        "applied": False,
        "ydweRoot": str(ydwe),
        "sandboxRoot": str(sandbox),
        "mapPath": str(map_file),
        "errors": [],
    }
    if replace_existing and not apply:
        result["errors"].append("replace_existing 只能与 apply=True 一起使用")
        return result
    map_path_safe = True
    if not sandbox.is_dir():
        result["errors"].append("专用 AutoTest 沙盒不存在")
        map_path_safe = False
    else:
        try:
            assert_safe_path(sandbox, sandbox, "专用 AutoTest 沙盒")
            assert_safe_path(map_file, sandbox, "mapdump 地图")
        except ValueError as exc:
            result["errors"].append(str(exc))
            map_path_safe = False
    try:
        _assert_safe_external_tree(ydwe, "mapdump YDWE 工具树")
    except ValueError as exc:
        result["errors"].append(str(exc))
    map_result = (
        inspect_map_container(map_file)
        if map_path_safe
        else {"ok": False, "kind": "unsafe-path", "path": str(map_file)}
    )
    result["map"] = map_result
    if not map_result.get("ok"):
        result["errors"].append(str(map_result.get("error", "地图不是实际 MPQ")))
    toolchain = audit_mapdump_toolchain(ydwe)
    result["toolchain"] = toolchain
    if not toolchain.get("ok"):
        result["errors"].extend(toolchain.get("errors", []))
    if result["errors"]:
        return result

    with contextlib.ExitStack() as stack:
        stack.enter_context(pin_directory(sandbox, sandbox, "mapdump 沙盒根"))
        temporary_name = stack.enter_context(
            tempfile.TemporaryDirectory(prefix="ydhost-mapdump-", dir=sandbox)
        )
        temporary = assert_safe_path(Path(temporary_name), sandbox, "mapdump 临时根")
        stack.enter_context(pin_directory(temporary, sandbox, "mapdump 临时根"))
        snapshot_root = temporary / "toolchain-snapshot"
        work = temporary / "work"
        secure_makedirs(work, sandbox, "mapdump work 目录")
        assert_safe_path(work, sandbox, "mapdump work 目录")
        stack.enter_context(pin_directory(work, sandbox, "mapdump work 目录"))
        map_snapshot_directory = temporary / "map-snapshot"
        secure_makedirs(map_snapshot_directory, sandbox, "mapdump 地图快照目录")
        assert_safe_path(map_snapshot_directory, sandbox, "mapdump 地图快照目录")
        stack.enter_context(pin_directory(map_snapshot_directory, sandbox, "mapdump 地图快照目录"))
        map_snapshot = map_snapshot_directory / f"{map_result['sha256']}.w3x"
        assert_safe_path(map_file, sandbox, "mapdump 活地图（复制前复查）")
        shutil.copy2(map_file, map_snapshot)
        assert_safe_path(map_snapshot, sandbox, "mapdump 地图快照")
        if (
            map_snapshot.stat().st_size != int(map_result["size"])
            or sha256_file(map_snapshot).casefold() != str(map_result["sha256"]).casefold()
        ):
            result["errors"].append("mapdump 地图快照与已审计 MPQ 不一致")
            return result
        snapshot = _materialize_mapdump_snapshot(ydwe, snapshot_root, sandbox, toolchain)
        isolated_ydwe = Path(snapshot["root"])
        stack.enter_context(pin_directory(isolated_ydwe, sandbox, "mapdump 工具链快照"))
        snapshot_bin = isolated_ydwe / "bin"
        stack.enter_context(pin_directory(snapshot_bin, sandbox, "mapdump 工具链 bin"))
        lua = snapshot_bin / "lua.exe"
        jass = isolated_ydwe / "compiler" / "jass" / "24"
        runner = work / "run_mapdump.lua"
        cfg = work / "map.cfg"
        runner.write_text(_MAPDUMP_RUNNER, encoding="utf-8")
        assert_safe_path(runner, sandbox, "mapdump runner")
        # 紧贴 CreateProcess 再计算一次快照 catalog，阻断 audit 活树后替换依赖的 TOCTOU。
        snapshot_rows = _catalog_rows(
            isolated_ydwe,
            [str(row["relativePath"]) for row in snapshot["files"]],
        )
        snapshot_recheck_sha = _catalog_digest(snapshot_rows)
        if (
            len(snapshot_rows) != MAPDUMP_CATALOG_FILE_COUNT
            or snapshot_recheck_sha.casefold() != MAPDUMP_CATALOG_SHA256
        ):
            result["errors"].append("mapdump 隔离快照启动前复查失败")
            return result
        for label, required_path in (
            ("mapdump Lua", lua),
            ("mapdump runner", runner),
            ("mapdump 地图快照", map_snapshot),
            ("mapdump runtime24", jass),
            ("mapdump 输出目录", cfg.parent),
            ("mapdump 工作目录", snapshot_bin),
        ):
            assert_safe_path(required_path, sandbox, label)
        command = [
            str(lua),
            "-E",
            str(runner),
            str(isolated_ydwe),
            str(sandbox),
            str(map_snapshot),
            str(jass),
            str(cfg),
        ]
        creation_flags = 0x08000000 if os.name == "nt" else 0
        try:
            subprocess_environment, environment_policy = _mapdump_subprocess_environment(
                isolated_ydwe,
                temporary,
            )
            completed = subprocess.run(
                command,
                cwd=snapshot_bin,
                env=subprocess_environment,
                capture_output=True,
                text=True,
                timeout=max(1, int(timeout)),
                creationflags=creation_flags,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            result["errors"].append(f"YDWE mapdump 执行失败: {exc}")
            return result
        result["execution"] = {
            "command": command,
            "returnCode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "isolatedSnapshot": {
                "fileCount": snapshot["fileCount"],
                "catalogSha256": snapshot_recheck_sha,
            },
            "environmentPolicy": environment_policy,
        }
        assert_safe_path(cfg, sandbox, "mapdump 输出 map.cfg")
        if completed.returncode != 0 or not cfg.is_file():
            result["errors"].append("YDWE mapdump 未成功生成 map.cfg")
            return result
        try:
            parsed = parse_map_cfg(cfg.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, ValueError) as exc:
            result["errors"].append(f"YDWE mapdump 输出无效: {exc}")
            return result
        cfg_sha = sha256_file(cfg)
        manifest = {
            "schemaVersion": METADATA_SCHEMA_VERSION,
            "generator": "YDWE mapdump",
            "generatorSha256": MAPDUMP_SOURCE_SHA256,
            "toolchainProfileId": MAPDUMP_PROFILE_ID,
            "toolchainCatalogSha256": MAPDUMP_CATALOG_SHA256,
            "toolchainFileCount": MAPDUMP_CATALOG_FILE_COUNT,
            "mapPath": str(map_file),
            "mapSha256": map_result["sha256"],
            "mapSize": map_result["size"],
            "mapCfgSha256": cfg_sha,
            "slotCount": sum(1 for key in parsed if _SLOT_RE.fullmatch(key)),
            "toolchainFiles": [
                {
                    "relativePath": row["relativePath"],
                    "size": row["size"],
                    "sha256": row["sha256"],
                }
                for row in snapshot_rows
            ],
        }
        target = _lexical_absolute(sandbox / ".ydhost-metadata" / str(map_result["sha256"]))
        result.update(
            {
                "ok": True,
                "targetRoot": str(target),
                "mapCfgSha256": cfg_sha,
                "mapCfgPreview": cfg.read_text(encoding="utf-8-sig"),
                "manifestPreview": manifest,
            }
        )
        if not apply:
            return result

        manifest_path = target / "manifest.json"
        target_cfg = target / "map.cfg"
        replace_target = False
        legacy_manifest_sha = ""
        if target.exists():
            existing = audit_map_metadata(map_file, target, sandbox)
            if existing.get("ok") and existing.get("mapCfgSha256") == cfg_sha:
                result.update({"applied": True, "operations": ["keep-existing"], "postAudit": existing})
                return result
            if not replace_existing:
                result.update({"ok": False, "errors": ["目标 mapdump 元数据已漂移，拒绝覆盖"]})
                return result
            # 显式升级只接受“同一地图、同一 map.cfg”的旧 manifest。任何语义
            # 漂移都拒绝，避免 replace_existing 退化为任意覆盖开关。
            try:
                assert_safe_path(target, sandbox, "旧 mapdump metadata 目录")
                assert_safe_path(target_cfg, sandbox, "旧 mapdump map.cfg")
                assert_safe_path(manifest_path, sandbox, "旧 mapdump manifest")
                if not target_cfg.is_file() or not manifest_path.is_file():
                    raise ValueError("旧 metadata 缺少 map.cfg/manifest.json")
                legacy_manifest_sha = sha256_file(manifest_path)
                legacy = json.loads(manifest_path.read_text(encoding="utf-8"))
                if sha256_file(target_cfg).casefold() != cfg_sha.casefold():
                    raise ValueError("旧 metadata 的 map.cfg 与新快照输出不一致")
                if str(legacy.get("mapSha256", "")).casefold() != str(map_result["sha256"]).casefold():
                    raise ValueError("旧 metadata 绑定了不同地图 SHA-256")
                if legacy.get("mapSize") != map_result["size"]:
                    raise ValueError("旧 metadata 绑定了不同地图大小")
                if str(legacy.get("mapCfgSha256", "")).casefold() != cfg_sha.casefold():
                    raise ValueError("旧 manifest 的 map.cfg SHA-256 不匹配")
            except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
                result.update({"ok": False, "errors": [f"旧 metadata 不满足安全升级前置条件: {exc}"]})
                return result
            replace_target = True

        with pin_nearest_existing_parent(target.parent, sandbox, "mapdump metadata 父目录创建"):
            assert_safe_path(target.parent, sandbox, "mapdump metadata 父目录（mkdir 前复查）")
            secure_makedirs(target.parent, sandbox, "mapdump metadata 父目录")
            assert_safe_path(target.parent, sandbox, "mapdump metadata 父目录（mkdir 后复查）")
        with pin_directory(target.parent, sandbox, "mapdump metadata 固定父目录"):
            staging = Path(tempfile.mkdtemp(prefix=f".{map_result['sha256'][:12]}-", dir=target.parent))
            backup: Path | None = None
            try:
                assert_safe_path(staging, sandbox, "mapdump metadata 临时目录")
                shutil.copy2(cfg, staging / "map.cfg")
                (staging / "manifest.json").write_text(
                    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                staged_audit = audit_map_metadata(map_file, staging, sandbox)
                if not staged_audit.get("ok"):
                    raise OSError(f"临时 mapdump 元数据校验失败: {staged_audit.get('errors')}")
                assert_safe_path(target.parent, sandbox, "mapdump metadata 父目录（replace 前复查）")
                assert_safe_path(staging, sandbox, "mapdump metadata 临时目录（replace 前复查）")
                assert_safe_path(target, sandbox, "mapdump metadata 目标（replace 前复查）")
                if replace_target:
                    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                    backup = target.parent / (
                        f".{map_result['sha256']}.backup-{stamp}-{legacy_manifest_sha[:12]}"
                    )
                    assert_safe_path(backup, sandbox, "旧 mapdump metadata 备份")
                    if backup.exists():
                        raise OSError("旧 metadata 备份目标已存在")
                    os.replace(target, backup)
                    try:
                        os.replace(staging, target)
                    except Exception:
                        os.replace(backup, target)
                        raise
                else:
                    os.replace(staging, target)
                assert_safe_path(target, sandbox, "mapdump metadata 目标（replace 后复查）")
            finally:
                if staging.exists():
                    assert_safe_path(staging, sandbox, "mapdump metadata 临时目录（清理前）")
                    shutil.rmtree(staging)
        post = audit_map_metadata(map_file, target, sandbox)
        if not post.get("ok"):
            raise OSError(f"mapdump 元数据落地后校验失败: {post.get('errors')}")
        result.update(
            {
                "applied": True,
                "operations": (
                    ([f"backup {backup}"] if backup is not None else [])
                    + [f"write {target_cfg}", f"write {manifest_path}"]
                ),
                "backupRoot": str(backup) if backup is not None else None,
                "postAudit": post,
            }
        )
        return result


def discover_archived_ydhost_bundles(
    sandbox_root: Path,
    required_files: Mapping[str, str] = YDHOST_REQUIRED_FILES,
    required_sizes: Mapping[str, int] | None = None,
) -> list[dict[str, Any]]:
    """只检查沙盒根目录的 ``YDWE*.zip``，不解压也不执行其中内容。"""

    sandbox = _lexical_absolute(sandbox_root)
    sizes = dict(
        required_sizes
        if required_sizes is not None
        else (YDHOST_REQUIRED_SIZES if required_files is YDHOST_REQUIRED_FILES else {})
    )
    results: list[dict[str, Any]] = []
    if not sandbox.is_dir():
        return results
    try:
        assert_safe_path(sandbox, sandbox, "ydhost archive 沙盒")
    except ValueError:
        return results
    for archive_path in sorted(sandbox.glob("YDWE*.zip"), key=lambda item: item.name.casefold()):
        row: dict[str, Any] = {
            "archive": str(archive_path),
            "size": archive_path.stat().st_size,
            "files": [],
            "errors": [],
        }
        try:
            assert_safe_path(archive_path, sandbox, "ydhost archive")
            with zipfile.ZipFile(archive_path) as archive:
                entries: dict[str, zipfile.ZipInfo] = {}
                for info in archive.infolist():
                    normalized = info.filename.replace("\\", "/")
                    for name in required_files:
                        suffix = f"/plugin/ydhost/{name}".casefold()
                        if normalized.casefold().endswith(suffix):
                            if name in entries:
                                row["errors"].append(f"压缩包内 {name} 不唯一")
                            else:
                                entries[name] = info
                for name, expected_sha in required_files.items():
                    info = entries.get(name)
                    file_row: dict[str, Any] = {
                        "name": name,
                        "expectedSha256": expected_sha,
                        "exists": info is not None,
                    }
                    if info is None:
                        file_row["shaMatches"] = False
                        row["errors"].append(f"压缩包缺少 {name}")
                    else:
                        expected_size = sizes.get(name)
                        declared_size = int(info.file_size)
                        compressed_size = int(info.compress_size)
                        file_row.update(
                            {
                                "entry": info.filename,
                                "declaredSize": declared_size,
                                "compressedSize": compressed_size,
                            }
                        )
                        if info.is_dir() or (info.flag_bits & 0x1):
                            file_row["shaMatches"] = False
                            row["errors"].append(f"压缩包内 {name} 是目录或加密项")
                        elif expected_size is not None and declared_size != expected_size:
                            file_row["shaMatches"] = False
                            row["errors"].append(
                                f"压缩包内 {name} 大小不符: {declared_size} != {expected_size}"
                            )
                        elif declared_size < 0 or declared_size > YDHOST_ZIP_MAX_ENTRY_BYTES:
                            file_row["shaMatches"] = False
                            row["errors"].append(f"压缩包内 {name} 超过流式读取上限")
                        elif declared_size > 0 and (
                            compressed_size <= 0
                            or declared_size / compressed_size > YDHOST_ZIP_MAX_COMPRESSION_RATIO
                        ):
                            file_row["shaMatches"] = False
                            row["errors"].append(f"压缩包内 {name} 压缩比异常，疑似 ZIP bomb")
                        else:
                            digest = hashlib.sha256()
                            total = 0
                            with archive.open(info, "r") as stream:
                                while True:
                                    chunk = stream.read(64 * 1024)
                                    if not chunk:
                                        break
                                    total += len(chunk)
                                    if total > declared_size or total > YDHOST_ZIP_MAX_ENTRY_BYTES:
                                        raise zipfile.BadZipFile(f"{name} 实际解压大小越界")
                                    digest.update(chunk)
                            file_row.update({"size": total, "sha256": digest.hexdigest()})
                            file_row["shaMatches"] = (
                                total == declared_size
                                and file_row["sha256"].casefold() == expected_sha.casefold()
                            )
                            if not file_row["shaMatches"]:
                                row["errors"].append(f"压缩包内 {name} 大小或 SHA-256 不匹配")
                    row["files"].append(file_row)
        except (OSError, zipfile.BadZipFile) as exc:
            row["errors"].append(f"无法读取压缩包: {exc}")
        row["complete"] = not row["errors"]
        results.append(row)
    return results


def audit_provision_manifest(bundle: Mapping[str, Any]) -> dict[str, Any]:
    """确认资产目录由显式 provision 步骤创建，且目标内容未漂移。"""

    root = _lexical_absolute(Path(str(bundle.get("root", ""))))
    path = root / PROVISION_MANIFEST_NAME
    result: dict[str, Any] = {"path": str(path), "errors": []}
    if not bundle.get("ok"):
        result["errors"].append("ydhost 二进制资产校验未通过")
        result["ok"] = False
        return result
    try:
        _assert_safe_external_tree(root, "ydhost provision 资产目录")
        if not root.is_dir():
            raise NotADirectoryError(root)
        _assert_safe_external_tree(path, f"ydhost {PROVISION_MANIFEST_NAME}")
        if not path.is_file():
            raise FileNotFoundError(path)
    except (OSError, ValueError) as exc:
        result["errors"].append(f"{PROVISION_MANIFEST_NAME} 路径不安全: {exc}")
        result["ok"] = False
        return result
    try:
        manifest = json.loads(path.read_text(encoding="utf-8-sig"))
        if not isinstance(manifest, dict):
            raise ValueError("顶层必须是对象")
        result["data"] = manifest
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        result["errors"].append(f"{PROVISION_MANIFEST_NAME} 无效: {exc}")
        result["ok"] = False
        return result
    if manifest.get("schemaVersion") != PROVISION_SCHEMA_VERSION:
        result["errors"].append("provision schemaVersion 不受支持")
    if manifest.get("profileId") != YDHOST_PROFILE_ID:
        result["errors"].append("provision profileId 不匹配")
    if _lexical_absolute(Path(str(manifest.get("targetRoot", "")))) != root:
        result["errors"].append("provision targetRoot 与实际目录不符")
    try:
        expected_files = {
            str(row.get("name")): (int(row.get("size", -1)), str(row.get("sha256", "")).casefold())
            for row in bundle.get("files", [])
        }
    except (AttributeError, TypeError, ValueError) as exc:
        result["errors"].append(f"资产审计 files 无效: {exc}")
        result["ok"] = False
        return result
    actual_rows = manifest.get("files")
    if not isinstance(actual_rows, list):
        result["errors"].append("provision files 必须是数组")
    else:
        try:
            actual_files = {
                str(row.get("name")): (int(row.get("size", -1)), str(row.get("sha256", "")).casefold())
                for row in actual_rows
                if isinstance(row, dict)
            }
            if len(actual_files) != len(actual_rows) or actual_files != expected_files:
                result["errors"].append("provision files 与目标实际内容不符")
        except (AttributeError, TypeError, ValueError) as exc:
            result["errors"].append(f"provision files 无效: {exc}")
    result["ok"] = not result["errors"]
    return result


def inspect_map_container(map_path: Path) -> dict[str, Any]:
    """识别真实 MPQ、w3x2lni 目录占位文件或未知输入。"""

    path = Path(map_path)
    result: dict[str, Any] = {"path": str(_lexical_absolute(path)), "exists": path.is_file()}
    if not path.is_file():
        result.update({"ok": False, "kind": "missing", "error": "地图文件不存在"})
        return result
    with path.open("rb") as stream:
        prefix = stream.read(4096)
    result.update({"size": path.stat().st_size, "sha256": sha256_file(path)})
    if len(prefix) >= 12 and prefix[8:12] == b"W2L\x01":
        result.update(
            {
                "ok": False,
                "kind": "w3x2lni-directory-marker",
                "error": "地图是 W2L 目录占位文件，ydhost 需要实际 MPQ w3x",
            }
        )
        return result
    mpq_offset = prefix.find(b"MPQ\x1a")
    if mpq_offset < 0:
        result.update({"ok": False, "kind": "unknown", "error": "前 4096 字节内没有 MPQ 头"})
        return result
    result.update({"ok": True, "kind": "mpq", "mpqOffset": mpq_offset})
    return result


def parse_map_cfg(text: str) -> dict[str, list[int]]:
    values: dict[str, list[int]] = {}
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if "=" not in line:
            raise ValueError(f"map.cfg 第 {line_number} 行缺少 '='")
        key, raw_values = (part.strip() for part in line.split("=", 1))
        if key in values:
            raise ValueError(f"map.cfg 字段重复: {key}")
        try:
            numbers = [int(value, 10) for value in raw_values.split()]
        except ValueError as exc:
            raise ValueError(f"map.cfg 第 {line_number} 行包含非十进制整数") from exc
        if not numbers or any(value < 0 or value > 255 for value in numbers):
            raise ValueError(f"map.cfg 第 {line_number} 行字节值必须为 0..255")
        values[key] = numbers
    for key, length in _FIXED_MAP_FIELDS.items():
        if len(values.get(key, [])) != length:
            raise ValueError(f"map.cfg {key} 必须包含 {length} 个字节")
    options = values.get("map_options")
    if options is None or len(options) != 1:
        raise ValueError("map.cfg map_options 必须包含 1 个字节")
    slots = sorted(
        (int(match.group(1)), key)
        for key in values
        if (match := _SLOT_RE.fullmatch(key)) is not None
    )
    if not slots or [number for number, _ in slots] != list(range(1, len(slots) + 1)):
        raise ValueError("map.cfg 至少需要一个从 map_slot1 连续编号的槽位")
    for _, key in slots:
        if len(values[key]) != 9:
            raise ValueError(f"map.cfg {key} 必须包含 9 个字节")
    unknown = set(values) - set(_FIXED_MAP_FIELDS) - {"map_options"} - {key for _, key in slots}
    if unknown:
        raise ValueError(f"map.cfg 包含未知字段: {', '.join(sorted(unknown))}")
    return values


def audit_map_metadata(
    map_path: Path,
    metadata_root: Path,
    sandbox_root: Path | None = None,
) -> dict[str, Any]:
    """校验 ``map.cfg`` 与 ``manifest.json`` 是否绑定到当前地图内容。"""

    root = _lexical_absolute(metadata_root)
    cfg = root / "map.cfg"
    manifest_path = root / "manifest.json"
    paths_safe = True
    path_errors: list[str] = []
    if sandbox_root is not None:
        sandbox = _lexical_absolute(sandbox_root)
        try:
            assert_safe_path(map_path, sandbox, "ydhost metadata 地图")
            assert_safe_path(root, sandbox, "ydhost metadata 目录")
            assert_safe_path(cfg, sandbox, "ydhost metadata map.cfg")
            assert_safe_path(manifest_path, sandbox, "ydhost metadata manifest.json")
        except ValueError as exc:
            path_errors.append(str(exc))
            paths_safe = False
    map_result = (
        inspect_map_container(map_path)
        if paths_safe
        else {"ok": False, "kind": "unsafe-path", "path": str(_lexical_absolute(map_path))}
    )
    result: dict[str, Any] = {
        "root": str(root),
        "map": map_result,
        "mapCfg": str(cfg),
        "manifest": str(manifest_path),
        "errors": path_errors,
    }
    if not map_result.get("ok"):
        result["errors"].append(str(map_result.get("error", "地图包无效")))
    if not cfg.is_file():
        result["errors"].append("缺少 map.cfg")
    elif paths_safe:
        try:
            parsed = parse_map_cfg(cfg.read_text(encoding="utf-8-sig"))
            result["slotCount"] = sum(1 for key in parsed if _SLOT_RE.fullmatch(key))
            result["mapCfgSha256"] = sha256_file(cfg)
        except (OSError, UnicodeError, ValueError) as exc:
            result["errors"].append(str(exc))
    manifest: dict[str, Any] | None = None
    if not manifest_path.is_file():
        result["errors"].append("缺少 manifest.json；不能证明 map.cfg 对应当前地图")
    elif paths_safe:
        try:
            loaded = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
            if not isinstance(loaded, dict):
                raise ValueError("manifest.json 顶层必须是对象")
            manifest = loaded
        except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
            result["errors"].append(f"manifest.json 无效: {exc}")
    if manifest is not None:
        result["manifestData"] = manifest
        if manifest.get("schemaVersion") != METADATA_SCHEMA_VERSION:
            result["errors"].append("manifest schemaVersion 不受支持")
        if str(manifest.get("generator", "")) != "YDWE mapdump":
            result["errors"].append("manifest generator 必须是 YDWE mapdump")
        if str(manifest.get("generatorSha256", "")).casefold() != MAPDUMP_SOURCE_SHA256:
            result["errors"].append("manifest generatorSha256 与已审计 mapdump.lua 不符")
        if str(manifest.get("toolchainProfileId", "")) != MAPDUMP_PROFILE_ID:
            result["errors"].append("manifest toolchainProfileId 不是隔离快照 v2")
        if str(manifest.get("toolchainCatalogSha256", "")).casefold() != MAPDUMP_CATALOG_SHA256:
            result["errors"].append("manifest toolchainCatalogSha256 与审计 catalog 不符")
        if manifest.get("toolchainFileCount") != MAPDUMP_CATALOG_FILE_COUNT:
            result["errors"].append("manifest toolchainFileCount 不符")
        toolchain_rows = manifest.get("toolchainFiles")
        if not isinstance(toolchain_rows, list):
            result["errors"].append("manifest toolchainFiles 必须是数组")
        else:
            try:
                normalized_rows = [
                    {
                        "relativePath": str(row["relativePath"]),
                        "size": int(row["size"]),
                        "sha256": str(row["sha256"]).casefold(),
                    }
                    for row in toolchain_rows
                    if isinstance(row, dict)
                ]
                if len(normalized_rows) != len(toolchain_rows):
                    raise ValueError("存在非对象项")
                if len({row["relativePath"] for row in normalized_rows}) != len(normalized_rows):
                    raise ValueError("relativePath 重复")
                if len(normalized_rows) != MAPDUMP_CATALOG_FILE_COUNT:
                    raise ValueError("文件数不符")
                if _catalog_digest(normalized_rows).casefold() != MAPDUMP_CATALOG_SHA256:
                    raise ValueError("catalog digest 不符")
            except (KeyError, TypeError, ValueError) as exc:
                result["errors"].append(f"manifest toolchainFiles 无效: {exc}")
        if map_result.get("sha256") and str(manifest.get("mapSha256", "")).casefold() != str(
            map_result["sha256"]
        ).casefold():
            result["errors"].append("manifest mapSha256 与地图不符")
        if map_result.get("size") and manifest.get("mapSize") != map_result["size"]:
            result["errors"].append("manifest mapSize 与地图不符")
        if result.get("mapCfgSha256") and str(manifest.get("mapCfgSha256", "")).casefold() != str(
            result["mapCfgSha256"]
        ).casefold():
            result["errors"].append("manifest mapCfgSha256 与 map.cfg 不符")
        if result.get("slotCount") is not None and manifest.get("slotCount") != result["slotCount"]:
            result["errors"].append("manifest slotCount 与 map.cfg 不符")
    result["ok"] = not result["errors"]
    return result


def preflight_ydhost_lan(
    sandbox_root: Path,
    map_path: Path,
    asset_roots: Sequence[Path] | None = None,
    metadata_root: Path | None = None,
) -> dict[str, Any]:
    """组合门禁；只报告就绪度，永远不启动外部进程。"""

    sandbox = _lexical_absolute(sandbox_root)
    try:
        assert_safe_path(sandbox, sandbox, "ydhost LAN 沙盒")
        assert_safe_path(map_path, sandbox, "ydhost LAN 地图")
    except ValueError as exc:
        return {
            "ok": False,
            "status": "blocked",
            "code": "YDHOST_SANDBOX_REPARSE_REJECTED",
            "error": str(exc),
            "countsAsLanAcceptance": False,
        }
    roots = list(asset_roots or (sandbox / "plugin" / "ydhost", sandbox / "KKWE插件" / "plugin" / "ydhost"))
    bundle_results = [audit_ydhost_bundle(root, sandbox) for root in roots]
    for bundle in bundle_results:
        bundle["provision"] = audit_provision_manifest(bundle)
    valid_bundles = [
        result for result in bundle_results if result.get("ok") and result["provision"].get("ok")
    ]
    archives: list[dict[str, Any]] = []
    if not valid_bundles:
        archives = discover_archived_ydhost_bundles(sandbox)
        if any(result.get("complete") for result in archives):
            code = "YDHOST_ASSETS_ARCHIVED_ONLY"
            error = "沙盒压缩包内有已审计 ydhost 资产，但运行目录仍为空；不会隐式解压或执行"
        elif any((Path(result["root"]).exists() for result in bundle_results)):
            code = "YDHOST_ASSETS_INVALID"
            error = "沙盒 ydhost 目录不完整或 SHA-256 与审计版本不符"
        else:
            code = "YDHOST_ASSETS_MISSING"
            error = "专用沙盒中没有可审计的 ydhost 资产"
        return {
            "ok": False,
            "status": "blocked",
            "code": code,
            "error": error,
            "profileId": YDHOST_PROFILE_ID,
            "bundles": bundle_results,
            "archives": archives,
            "countsAsLanAcceptance": False,
        }

    map_result = inspect_map_container(map_path)
    if not map_result.get("ok"):
        return {
            "ok": False,
            "status": "blocked",
            "code": "YDHOST_MAP_NOT_PACKAGED",
            "error": map_result.get("error"),
            "profileId": YDHOST_PROFILE_ID,
            "bundle": valid_bundles[0],
            "map": map_result,
            "countsAsLanAcceptance": False,
        }

    metadata = Path(metadata_root) if metadata_root is not None else sandbox / ".ydhost-metadata" / str(
        map_result["sha256"]
    )
    metadata_result = audit_map_metadata(map_path, metadata, sandbox)
    if not metadata_result.get("ok"):
        metadata_exists = Path(metadata).is_dir()
        return {
            "ok": False,
            "status": "blocked",
            "code": "YDHOST_MAP_METADATA_INVALID" if metadata_exists else "YDHOST_MAP_METADATA_MISSING",
            "error": (
                "现有 YDWE mapdump 元数据未通过隔离快照 v2 校验"
                if metadata_exists
                else "缺少与地图 SHA-256 绑定的 YDWE mapdump 元数据"
            ),
            "profileId": YDHOST_PROFILE_ID,
            "bundle": valid_bundles[0],
            "map": map_result,
            "metadata": metadata_result,
            "countsAsLanAcceptance": False,
        }

    return {
        "ok": True,
        "status": "ready-for-launch-adapter",
        "code": "YDHOST_PREFLIGHT_READY",
        "profileId": YDHOST_PROFILE_ID,
        "bundle": valid_bundles[0],
        "map": map_result,
        "metadata": metadata_result,
        "launchExecuted": False,
        "countsAsLanAcceptance": False,
    }


def _main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="YDWE ydhost 资产/地图元数据 fail-closed 前置检查")
    subparsers = parser.add_subparsers(dest="command", required=True)

    provision_parser = subparsers.add_parser("provision", help="把哈希锁定资产显式配置到专用沙盒")
    provision_parser.add_argument("--source", required=True, type=Path)
    provision_parser.add_argument("--sandbox", required=True, type=Path)
    provision_parser.add_argument("--target", type=Path)
    provision_parser.add_argument("--apply", action="store_true", help="实际写入；缺省只 dry-run")

    preflight_parser = subparsers.add_parser("preflight", help="只读检查资产、地图和 mapdump 元数据")
    preflight_parser.add_argument("--sandbox", required=True, type=Path)
    preflight_parser.add_argument("--map", required=True, type=Path)
    preflight_parser.add_argument("--metadata", type=Path)

    mapdump_parser = subparsers.add_parser("mapdump", help="用哈希锁定的 YDWE mapdump 生成地图元数据")
    mapdump_parser.add_argument("--ydwe", required=True, type=Path)
    mapdump_parser.add_argument("--sandbox", required=True, type=Path)
    mapdump_parser.add_argument("--map", required=True, type=Path)
    mapdump_parser.add_argument("--timeout", type=int, default=180)
    mapdump_parser.add_argument("--apply", action="store_true", help="保留到 .ydhost-metadata；缺省临时执行")
    mapdump_parser.add_argument(
        "--replace-existing",
        action="store_true",
        help="仅当旧 metadata 的地图与 map.cfg SHA 完全一致时，原子备份并升级 manifest",
    )

    args = parser.parse_args(argv)
    if args.command == "provision":
        result = provision_ydhost_assets(args.source, args.sandbox, args.target, apply=args.apply)
    elif args.command == "preflight":
        result = preflight_ydhost_lan(args.sandbox, args.map, metadata_root=args.metadata)
    else:
        result = generate_ydhost_map_metadata(
            args.ydwe,
            args.sandbox,
            args.map,
            apply=args.apply,
            replace_existing=args.replace_existing,
            timeout=args.timeout,
        )
    json.dump(result, sys.stdout, ensure_ascii=False, indent=2)
    sys.stdout.write("\n")
    return 0 if result.get("ok") else 3


if __name__ == "__main__":
    raise SystemExit(_main())
