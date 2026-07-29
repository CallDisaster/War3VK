#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Warcraft III AutoTest 多实例会话与文件隔离基础设施。

这个模块刻意不依赖 MCP 或 Win32 API，便于独立测试。进程、Desktop 与
Job Object 的创建仍由 :mod:`war3_autotest_mcp` 负责；这里负责路径边界、
实例布局、内容寻址地图、会话索引和 DBWIN 事件分发。
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional


DEFAULT_SANDBOX_ROOT = Path(r"E:\Work\War3_AutoTestSandbox")
DEFAULT_INSTANCE_POOL_NAME = "_AutoTestInstances"
MAX_SESSION_EVENTS = 4000

_IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
_REQUIRED_ROOT_FILES = ("war3.exe", "Game.dll", "war3.mpq", "war3x.mpq", "war3patch.mpq")
_MUTABLE_TOP_LEVEL_DIRS = (
    "Crash",
    "crashes",
    "Errors",
    "FLK",
    "Logger",
    "Logs",
    "MemoryReports",
    "Replay",
    "Save",
)
_RUNTIME_DIRS = (
    ".dz_res",
    "jass_plugin",
    "MemHack",
    "Movies",
    "plugin",
    "redist",
    "scripts",
    "StormBreaker",
    "TestGame",
    "WarVK",
)
_RUNTIME_ROOT_SUFFIXES = {
    ".ax",
    ".cfg",
    ".dll",
    ".exe",
    ".ini",
    ".json",
    ".mix",
    ".reg",
    ".toml",
}
_EXCLUDED_DIR_NAMES = {
    "__pycache__",
    "Crash",
    "crashes",
    "Errors",
    "Log",
    "Logger",
    "Logs",
    "MemoryReports",
    "Replay",
    "Save",
    "Temp",
}


def utc_now_text() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def normalize_identifier(value: str, prefix: str) -> str:
    """校验用户可见 ID；空值生成稳定可读的随机 ID。"""
    text = str(value or "").strip()
    if not text:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        text = f"{prefix}_{stamp}_{uuid.uuid4().hex[:8]}"
    if not _IDENTIFIER_RE.fullmatch(text):
        raise ValueError(f"非法 {prefix}：仅允许 1-64 位字母、数字、点、下划线和连字符")
    return text


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        while True:
            chunk = stream.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def is_path_within(path: Path, root: Path, allow_root: bool = True) -> bool:
    """使用解析后的路径做大小写无关边界判断，防止 ``..``/前缀绕过。"""
    candidate = Path(path).resolve(strict=False)
    boundary = Path(root).resolve(strict=False)
    try:
        common = Path(os.path.commonpath([str(candidate), str(boundary)]))
    except ValueError:
        return False
    same = os.path.normcase(str(common)) == os.path.normcase(str(boundary))
    if not same:
        return False
    if not allow_root and os.path.normcase(str(candidate)) == os.path.normcase(str(boundary)):
        return False
    return True


def require_path_within(path: Path, root: Path, label: str, allow_root: bool = False) -> Path:
    resolved = Path(path).resolve(strict=False)
    if not is_path_within(resolved, root, allow_root=allow_root):
        raise ValueError(f"{label} 越过允许边界: {resolved}（边界: {Path(root).resolve(strict=False)}）")
    return resolved


@dataclass(frozen=True)
class InstanceLayout:
    sandbox_root: Path
    pool_root: Path
    run_id: str
    session_id: str
    instance_root: Path
    artifact_dir: Path
    desktop_name: str

    def to_dict(self) -> Dict[str, Any]:
        return {
            "sandboxRoot": str(self.sandbox_root),
            "poolRoot": str(self.pool_root),
            "runId": self.run_id,
            "sessionId": self.session_id,
            "instanceRoot": str(self.instance_root),
            "artifactDir": str(self.artifact_dir),
            "desktopName": self.desktop_name,
        }


def build_instance_layout(
    sandbox_root: Path,
    artifact_root: Path,
    run_id: str = "",
    session_id: str = "",
) -> InstanceLayout:
    sandbox = Path(sandbox_root).resolve(strict=False)
    run = normalize_identifier(run_id, "run")
    session = normalize_identifier(session_id, "session")
    pool = sandbox / DEFAULT_INSTANCE_POOL_NAME
    instance = require_path_within(pool / run / session / "root", pool, "instance_root")
    artifacts = require_path_within(
        Path(artifact_root).resolve(strict=False) / run / session,
        Path(artifact_root).resolve(strict=False),
        "artifact_dir",
    )
    desktop = f"War3AutoTest_{run}_{session}"[:120]
    return InstanceLayout(
        sandbox_root=sandbox,
        pool_root=pool,
        run_id=run,
        session_id=session,
        instance_root=instance,
        artifact_dir=artifacts,
        desktop_name=desktop,
    )


def preflight_instance_pool(
    sandbox_root: Path,
    artifact_root: Path,
    map_path: Optional[Path],
    instance_count: int,
    run_id: str = "",
    session_prefix: str = "client",
) -> Dict[str, Any]:
    """只读预检；不会创建实例目录或修改沙盒。"""
    sandbox = Path(sandbox_root).resolve(strict=False)
    count = int(instance_count)
    errors: List[str] = []
    warnings: List[str] = []
    if count < 1 or count > 6:
        errors.append("instance_count 必须在 1..6 范围内")

    missing = [name for name in _REQUIRED_ROOT_FILES if not (sandbox / name).is_file()]
    if not sandbox.is_dir():
        errors.append(f"沙盒目录不存在: {sandbox}")
    elif missing:
        errors.append(f"沙盒缺少必需文件: {', '.join(missing)}")

    source_map = Path(map_path).resolve(strict=False) if map_path else None
    map_hash = ""
    if source_map is not None:
        if not source_map.is_file():
            errors.append(f"地图不存在: {source_map}")
        else:
            map_hash = sha256_file(source_map)

    try:
        run = normalize_identifier(run_id, "run")
        prefix = normalize_identifier(session_prefix, "session_prefix")
        if len(prefix) > 60:
            raise ValueError("session_prefix 最长为 60 位（需为批次序号保留空间）")
    except ValueError as exc:
        errors.append(str(exc))
        run = "invalid"
        prefix = "client"

    pool = sandbox / DEFAULT_INSTANCE_POOL_NAME
    layouts: List[Dict[str, Any]] = []
    if 1 <= count <= 6 and run != "invalid":
        for index in range(1, count + 1):
            session = f"{prefix}-{index:02d}"
            layout = build_instance_layout(sandbox, artifact_root, run, session)
            row = layout.to_dict()
            row["exists"] = layout.instance_root.exists()
            layouts.append(row)
            if layout.instance_root.exists():
                warnings.append(f"实例目录已存在，启动时默认拒绝覆盖: {layout.instance_root}")

    try:
        disk = shutil.disk_usage(sandbox if sandbox.exists() else sandbox.parent)
        free_bytes = int(disk.free)
    except OSError:
        free_bytes = 0
        warnings.append("无法读取沙盒所在卷的剩余空间")

    copy_bytes_per_instance = 0
    hardlink_bytes_per_instance = 0
    if sandbox.is_dir():
        try:
            # 预检必须保持快速，只统计根目录运行文件；子目录按固定安全预算计入。
            for source in sandbox.iterdir():
                if not source.is_file():
                    continue
                suffix = source.suffix.lower()
                if suffix == ".mpq":
                    hardlink_bytes_per_instance += int(source.stat().st_size)
                elif suffix in _RUNTIME_ROOT_SUFFIXES:
                    copy_bytes_per_instance += int(source.stat().st_size)
        except OSError as exc:
            warnings.append(f"无法完整估算实例文件体积: {exc}")
    map_bytes = int(source_map.stat().st_size) if source_map is not None and source_map.is_file() else 0
    runtime_directory_reserve_per_instance = 1024 * 1024 * 1024
    estimated_write_bytes = (
        copy_bytes_per_instance + runtime_directory_reserve_per_instance + map_bytes
    ) * max(0, count)
    reserve_bytes = 1024 * 1024 * 1024
    if free_bytes > 0 and estimated_write_bytes + reserve_bytes > free_bytes:
        errors.append(
            f"空间不足：预计写入 {estimated_write_bytes} 字节，且必须保留 {reserve_bytes} 字节"
        )

    return {
        "ok": not errors,
        "sandboxRoot": str(sandbox),
        "poolRoot": str(pool),
        "artifactRoot": str(Path(artifact_root).resolve(strict=False)),
        "runId": run,
        "instanceCount": count,
        "sourceMap": str(source_map) if source_map else "",
        "mapSha256": map_hash,
        "freeBytes": free_bytes,
        "copyBytesPerInstance": copy_bytes_per_instance,
        "hardlinkBytesPerInstance": hardlink_bytes_per_instance,
        "runtimeDirectoryReservePerInstance": runtime_directory_reserve_per_instance,
        "estimatedWriteBytes": estimated_write_bytes,
        "requiredReserveBytes": reserve_bytes,
        "sameVolumeForHardlinks": os.path.splitdrive(str(sandbox))[0].lower()
        == os.path.splitdrive(str(pool))[0].lower(),
        "layouts": layouts,
        "errors": errors,
        "warnings": warnings,
    }


def _iter_runtime_files(sandbox_root: Path) -> Iterable[tuple[Path, Path]]:
    """列举需要复制到实例的运行时文件，排除历史日志、IDA 数据和编辑器。"""
    sandbox = Path(sandbox_root)
    for source in sandbox.iterdir():
        if not source.is_file():
            continue
        suffix = source.suffix.lower()
        if suffix == ".mpq" or suffix in _RUNTIME_ROOT_SUFFIXES:
            yield source, Path(source.name)

    for top_name in _RUNTIME_DIRS:
        top = sandbox / top_name
        if not top.is_dir():
            continue
        for current, dir_names, file_names in os.walk(top):
            dir_names[:] = [name for name in dir_names if name not in _EXCLUDED_DIR_NAMES]
            current_path = Path(current)
            for file_name in file_names:
                source = current_path / file_name
                yield source, source.relative_to(sandbox)


def materialize_instance_root(layout: InstanceLayout, reuse_existing: bool = False) -> Dict[str, Any]:
    """创建独立运行根：MPQ 使用硬链接，其余运行时文件独立复制。"""
    root = require_path_within(layout.instance_root, layout.pool_root, "instance_root")
    if root.exists() and any(root.iterdir()) and not reuse_existing:
        return {"ok": False, "error": f"实例目录非空，拒绝覆盖: {root}"}

    root.mkdir(parents=True, exist_ok=True)
    layout.artifact_dir.mkdir(parents=True, exist_ok=True)
    linked = 0
    copied = 0
    skipped = 0
    for source, relative in _iter_runtime_files(layout.sandbox_root):
        destination = require_path_within(root / relative, root, "instance file")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            skipped += 1
            continue
        try:
            if source.suffix.lower() == ".mpq":
                os.link(source, destination)
                linked += 1
            else:
                shutil.copy2(source, destination)
                copied += 1
        except OSError as exc:
            return {
                "ok": False,
                "error": f"实例文件部署失败: {source} -> {destination}: {exc}",
                "linked": linked,
                "copied": copied,
            }

    for relative in _MUTABLE_TOP_LEVEL_DIRS:
        (root / relative).mkdir(parents=True, exist_ok=True)
    for relative in (Path("Maps") / "AutoTest", Path("WarVK") / "Log", Path("WarVK") / "Temp"):
        (root / relative).mkdir(parents=True, exist_ok=True)
    return {
        "ok": True,
        "instanceRoot": str(root),
        "artifactDir": str(layout.artifact_dir),
        "hardlinkedMpqFiles": linked,
        "copiedFiles": copied,
        "reusedFiles": skipped,
    }


def deploy_content_addressed_map(instance_root: Path, source_map: Path) -> Dict[str, Any]:
    source = Path(source_map).resolve(strict=True)
    root = Path(instance_root).resolve(strict=True)
    digest = sha256_file(source)
    safe_name = source.name if source.suffix.lower() in {".w3x", ".w3m"} else f"{source.name}.w3x"
    relative = Path("Maps") / "AutoTest" / digest[:16] / safe_name
    destination = require_path_within(root / relative, root, "map destination")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or sha256_file(destination) != digest:
        shutil.copy2(source, destination)
    return {
        "ok": True,
        "source": str(source),
        "target": str(destination),
        "loadfileArg": str(relative).replace("/", "\\"),
        "sha256": digest,
    }


@dataclass
class AutoTestSession:
    session_id: str
    run_id: str
    sandbox_root: Path
    instance_root: Path
    artifact_dir: Path
    desktop_name: str = ""
    desktop_handle: int = 0
    desktop_mode: str = "default"
    pid: int = 0
    process: Any = field(default=None, repr=False, compare=False)
    process_created_at_ms: int = 0
    job_handle: int = 0
    map_source: Optional[Path] = None
    map_path: Optional[Path] = None
    map_sha256: str = ""
    dll_sha256: str = ""
    state: str = "reserved"
    created_at: str = field(default_factory=utc_now_text)
    stopped_at: str = ""
    stop_reason: str = ""
    managed_root: bool = True
    debug_events: List[Dict[str, Any]] = field(default_factory=list, repr=False)
    debug_seq: int = 0
    _debug_lock: threading.Lock = field(default_factory=threading.Lock, repr=False, compare=False)

    def push_event(self, pid: int, message: str) -> None:
        with self._debug_lock:
            self.debug_seq += 1
            self.debug_events.append(
                {
                    "id": self.debug_seq,
                    "ts": utc_now_text(),
                    "pid": int(pid),
                    "sessionId": self.session_id,
                    "msg": str(message).strip(),
                }
            )
            if len(self.debug_events) > MAX_SESSION_EVENTS:
                self.debug_events = self.debug_events[-MAX_SESSION_EVENTS:]

    def get_events(self, since_id: int, limit: int, contains: str = "") -> List[Dict[str, Any]]:
        with self._debug_lock:
            rows = [row.copy() for row in self.debug_events if int(row["id"]) > int(since_id)]
        needle = str(contains or "").lower()
        if needle:
            rows = [row for row in rows if needle in str(row.get("msg", "")).lower()]
        return rows[: max(1, min(int(limit), 1000))]

    def to_dict(self, alive: Optional[bool] = None) -> Dict[str, Any]:
        row = {
            "sessionId": self.session_id,
            "runId": self.run_id,
            "pid": int(self.pid),
            "processCreatedAtMs": int(self.process_created_at_ms),
            "state": self.state,
            "createdAt": self.created_at,
            "stoppedAt": self.stopped_at,
            "stopReason": self.stop_reason,
            "sandboxRoot": str(self.sandbox_root),
            "instanceRoot": str(self.instance_root),
            "artifactDir": str(self.artifact_dir),
            "desktopMode": self.desktop_mode,
            "desktopName": self.desktop_name,
            "desktopHandle": int(self.desktop_handle),
            "jobHandle": int(self.job_handle),
            "mapSource": str(self.map_source) if self.map_source else "",
            "mapPath": str(self.map_path) if self.map_path else "",
            "mapSha256": self.map_sha256,
            "dllSha256": self.dll_sha256,
            "debugEvents": len(self.debug_events),
            "managedRoot": bool(self.managed_root),
        }
        if alive is not None:
            row["alive"] = bool(alive)
        return row


class SessionRegistry:
    """线程安全的 session_id/PID 双向索引。"""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._sessions: Dict[str, AutoTestSession] = {}
        self._pid_index: Dict[int, str] = {}

    def reserve(self, session: AutoTestSession) -> AutoTestSession:
        with self._lock:
            if session.session_id in self._sessions:
                raise ValueError(f"session_id 已存在: {session.session_id}")
            if session.pid > 0 and session.pid in self._pid_index:
                raise ValueError(f"pid 已被其他会话占用: {session.pid}")
            self._sessions[session.session_id] = session
            if session.pid > 0:
                self._pid_index[int(session.pid)] = session.session_id
            return session

    def attach_pid(self, session_id: str, pid: int, process: Any = None) -> AutoTestSession:
        with self._lock:
            session = self.require(session_id)
            target_pid = int(pid)
            owner = self._pid_index.get(target_pid)
            if owner and owner != session_id:
                raise ValueError(f"pid {target_pid} 已属于会话 {owner}")
            if session.pid > 0:
                self._pid_index.pop(int(session.pid), None)
            session.pid = target_pid
            session.process = process
            session.process_created_at_ms = int(time.time() * 1000)
            session.state = "running"
            self._pid_index[target_pid] = session_id
            return session

    def get(self, session_id: str = "", pid: int = 0) -> Optional[AutoTestSession]:
        with self._lock:
            if session_id:
                return self._sessions.get(str(session_id))
            if int(pid) > 0:
                owner = self._pid_index.get(int(pid))
                return self._sessions.get(owner) if owner else None
            return None

    def require(self, session_id: str = "", pid: int = 0) -> AutoTestSession:
        session = self.get(session_id=session_id, pid=pid)
        if session is None:
            selector = f"session_id={session_id}" if session_id else f"pid={pid}"
            raise KeyError(f"未找到 AutoTest 会话: {selector}")
        return session

    def list(self, run_id: str = "", include_stopped: bool = True) -> List[AutoTestSession]:
        with self._lock:
            rows = list(self._sessions.values())
        if run_id:
            rows = [row for row in rows if row.run_id == run_id]
        if not include_stopped:
            rows = [row for row in rows if row.state not in {"stopped", "orphaned", "failed"}]
        return sorted(rows, key=lambda row: (row.created_at, row.session_id))

    def mark_stopped(self, session_id: str, reason: str, state: str = "stopped") -> AutoTestSession:
        with self._lock:
            session = self.require(session_id)
            if session.pid > 0:
                self._pid_index.pop(int(session.pid), None)
            session.state = state
            session.stopped_at = utc_now_text()
            session.stop_reason = str(reason)
            return session

    def remove(self, session_id: str) -> Optional[AutoTestSession]:
        with self._lock:
            session = self._sessions.pop(session_id, None)
            if session and session.pid > 0:
                self._pid_index.pop(int(session.pid), None)
            return session

    def route_event(self, pid: int, message: str) -> bool:
        session = self.get(pid=int(pid))
        if session is None:
            return False
        session.push_event(int(pid), message)
        return True

    def cleanup_orphans(self, is_alive: Callable[[int], bool]) -> List[AutoTestSession]:
        cleaned: List[AutoTestSession] = []
        for session in self.list(include_stopped=False):
            if session.pid > 0 and not is_alive(int(session.pid)):
                cleaned.append(self.mark_stopped(session.session_id, "process no longer exists", state="orphaned"))
        return cleaned
