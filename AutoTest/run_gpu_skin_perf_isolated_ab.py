#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Isolated, attribution-only GPU skin disabled/bypass A/B.

This runner deliberately does not build or deploy.  It uses the already deployed
E:\\Work\\War3\\d3d9.dll, launches exactly one War3 process at a time on an
isolated desktop, and preserves enough process/module/log/report evidence to
prove that both cases used the same binary and map.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import war3_autotest_mcp as autotest  # noqa: E402


WAR3_DIR = Path(r"E:\Work\War3")
MAP_PATH = Path(r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x")
DEPLOYED_DLL = WAR3_DIR / "d3d9.dll"
WAR3_EXE = WAR3_DIR / "war3.exe"
GAME_DLL = WAR3_DIR / "Game.dll"
ARTIFACTS = HERE / "artifacts"
SAMPLE_DURATION_SEC = 36
PERIODIC_SPIKE_PERIOD_FRAMES = 300
PERIODIC_TRIM_THRESHOLD_MS = 25.0
EXPECTED_DLL_SHA256 = (
    "7448311EE7E9BC2E4278E6CF1749E92913B688213FAC93D260BFD98DC3113319"
)
GPU_SKIN_EXECUTION_ROUTE_ENV = "DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE"
GPU_SKIN_EXECUTION_ROUTE_CHOICES = (
    "compute",
    "vertex_shader_bypass",
)
REPORT_DIR = WAR3_DIR / "WarVK" / "Log"
LOG_PATHS = (WAR3_DIR / "war3_d3d9.log", WAR3_DIR / "dxvk.log")
CASE_ORDERS = {
    "disabled-bypass": (
        ("A1_disabled", "disabled"),
        ("B1_bypass", "bypass"),
    ),
    "bypass-disabled": (
        ("B1_bypass", "bypass"),
        ("A1_disabled", "disabled"),
    ),
    "abba": (
        ("A1_disabled", "disabled"),
        ("B1_bypass", "bypass"),
        ("B2_bypass", "bypass"),
        ("A2_disabled", "disabled"),
    ),
}

METRIC_FIELDS = (
    "avgFps",
    "avgFrameTimeMs",
    "avgProcessCpuMs",
    "avgMainThreadCpuMs",
    "avgTrackedActiveCpuMs",
    "avgUntrackedActiveCpuMs",
    "avgIdleWaitCpuMs",
    "avgGpuTimeMs",
    "cpuCoveragePct",
)

DRAW_CHAIN_TIMING_ENV = "DXVK_WAR3_GPU_SKIN_DRAW_CHAIN_TIMING"
GPU_SKIN_POISON_SIDECAR_ENV = "DXVK_WAR3_GPU_SKIN_POISON_SIDECAR"
GPU_SKIN_POISON_SIDECAR_VALUE = "none"
S1_TERRAIN_CAPTURE_PERIOD_ENV = "DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD"
S1_TERRAIN_CAPTURE_PERIOD_VALUE = "1"
DRAW_CHAIN_TIMING_PERIOD = 256
DRAW_CHAIN_TIMING_PHASE = 0x5A
DRAW_CHAIN_ROOT_PATH = "GpuSkinDrawChain/LeafHostRoot"
DRAW_CHAIN_DIRECT_STAGES = (
    "BeforeUi",
    "UploadPerDrawData",
    "GpuSkinDip",
    "ShadowCapture",
    "PrepareMainRestoreRebind",
    "PrepareMainRestoreDeferred",
    "PrepareMainNormal",
    "HostEnqueueMain",
    "PrepareOutline",
    "HostEnqueueOutline",
)
DRAW_CHAIN_EVERY_LEAF_STAGES = (
    "BeforeUi",
    "UploadPerDrawData",
    "GpuSkinDip",
)
DRAW_CHAIN_ROUNDING_TOLERANCE_MS = 0.010


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def json_write(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=False, default=str),
        encoding="utf-8",
    )


def snapshot_log_offsets() -> Dict[str, Dict[str, Any]]:
    result: Dict[str, Dict[str, Any]] = {}
    for path in LOG_PATHS:
        try:
            size = path.stat().st_size
            with path.open("rb") as stream:
                stream.seek(max(0, size - 128))
                tail = stream.read()
            result[str(path)] = {
                "size": size,
                "tailStart": max(0, size - 128),
                "tailSha256": hashlib.sha256(tail).hexdigest().upper(),
            }
        except OSError:
            result[str(path)] = {"size": 0, "tailStart": 0, "tailSha256": ""}
    return result


def save_log_deltas(
    case_dir: Path, offsets: Dict[str, Dict[str, Any]]
) -> List[Dict[str, Any]]:
    evidence: List[Dict[str, Any]] = []
    for path in LOG_PATHS:
        old = dict(offsets.get(str(path), {}) or {})
        old_size = int(old.get("size", 0) or 0)
        try:
            new_size = path.stat().st_size
            tail_start = int(old.get("tailStart", 0) or 0)
            old_tail_hash = str(old.get("tailSha256", "") or "")
            old_prefix_still_present = old_size == 0
            if old_size > 0 and new_size >= old_size:
                with path.open("rb") as stream:
                    stream.seek(tail_start)
                    current_old_tail = stream.read(old_size - tail_start)
                old_prefix_still_present = (
                    hashlib.sha256(current_old_tail).hexdigest().upper()
                    == old_tail_hash
                )
            start = old_size if old_prefix_still_present and new_size >= old_size else 0
            with path.open("rb") as stream:
                stream.seek(start)
                raw = stream.read()
            output = case_dir / f"{path.name}.delta.log"
            output.write_bytes(raw)
            evidence.append(
                {
                    "source": str(path),
                    "artifact": str(output),
                    "oldSize": old_size,
                    "newSize": new_size,
                    "oldPrefixStillPresent": old_prefix_still_present,
                    "sourceWasReplacedOrTruncated": not old_prefix_still_present,
                    "deltaBytes": len(raw),
                    "sha256": hashlib.sha256(raw).hexdigest().upper(),
                }
            )
        except OSError as exc:
            evidence.append({"source": str(path), "error": str(exc)})
    return evidence


def process_rows() -> List[Dict[str, Any]]:
    rows = []
    for row in autotest._snapshot_process_entries():
        name = str(row.get("exeName", row.get("name", ""))).casefold()
        if name == "war3.exe":
            rows.append(dict(row))
    return rows


def query_modules(pid: int) -> Dict[str, Any]:
    script = rf"""
$ErrorActionPreference = 'Stop'
$p = Get-Process -Id {pid}
@($p.Modules | Where-Object {{
  $_.ModuleName -ieq 'war3.exe' -or
  $_.ModuleName -ieq 'game.dll' -or
  $_.ModuleName -ieq 'd3d9.dll'
}} | ForEach-Object {{
  [pscustomobject]@{{ Name=$_.ModuleName; Path=$_.FileName; BaseAddress=('0x{{0:X}}' -f $_.BaseAddress.ToInt64()); Size=$_.ModuleMemorySize }}
}}) | ConvertTo-Json -Depth 4 -Compress
"""
    powershell32 = Path(
        r"C:\Windows\SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    )
    powershell = str(powershell32) if powershell32.is_file() else "powershell.exe"
    completed = subprocess.run(
        [
            powershell,
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
        check=False,
    )
    if completed.returncode != 0:
        return {
            "ok": False,
            "pid": pid,
            "returncode": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
        }
    text = completed.stdout.strip()
    parsed: Any = [] if not text else json.loads(text)
    rows = parsed if isinstance(parsed, list) else [parsed]
    for row in rows:
        path = Path(str(row.get("Path", "")))
        if path.is_file():
            row["sha256"] = sha256(path)
    names = {str(row.get("Name", "")).lower() for row in rows}
    return {
        "ok": "war3.exe" in names and "d3d9.dll" in names,
        "pid": pid,
        "modules": rows,
        "requiredNames": ["war3.exe", "d3d9.dll"],
        "gameDllObserved": "game.dll" in names,
    }


def monitor_modules(stop_event: threading.Event, output: Dict[str, Any]) -> None:
    deadline = time.time() + 150.0
    last_error: Dict[str, Any] = {"ok": False, "error": "pid not observed"}
    best_evidence: Dict[str, Any] = {}
    while time.time() < deadline and not stop_event.is_set():
        pid = int(getattr(autotest.STATE, "war3_pid", 0) or 0)
        if pid > 0:
            try:
                evidence = query_modules(pid)
                last_error = evidence
                if len(evidence.get("modules", []) or []) > len(
                    best_evidence.get("modules", []) or []
                ):
                    best_evidence = evidence
                if evidence.get("ok") and evidence.get("gameDllObserved"):
                    output.update(evidence)
                    return
            except Exception as exc:  # best-effort evidence must not affect test
                last_error = {"ok": False, "pid": pid, "error": repr(exc)}
        stop_event.wait(0.5)
    output.update(best_evidence or last_error)


def exact_cleanup(
    pid: int,
    preexisting_pids: set[int],
    expected_witness: Dict[str, Any],
) -> List[Dict[str, Any]]:
    attempts: List[Dict[str, Any]] = []
    expected_pid = int(expected_witness.get("pid", 0) or 0)
    expected_creation = int(
        expected_witness.get("creationEpochMs", 0) or 0
    )
    expected_path = str(
        expected_witness.get("canonicalExePath", "") or ""
    )
    expected_shape = bool(
        pid > 0
        and pid not in preexisting_pids
        and expected_pid == pid
        and expected_creation > 0
        and expected_path
    )
    for attempt in range(2):
        alive_rows = process_rows()
        alive_pids = {
            int(row.get("pid", 0) or 0) for row in alive_rows
        }
        targets: List[int] = []
        stopped: List[Dict[str, Any]] = []
        binding: Dict[str, Any] = {
            "expectedShape": expected_shape,
            "expectedPid": expected_pid,
            "expectedCreationEpochMs": expected_creation,
            "expectedCanonicalExePath": expected_path,
            "currentProcessPresent": pid in alive_pids,
            "exact": False,
        }
        if expected_shape and pid in alive_pids:
            witness = None
            try:
                witness, acquisition = autotest._open_native_process_witness(
                    pid,
                    expected_path,
                    "gpu-skin-abba-cleanup",
                )
                binding["acquisition"] = acquisition
                current = witness.snapshot() if witness is not None else {}
                binding["currentWitness"] = current
                exact = bool(
                    witness is not None
                    and int(current.get("pid", 0) or 0) == expected_pid
                    and int(current.get("creationEpochMs", 0) or 0)
                    == expected_creation
                    and str(current.get("canonicalExePath", "") or "")
                    == expected_path
                )
                binding["exact"] = exact
                if exact:
                    targets.append(pid)
                    stopped.append(witness.terminate_exact(
                        expected_pid, expected_creation, expected_path,
                        wait_timeout_sec=10.0,
                    ))
            except Exception as exc:
                binding["error"] = repr(exc)
            finally:
                if witness is not None:
                    binding["close"] = witness.close()
        time.sleep(0.7)
        remaining = [
            row
            for row in process_rows()
            if int(row.get("pid", 0) or 0) not in preexisting_pids
        ]
        attempts.append(
            {
                "attempt": attempt + 1,
                "scope": "exact-launch-process-instance-only",
                "targets": targets,
                "binding": binding,
                "stopResults": stopped,
                "remainingNewWar3": remaining,
            }
        )
    return attempts


def report_path(result: Dict[str, Any]) -> Path | None:
    report = dict(result.get("report", {}) or {})
    for raw in (
        report.get("latestReportPath"),
        report.get("reportPath"),
        result.get("reportPath"),
    ):
        if raw and Path(str(raw)).is_file():
            return Path(str(raw))
    return None


def report_snapshot() -> Dict[str, Dict[str, int]]:
    result: Dict[str, Dict[str, int]] = {}
    if not REPORT_DIR.is_dir():
        return result
    for path in REPORT_DIR.glob("war3_perf_report*.html"):
        try:
            stat = path.stat()
        except OSError:
            continue
        result[str(path.resolve())] = {
            "mtimeNs": int(stat.st_mtime_ns),
            "size": int(stat.st_size),
        }
    return result


def source_report_is_new(
    source: Path | None,
    before: Dict[str, Dict[str, int]],
    result: Dict[str, Any],
) -> bool:
    if source is None or not source.is_file():
        return False
    report = dict(result.get("report", {}) or {})
    if report.get("newReportDetected") is not True:
        return False
    key = str(source.resolve())
    try:
        stat = source.stat()
    except OSError:
        return False
    previous = before.get(key)
    return bool(
        previous is None
        or int(stat.st_mtime_ns) > int(previous.get("mtimeNs", 0) or 0)
        or int(stat.st_size) != int(previous.get("size", -1) or -1)
    )


def report_process_identity_from_report(report: Path | None) -> Dict[str, Any]:
    try:
        if report is None:
            raise ValueError("report is unavailable")
        data = _embedded_report_data(report)
        snapshot = data.get("gpuSkinSnapshot")
        if not isinstance(snapshot, dict):
            raise ValueError("embedded report has no gpuSkinSnapshot")
        process_id = int(snapshot.get("processId", 0) or 0)
        start_filetime = int(
            snapshot.get("processStartFileTime100ns", 0) or 0
        )
        start_exact = str(
            snapshot.get("processStartFileTime100nsExact", "") or ""
        )
        creation_ms = (
            (start_filetime - 116_444_736_000_000_000) // 10_000
            if start_filetime >= 116_444_736_000_000_000
            else 0
        )
        exact_fields = bool(
            process_id > 0
            and start_filetime > 0
            and start_exact == str(start_filetime)
        )
        return {
            "available": True,
            "processId": process_id,
            "processStartFileTime100ns": start_filetime,
            "processStartFileTime100nsExact": start_exact,
            "derivedCreationEpochMs": creation_ms,
            "exactFieldsClosed": exact_fields,
        }
    except Exception as exc:
        return {"available": False, "error": repr(exc)}


def case_launch_contract(
    launch: Dict[str, Any],
    expected_env: Dict[str, str],
    expected_map_hash: str,
    pid: int,
) -> Dict[str, Any]:
    runtime_env = dict(expected_env)
    runtime_env.update({
        "DXVK_WAR3_RUNTIME_BENCHMARK": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC": str(
            SAMPLE_DURATION_SEC - 1
        ),
    })
    actual_overrides = dict(launch.get("envOverrides", {}) or {})
    expected_effective = dict(runtime_env)
    expected_effective.update({
        "DXVK_WAR3_PERF_RECORD_AFTER_GAME_START": "1",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": str(
            SAMPLE_DURATION_SEC + 2
        ),
    })
    actual_effective = dict(
        launch.get("effectiveWar3Environment", {}) or {}
    )
    witness = dict(launch.get("nativeProcessWitness", {}) or {})
    expected_exe = str(WAR3_EXE.resolve()).casefold()
    checks = {
        "pid": bool(pid > 0 and int(launch.get("pid", 0) or 0) == pid),
        "isolatedDesktop": launch.get("useIsolatedDesktop") is True,
        "mapSha256": (
            str(launch.get("sourceMapSha256", "")).upper()
            == expected_map_hash
        ),
        "overridesExact": actual_overrides == runtime_env,
        "effectiveEnvironmentExact": actual_effective == expected_effective,
        "noDeploy": launch.get("deploy") is None,
        "nativeWitness": bool(
            witness.get("available") is True
            and witness.get("ownsNativeHandle") is True
            and int(witness.get("pid", 0) or 0) == pid
            and int(witness.get("creationEpochMs", 0) or 0) > 0
            and str(witness.get("canonicalExePath", "")).casefold()
            == expected_exe
        ),
    }
    return {
        "expectedEnvironment": runtime_env,
        "actualOverrides": actual_overrides,
        "expectedEffectiveEnvironment": expected_effective,
        "actualEffectiveEnvironment": actual_effective,
        "expectedMapSha256": expected_map_hash,
        "actualMapSha256": str(launch.get("sourceMapSha256", "")).upper(),
        "nativeProcessWitness": witness,
        "expectedCanonicalExePath": expected_exe,
        "checks": checks,
        "closed": all(checks.values()),
    }


def case_report_identity_contract(
    identity: Dict[str, Any],
    launch: Dict[str, Any],
    module_evidence: Dict[str, Any],
    pid: int,
) -> Dict[str, Any]:
    witness = dict(launch.get("nativeProcessWitness", {}) or {})
    checks = {
        "exactReportFields": bool(identity.get("exactFieldsClosed") is True),
        "samePid": bool(
            pid > 0
            and int(identity.get("processId", 0) or 0) == pid
            and int(witness.get("pid", 0) or 0) == pid
            and int(module_evidence.get("pid", 0) or 0) == pid
        ),
        "sameCreationTime": bool(
            int(witness.get("creationEpochMs", 0) or 0) > 0
            and int(identity.get("derivedCreationEpochMs", 0) or 0)
            == int(witness.get("creationEpochMs", 0) or 0)
        ),
    }
    return {
        "reportIdentity": identity,
        "launchNativeProcessWitness": witness,
        "checks": checks,
        "closed": all(checks.values()),
    }


def exact_module_contract(
    evidence: Dict[str, Any],
    pid: int,
    expected_hashes: Dict[str, str],
) -> Dict[str, Any]:
    rows = list(evidence.get("modules", []) or [])
    by_name = {
        name: [
            row for row in rows
            if str(row.get("Name", "")).casefold() == name
        ]
        for name in ("war3.exe", "game.dll", "d3d9.dll")
    }
    expected_paths = {
        "war3.exe": str(WAR3_EXE.resolve()).casefold(),
        "game.dll": str(GAME_DLL.resolve()).casefold(),
        "d3d9.dll": str(DEPLOYED_DLL.resolve()).casefold(),
    }
    actual_paths: Dict[str, str] = {}
    actual_hashes: Dict[str, str] = {}
    for name, values in by_name.items():
        path_text = (
            str(values[0].get("Path", ""))
            if len(values) == 1 else ""
        )
        actual_paths[name] = (
            str(Path(path_text).resolve()).casefold() if path_text else ""
        )
        actual_hashes[name] = (
            str(values[0].get("sha256", "")).upper()
            if len(values) == 1 else ""
        )
    checks = {
        "monitor": bool(
            evidence.get("ok") is True
            and evidence.get("gameDllObserved") is True
        ),
        "pid": bool(
            pid > 0 and int(evidence.get("pid", 0) or 0) == pid
        ),
        "exactlyOnce": all(len(values) == 1 for values in by_name.values()),
        "paths": all(
            actual_paths[name] == expected_paths[name] for name in by_name
        ),
        "hashes": all(
            actual_hashes[name] == expected_hashes[name] for name in by_name
        ),
    }
    return {
        "requiredExactlyOnce": ["war3.exe", "game.dll", "d3d9.dll"],
        "expectedPid": pid,
        "expectedPaths": expected_paths,
        "actualPaths": actual_paths,
        "expectedSha256": expected_hashes,
        "actualSha256": actual_hashes,
        "checks": checks,
        "closed": all(checks.values()),
    }


def metric_row(result: Dict[str, Any]) -> Dict[str, Any]:
    report = dict(result.get("report", {}) or {})
    names = (
        "frameCount",
        "windowSec",
        "avgFps",
        "avgFrameTimeMs",
        "avgProcessCpuMs",
        "avgMainThreadCpuMs",
        "avgTrackedActiveCpuMs",
        "avgUntrackedActiveCpuMs",
        "avgIdleWaitCpuMs",
        "avgGpuTimeMs",
        "cpuCoveragePct",
        "fpsSampleReliable",
        "fpsSampleFrameCount",
        "fpsSampleWindowSec",
    )
    return {name: report.get(name) for name in names}


def _embedded_report_data(report: Path) -> Dict[str, Any]:
    text = report.read_text(encoding="utf-8")
    marker = "const data = "
    marker_offset = text.find(marker)
    if marker_offset < 0:
        raise ValueError("embedded report data marker not found")
    data, _ = json.JSONDecoder().raw_decode(
        text[marker_offset + len(marker):]
    )
    if not isinstance(data, dict):
        raise ValueError("embedded report data is not an object")
    return data


def _draw_chain_section_row(
    raw: Dict[str, Any], expected_path: str
) -> Dict[str, Any]:
    calls = raw.get("calls")
    total_cpu_ms = raw.get("totalCpuMs")
    if isinstance(calls, bool) or not isinstance(calls, int) or calls < 0:
        raise ValueError(f"invalid calls for section {expected_path!r}")
    try:
        total_cpu_ms = float(total_cpu_ms)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            f"invalid totalCpuMs for section {expected_path!r}"
        ) from exc
    if not math.isfinite(total_cpu_ms) or total_cpu_ms < 0.0:
        raise ValueError(f"invalid totalCpuMs for section {expected_path!r}")
    actual_path = str(raw.get("path", ""))
    if actual_path != expected_path:
        raise ValueError(
            f"section path mismatch: expected={expected_path!r} "
            f"actual={actual_path!r}"
        )
    return {
        "present": True,
        "name": str(raw.get("name", "")),
        "path": actual_path,
        "parentPath": str(raw.get("parentPath", "")),
        "calls": calls,
        "totalCpuMs": total_cpu_ms,
        "avgUsPerCall": (
            total_cpu_ms * 1000.0 / calls if calls > 0 else None
        ),
    }


def analyze_draw_chain_sections(data: Dict[str, Any]) -> Dict[str, Any]:
    sections = data.get("sections")
    if not isinstance(sections, list):
        raise ValueError("embedded report data has no sections array")

    prefix = f"{DRAW_CHAIN_ROOT_PATH}/"
    matching = [
        row for row in sections
        if isinstance(row, dict)
        and (
            str(row.get("path", "")) == DRAW_CHAIN_ROOT_PATH
            or str(row.get("path", "")).startswith(prefix)
        )
    ]
    root_rows = [
        row for row in matching
        if str(row.get("path", "")) == DRAW_CHAIN_ROOT_PATH
    ]
    if len(root_rows) != 1:
        raise ValueError(
            f"expected one {DRAW_CHAIN_ROOT_PATH!r} section, "
            f"found {len(root_rows)}"
        )
    root = _draw_chain_section_row(root_rows[0], DRAW_CHAIN_ROOT_PATH)

    direct_rows: Dict[str, Dict[str, Any]] = {}
    duplicate_direct_names: List[str] = []
    unexpected_direct_paths: List[str] = []
    ignored_nested_paths: List[str] = []
    for raw in matching:
        path = str(raw.get("path", ""))
        if path == DRAW_CHAIN_ROOT_PATH:
            continue
        if str(raw.get("parentPath", "")) != DRAW_CHAIN_ROOT_PATH:
            ignored_nested_paths.append(path)
            continue
        stage_name = path[len(prefix):]
        if stage_name not in DRAW_CHAIN_DIRECT_STAGES:
            unexpected_direct_paths.append(path)
            continue
        if stage_name in direct_rows:
            duplicate_direct_names.append(stage_name)
            continue
        direct_rows[stage_name] = _draw_chain_section_row(raw, path)

    stages: Dict[str, Dict[str, Any]] = {}
    for stage_name in DRAW_CHAIN_DIRECT_STAGES:
        row = direct_rows.get(stage_name)
        if row is None:
            row = {
                "present": False,
                "name": stage_name,
                "path": f"{prefix}{stage_name}",
                "parentPath": DRAW_CHAIN_ROOT_PATH,
                "calls": 0,
                "totalCpuMs": 0.0,
                "avgUsPerCall": None,
            }
        stages[stage_name] = row

    errors: List[str] = []
    if root["calls"] <= 0:
        errors.append("root calls must be positive")
    if duplicate_direct_names:
        errors.append(
            "duplicate direct child sections: "
            + ",".join(sorted(set(duplicate_direct_names)))
        )
    if unexpected_direct_paths:
        errors.append(
            "unexpected direct child paths: "
            + ",".join(sorted(set(unexpected_direct_paths)))
        )

    for stage_name in DRAW_CHAIN_EVERY_LEAF_STAGES:
        row = stages[stage_name]
        if not row["present"]:
            errors.append(f"required direct child missing: {stage_name}")

    for stage_name, row in stages.items():
        if row["calls"] > root["calls"]:
            errors.append(
                f"{stage_name} calls {row['calls']} > root "
                f"calls {root['calls']}"
            )
        if (
            row["totalCpuMs"]
            > root["totalCpuMs"] + DRAW_CHAIN_ROUNDING_TOLERANCE_MS
        ):
            errors.append(
                f"{stage_name} totalCpuMs {row['totalCpuMs']:.6f} > "
                f"root {root['totalCpuMs']:.6f}"
            )

    prepare_main_calls = sum(
        stages[name]["calls"] for name in (
            "PrepareMainRestoreRebind",
            "PrepareMainRestoreDeferred",
            "PrepareMainNormal",
        )
    )
    if prepare_main_calls > root["calls"]:
        errors.append(
            f"PrepareMain bucket calls {prepare_main_calls} > root "
            f"calls {root['calls']}"
        )

    direct_total_cpu_ms = sum(
        row["totalCpuMs"] for row in stages.values()
    )
    raw_host_other_ms = root["totalCpuMs"] - direct_total_cpu_ms
    if raw_host_other_ms < -DRAW_CHAIN_ROUNDING_TOLERANCE_MS:
        errors.append(
            f"direct child total {direct_total_cpu_ms:.6f}ms > root "
            f"{root['totalCpuMs']:.6f}ms"
        )
    host_other_ms = max(0.0, raw_host_other_ms)
    host_other = {
        "present": True,
        "name": "HostOther",
        "path": f"{DRAW_CHAIN_ROOT_PATH}/HostOther(derived)",
        "parentPath": DRAW_CHAIN_ROOT_PATH,
        "calls": root["calls"],
        "totalCpuMs": host_other_ms,
        "avgUsPerCall": (
            host_other_ms * 1000.0 / root["calls"]
            if root["calls"] > 0 else None
        ),
        "derived": True,
    }
    ranked = sorted(
        (
            {"stage": name, **row}
            for name, row in {**stages, "HostOther": host_other}.items()
        ),
        key=lambda row: row["totalCpuMs"],
        reverse=True,
    )
    return {
        "available": True,
        "contractClosed": not errors,
        "validationErrors": errors,
        "period": DRAW_CHAIN_TIMING_PERIOD,
        "phase": DRAW_CHAIN_TIMING_PHASE,
        "phaseHex": f"0x{DRAW_CHAIN_TIMING_PHASE:02X}",
        "rootPath": DRAW_CHAIN_ROOT_PATH,
        "roundingToleranceMs": DRAW_CHAIN_ROUNDING_TOLERANCE_MS,
        "root": root,
        "stages": stages,
        "prepareMainBucketCalls": prepare_main_calls,
        "directChildTotalCpuMs": direct_total_cpu_ms,
        "hostOther": host_other,
        "ignoredNestedSectionCount": len(ignored_nested_paths),
        "ignoredNestedSectionPaths": sorted(ignored_nested_paths),
        "unexpectedDirectSectionPaths": sorted(unexpected_direct_paths),
        "rankedDirectByTotalCpuMs": ranked,
    }


def draw_chain_timing_from_report(report: Path | None) -> Dict[str, Any]:
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        result = analyze_draw_chain_sections(_embedded_report_data(report))
        result["sourceReport"] = str(report)
        return result
    except Exception as exc:
        return {
            "available": False,
            "contractClosed": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


def resource_census_contract_from_report(
    report: Path | None,
) -> Dict[str, Any]:
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        data = _embedded_report_data(report)
        census = data.get("resourceResidencyCensus")
        if not isinstance(census, dict):
            raise ValueError("embedded report has no resourceResidencyCensus")
        enabled = census.get("enabled")
        performance_comparable = census.get("performanceComparable")
        contract = census.get("contract")
        closed = bool(
            enabled is False
            and performance_comparable is True
            and contract == "diagnostics-only-observation-v1"
        )
        return {
            "available": True,
            "requestedEnabled": False,
            "reportedEnabled": enabled,
            "reportedPerformanceComparable": performance_comparable,
            "reportedContract": contract,
            "closed": closed,
        }
    except Exception as exc:
        return {
            "available": False,
            "closed": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


def _nearest_rank_percentile(values: List[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, min(len(ordered), math.ceil(quantile * len(ordered))))
    return ordered[rank - 1]


def analyze_periodic_frame_spikes(
    frame_times: List[Any],
    period_frames: int = PERIODIC_SPIKE_PERIOD_FRAMES,
    trim_threshold_ms: float = PERIODIC_TRIM_THRESHOLD_MS,
) -> Dict[str, Any]:
    """Quantify a fixed-frame-period phase without assuming it is causal."""
    if period_frames <= 0:
        raise ValueError("period_frames must be positive")

    samples: List[tuple[int, float]] = []
    for index, raw in enumerate(frame_times):
        try:
            value = float(raw)
        except (TypeError, ValueError):
            continue
        if math.isfinite(value) and value >= 0.0:
            samples.append((index, value))

    if not samples:
        return {
            "available": False,
            "error": "frameTimes contains no finite non-negative samples",
            "periodFrames": period_frames,
            "trimThresholdMs": trim_threshold_ms,
            "sourceFrameCount": len(frame_times),
            "finiteFrameCount": 0,
            "invalidFrameCount": len(frame_times),
        }

    all_values = [value for _, value in samples]
    trimmed_values = [value for value in all_values if value <= trim_threshold_ms]
    trimmed_fallback = not trimmed_values
    trimmed_reference_values = trimmed_values or all_values
    global_trimmed_median = float(statistics.median(trimmed_reference_values))
    global_mean = float(statistics.fmean(all_values))
    global_max = max(all_values)
    global_p99 = _nearest_rank_percentile(all_values, 0.99)
    global_max_indices = [index for index, value in samples if value == global_max]

    grouped: List[List[tuple[int, float]]] = [
        [] for _ in range(period_frames)
    ]
    for index, value in samples:
        grouped[index % period_frames].append((index, value))

    residue_rows: List[Dict[str, Any]] = []
    for residue, group in enumerate(grouped):
        values = [value for _, value in group]
        if values:
            mean_ms = float(statistics.fmean(values))
            median_ms = float(statistics.median(values))
            max_ms = max(values)
            residue_rows.append(
                {
                    "residue": residue,
                    "count": len(values),
                    "meanMs": mean_ms,
                    "medianMs": median_ms,
                    "maxMs": max_ms,
                    "meanExcessVsGlobalTrimmedMedianMs": (
                        mean_ms - global_trimmed_median
                    ),
                    "medianExcessVsGlobalTrimmedMedianMs": (
                        median_ms - global_trimmed_median
                    ),
                    "maxExcessVsGlobalTrimmedMedianMs": (
                        max_ms - global_trimmed_median
                    ),
                }
            )
        else:
            residue_rows.append(
                {
                    "residue": residue,
                    "count": 0,
                    "meanMs": None,
                    "medianMs": None,
                    "maxMs": None,
                    "meanExcessVsGlobalTrimmedMedianMs": None,
                    "medianExcessVsGlobalTrimmedMedianMs": None,
                    "maxExcessVsGlobalTrimmedMedianMs": None,
                }
            )

    populated_rows = [row for row in residue_rows if row["count"]]
    selected_row = max(
        populated_rows,
        key=lambda row: (
            float(row["medianExcessVsGlobalTrimmedMedianMs"]),
            float(row["meanExcessVsGlobalTrimmedMedianMs"]),
            float(row["maxExcessVsGlobalTrimmedMedianMs"]),
            -int(row["residue"]),
        ),
    )
    selected_residue = int(selected_row["residue"])
    selected_samples = grouped[selected_residue]
    selected_values = [value for _, value in selected_samples]
    selected_indices = [index for index, _ in selected_samples]
    adjacent_intervals = [
        selected_indices[index] - selected_indices[index - 1]
        for index in range(1, len(selected_indices))
    ]
    non_exact_intervals = [
        interval for interval in adjacent_intervals if interval != period_frames
    ]

    no_phase_values = [
        value for index, value in samples if index % period_frames != selected_residue
    ]
    no_phase_mean = (
        float(statistics.fmean(no_phase_values)) if no_phase_values else None
    )
    no_phase_fps = (
        1000.0 / no_phase_mean if no_phase_mean is not None and no_phase_mean > 0.0
        else None
    )
    original_fps = 1000.0 / global_mean if global_mean > 0.0 else None
    positive_excess_total = sum(
        max(0.0, value - global_trimmed_median) for value in selected_values
    )
    selected_phase_max = max(selected_values)
    selected_phase_p99 = _nearest_rank_percentile(selected_values, 0.99)
    selected_phase = {
        **selected_row,
        "selectionRule": (
            "largest median excess versus the median of finite frameTimes <= "
            f"{trim_threshold_ms:g} ms; ties use mean excess, max excess, then "
            "lowest residue"
        ),
        "samples": [
            {"index": index, "valueMs": value}
            for index, value in selected_samples
        ],
        "adjacentIntervalsFrames": adjacent_intervals,
        "allAdjacentIntervalsExactPeriod": not non_exact_intervals,
        "nonExactAdjacentIntervalsFrames": non_exact_intervals,
        "positiveExcessTotalMs": positive_excess_total,
        "positiveExcessAmortizedMsPerFrame": (
            positive_excess_total / len(samples)
        ),
        "p99Ms": selected_phase_p99,
        "withoutSelectedPhase": {
            "method": "exclude every finite sample whose index modulo period equals selected residue",
            "removedSampleCount": len(selected_samples),
            "remainingSampleCount": len(no_phase_values),
            "meanFrameTimeMs": no_phase_mean,
            "fpsFromMeanFrameTime": no_phase_fps,
            "meanFrameTimeDeltaVsOriginalMs": (
                no_phase_mean - global_mean if no_phase_mean is not None else None
            ),
            "fpsDeltaVsOriginal": (
                no_phase_fps - original_fps
                if no_phase_fps is not None and original_fps is not None
                else None
            ),
        },
        "maxP99Comparison": {
            "globalMaxMs": global_max,
            "globalP99Ms": global_p99,
            "selectedPhaseMaxMs": selected_phase_max,
            "selectedPhaseP99Ms": selected_phase_p99,
            "selectedPhaseContainsGlobalMax": any(
                value == global_max for value in selected_values
            ),
            "selectedPhaseMaxMinusGlobalP99Ms": (
                selected_phase_max - global_p99
                if global_p99 is not None else None
            ),
            "selectedPhaseSamplesAtOrAboveGlobalP99": sum(
                1 for value in selected_values
                if global_p99 is not None and value >= global_p99
            ),
            "periodicPositiveExcessAmortizedMsPerFrame": (
                positive_excess_total / len(samples)
            ),
        },
    }

    return {
        "available": True,
        "periodFrames": period_frames,
        "trimThresholdMs": trim_threshold_ms,
        "sourceFrameCount": len(frame_times),
        "finiteFrameCount": len(samples),
        "invalidFrameCount": len(frame_times) - len(samples),
        "global": {
            "meanFrameTimeMs": global_mean,
            "fpsFromMeanFrameTime": original_fps,
            "medianFrameTimeMs": float(statistics.median(all_values)),
            "trimmedSampleCount": len(trimmed_values),
            "trimmedMedianMs": global_trimmed_median,
            "trimmedMedianFallbackToAllFinite": trimmed_fallback,
            "maxMs": global_max,
            "maxIndices": global_max_indices,
            "p99Ms": global_p99,
            "p99Method": "nearest-rank ceil(0.99*N)",
        },
        "residues": residue_rows,
        "selectedPhase": selected_phase,
    }


def periodic_analysis_from_report(report: Path | None) -> Dict[str, Any]:
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        text = report.read_text(encoding="utf-8")
        marker = "const data = "
        marker_offset = text.find(marker)
        if marker_offset < 0:
            raise ValueError("embedded report data marker not found")
        data, _ = json.JSONDecoder().raw_decode(
            text[marker_offset + len(marker):]
        )
        frame_times = data.get("frameTimes")
        if not isinstance(frame_times, list):
            raise ValueError("embedded report data has no frameTimes array")
        analysis = analyze_periodic_frame_spikes(frame_times)
        analysis["sourceReport"] = str(report)
        return analysis
    except Exception as exc:
        return {
            "available": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


_PERIODIC_PAIRED_STAGE_NAMES = (
    "presentPreTracking", "worldHookInclusive", "worldCollector",
    "worldOriginal", "worldTrackNewBatches", "flushRoot", "flushNotify",
    "flushTransactionBegin", "flushOriginalBody", "flushReimplOpaque",
    "flushReimplTransparent", "flushTransactionEnd", "dispatchRoot",
    "dispatchResolveSemantic", "dispatchNativeBegin", "dispatchExecBegin",
    "dispatchOriginal", "dispatchPublishVisible", "dispatchExecEnd",
    "dispatchNativeEnd", "reimplExecBegin", "reimplExecEnd",
)
_PERIODIC_FLUSH_TERMINAL_NAMES = (
    "unclassified", "missingGlobalsOriginal", "missingDispatchOriginal",
    "decisionFallbackOriginal", "opaqueFailureOriginal",
    "transparentFailureOriginal", "takeoverSuccess",
)
_PERIODIC_DISPATCH_CLOSURE_NAMES = (
    "dispatchPath", "dispatchRoot", "worldFast", "getTagStage",
    "rawTiming", "qpcReads", "pairedTiming", "flushTopology", "overall",
)
_PERIODIC_FLUSH_CHILD_STAGES = (
    "flushNotify", "flushTransactionBegin", "flushOriginalBody",
    "flushReimplOpaque", "flushReimplTransparent", "flushTransactionEnd",
)
_PERIODIC_DISPATCH_CHILD_STAGES = (
    "dispatchResolveSemantic", "dispatchNativeBegin", "dispatchExecBegin",
    "dispatchOriginal", "dispatchPublishVisible", "dispatchExecEnd",
    "dispatchNativeEnd",
)
_PERIODIC_GROUP_OUTCOME_NAMES = (
    "unclassified", "validNonEmpty", "validEmptyAfterFilter", "nullWorld",
    "invalidGroup", "unreadableWorld", "nullList", "unreadableList",
    "nullData", "countZero", "countCap", "unreadableData",
    "filteredNoTargets",
)
_PERIODIC_GROUP_CLOSURE_NAMES = (
    "outcome", "partition", "hookCollectorCalls", "hookContainsCollector",
    "registerContainsFeeds", "entryBounds", "unobservedZero",
)
_UINT64_MAX = (1 << 64) - 1


def _periodic_uint(value: Any, field: str) -> int:
    if (isinstance(value, bool) or not isinstance(value, int)
            or value < 0 or value > _UINT64_MAX):
        raise ValueError(f"{field} is not an unsigned 64-bit integer")
    return value


def _periodic_bool(value: Any, field: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{field} is not boolean")
    return value


def _parse_periodic_raw_timing(raw: Any, field: str) -> Dict[str, int]:
    if not isinstance(raw, dict):
        raise ValueError(f"{field} is not an object")
    return {
        name: _periodic_uint(raw.get(name), f"{field}.{name}")
        for name in ("calls", "ticks", "maxTicks")
    }


def _parse_periodic_dispatch_block(
    raw: Any, field: str, frequency: int,
) -> Dict[str, Any]:
    if not isinstance(raw, dict):
        raise ValueError(f"{field} is not an object")
    capture_requested = _periodic_bool(
        raw.get("captureRequested"), f"{field}.captureRequested"
    )
    finalized = _periodic_bool(raw.get("finalized"), f"{field}.finalized")
    scalar_names = (
        "captureFrameSerial", "ownerThreadId", "qpcReadCount",
        "commonCalls", "specialCalls", "group0Calls", "otherStageCalls",
        "dispatchRootCalls", "dispatchRootTicks",
        "worldFastEligibleIgnoringIdentity", "worldFastBlockedByIdentity",
        "getTagStageCalls", "getTagStageHits", "getTagStageMisses",
        "getTagStageConflicts", "getTagStageProbes", "getTagStageTicks",
    )
    values = {
        name: _periodic_uint(raw.get(name), f"{field}.{name}")
        for name in scalar_names
    }

    topology_raw = raw.get("flushTopology")
    if not isinstance(topology_raw, dict):
        raise ValueError(f"{field}.flushTopology is not an object")
    topology = {
        name: _periodic_uint(
            topology_raw.get(name), f"{field}.flushTopology.{name}"
        )
        for name in ("calls", "opaqueTotal", "transparentTotal", "hash")
    }
    terminals_raw = raw.get("flushTerminalCounts")
    if not isinstance(terminals_raw, dict) or set(terminals_raw) != set(
            _PERIODIC_FLUSH_TERMINAL_NAMES):
        raise ValueError(f"{field}.flushTerminalCounts shape mismatch")
    terminals = {
        name: _periodic_uint(
            terminals_raw.get(name), f"{field}.flushTerminalCounts.{name}"
        )
        for name in _PERIODIC_FLUSH_TERMINAL_NAMES
    }
    stages_raw = raw.get("stageTimings")
    if not isinstance(stages_raw, dict) or set(stages_raw) != set(
            _PERIODIC_PAIRED_STAGE_NAMES):
        raise ValueError(f"{field}.stageTimings shape mismatch")
    stages = {
        name: _parse_periodic_raw_timing(
            stages_raw.get(name), f"{field}.stageTimings.{name}"
        )
        for name in _PERIODIC_PAIRED_STAGE_NAMES
    }
    residual_raw = raw.get("residualTicks")
    if not isinstance(residual_raw, dict):
        raise ValueError(f"{field}.residualTicks is not an object")
    residual = {
        name: _periodic_uint(
            residual_raw.get(name), f"{field}.residualTicks.{name}"
        )
        for name in ("flush", "dispatch")
    }
    closures_raw = raw.get("closures")
    if not isinstance(closures_raw, dict) or set(closures_raw) != set(
            _PERIODIC_DISPATCH_CLOSURE_NAMES):
        raise ValueError(f"{field}.closures shape mismatch")
    reported_closures = {
        name: _periodic_bool(
            closures_raw.get(name), f"{field}.closures.{name}"
        )
        for name in _PERIODIC_DISPATCH_CLOSURE_NAMES
    }

    dispatch_calls = values["commonCalls"] + values["specialCalls"]
    raw_timing_clean = True
    paired_timed_calls = 0
    for name, timing in stages.items():
        calls = timing["calls"]
        ticks = timing["ticks"]
        maximum = timing["maxTicks"]
        max_product_fits = maximum == 0 or calls <= _UINT64_MAX // maximum
        raw_timing_clean = bool(
            raw_timing_clean and maximum <= ticks
            and (
                (calls == 0 and ticks == 0 and maximum == 0)
                or (
                    calls > 0 and max_product_fits
                    and ticks <= calls * maximum
                )
            )
        )
        if name != "worldCollector":
            paired_timed_calls += calls
    qpc_formula_fits = bool(
        paired_timed_calls <= _UINT64_MAX // 2
        and values["getTagStageCalls"] <=
            _UINT64_MAX // 2 - paired_timed_calls
    )
    expected_qpc_reads = (
        2 * (paired_timed_calls + values["getTagStageCalls"])
        if qpc_formula_fits else None
    )
    qpc_reads_clean = bool(
        qpc_formula_fits and values["qpcReadCount"] == expected_qpc_reads
    )
    world_known_ticks = sum(
        stages[name]["ticks"] for name in (
            "worldCollector", "worldOriginal", "worldTrackNewBatches"
        )
    )
    flush_known_ticks = sum(
        stages[name]["ticks"] for name in _PERIODIC_FLUSH_CHILD_STAGES
    )
    dispatch_known_ticks = sum(
        stages[name]["ticks"] for name in _PERIODIC_DISPATCH_CHILD_STAGES
    )
    residual_computed = {
        "world": (
            stages["worldHookInclusive"]["ticks"] - world_known_ticks
            if stages["worldHookInclusive"]["ticks"] >= world_known_ticks
            else 0
        ),
        "flush": (
            stages["flushRoot"]["ticks"] - flush_known_ticks
            if stages["flushRoot"]["ticks"] >= flush_known_ticks else 0
        ),
        "dispatch": (
            stages["dispatchRoot"]["ticks"] - dispatch_known_ticks
            if stages["dispatchRoot"]["ticks"] >= dispatch_known_ticks else 0
        ),
    }
    residual_matches = bool(
        residual["flush"] == residual_computed["flush"]
        and residual["dispatch"] == residual_computed["dispatch"]
    )
    terminal_calls = sum(terminals.values())
    computed_closures: Dict[str, bool] = {
        "dispatchPath": bool(
            dispatch_calls ==
            values["group0Calls"] + values["otherStageCalls"]
        ),
        "dispatchRoot": bool(
            (dispatch_calls == 0 or values["dispatchRootCalls"] != 0)
            and (values["dispatchRootCalls"] != 0
                 or values["dispatchRootTicks"] == 0)
        ),
        "worldFast": bool(
            values["worldFastBlockedByIdentity"] <=
            values["worldFastEligibleIgnoringIdentity"] <= dispatch_calls
        ),
        "getTagStage": bool(
            values["getTagStageCalls"] ==
            values["getTagStageHits"] + values["getTagStageMisses"]
            and values["getTagStageConflicts"] <=
            values["getTagStageHits"] <= values["getTagStageProbes"]
            and values["getTagStageProbes"] <=
                values["getTagStageCalls"] * 16
        ),
        "rawTiming": raw_timing_clean,
        "qpcReads": qpc_reads_clean,
    }
    computed_closures["flushTopology"] = bool(
        stages["flushRoot"]["calls"] == values["dispatchRootCalls"]
        and stages["flushRoot"]["ticks"] == values["dispatchRootTicks"]
        and topology["calls"] == stages["flushRoot"]["calls"]
        and terminal_calls == stages["flushRoot"]["calls"]
        and terminals["unclassified"] == 0
    )
    computed_closures["pairedTiming"] = bool(
        stages["presentPreTracking"]["calls"] == 1
        and stages["flushNotify"]["calls"] == stages["flushRoot"]["calls"]
        and stages["flushTransactionBegin"]["calls"] ==
            stages["flushRoot"]["calls"]
        and stages["flushOriginalBody"]["calls"] <=
            stages["flushRoot"]["calls"]
        and stages["flushReimplOpaque"]["calls"] <=
            stages["flushRoot"]["calls"]
        and stages["flushReimplTransparent"]["calls"] <=
            stages["flushRoot"]["calls"]
        and stages["flushOriginalBody"]["calls"]
            + stages["flushReimplOpaque"]["calls"] >=
            stages["flushRoot"]["calls"]
        and stages["flushTransactionEnd"]["calls"] <=
            stages["flushTransactionBegin"]["calls"]
        and stages["dispatchRoot"]["calls"] == dispatch_calls
        and stages["dispatchResolveSemantic"]["calls"] == dispatch_calls
        and stages["dispatchNativeBegin"]["calls"] == dispatch_calls
        and stages["dispatchOriginal"]["calls"] == dispatch_calls
        and stages["dispatchNativeEnd"]["calls"] == dispatch_calls
        and raw_timing_clean and qpc_reads_clean
        and stages["worldHookInclusive"]["ticks"] >= world_known_ticks
        and stages["flushRoot"]["ticks"] >= flush_known_ticks
        and stages["dispatchRoot"]["ticks"] >= dispatch_known_ticks
        and (stages["flushRoot"]["calls"] == 0
             or values["qpcReadCount"] != 0)
    )
    extended_zero = bool(
        values["captureFrameSerial"] == 0
        and values["ownerThreadId"] == 0
        and values["qpcReadCount"] == 0
        and all(value == 0 for value in topology.values())
        and all(value == 0 for value in terminals.values())
        and all(
            timing[name] == 0
            for timing in stages.values()
            for name in ("calls", "ticks", "maxTicks")
        )
    )
    legacy_zero = all(
        values[name] == 0 for name in (
            "commonCalls", "specialCalls", "group0Calls", "otherStageCalls",
            "dispatchRootCalls", "dispatchRootTicks",
            "worldFastEligibleIgnoringIdentity",
            "worldFastBlockedByIdentity", "getTagStageCalls",
            "getTagStageHits", "getTagStageMisses", "getTagStageConflicts",
            "getTagStageProbes", "getTagStageTicks",
        )
    )
    computed_closures["overall"] = bool(
        finalized
        and all(computed_closures[name] for name in (
            "dispatchPath", "dispatchRoot", "worldFast", "getTagStage",
            "pairedTiming", "flushTopology",
        ))
        if capture_requested
        else (not finalized and legacy_zero and extended_zero)
    )
    producer_closures_match = all(
        reported_closures[name] == computed_closures[name]
        for name in _PERIODIC_DISPATCH_CLOSURE_NAMES
    )
    root_contains_get_tag = bool(
        stages["flushRoot"]["ticks"] >= values["getTagStageTicks"]
    )
    return {
        "captureRequested": capture_requested,
        "finalized": finalized,
        **values,
        "flushTopology": topology,
        "flushTerminalCounts": terminals,
        "stageTimings": stages,
        "reportedResidualTicks": residual,
        "computedResidualTicks": residual_computed,
        "residualsMatch": residual_matches,
        "reportedClosures": reported_closures,
        "computedClosures": computed_closures,
        "producerClosuresMatch": producer_closures_match,
        "expectedQpcReadCount": expected_qpc_reads,
        "terminalCallSum": terminal_calls,
        "rootContainsGetTag": root_contains_get_tag,
        "dispatchRootMs": values["dispatchRootTicks"] * 1000.0 / frequency,
        "getTagStageMs": values["getTagStageTicks"] * 1000.0 / frequency,
        "contractClosed": bool(
            producer_closures_match and residual_matches
            and root_contains_get_tag
            and computed_closures["overall"]
        ),
    }


def _parse_periodic_event_groups(
    event: Dict[str, Any], field: str,
) -> Dict[str, Any]:
    mask_names = (
        "collectorGroupMask", "hookGroupMask", "observedGroupMask",
        "duplicateCollectorGroupMask", "duplicateHookGroupMask",
    )
    masks = {
        name: _periodic_uint(event.get(name), f"{field}.{name}")
        for name in mask_names
    }
    raw_groups = event.get("groups")
    if not isinstance(raw_groups, list) or len(raw_groups) != 3:
        raise ValueError(f"{field}.groups must contain exactly three groups")
    int_names = (
        "hookCalls", "collectorCalls", "modelFeedCalls", "shadowFeedCalls",
        "hookInclusiveTicks", "collectorInclusiveTicks", "setupTicks",
        "iterateTicks", "registerTicks", "tailTicks", "modelFeedTicks",
        "shadowFeedTicks", "listEntries", "acceptedEntries",
        "sceneNodeEntries", "handleEntries",
    )
    parsed_groups: List[Dict[str, Any]] = []
    raw_observed_mask = masks["collectorGroupMask"] | masks["hookGroupMask"]
    computed_observed_mask = raw_observed_mask & 0x7
    group_closure_clean = True
    unobserved_groups_zero = True
    for index, group_raw in enumerate(raw_groups):
        if not isinstance(group_raw, dict):
            raise ValueError(f"{field}.groups[{index}] is not an object")
        group_index = _periodic_uint(
            group_raw.get("group"), f"{field}.groups[{index}].group"
        )
        if group_index != index:
            raise ValueError(f"{field}.groups[{index}] index mismatch")
        observed = _periodic_bool(
            group_raw.get("observed"), f"{field}.groups[{index}].observed"
        )
        values = {
            name: _periodic_uint(
                group_raw.get(name), f"{field}.groups[{index}].{name}"
            )
            for name in int_names
        }
        outcomes_raw = group_raw.get("outcomes")
        if not isinstance(outcomes_raw, dict) or set(outcomes_raw) != set(
                _PERIODIC_GROUP_OUTCOME_NAMES):
            raise ValueError(f"{field}.groups[{index}].outcomes shape mismatch")
        outcomes = {
            name: _periodic_uint(
                outcomes_raw.get(name),
                f"{field}.groups[{index}].outcomes.{name}",
            )
            for name in _PERIODIC_GROUP_OUTCOME_NAMES
        }
        closures_raw = group_raw.get("closures")
        if not isinstance(closures_raw, dict) or set(closures_raw) != set(
                _PERIODIC_GROUP_CLOSURE_NAMES):
            raise ValueError(f"{field}.groups[{index}].closures shape mismatch")
        reported_closures = {
            name: _periodic_bool(
                closures_raw.get(name),
                f"{field}.groups[{index}].closures.{name}",
            )
            for name in _PERIODIC_GROUP_CLOSURE_NAMES
        }
        expected_observed = (computed_observed_mask & (1 << index)) != 0
        outcome_calls = sum(outcomes.values())
        zero_residual = bool(
            all(value == 0 for value in values.values())
            and outcome_calls == 0
        )
        computed_closures = {
            "outcome": outcome_calls == values["collectorCalls"],
            "partition": values["collectorInclusiveTicks"] == sum(
                values[name] for name in (
                    "setupTicks", "iterateTicks", "registerTicks", "tailTicks"
                )
            ),
            "hookCollectorCalls": (
                values["hookCalls"] == values["collectorCalls"]
            ),
            "hookContainsCollector": (
                values["hookInclusiveTicks"] >=
                values["collectorInclusiveTicks"]
            ),
            "registerContainsFeeds": (
                values["registerTicks"] >=
                    values["modelFeedTicks"] + values["shadowFeedTicks"]
                and values["modelFeedCalls"] == values["shadowFeedCalls"]
            ),
            "entryBounds": (
                values["acceptedEntries"] <= values["listEntries"]
                and values["sceneNodeEntries"] <= values["acceptedEntries"]
                and values["handleEntries"] <= values["acceptedEntries"]
            ),
            "unobservedZero": expected_observed or zero_residual,
        }
        producer_closures_match = all(
            reported_closures[name] == computed_closures[name]
            for name in _PERIODIC_GROUP_CLOSURE_NAMES
        )
        if expected_observed:
            group_clean = bool(
                values["hookCalls"] != 0 and values["collectorCalls"] != 0
                and all(computed_closures[name] for name in (
                    "outcome", "partition", "hookCollectorCalls",
                    "hookContainsCollector", "registerContainsFeeds",
                    "entryBounds",
                ))
            )
        else:
            group_clean = zero_residual
            unobserved_groups_zero = unobserved_groups_zero and zero_residual
        group_closure_clean = group_closure_clean and group_clean
        parsed_groups.append({
            "group": group_index,
            "observed": observed,
            "expectedObserved": expected_observed,
            **values,
            "outcomes": outcomes,
            "reportedClosures": reported_closures,
            "computedClosures": computed_closures,
            "producerFlagsMatch": bool(
                observed == expected_observed and producer_closures_match
            ),
            "zeroResidual": zero_residual,
            "groupContractClosed": group_clean,
        })
    complete_observed = bool(
        raw_observed_mask == computed_observed_mask
        and masks["collectorGroupMask"] == masks["hookGroupMask"]
        and masks["duplicateCollectorGroupMask"] == 0
        and masks["duplicateHookGroupMask"] == 0
        and group_closure_clean and unobserved_groups_zero
    )
    return {
        **masks,
        "computedObservedGroupMask": computed_observed_mask,
        "computedGroupClosureClean": group_closure_clean,
        "computedUnobservedGroupsZeroClean": unobserved_groups_zero,
        "computedCompleteObservedGroups": complete_observed,
        "groups": parsed_groups,
        "producerGroupFlagsMatch": all(
            group["producerFlagsMatch"] for group in parsed_groups
        ),
        "periodicHookCalls": sum(g["hookCalls"] for g in parsed_groups),
        "periodicHookTicks": sum(
            g["hookInclusiveTicks"] for g in parsed_groups
        ),
        "periodicCollectorCalls": sum(
            g["collectorCalls"] for g in parsed_groups
        ),
        "periodicCollectorTicks": sum(
            g["collectorInclusiveTicks"] for g in parsed_groups
        ),
    }


def _periodic_pair_tick_delta(
    periodic: Dict[str, Any], control: Dict[str, Any], frequency: int,
) -> Dict[str, Any]:
    rows: List[Dict[str, Any]] = []
    for name in _PERIODIC_PAIRED_STAGE_NAMES:
        periodic_calls = periodic["stageTimings"][name]["calls"]
        control_calls = control["stageTimings"][name]["calls"]
        periodic_ticks = periodic["stageTimings"][name]["ticks"]
        control_ticks = control["stageTimings"][name]["ticks"]
        signed = periodic_ticks - control_ticks
        rows.append({
            "stage": name,
            "periodicCalls": periodic_calls,
            "controlCalls": control_calls,
            "callDelta": periodic_calls - control_calls,
            "callsEqual": periodic_calls == control_calls,
            "periodicTicks": periodic_ticks,
            "controlTicks": control_ticks,
            "periodicMinusControlTicks": signed,
            "periodicMinusControlMs": signed * 1000.0 / frequency,
            "classification": (
                "periodicExcess" if signed > 0
                else "controlExceededPeriodic" if signed < 0
                else "equal"
            ),
            "positivePeriodicExcessTicks": signed if signed > 0 else 0,
            "controlExceededPeriodicByTicks": -signed if signed < 0 else 0,
        })
    ranked = sorted(
        rows, key=lambda row: (-row["periodicMinusControlTicks"], row["stage"])
    )
    periodic_get_tag_qpc_reads = 2 * periodic["getTagStageCalls"]
    control_get_tag_qpc_reads = 2 * control["getTagStageCalls"]
    total_qpc_delta = periodic["qpcReadCount"] - control["qpcReadCount"]
    get_tag_qpc_delta = (
        periodic_get_tag_qpc_reads - control_get_tag_qpc_reads
    )
    return {
        "contract": "signed-periodic-minus-adjacent-control-raw-qpc-v1",
        "nestedTimingWarning": (
            "Inclusive stages overlap; rank stages individually and never sum "
            "them into a frame total."
        ),
        "negativeDeltaPolicy": (
            "A negative signed delta means the control was slower. It is "
            "reported verbatim and never counted as periodic savings."
        ),
        "qpcReadAttribution": {
            "periodicTotalQpcReads": periodic["qpcReadCount"],
            "controlTotalQpcReads": control["qpcReadCount"],
            "totalQpcDelta": total_qpc_delta,
            "totalQpcReadDelta": total_qpc_delta,
            "periodicGetTagQpcReads": periodic_get_tag_qpc_reads,
            "controlGetTagQpcReads": control_get_tag_qpc_reads,
            "getTagQpcDelta": get_tag_qpc_delta,
            "getTagQpcReadDelta": get_tag_qpc_delta,
            "nonGetTagQpcReadDelta": total_qpc_delta - get_tag_qpc_delta,
            "measurementPollution": (
                "GetTag timing deliberately adds two QPC reads per measured "
                "call. Total QPC equality is not required; only the count "
                "after subtracting those target reads is topology evidence."
            ),
        },
        "stages": rows,
        "rankedBySignedDeltaTicks": ranked,
        "positivePeriodicExcessRanking": [
            row for row in ranked if row["periodicMinusControlTicks"] > 0
        ],
        "negativeDeltaNotSavings": [
            row for row in reversed(ranked)
            if row["periodicMinusControlTicks"] < 0
        ],
        "residualTicks": {
            name: (
                periodic["computedResidualTicks"][name]
                - control["computedResidualTicks"][name]
            )
            for name in ("world", "flush", "dispatch")
        },
    }


def _analyze_periodic_dispatch_attribution(
    raw: Dict[str, Any], source_report: str,
) -> Dict[str, Any]:
    expected_contract = (
        "pure-periodic-plus-adjacent-control-render-tls-present-v3"
    )
    contract = raw.get("periodicDispatchContract")
    frequency = _periodic_uint(
        raw.get("qpcFrequency"), "worldObjectsMaintenanceTiming.qpcFrequency"
    )
    latest_sequence = _periodic_uint(
        raw.get("latestEventSequence"),
        "worldObjectsMaintenanceTiming.latestEventSequence",
    )
    if contract != expected_contract:
        raise ValueError(f"periodic dispatch contract mismatch: {contract!r}")
    if frequency == 0:
        raise ValueError("invalid worldObjectsMaintenanceTiming qpcFrequency")
    events = raw.get("events")
    if not isinstance(events, list):
        raise ValueError("worldObjectsMaintenanceTiming has no events array")
    faults_raw = raw.get("pairedCaptureFaults")
    if not isinstance(faults_raw, dict) or set(faults_raw) != {
        "duplicatePublish", "lostPublish", "slotMismatch",
    }:
        raise ValueError("pairedCaptureFaults shape mismatch")
    faults = {
        name: _periodic_uint(
            faults_raw.get(name), f"pairedCaptureFaults.{name}"
        )
        for name in ("duplicatePublish", "lostPublish", "slotMismatch")
    }
    paired_faults_clean = all(value == 0 for value in faults.values())

    parsed_events: List[Dict[str, Any]] = []
    completed_pairs: List[Dict[str, Any]] = []
    comparable_pairs: List[Dict[str, Any]] = []
    active_pairs: List[Dict[str, Any]] = []
    malformed: List[Dict[str, Any]] = []
    for event_index, event in enumerate(events):
        if not isinstance(event, dict):
            malformed.append({
                "eventIndex": event_index,
                "sequence": None,
                "reason": "event is not an object",
            })
            continue
        sequence = event.get("sequence")
        try:
            sequence = _periodic_uint(sequence, f"events[{event_index}].sequence")
            if sequence == 0 or sequence > latest_sequence:
                raise ValueError("sequence is outside retained lifetime range")
            event_int_names = (
                "frameSerial", "collectionFrameSerial", "poseSerial",
                "refreshPeriod", "reasonValue", "reasonMask",
                "trackingInclusiveTicks", "trackingQueryTicks",
                "trackingDecisionTicks",
            )
            event_values = {
                name: _periodic_uint(
                    event.get(name), f"events[{event_index}].{name}"
                )
                for name in event_int_names
            }
            wants_identity = _periodic_bool(
                event.get("wantsObjectIdentity"),
                f"events[{event_index}].wantsObjectIdentity",
            )
            wants_fallback = _periodic_bool(
                event.get("wantsFallbackBridge"),
                f"events[{event_index}].wantsFallbackBridge",
            )
            reported_tracking_partition = _periodic_bool(
                event.get("trackingPartitionClean"),
                f"events[{event_index}].trackingPartitionClean",
            )
            reported_complete_groups = _periodic_bool(
                event.get("completeObservedGroups"),
                f"events[{event_index}].completeObservedGroups",
            )
            reported_unobserved_zero = _periodic_bool(
                event.get("unobservedGroupsZeroClean"),
                f"events[{event_index}].unobservedGroupsZeroClean",
            )
            reported_group_closure = _periodic_bool(
                event.get("groupClosureClean"),
                f"events[{event_index}].groupClosureClean",
            )
            reported_pair_lifecycle = _periodic_bool(
                event.get("pairLifecycleClosureClean"),
                f"events[{event_index}].pairLifecycleClosureClean",
            )
            reported_subset = _periodic_bool(
                event.get("periodicEventSubsetClosureClean"),
                f"events[{event_index}].periodicEventSubsetClosureClean",
            )
            reported_qpc_excluding_get_tag = _periodic_bool(
                event.get("pairQpcBalancedExcludingGetTag"),
                f"events[{event_index}].pairQpcBalancedExcludingGetTag",
            )
            reported_qpc_including_get_tag = _periodic_bool(
                event.get("pairQpcBalancedIncludingGetTag"),
                f"events[{event_index}].pairQpcBalancedIncludingGetTag",
            )
            reported_topology_comparable = _periodic_bool(
                event.get("pairTopologyComparable"),
                f"events[{event_index}].pairTopologyComparable",
            )
            reported_pair_comparable = _periodic_bool(
                event.get("pairComparable"),
                f"events[{event_index}].pairComparable",
            )
            periodic = _parse_periodic_dispatch_block(
                event.get("periodicDispatch"),
                f"events[{event_index}].periodicDispatch", frequency,
            )
            control = _parse_periodic_dispatch_block(
                event.get("postPeriodicControl"),
                f"events[{event_index}].postPeriodicControl", frequency,
            )
            groups = _parse_periodic_event_groups(
                event, f"events[{event_index}]"
            )
        except ValueError as exc:
            malformed.append({
                "eventIndex": event_index,
                "sequence": sequence,
                "reason": str(exc),
            })
            continue

        pure_periodic_shape = bool(
            event.get("reason") == "periodicMaintenance"
            and event_values["reasonValue"] == 3
            and event_values["reasonMask"] == (1 << 3)
            and event_values["refreshPeriod"] > 0
            and event_values["poseSerial"] % event_values["refreshPeriod"] == 0
            and wants_identity
        )
        periodic_capture_shape = periodic["captureRequested"] == pure_periodic_shape
        event_capture_source_clean = bool(
            event_values["collectionFrameSerial"] ==
                event_values["frameSerial"] + 1
            and periodic["captureFrameSerial"] ==
                event_values["collectionFrameSerial"]
        )
        computed_tracking_partition = bool(
            event_values["trackingInclusiveTicks"] ==
            event_values["trackingQueryTicks"]
                + event_values["trackingDecisionTicks"]
        )
        computed_group_flags_match = bool(
            groups["producerGroupFlagsMatch"]
            and event.get("observedGroupMask") ==
                groups["computedObservedGroupMask"]
            and reported_complete_groups ==
                groups["computedCompleteObservedGroups"]
            and reported_unobserved_zero ==
                groups["computedUnobservedGroupsZeroClean"]
            and reported_group_closure ==
                groups["computedGroupClosureClean"]
        )
        periodic_world_hook = periodic["stageTimings"]["worldHookInclusive"]
        periodic_world_collector = periodic["stageTimings"]["worldCollector"]
        computed_subset = bool(
            periodic_world_collector["calls"] ==
                groups["periodicCollectorCalls"]
            and periodic_world_collector["ticks"] ==
                groups["periodicCollectorTicks"]
            and periodic_world_hook["calls"] >= groups["periodicHookCalls"]
            and periodic_world_hook["ticks"] >= groups["periodicHookTicks"]
        )
        computed_lifecycle = bool(
            event_capture_source_clean
            and periodic["captureRequested"] and periodic["finalized"]
            and control["captureRequested"] and control["finalized"]
            and control["captureFrameSerial"] ==
                periodic["captureFrameSerial"] + 1
            and periodic["ownerThreadId"] != 0
            and periodic["ownerThreadId"] == control["ownerThreadId"]
        )
        periodic_get_tag_qpc_bounded = bool(
            periodic["getTagStageCalls"] <= _UINT64_MAX // 2
            and periodic["qpcReadCount"] >=
                2 * periodic["getTagStageCalls"]
        )
        control_get_tag_qpc_bounded = bool(
            control["getTagStageCalls"] <= _UINT64_MAX // 2
            and control["qpcReadCount"] >= 2 * control["getTagStageCalls"]
        )
        computed_qpc_excluding_get_tag = bool(
            periodic_get_tag_qpc_bounded and control_get_tag_qpc_bounded
            and periodic["qpcReadCount"]
                - 2 * periodic["getTagStageCalls"]
                == control["qpcReadCount"]
                - 2 * control["getTagStageCalls"]
        )
        computed_qpc_including_get_tag = bool(
            periodic["qpcReadCount"] == control["qpcReadCount"]
        )
        computed_topology_comparable = bool(
            computed_lifecycle
            and periodic["computedClosures"]["overall"]
            and control["computedClosures"]["overall"]
            and periodic["flushTopology"]["calls"] > 0
            and control["flushTopology"]["calls"] > 0
            and periodic["stageTimings"]["worldHookInclusive"]["calls"] > 0
            and control["stageTimings"]["worldHookInclusive"]["calls"] > 0
            and periodic["stageTimings"]["worldHookInclusive"]["calls"] ==
                control["stageTimings"]["worldHookInclusive"]["calls"]
            and periodic["stageTimings"]["dispatchRoot"]["calls"] > 0
            and control["stageTimings"]["dispatchRoot"]["calls"] > 0
            and periodic["dispatchRootCalls"] == control["dispatchRootCalls"]
            and periodic["commonCalls"] == control["commonCalls"]
            and periodic["specialCalls"] == control["specialCalls"]
            and periodic["group0Calls"] == control["group0Calls"]
            and periodic["otherStageCalls"] == control["otherStageCalls"]
            and periodic["worldFastEligibleIgnoringIdentity"] ==
                control["worldFastEligibleIgnoringIdentity"]
            and control["worldFastBlockedByIdentity"] == 0
            and periodic["flushTopology"] == control["flushTopology"]
            and periodic["flushTerminalCounts"] ==
                control["flushTerminalCounts"]
            and computed_qpc_excluding_get_tag
            and groups["computedCompleteObservedGroups"]
            and computed_subset
        )
        computed_comparable = computed_topology_comparable
        producer_flags_match = bool(
            periodic["producerClosuresMatch"]
            and control["producerClosuresMatch"]
            and computed_group_flags_match
            and reported_tracking_partition == computed_tracking_partition
            and reported_subset == computed_subset
            and reported_qpc_excluding_get_tag ==
                computed_qpc_excluding_get_tag
            and reported_qpc_including_get_tag ==
                computed_qpc_including_get_tag
            and reported_topology_comparable == computed_topology_comparable
            and reported_pair_lifecycle == computed_lifecycle
            and reported_pair_comparable == computed_comparable
        )
        completed = bool(periodic["finalized"] and control["finalized"])
        active_periodic = bool(
            periodic["captureRequested"] and not periodic["finalized"]
            and not control["captureRequested"] and not control["finalized"]
        )
        active_control = bool(
            periodic["finalized"] and control["captureRequested"]
            and not control["finalized"]
        )
        row = {
            "sequence": sequence,
            **event_values,
            "reason": event.get("reason"),
            "wantsObjectIdentity": wants_identity,
            "wantsFallbackBridge": wants_fallback,
            "purePeriodicShape": pure_periodic_shape,
            "periodicCaptureShape": periodic_capture_shape,
            "eventCaptureSourceClean": event_capture_source_clean,
            "periodicDispatch": periodic,
            "postPeriodicControl": control,
            "groupEvidence": groups,
            "reportedEventFlags": {
                "trackingPartitionClean": reported_tracking_partition,
                "completeObservedGroups": reported_complete_groups,
                "unobservedGroupsZeroClean": reported_unobserved_zero,
                "groupClosureClean": reported_group_closure,
                "pairLifecycleClosureClean": reported_pair_lifecycle,
                "periodicEventSubsetClosureClean": reported_subset,
                "pairQpcBalancedExcludingGetTag":
                    reported_qpc_excluding_get_tag,
                "pairQpcBalancedIncludingGetTag":
                    reported_qpc_including_get_tag,
                "pairTopologyComparable": reported_topology_comparable,
                "pairComparable": reported_pair_comparable,
            },
            "computedEventFlags": {
                "trackingPartitionClean": computed_tracking_partition,
                "completeObservedGroups":
                    groups["computedCompleteObservedGroups"],
                "unobservedGroupsZeroClean":
                    groups["computedUnobservedGroupsZeroClean"],
                "groupClosureClean": groups["computedGroupClosureClean"],
                "pairLifecycleClosureClean": computed_lifecycle,
                "periodicEventSubsetClosureClean": computed_subset,
                "pairQpcBalancedExcludingGetTag":
                    computed_qpc_excluding_get_tag,
                "pairQpcBalancedIncludingGetTag":
                    computed_qpc_including_get_tag,
                "pairTopologyComparable": computed_topology_comparable,
                "pairComparable": computed_comparable,
            },
            "producerFlagsMatch": producer_flags_match,
            "completedPair": completed,
            "activePeriodicEndpoint": active_periodic,
            "activeControlEndpoint": active_control,
            "pairContractClosed": bool(
                pure_periodic_shape and periodic_capture_shape
                and computed_tracking_partition
                and completed and computed_comparable and producer_flags_match
                and periodic["contractClosed"] and control["contractClosed"]
            ),
            "periodicMinusControlRawTicks": None,
        }
        if (
            paired_faults_clean and reported_pair_comparable
            and computed_comparable and producer_flags_match
        ):
            row["periodicMinusControlRawTicks"] = _periodic_pair_tick_delta(
                periodic, control, frequency
            )
            comparable_pairs.append(row)
        if pure_periodic_shape and completed:
            completed_pairs.append(row)
        elif pure_periodic_shape and (active_periodic or active_control):
            active_pairs.append(row)
        elif pure_periodic_shape:
            malformed.append({
                "eventIndex": event_index,
                "sequence": sequence,
                "reason": "periodic pair has no valid completed/active lifecycle",
            })
        parsed_events.append(row)

    sequences = [row["sequence"] for row in parsed_events]
    sequences_unique = len(sequences) == len(set(sequences))
    active_is_exact_latest = bool(
        len(active_pairs) == 0
        or (
            len(active_pairs) == 1
            and active_pairs[0]["sequence"] == latest_sequence
            and all(
                row["sequence"] < active_pairs[0]["sequence"]
                for row in completed_pairs
            )
        )
    )
    completed_ordered = sorted(completed_pairs, key=lambda row: row["sequence"])
    cadence_closed = all(
        current["periodicDispatch"]["captureFrameSerial"]
            - previous["periodicDispatch"]["captureFrameSerial"]
            == current["refreshPeriod"]
        and current["poseSerial"] - previous["poseSerial"]
            == current["refreshPeriod"]
        for previous, current in zip(completed_ordered, completed_ordered[1:])
    )
    hypothesis_rows = comparable_pairs
    world_fast_equality = bool(
        hypothesis_rows and all(
            row["periodicDispatch"]["worldFastBlockedByIdentity"] ==
            row["periodicDispatch"]["worldFastEligibleIgnoringIdentity"]
            for row in hypothesis_rows
        )
    )
    get_tag_covers_blocked = bool(
        hypothesis_rows and all(
            row["periodicDispatch"]["getTagStageCalls"] >=
            row["periodicDispatch"]["worldFastBlockedByIdentity"]
            for row in hypothesis_rows
        )
    )
    blocked_positive = bool(
        hypothesis_rows and any(
            row["periodicDispatch"]["worldFastBlockedByIdentity"] > 0
            for row in hypothesis_rows
        )
    )
    contract_closed = bool(
        paired_faults_clean and not malformed and sequences_unique
        and active_is_exact_latest and cadence_closed
        and all(row["producerFlagsMatch"] for row in parsed_events)
        and all(row["pairContractClosed"] for row in completed_pairs)
    )

    aggregate_delta = None
    if comparable_pairs:
        aggregate_rows = []
        for stage in _PERIODIC_PAIRED_STAGE_NAMES:
            deltas = [
                next(
                    item for item in
                    row["periodicMinusControlRawTicks"]["stages"]
                    if item["stage"] == stage
                )["periodicMinusControlTicks"]
                for row in comparable_pairs
            ]
            total = sum(deltas)
            aggregate_rows.append({
                "stage": stage,
                "pairCount": len(deltas),
                "totalPeriodicMinusControlTicks": total,
                "meanPeriodicMinusControlTicks": statistics.fmean(deltas),
                "meanPeriodicMinusControlMs": (
                    statistics.fmean(deltas) * 1000.0 / frequency
                ),
                "classification": (
                    "periodicExcess" if total > 0
                    else "controlExceededPeriodic" if total < 0
                    else "equal"
                ),
            })
        aggregate_delta = {
            "pairCount": len(comparable_pairs),
            "stages": aggregate_rows,
            "rankedByMeanSignedDeltaTicks": sorted(
                aggregate_rows,
                key=lambda row: (
                    -row["meanPeriodicMinusControlTicks"], row["stage"]
                ),
            ),
            "negativeDeltaPolicy": (
                "Negative means control exceeded periodic and is not savings."
            ),
        }

    root_values = [
        row["periodicDispatch"]["dispatchRootMs"] for row in comparable_pairs
    ]
    tag_values = [
        row["periodicDispatch"]["getTagStageMs"] for row in comparable_pairs
    ]
    return {
        "available": True,
        "sourceReport": source_report,
        "contract": contract,
        "frequency": frequency,
        "latestEventSequence": latest_sequence,
        "pairedCaptureFaults": faults,
        "pairedCaptureFaultsClean": paired_faults_clean,
        "eventCount": len(events),
        "parsedEventCount": len(parsed_events),
        "captureRequestedCount": len(completed_pairs) + len(active_pairs),
        "finalizedCount": len(completed_pairs),
        "completedPairCount": len(completed_pairs),
        "pairComparableCount": len(comparable_pairs),
        "activeEndpointCount": len(active_pairs),
        "malformedCount": len(malformed),
        "evidencePresent": bool(comparable_pairs),
        "captureSequencesUnique": sequences_unique,
        "activeEndpointIsExactLatest": active_is_exact_latest,
        "purePeriodicCadenceClosed": cadence_closed,
        "contractClosed": contract_closed,
        "runtimeEvidenceReady": bool(contract_closed and comparable_pairs),
        "getTagHypothesisChecks": {
            "worldFastBlockedEqualsEligible": world_fast_equality,
            "getTagCallsCoverBlocked": get_tag_covers_blocked,
            "worldFastBlockedPositive": blocked_positive,
            "allSupportCurrentHypothesis": bool(
                world_fast_equality and get_tag_covers_blocked
                and blocked_positive
            ),
            "note": (
                "Hypothesis evidence only; these equalities are not a "
                "general dispatch contract."
            ),
        },
        "finalizedDispatchRootMs": {
            "mean": statistics.fmean(root_values) if root_values else None,
            "max": max(root_values) if root_values else None,
        },
        "finalizedGetTagStageMs": {
            "mean": statistics.fmean(tag_values) if tag_values else None,
            "max": max(tag_values) if tag_values else None,
        },
        "pairedRawTickDeltaAggregate": aggregate_delta,
        "latestFinalized": completed_pairs[-1] if completed_pairs else None,
        "finalizedEvents": completed_pairs,
        "comparableEvents": comparable_pairs,
        "activeEndpoints": active_pairs,
        "allParsedEvents": parsed_events,
        "malformedEvents": malformed,
    }


def periodic_dispatch_attribution_from_report(
    report: Path | None,
) -> Dict[str, Any]:
    """Extract independently closed periodic/adjacent-control attribution.

    Only a finalized, topology-comparable pair with matching producer flags
    may emit signed raw-tick deltas. One exact latest active endpoint is kept
    as lifecycle evidence but never treated as performance evidence.
    """
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        data = _embedded_report_data(report)
        raw = data.get("worldObjectsMaintenanceTiming")
        if not isinstance(raw, dict):
            raise ValueError("embedded report has no worldObjectsMaintenanceTiming")
        return _analyze_periodic_dispatch_attribution(raw, str(report))
    except Exception as exc:
        return {
            "available": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


def identity_dedup_from_report(report: Path | None) -> Dict[str, Any]:
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        text = report.read_text(encoding="utf-8")
        marker = "const data = "
        marker_offset = text.find(marker)
        if marker_offset < 0:
            raise ValueError("embedded report data marker not found")
        data, _ = json.JSONDecoder().raw_decode(
            text[marker_offset + len(marker):]
        )
        raw = data.get("identitySameFrameDedup")
        if not isinstance(raw, dict):
            raise ValueError("embedded report has no identitySameFrameDedup")
        result: Dict[str, Any] = {
            "available": True,
            "sourceReport": str(report),
        }
        for name in ("model", "shadow"):
            row = raw.get(name)
            if not isinstance(row, dict):
                raise ValueError(f"identitySameFrameDedup.{name} missing")
            miss_names = (
                "missMissingAlias", "missCrossFrame", "missNoBatchProof",
                "missIncomplete", "missInputMismatch",
                "missAliasConflict", "missRuntimeOwner",
            )
            values = {
                key: row.get(key)
                for key in ("attempts", "hits", *miss_names, "batchMarked")
            }
            if not all(isinstance(value, int) for value in values.values()):
                raise ValueError(
                    f"identitySameFrameDedup.{name} fields incomplete"
                )
            miss_total = sum(values[key] for key in miss_names)
            values["missTotal"] = miss_total
            values["closed"] = bool(
                row.get("closed") is True
                and values["attempts"] == values["hits"] + miss_total
            )
            result[name] = values
        result["allClosed"] = bool(
            result["model"]["closed"] and result["shadow"]["closed"]
        )
        return result
    except Exception as exc:
        return {
            "available": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


_GPU_SKIN_EVENT_TIMING_NAMES = (
    "flushRoot", "dispatchSemanticLookup", "dispatchBeginRoot",
    "dispatchEndRoot", "semanticInclusive", "semanticOriginal",
    "dipDeviceRootOutside", "dipDeviceRootNoUpload",
    "dipDeviceRootCorrelated", "dipBridgeOutside",
    "dipBridgeNoUpload", "dipBridgeCorrelated", "dipResolveOutside",
    "dipResolveNoUpload", "dipResolveCorrelated",
    # Parents for the v4 partition aliases below.  These are inclusive and
    # must never be added to their class/reason children.
    "outerAdmissionAccepted", "outerFastComplete",
    "outerFallbackInclusive",
)

_GPU_SKIN_OUTSIDE_ADMISSION_CLASSES = (
    "noPoisonFlush", "noPoisonIndependent",
    "poisonFlush", "poisonIndependent",
    "noPoisonSemantic", "poisonSemantic",
)

_GPU_SKIN_OUTSIDE_REJECT_REASONS = (
    "unknown",
    "activeFastMarker",
    "modeNotBypass",
    "fullDiagnostics",
    "ingressClosed",
    "bypassDisabled",
    "wrongThread",
    "nullDevice",
    "dispatchOwned",
    "semanticScopeHazard",
    "dispatchOverflow",
    "semanticOverflow",
    "nestedUpload",
    "genericUploadInFlight",
    "fastRejectUploadInFlight",
    "evidenceUploadInFlight",
    "retirementPending",
    "retirementQueueFault",
    "resetGenerationMismatch",
    "evidenceCohort",
    "poisonReadFailure",
    "poisonOverlap",
    "poisonPostScanRevalidation",
    "independentPinRevalidation",
)


def _runtime_outside_poison_sidecar_policy(raw: Any) -> Dict[str, Any]:
    expected_keys = (
        "policy", "policyValue", "explicit", "invalid",
        "counterPolicyValue", "counterExplicit", "counterInvalid",
        "counterClosureClean", "configCounterExact",
    )
    if not isinstance(raw, dict) or tuple(raw.keys()) != expected_keys:
        raise ValueError("outsidePoisonSidecar runtime policy shape mismatch")
    if type(raw["policy"]) is not str:
        raise ValueError("outsidePoisonSidecar.policy invalid")
    for name in ("policyValue", "counterPolicyValue"):
        value = raw[name]
        if type(value) is not int or value < 0 or value > _UINT64_MAX:
            raise ValueError(f"outsidePoisonSidecar.{name} invalid")
    for name in (
        "explicit", "invalid", "counterExplicit", "counterInvalid",
        "counterClosureClean", "configCounterExact",
    ):
        if type(raw[name]) is not bool:
            raise ValueError(f"outsidePoisonSidecar.{name} invalid")
    recomputed_config_counter_exact = bool(
        raw["policyValue"] == raw["counterPolicyValue"]
        and raw["explicit"] == raw["counterExplicit"]
        and raw["invalid"] == raw["counterInvalid"]
    )
    exact = bool(
        raw["policy"] == GPU_SKIN_POISON_SIDECAR_VALUE
        and raw["policyValue"] == 0
        and raw["explicit"] is True
        and raw["invalid"] is False
        and raw["counterPolicyValue"] == 0
        and raw["counterExplicit"] is True
        and raw["counterInvalid"] is False
        and raw["counterClosureClean"] is True
        and raw["configCounterExact"] is True
        and recomputed_config_counter_exact
    )
    return {
        **raw,
        "expectedPolicy": GPU_SKIN_POISON_SIDECAR_VALUE,
        "recomputedConfigCounterExact": recomputed_config_counter_exact,
        "exact": exact,
        "contract": (
            "immutable-runtime-config-and-native-counter-policy-exact-"
            "explicit-sidecar-none-v1"
        ),
    }


def _outside_poison_accepted_policy(
    production_light: bool,
    accepted_with_poison: int,
    poison_scan_no_overlap: int,
) -> Dict[str, Any]:
    """Close the manager-visible poison acceptance partition.

    Product-light + sidecar-none uses the O1 actual-Lock authority for ordinary
    poison-bearing uploads.  Only the frozen evidence cohort still performs the
    legacy pre-scan, so accepted-with-poison is the disjoint sum of the scanned
    no-overlap cohort and a derived product-authority cohort.  The independent
    O1 transaction/authority ledger remains a P4 forced-diagnostic hard gate;
    this performance-report parser intentionally labels the manager-visible
    difference only as an unscanned cohort, never independent authority.
    """
    for value, label in (
        (accepted_with_poison, "acceptedWithPoison"),
        (poison_scan_no_overlap, "poisonScanNoOverlap"),
    ):
        if type(value) is not int or value < 0 or value > _UINT64_MAX:
            raise ValueError(f"outside poison accepted policy {label} invalid")

    underflow = accepted_with_poison < poison_scan_no_overlap
    unscanned_accepted_with_poison = (
        accepted_with_poison - poison_scan_no_overlap
        if not underflow else None
    )
    if production_light:
        exact = bool(
            not underflow
            and unscanned_accepted_with_poison is not None
            and unscanned_accepted_with_poison > 0
        )
        contract = (
            "product-light-sidecar-none-acceptedWithPoison-equals-"
            "legacyEvidenceNoOverlap-plus-unscannedAccepted-positive-v1"
        )
    else:
        exact = bool(
            not underflow
            and unscanned_accepted_with_poison == 0
        )
        contract = "non-product-legacy-scan-acceptedWithPoison-equals-noOverlap-v1"
    return {
        "contract": contract,
        "productionLight": production_light,
        "acceptedWithPoison": accepted_with_poison,
        "legacyEvidenceNoOverlap": poison_scan_no_overlap,
        "derivedUnscannedAcceptedWithPoison": unscanned_accepted_with_poison,
        "underflow": underflow,
        "exact": exact,
        "independentAuthorityVerified": False,
        "authorityHardGate": "P4 forced nativePoisonO1Authority ledger",
    }

def gpu_skin_event_graph_from_report(report: Path | None) -> Dict[str, Any]:
    if report is None:
        return {"available": False, "error": "performance report unavailable"}
    try:
        text = report.read_text(encoding="utf-8")
        marker = "const data = "
        marker_offset = text.find(marker)
        if marker_offset < 0:
            raise ValueError("embedded report data marker not found")
        data, _ = json.JSONDecoder().raw_decode(
            text[marker_offset + len(marker):]
        )
        frame_count = data.get("frameCount")
        snapshot = data.get("gpuSkinSnapshot")
        if not isinstance(snapshot, dict):
            raise ValueError("embedded report has no gpuSkinSnapshot")
        mode = snapshot.get("mode")
        full_diagnostics = snapshot.get("fullDiagnostics")
        production_light = bool(
            mode == "bypass" and full_diagnostics is False
        )
        runtime_poison_sidecar = _runtime_outside_poison_sidecar_policy(
            snapshot.get("outsidePoisonSidecar")
        )
        timing = snapshot.get("timing")
        if not isinstance(timing, dict):
            raise ValueError("gpuSkinSnapshot has no timing")
        frequency = timing.get("frequency")
        sampled = timing.get("productionLightSampled")
        if not isinstance(sampled, dict):
            raise ValueError("productionLightSampled missing")
        period = sampled.get("period")
        phase = sampled.get("phase")
        snapshot_contract = snapshot.get("snapshotContract")
        population_contract = sampled.get("populationContract")
        descriptor_contract_exact = bool(
            snapshot_contract == "export-time-process-lifetime-v1"
            and population_contract ==
                "outer-kernel-event-independent_callback-stable-id-aligned"
        )
        if (type(frequency) is not int or frequency <= 0
                or frequency > _UINT64_MAX):
            raise ValueError("invalid production timing frequency")
        if period != 256 or phase != 0xA5:
            raise ValueError("unexpected production sampling contract")
        if (type(frame_count) is not int or frame_count <= 0
                or frame_count > _UINT64_MAX):
            raise ValueError("invalid report frameCount")

        def parse_timing_record(
            raw: Any, label: str,
        ) -> Dict[str, Any]:
            if not isinstance(raw, dict):
                raise ValueError(f"production timing {label} missing")
            calls = raw.get("calls")
            ticks = raw.get("ticks")
            max_ticks = raw.get("maxTicks")
            if not all(type(value) is int for value in (
                calls, ticks, max_ticks,
            )):
                raise ValueError(f"production timing {label} incomplete")
            total_within_per_call_max = bool(
                max_ticks == 0 and ticks == 0
                or max_ticks > 0 and (
                    calls > _UINT64_MAX // max_ticks
                    or ticks <= calls * max_ticks
                )
            )
            valid = bool(
                0 <= calls <= _UINT64_MAX
                and 0 <= ticks <= _UINT64_MAX
                and 0 <= max_ticks <= ticks
                and total_within_per_call_max
                and ((calls == 0 and ticks == 0 and max_ticks == 0)
                     or (calls > 0
                         and ((ticks == 0 and max_ticks == 0)
                              or (ticks > 0 and max_ticks > 0))))
            )
            sampled_total_ms = ticks * 1000.0 / frequency
            estimated_process_lifetime_total_ms = sampled_total_ms * period
            return {
                "calls": calls,
                "ticks": ticks,
                "maxTicks": max_ticks,
                "shapeValid": valid,
                "sampledTotalMs": sampled_total_ms,
                "averageUs": (
                    ticks * 1_000_000.0 / (frequency * calls)
                    if calls > 0 else None
                ),
                "maximumUs": max_ticks * 1_000_000.0 / frequency,
                # Native counters start at process initialization, while the
                # HTML frame history starts at resetHistory. Keep this value
                # in its real lifetime domain and never divide it by the
                # report-only frameCount.
                "estimatedProcessLifetimeTotalMs": (
                    estimated_process_lifetime_total_ms
                ),
            }

        records: Dict[str, Dict[str, Any]] = {}
        shape_valid = True
        for name in _GPU_SKIN_EVENT_TIMING_NAMES:
            record = parse_timing_record(sampled.get(name), name)
            shape_valid = shape_valid and record["shapeValid"]
            records[name] = record

        admission = snapshot.get("admission")
        if not isinstance(admission, dict):
            raise ValueError("gpuSkinSnapshot has no admission")
        outside_raw = admission.get("outsideAdmissionExact")
        if not isinstance(outside_raw, dict):
            raise ValueError("outsideAdmissionExact missing")
        reason_order = outside_raw.get("reasonOrder")
        if reason_order != list(_GPU_SKIN_OUTSIDE_REJECT_REASONS):
            raise ValueError("outside admission reason order mismatch")

        def exact_nonnegative_int(
            owner: Dict[str, Any], name: str, label: str,
        ) -> int:
            value = owner.get(name)
            if (type(value) is not int or value < 0
                    or value > _UINT64_MAX):
                raise ValueError(f"{label}.{name} invalid")
            return value

        def checked_u64_sum(values: Any, label: str) -> int:
            total = 0
            for value in values:
                if (type(value) is not int or value < 0
                        or value > _UINT64_MAX - total):
                    raise ValueError(f"{label} uint64 overflow")
                total += value
            return total

        accepted_raw = outside_raw.get("accepted")
        totals_raw = outside_raw.get("totals")
        closure_raw = outside_raw.get("closure")
        reject_no_poison_raw = outside_raw.get("rejectNoPoison")
        reject_with_poison_raw = outside_raw.get("rejectWithPoison")
        if not all(isinstance(value, dict) for value in (
            accepted_raw, totals_raw, closure_raw,
            reject_no_poison_raw, reject_with_poison_raw,
        )):
            raise ValueError("outside admission object shape incomplete")
        if (tuple(reject_no_poison_raw.keys()) !=
                _GPU_SKIN_OUTSIDE_REJECT_REASONS or
                tuple(reject_with_poison_raw.keys()) !=
                _GPU_SKIN_OUTSIDE_REJECT_REASONS):
            raise ValueError("outside admission reject keys/order mismatch")

        accepted = {
            name: exact_nonnegative_int(
                accepted_raw, name, "outsideAdmissionExact.accepted"
            )
            for name in ("noPoison", "withPoison", "total")
        }
        reject_no_poison = {
            name: exact_nonnegative_int(
                reject_no_poison_raw, name,
                "outsideAdmissionExact.rejectNoPoison",
            )
            for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
        }
        reject_with_poison = {
            name: exact_nonnegative_int(
                reject_with_poison_raw, name,
                "outsideAdmissionExact.rejectWithPoison",
            )
            for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
        }
        cancellations = exact_nonnegative_int(
            outside_raw, "cancellations", "outsideAdmissionExact"
        )
        lifecycle_excluded = exact_nonnegative_int(
            outside_raw, "lifecycleExcluded", "outsideAdmissionExact"
        )
        tracked_resolved_inside = exact_nonnegative_int(
            outside_raw, "trackedResolvedInside", "outsideAdmissionExact"
        )
        untracked_resolved_outside = exact_nonnegative_int(
            outside_raw, "untrackedResolvedOutside",
            "outsideAdmissionExact",
        )
        totals = {
            name: exact_nonnegative_int(
                totals_raw, name, "outsideAdmissionExact.totals"
            )
            for name in (
                "rejectNoPoison", "rejectWithPoison", "reject",
                "attempt", "trackedResolvedOutside",
                "resolvedExpectedOutside", "actualOutside",
            )
        }
        telemetry_batched_add_term = exact_nonnegative_int(
            outside_raw, "telemetryBatchedAddTerm",
            "outsideAdmissionExact",
        )
        unknown_index = exact_nonnegative_int(
            outside_raw, "unknownIndex", "outsideAdmissionExact"
        )
        closure_names = (
            "snapshotAvailable", "unknownHardZero",
            "acceptedResolutionClean", "outsideClean",
            "poisonScanClean", "poisonReasonClean", "arithmeticClean",
            "zeroWhenFull",
        )
        if not all(
            isinstance(closure_raw.get(name), bool)
            for name in closure_names
        ):
            raise ValueError("outside admission closure incomplete")

        accepted_partition_total = checked_u64_sum(
            (accepted["noPoison"], accepted["withPoison"]),
            "outsideAdmissionExact.accepted partition",
        )
        reject_no_poison_total = checked_u64_sum(
            reject_no_poison.values(),
            "outsideAdmissionExact.rejectNoPoison total",
        )
        reject_with_poison_total = checked_u64_sum(
            reject_with_poison.values(),
            "outsideAdmissionExact.rejectWithPoison total",
        )
        reject_total = checked_u64_sum(
            (reject_no_poison_total, reject_with_poison_total),
            "outsideAdmissionExact.reject total",
        )
        attempt_total = checked_u64_sum(
            (accepted["total"], reject_total),
            "outsideAdmissionExact.attempt total",
        )
        tracked_exclusions = checked_u64_sum(
            (cancellations, lifecycle_excluded, tracked_resolved_inside),
            "outsideAdmissionExact.tracked exclusions",
        )
        tracked_resolved_outside = (
            attempt_total - tracked_exclusions
            if attempt_total >= tracked_exclusions else None
        )
        resolved_expected = (
            checked_u64_sum(
                (tracked_resolved_outside, untracked_resolved_outside),
                "outsideAdmissionExact.resolved expected",
            )
            if tracked_resolved_outside is not None
            else None
        )
        telemetry_batched_add_expected = checked_u64_sum(
            (
                attempt_total, cancellations, lifecycle_excluded,
                tracked_resolved_inside, untracked_resolved_outside,
            ),
            "outsideAdmissionExact.telemetry batched-add term",
        )
        outside_native_fast = exact_nonnegative_int(
            admission, "outsideNativeFastPath", "admission"
        )
        direct_raw = admission.get("outsideNoPoisonDirectOriginal")
        if not isinstance(direct_raw, dict):
            raise ValueError("outsideNoPoisonDirectOriginal missing")
        direct = {
            name: exact_nonnegative_int(
                direct_raw, name, "outsideNoPoisonDirectOriginal"
            )
            for name in (
                "attempts", "kernelCalls", "normalReturns",
                "kernelNoNormalReturns", "completed", "conflicts",
                "cancellations", "active",
                "resetCompletedWhileActive", "latePoison",
            )
        }
        direct_attempts_in_admission = exact_nonnegative_int(
            outside_raw, "noPoisonDirectOriginalAttempts",
            "outsideAdmissionExact",
        )
        direct_policy_closed = bool(
            direct_attempts_in_admission == direct["attempts"]
            and direct["attempts"] ==
                direct["completed"] + direct["cancellations"]
            and direct["kernelCalls"] ==
                direct["normalReturns"] +
                direct["kernelNoNormalReturns"]
            and direct["kernelCalls"] == direct["completed"]
            and direct["completed"] <= outside_native_fast
            and direct["attempts"] <= accepted["noPoison"]
            and direct["conflicts"] == 0
            and direct["cancellations"] == 0
            and direct["active"] == 0
            and direct["resetCompletedWhileActive"] == 0
            and direct["latePoison"] == 0
            and (
                production_light and direct["attempts"] > 0
                or not production_light and all(
                    value == 0 for value in direct.values()
                )
            )
        )
        outside_uploads_parent = exact_nonnegative_int(
            admission, "uploadsOutsideDispatch", "admission"
        )
        poison_scan_attempts = exact_nonnegative_int(
            admission, "outsidePoisonScanAttempts", "admission"
        )
        poison_no_overlap = exact_nonnegative_int(
            admission, "outsidePoisonNoOverlapAdmissions", "admission"
        )
        poison_overlap = exact_nonnegative_int(
            admission, "outsidePoisonOverlapRejects", "admission"
        )
        poison_read_fail = exact_nonnegative_int(
            admission, "outsidePoisonReadFailRejects", "admission"
        )
        poison_read_reason_total = checked_u64_sum(
            (
                reject_with_poison["poisonReadFailure"],
                reject_with_poison["poisonPostScanRevalidation"],
                reject_with_poison["independentPinRevalidation"],
            ),
            "outsideAdmissionExact.poison read reasons",
        )
        poison_scan_terminal_total = checked_u64_sum(
            (poison_no_overlap, poison_overlap, poison_read_fail),
            "outsideAdmissionExact.poison scan terminals",
        )
        poison_accepted_policy = _outside_poison_accepted_policy(
            production_light,
            accepted["withPoison"],
            poison_no_overlap,
        )
        computed_outside_closures = {
            "accepted": accepted["total"] == accepted_partition_total,
            "rejectNoPoison": (
                totals["rejectNoPoison"] == reject_no_poison_total
            ),
            "rejectWithPoison": (
                totals["rejectWithPoison"] == reject_with_poison_total
            ),
            "reject": totals["reject"] == reject_total,
            "attempt": totals["attempt"] == attempt_total,
            "trackedResolvedOutside": (
                tracked_resolved_outside is not None
                and totals["trackedResolvedOutside"] ==
                    tracked_resolved_outside
            ),
            "resolvedExpected": (
                resolved_expected is not None
                and totals["resolvedExpectedOutside"] == resolved_expected
            ),
            "telemetryTerm": (
                telemetry_batched_add_term ==
                    telemetry_batched_add_expected
            ),
            "unknownHardZero": (
                unknown_index == 0
                and reject_no_poison["unknown"] == 0
                and reject_with_poison["unknown"] == 0
            ),
            "populationSentinelsHardZero": all(
                table[name] == 0
                for table in (reject_no_poison, reject_with_poison)
                for name in (
                    "unknown", "modeNotBypass", "fullDiagnostics",
                    "dispatchOwned",
                )
            ),
            "acceptedResolution": (
                accepted["total"] == outside_native_fast
            ),
            "outside": (
                resolved_expected is not None
                and resolved_expected == totals["actualOutside"]
                and totals["actualOutside"] == outside_uploads_parent
            ),
            "poisonScan": poison_scan_attempts == poison_scan_terminal_total,
            "poisonAccepted": bool(
                runtime_poison_sidecar["exact"]
                and poison_accepted_policy["exact"]
            ),
            "poisonReason": (
                reject_no_poison["poisonReadFailure"] == 0
                and reject_no_poison["poisonOverlap"] == 0
                and reject_no_poison["poisonPostScanRevalidation"] == 0
                and reject_with_poison["poisonOverlap"] == poison_overlap
                and poison_read_reason_total == poison_read_fail
            ),
        }

        cover_raw = admission.get("outsideUploadCover")
        if not isinstance(cover_raw, dict):
            raise ValueError("outsideUploadCover missing")
        cover_begins_raw = cover_raw.get("begins")
        cover_pins_raw = cover_raw.get("independentPins")
        cover_closure_raw = cover_raw.get("closure")
        if not all(isinstance(value, dict) for value in (
            cover_begins_raw, cover_pins_raw, cover_closure_raw,
        )):
            raise ValueError("outsideUploadCover object shape incomplete")
        cover_begin_names = ("flush", "semantic", "independent", "total")
        cover_begins = {
            name: exact_nonnegative_int(
                cover_begins_raw, name, "outsideUploadCover.begins"
            )
            for name in cover_begin_names
        }
        cover_independent_pins = {
            name: exact_nonnegative_int(
                cover_pins_raw, name, "outsideUploadCover.independentPins"
            )
            for name in ("begins", "ends")
        }
        cover_accepted_total = exact_nonnegative_int(
            cover_raw, "acceptedTotal", "outsideUploadCover"
        )
        cover_direct_attempts = exact_nonnegative_int(
            cover_raw, "directOriginalAttempts", "outsideUploadCover"
        )
        cover_closure_names = (
            "arithmeticClean", "acceptedClean",
            "independentPinsClean", "zeroWhenFull",
        )
        if not all(
            isinstance(cover_closure_raw.get(name), bool)
            for name in cover_closure_names
        ):
            raise ValueError("outsideUploadCover closure incomplete")
        cover_begin_total = checked_u64_sum(
            (cover_begins[name] for name in (
                "flush", "semantic", "independent",
            )),
            "outsideUploadCover begin total",
        )
        computed_cover_closures = {
            "arithmeticClean": True,
            "beginTotal": cover_begins["total"] == cover_begin_total,
            "directAttemptCopiesExact": (
                cover_direct_attempts == direct["attempts"] ==
                    direct_attempts_in_admission
            ),
            "acceptedClean": (
                checked_u64_sum(
                    (cover_begin_total, cover_direct_attempts),
                    "outsideUploadCover covered plus direct",
                ) == cover_accepted_total == accepted["total"]
            ),
            "independentPinsClean": (
                cover_independent_pins["begins"] ==
                    cover_independent_pins["ends"]
            ),
            "independentPinsCover": (
                cover_independent_pins["begins"] >=
                    cover_begins["independent"]
            ),
        }
        cover_fields_zero = bool(
            cover_begin_total == 0
            and cover_independent_pins["begins"] == 0
            and cover_independent_pins["ends"] == 0
        )
        cover_report_matches = bool(
            cover_closure_raw["arithmeticClean"] ==
                computed_cover_closures["arithmeticClean"]
            and cover_closure_raw["acceptedClean"] ==
                computed_cover_closures["acceptedClean"]
            and cover_closure_raw["independentPinsClean"] ==
                computed_cover_closures["independentPinsClean"]
            and cover_closure_raw["zeroWhenFull"] ==
                (not full_diagnostics or cover_fields_zero)
        )
        cover_policy_closed = bool(
            cover_raw.get("contract") ==
                "accepted-fast-minus-direct-borrows-exact-flush-or-"
                "semantic-otherwise-independent-v2"
            and cover_report_matches
            and all(computed_cover_closures.values())
            and (
                production_light
                and cover_begin_total > 0
                and cover_begins["semantic"] > 0
                or not production_light and cover_fields_zero
            )
        )

        accepted_alias_raw = sampled.get("outerAdmissionAcceptedByClass")
        complete_alias_raw = sampled.get("outerFastCompleteByClass")
        fallback_alias_raw = sampled.get("outerFallbackByReason")
        alias_closure_raw = sampled.get("outerAdmissionAliasClosure")
        if not all(isinstance(value, dict) for value in (
            accepted_alias_raw, complete_alias_raw,
            fallback_alias_raw, alias_closure_raw,
        )):
            raise ValueError("outer admission alias timing missing")
        if (tuple(accepted_alias_raw.keys()) !=
                _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES or
                tuple(complete_alias_raw.keys()) !=
                _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES or
                tuple(fallback_alias_raw.keys()) !=
                _GPU_SKIN_OUTSIDE_REJECT_REASONS):
            raise ValueError("outer admission alias timing order mismatch")
        accepted_aliases = {
            name: parse_timing_record(
                accepted_alias_raw[name],
                f"outerAdmissionAcceptedByClass.{name}",
            )
            for name in _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES
        }
        complete_aliases = {
            name: parse_timing_record(
                complete_alias_raw[name],
                f"outerFastCompleteByClass.{name}",
            )
            for name in _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES
        }
        fallback_aliases = {
            name: parse_timing_record(
                fallback_alias_raw[name],
                f"outerFallbackByReason.{name}",
            )
            for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
        }
        alias_shape_valid = all(
            row["shapeValid"]
            for table in (
                accepted_aliases, complete_aliases, fallback_aliases,
            )
            for row in table.values()
        )
        shape_valid = shape_valid and alias_shape_valid

        def timing_partition_closed(
            parent: Dict[str, Any], children: Dict[str, Dict[str, Any]],
        ) -> bool:
            child_calls = checked_u64_sum(
                (row["calls"] for row in children.values()),
                "production timing partition calls",
            )
            child_ticks = checked_u64_sum(
                (row["ticks"] for row in children.values()),
                "production timing partition ticks",
            )
            return bool(
                parent["calls"] == child_calls
                and parent["ticks"] == child_ticks
                and parent["maxTicks"] == max(
                    (row["maxTicks"] for row in children.values()),
                    default=0,
                )
            )

        computed_alias_closures = {
            "acceptedClass": timing_partition_closed(
                records["outerAdmissionAccepted"], accepted_aliases
            ),
            "completeClass": timing_partition_closed(
                records["outerFastComplete"], complete_aliases
            ),
            "fallbackReason": timing_partition_closed(
                records["outerFallbackInclusive"], fallback_aliases
            ),
            "unknownHardZero": all(
                fallback_aliases["unknown"][name] == 0
                for name in ("calls", "ticks", "maxTicks")
            ),
            "acceptedCompleteClassCalls": all(
                accepted_aliases[name]["calls"] ==
                    complete_aliases[name]["calls"]
                for name in _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES
            ),
        }
        alias_report_names = (
            "snapshotAvailable", "acceptedClassClean",
            "completeClassClean", "fallbackReasonClean",
            "unknownHardZero", "zeroWhenFull",
        )
        if not all(
            isinstance(alias_closure_raw.get(name), bool)
            for name in alias_report_names
        ):
            raise ValueError("outer admission alias closure incomplete")
        alias_report_matches = bool(
            alias_closure_raw.get("contract") ==
                "partition-alias-inclusive-do-not-add-to-parent"
            and (
                production_light
                and alias_closure_raw["snapshotAvailable"]
                and alias_closure_raw["acceptedClassClean"] ==
                    computed_alias_closures["acceptedClass"]
                and alias_closure_raw["completeClassClean"] ==
                    computed_alias_closures["completeClass"]
                and alias_closure_raw["fallbackReasonClean"] ==
                    computed_alias_closures["fallbackReason"]
                and alias_closure_raw["unknownHardZero"] ==
                    computed_alias_closures["unknownHardZero"]
                or not production_light
                and not alias_closure_raw["snapshotAvailable"]
                and not alias_closure_raw["acceptedClassClean"]
                and not alias_closure_raw["completeClassClean"]
                and not alias_closure_raw["fallbackReasonClean"]
                and not alias_closure_raw["unknownHardZero"]
                and alias_closure_raw["zeroWhenFull"]
            )
        )
        exact_fields_zero = bool(
            attempt_total == 0 and cancellations == 0
            and lifecycle_excluded == 0
            and tracked_resolved_inside == 0
            and untracked_resolved_outside == 0
        )
        alias_fields_zero = all(
            row[name] == 0
            for table in (
                accepted_aliases, complete_aliases, fallback_aliases,
            )
            for row in table.values()
            for name in ("calls", "ticks", "maxTicks")
        )
        outside_policy_closed = bool(
            outside_raw.get("populationContract") ==
                "bypass-light-entry-dispatch-depth-zero-first-terminal-"
                "plus-final-boundary-v2"
            and outside_raw.get("attemptContract") ==
                "accepted-plus-first-reject-disjoint"
            and outside_raw.get("outsideClosureDenominator") ==
                "attempt-minus-cancellation-minus-lifecycleExcluded-minus-"
                "trackedResolvedInside-plus-untrackedResolvedOutside"
            and outside_raw.get("availabilityContract") ==
                "render-thread-quiescent-published-live-ingress-no-exclusions"
            and closure_raw["arithmeticClean"]
            and all(computed_outside_closures[name] for name in (
                "accepted", "rejectNoPoison", "rejectWithPoison",
                "reject", "attempt", "trackedResolvedOutside",
                "resolvedExpected", "telemetryTerm",
                "unknownHardZero", "poisonScan", "poisonAccepted",
                "poisonReason", "populationSentinelsHardZero",
            ))
            and closure_raw["poisonScanClean"] ==
                computed_outside_closures["poisonScan"]
            and closure_raw["poisonReasonClean"] ==
                computed_outside_closures["poisonReason"]
            and (
                production_light
                and outside_native_fast > 0
                and cancellations == 0
                and lifecycle_excluded == 0
                and closure_raw["snapshotAvailable"]
                and closure_raw["unknownHardZero"]
                and closure_raw["acceptedResolutionClean"]
                and closure_raw["outsideClean"]
                and closure_raw["poisonScanClean"]
                and closure_raw["poisonReasonClean"]
                and computed_outside_closures["acceptedResolution"]
                and computed_outside_closures["outside"]
                or not production_light
                and exact_fields_zero
                and not closure_raw["snapshotAvailable"]
                and not closure_raw["unknownHardZero"]
                and not closure_raw["acceptedResolutionClean"]
                and not closure_raw["outsideClean"]
                and closure_raw["zeroWhenFull"]
            )
        )
        alias_policy_closed = bool(
            alias_shape_valid and alias_report_matches
            and all(computed_alias_closures.values())
            and (
                production_light
                and alias_closure_raw["snapshotAvailable"]
                and records["outerAdmissionAccepted"]["calls"] > 0
                and records["outerFallbackInclusive"]["calls"] > 0
                or not production_light
                and alias_fields_zero
                and alias_closure_raw["zeroWhenFull"]
            )
        )
        outside_admission = {
            "populationContract": outside_raw.get("populationContract"),
            "attemptContract": outside_raw.get("attemptContract"),
            "outsideClosureDenominator": outside_raw.get(
                "outsideClosureDenominator"
            ),
            "availabilityContract": outside_raw.get(
                "availabilityContract"
            ),
            "reasonOrder": reason_order,
            "accepted": accepted,
            "cancellations": cancellations,
            "lifecycleExcluded": lifecycle_excluded,
            "trackedResolvedInside": tracked_resolved_inside,
            "untrackedResolvedOutside": untracked_resolved_outside,
            "rejectNoPoison": reject_no_poison,
            "rejectWithPoison": reject_with_poison,
            "totals": totals,
            "reportedClosure": {
                name: closure_raw[name] for name in closure_names
            },
            "computedClosure": computed_outside_closures,
            "poisonAcceptedPolicy": poison_accepted_policy,
            "runtimePoisonSidecar": runtime_poison_sidecar,
            "telemetryBatchedAddTerm": telemetry_batched_add_term,
            "noPoisonDirectOriginal": direct,
            "noPoisonDirectOriginalPolicyClosed": direct_policy_closed,
            "parentUploadsOutsideDispatch": outside_uploads_parent,
            "unknownIndex": unknown_index,
            "policyClosed": outside_policy_closed,
        }
        outside_upload_cover = {
            "contract": cover_raw.get("contract"),
            "begins": cover_begins,
            "independentPins": cover_independent_pins,
            "acceptedTotal": cover_accepted_total,
            "directOriginalAttempts": cover_direct_attempts,
            "reportedClosure": {
                name: cover_closure_raw[name]
                for name in cover_closure_names
            },
            "computedClosure": computed_cover_closures,
            "reportedMatchesComputed": cover_report_matches,
            "policyClosed": cover_policy_closed,
        }
        outer_admission_aliases = {
            "contract": alias_closure_raw.get("contract"),
            "acceptedByClass": accepted_aliases,
            "completeByClass": complete_aliases,
            "fallbackByReason": fallback_aliases,
            "reportedClosure": {
                name: alias_closure_raw[name]
                for name in alias_report_names
            },
            "computedClosure": computed_alias_closures,
            "reportedMatchesComputed": alias_report_matches,
            "policyClosed": alias_policy_closed,
            "note": "Inclusive aliases partition parents; do not sum aliases with parents.",
        }

        semantic = {
            "callsClosed": (
                records["semanticInclusive"]["calls"] ==
                records["semanticOriginal"]["calls"]
            ),
            "ticksContained": (
                records["semanticInclusive"]["ticks"] >=
                records["semanticOriginal"]["ticks"]
            ),
            "maxContained": (
                records["semanticInclusive"]["maxTicks"] >=
                records["semanticOriginal"]["maxTicks"]
            ),
        }
        dip_classes: Dict[str, Any] = {}
        dip_closed = True
        for suffix, label in (
            ("Outside", "outside"), ("NoUpload", "noUpload"),
            ("Correlated", "correlated"),
        ):
            device = records[f"dipDeviceRoot{suffix}"]
            bridge = records[f"dipBridge{suffix}"]
            resolve = records[f"dipResolve{suffix}"]
            row = {
                "callsClosed": (
                    device["calls"] == bridge["calls"]
                    and resolve["calls"] <= device["calls"]
                ),
                "ticksContained": (
                    device["ticks"] >= checked_u64_sum(
                        (bridge["ticks"], resolve["ticks"]),
                        f"production timing DIP {label} child ticks",
                    )
                ),
                "maxContained": (
                    device["maxTicks"] >= bridge["maxTicks"]
                    and device["maxTicks"] >= resolve["maxTicks"]
                ),
            }
            dip_closed = dip_closed and all(row.values())
            dip_classes[label] = row
        dip_total_device = checked_u64_sum(
            (records[name]["calls"] for name in (
                "dipDeviceRootOutside", "dipDeviceRootNoUpload",
                "dipDeviceRootCorrelated",
            )),
            "production timing DIP device calls",
        )
        dip_total_bridge = checked_u64_sum(
            (records[name]["calls"] for name in (
                "dipBridgeOutside", "dipBridgeNoUpload",
                "dipBridgeCorrelated",
            )),
            "production timing DIP bridge calls",
        )
        dip_total_closed = dip_total_device == dip_total_bridge
        writer = sampled.get("writerSnapshot")
        if not isinstance(writer, dict):
            raise ValueError("production timing writerSnapshot missing")
        writer_started = _periodic_uint(
            writer.get("started"), "production timing writerSnapshot.started"
        )
        writer_completed = _periodic_uint(
            writer.get("completed"),
            "production timing writerSnapshot.completed",
        )
        writer_active = _periodic_uint(
            writer.get("active"), "production timing writerSnapshot.active"
        )
        writer_pending = _periodic_bool(
            writer.get("pending"), "production timing writerSnapshot.pending"
        )
        writer_closed = bool(
            writer_started == writer_completed
            and writer_active == 0 and not writer_pending
        )
        all_zero = all(
            row["calls"] == row["ticks"] == row["maxTicks"] == 0
            for row in records.values()
        )
        known_mode = type(mode) is str and mode in {
            "disabled", "observe", "dual", "shadow", "main", "bypass",
        }
        light_event_evidence_positive = bool(
            records["flushRoot"]["calls"] > 0
            and records["dispatchSemanticLookup"]["calls"] > 0
            and records["dispatchBeginRoot"]["calls"] > 0
            and records["dispatchEndRoot"]["calls"] > 0
            and records["semanticInclusive"]["calls"] > 0
            and dip_total_device > 0
        )
        policy_closed = bool(
            known_mode and isinstance(full_diagnostics, bool)
            and ((full_diagnostics is True and all_zero)
            or (full_diagnostics is False and mode == "bypass"
                and not all_zero and light_event_evidence_positive)
            or (full_diagnostics is False and mode != "bypass" and all_zero))
        )
        inclusive_names = (
            "flushRoot", "dispatchSemanticLookup", "dispatchBeginRoot",
            "dispatchEndRoot", "semanticInclusive",
            "dipDeviceRootOutside", "dipDeviceRootNoUpload",
            "dipDeviceRootCorrelated",
        )
        ranked = sorted(
            (
                {"stage": name, **records[name]}
                for name in inclusive_names
            ),
            key=lambda row: row["estimatedProcessLifetimeTotalMs"],
            reverse=True,
        )
        return {
            "available": True,
            "sourceReport": str(report),
            "mode": mode,
            "fullDiagnostics": full_diagnostics,
            "snapshotContract": snapshot_contract,
            "populationContract": population_contract,
            "descriptorContractExact": descriptor_contract_exact,
            "frameCount": frame_count,
            "timeDomain": (
                "process-lifetime sampled counters; report frameCount has a "
                "different resetHistory window"
            ),
            "formalMsPerFrameAvailable": False,
            "frequency": frequency,
            "period": period,
            "phase": phase,
            "writerClosed": writer_closed,
            "shapeValid": shape_valid,
            "semantic": semantic,
            "dipClasses": dip_classes,
            "dipTotalDeviceCalls": dip_total_device,
            "dipTotalBridgeCalls": dip_total_bridge,
            "dipTotalCallsClosed": dip_total_closed,
            "outsideAdmissionExact": outside_admission,
            "outsideNoPoisonDirectOriginal": direct,
            "outsideUploadCover": outside_upload_cover,
            "outerAdmissionAliases": outer_admission_aliases,
            "allZero": all_zero,
            "lightEventEvidencePositive": light_event_evidence_positive,
            "policyClosed": policy_closed,
            "contractClosed": bool(
                descriptor_contract_exact and writer_closed
                and shape_valid and all(semantic.values())
                and dip_closed and dip_total_closed and policy_closed
                and runtime_poison_sidecar["exact"]
                and outside_policy_closed and cover_policy_closed
                and alias_policy_closed and direct_policy_closed
            ),
            "rankedInclusiveByEstimatedProcessLifetimeMs": ranked,
            "records": records,
        }
    except Exception as exc:
        return {
            "available": False,
            "sourceReport": str(report),
            "error": repr(exc),
        }


def compare_periodic_analyses(cases: List[Dict[str, Any]]) -> Dict[str, Any]:
    cases_by_mode = {
        mode: [case for case in cases if case.get("mode") == mode]
        for mode in ("disabled", "bypass")
    }
    if not all(cases_by_mode.values()):
        return {
            "available": False,
            "error": "at least one completed case per mode required",
        }
    analyses_by_mode = {
        mode: [case.get("periodic300FrameAnalysis", {}) for case in rows]
        for mode, rows in cases_by_mode.items()
    }
    if not all(
        bool(analysis.get("available"))
        for analyses in analyses_by_mode.values()
        for analysis in analyses
    ):
        return {
            "available": False,
            "error": "periodic analysis unavailable for one or more cases",
        }

    def focus(analysis: Dict[str, Any]) -> Dict[str, Any]:
        global_row = analysis["global"]
        phase = analysis["selectedPhase"]
        without = phase["withoutSelectedPhase"]
        return {
            "selectedResidue": phase["residue"],
            "globalMaxMs": global_row["maxMs"],
            "globalP99Ms": global_row["p99Ms"],
            "globalTrimmedMedianMs": global_row["trimmedMedianMs"],
            "periodicMeanExcessMs": phase[
                "meanExcessVsGlobalTrimmedMedianMs"
            ],
            "periodicMedianExcessMs": phase[
                "medianExcessVsGlobalTrimmedMedianMs"
            ],
            "periodicMaxExcessMs": phase[
                "maxExcessVsGlobalTrimmedMedianMs"
            ],
            "periodicPositiveExcessAmortizedMsPerFrame": phase[
                "positiveExcessAmortizedMsPerFrame"
            ],
            "selectedPhaseContainsGlobalMax": phase["maxP99Comparison"][
                "selectedPhaseContainsGlobalMax"
            ],
            "withoutPhaseMeanFrameTimeMs": without["meanFrameTimeMs"],
            "withoutPhaseFpsFromMean": without["fpsFromMeanFrameTime"],
        }

    focused_by_mode = {
        mode: [focus(analysis) for analysis in analyses]
        for mode, analyses in analyses_by_mode.items()
    }
    delta_fields = (
        "globalMaxMs",
        "globalP99Ms",
        "globalTrimmedMedianMs",
        "periodicMeanExcessMs",
        "periodicMedianExcessMs",
        "periodicMaxExcessMs",
        "periodicPositiveExcessAmortizedMsPerFrame",
        "withoutPhaseMeanFrameTimeMs",
        "withoutPhaseFpsFromMean",
    )
    aggregate_by_mode: Dict[str, Dict[str, Any]] = {}
    for mode, rows in focused_by_mode.items():
        aggregate: Dict[str, Any] = {
            "runCount": len(rows),
            "selectedResidues": [row["selectedResidue"] for row in rows],
        }
        for field in delta_fields:
            values = [
                float(row[field]) for row in rows if row.get(field) is not None
            ]
            aggregate[field] = statistics.fmean(values) if values else None
        aggregate_by_mode[mode] = aggregate

    disabled = aggregate_by_mode["disabled"]
    bypass = aggregate_by_mode["bypass"]
    deltas = {}
    for field in delta_fields:
        if bypass.get(field) is not None and disabled.get(field) is not None:
            deltas[field] = float(bypass[field]) - float(disabled[field])
    all_residues = disabled["selectedResidues"] + bypass["selectedResidues"]
    return {
        "available": True,
        "periodFrames": PERIODIC_SPIKE_PERIOD_FRAMES,
        "disabled": disabled,
        "bypass": bypass,
        "bypassMinusDisabled": deltas,
        "selectedResidueSame": len(set(all_residues)) == 1,
        "runs": focused_by_mode,
    }


def _run_periodic_analysis_self_test() -> None:
    frame_times = [10.0 + ((index % 5) - 2) * 0.02 for index in range(1200)]
    injected_indices = list(range(73, len(frame_times), 300))
    for index in injected_indices:
        frame_times[index] = 34.0 + (index // 300)

    analysis = analyze_periodic_frame_spikes(frame_times)
    phase = analysis["selectedPhase"]
    assert analysis["available"]
    assert len(analysis["residues"]) == 300
    assert phase["residue"] == 73
    assert [row["index"] for row in phase["samples"]] == injected_indices
    assert phase["adjacentIntervalsFrames"] == [300, 300, 300]
    assert phase["allAdjacentIntervalsExactPeriod"]
    assert phase["maxP99Comparison"]["selectedPhaseContainsGlobalMax"]
    assert phase["maxP99Comparison"]["selectedPhaseMaxMinusGlobalP99Ms"] > 0.0
    assert phase["withoutSelectedPhase"]["meanFrameTimeMs"] < analysis["global"][
        "meanFrameTimeMs"
    ]
    assert phase["withoutSelectedPhase"]["fpsFromMeanFrameTime"] > analysis[
        "global"
    ]["fpsFromMeanFrameTime"]

    bypass_frame_times = list(frame_times)
    for index in injected_indices:
        bypass_frame_times[index] += 4.0
    bypass_analysis = analyze_periodic_frame_spikes(bypass_frame_times)
    comparison = compare_periodic_analyses(
        [
            {
                "mode": "disabled",
                "periodic300FrameAnalysis": analysis,
            },
            {
                "mode": "bypass",
                "periodic300FrameAnalysis": bypass_analysis,
            },
        ]
    )
    assert comparison["available"]
    assert comparison["selectedResidueSame"]
    assert comparison["bypassMinusDisabled"]["periodicMedianExcessMs"] > 0.0
    assert comparison["bypassMinusDisabled"]["globalMaxMs"] == 4.0


def _run_periodic_dispatch_parser_self_test() -> None:
    contract = "pure-periodic-plus-adjacent-control-render-tls-present-v3"

    def timing(calls: int, ticks: int) -> Dict[str, int]:
        return {
            "calls": calls,
            "ticks": ticks,
            "maxTicks": ((ticks + calls - 1) // calls) if calls else 0,
        }

    def block(periodic: bool) -> Dict[str, Any]:
        stage_specs = {
            "presentPreTracking": (1, 12 if periodic else 14),
            "worldHookInclusive": (3, 90 if periodic else 82),
            "worldCollector": (3, 15),
            "worldOriginal": (3, 30),
            "worldTrackNewBatches": (3, 15),
            "flushRoot": (2, 200 if periodic else 170),
            "flushNotify": (2, 10),
            "flushTransactionBegin": (2, 10),
            "flushOriginalBody": (1, 60),
            "flushReimplOpaque": (1, 40),
            "flushReimplTransparent": (0, 0),
            "flushTransactionEnd": (2, 10),
            "dispatchRoot": (3, 180 if periodic else 160),
            "dispatchResolveSemantic": (3, 15),
            "dispatchNativeBegin": (3, 15),
            "dispatchExecBegin": (3, 15),
            "dispatchOriginal": (3, 15),
            "dispatchPublishVisible": (3, 15),
            "dispatchExecEnd": (3, 15),
            "dispatchNativeEnd": (3, 15),
            "reimplExecBegin": (1, 5),
            "reimplExecEnd": (1, 5),
        }
        stages = {
            name: timing(*stage_specs[name])
            for name in _PERIODIC_PAIRED_STAGE_NAMES
        }
        get_tag_calls = 2 if periodic else 0
        paired_calls = sum(
            row["calls"] for name, row in stages.items()
            if name != "worldCollector"
        )
        flush_known = sum(
            stages[name]["ticks"] for name in _PERIODIC_FLUSH_CHILD_STAGES
        )
        dispatch_known = sum(
            stages[name]["ticks"] for name in _PERIODIC_DISPATCH_CHILD_STAGES
        )
        return {
            "captureRequested": True,
            "finalized": True,
            "captureFrameSerial": 300 if periodic else 301,
            "ownerThreadId": 77,
            "qpcReadCount": 2 * (paired_calls + get_tag_calls),
            "commonCalls": 2,
            "specialCalls": 1,
            "group0Calls": 2,
            "otherStageCalls": 1,
            "dispatchRootCalls": 2,
            "dispatchRootTicks": stages["flushRoot"]["ticks"],
            "worldFastEligibleIgnoringIdentity": 2,
            "worldFastBlockedByIdentity": 2 if periodic else 0,
            "getTagStageCalls": get_tag_calls,
            "getTagStageHits": 1 if periodic else 0,
            "getTagStageMisses": 1 if periodic else 0,
            "getTagStageConflicts": 0,
            "getTagStageProbes": 1 if periodic else 0,
            "getTagStageTicks": 20 if periodic else 0,
            "flushTopology": {
                "calls": 2,
                "opaqueTotal": 8,
                "transparentTotal": 3,
                "hash": 0xABCDEF,
            },
            "flushTerminalCounts": {
                name: (2 if name == "takeoverSuccess" else 0)
                for name in _PERIODIC_FLUSH_TERMINAL_NAMES
            },
            "stageTimings": stages,
            "residualTicks": {
                "flush": stages["flushRoot"]["ticks"] - flush_known,
                "dispatch": stages["dispatchRoot"]["ticks"] - dispatch_known,
            },
            "closures": {
                name: True for name in _PERIODIC_DISPATCH_CLOSURE_NAMES
            },
        }

    def group(index: int) -> Dict[str, Any]:
        outcomes = {name: 0 for name in _PERIODIC_GROUP_OUTCOME_NAMES}
        outcomes["validNonEmpty"] = 1
        return {
            "group": index,
            "observed": True,
            "hookCalls": 1,
            "collectorCalls": 1,
            "modelFeedCalls": 1,
            "shadowFeedCalls": 1,
            "hookInclusiveTicks": 20,
            "collectorInclusiveTicks": 5,
            "setupTicks": 1,
            "iterateTicks": 1,
            "registerTicks": 2,
            "tailTicks": 1,
            "modelFeedTicks": 1,
            "shadowFeedTicks": 1,
            "listEntries": 3,
            "acceptedEntries": 2,
            "sceneNodeEntries": 1,
            "handleEntries": 1,
            "outcomes": outcomes,
            "closures": {
                name: True for name in _PERIODIC_GROUP_CLOSURE_NAMES
            },
        }

    def event(sequence: int = 1) -> Dict[str, Any]:
        return {
            "sequence": sequence,
            "frameSerial": sequence * 300 - 1,
            "collectionFrameSerial": sequence * 300,
            "poseSerial": sequence * 300,
            "reason": "periodicMaintenance",
            "reasonValue": 3,
            "reasonMask": 1 << 3,
            "refreshPeriod": 300,
            "trackingInclusiveTicks": 30,
            "trackingQueryTicks": 12,
            "trackingDecisionTicks": 18,
            "trackingPartitionClean": True,
            "wantsObjectIdentity": True,
            "wantsFallbackBridge": False,
            "collectorGroupMask": 0x7,
            "hookGroupMask": 0x7,
            "observedGroupMask": 0x7,
            "duplicateCollectorGroupMask": 0,
            "duplicateHookGroupMask": 0,
            "completeObservedGroups": True,
            "unobservedGroupsZeroClean": True,
            "groupClosureClean": True,
            "pairLifecycleClosureClean": True,
            "periodicEventSubsetClosureClean": True,
            # The identity-on periodic side deliberately has two GetTag
            # samples (four QPC reads); topology is equal after subtracting
            # those target reads, while total reads are intentionally unequal.
            "pairQpcBalancedExcludingGetTag": True,
            "pairQpcBalancedIncludingGetTag": False,
            "pairTopologyComparable": True,
            "pairComparable": True,
            "periodicDispatch": block(True),
            "postPeriodicControl": block(False),
            "groups": [group(index) for index in range(3)],
        }

    def analyze(
        synthetic_events: List[Dict[str, Any]], latest: int = 1,
        faults: Dict[str, int] | None = None,
    ) -> Dict[str, Any]:
        return _analyze_periodic_dispatch_attribution(
            {
                "periodicDispatchContract": contract,
                "qpcFrequency": 10_000_000,
                "latestEventSequence": latest,
                "pairedCaptureFaults": faults or {
                    "duplicatePublish": 0,
                    "lostPublish": 0,
                    "slotMismatch": 0,
                },
                "events": synthetic_events,
            },
            "synthetic",
        )

    good = analyze([event()])
    assert good["contractClosed"]
    assert good["runtimeEvidenceReady"]
    assert good["completedPairCount"] == 1
    assert good["pairComparableCount"] == 1
    delta = good["comparableEvents"][0]["periodicMinusControlRawTicks"]
    qpc = delta["qpcReadAttribution"]
    assert qpc["totalQpcReadDelta"] == 4
    assert qpc["getTagQpcReadDelta"] == 4
    assert qpc["nonGetTagQpcReadDelta"] == 0
    present = next(
        row for row in delta["stages"]
        if row["stage"] == "presentPreTracking"
    )
    assert present["periodicMinusControlTicks"] < 0
    assert present["classification"] == "controlExceededPeriodic"
    assert present in delta["negativeDeltaNotSavings"]
    assert present not in delta["positivePeriodicExcessRanking"]
    assert all(row["callsEqual"] for row in delta["stages"])

    frame_gap = event()
    frame_gap["postPeriodicControl"]["captureFrameSerial"] += 1
    frame_gap["pairLifecycleClosureClean"] = False
    frame_gap["pairTopologyComparable"] = False
    frame_gap["pairComparable"] = False
    result = analyze([frame_gap])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    source_frame_mismatch = event()
    source_frame_mismatch["frameSerial"] -= 1
    # The producer-local N/N+1 pair still claims success; the independent
    # event/capture cross-check must reject the mixed-slot evidence.
    result = analyze([source_frame_mismatch])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    hash_mismatch = event()
    hash_mismatch["postPeriodicControl"]["flushTopology"]["hash"] += 1
    hash_mismatch["pairTopologyComparable"] = False
    hash_mismatch["pairComparable"] = False
    result = analyze([hash_mismatch])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    terminal_mismatch = event()
    terminals = terminal_mismatch["postPeriodicControl"][
        "flushTerminalCounts"
    ]
    terminals["takeoverSuccess"] = 1
    terminals["missingGlobalsOriginal"] = 1
    terminal_mismatch["pairTopologyComparable"] = False
    terminal_mismatch["pairComparable"] = False
    result = analyze([terminal_mismatch])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    qpc_mismatch = event()
    control = qpc_mismatch["postPeriodicControl"]
    control["stageTimings"]["reimplExecEnd"] = timing(2, 6)
    control["qpcReadCount"] += 2
    qpc_mismatch["pairQpcBalancedExcludingGetTag"] = False
    qpc_mismatch["pairTopologyComparable"] = False
    qpc_mismatch["pairComparable"] = False
    result = analyze([qpc_mismatch])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    subset_mismatch = event()
    subset_mismatch["periodicDispatch"]["stageTimings"][
        "worldCollector"
    ] = timing(3, 16)
    subset_mismatch["periodicEventSubsetClosureClean"] = False
    subset_mismatch["pairTopologyComparable"] = False
    subset_mismatch["pairComparable"] = False
    result = analyze([subset_mismatch])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    missing_stage = event()
    del missing_stage["periodicDispatch"]["stageTimings"]["flushNotify"]
    result = analyze([missing_stage])
    assert not result["contractClosed"] and result["malformedCount"] == 1

    broken_stage_call_contract = event()
    broken_stage_call_contract["periodicDispatch"]["stageTimings"][
        "flushNotify"
    ] = timing(1, 10)
    broken_stage_call_contract["periodicDispatch"]["closures"][
        "pairedTiming"
    ] = False
    broken_stage_call_contract["periodicDispatch"]["closures"][
        "overall"
    ] = False
    broken_stage_call_contract["pairTopologyComparable"] = False
    broken_stage_call_contract["pairComparable"] = False
    result = analyze([broken_stage_call_contract])
    assert not result["contractClosed"] and result["pairComparableCount"] == 0

    bad_tracking = event()
    bad_tracking["trackingDecisionTicks"] += 1
    bad_tracking["trackingPartitionClean"] = False
    result = analyze([bad_tracking])
    assert not result["contractClosed"]

    for fault_name in ("duplicatePublish", "lostPublish", "slotMismatch"):
        faults = {"duplicatePublish": 0, "lostPublish": 0, "slotMismatch": 0}
        faults[fault_name] = 1
        result = analyze([event()], faults=faults)
        assert not result["contractClosed"]
        assert result["pairComparableCount"] == 0
        assert result["pairedCaptureFaults"][fault_name] == 1

    non_boolean = event()
    non_boolean["pairComparable"] = 1
    result = analyze([non_boolean])
    assert not result["contractClosed"] and result["malformedCount"] == 1

    boolean_timing = event()
    boolean_timing["periodicDispatch"]["stageTimings"]["flushRoot"][
        "calls"
    ] = True
    result = analyze([boolean_timing])
    assert not result["contractClosed"] and result["malformedCount"] == 1


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--order",
        choices=tuple(CASE_ORDERS),
        default="disabled-bypass",
        help=(
            "Execution order. Use bypass-disabled to reverse a prior A/B run, "
            "or abba for an order-balanced four-case attribution run."
        ),
    )
    parser.add_argument(
        "--execution-route",
        choices=GPU_SKIN_EXECUTION_ROUTE_CHOICES,
        default="vertex_shader_bypass",
        help=(
            "显式指定 bypass 组的蒙皮路线；disabled 组固定使用 compute 控制。"
        ),
    )
    return parser.parse_args(argv)


def aggregate_metrics_by_mode(cases: List[Dict[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for mode in ("disabled", "bypass"):
        rows = [case for case in cases if case.get("mode") == mode]
        aggregate: Dict[str, Any] = {"runCount": len(rows)}
        for field in METRIC_FIELDS:
            values: List[float] = []
            for row in rows:
                try:
                    values.append(float(row["metrics"][field]))
                except (KeyError, TypeError, ValueError):
                    pass
            aggregate[field] = statistics.fmean(values) if values else None
        result[mode] = aggregate
    return result


def _draw_chain_timing_rows(
    analysis: Dict[str, Any]
) -> Dict[str, Dict[str, Any]]:
    if not analysis.get("available"):
        return {}
    rows = {
        "LeafHostRoot": dict(analysis.get("root", {}) or {}),
        **{
            name: dict(row or {})
            for name, row in dict(analysis.get("stages", {}) or {}).items()
        },
        "HostOther": dict(analysis.get("hostOther", {}) or {}),
    }
    return rows


def aggregate_draw_chain_by_mode(cases: List[Dict[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    row_names = (
        "LeafHostRoot",
        *DRAW_CHAIN_DIRECT_STAGES,
        "HostOther",
    )
    for mode in ("disabled", "bypass"):
        mode_cases = [case for case in cases if case.get("mode") == mode]
        analyses = [
            dict(case.get("drawChainTiming", {}) or {})
            for case in mode_cases
        ]
        valid = [
            analysis for analysis in analyses
            if analysis.get("available") and analysis.get("contractClosed")
        ]
        aggregate: Dict[str, Any] = {
            "runCount": len(mode_cases),
            "validRunCount": len(valid),
            "contractClosed": bool(mode_cases) and len(valid) == len(mode_cases),
            "sections": {},
        }
        for row_name in row_names:
            source_rows = [
                _draw_chain_timing_rows(analysis).get(row_name, {})
                for analysis in valid
            ]
            row: Dict[str, Any] = {}
            for metric in ("calls", "totalCpuMs", "avgUsPerCall"):
                values = [
                    float(source[metric]) for source in source_rows
                    if source.get(metric) is not None
                ]
                row[metric] = statistics.fmean(values) if values else None
            aggregate["sections"][row_name] = row
        result[mode] = aggregate
    return result


def compare_draw_chain_mode_means(
    means: Dict[str, Any]
) -> Dict[str, Any]:
    disabled = dict(means.get("disabled", {}) or {})
    bypass = dict(means.get("bypass", {}) or {})
    if not disabled.get("contractClosed") or not bypass.get("contractClosed"):
        return {
            "available": False,
            "error": "closed draw-chain timing required for both modes",
        }
    disabled_sections = dict(disabled.get("sections", {}) or {})
    bypass_sections = dict(bypass.get("sections", {}) or {})
    deltas: Dict[str, Any] = {}
    for row_name in (
        "LeafHostRoot",
        *DRAW_CHAIN_DIRECT_STAGES,
        "HostOther",
    ):
        a = dict(disabled_sections.get(row_name, {}) or {})
        b = dict(bypass_sections.get(row_name, {}) or {})
        row: Dict[str, Any] = {}
        for metric in ("calls", "totalCpuMs", "avgUsPerCall"):
            row[metric] = (
                float(b[metric]) - float(a[metric])
                if a.get(metric) is not None and b.get(metric) is not None
                else None
            )
        deltas[row_name] = row
    return {
        "available": True,
        "direction": "bypass-minus-disabled",
        "sections": deltas,
    }


def _run_outside_poison_policy_self_test() -> None:
    sidecar_none_raw = {
        "policy": "none",
        "policyValue": 0,
        "explicit": True,
        "invalid": False,
        "counterPolicyValue": 0,
        "counterExplicit": True,
        "counterInvalid": False,
        "counterClosureClean": True,
        "configCounterExact": True,
    }
    sidecar_none = _runtime_outside_poison_sidecar_policy(sidecar_none_raw)
    sidecar_both_raw = dict(sidecar_none_raw)
    sidecar_both_raw.update({
        "policy": "both",
        "policyValue": 3,
        "counterPolicyValue": 3,
    })
    sidecar_both = _runtime_outside_poison_sidecar_policy(sidecar_both_raw)
    sidecar_counter_mismatch_raw = dict(sidecar_none_raw)
    sidecar_counter_mismatch_raw.update({
        "counterPolicyValue": 1,
        "configCounterExact": False,
    })
    sidecar_counter_mismatch = _runtime_outside_poison_sidecar_policy(
        sidecar_counter_mismatch_raw
    )
    sidecar_missing_rejected = False
    try:
        _runtime_outside_poison_sidecar_policy(None)
    except ValueError:
        sidecar_missing_rejected = True
    product = _outside_poison_accepted_policy(True, 377508, 2967)
    product_underflow = _outside_poison_accepted_policy(True, 2, 3)
    product_no_authority = _outside_poison_accepted_policy(True, 3, 3)
    non_product_zero = _outside_poison_accepted_policy(False, 0, 0)
    non_product_mismatch = _outside_poison_accepted_policy(False, 1, 0)
    checks = {
        "runtimeSidecarNoneExact": sidecar_none["exact"] is True,
        "runtimeSidecarBothRejected": sidecar_both["exact"] is False,
        "runtimeSidecarCounterMismatchRejected": (
            sidecar_counter_mismatch["exact"] is False
        ),
        "runtimeSidecarMissingRejected": sidecar_missing_rejected,
        "productExact": product["exact"] is True,
        "productDerivedExact": (
            product["derivedUnscannedAcceptedWithPoison"] == 374541
        ),
        "productIsNotAuthorityEvidence": (
            product["independentAuthorityVerified"] is False
        ),
        "productUnderflowRejected": product_underflow["exact"] is False,
        "productZeroAuthorityRejected": (
            product_no_authority["exact"] is False
        ),
        "nonProductZeroExact": non_product_zero["exact"] is True,
        "nonProductMismatchRejected": (
            non_product_mismatch["exact"] is False
        ),
    }
    if not all(checks.values()):
        raise AssertionError(
            "outside poison policy synthetic self-test failed: "
            + json.dumps(checks, sort_keys=True)
        )


def _run_draw_chain_parser_self_test() -> None:
    def synthetic(root_ms: float, gpu_skin_ms: float) -> Dict[str, Any]:
        root_calls = 100

        def section(
            name: str,
            total_ms: float,
            calls: int,
            parent: str = DRAW_CHAIN_ROOT_PATH,
        ) -> Dict[str, Any]:
            path = (
                DRAW_CHAIN_ROOT_PATH
                if name == "LeafHostRoot"
                else f"{parent}/{name}"
            )
            return {
                "name": name,
                "path": path,
                "parentPath": "" if name == "LeafHostRoot" else parent,
                "calls": calls,
                "totalCpuMs": total_ms,
            }

        rows = [
            section("LeafHostRoot", root_ms, root_calls),
            section("BeforeUi", 1.0, root_calls),
            section("UploadPerDrawData", 2.0, root_calls),
            section("GpuSkinDip", gpu_skin_ms, root_calls),
            section("ShadowCapture", 2.0, 80),
            section("PrepareMainNormal", 2.0, 70),
            section("PrepareMainRestoreRebind", 1.0, 20),
            section("HostEnqueueMain", 1.0, 85),
            # This deliberately huge nested row must not be added to the
            # direct-child partition a second time.
            section(
                "NestedIgnored",
                100.0,
                1,
                f"{DRAW_CHAIN_ROOT_PATH}/BeforeUi",
            ),
        ]
        return {"frameCount": 10, "sections": rows}

    disabled = analyze_draw_chain_sections(synthetic(15.0, 2.0))
    bypass = analyze_draw_chain_sections(synthetic(19.0, 4.0))
    assert disabled["contractClosed"]
    assert bypass["contractClosed"]
    assert disabled["root"]["calls"] == 100
    assert disabled["ignoredNestedSectionCount"] == 1
    assert disabled["directChildTotalCpuMs"] == 11.0
    assert disabled["hostOther"]["totalCpuMs"] == 4.0
    assert not disabled["stages"]["PrepareOutline"]["present"]
    means = aggregate_draw_chain_by_mode(
        [
            {"mode": "disabled", "drawChainTiming": disabled},
            {"mode": "bypass", "drawChainTiming": bypass},
        ]
    )
    comparison = compare_draw_chain_mode_means(means)
    assert comparison["available"]
    assert comparison["sections"]["LeafHostRoot"]["totalCpuMs"] == 4.0
    assert comparison["sections"]["LeafHostRoot"]["avgUsPerCall"] == 40.0
    assert comparison["sections"]["GpuSkinDip"]["avgUsPerCall"] == 20.0


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    case_order = CASE_ORDERS[args.order]
    if not MAP_PATH.is_file():
        raise SystemExit(f"map missing: {MAP_PATH}")
    if not DEPLOYED_DLL.is_file():
        raise SystemExit(f"deployed DLL missing: {DEPLOYED_DLL}")
    if not WAR3_EXE.is_file() or not GAME_DLL.is_file():
        raise SystemExit("war3.exe or Game.dll is missing")
    dll_hash = sha256(DEPLOYED_DLL)
    if dll_hash != EXPECTED_DLL_SHA256:
        raise SystemExit(
            f"deployed DLL hash mismatch: expected={EXPECTED_DLL_SHA256} actual={dll_hash}"
        )
    expected_module_hashes = {
        "war3.exe": sha256(WAR3_EXE),
        "game.dll": sha256(GAME_DLL),
        "d3d9.dll": dll_hash,
    }

    controlled_dxvk_keys = {
        "DXVK_WAR3_GPU_SKIN_MODE",
        GPU_SKIN_EXECUTION_ROUTE_ENV,
        "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS",
        "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES",
        GPU_SKIN_POISON_SIDECAR_ENV,
        DRAW_CHAIN_TIMING_ENV,
        S1_TERRAIN_CAPTURE_PERIOD_ENV,
        "DXVK_WAR3_RESOURCE_CENSUS",
        "DXVK_WAR3_PERF_HISTORY_FRAMES",
        "DXVK_WAR3_RUNTIME_BENCHMARK",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC",
        "DXVK_WAR3_PERF_RECORD_AFTER_GAME_START",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC",
    }
    unexpected_inherited = sorted(
        str(key) for key in os.environ
        if str(key).upper().startswith("DXVK_WAR3_")
        and (
            str(key) != str(key).upper()
            or str(key).upper() not in controlled_dxvk_keys
        )
    )
    if unexpected_inherited:
        raise SystemExit(
            "uncontrolled inherited DXVK_WAR3_* environment; refusing ABBA: "
            + json.dumps(unexpected_inherited, ensure_ascii=False)
        )

    preexisting_rows = process_rows()
    if preexisting_rows:
        raise SystemExit(
            "pre-existing War3 process detected; refusing concurrent ABBA: "
            + json.dumps(preexisting_rows, ensure_ascii=False)
        )
    preexisting_pids = {
        int(row.get("pid", 0) or 0)
        for row in preexisting_rows
        if int(row.get("pid", 0) or 0) > 0
    }
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = ARTIFACTS / f"gpu_skin_perf_isolated_ab_{stamp}"
    out_dir.mkdir(parents=True, exist_ok=False)
    common = {
        "artifact": str(out_dir),
        "startedAt": datetime.now().isoformat(),
        "attributionOnly": True,
        "formalFpsJudgement": False,
        "useIsolatedDesktop": True,
        "buildPerformed": False,
        "deployPerformed": False,
        "war3Dir": str(WAR3_DIR),
        "map": str(MAP_PATH),
        "mapSha256": sha256(MAP_PATH),
        "deployedDll": str(DEPLOYED_DLL),
        "deployedDllSha256": dll_hash,
        "expectedModuleSha256": expected_module_hashes,
        "sampleDurationSec": SAMPLE_DURATION_SEC,
        "caseOrder": args.order,
        "caseSequence": [
            {
                "name": name,
                "mode": mode,
                "executionRoute": (
                    "compute" if mode == "disabled" else args.execution_route
                ),
            }
            for name, mode in case_order
        ],
        "executionRoute": args.execution_route,
        "preexistingWar3": preexisting_rows,
        "fixedEnvironment": {
            "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS": "light",
            "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES": "0",
            GPU_SKIN_EXECUTION_ROUTE_ENV: args.execution_route,
            GPU_SKIN_POISON_SIDECAR_ENV:
                GPU_SKIN_POISON_SIDECAR_VALUE,
            DRAW_CHAIN_TIMING_ENV: "1",
            S1_TERRAIN_CAPTURE_PERIOD_ENV:
                S1_TERRAIN_CAPTURE_PERIOD_VALUE,
            "DXVK_WAR3_RESOURCE_CENSUS": "0",
            "DXVK_WAR3_PERF_HISTORY_FRAMES": "7200",
        },
        "drawChainTimingContract": {
            "hostOnly": True,
            "environmentGate": DRAW_CHAIN_TIMING_ENV,
            "environmentValue": "1",
            "period": DRAW_CHAIN_TIMING_PERIOD,
            "phase": DRAW_CHAIN_TIMING_PHASE,
            "phaseHex": f"0x{DRAW_CHAIN_TIMING_PHASE:02X}",
            "rootPath": DRAW_CHAIN_ROOT_PATH,
            "directStages": list(DRAW_CHAIN_DIRECT_STAGES),
        },
        # S1 must remain period 1 for every GPU-skin comparison. Record the
        # exact value in the artifact instead of relying on an inherited
        # parent-shell variable that the per-case environment omits.
        "s1PeriodOverride": int(S1_TERRAIN_CAPTURE_PERIOD_VALUE),
        "outsidePoisonAcceptanceContract": {
            "sidecarPolicy": GPU_SKIN_POISON_SIDECAR_VALUE,
            "managerVisibleOnly": True,
            "independentAuthorityVerifiedByRunner": False,
            "independentAuthorityHardGate": (
                "P4 forced nativePoisonO1Authority ledger"
            ),
        },
    }
    json_write(out_dir / "manifest.json", common)

    cases: List[Dict[str, Any]] = []
    for case_name, mode in case_order:
        case_dir = out_dir / case_name
        case_dir.mkdir()
        offsets = snapshot_log_offsets()
        module_evidence: Dict[str, Any] = {}
        monitor_stop = threading.Event()
        monitor = threading.Thread(
            target=monitor_modules,
            args=(monitor_stop, module_evidence),
            name=f"module-monitor-{case_name}",
            daemon=True,
        )
        monitor.start()
        env = {
            "DXVK_WAR3_GPU_SKIN_MODE": mode,
            "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS": "light",
            "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES": "0",
            # disabled 是同一份 DLL 的 Compute 控制；bypass 才启用指定 VS 路线。
            GPU_SKIN_EXECUTION_ROUTE_ENV: (
                "compute" if mode == "disabled" else args.execution_route
            ),
            GPU_SKIN_POISON_SIDECAR_ENV:
                GPU_SKIN_POISON_SIDECAR_VALUE,
            DRAW_CHAIN_TIMING_ENV: "1",
            S1_TERRAIN_CAPTURE_PERIOD_ENV:
                S1_TERRAIN_CAPTURE_PERIOD_VALUE,
            "DXVK_WAR3_RESOURCE_CENSUS": "0",
            "DXVK_WAR3_PERF_HISTORY_FRAMES": "7200",
        }
        reports_before = report_snapshot()
        started = time.time()
        result: Dict[str, Any]
        pid = 0
        state_witness_snapshot: Dict[str, Any] = {}
        state_witness_object: Any = None
        try:
            result = autotest.run_quick_autotest(
                war3_dir=str(WAR3_DIR),
                map_path=str(MAP_PATH),
                ready_timeout_sec=120,
                sample_duration_sec=SAMPLE_DURATION_SEC,
                windowed=True,
                use_isolated_desktop=True,
                desktop_name=f"War3GpuSkinPerfAB_{mode}_{stamp}",
                auto_perf_record=True,
                record_after_game_started=True,
                auto_perf_export_sec=SAMPLE_DURATION_SEC + 2,
                deploy_d3d9_before_launch=False,
                enforce_video_baseline=False,
                include_sections_in_report=True,
                section_top_n=256,
                avoid_focus_on_stop=True,
                env_overrides_json=json.dumps(env),
                require_control_plane_ready=True,
            )
            pid = int(dict(result.get("launch", {}) or {}).get("pid", 0) or 0)
        except Exception as exc:
            result = {"ok": False, "stage": "runner-exception", "error": repr(exc)}
            pid = int(getattr(autotest.STATE, "war3_pid", 0) or 0)
        finally:
            state_witness = getattr(
                autotest.STATE, "retained_native_process", None
            )
            if state_witness is not None:
                state_witness_object = state_witness
                try:
                    state_witness_snapshot = dict(
                        state_witness.snapshot() or {}
                    )
                except Exception:
                    state_witness_snapshot = {}
            monitor_stop.set()
            monitor.join(timeout=16.0)

        if pid <= 0:
            pid = int(state_witness_snapshot.get("pid", 0) or 0)
        if pid <= 0:
            pid = int(module_evidence.get("pid", 0) or 0)
        emergency_state_stop: Dict[str, Any] = {
            "ok": True,
            "skipped": True,
            "reason": "run_quick_autotest settled retained native witness",
        }
        if (
            state_witness_object is not None
            and getattr(
                autotest.STATE, "retained_native_process", None
            ) is state_witness_object
        ):
            try:
                emergency_state_stop = dict(autotest.stop_war3(
                    pid=pid,
                    graceful_wait_sec=0,
                    force=True,
                    avoid_foreground_switch=True,
                ) or {})
            except Exception as exc:
                emergency_state_stop = {
                    "ok": False,
                    "stopped": False,
                    "error": repr(exc),
                    "pidTerminationCommandIssued": False,
                }

        source_report = report_path(result)
        report_new = source_report_is_new(
            source_report, reports_before, result,
        )
        copied_report = None
        if report_new and source_report is not None:
            copied_report = case_dir / source_report.name
            shutil.copy2(source_report, copied_report)
        periodic_analysis = periodic_analysis_from_report(copied_report)
        periodic_dispatch_attribution = (
            periodic_dispatch_attribution_from_report(copied_report)
        )
        identity_dedup = identity_dedup_from_report(copied_report)
        gpu_skin_event_graph = gpu_skin_event_graph_from_report(copied_report)
        draw_chain_timing = draw_chain_timing_from_report(copied_report)
        resource_census_contract = resource_census_contract_from_report(
            copied_report
        )
        requested_mode_contract = bool(
            gpu_skin_event_graph.get("available")
            and gpu_skin_event_graph.get("mode") == mode
            and gpu_skin_event_graph.get("fullDiagnostics") is False
        )

        launch = dict(result.get("launch", {}) or {})
        launch_witness = dict(launch.get("nativeProcessWitness", {}) or {})
        if not launch_witness and state_witness_snapshot:
            launch_witness = state_witness_snapshot
        cleanup = exact_cleanup(
            pid, preexisting_pids, launch_witness,
        )
        modules = exact_module_contract(
            module_evidence, pid, expected_module_hashes,
        )
        launch_provenance = case_launch_contract(
            launch, env, common["mapSha256"], pid,
        )
        report_identity = report_process_identity_from_report(copied_report)
        report_identity_contract = case_report_identity_contract(
            report_identity, launch, module_evidence, pid,
        )
        source_report_hash = (
            sha256(source_report)
            if report_new and source_report is not None else None
        )
        copied_report_hash = (
            sha256(copied_report) if copied_report is not None else None
        )
        report_copy_exact = bool(
            source_report_hash
            and copied_report_hash
            and source_report_hash == copied_report_hash
        )
        conductor_stop = dict(result.get("stop", {}) or {})
        conductor_contract = bool(
            result.get("stage") == "done"
            and conductor_stop.get("ok") is True
            and conductor_stop.get("stopped") is True
            and conductor_stop.get("exactNativeHandleStop") is True
            and conductor_stop.get("pidTerminationCommandIssued") is False
        )
        emergency_stop_contract = bool(
            emergency_state_stop.get("skipped") is True
            or (
                emergency_state_stop.get("ok") is True
                and emergency_state_stop.get("stopped") is True
                and emergency_state_stop.get("exactNativeHandleStop") is True
                and emergency_state_stop.get(
                    "pidTerminationCommandIssued"
                ) is False
            )
        )
        cleanup_contract = bool(
            len(cleanup) == 2
            and launch_provenance.get("closed") is True
            and emergency_stop_contract
            and all(
                not list(row.get("remainingNewWar3", []) or [])
                for row in cleanup
            )
            and all(
                list(row.get("targets", []) or []) in ([], [pid])
                for row in cleanup
            )
            and all(
                not bool(row.get("binding", {}).get("currentProcessPresent"))
                or bool(row.get("binding", {}).get("exact"))
                for row in cleanup
            )
        )
        case_record = {
            "name": case_name,
            "mode": mode,
            "environment": env,
            "startedAtEpoch": started,
            "endedAtEpoch": time.time(),
            "wallSec": time.time() - started,
            "pid": pid,
            "ok": bool(
                result.get("ok")
                and draw_chain_timing.get("available")
                and draw_chain_timing.get("contractClosed")
                and gpu_skin_event_graph.get("available")
                and gpu_skin_event_graph.get("contractClosed")
                and requested_mode_contract
                and resource_census_contract.get("closed")
                and report_new
                and modules.get("closed")
                and launch_provenance.get("closed")
                and report_identity_contract.get("closed")
                and report_copy_exact
                and conductor_contract
                and cleanup_contract
            ),
            "stage": result.get("stage"),
            "metrics": metric_row(result),
            "moduleEvidence": module_evidence,
            "moduleContract": modules,
            "launchContract": launch_provenance,
            "stateNativeProcessWitness": state_witness_snapshot,
            "emergencyStateExactStop": emergency_state_stop,
            "emergencyStateExactStopContract": emergency_stop_contract,
            "reportProcessIdentityContract": report_identity_contract,
            "sourceReport": str(source_report) if source_report else None,
            "sourceReportSha256": source_report_hash,
            "sourceReportWasNew": report_new,
            "copiedReport": str(copied_report) if copied_report else None,
            "copiedReportSha256": copied_report_hash,
            "reportCopyExact": report_copy_exact,
            "conductorStopContract": {
                "stageDone": result.get("stage") == "done",
                "stop": conductor_stop,
                "closed": conductor_contract,
            },
            "periodic300FrameAnalysis": periodic_analysis,
            "periodicDispatchAttribution": periodic_dispatch_attribution,
            "identitySameFrameDedup": identity_dedup,
            "gpuSkinProductionEventGraph": gpu_skin_event_graph,
            "requestedModeContract": {
                "requestedMode": mode,
                "requestedExecutionRoute": env[GPU_SKIN_EXECUTION_ROUTE_ENV],
                "reportedMode": gpu_skin_event_graph.get("mode"),
                "reportedFullDiagnostics": gpu_skin_event_graph.get(
                    "fullDiagnostics"
                ),
                "closed": requested_mode_contract,
            },
            "drawChainTiming": draw_chain_timing,
            "drawChainTimingContract": common["drawChainTimingContract"],
            "resourceCensusContract": resource_census_contract,
            "logDeltas": save_log_deltas(case_dir, offsets),
            "cleanup": cleanup,
            "cleanupContract": {
                "twoPasses": len(cleanup) == 2,
                "exactLaunchProcessInstanceOnly": True,
                "unrelatedWar3NeverTargeted": True,
                "pythonNodeIdaNeverTargeted": True,
                "noNewWar3AfterEachPass": all(
                    not list(row.get("remainingNewWar3", []) or [])
                    for row in cleanup
                ),
                "closed": cleanup_contract,
            },
            "resultArtifact": str(case_dir / "result.json"),
        }
        json_write(case_dir / "raw_result.json", result)
        json_write(case_dir / "result.json", case_record)
        cases.append(case_record)
        print(
            f"{case_name}: ok={case_record['ok']} pid={pid} "
            f"fps={case_record['metrics'].get('avgFps')} "
            f"frame={case_record['metrics'].get('avgFrameTimeMs')} "
            f"drawRootUs={draw_chain_timing.get('root', {}).get('avgUsPerCall')} "
            f"report={source_report}",
            flush=True,
        )
        if not case_record["ok"]:
            break

    aggregates = aggregate_metrics_by_mode(cases)
    draw_chain_means = aggregate_draw_chain_by_mode(cases)
    deltas: Dict[str, Any] = {}
    disabled = aggregates["disabled"]
    bypass = aggregates["bypass"]
    for name in METRIC_FIELDS:
        if disabled.get(name) is not None and bypass.get(name) is not None:
            deltas[f"bypassMinusDisabled.{name}"] = (
                float(bypass[name]) - float(disabled[name])
            )
        else:
            deltas[f"bypassMinusDisabled.{name}"] = None

    final_new_war3 = [
        row
        for row in process_rows()
        if int(row.get("pid", 0) or 0) not in preexisting_pids
    ]
    summary = {
        **common,
        "endedAt": datetime.now().isoformat(),
        "cases": cases,
        "modeMetricMeans": aggregates,
        "modeDrawChainMeans": draw_chain_means,
        "drawChainBypassMinusDisabled": compare_draw_chain_mode_means(
            draw_chain_means
        ),
        "deltas": deltas,
        "periodic300FrameComparison": compare_periodic_analyses(cases),
        "isolatedDesktopWarning": (
            "Attribution-only: isolated/windowed frame cadence is not a formal FPS gate."
        ),
        "finalNewWar3": final_new_war3,
        "passRequiresFinalNewWar3Empty": True,
    }
    json_write(out_dir / "summary.json", summary)
    print(out_dir, flush=True)
    return 0 if (
        len(cases) == len(case_order)
        and all(row["ok"] for row in cases)
        and not final_new_war3
    ) else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test-outside-poison-policy"]:
        _run_outside_poison_policy_self_test()
        print("outside poison policy synthetic self-test PASS", flush=True)
        raise SystemExit(0)
    if sys.argv[1:] == ["--self-test-periodic-analysis"]:
        _run_periodic_analysis_self_test()
        print("periodic frame analysis synthetic self-test PASS", flush=True)
        raise SystemExit(0)
    if sys.argv[1:] == ["--self-test-periodic-dispatch-parser"]:
        _run_periodic_dispatch_parser_self_test()
        print("periodic dispatch parser synthetic self-test PASS", flush=True)
        raise SystemExit(0)
    if sys.argv[1:] == ["--self-test-draw-chain-parser"]:
        _run_draw_chain_parser_self_test()
        print("draw-chain parser synthetic self-test PASS", flush=True)
        raise SystemExit(0)
    raise SystemExit(main(sys.argv[1:]))
