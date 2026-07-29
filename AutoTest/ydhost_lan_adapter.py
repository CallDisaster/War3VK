#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ydhost LAN 启动状态机。

这里实现协议顺序、证据门禁和清理语义，但不绑定任何真实进程 API。真实启动器、
时钟和探针全部由依赖注入提供，因此单元测试不会启动 ydhost、Warcraft III 或
World Editor。当前生产入口还会因客户端 ``YDWE.exe -war3 -auto`` 与现有
``war3.exe -loadfile`` 启动链不等价而 fail-closed。
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from threading import RLock
from typing import Any, Callable, Mapping, Protocol, Sequence

from autotest_sessions import normalize_identifier
from ydhost_adapter import assert_safe_path, sha256_file
from ydwe_lan_protocol import (
    CLIENT_PROTOCOL_BLOCKER as YDWE_CLIENT_PROTOCOL_BLOCKER,
    describe_protocol as describe_ydwe_protocol,
    identity_contract as ydwe_identity_contract,
)


CLIENT_PROTOCOL_BLOCKER = "YDHOST_CLIENT_LAUNCH_PROTOCOL_UNRESOLVED"


class LanState(str, Enum):
    PREPARING = "preparing"
    HOST_STARTED = "host-started"
    HOST_LISTENING = "host-listening"
    CLIENTS_STARTING = "clients-starting"
    WAITING_FOR_JOIN = "waiting-for-join"
    RUNNING = "running"
    STOPPING = "stopping"
    STOPPED = "stopped"
    FAILED = "failed"


@dataclass(frozen=True)
class LanLaunchSpec:
    run_id: str
    instance_count: int
    session_prefix: str = "client"
    timeout_seconds: float = 60.0
    poll_interval_seconds: float = 0.1


@dataclass(frozen=True)
class PreparedLanRun:
    run_id: str
    sandbox_root: Path
    run_root: Path
    artifact_root: Path
    ydhost_cfg: Path
    map_cfg: Path
    evidence: Mapping[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class HostHandle:
    pid: int
    job_id: str
    hidden: bool


@dataclass(frozen=True)
class ClientHandle:
    session_id: str
    pid: int
    job_id: str
    desktop_name: str


@dataclass(frozen=True)
class LanProbeSnapshot:
    host_alive: bool
    listening: bool = False
    game_created: bool = False
    client_alive: frozenset[str] = frozenset()
    joined_clients: frozenset[str] = frozenset()
    ready_clients: frozenset[str] = frozenset()
    listening_evidence: str = ""
    game_created_evidence: str = ""
    joined_evidence: Mapping[str, str] = field(default_factory=dict)
    ready_evidence: Mapping[str, str] = field(default_factory=dict)
    raw: Mapping[str, Any] = field(default_factory=dict)


class Clock(Protocol):
    def monotonic(self) -> float: ...

    def sleep(self, seconds: float) -> None: ...


class SystemClock:
    def monotonic(self) -> float:
        return time.monotonic()

    def sleep(self, seconds: float) -> None:
        time.sleep(seconds)


@dataclass(frozen=True)
class LanDependencies:
    prepare: Callable[[LanLaunchSpec], PreparedLanRun]
    launch_host: Callable[[PreparedLanRun], HostHandle]
    launch_client: Callable[[PreparedLanRun, int, str], ClientHandle]
    probe: Callable[[PreparedLanRun, HostHandle, Sequence[ClientHandle]], LanProbeSnapshot]
    stop_client: Callable[[PreparedLanRun, ClientHandle, str], Mapping[str, Any]]
    stop_host: Callable[[PreparedLanRun, HostHandle, str], Mapping[str, Any]]
    record_event: Callable[[PreparedLanRun, Mapping[str, Any]], None] = lambda _run, _event: None
    acceptance_authorized: bool = False


@dataclass
class LanRunRecord:
    spec: LanLaunchSpec
    prepared: PreparedLanRun
    host: HostHandle
    clients: dict[str, ClientHandle]
    state: LanState
    events: list[dict[str, Any]] = field(default_factory=list)
    host_stopped: bool = False


class LanRunRegistry:
    def __init__(self) -> None:
        self._lock = RLock()
        self._runs: dict[str, LanRunRecord] = {}

    def add(self, record: LanRunRecord) -> None:
        with self._lock:
            if record.spec.run_id in self._runs:
                raise ValueError(f"LAN run_id 已存在: {record.spec.run_id}")
            self._runs[record.spec.run_id] = record

    def get(self, run_id: str) -> LanRunRecord | None:
        with self._lock:
            return self._runs.get(run_id)

    def require(self, run_id: str) -> LanRunRecord:
        record = self.get(run_id)
        if record is None:
            raise KeyError(f"未找到 LAN run: {run_id}")
        return record

    def remove(self, run_id: str) -> LanRunRecord | None:
        with self._lock:
            return self._runs.pop(run_id, None)


class YdhostLanCoordinator:
    def __init__(
        self,
        dependencies: LanDependencies,
        *,
        clock: Clock | None = None,
        registry: LanRunRegistry | None = None,
    ) -> None:
        self.dependencies = dependencies
        self.clock = clock or SystemClock()
        self.registry = registry or LanRunRegistry()

    @staticmethod
    def _validate_spec(spec: LanLaunchSpec) -> None:
        if not spec.run_id:
            raise ValueError("run_id 不能为空")
        if normalize_identifier(spec.run_id, "run") != spec.run_id:
            raise ValueError("run_id 必须是未经修剪的严格标识符")
        if normalize_identifier(spec.session_prefix, "session_prefix") != spec.session_prefix:
            raise ValueError("session_prefix 必须是未经修剪的严格标识符")
        if spec.instance_count < 2 or spec.instance_count > 6:
            raise ValueError("ydhost LAN 仅允许 2-6 个客户端")
        for index in range(1, spec.instance_count + 1):
            normalize_identifier(f"{spec.session_prefix}-{index:02d}", "session")
        if spec.timeout_seconds <= 0 or spec.poll_interval_seconds <= 0:
            raise ValueError("timeout/poll interval 必须大于 0")

    @staticmethod
    def _validate_prepared(spec: LanLaunchSpec, prepared: PreparedLanRun) -> None:
        if prepared.run_id != spec.run_id:
            raise ValueError("prepare 返回的 run_id 不匹配")
        sandbox = assert_safe_path(prepared.sandbox_root, prepared.sandbox_root, "LAN sandbox_root")
        if not sandbox.is_dir():
            raise ValueError("LAN sandbox_root 不存在或不是目录")
        root = assert_safe_path(prepared.run_root, sandbox, "LAN run_root")
        if not root.is_dir():
            raise ValueError("LAN run_root 不存在或不是目录")
        artifact_root = assert_safe_path(prepared.artifact_root, sandbox, "LAN artifact_root")
        if not artifact_root.is_dir():
            raise ValueError("LAN artifact_root 不存在或不是目录")

        evidence_pairs = (
            ("ydhost.cfg", prepared.ydhost_cfg, "configSha256"),
            ("map.cfg", prepared.map_cfg, "mapSha256"),
        )
        for label, path, evidence_key in evidence_pairs:
            safe_path = assert_safe_path(path, sandbox, f"LAN {label}")
            try:
                safe_path.relative_to(root)
            except ValueError as exc:
                raise ValueError(f"{label} 不在独立 run_root 内") from exc
            if not safe_path.is_file():
                raise ValueError(f"{label} 不存在或不是普通文件")
            expected_sha = str(prepared.evidence.get(evidence_key, "")).casefold()
            if len(expected_sha) != 64 or any(ch not in "0123456789abcdef" for ch in expected_sha):
                raise ValueError(f"prepare 缺少有效的 {evidence_key} 证据")
            actual_sha = sha256_file(safe_path).casefold()
            if actual_sha != expected_sha:
                raise ValueError(f"{label} 与 {evidence_key} 证据不匹配")

    @staticmethod
    def _validate_host(host: HostHandle) -> None:
        if host.pid <= 0 or not host.job_id:
            raise ValueError("ydhost 必须拥有有效 PID 和独立 Job")
        if not host.hidden:
            raise ValueError("ydhost 必须隐藏启动")

    @staticmethod
    def _validate_client(
        client: ClientHandle,
        expected_session: str,
        occupied_pids: set[int],
        occupied_jobs: set[str],
        occupied_desktops: set[str],
    ) -> None:
        if client.session_id != expected_session:
            raise ValueError(f"客户端 session_id 不匹配: {client.session_id} != {expected_session}")
        if client.pid <= 0 or client.pid in occupied_pids or not client.job_id or client.job_id in occupied_jobs:
            raise ValueError("每个客户端必须拥有唯一 PID/Job")
        if not client.desktop_name or client.desktop_name in occupied_desktops:
            raise ValueError("每个客户端必须拥有唯一独立 Desktop")

    def _event(self, prepared: PreparedLanRun, events: list[dict[str, Any]], state: LanState, message: str, **extra: Any) -> None:
        row = {
            "at": self.clock.monotonic(),
            "state": state.value,
            "message": message,
            **extra,
        }
        events.append(row)
        try:
            self.dependencies.record_event(prepared, row)
        except Exception as exc:
            # 工件记录是 best-effort；绝不能因为日志后端故障跳过进程清理。
            row["recordEventError"] = str(exc)

    def _wait_for(
        self,
        prepared: PreparedLanRun,
        host: HostHandle,
        clients: Sequence[ClientHandle],
        spec: LanLaunchSpec,
        predicate: Callable[[LanProbeSnapshot], tuple[bool, str]],
    ) -> LanProbeSnapshot:
        deadline = self.clock.monotonic() + spec.timeout_seconds
        last: LanProbeSnapshot | None = None
        last_reason = "尚无探针样本"
        while self.clock.monotonic() <= deadline:
            last = self.dependencies.probe(prepared, host, clients)
            if not last.host_alive:
                raise RuntimeError("ydhost 在状态门禁完成前退出")
            expected_sessions = {client.session_id for client in clients}
            missing_alive = expected_sessions - set(last.client_alive)
            if clients and missing_alive:
                raise RuntimeError(f"客户端提前退出: {', '.join(sorted(missing_alive))}")
            accepted, last_reason = predicate(last)
            if accepted:
                return last
            self.clock.sleep(spec.poll_interval_seconds)
        raw = dict(last.raw) if last is not None else {}
        raise TimeoutError(f"LAN 状态门禁超时: {last_reason}; last={raw}")

    def launch(self, spec: LanLaunchSpec) -> dict[str, Any]:
        self._validate_spec(spec)
        if self.registry.get(spec.run_id) is not None:
            return {"ok": False, "code": "YDHOST_RUN_ALREADY_EXISTS", "runId": spec.run_id}

        events: list[dict[str, Any]] = []
        prepared: PreparedLanRun | None = None
        host: HostHandle | None = None
        clients: list[ClientHandle] = []
        cleanup: list[Mapping[str, Any]] = []
        state = LanState.PREPARING
        try:
            prepared = self.dependencies.prepare(spec)
            self._validate_prepared(spec, prepared)
            self._event(prepared, events, state, "per-run cfg/artifact 已准备")

            host = self.dependencies.launch_host(prepared)
            self._validate_host(host)
            state = LanState.HOST_STARTED
            self._event(prepared, events, state, "ydhost 已隐藏启动", pid=host.pid, jobId=host.job_id)

            listen_snapshot = self._wait_for(
                prepared,
                host,
                clients,
                spec,
                lambda sample: (
                    bool(sample.listening and sample.listening_evidence),
                    "缺少 listening 日志证据",
                ),
            )
            state = LanState.HOST_LISTENING
            self._event(
                prepared,
                events,
                state,
                "ydhost 已监听",
                evidence=listen_snapshot.listening_evidence,
            )

            state = LanState.CLIENTS_STARTING
            occupied_pids = {host.pid}
            occupied_jobs = {host.job_id}
            occupied_desktops: set[str] = set()
            for index in range(1, spec.instance_count + 1):
                session_id = f"{spec.session_prefix}-{index:02d}"
                client = self.dependencies.launch_client(prepared, index, session_id)
                # launch_client 返回即代表外部进程可能已经存在，必须先纳入 provisional
                # cleanup，再做所有字段/唯一性校验。
                clients.append(client)
                self._validate_client(
                    client,
                    session_id,
                    occupied_pids,
                    occupied_jobs,
                    occupied_desktops,
                )
                occupied_pids.add(client.pid)
                occupied_jobs.add(client.job_id)
                occupied_desktops.add(client.desktop_name)
                self._event(
                    prepared,
                    events,
                    state,
                    "LAN 客户端已启动",
                    sessionId=client.session_id,
                    pid=client.pid,
                    jobId=client.job_id,
                    desktop=client.desktop_name,
                )

            state = LanState.WAITING_FOR_JOIN
            expected = {client.session_id for client in clients}

            def all_ready(sample: LanProbeSnapshot) -> tuple[bool, str]:
                if not sample.game_created or not sample.game_created_evidence:
                    return False, "缺少 game-created 日志证据"
                if set(sample.joined_clients) != expected:
                    return False, "入房客户端集合不完整"
                if any(not sample.joined_evidence.get(session) for session in expected):
                    return False, "缺少逐客户端 joined 证据"
                if set(sample.ready_clients) != expected:
                    return False, "ready 客户端集合不完整"
                if any(not sample.ready_evidence.get(session) for session in expected):
                    return False, "缺少逐客户端 ready 证据"
                return True, "ready"

            ready_snapshot = self._wait_for(prepared, host, clients, spec, all_ready)
            state = LanState.RUNNING
            self._event(
                prepared,
                events,
                state,
                "所有客户端已入房并 ready",
                gameEvidence=ready_snapshot.game_created_evidence,
            )
            record = LanRunRecord(
                spec=spec,
                prepared=prepared,
                host=host,
                clients={client.session_id: client for client in clients},
                state=state,
                events=events,
            )
            self.registry.add(record)
            return {
                "ok": True,
                "code": "YDHOST_LAN_READY",
                "runId": spec.run_id,
                "state": state.value,
                "host": {"pid": host.pid, "jobId": host.job_id, "hidden": host.hidden},
                "clients": [
                    {
                        "sessionId": client.session_id,
                        "pid": client.pid,
                        "jobId": client.job_id,
                        "desktop": client.desktop_name,
                    }
                    for client in clients
                ],
                "events": events,
                "acceptanceScope": (
                    "production-authorized"
                    if self.dependencies.acceptance_authorized
                    else "dependency-injection-simulation"
                ),
                "countsAsLanAcceptance": bool(self.dependencies.acceptance_authorized),
            }
        except Exception as exc:
            state = LanState.FAILED
            if prepared is not None:
                self._event(prepared, events, state, str(exc))
                for client in reversed(clients):
                    try:
                        cleanup.append(self.dependencies.stop_client(prepared, client, "launch-failed"))
                    except Exception as cleanup_exc:
                        cleanup.append({"ok": False, "sessionId": client.session_id, "error": str(cleanup_exc)})
                if host is not None:
                    try:
                        cleanup.append(self.dependencies.stop_host(prepared, host, "launch-failed"))
                    except Exception as cleanup_exc:
                        cleanup.append({"ok": False, "component": "ydhost", "error": str(cleanup_exc)})
            return {
                "ok": False,
                "code": "YDHOST_LAN_STATE_FAILED",
                "runId": spec.run_id,
                "state": state.value,
                "error": str(exc),
                "cleanup": cleanup,
                "events": events,
                "countsAsLanAcceptance": False,
            }

    def stop_client(self, run_id: str, session_id: str, reason: str = "requested") -> dict[str, Any]:
        record = self.registry.require(run_id)
        client = record.clients.get(session_id)
        if client is None:
            return {"ok": False, "error": f"run 中不存在客户端: {session_id}"}
        result = dict(self.dependencies.stop_client(record.prepared, client, reason))
        if result.get("ok"):
            record.clients.pop(session_id, None)
        return {
            "ok": bool(result.get("ok")),
            "runId": run_id,
            "sessionId": session_id,
            "hostPreserved": True,
            "remainingClients": sorted(record.clients),
            "detail": result,
        }

    def stop_run(self, run_id: str, reason: str = "requested") -> dict[str, Any]:
        record = self.registry.require(run_id)
        record.state = LanState.STOPPING
        cleanup: list[Mapping[str, Any]] = []
        for client in reversed(list(record.clients.values())):
            try:
                row = dict(self.dependencies.stop_client(record.prepared, client, reason))
                cleanup.append(row)
                if row.get("ok"):
                    record.clients.pop(client.session_id, None)
            except Exception as exc:
                cleanup.append({"ok": False, "sessionId": client.session_id, "error": str(exc)})
        if not record.host_stopped:
            try:
                row = dict(self.dependencies.stop_host(record.prepared, record.host, reason))
                cleanup.append(row)
                if row.get("ok"):
                    record.host_stopped = True
            except Exception as exc:
                cleanup.append({"ok": False, "component": "ydhost", "error": str(exc)})
        stopped = not record.clients and record.host_stopped
        record.state = LanState.STOPPED if stopped else LanState.FAILED
        if stopped:
            self.registry.remove(run_id)
        return {
            "ok": stopped,
            "runId": run_id,
            "state": record.state.value,
            "cleanup": cleanup,
            "remainingClients": sorted(record.clients),
            "hostStopped": record.host_stopped,
            "retryable": not stopped,
        }


def real_launch_capability() -> dict[str, Any]:
    """公开生产接线缺口；调用方必须据此 fail-closed。"""

    return {
        "ok": False,
        "code": CLIENT_PROTOCOL_BLOCKER,
        "error": (
            "YDWE 证据链要求 `YDWE.exe -war3 -auto`，而不是 `war3.exe -loadfile`。"
            "源码进一步确认原版 YDWE 只从当前用户/机器共享的 Warcraft III InstallPath "
            "选择 War3 根，没有已证明的 per-process 根参数；隔离 Desktop 不隔离注册表，"
            "因此无法把并发包装器安全绑定到各自实例根。真实启动保持禁用。"
        ),
        "evidence": [
            "SourceMap/YDWE1.32.13 - MemoryHack/script/ydwe/ydwe_on_test.lua:105",
            "E:/Mycode/Source/Repos/YDWE/Development/Core/ydwar3/warcraft3/directory.cpp:8",
            "E:/Mycode/Source/Repos/YDWE/Development/Core/YDWEStartup/LaunchWarcraft3.cpp:165",
            "Core/Base/Graphics/dxvk/AutoTest/war3_autotest_mcp.py: _launch_war3_instance_impl",
        ],
        "protocolBlockerCode": YDWE_CLIENT_PROTOCOL_BLOCKER,
        "protocol": describe_ydwe_protocol(),
        "identityContract": ydwe_identity_contract(),
        "protocolDocument": "Core/Base/Graphics/dxvk/AutoTest/YDWE_LAN_PROTOCOL.md",
        "additionalProductionBlockers": [
            {
                "code": "YDHOST_FAILED_LAUNCH_HANDLE_RETENTION_REQUIRED",
                "detail": "启动失败且清理失败时必须注册 FAILED record，不能丢失 PID/Job/process handles。",
            },
            {
                "code": "YDHOST_SUSPENDED_JOB_BINDING_REQUIRED",
                "detail": (
                    "真实进程需 retained handle + CREATE_SUSPENDED → AssignJob → Resume，并对 "
                    "run/session stop 加锁、identity fail-closed、固定 System32 工具与 timeout。"
                ),
            },
            {
                "code": "MAPDUMP_SNAPSHOT_WRITE_PIN_REQUIRED",
                "detail": "434 文件复核到 CreateProcess 之间仍需 deny-write 文件句柄或等价不可变快照。",
            },
        ],
        "stateMachineImplemented": True,
        "realProcessLauncherBound": False,
    }
