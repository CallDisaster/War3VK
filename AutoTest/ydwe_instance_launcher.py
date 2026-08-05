#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""为 Win32 YDWE instance helper 生成 fail-closed 启动请求。

本模块永远不调用 ``subprocess``，也不启动 Warcraft、YDWE 或注入器。默认
``execute=False``；即使显式 opt-in 且预检全部通过，也只返回不可执行的审计 argv。
OwnedJob capability 接入 SessionRegistry、依赖绝对预载与 late-runtime/ready 生命周期
闭环完成前，接口固定 ``launchable=False``。
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import re
import stat
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

from autotest_sessions import DEFAULT_INSTANCE_POOL_NAME, DEFAULT_SANDBOX_ROOT


SCHEMA_VERSION = "ydwe-instance-launch-request-v1"
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,119}$")
_RESERVED_WAR3_ARGS = {
    "-auto", "--auto", "/auto",
    "-window", "--window", "/window",
    "-loadfile", "--loadfile", "/loadfile",
    "-opengl", "--opengl", "/opengl",
    "-ydwe", "--ydwe", "/ydwe",
    "-war3", "--war3", "/war3",
    "-closew2l", "--closew2l", "/closew2l",
}
_MAX_PASSTHROUGH_ARGUMENTS = 128
_MAX_PASSTHROUGH_ARGUMENT_CHARACTERS = 4096
_MAX_PASSTHROUGH_TOTAL_CHARACTERS = 30000


@dataclass(frozen=True)
class RuntimeHashes:
    helper: str
    war3: str
    game: str
    lua_engine: str
    yd_loader: str
    lua53: str
    ydbase: str
    war3_main: str
    plugin_config: str


BindingProbe = Callable[[str, str], dict[str, Any]]


def _sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def _path_is_reparse(path: Path) -> bool:
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    attributes = int(getattr(info, "st_file_attributes", 0))
    is_junction = getattr(path, "is_junction", None)
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)) or path.is_symlink() or bool(
        callable(is_junction) and is_junction()
    )


def _lexical_absolute(path: Path | str) -> Path:
    text = os.fspath(path)
    if any(character in text for character in (';', '"')) or any(ord(character) < 0x20 for character in text):
        raise ValueError(f"路径包含 PATH/命令行不安全字符: {text!r}")
    return Path(os.path.abspath(os.path.expanduser(text)))


def _assert_no_reparse_chain(path: Path | str, label: str) -> Path:
    absolute = _lexical_absolute(path)
    for candidate in reversed((absolute, *absolute.parents)):
        if (candidate.exists() or _path_is_reparse(candidate)) and _path_is_reparse(candidate):
            raise ValueError(f"{label} 路径链包含 symlink/junction/reparse point: {candidate}")
    return absolute


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(str(left)) == os.path.normcase(str(right))


def _require_within(path: Path, root: Path, label: str, *, allow_root: bool = False) -> Path:
    candidate = _assert_no_reparse_chain(path, label)
    boundary = _assert_no_reparse_chain(root, f"{label} boundary")
    try:
        common = Path(os.path.commonpath([str(candidate), str(boundary)]))
    except ValueError as exc:
        raise ValueError(f"{label} 与允许根不在同一卷") from exc
    if not _same_path(common, boundary) or (not allow_root and _same_path(candidate, boundary)):
        raise ValueError(f"{label} 越过允许根: {candidate}（root={boundary}）")
    return candidate


def _require_directory(path: Path, label: str) -> Path:
    safe = _assert_no_reparse_chain(path, label)
    if not safe.is_dir():
        raise ValueError(f"{label} 不存在或不是目录: {safe}")
    return safe


def _read_pe_identity(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"不是 PE 文件: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset < 0x40 or pe_offset + 24 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError(f"PE header 越界或签名无效: {path}")
    machine, _, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", data, pe_offset + 4)
    optional_offset = pe_offset + 24
    if optional_size < 2 or optional_offset + optional_size > len(data):
        raise ValueError(f"PE optional header 越界: {path}")
    magic = struct.unpack_from("<H", data, optional_offset)[0]
    return {
        "machine": machine,
        "optionalMagic": magic,
        "isDll": bool(characteristics & 0x2000),
        "executableImage": bool(characteristics & 0x0002),
    }


def _audit_file(path: Path, expected_sha: str, label: str, *, pe_dll: bool | None) -> dict[str, Any]:
    safe = _assert_no_reparse_chain(path, label)
    if not safe.is_file():
        raise ValueError(f"缺少 {label}: {safe}")
    if not _SHA256_RE.fullmatch(str(expected_sha or "")):
        raise ValueError(f"{label} expected SHA-256 非法")
    actual_sha = _sha256_file(safe)
    if actual_sha.casefold() != expected_sha.casefold():
        raise ValueError(f"{label} SHA-256 不匹配: {actual_sha}")
    row: dict[str, Any] = {
        "label": label,
        "path": str(safe),
        "size": safe.stat().st_size,
        "sha256": actual_sha,
    }
    if pe_dll is not None:
        pe = _read_pe_identity(safe)
        if pe["machine"] != 0x014C or pe["optionalMagic"] != 0x010B or not pe["executableImage"]:
            raise ValueError(f"{label} 不是 x86 PE32 executable image")
        if bool(pe["isDll"]) != pe_dll:
            raise ValueError(f"{label} DLL/EXE characteristics 不匹配")
        row["pe"] = pe
    return row


def _validate_binding_names(job_name: str, desktop_name: str) -> None:
    if not job_name.startswith("Local\\War3AutoTest_"):
        raise ValueError("job_name 必须使用 Local\\War3AutoTest_ 前缀")
    suffix = job_name[len("Local\\War3AutoTest_") :]
    if not _IDENTIFIER_RE.fullmatch(suffix):
        raise ValueError("job_name 后缀不是严格标识符")
    if not _IDENTIFIER_RE.fullmatch(desktop_name) or not desktop_name.startswith("War3AutoTest_"):
        raise ValueError("desktop_name 必须是 War3AutoTest_ 开头的严格标识符")


def probe_named_bindings(job_name: str, desktop_name: str) -> dict[str, Any]:
    """只读打开既有 Job/Desktop；不创建、修改或关闭调用方持有的原句柄。"""

    if os.name != "nt":
        return {"ok": False, "error": "named Job/Desktop probe 仅支持 Windows"}
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32.OpenJobObjectW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    kernel32.OpenJobObjectW.restype = ctypes.c_void_p
    kernel32.QueryInformationJobObject.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
    ]
    kernel32.QueryInformationJobObject.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    user32.OpenDesktopW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    user32.OpenDesktopW.restype = ctypes.c_void_p
    user32.CloseDesktop.argtypes = [ctypes.c_void_p]
    user32.CloseDesktop.restype = ctypes.c_int

    job = kernel32.OpenJobObjectW(0x0001 | 0x0004, 0, job_name)
    if not job:
        return {"ok": False, "jobExists": False, "desktopExists": False,
                "error": f"OpenJobObjectW failed: {ctypes.get_last_error()}"}
    desktop = None
    try:
        class BasicLimit(ctypes.Structure):
            _fields_ = [
                ("perProcessUserTime", ctypes.c_longlong),
                ("perJobUserTime", ctypes.c_longlong),
                ("limitFlags", ctypes.c_uint32),
                ("minimumWorkingSet", ctypes.c_size_t),
                ("maximumWorkingSet", ctypes.c_size_t),
                ("activeProcessLimit", ctypes.c_uint32),
                ("affinity", ctypes.c_size_t),
                ("priorityClass", ctypes.c_uint32),
                ("schedulingClass", ctypes.c_uint32),
            ]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_ulonglong) for name in (
                "readOperationCount", "writeOperationCount", "otherOperationCount",
                "readTransferCount", "writeTransferCount", "otherTransferCount",
            )]

        class ExtendedLimit(ctypes.Structure):
            _fields_ = [
                ("basic", BasicLimit), ("io", IoCounters),
                ("processMemoryLimit", ctypes.c_size_t), ("jobMemoryLimit", ctypes.c_size_t),
                ("peakProcessMemoryUsed", ctypes.c_size_t), ("peakJobMemoryUsed", ctypes.c_size_t),
            ]

        class BasicAccounting(ctypes.Structure):
            _fields_ = [
                ("totalUserTime", ctypes.c_longlong), ("totalKernelTime", ctypes.c_longlong),
                ("thisPeriodUserTime", ctypes.c_longlong), ("thisPeriodKernelTime", ctypes.c_longlong),
                ("totalPageFaultCount", ctypes.c_uint32), ("totalProcesses", ctypes.c_uint32),
                ("activeProcesses", ctypes.c_uint32), ("totalTerminatedProcesses", ctypes.c_uint32),
            ]

        limits = ExtendedLimit()
        accounting = BasicAccounting()
        if not kernel32.QueryInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits), None):
            return {"ok": False, "jobExists": True, "desktopExists": False,
                    "error": f"QueryInformationJobObject(limit) failed: {ctypes.get_last_error()}"}
        if not kernel32.QueryInformationJobObject(job, 1, ctypes.byref(accounting), ctypes.sizeof(accounting), None):
            return {"ok": False, "jobExists": True, "desktopExists": False,
                    "error": f"QueryInformationJobObject(accounting) failed: {ctypes.get_last_error()}"}
        desktop = user32.OpenDesktopW(desktop_name, 0, 0, 0x0002 | 0x0040 | 0x0001 | 0x0080)
        if not desktop:
            return {"ok": False, "jobExists": True, "desktopExists": False,
                    "error": f"OpenDesktopW failed: {ctypes.get_last_error()}"}
        return {
            "ok": True,
            "jobExists": True,
            "desktopExists": True,
            "jobKillOnClose": bool(limits.basic.limitFlags & 0x00002000),
            "jobActiveProcesses": int(accounting.activeProcesses),
            "helperWillRevalidateKillOnCloseAndEmptyJob": True,
        }
    finally:
        if desktop:
            user32.CloseDesktop(desktop)
        kernel32.CloseHandle(job)


def _reserved_passthrough(arguments: Iterable[str]) -> list[str]:
    rejected: list[str] = []
    for value in arguments:
        folded = str(value).casefold()
        if folded in _RESERVED_WAR3_ARGS or any(folded.startswith(item + "=") for item in _RESERVED_WAR3_ARGS):
            rejected.append(str(value))
    return rejected


def _validate_passthrough(arguments: Sequence[str]) -> tuple[str, ...]:
    normalized = tuple(str(value) for value in arguments)
    if len(normalized) > _MAX_PASSTHROUGH_ARGUMENTS:
        raise ValueError("透传参数数量超过 128 项安全上限")
    if any("\0" in value for value in normalized):
        raise ValueError("透传参数不得包含 NUL")
    if any(len(value) > _MAX_PASSTHROUGH_ARGUMENT_CHARACTERS for value in normalized):
        raise ValueError("单项透传参数超过 4096 字符安全上限")
    if sum(len(value) + 1 for value in normalized) > _MAX_PASSTHROUGH_TOTAL_CHARACTERS:
        raise ValueError("透传参数总长度超过 30000 字符安全上限")
    rejected = _reserved_passthrough(normalized)
    if rejected:
        raise ValueError("调用方不得透传 helper 保留参数: " + ", ".join(rejected))
    return normalized


def prepare_ydwe_instance_launch(
    *,
    sandbox_root: Path,
    instance_root: Path,
    ydwe_root: Path,
    helper_path: Path,
    trusted_helper_root: Path,
    hashes: RuntimeHashes,
    job_name: str,
    desktop_name: str,
    passthrough_arguments: Sequence[str] = (),
    execute: bool = False,
    windowed: bool = True,
    injection_timeout_ms: int = 15000,
    binding_probe: BindingProbe | None = None,
) -> dict[str, Any]:
    """验证并生成只读审计 argv；本函数不授权、也不执行该 argv。"""

    result: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "ok": False,
        "launchable": False,
        "executeOptIn": bool(execute),
        "auditCommandGenerated": False,
        "executionRequestGenerated": False,
        "realProcessLaunchExecuted": False,
        "countsAsLanAcceptance": False,
        "launcherRegistryInstallPathReadOrWritten": False,
        "ydweWrapperWillBeStarted": False,
        "blockers": [],
    }
    try:
        sandbox = _require_directory(sandbox_root, "sandbox_root")
        required_sandbox = _lexical_absolute(DEFAULT_SANDBOX_ROOT)
        if not _same_path(sandbox, required_sandbox):
            raise ValueError(f"只允许专用 AutoTest sandbox: {required_sandbox}")
        pool = _require_directory(sandbox / DEFAULT_INSTANCE_POOL_NAME, "instance_pool")
        instance = _require_within(instance_root, pool, "instance_root")
        instance = _require_directory(instance, "instance_root")
        ydwe = _require_within(ydwe_root, instance, "ydwe_root")
        ydwe = _require_directory(ydwe, "ydwe_root")
        helper_boundary = _require_directory(trusted_helper_root, "trusted_helper_root")
        helper = _require_within(helper_path, helper_boundary, "helper_path")
        _validate_binding_names(job_name, desktop_name)
        if injection_timeout_ms < 1000 or injection_timeout_ms > 60000:
            raise ValueError("injection_timeout_ms 必须位于 1000..60000")
        passthrough = _validate_passthrough(passthrough_arguments)

        files = [
            _audit_file(helper, hashes.helper, "helper", pe_dll=False),
            _audit_file(instance / "war3.exe", hashes.war3, "war3.exe", pe_dll=False),
            _audit_file(instance / "Game.dll", hashes.game, "Game.dll", pe_dll=True),
            _audit_file(ydwe / "bin" / "LuaEngine.dll", hashes.lua_engine, "LuaEngine.dll", pe_dll=True),
            _audit_file(ydwe / "plugin" / "warcraft3" / "yd_loader.dll", hashes.yd_loader,
                        "yd_loader.dll", pe_dll=True),
            _audit_file(ydwe / "bin" / "lua53.dll", hashes.lua53, "lua53.dll", pe_dll=True),
            _audit_file(ydwe / "bin" / "ydbase.dll", hashes.ydbase, "ydbase.dll", pe_dll=True),
            _audit_file(ydwe / "script" / "war3" / "main.lua", hashes.war3_main,
                        "war3/main.lua", pe_dll=None),
            _audit_file(ydwe / "plugin" / "warcraft3" / "config.cfg", hashes.plugin_config,
                        "warcraft3/config.cfg", pe_dll=None),
        ]
        probe = (binding_probe or probe_named_bindings)(job_name, desktop_name)
        if (not probe.get("ok") or not probe.get("jobExists") or not probe.get("desktopExists") or
                not probe.get("jobKillOnClose") or int(probe.get("jobActiveProcesses", -1)) != 0):
            raise ValueError("named Job/Desktop 证据不足: " + str(probe.get("error", probe)))

        command = [
            str(helper),
            "--war3-root", str(instance),
            "--ydwe-root", str(ydwe),
            "--desktop", desktop_name,
            "--job-name", job_name,
            "--war3-sha256", hashes.war3,
            "--game-sha256", hashes.game,
            "--lua-engine-sha256", hashes.lua_engine,
            "--yd-loader-sha256", hashes.yd_loader,
            "--lua53-sha256", hashes.lua53,
            "--ydbase-sha256", hashes.ydbase,
            "--war3-main-sha256", hashes.war3_main,
            "--plugin-config-sha256", hashes.plugin_config,
            "--inject-timeout-ms", str(int(injection_timeout_ms)),
        ]
        if windowed:
            command.append("--windowed")
        command.append("--")
        command.extend(passthrough)
        result.update({
            "identity": {"files": files, "helperSha256": hashes.helper},
            "bindingEvidence": probe,
            "bindingEvidenceAuthoritativeForExecution": False,
            "sandboxRoot": str(sandbox),
            "instanceRoot": str(instance),
            "ydweRoot": str(ydwe),
            "helperPath": str(helper),
            "jobName": job_name,
            "desktopName": desktop_name,
            "command": command,
            "commandContract": {
                "helperInjectsExactlyOneAuto": True,
                "helperOwnsYdweArgument": True,
                "loadfileForbiddenForLan": True,
                "openglForbidden": True,
                "windowed": bool(windowed),
                "childCwd": str(instance),
                "childEnvironmentDelta": {
                    "PATH": f"prepend {ydwe / 'bin'}",
                    "ydwe-process-name": "war3",
                },
            },
            "auditCommandGenerated": True,
        })

        if not execute:
            result["code"] = "DRY_RUN_ONLY"
            result["blockers"].append("execute opt-in 未启用；只返回审计计划")
            return result
        result["code"] = "EXECUTION_BRIDGE_NOT_PRODUCTION_READY"
        result["blockers"].extend([
            "OwnedJob capability 尚未接入 SessionRegistry，且 owner HANDLE 必须贯穿整个 session 生命周期",
            "尚未以绝对路径预载并核验 lua53.dll 与 ydbase.dll 的实际远程模块来源",
            "late runtime closure、ready 前文件句柄保持与逐 PID ready watchdog 尚未完成",
        ])
        return result
    except (OSError, ValueError, struct.error) as exc:
        result["code"] = "PREFLIGHT_REJECTED"
        result["blockers"].append(str(exc))
        return result


__all__ = [
    "RuntimeHashes",
    "prepare_ydwe_instance_launch",
    "probe_named_bindings",
]
