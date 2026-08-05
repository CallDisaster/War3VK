#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""运行一轮仅用于诊断的 War3 重图资源驻留普查。

本运行器绝不构建或部署。它首先证明现有 x86 构建产物与已部署 DLL 逐字节一致，
随后在隔离桌面运行用户指定地图；默认且正式的驻留证据地图为“生与死”。清理范围
刻意限制为本运行器启动的精确
War3 PID；绝不会终止无关的 Python、Node、IDA 或测试前已存在的 War3 进程。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from typing import Any


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import run_gpu_skin_perf_isolated_ab as gpu_skin_ab  # noqa: E402
import war3_autotest_mcp as autotest  # noqa: E402


WAR3_DIR = Path(r"E:\Work\War3")
DEFAULT_MAP_PATH = (
    WAR3_DIR / "Maps" / "(4)生与死v1.28读档bug修复.w3x"
)
BUILD_DLL = HERE.parent / "build32" / "src" / "d3d9" / "d3d9.dll"
DEPLOYED_DLL = WAR3_DIR / "d3d9.dll"
WAR3_EXE = WAR3_DIR / "war3.exe"
GAME_DLL = WAR3_DIR / "Game.dll"
REPORT_DIR = WAR3_DIR / "WarVK" / "Log"
ARTIFACTS = HERE / "artifacts"
ANALYZER = HERE / "analyze_resource_residency_census.py"
SAMPLE_DURATION_SEC = 45

FIXED_ENVIRONMENT = {
    "DXVK_WAR3_GPU_SKIN_MODE": "bypass",
    "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS": "light",
    "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES": "0",
    "DXVK_WAR3_GPU_SKIN_POISON_SIDECAR": "none",
    "DXVK_WAR3_GPU_SKIN_DRAW_CHAIN_TIMING": "0",
    "DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "1",
    "DXVK_WAR3_RESOURCE_CENSUS": "1",
    "DXVK_WAR3_PERF_HISTORY_FRAMES": "7200",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def json_write(path: Path, value: Any) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, default=str) + "\n",
        encoding="utf-8",
    )


def war3_process_rows() -> list[dict[str, Any]]:
    """只返回 War3 进程记录；绝不枚举或终止辅助运行时。"""
    rows: list[dict[str, Any]] = []
    for raw in autotest._snapshot_process_entries():
        name = str(raw.get("exeName", raw.get("name", "")) or "")
        if name.casefold() != "war3.exe":
            continue
        pid = int(raw.get("pid", 0) or 0)
        image_path = ""
        if pid > 0:
            try:
                image_path = str(autotest._query_process_image_path(pid) or "")
            except Exception:
                image_path = ""
        rows.append(
            {
                "pid": pid,
                "parentPid": int(raw.get("parentPid", 0) or 0),
                "exeName": name,
                "imagePath": image_path,
            }
        )
    return rows


def report_snapshot() -> dict[str, dict[str, int]]:
    result: dict[str, dict[str, int]] = {}
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


def report_path(result: dict[str, Any]) -> Path | None:
    report = dict(result.get("report", {}) or {})
    for raw in (
        report.get("latestReportPath"),
        report.get("reportPath"),
        result.get("reportPath"),
    ):
        if raw and Path(str(raw)).is_file():
            return Path(str(raw))
    return None


def source_report_is_new(
    source: Path | None,
    before: dict[str, dict[str, int]],
    result: dict[str, Any],
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


def module_contract(
    evidence: dict[str, Any], pid: int, expected_hashes: dict[str, str]
) -> dict[str, Any]:
    rows = list(evidence.get("modules", []) or [])
    by_name = {
        name: [
            row
            for row in rows
            if str(row.get("Name", "")).casefold() == name
        ]
        for name in ("war3.exe", "game.dll", "d3d9.dll")
    }
    expected_paths = {
        "war3.exe": str(WAR3_EXE.resolve()).casefold(),
        "game.dll": str(GAME_DLL.resolve()).casefold(),
        "d3d9.dll": str(DEPLOYED_DLL.resolve()).casefold(),
    }
    actual_paths: dict[str, str] = {}
    actual_hashes: dict[str, str] = {}
    for name, values in by_name.items():
        actual_paths[name] = (
            str(Path(str(values[0].get("Path", ""))).resolve()).casefold()
            if len(values) == 1 and values[0].get("Path")
            else ""
        )
        actual_hashes[name] = (
            str(values[0].get("sha256", "")).upper()
            if len(values) == 1
            else ""
        )
    closed = bool(
        evidence.get("ok") is True
        and evidence.get("gameDllObserved") is True
        and pid > 0
        and int(evidence.get("pid", 0) or 0) == pid
        and all(len(value) == 1 for value in by_name.values())
        and all(actual_paths[name] == expected_paths[name] for name in by_name)
        and all(
            actual_hashes[name] == expected_hashes[name]
            for name in by_name
        )
    )
    return {
        "requiredExactlyOnce": ["war3.exe", "game.dll", "d3d9.dll"],
        "expectedPid": pid,
        "expectedPaths": expected_paths,
        "actualPaths": actual_paths,
        "expectedSha256": expected_hashes,
        "actualSha256": actual_hashes,
        "closed": closed,
    }


def launch_contract(launch: dict[str, Any], map_hash: str) -> dict[str, Any]:
    actual_overrides = dict(launch.get("envOverrides", {}) or {})
    expected_overrides = dict(FIXED_ENVIRONMENT)
    expected_overrides.update({
        "DXVK_WAR3_RUNTIME_BENCHMARK": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_WARMUP_SEC": "1",
        "DXVK_WAR3_RUNTIME_BENCHMARK_SAMPLE_SEC": "44",
    })
    expected_effective = dict(expected_overrides)
    expected_effective.update({
        "DXVK_WAR3_PERF_RECORD_AFTER_GAME_START": "1",
        "DXVK_WAR3_PERF_AUTO_EXPORT_SEC": "47",
    })
    actual_effective = dict(
        launch.get("effectiveWar3Environment", {}) or {}
    )
    checks = {
        "isolatedDesktop": launch.get("useIsolatedDesktop") is True,
        "mapSha256": (
            str(launch.get("sourceMapSha256", "")).upper() == map_hash
        ),
        "overrides": actual_overrides == expected_overrides,
        "effectiveEnvironment": actual_effective == expected_effective,
        "noDeploy": launch.get("deploy") is None,
    }
    return {
        "expectedOverrides": expected_overrides,
        "actualOverrides": actual_overrides,
        "expectedEffectiveEnvironment": expected_effective,
        "actualEffectiveEnvironment": actual_effective,
        "expectedMapSha256": map_hash,
        "actualMapSha256": str(launch.get("sourceMapSha256", "")).upper(),
        "deployEvidence": launch.get("deploy"),
        "checks": checks,
        "closed": all(checks.values()),
    }


def run_analyzer(report: Path, output: Path) -> dict[str, Any]:
    command = [
        sys.executable,
        str(ANALYZER),
        str(report),
        "--output",
        str(output),
    ]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
            check=False,
        )
    except Exception as exc:
        return {
            "command": command,
            "returncode": None,
            "stdout": "",
            "stderr": "",
            "error": repr(exc),
            "ok": False,
        }
    return {
        "command": command,
        "returncode": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "output": str(output),
        "outputSha256": sha256(output) if output.is_file() else None,
        "ok": completed.returncode == 0 and output.is_file(),
    }


def report_process_identity_contract(
    analysis_path: Path,
    launch: dict[str, Any],
    pid: int,
    module_evidence: dict[str, Any],
) -> dict[str, Any]:
    try:
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return {"closed": False, "error": repr(exc)}
    identity = dict(analysis.get("reportProcessIdentity", {}) or {})
    witness = dict(launch.get("nativeProcessWitness", {}) or {})
    report_pid = int(identity.get("processId", 0) or 0)
    report_filetime = int(
        identity.get("processStartFileTime100ns", 0) or 0
    )
    report_filetime_exact = str(
        identity.get("processStartFileTime100nsExact", "") or ""
    )
    witness_pid = int(witness.get("pid", 0) or 0)
    witness_creation_ms = int(witness.get("creationEpochMs", 0) or 0)
    witness_path = str(witness.get("canonicalExePath", "") or "")
    report_creation_ms = (
        (report_filetime - 116_444_736_000_000_000) // 10_000
        if report_filetime >= 116_444_736_000_000_000
        else 0
    )
    expected_path = str(WAR3_EXE.resolve()).casefold()
    checks = {
        "reportExactFields": bool(
            identity.get("exactFieldsClosed") is True
            and report_filetime_exact == str(report_filetime)
        ),
        "pid": bool(
            pid > 0
            and report_pid == pid
            and witness_pid == pid
            and int(module_evidence.get("pid", 0) or 0) == pid
        ),
        "creationTime": bool(
            witness_creation_ms > 0
            and report_creation_ms == witness_creation_ms
        ),
        "launchWitness": bool(
            witness.get("available") is True
            and witness.get("ownsNativeHandle") is True
            and witness_path.casefold() == expected_path
        ),
    }
    return {
        "reportIdentity": identity,
        "launchNativeProcessWitness": witness,
        "derivedReportCreationEpochMs": report_creation_ms,
        "expectedCanonicalExePath": expected_path,
        "checks": checks,
        "closed": all(checks.values()),
    }


def allocator_chunk_contract(analysis_path: Path) -> dict[str, Any]:
    """读取 analyzer 的独立 chunk 硬门；analyzer 成功本身不等于闭合。"""
    try:
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return {"closed": False, "error": repr(exc)}
    contract = analysis.get("allocatorChunkContract")
    if not isinstance(contract, dict):
        return {
            "closed": False,
            "error": "allocatorChunkContract is missing",
        }
    return dict(contract)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="在隔离桌面运行只读资源驻留普查，默认使用生与死重图。"
    )
    parser.add_argument(
        "--map-path",
        type=Path,
        default=DEFAULT_MAP_PATH,
        help="待普查地图；正式重图证据应保持默认的生与死。",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    map_path = Path(args.map_path).resolve()
    for required in (
        map_path, BUILD_DLL, DEPLOYED_DLL, WAR3_EXE, GAME_DLL, ANALYZER,
    ):
        if not required.is_file():
            raise SystemExit(f"required file missing: {required}")

    build_hash = sha256(BUILD_DLL)
    deployed_hash = sha256(DEPLOYED_DLL)
    module_hashes = {
        "war3.exe": sha256(WAR3_EXE),
        "game.dll": sha256(GAME_DLL),
        "d3d9.dll": deployed_hash,
    }
    if build_hash != deployed_hash:
        raise SystemExit(
            "build/deployed DLL hash mismatch; refusing to launch: "
            f"build={build_hash} deployed={deployed_hash}"
        )

    controlled_dxvk_keys = {
        *FIXED_ENVIRONMENT.keys(),
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
            "uncontrolled inherited DXVK_WAR3_* environment; refusing census: "
            + json.dumps(unexpected_inherited, ensure_ascii=False)
        )

    preexisting_rows = war3_process_rows()
    if preexisting_rows:
        raise SystemExit(
            "pre-existing War3 process detected; refusing concurrent test: "
            + json.dumps(preexisting_rows, ensure_ascii=False)
        )
    preexisting_pids = {
        int(row.get("pid", 0) or 0)
        for row in preexisting_rows
        if int(row.get("pid", 0) or 0) > 0
    }

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = (
        ARTIFACTS
        / f"resource_residency_census_isolated_life_and_death_{stamp}"
    )
    out_dir.mkdir(parents=True, exist_ok=False)
    reports_before = report_snapshot()
    map_hash = sha256(map_path)
    manifest = {
        "artifact": str(out_dir),
        "startedAt": datetime.now().isoformat(),
        "contract": "diagnostics-only-resource-census-runner-v2",
        "diagnosticsOnly": True,
        "evictionAuthority": False,
        "performanceComparable": False,
        "buildPerformed": False,
        "deployPerformed": False,
        "useIsolatedDesktop": True,
        "sampleDurationSec": SAMPLE_DURATION_SEC,
        "map": str(map_path),
        "mapSha256": map_hash,
        "buildDll": str(BUILD_DLL.resolve()),
        "buildDllSha256": build_hash,
        "deployedDll": str(DEPLOYED_DLL.resolve()),
        "deployedDllSha256": deployed_hash,
        "expectedModuleSha256": module_hashes,
        "buildDeployedExact": build_hash == deployed_hash,
        "fixedEnvironment": dict(FIXED_ENVIRONMENT),
        "preexistingWar3": preexisting_rows,
        "cleanupScope": (
            "only the exact War3 PID launched by this runner; never Python, "
            "Node, IDA, or unrelated processes"
        ),
    }
    json_write(out_dir / "manifest.json", manifest)

    result: dict[str, Any] = {}
    module_evidence: dict[str, Any] = {}
    state_witness_snapshot: dict[str, Any] = {}
    state_witness_object: Any = None
    owned_pids: set[int] = set()
    monitor_stop = threading.Event()
    monitor = threading.Thread(
        target=gpu_skin_ab.monitor_modules,
        args=(monitor_stop, module_evidence),
        name=f"resource-census-module-monitor-{stamp}",
        daemon=True,
    )
    monitor.start()
    started = time.time()
    try:
        result = autotest.run_quick_autotest(
            war3_dir=str(WAR3_DIR),
            map_path=str(map_path),
            ready_timeout_sec=120,
            sample_duration_sec=SAMPLE_DURATION_SEC,
            windowed=True,
            use_isolated_desktop=True,
            desktop_name=f"War3ResourceCensus_{stamp}",
            auto_perf_record=True,
            record_after_game_started=True,
            auto_perf_export_sec=SAMPLE_DURATION_SEC + 2,
            deploy_d3d9_before_launch=False,
            enforce_video_baseline=False,
            include_sections_in_report=False,
            avoid_focus_on_stop=True,
            env_overrides_json=json.dumps(FIXED_ENVIRONMENT),
            require_control_plane_ready=True,
        )
        pid = int(dict(result.get("launch", {}) or {}).get("pid", 0) or 0)
        if pid > 0:
            owned_pids.add(pid)
    except Exception as exc:
        result = {
            "ok": False,
            "stage": "runner-exception",
            "error": repr(exc),
        }
        state_pid = int(getattr(autotest.STATE, "war3_pid", 0) or 0)
        if state_pid > 0:
            owned_pids.add(state_pid)
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
        evidence_pid = int(module_evidence.get("pid", 0) or 0)
        state_pid = int(getattr(autotest.STATE, "war3_pid", 0) or 0)
        if evidence_pid > 0:
            owned_pids.add(evidence_pid)
        if state_pid > 0:
            owned_pids.add(state_pid)
        monitor_stop.set()
        monitor.join(timeout=16.0)

    evidence_pid = int(module_evidence.get("pid", 0) or 0)
    if evidence_pid > 0:
        owned_pids.add(evidence_pid)
    launch = dict(result.get("launch", {}) or {})
    pid = int(launch.get("pid", 0) or 0)
    if pid <= 0 and len(owned_pids) == 1:
        pid = next(iter(owned_pids))

    emergency_state_stop: dict[str, Any] = {
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

    # 在复制报告或运行分析器之前，先持久化 Conductor 原始结果并结算自有进程。
    # 尤其要保证分析器超时绝不会延长异常遗留的 War3 进程寿命。
    json_write(out_dir / "raw_result.json", result)
    launch_witness = dict(launch.get("nativeProcessWitness", {}) or {})
    if not launch_witness and state_witness_snapshot:
        launch_witness = state_witness_snapshot
    cleanup = gpu_skin_ab.exact_cleanup(
        pid, preexisting_pids, launch_witness,
    )

    source_report = report_path(result)
    report_new = source_report_is_new(source_report, reports_before, result)
    copied_report: Path | None = None
    if report_new and source_report is not None:
        copied_report = out_dir / source_report.name
        shutil.copy2(source_report, copied_report)

    analysis_path = out_dir / "analysis.json"
    analyzer_result = (
        run_analyzer(copied_report, analysis_path)
        if copied_report is not None
        else {
            "ok": False,
            "error": "no fresh perf HTML was available",
            "output": str(analysis_path),
        }
    )
    modules = module_contract(module_evidence, pid, module_hashes)
    launch_provenance = launch_contract(launch, map_hash)
    report_identity = report_process_identity_contract(
        analysis_path, launch, pid, module_evidence,
    ) if analysis_path.is_file() else {
        "closed": False,
        "error": "analysis output unavailable",
    }
    chunk_contract = allocator_chunk_contract(
        analysis_path
    ) if analysis_path.is_file() else {
        "closed": False,
        "error": "analysis output unavailable",
    }
    source_report_hash = (
        sha256(source_report) if source_report is not None else None
    )
    copied_report_hash = (
        sha256(copied_report) if copied_report is not None else None
    )
    report_copy_exact = bool(
        source_report_hash
        and copied_report_hash
        and source_report_hash == copied_report_hash
    )
    # 此处刻意作为最后一次进程快照。不会触碰并发启动的无关 War3，
    # 但其存在会使本轮测试无法通过。
    final_new_war3 = [
        row
        for row in war3_process_rows()
        if int(row.get("pid", 0) or 0) not in preexisting_pids
    ]
    cleanup_closed = bool(
        len(cleanup) == 2
        and all(
            not list(row.get("remainingNewWar3", []) or [])
            for row in cleanup
        )
        and not final_new_war3
    )
    emergency_stop_closed = bool(
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
    report = dict(result.get("report", {}) or {})
    conductor_stop = dict(result.get("stop", {}) or {})
    conductor_stop_closed = bool(
        result.get("stage") == "done"
        and conductor_stop.get("ok") is True
        and conductor_stop.get("stopped") is True
        and conductor_stop.get("exactNativeHandleStop") is True
        and conductor_stop.get("pidTerminationCommandIssued") is False
    )
    passed = bool(
        result.get("ok") is True
        and result.get("stage") == "done"
        and report_new
        and copied_report is not None
        and copied_report.is_file()
        and analyzer_result.get("ok") is True
        and modules.get("closed") is True
        and launch_provenance.get("closed") is True
        and report_identity.get("closed") is True
        and chunk_contract.get("closed") is True
        and report_copy_exact
        and conductor_stop_closed
        and emergency_stop_closed
        and cleanup_closed
        and not final_new_war3
    )
    record = {
        **manifest,
        "endedAt": datetime.now().isoformat(),
        "wallSec": time.time() - started,
        "pid": pid,
        "ownedWar3Pids": sorted(owned_pids),
        "launchNativeProcessWitness": launch.get("nativeProcessWitness"),
        "stateNativeProcessWitness": state_witness_snapshot,
        "emergencyStateExactStop": emergency_state_stop,
        "emergencyStateExactStopContract": emergency_stop_closed,
        "explicitLaunchEnvironment": dict(FIXED_ENVIRONMENT),
        "rawResult": str(out_dir / "raw_result.json"),
        "sourceReport": str(source_report) if source_report else None,
        "sourceReportSha256": source_report_hash,
        "sourceReportWasNew": report_new,
        "sourceReportNewFlag": report.get("newReportDetected"),
        "copiedReport": str(copied_report) if copied_report else None,
        "copiedReportSha256": copied_report_hash,
        "reportCopyExact": report_copy_exact,
        "analysis": str(analysis_path) if analysis_path.is_file() else None,
        "analysisSha256": (
            sha256(analysis_path) if analysis_path.is_file() else None
        ),
        "analyzerProcess": analyzer_result,
        "moduleEvidence": module_evidence,
        "moduleContract": modules,
        "launchContract": launch_provenance,
        "reportProcessIdentityContract": report_identity,
        "allocatorChunkContract": chunk_contract,
        "conductorStopContract": {
            "stageDone": result.get("stage") == "done",
            "stop": conductor_stop,
            "closed": conductor_stop_closed,
        },
        "cleanup": cleanup,
        "cleanupContract": {
            "twoPasses": len(cleanup) == 2,
            "runnerOwnedWar3Only": True,
            "pythonNodeIdaNeverTargeted": True,
            "closed": cleanup_closed,
        },
        "finalNewWar3": final_new_war3,
        "passRequiresFinalNewWar3Empty": True,
        "pass": passed,
    }
    json_write(out_dir / "result.json", record)
    print(out_dir, flush=True)
    print(f"PASS={passed} finalNewWar3={len(final_new_war3)}", flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
