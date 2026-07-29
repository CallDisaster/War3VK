#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P4 native CPU-skin/upload bypass isolated-desktop conductor.

This is deliberately a single-owner test entry point.  It has three explicit
phases: ``build-only``, a short crash gate, and a longer lifecycle gate.  It is
not a performance benchmark; isolated-desktop FPS must not be used for a P4
promotion decision.

Examples (only run after the Test Conductor is explicitly released)::

    py AutoTest\run_gpu_skin_p4_isolated.py --phase build-only
    py AutoTest\run_gpu_skin_p4_isolated.py --phase crash-gate
    py AutoTest\run_gpu_skin_p4_isolated.py --phase lifecycle --duration-sec 300
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import msvcrt
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ARTIFACTS = HERE / "artifacts"
WAR3_DIR = Path(r"E:\Work\War3")
LOW_MAP = r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x"
BUILD_DLL = ROOT / "build32" / "src" / "d3d9" / "d3d9.dll"
DEPLOYED_DLL = WAR3_DIR / "d3d9.dll"
GAME_DLL = WAR3_DIR / "Game.dll"
REPO_SHADER_CONFIG = ROOT / "shader_packs.json"
DEPLOYED_SHADER_CONFIG = WAR3_DIR / "shader_packs.json"
REPO_OUTLINE_SHADER = ROOT / "shaders" / "war3_outline.hlsl"
DEPLOYED_OUTLINE_SHADER = WAR3_DIR / "shaders" / "war3_outline.hlsl"
WAIT_CHAIN_COLLECTOR = HERE / "collect_process_wait_chains.py"
WAIT_CHAIN_HELPER_TIMEOUT_SEC = 5.0
WAIT_CHAIN_HELPER_MAX_THREADS = 64

P4_ENV = {
    "DXVK_WAR3_GPU_SKIN_MODE": "bypass",
    "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS": "full",
    "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES": "0",
    # This specialized evidence runner explicitly exercises both report-only
    # poison sidecars. Native ordinary-production default remains ``none``.
    "DXVK_WAR3_GPU_SKIN_POISON_SIDECAR": "both",
    # S1 is a semantic control, not a performance knob. Every isolated P4
    # launch records and enforces the project-wide period-1 contract.
    "DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD": "1",
}
GPU_SKIN_POISON_SIDECAR_ENV = "DXVK_WAR3_GPU_SKIN_POISON_SIDECAR"
GPU_SKIN_POISON_SIDECAR_POLICIES = {
    "none": 0,
    "o0": 1,
    "o1": 2,
    "both": 3,
}
GPU_SKIN_EXECUTION_ROUTES = (
    "compute", "vertex_shader", "vertex_shader_input_only",
    "vertex_shader_bypass",
)
GPU_SKIN_EXECUTION_ROUTE_ENV = "DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE"
GPU_SKIN_PALETTE_MATRIX_BYTES = 48
GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK = 3
GPU_SKIN_MAIN_CONSUMER_MASK = 1

_GPU_SKIN_OUTSIDE_ADMISSION_CLASSES = (
    "noPoisonFlush", "noPoisonIndependent",
    "poisonFlush", "poisonIndependent",
    "noPoisonSemantic", "poisonSemantic",
)

_GPU_SKIN_OUTSIDE_REJECT_REASONS = (
    "unknown", "activeFastMarker", "modeNotBypass", "fullDiagnostics",
    "ingressClosed", "bypassDisabled", "wrongThread", "nullDevice",
    "dispatchOwned", "semanticScopeHazard", "dispatchOverflow",
    "semanticOverflow", "nestedUpload", "genericUploadInFlight",
    "fastRejectUploadInFlight", "evidenceUploadInFlight",
    "retirementPending", "retirementQueueFault",
    "resetGenerationMismatch", "evidenceCohort", "poisonReadFailure",
    "poisonOverlap", "poisonPostScanRevalidation",
    "independentPinRevalidation",
)

_GPU_SKIN_DIP_FAST_REJECT_REASONS = (
    "localScope", "ticketGate", "lifecycle", "pendingKernel",
    "resetOrRetirement", "poisonNeedsInput", "poisonCountMismatch",
    "poisonHit",
)

_GPU_SKIN_DIP_FAST_LOCAL_REJECT_REASONS = (
    "noTransactionCover", "cleanSemanticOnly", "dispatchActive",
    "semanticHazard", "uploadOrMarker", "callbackReentry",
    "otherChronology",
)

_GPU_SKIN_DIP_FAST_READER_FIELDS = (
    "begins", "ends", "commits", "rejects", "evidenceFallbacks",
    "mismatches",
)

_GPU_SKIN_DIP_FAST_COVER_FIELDS = (
    "flush", "observer", "reader",
)


def _p4_environment(
    diagnostics: str, sidecar_policy: str = "both",
    execution_route: str = "compute",
) -> Dict[str, str]:
    if diagnostics not in ("light", "full"):
        raise ValueError(f"unsupported GPU-skin diagnostics mode: {diagnostics}")
    if sidecar_policy not in GPU_SKIN_POISON_SIDECAR_POLICIES:
        raise ValueError(
            f"unsupported GPU-skin poison sidecar policy: {sidecar_policy}"
        )
    if execution_route not in GPU_SKIN_EXECUTION_ROUTES:
        raise ValueError(f"unsupported GPU-skin execution route: {execution_route}")
    environment = dict(P4_ENV)
    environment["DXVK_WAR3_GPU_SKIN_DIAGNOSTICS"] = diagnostics
    environment["DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES"] = "0"
    environment[GPU_SKIN_POISON_SIDECAR_ENV] = sidecar_policy
    # Compute 通过“未设置环境变量”验证真实默认值；VS-A 必须显式开启。
    if execution_route != "compute":
        environment[GPU_SKIN_EXECUTION_ROUTE_ENV] = execution_route
    return environment


LOG_CANDIDATES = (
    WAR3_DIR / "war3_d3d9.log",
    WAR3_DIR / "dxvk.log",
    WAR3_DIR / "war3.log",
    WAR3_DIR / "War3.log",
)
POWERSHELL_X86 = (
    Path(os.environ.get("WINDIR", r"C:\Windows"))
    / "SysWOW64" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
)
READY_EVIDENCE_REQUIRED_MODULES = ("war3.exe", "game.dll", "d3d9.dll")
TOOLHELP_ERROR_NO_MORE_FILES = 18
# Windows process StartTime and Application event TimeCreated share the system
# wall clock. No authorization tolerance is needed; any pre-instance event is
# retained only as report-only evidence.
PROCESS_INSTANCE_CLOCK_TOLERANCE_MS = 0
CRASH_RE = re.compile(
    r"(?:unhandled exception|exception_access_violation|access violation|"
    r"fatal error|assertion failed|crash dump|crashed with)",
    re.IGNORECASE,
)
GPU_SKIN_FAILURE_RE = re.compile(
    r"(?:unhandled exception|access violation|fatal|assert|crash|"
    r"\b(?:fault|invalid|ackMismatch|commitBlocked|overflow)\s*[:=]\s*(?!0\b)\d+)",
    re.IGNORECASE,
)
BASE_HARD_GATE_NAMES = (
    "diagnosticsPresent",
    "forcedDiagnosticsSnapshot",
    "forcedDiagnosticsQuiescent",
    "protocolAccountingClosed",
    "managerDispatchAccountingClosed",
    "managerDispatchPolicyClean",
    "dispatchCpuOnlySealContractClean",
    "telemetryBatchingExact",
    "dipFastProbeContractClean",
    "productionSampleTimingExact",
    "computeAccountingClosed",
    "kernelAccountingClosed",
    "formatHistogramClosed",
    "skinHistogramClosed",
    "formatBucketsKnown",
    "nativeInsideUploadRangeValid",
    "strictUploadClassificationClosed",
    "outsideNativeFastPathPolicyClean",
    "outsideNoPoisonDirectOriginalContractClean",
    "nativePoisonSidecarPolicyContractClean",
    "nativePoisonShadowContractClean",
    "nativePoisonO1ShadowContractClean",
    "nativePoisonO1AuthorityContractClean",
    "outsideAdmissionAttributionClean",
    "nativeUploadExactlyOnce",
    "nativeFanoutAccountingClosed",
    "nativeBeginSamplerCadenceClean",
    "resourceAccountingClosed",
    "poisonDiscardAccountingCovered",
    "processAlive",
    "twoScreenshots",
    "bypassedKernelCallsPositive",
    "bypassedKernelBytesPositive",
    "poisonCreateAndHitPositive",
    "poisonHitCreateExact",
    "poisonOverflowZero",
    "poisonResetStaleZero",
    "poisonOutstandingZero",
    "nativeDirectDiscardPathExercised",
    "nativeDirectDiscardAccountingClosed",
    "nativeCrossBackingPoisonMergeZero",
    "indexTicketClean",
    "indexTicketExact",
    "takeoverCountsExact",
    "postSkipClean",
    "p3RestoreClean",
    "bypassMismatchZero",
    "bypassHostAuthorizationMismatchZero",
    "bypassShadowConsumerPositive",
    "p4ShadowFinalClean",
    "ledgerClean",
    "ledgerTerminalClean",
    "ledgerReasonConsistent",
    "latePoisonPartitionsClosed",
    "latePoisonFlagIncidenceConsistent",
    "latePoisonKnownFlagMasks",
    "latePoisonNativePartitionsClosed",
    "latePoisonSampleDumpClosed",
    "latePoisonSampleGeometryConsistent",
    "latePoisonSampleStorageConsistent",
    "latePoisonTotalZero",
    "lifetimeClean",
    "exactTakeoverConflictClean",
    "nativeKernelNormalClean",
    "nativeRetirementClean",
    "nativeResetFaultClean",
    "crashScanClean",
)
DIAGNOSTIC_HARD_GATE_NAMES = {
    "full": ("hotPathTimingContractClean",),
    "light": ("lightDiagnosticsContractClean",),
}
VS_ROUTE_HARD_GATE_NAMES = (
    "vsRouteConfigExact",
    "vsRouteEnvironmentExact",
    "vsRouteInputContract",
    "vsRouteMainContract",
    "vsRouteComputeRetained",
    "vsRouteInputConsumerContract",
    "vsRouteShadowCaptureContract",
    "vsRouteShadowDirectContract",
    "vsRouteShadowPairDeltaContract",
    "vsRouteP4AuthorityContract",
    "vsRouteForcedSnapshotContract",
    "vsRouteLedgerTerminalContract",
)

# VS-B0 只验证“输入直供 + 省略 compute output/job”，故意保留原生 CPU
# skin kernel。下面这些旧 P4 门要求必须出现一次真实 kernel bypass，不能拿来
# 判定 VS-B0；它们由新的零权限合同替代，其他安全、账本和生命周期门仍保留。
VS_INPUT_ONLY_P4_EXERCISE_GATE_NAMES = frozenset((
    "bypassedKernelCallsPositive",
    "bypassedKernelBytesPositive",
    "poisonCreateAndHitPositive",
    "poisonHitCreateExact",
    "nativeDirectDiscardPathExercised",
    "indexTicketExact",
    "takeoverCountsExact",
    "bypassShadowConsumerPositive",
    "forcedDiagnosticsSnapshot",
    "forcedDiagnosticsQuiescent",
    "outsideNativeFastPathPolicyClean",
    "nativePoisonO1AuthorityContractClean",
    "ledgerTerminalClean",
))

NATIVE_POISON_SHADOW_STATES = (
    "noOverlap", "overlap", "readFailure",
)
NATIVE_POISON_SHADOW_REASON_FIELDS = (
    ("noLock", "noLock"),
    ("multipleLocks", "multi"),
    ("ownerOrLifo", "owner"),
    ("reentry", "reentry"),
    ("resetOrRetirement", "reset"),
    ("poisonMutation", "mutation"),
    ("formatOrFvf", "format"),
    ("lockDescriptor", "lock"),
    ("resourceIdentity", "resource"),
    ("storageIdentity", "storage"),
    ("range", "range"),
    ("kernelNotNormal", "kernel"),
)
NATIVE_POISON_O1_SHADOW_REASON_FIELDS = (
    ("noLock", "noLock"),
    ("multipleLocks", "multipleLocks"),
    ("ownerOrLifo", "ownerOrLifo"),
    ("reentry", "reentry"),
    ("resetOrRetirement", "resetOrRetirement"),
    ("poisonMutation", "poisonMutation"),
    ("formatOrFvf", "formatOrFvf"),
    ("lockDescriptor", "lockDescriptor"),
    ("resourceIdentity", "resourceIdentity"),
    ("storageIdentity", "storageIdentity"),
    ("range", "range"),
    ("kernelNotNormal", "kernelNotNormal"),
    ("kernelNotObserved", "kernelNotObserved"),
    ("multipleKernels", "multipleKernels"),
    ("kernelStateRead", "kernelStateRead"),
    ("kernelMode", "kernelMode"),
    ("kernelFormat", "kernelFormat"),
    ("kernelMappedDst", "kernelMappedDst"),
    ("unlockNotObserved", "unlockNotObserved"),
    ("multipleUnlocks", "multipleUnlocks"),
    ("unlockBeforeFreeze", "unlockBeforeFreeze"),
    ("unlockFailed", "unlockFailed"),
    ("unlockIdentity", "unlockIdentity"),
    ("unlockGeneration", "unlockGeneration"),
    ("outerResult", "outerResult"),
)
NATIVE_POISON_O1_LOCK_LANES = (
    "bufferNoOverwrite",
    "bufferDiscard",
    "directNoOverwrite",
    "directDiscard",
)
NATIVE_POISON_O1_SCAN_FAILURE_FIELDS = (
    "ledgerIncomplete",
    "currentIncomplete",
    "poisonIncomplete",
    "partialIdentity",
    "device",
    "layout",
    "resourceGeneration",
    "range",
)
NATIVE_POISON_O1_UNLOCK_COMPONENTS = (
    "resource", "real", "mapping", "mapAllocation",
)
NATIVE_POISON_O1_DISCARD_JOINT_FIELDS = (
    "oldOverlapFrozen",
    "discardNotifications",
    "discardOldOverlapRetired",
    "oldOToN",
    "exactDiscard",
    "otherOldOToN",
)
LIFECYCLE_HARD_GATE_NAMES = (
    "lifecycleWindowMatrixClean",
    "lifecycleResetRequestsOk",
    "lifecycleResetProgressAlive",
    "lifecycleResetDiagnosticsPresent",
    "lifecycleResetLifecycleClean",
    "lifecycleFirstStopClean",
    "lifecycleRelaunchReady",
    "lifecycleRelaunchAlive",
    "lifecycleRelaunchScreenshot",
    "lifecycleRelaunchDiagnosticsPresent",
    "lifecycleRelaunchGpuSkinClean",
    "lifecycleRelaunchOutlineControlApplied",
    "lifecycleRelaunchOutlineControlRestored",
    "lifecycleRelaunchCrashScanClean",
    "lifecycleSecondStopClean",
)
HARD_GATE_NAMES = {
    "base": BASE_HARD_GATE_NAMES,
    "lifecycle": LIFECYCLE_HARD_GATE_NAMES,
}

sys.path.insert(0, str(HERE))
from war3_autotest_mcp import (  # noqa: E402
    _control_plane_request,
    _finalize_state_after_exact_native_termination,
    _open_native_process_witness,
    _replace_state_retained_native_process,
    STATE,
    capture_war3_screenshot,
    control_war3_window,
    get_runtime_events,
    is_war3_running,
    invoke_internal_test_api,
    launch_war3_test,
    query_war3_window,
    read_runtime_status,
    sync_all_debug,
)


def _launch_war3_with_execution_route_isolation(
    *, execution_route: str, _launcher: Any = None, **kwargs: Any,
) -> Dict[str, Any]:
    """启动时隔离宿主遗留的 GPU 蒙皮路线环境变量。"""
    if execution_route not in GPU_SKIN_EXECUTION_ROUTES:
        raise ValueError(f"unsupported GPU-skin execution route: {execution_route}")

    inherited_present = GPU_SKIN_EXECUTION_ROUTE_ENV in os.environ
    inherited_value = os.environ.pop(GPU_SKIN_EXECUTION_ROUTE_ENV, None)
    try:
        result = (_launcher or launch_war3_test)(**kwargs)
    finally:
        if inherited_present:
            os.environ[GPU_SKIN_EXECUTION_ROUTE_ENV] = str(
                inherited_value if inherited_value is not None else ""
            )
        else:
            os.environ.pop(GPU_SKIN_EXECUTION_ROUTE_ENV, None)

    result = dict(result or {})
    result["executionRouteHostEnvironmentIsolation"] = {
        "environmentName": GPU_SKIN_EXECUTION_ROUTE_ENV,
        "inheritedValueRemoved": inherited_present,
        "hostEnvironmentRestored": bool(
            (GPU_SKIN_EXECUTION_ROUTE_ENV in os.environ) == inherited_present
            and (
                not inherited_present
                or os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                    str(inherited_value if inherited_value is not None else "")
            )
        ),
        "note": "不记录宿主原值；子进程只消费本次导体显式的路线配置。",
    }
    return result


def _now() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def _json_write(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def _text_write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", errors="replace")


def _sha256(path: Path) -> Optional[str]:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def _map_metadata(path: Path) -> Dict[str, Any]:
    stat = path.stat()
    return {
        "resolvedPath": str(path),
        "size": stat.st_size,
        "sha256": _sha256(path),
    }


def _sanitize_case_label(value: Optional[str]) -> str:
    if not value:
        return ""
    # Keep Unicode word characters for readable local case names while
    # excluding every Windows path separator/reserved punctuation character.
    safe = re.sub(r"[^\w.-]+", "_", value.strip(), flags=re.UNICODE)
    return safe.strip("._-")[:48]


def _run(command: List[str], timeout_sec: int = 120) -> Dict[str, Any]:
    try:
        completed = subprocess.run(
            command,
            cwd=str(ROOT),
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_sec,
            check=False,
        )
        return {
            "command": command,
            "returncode": completed.returncode,
            "output": completed.stdout,
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "command": command,
            "returncode": None,
            "timedOut": True,
            "output": str(exc),
        }
    except OSError as exc:
        return {"command": command, "returncode": None, "error": str(exc)}


def _git_summary() -> Dict[str, Any]:
    return {
        "head": _run(["git", "rev-parse", "HEAD"], 20).get("output", "").strip(),
        "branch": _run(["git", "branch", "--show-current"], 20).get("output", "").strip(),
        "status": _run(["git", "status", "--short"], 30).get("output", ""),
        "diffStat": _run(["git", "diff", "--stat"], 30).get("output", ""),
        "diffNumstat": _run(["git", "diff", "--numstat"], 30).get("output", ""),
    }


def _dll_hashes() -> Dict[str, Optional[str]]:
    return {
        "build32D3D9": _sha256(BUILD_DLL),
        "deployedD3D9": _sha256(DEPLOYED_DLL),
        "gameDll": _sha256(GAME_DLL),
    }


def _outline_shader_hashes() -> Dict[str, Optional[str]]:
    return {
        "repoShaderConfig": _sha256(REPO_SHADER_CONFIG),
        "deployedShaderConfig": _sha256(DEPLOYED_SHADER_CONFIG),
        "repoOutlineShader": _sha256(REPO_OUTLINE_SHADER),
        "deployedOutlineShader": _sha256(DEPLOYED_OUTLINE_SHADER),
    }


def _snapshot_log_offsets() -> Dict[str, Dict[str, Any]]:
    result: Dict[str, Dict[str, Any]] = {}
    for path in LOG_CANDIDATES:
        if path.is_file():
            stat = path.stat()
            result[str(path)] = {"exists": True, "size": stat.st_size, "mtime": stat.st_mtime}
        else:
            result[str(path)] = {"exists": False, "size": 0, "mtime": 0.0}
    return result


def _copy_new_log_bytes(offsets: Dict[str, Dict[str, Any]], out_dir: Path, tag: str) -> Dict[str, Any]:
    saved: Dict[str, Any] = {}
    for raw_path, before in offsets.items():
        path = Path(raw_path)
        if not path.is_file():
            continue
        start = int(before.get("size", 0)) if before.get("exists") else 0
        size = path.stat().st_size
        if size < start:
            start = 0
        data = path.read_bytes()[start:]
        target = out_dir / (path.stem + f"_{tag}_pre_stop" + path.suffix)
        target.write_bytes(data)
        saved[str(path)] = {"output": str(target), "start": start, "bytes": len(data)}
    return saved


def _read_new_log_text(offsets: Dict[str, Dict[str, Any]]) -> str:
    parts: List[str] = []
    for raw_path, before in offsets.items():
        path = Path(raw_path)
        if not path.is_file():
            continue
        try:
            start = int(before.get("size", 0)) if before.get("exists") else 0
            size = path.stat().st_size
            if size < start:
                start = 0
            parts.append(path.read_bytes()[start:].decode("utf-8", errors="replace"))
        except OSError:
            continue
    return "\n".join(parts)


def _strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)


def _gpu_skin_diag_payload(line: str) -> str:
    """Normalize DBWIN and `Logger::info` lines to one exact payload."""
    clean = _strip_ansi(str(line or "")).strip()
    marker = "DXVK War3GpuSkin:"
    index = clean.find(marker)
    return clean[index:] if index >= 0 else clean


def _int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value, 0)
    except ValueError:
        return None


def _match_int(text: str, pattern: str) -> Optional[int]:
    match = re.search(pattern, text, re.IGNORECASE)
    return _int(match.group(1)) if match else None


def _match_tuple(text: str, pattern: str, count: int) -> List[Optional[int]]:
    match = re.search(pattern, text, re.IGNORECASE)
    if not match:
        return [None] * count
    return [_int(match.group(index + 1)) for index in range(count)]


def _match_named_int(text: str, *names: str) -> Optional[int]:
    """Read a named diagnostic field without accepting an absent field as zero."""
    if not text:
        return None
    alternatives = "|".join(re.escape(name) for name in names)
    return _match_int(text, rf"\b(?:{alternatives})\s*[:=]\s*(0x[0-9a-fA-F]+|\d+)")


def _parse_native_poison_shadow(
    main_line: str, reasons_line: str,
) -> Dict[str, Any]:
    scalar_fields = (
        ("attempts", "attempt"),
        ("created", "created"),
        ("overflow", "overflow"),
        ("active", "active"),
        ("lockNotifications", "locks"),
        ("settled", "settled"),
        ("cancelled", "cancel"),
        ("resetAborted", "resetAbort"),
        ("comparable", "comparable"),
        ("unprovable", "unprovable"),
        ("offDiagonal", "offdiag"),
        ("legacyMissedOverlap", "legacyMissedOverlap"),
    )
    scalars = {
        output_name: _match_named_int(main_line, log_name)
        for output_name, log_name in scalar_fields
    }
    matrix_flat = _match_tuple(
        main_line,
        r"\bmatrix=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/"
        r"(\d+)/(\d+)/(\d+)",
        9,
    )
    closure_values = _match_tuple(
        main_line, r"\bclosure=(\d+)/(\d+)/(\d+)", 3,
    )
    reasons = {
        output_name: _match_named_int(reasons_line, log_name)
        for output_name, log_name in NATIVE_POISON_SHADOW_REASON_FIELDS
    }
    values_present = all(
        isinstance(value, int)
        for value in (
            *scalars.values(), *matrix_flat, *closure_values,
            *reasons.values(),
        )
    )
    matrix_total = sum(matrix_flat) if values_present else None
    reason_total = sum(reasons.values()) if values_present else None
    off_diagonal_computed = (
        sum(matrix_flat[index] for index in (1, 2, 3, 5, 6, 7))
        if values_present else None
    )
    matrix = {
        old_state: dict(zip(
            NATIVE_POISON_SHADOW_STATES,
            matrix_flat[row * 3:(row + 1) * 3],
        ))
        for row, old_state in enumerate(NATIVE_POISON_SHADOW_STATES)
    }
    computed_closure = {
        "attempt": bool(
            values_present
            and scalars["attempts"] ==
                scalars["created"] + scalars["overflow"]
        ),
        "lifetime": bool(
            values_present
            and scalars["created"] ==
                scalars["settled"] + scalars["cancelled"]
                + scalars["resetAborted"] + scalars["active"]
        ),
        "settlement": bool(
            values_present
            and scalars["settled"] ==
                scalars["comparable"] + scalars["unprovable"]
            and scalars["comparable"] == matrix_total
            and scalars["unprovable"] == reason_total
        ),
        "offDiagonal": bool(
            values_present
            and scalars["offDiagonal"] == off_diagonal_computed
        ),
        "legacyMissedOverlap": bool(
            values_present
            and scalars["legacyMissedOverlap"] == matrix_flat[1]
        ),
    }
    reported_closure = dict(zip(
        ("attempt", "lifetime", "settlement"), closure_values,
    ))
    reported_closure_clean = bool(
        values_present and closure_values == [1, 1, 1]
    )
    # A disabled production-light sidecar deliberately observes no attempts
    # even while the outer poison scanner remains active.  Native therefore
    # reports attempt closure 0, but the immutable zero surface is still an
    # exact cold contract when every locally-computable closure is true.  This
    # bit is never sufficient for an enabled/live sidecar.
    cold_contract_closed = bool(
        values_present and scalars["active"] == 0
        and closure_values[0] in (0, 1)
        and closure_values[1:] == [1, 1]
        and all(computed_closure.values())
        and all(value == 0 for value in (
            *scalars.values(), *matrix_flat, *reasons.values(),
        ))
    )
    contract_closed = bool(
        values_present and reported_closure_clean
        and all(computed_closure.values())
    )
    counter_values = (
        *scalars.values(), *matrix_flat, *reasons.values(),
    )
    return {
        **scalars,
        "matrix": matrix,
        "matrixFlat": matrix_flat,
        "matrixAxis": {
            "rows": "legacySafeCopy",
            "columns": "d3d9VertexLockSidecar",
            "order": list(NATIVE_POISON_SHADOW_STATES),
        },
        "matrixTotal": matrix_total,
        "unprovableReasons": reasons,
        "unprovableReasonTotal": reason_total,
        "reportedClosure": reported_closure,
        "computedClosure": computed_closure,
        "present": values_present,
        "contractClosed": contract_closed,
        "coldContractClosed": cold_contract_closed,
        "allCountersZero": bool(
            values_present and all(value == 0 for value in counter_values)
        ),
        "endpointClean": bool(
            contract_closed and scalars["active"] == 0
            and scalars["legacyMissedOverlap"] == 0
        ),
        # O0 is evidence only. Neither this parser nor its hard gate exposes an
        # authorization bit to the runtime takeover path.
        "authorizationAuthority": 0,
        "reportOnly": True,
        "raw": {
            "main": main_line,
            "reasons": reasons_line,
        },
    }


def _native_poison_shadow_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    cumulative_names = (
        "attempts", "created", "overflow", "lockNotifications",
        "settled", "cancelled", "resetAborted", "comparable",
        "unprovable", "offDiagonal", "legacyMissedOverlap",
    )
    previous_matrix = list(previous.get("matrixFlat", []) or [])
    current_matrix = list(current.get("matrixFlat", []) or [])
    previous_reasons = dict(previous.get("unprovableReasons", {}) or {})
    current_reasons = dict(current.get("unprovableReasons", {}) or {})
    reason_names = tuple(
        output_name for output_name, _ in NATIVE_POISON_SHADOW_REASON_FIELDS
    )
    present = bool(
        previous.get("present") is True
        and current.get("present") is True
        and previous.get("contractClosed") is True
        and current.get("contractClosed") is True
        and len(previous_matrix) == len(current_matrix) == 9
        and all(
            isinstance(previous.get(name), int)
            and isinstance(current.get(name), int)
            for name in cumulative_names + ("active",)
        )
        and all(
            isinstance(previous_reasons.get(name), int)
            and isinstance(current_reasons.get(name), int)
            for name in reason_names
        )
    )
    monotonic = bool(
        present
        and all(current[name] >= previous[name] for name in cumulative_names)
        and all(
            current_value >= previous_value
            for previous_value, current_value in zip(
                previous_matrix, current_matrix,
            )
        )
        and all(
            current_reasons[name] >= previous_reasons[name]
            for name in reason_names
        )
    )
    if not monotonic:
        return {
            "present": present,
            "monotonic": False,
            "valid": False,
            "reason": "missing, invalid, or non-monotonic poison-shadow counters",
            "authorizationAuthority": 0,
            "reportOnly": True,
        }
    deltas = {
        name: current[name] - previous[name] for name in cumulative_names
    }
    matrix_delta = [
        current_value - previous_value
        for previous_value, current_value in zip(
            previous_matrix, current_matrix,
        )
    ]
    reasons_delta = {
        name: current_reasons[name] - previous_reasons[name]
        for name in reason_names
    }
    matrix_total = sum(matrix_delta)
    reason_total = sum(reasons_delta.values())
    off_diagonal = sum(
        matrix_delta[index] for index in (1, 2, 3, 5, 6, 7)
    )
    endpoints_quiescent = bool(
        previous["active"] == 0 and current["active"] == 0
    )
    computed_closure = {
        "attempt": deltas["attempts"] ==
            deltas["created"] + deltas["overflow"],
        "lifetime": (
            endpoints_quiescent
            and deltas["created"] ==
                deltas["settled"] + deltas["cancelled"]
                + deltas["resetAborted"]
        ),
        "settlement": (
            deltas["settled"] ==
                deltas["comparable"] + deltas["unprovable"]
            and deltas["comparable"] == matrix_total
            and deltas["unprovable"] == reason_total
        ),
        "offDiagonal": deltas["offDiagonal"] == off_diagonal,
        "legacyMissedOverlap": (
            deltas["legacyMissedOverlap"] == matrix_delta[1]
        ),
    }
    valid = bool(
        endpoints_quiescent and all(computed_closure.values())
    )
    matrix = {
        old_state: dict(zip(
            NATIVE_POISON_SHADOW_STATES,
            matrix_delta[row * 3:(row + 1) * 3],
        ))
        for row, old_state in enumerate(NATIVE_POISON_SHADOW_STATES)
    }
    return {
        "present": True,
        "monotonic": True,
        "valid": valid,
        "endpointsQuiescent": endpoints_quiescent,
        "deltas": deltas,
        "matrix": matrix,
        "matrixFlat": matrix_delta,
        "matrixAxis": {
            "rows": "legacySafeCopy",
            "columns": "d3d9VertexLockSidecar",
            "order": list(NATIVE_POISON_SHADOW_STATES),
        },
        "matrixTotal": matrix_total,
        "unprovableReasons": reasons_delta,
        "unprovableReasonTotal": reason_total,
        "computedClosure": computed_closure,
        "allDeltasZero": all(
            value == 0
            for value in (
                *deltas.values(), *matrix_delta, *reasons_delta.values(),
            )
        ),
        "authorizationAuthority": 0,
        "reportOnly": True,
        "note": (
            "O0 permits unprovable and conservative off-diagonal cells; "
            "legacy no-overlap -> sidecar overlap remains a hard failure."
        ),
    }


def _native_poison_shadow_pair_policy(
    previous: Dict[str, Any], current: Dict[str, Any],
    poison_scan_delta: Optional[int], full: bool, light: bool,
) -> Dict[str, Any]:
    delta = _native_poison_shadow_delta(previous, current)
    attempts_delta = delta.get("deltas", {}).get("attempts")
    legacy_delta = delta.get("deltas", {}).get("legacyMissedOverlap")
    recognized_exact = bool(full != light)
    full_exact = bool(
        full and not light and poison_scan_delta == 0
        and delta.get("valid") is True
        and delta.get("allDeltasZero") is True
        and previous.get("allCountersZero") is True
        and current.get("allCountersZero") is True
    )
    light_implication_exact = bool(
        isinstance(poison_scan_delta, int) and poison_scan_delta >= 0
        and (
            poison_scan_delta == 0 and attempts_delta == 0
            or poison_scan_delta > 0
            and isinstance(attempts_delta, int) and attempts_delta > 0
            and attempts_delta == poison_scan_delta
        )
    )
    light_exact = bool(
        light and not full and delta.get("valid") is True
        and light_implication_exact
        and previous.get("active") == 0 and current.get("active") == 0
        and previous.get("legacyMissedOverlap") == 0
        and current.get("legacyMissedOverlap") == 0
        and legacy_delta == 0
    )
    return {
        "present": bool(
            previous.get("present") is True
            and current.get("present") is True
            and isinstance(poison_scan_delta, int)
        ),
        "recognizedExact": recognized_exact,
        "full": full,
        "light": light,
        "fullExact": full_exact,
        "lightExact": light_exact,
        "exact": bool(
            recognized_exact and (full_exact if full else light_exact)
        ),
        "poisonScanAttemptsDelta": poison_scan_delta,
        "attemptsDeltaMatchesPoisonScan": (
            attempts_delta == poison_scan_delta
            if isinstance(attempts_delta, int)
            and isinstance(poison_scan_delta, int) else None
        ),
        "delta": delta,
        "authorizationAuthority": 0,
        "reportOnly": True,
    }


def _native_poison_shadow_synthetic_self_tests() -> Dict[str, Any]:
    """Pure parser/contract fixtures; never touches War3 or the filesystem."""
    def snapshot(
        matrix: Optional[List[int]] = None,
        reasons: Optional[List[int]] = None,
        active: int = 0,
        overrides: Optional[Dict[str, int]] = None,
        include_reasons: bool = True,
    ) -> Dict[str, Any]:
        matrix = list(matrix if matrix is not None else [0] * 9)
        reasons = list(
            reasons if reasons is not None
            else [0] * len(NATIVE_POISON_SHADOW_REASON_FIELDS)
        )
        comparable = sum(matrix)
        unprovable = sum(reasons)
        settled = comparable + unprovable
        values = {
            "attempt": settled + active,
            "created": settled + active,
            "overflow": 0,
            "active": active,
            "locks": settled,
            "settled": settled,
            "cancel": 0,
            "resetAbort": 0,
            "comparable": comparable,
            "unprovable": unprovable,
            "offdiag": sum(matrix[index] for index in (1, 2, 3, 5, 6, 7)),
            "legacyMissedOverlap": matrix[1],
        }
        values.update(dict(overrides or {}))
        main = (
            "DXVK War3GpuSkin: diag nativePoisonShadow "
            f"attempt={values['attempt']} created={values['created']} "
            f"overflow={values['overflow']} active={values['active']} "
            f"locks={values['locks']} settled={values['settled']} "
            f"cancel={values['cancel']} resetAbort={values['resetAbort']} "
            f"comparable={values['comparable']} "
            f"unprovable={values['unprovable']} "
            f"matrix={'/'.join(str(value) for value in matrix)} "
            f"offdiag={values['offdiag']} "
            f"legacyMissedOverlap={values['legacyMissedOverlap']} "
            "closure=1/1/1"
        )
        reasons_line = ""
        if include_reasons:
            reasons_line = (
                "DXVK War3GpuSkin: diag nativePoisonShadowReasons "
                + " ".join(
                    f"{log_name}={value}"
                    for (_, log_name), value in zip(
                        NATIVE_POISON_SHADOW_REASON_FIELDS, reasons,
                    )
                )
            )
        return _parse_native_poison_shadow(main, reasons_line)

    zero = snapshot()
    light = snapshot(
        matrix=[2, 0, 0, 0, 1, 0, 0, 0, 0],
        reasons=[1] + [0] * 11,
    )
    conservative_offdiag = snapshot(
        matrix=[0, 0, 0, 1, 0, 0, 0, 0, 0],
    )
    legacy_missed = snapshot(
        matrix=[0, 1, 0, 0, 0, 0, 0, 0, 0],
    )
    active = snapshot(active=1)
    matrix_mismatch = snapshot(
        matrix=[1, 0, 0, 0, 0, 0, 0, 0, 0],
        overrides={"comparable": 2, "settled": 2, "created": 2,
                   "attempt": 2},
    )
    attempt_mismatch = snapshot(overrides={"attempt": 1})
    reason_mismatch = snapshot(
        reasons=[1] + [0] * 11,
        overrides={"unprovable": 0, "settled": 0, "created": 0,
                   "attempt": 0},
    )
    reported_closure_mismatch = _parse_native_poison_shadow(
        zero["raw"]["main"].replace("closure=1/1/1", "closure=1/0/1"),
        zero["raw"]["reasons"],
    )
    missing_reasons = snapshot(include_reasons=False)
    full_policy = _native_poison_shadow_pair_policy(
        zero, zero, 0, True, False,
    )
    light_policy = _native_poison_shadow_pair_policy(
        zero, light, light["attempts"], False, True,
    )
    conservative_policy = _native_poison_shadow_pair_policy(
        zero, conservative_offdiag, conservative_offdiag["attempts"],
        False, True,
    )
    legacy_policy = _native_poison_shadow_pair_policy(
        zero, legacy_missed, legacy_missed["attempts"], False, True,
    )
    poison_scan_mismatch_policy = _native_poison_shadow_pair_policy(
        zero, light, light["attempts"] + 1, False, True,
    )
    full_nonzero_policy = _native_poison_shadow_pair_policy(
        zero, light, light["attempts"], True, False,
    )
    active_policy = _native_poison_shadow_pair_policy(
        zero, active, active["attempts"], False, True,
    )
    integrated = _parse_gpu_skin_diag(
        zero["raw"]["main"] + "\n" + zero["raw"]["reasons"], {}
    ).get("nativePoisonShadow", {})
    checks = {
        "zeroParsed": zero["present"] and zero["contractClosed"],
        "latestLineIntegrationParsed": (
            integrated.get("present") is True
            and integrated.get("contractClosed") is True
        ),
        "fullAllCountersZero": zero["allCountersZero"],
        "fullPolicyExact": full_policy["exact"],
        "lightPolicyExact": light_policy["exact"],
        "unprovableAllowedAtO0": (
            light_policy["exact"] and
            light_policy["delta"]["deltas"]["unprovable"] == 1
        ),
        "conservativeOffDiagonalAllowedAtO0": (
            conservative_policy["exact"] and
            conservative_policy["delta"]["deltas"]["offDiagonal"] == 1
        ),
        "legacyMissedOverlapRejected": not legacy_policy["exact"],
        "activeEndpointRejected": not active_policy["exact"],
        "matrixMismatchRejected": not matrix_mismatch["contractClosed"],
        "attemptMismatchRejected": not attempt_mismatch["contractClosed"],
        "reasonMismatchRejected": not reason_mismatch["contractClosed"],
        "reportedClosureMismatchRejected": (
            not reported_closure_mismatch["contractClosed"]
        ),
        "missingReasonsRejected": not missing_reasons["present"],
        "poisonScanMismatchRejected": not poison_scan_mismatch_policy["exact"],
        "fullNonzeroRejected": not full_nonzero_policy["exact"],
        "authorityAlwaysZero": all(
            item.get("authorizationAuthority") == 0
            for item in (
                zero, light, full_policy, light_policy,
                conservative_policy, legacy_policy, active_policy,
            )
        ),
    }
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "native poison-shadow synthetic self-test failed: "
            + json.dumps(result, sort_keys=True)
        )
    return result


def _parse_native_poison_o1_authority(
    main_line: str, evidence_line: str,
) -> Dict[str, Any]:
    """Parse production O1's actual-Lock authority and legacy evidence lane."""
    scalar_fields = (
        ("attempts", "attempt"),
        ("created", "created"),
        ("overflow", "overflow"),
        ("armed", "armed"),
        ("active", "active"),
        ("settled", "settled"),
        ("cancelled", "cancel"),
        ("resetAborted", "resetAbort"),
    )
    scalars = {
        output_name: _match_named_int(main_line, log_name)
        for output_name, log_name in scalar_fields
    }
    lock_values = _match_tuple(
        main_line, r"\block=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    kernel_values = _match_tuple(
        main_line, r"\bkernel=(\d+)/(\d+)/(\d+)", 3,
    )
    unlock_values = _match_tuple(
        main_line, r"\bunlock=(\d+)/(\d+)/(\d+)", 3,
    )
    commit_values = _match_tuple(
        main_line, r"\bcommit=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5,
    )
    closure_values = _match_tuple(
        main_line, r"\bclosure=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 6,
    )
    evidence = {
        "attempts": _match_named_int(evidence_line, "attempt"),
        "comparable": _match_named_int(evidence_line, "comparable"),
        "unprovable": _match_named_int(evidence_line, "unprovable"),
        "mismatches": _match_named_int(evidence_line, "mismatch"),
        "authority": _match_named_int(evidence_line, "authority"),
        "legacyBackedAuthority": _match_named_int(
            evidence_line, "legacyAuthority",
        ),
    }
    evidence_matrix = _match_tuple(
        evidence_line,
        r"\bmatrix=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/"
        r"(\d+)/(\d+)/(\d+)",
        9,
    )
    values_present = all(
        isinstance(value, int)
        for value in (
            *scalars.values(), *lock_values, *kernel_values,
            *unlock_values, *commit_values, *closure_values,
            *evidence.values(), *evidence_matrix,
        )
    )
    lock = dict(zip(
        ("notifications", "noOverlap", "overlap", "rejects"),
        lock_values,
    ))
    kernel = dict(zip(("ready", "rejects", "normalReturns"), kernel_values))
    unlock = dict(zip(("notifications", "exact", "rejects"), unlock_values))
    commit = dict(zip(
        ("noOverlap", "rewrite", "retained", "poisonClears", "authority"),
        commit_values,
    ))
    matrix_total = sum(evidence_matrix) if values_present else None
    computed_closure = {
        "attempt": bool(
            values_present
            and scalars["attempts"] == scalars["created"] + scalars["overflow"]
        ),
        "lifetime": bool(
            values_present
            and scalars["created"] == scalars["settled"]
                + scalars["cancelled"] + scalars["resetAborted"]
                + scalars["active"]
        ),
        "lock": bool(
            values_present
            and scalars["armed"] == lock["noOverlap"]
                + lock["overlap"] + lock["rejects"]
            and lock["notifications"] <= scalars["armed"]
        ),
        "execution": bool(
            values_present
            and scalars["armed"] == kernel["ready"] + kernel["rejects"]
            and scalars["armed"] == unlock["exact"] + unlock["rejects"]
            and kernel["normalReturns"] <= scalars["armed"]
            and unlock["notifications"] <= scalars["armed"]
        ),
        "settlement": bool(
            values_present
            and commit["authority"] == commit["noOverlap"] + commit["rewrite"]
            and scalars["armed"] == commit["authority"] + commit["retained"]
            and commit["poisonClears"] == commit["rewrite"]
        ),
        "evidence": bool(
            values_present
            and evidence["attempts"] <= scalars["attempts"]
            and evidence["attempts"] == evidence["comparable"]
                + evidence["unprovable"]
            and evidence["comparable"] == matrix_total
            and evidence["mismatches"] == 0
            and evidence["authority"] == 0
            and evidence["legacyBackedAuthority"] == 0
        ),
    }
    reported_closure = dict(zip(
        ("attempt", "lifetime", "lock", "execution", "settlement", "evidence"),
        closure_values,
    ))
    contract_closed = bool(
        values_present and closure_values == [1, 1, 1, 1, 1, 1]
        and all(computed_closure.values())
    )
    counter_values = (
        *scalars.values(), *lock_values, *kernel_values, *unlock_values,
        *commit_values, *evidence.values(), *evidence_matrix,
    )
    evidence_matrix_rows = {
        old_state: dict(zip(
            NATIVE_POISON_SHADOW_STATES,
            evidence_matrix[row * 3:(row + 1) * 3],
        ))
        for row, old_state in enumerate(NATIVE_POISON_SHADOW_STATES)
    }
    return {
        **scalars,
        "lock": lock,
        "kernel": kernel,
        "unlock": unlock,
        "commit": commit,
        "authority": commit["authority"],
        "evidence": {
            **evidence,
            "matrix": evidence_matrix_rows,
            "matrixFlat": evidence_matrix,
            "matrixTotal": matrix_total,
            "matrixAxis": {
                "rows": "legacySafeCopy",
                "columns": "actualSuccessfulLock",
                "order": list(NATIVE_POISON_SHADOW_STATES),
            },
            "authorizationAuthority": evidence["authority"],
            "reportOnly": True,
        },
        "reportedClosure": reported_closure,
        "computedClosure": computed_closure,
        "present": values_present,
        "contractClosed": contract_closed,
        "endpointClean": bool(contract_closed and scalars["active"] == 0),
        "allCountersZero": bool(
            values_present and all(value == 0 for value in counter_values)
        ),
        # Unlike O0/O1a, this is real manager-free CPU-only authority. It never
        # bypasses the original native kernel; commit means only that the actual
        # Lock/kernel/Unlock transaction was safe to settle.
        "authorizationAuthority": commit["authority"],
        "reportOnly": False,
        "raw": {"main": main_line, "evidence": evidence_line},
    }


def _native_poison_o1_authority_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    cumulative_names = (
        "attempts", "created", "overflow", "armed", "settled", "cancelled",
        "resetAborted",
    )
    grouped_names = {
        "lock": ("notifications", "noOverlap", "overlap", "rejects"),
        "kernel": ("ready", "rejects", "normalReturns"),
        "unlock": ("notifications", "exact", "rejects"),
        "commit": ("noOverlap", "rewrite", "retained", "poisonClears", "authority"),
        "evidence": (
            "attempts", "comparable", "unprovable", "mismatches",
            "authority", "legacyBackedAuthority",
        ),
    }
    previous_matrix = list(
        previous.get("evidence", {}).get("matrixFlat", []) or []
    )
    current_matrix = list(
        current.get("evidence", {}).get("matrixFlat", []) or []
    )
    present = bool(
        previous.get("present") is True and current.get("present") is True
        and previous.get("contractClosed") is True
        and current.get("contractClosed") is True
        and len(previous_matrix) == len(current_matrix) == 9
        and all(
            isinstance(previous.get(name), int)
            and isinstance(current.get(name), int)
            for name in cumulative_names + ("active",)
        )
        and all(
            isinstance(previous.get(group, {}).get(name), int)
            and isinstance(current.get(group, {}).get(name), int)
            for group, names in grouped_names.items() for name in names
        )
    )
    monotonic = bool(
        present
        and all(current[name] >= previous[name] for name in cumulative_names)
        and all(
            current[group][name] >= previous[group][name]
            for group, names in grouped_names.items() for name in names
        )
        and all(
            current_value >= previous_value
            for previous_value, current_value in zip(
                previous_matrix, current_matrix,
            )
        )
    )
    if not monotonic:
        return {
            "present": present,
            "monotonic": False,
            "valid": False,
            "reason": "missing, invalid, or non-monotonic O1 authority counters",
            "authorizationAuthority": 0,
        }
    deltas = {
        name: current[name] - previous[name] for name in cumulative_names
    }
    grouped_deltas = {
        group: {
            name: current[group][name] - previous[group][name]
            for name in names
        }
        for group, names in grouped_names.items()
    }
    matrix_delta = [
        current_value - previous_value
        for previous_value, current_value in zip(
            previous_matrix, current_matrix,
        )
    ]
    endpoints_quiescent = previous.get("active") == current.get("active") == 0
    lock = grouped_deltas["lock"]
    kernel = grouped_deltas["kernel"]
    unlock = grouped_deltas["unlock"]
    commit = grouped_deltas["commit"]
    evidence = grouped_deltas["evidence"]
    computed_closure = {
        "attempt": deltas["attempts"] == deltas["created"] + deltas["overflow"],
        "lifetime": bool(
            endpoints_quiescent and deltas["created"] == deltas["settled"]
                + deltas["cancelled"] + deltas["resetAborted"]
        ),
        "lock": bool(
            deltas["armed"] == lock["noOverlap"] + lock["overlap"]
                + lock["rejects"]
            and lock["notifications"] <= deltas["armed"]
        ),
        "execution": bool(
            deltas["armed"] == kernel["ready"] + kernel["rejects"]
            and deltas["armed"] == unlock["exact"] + unlock["rejects"]
            and kernel["normalReturns"] <= deltas["armed"]
            and unlock["notifications"] <= deltas["armed"]
        ),
        "settlement": bool(
            commit["authority"] == commit["noOverlap"] + commit["rewrite"]
            and deltas["armed"] == commit["authority"] + commit["retained"]
            and commit["poisonClears"] == commit["rewrite"]
        ),
        "evidence": bool(
            evidence["attempts"] <= deltas["attempts"]
            and evidence["attempts"] == evidence["comparable"]
                + evidence["unprovable"]
            and evidence["comparable"] == sum(matrix_delta)
            and evidence["mismatches"] == 0
            and evidence["authority"] == 0
            and evidence["legacyBackedAuthority"] == 0
        ),
    }
    valid = bool(endpoints_quiescent and all(computed_closure.values()))
    return {
        "present": True,
        "monotonic": True,
        "valid": valid,
        "endpointsQuiescent": endpoints_quiescent,
        "deltas": deltas,
        **grouped_deltas,
        "matrixFlat": matrix_delta,
        "matrixTotal": sum(matrix_delta),
        "computedClosure": computed_closure,
        "allDeltasZero": all(
            value == 0
            for value in (
                *deltas.values(),
                *(value for group in grouped_deltas.values()
                  for value in group.values()),
                *matrix_delta,
            )
        ),
        "authorizationAuthority": commit["authority"],
    }


def _native_poison_o1_authority_pair_policy(
    previous: Dict[str, Any], current: Dict[str, Any],
    full: bool, light: bool, sidecar_enabled: bool,
    poison_scan_delta: Optional[int] = None,
    poison_no_overlap_delta: Optional[int] = None,
) -> Dict[str, Any]:
    delta = _native_poison_o1_authority_delta(previous, current)
    values = dict(delta.get("deltas", {}) or {})
    lock = dict(delta.get("lock", {}) or {})
    kernel = dict(delta.get("kernel", {}) or {})
    unlock = dict(delta.get("unlock", {}) or {})
    commit = dict(delta.get("commit", {}) or {})
    evidence = dict(delta.get("evidence", {}) or {})
    matrix = list(delta.get("matrixFlat", []) or [])
    attempts = values.get("attempts")
    evidence_attempts = evidence.get("attempts")
    cadence_exact = bool(
        isinstance(attempts, int) and attempts >= 0
        and isinstance(evidence_attempts, int)
        and attempts // 127 <= evidence_attempts <= (attempts + 126) // 127
        and (attempts < 127 or evidence_attempts > 0)
    )
    matrix_exact = bool(
        len(matrix) == 9
        and all(matrix[index] == 0 for index in (1, 2, 5, 6, 7, 8))
    )
    matrix_total_exact = bool(
        len(matrix) == 9 and evidence.get("comparable") == sum(matrix)
    )
    evidence_old_overlap_or_failure = (
        sum(matrix[3:9]) if len(matrix) == 9 else None
    )
    expected_armed = (
        attempts - poison_scan_delta + poison_no_overlap_delta
        + evidence_old_overlap_or_failure
        if all(isinstance(value, int) for value in (
            attempts, poison_scan_delta, poison_no_overlap_delta,
            evidence_old_overlap_or_failure,
        )) else None
    )
    scan_partition_exact = bool(
        isinstance(poison_scan_delta, int)
        and isinstance(poison_no_overlap_delta, int)
        and isinstance(attempts, int)
        and isinstance(evidence_attempts, int)
        and 0 <= poison_no_overlap_delta <= poison_scan_delta <= attempts
        and (
            sidecar_enabled and poison_scan_delta == attempts
            or not sidecar_enabled
            and poison_scan_delta == evidence_attempts
        )
    )
    full_exact = bool(
        full and not light and delta.get("valid") is True
        and delta.get("allDeltasZero") is True
        and previous.get("allCountersZero") is True
        and current.get("allCountersZero") is True
    )
    common_light_exact = bool(
        light and not full and delta.get("valid") is True
        and isinstance(attempts, int) and attempts > 0
        and values.get("created") == values.get("settled") == attempts
        and values.get("overflow") == values.get("cancelled") ==
            values.get("resetAborted") == 0
        and isinstance(expected_armed, int) and expected_armed > 0
        and values.get("armed") == expected_armed
        and lock.get("notifications") == expected_armed
        and lock.get("noOverlap", 0) + lock.get("overlap", 0) == expected_armed
        and lock.get("rejects") == 0
        and kernel.get("ready") == kernel.get("normalReturns") == expected_armed
        and kernel.get("rejects") == 0
        and unlock.get("notifications") == unlock.get("exact") == expected_armed
        and unlock.get("rejects") == 0
        and commit.get("authority") ==
            commit.get("noOverlap", 0) + commit.get("rewrite", 0)
        and commit.get("authority", 0) + commit.get("retained", 0) == expected_armed
        and commit.get("retained", 0) >= evidence_attempts
        and commit.get("poisonClears") == commit.get("rewrite")
        and evidence.get("comparable") == evidence_attempts
        and evidence.get("unprovable") == evidence.get("mismatches") == 0
        and evidence.get("authority") == 0
        and evidence.get("legacyBackedAuthority") == 0
        and matrix_total_exact and cadence_exact and matrix_exact
        and matrix[0] > 0 and scan_partition_exact
    )
    sidecar_partition_exact = bool(
        common_light_exact and sidecar_enabled
        and commit.get("authority") == 0
        and commit.get("retained") == expected_armed
    )
    production_partition_exact = bool(
        common_light_exact and not sidecar_enabled
        and isinstance(commit.get("authority"), int)
        and commit["authority"] > 0
    )
    light_exact = bool(
        sidecar_partition_exact if sidecar_enabled
        else production_partition_exact
    )
    return {
        "present": delta.get("present") is True,
        "recognizedExact": bool(full != light),
        "full": full,
        "light": light,
        "sidecarEnabled": sidecar_enabled,
        "fullExact": full_exact,
        "lightExact": light_exact,
        "exact": bool(full_exact if full else light_exact),
        "commonLightExact": common_light_exact,
        "sidecarPartitionExact": sidecar_partition_exact,
        "productionPartitionExact": production_partition_exact,
        "evidenceCadenceExact": cadence_exact,
        "evidenceMatrixExact": matrix_exact,
        "poisonScanDelta": poison_scan_delta,
        "poisonNoOverlapDelta": poison_no_overlap_delta,
        "scanPartitionExact": scan_partition_exact,
        "expectedArmed": expected_armed,
        "delta": delta,
        "authorizationAuthority": commit.get("authority"),
        "reportOnly": False,
        "note": (
            "Production O1 always executes the original CPU kernel once. With "
            "sidecars disabled, actual Lock/normal-return/Unlock may settle N "
            "or an exact O rewrite; sidecar modes retain every legacy-backed "
            "candidate and grant zero production authority."
        ),
    }


def _outside_poison_accepted_partition_contract(
    accepted_with_poison: Any,
    poison_no_overlap: Any,
    poison_overlap: Any,
    poison_read_fail: Any,
    authority_attempts: Any,
    sidecar_policy: Dict[str, Any],
) -> Dict[str, Any]:
    """Close poison admission against the lane that actually scanned it.

    Report-only O0/O1a sidecars preserve the legacy all-attempt SafeCopy
    partition, so accepted poison work is exactly its no-overlap bucket. With
    sidecars disabled, production O1 scans only the immutable 1/127 evidence
    cohort; the exact admission partition is therefore authority attempts =
    accepted + legacy evidence overlap/read-failure rejects.
    """
    policy_value = sidecar_policy.get("value")
    policy_exact = bool(
        sidecar_policy.get("contractClosed") is True
        and isinstance(policy_value, int)
        and policy_value in GPU_SKIN_POISON_SIDECAR_POLICIES.values()
    )
    inputs_present = bool(
        policy_exact
        and all(
            isinstance(value, int) and value >= 0
            for value in (
                accepted_with_poison, poison_no_overlap, poison_overlap,
                poison_read_fail, authority_attempts,
            )
        )
    )
    sidecar_enabled = bool(policy_value) if policy_exact else None
    if inputs_present and sidecar_enabled:
        expected = poison_no_overlap
        observed = accepted_with_poison
        contract = "legacy-all-scan-accepted-equals-no-overlap"
    elif inputs_present:
        expected = authority_attempts
        observed = (
            accepted_with_poison + poison_overlap + poison_read_fail
        )
        contract = (
            "production-o1-authority-attempts-equals-accepted-plus-"
            "evidence-rejects"
        )
    else:
        expected = None
        observed = None
        contract = "unprovable"
    return {
        "exact": bool(inputs_present and observed == expected),
        "contract": contract,
        "policyExact": policy_exact,
        "sidecarEnabled": sidecar_enabled,
        "observed": observed,
        "expected": expected,
        "authorityAttempts": authority_attempts,
        "acceptedWithPoison": accepted_with_poison,
        "legacyNoOverlap": poison_no_overlap,
        "legacyOverlap": poison_overlap,
        "legacyReadFail": poison_read_fail,
    }


def _outside_poison_scan_authority_contract(
    poison_scan: Any,
    authority_attempts: Any,
    evidence_attempts: Any,
    sidecar_policy: Dict[str, Any],
) -> Dict[str, Any]:
    """Bind each cumulative endpoint to the exact legacy-scan producer."""
    policy_value = sidecar_policy.get("value")
    policy_exact = bool(
        sidecar_policy.get("contractClosed") is True
        and isinstance(policy_value, int)
        and policy_value in GPU_SKIN_POISON_SIDECAR_POLICIES.values()
    )
    inputs_present = bool(
        policy_exact
        and all(
            isinstance(value, int) and value >= 0
            for value in (
                poison_scan, authority_attempts, evidence_attempts,
            )
        )
    )
    sidecar_enabled = bool(policy_value) if policy_exact else None
    expected = (
        authority_attempts if inputs_present and sidecar_enabled
        else evidence_attempts if inputs_present
        else None
    )
    return {
        "exact": bool(inputs_present and poison_scan == expected),
        "contract": (
            "sidecar-all-authority-attempts"
            if inputs_present and sidecar_enabled
            else "production-evidence-attempts"
            if inputs_present
            else "unprovable"
        ),
        "policyExact": policy_exact,
        "sidecarEnabled": sidecar_enabled,
        "observed": poison_scan,
        "expected": expected,
        "authorityAttempts": authority_attempts,
        "evidenceAttempts": evidence_attempts,
    }


def _native_poison_o1_authority_synthetic_self_tests() -> Dict[str, Any]:
    """Pure O1 authority parser/partition fixtures; never launches War3."""
    def snapshot(
        attempts: int = 0, authority: int = 0, retained: int = 0,
        evidence_attempts: int = 0, matrix: Optional[List[int]] = None,
        evidence_unprovable: int = 0, evidence_mismatches: int = 0,
        lock_rejects: int = 0, active: int = 0,
    ) -> Dict[str, Any]:
        matrix = list(matrix if matrix is not None else [0] * 9)
        comparable = sum(matrix)
        armed = authority + retained
        lock_no_overlap = armed - lock_rejects
        main = (
            "DXVK War3GpuSkin: diag nativePoisonO1Authority "
            f"attempt={attempts} created={attempts} overflow=0 armed={armed} "
            f"active={active} settled={attempts - active} cancel=0 resetAbort=0 "
            f"lock={armed}/{lock_no_overlap}/0/{lock_rejects} "
            f"kernel={armed}/0/{armed} unlock={armed}/{armed}/0 "
            f"commit={authority}/0/{retained}/0/{authority} "
            "closure=1/1/1/1/1/1"
        )
        evidence = (
            "DXVK War3GpuSkin: diag nativePoisonO1AuthorityEvidence "
            f"attempt={evidence_attempts} comparable={comparable} "
            f"unprovable={evidence_unprovable} "
            f"mismatch={evidence_mismatches} "
            "authority=0 legacyAuthority=0 "
            f"matrix={'/'.join(str(value) for value in matrix)}"
        )
        return _parse_native_poison_o1_authority(main, evidence)

    zero = snapshot()
    production = snapshot(
        attempts=254, authority=252, retained=2,
        evidence_attempts=2, matrix=[2] + [0] * 8,
    )
    sidecar = snapshot(
        attempts=254, authority=0, retained=254,
        evidence_attempts=2, matrix=[2] + [0] * 8,
    )
    sidecar_with_unarmed_legacy_rejects = snapshot(
        attempts=254, authority=0, retained=251,
        evidence_attempts=2, matrix=[1, 0, 0, 1, 0, 0, 0, 0, 0],
    )
    exact_discard = snapshot(
        attempts=254, authority=252, retained=2,
        evidence_attempts=2, matrix=[1, 0, 0, 1, 0, 0, 0, 0, 0],
    )
    bad_mismatch = snapshot(
        attempts=254, authority=252, retained=2,
        evidence_attempts=2, matrix=[2] + [0] * 8,
        evidence_mismatches=1,
    )
    bad_reject = snapshot(
        attempts=254, authority=252, retained=2,
        evidence_attempts=2, matrix=[2] + [0] * 8,
        lock_rejects=1,
    )
    bad_cadence = snapshot(
        attempts=254, authority=251, retained=3,
        evidence_attempts=3, matrix=[3] + [0] * 8,
    )
    unexpected_non_evidence_scan = snapshot(
        attempts=254, authority=251, retained=3,
        evidence_attempts=2, matrix=[2] + [0] * 8,
    )
    unexpected_non_evidence_scan_policy = (
        _native_poison_o1_authority_pair_policy(
            zero, unexpected_non_evidence_scan,
            False, True, False, 3, 3,
        )
    )
    none_policy = {"contractClosed": True, "value": 0}
    both_policy = {"contractClosed": True, "value": 3}
    production_admission = _outside_poison_accepted_partition_contract(
        500525, 3936, 5, 0, 500530, none_policy,
    )
    stale_production_admission = _outside_poison_accepted_partition_contract(
        3936, 3936, 5, 0, 500530, none_policy,
    )
    sidecar_admission = _outside_poison_accepted_partition_contract(
        500525, 500525, 5, 0, 500530, both_policy,
    )
    wrong_sidecar_admission = _outside_poison_accepted_partition_contract(
        500525, 3936, 5, 0, 500530, both_policy,
    )
    production_scan_endpoint = _outside_poison_scan_authority_contract(
        3941, 500530, 3941, none_policy,
    )
    sidecar_scan_endpoint = _outside_poison_scan_authority_contract(
        500530, 500530, 3941, both_policy,
    )
    persistent_baseline = snapshot(
        attempts=127, authority=126, retained=1,
        evidence_attempts=1, matrix=[1] + [0] * 8,
    )
    persistent_current = snapshot(
        attempts=254, authority=252, retained=2,
        evidence_attempts=2, matrix=[2] + [0] * 8,
    )
    persistent_pair_only = _native_poison_o1_authority_pair_policy(
        persistent_baseline, persistent_current,
        False, True, False, 1, 1,
    )
    persistent_endpoint = _outside_poison_scan_authority_contract(
        3, persistent_current.get("attempts"),
        persistent_current.get("evidence", {}).get("attempts"),
        none_policy,
    )
    checks = {
        "zeroParsed": zero.get("contractClosed") is True,
        "fullZeroExact": _native_poison_o1_authority_pair_policy(
            zero, zero, True, False, False,
        ).get("exact") is True,
        "productionExact": _native_poison_o1_authority_pair_policy(
            zero, production, False, True, False, 2, 2,
        ).get("exact") is True,
        "sidecarRetainsExact": _native_poison_o1_authority_pair_policy(
            zero, sidecar, False, True, True, 254, 254,
        ).get("exact") is True,
        "sidecarUnarmedLegacyRejectsExact": (
            _native_poison_o1_authority_pair_policy(
                zero, sidecar_with_unarmed_legacy_rejects,
                False, True, True, 254, 250,
            ).get("exact") is True
        ),
        "exactDiscardEvidenceAllowed": _native_poison_o1_authority_pair_policy(
            zero, exact_discard, False, True, False, 2, 1,
        ).get("exact") is True,
        "mismatchRejected": _native_poison_o1_authority_pair_policy(
            zero, bad_mismatch, False, True, False, 2, 2,
        ).get("exact") is not True,
        "lockRejectRejected": _native_poison_o1_authority_pair_policy(
            zero, bad_reject, False, True, False, 2, 2,
        ).get("exact") is not True,
        "productionRequiresAuthority": _native_poison_o1_authority_pair_policy(
            zero, sidecar, False, True, False, 2, 2,
        ).get("exact") is not True,
        "sidecarForbidsAuthority": _native_poison_o1_authority_pair_policy(
            zero, production, False, True, True, 254, 254,
        ).get("exact") is not True,
        "cadenceMismatchRejected": _native_poison_o1_authority_pair_policy(
            zero, bad_cadence, False, True, False, 3, 3,
        ).get("exact") is not True,
        "unexpectedNonEvidenceScanRejected": bool(
            unexpected_non_evidence_scan_policy.get(
                "scanPartitionExact"
            ) is False
            and unexpected_non_evidence_scan_policy.get("exact") is not True
        ),
        "productionAdmissionUsesAuthorityPartition": bool(
            production_admission.get("exact") is True
            and production_admission.get("observed") == 500530
            and production_admission.get("expected") == 500530
        ),
        "staleProductionAdmissionRejected": (
            stale_production_admission.get("exact") is not True
        ),
        "sidecarAdmissionUsesLegacyPartition": (
            sidecar_admission.get("exact") is True
        ),
        "sidecarCannotUseSampledPartition": (
            wrong_sidecar_admission.get("exact") is not True
        ),
        "productionEndpointScanExact": (
            production_scan_endpoint.get("exact") is True
        ),
        "sidecarEndpointScanExact": (
            sidecar_scan_endpoint.get("exact") is True
        ),
        "persistentBaselineMismatchNeedsEndpointGate": bool(
            persistent_pair_only.get("exact") is True
            and persistent_endpoint.get("exact") is not True
        ),
    }
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "native poison O1 authority synthetic self-test failed: "
            + json.dumps(result, sort_keys=True)
        )
    return result


def _parse_native_poison_o1_shadow(
    main_line: str, reasons_line: str, scanner_line: str = "",
    scanner_reasons_line: str = "",
    lane_lines: Optional[Dict[str, str]] = None,
    physical_line: str = "",
    discard_joint_line: str = "",
) -> Dict[str, Any]:
    """Parse O1a-v2's Lock-logical and post-Unlock physical partitions."""
    scalar_fields = (
        ("attempts", "attempt"),
        ("created", "created"),
        ("overflow", "overflow"),
        ("active", "active"),
        ("lockNotifications", "locks"),
        ("kernelNotifications", "kernels"),
        ("unlockNotifications", "unlocks"),
        ("frozen", "frozen"),
        ("settled", "settled"),
        ("cancelled", "cancel"),
        ("resetAborted", "resetAbort"),
        ("comparable", "comparable"),
        ("unprovable", "unprovable"),
        ("comparisonMissing", "comparisonMissing"),
        ("wouldClear", "wouldClear"),
        ("authority", "authority"),
        ("offDiagonal", "offdiag"),
        ("legacyMissedOverlap", "legacyMissedOverlap"),
    )
    scalars = {
        output_name: _match_named_int(main_line, log_name)
        for output_name, log_name in scalar_fields
    }
    matrix_flat = _match_tuple(
        main_line,
        r"\bmatrix=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/"
        r"(\d+)/(\d+)/(\d+)",
        9,
    )
    closure_values = _match_tuple(
        main_line, r"\bclosure=(\d+)/(\d+)/(\d+)", 3,
    )
    reasons = {
        output_name: _match_named_int(reasons_line, log_name)
        for output_name, log_name in NATIVE_POISON_O1_SHADOW_REASON_FIELDS
    }
    discard_joint = {
        name: _match_named_int(discard_joint_line, name)
        for name in NATIVE_POISON_O1_DISCARD_JOINT_FIELDS
    }
    discard_joint_closure = _match_tuple(
        discard_joint_line, r"\bclosure=(\d+)", 1,
    )[0]
    discard_joint_authority = _match_named_int(
        discard_joint_line, "authority"
    )

    scanner_fields = (
        ("calls", "calls"),
        ("noOverlap", "N"),
        ("overlap", "O"),
        ("readFailure", "R"),
        ("differentTarget", "differentTarget"),
        ("logicalExact", "logicalExact"),
        ("storageDiagnosticMismatch", "storageDiagnosticMismatch"),
        ("realStorageDrift", "realDrift"),
        ("mappingStorageDrift", "mappingDrift"),
        ("mapAllocationDiagnosticMismatch", "mapAllocationMismatch"),
        ("wouldClear", "wouldClear"),
    )
    scanner = {
        output_name: _match_named_int(scanner_line, log_name)
        for output_name, log_name in scanner_fields
    }
    scanner_closure = _match_tuple(
        scanner_line, r"\bclosure=(\d+)", 1,
    )[0]
    scanner_failure_reasons = {
        name: _match_named_int(scanner_reasons_line, name)
        for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
    }

    lane_lines = dict(lane_lines or {})
    lanes: Dict[str, Dict[str, Any]] = {}
    for lane_name in NATIVE_POISON_O1_LOCK_LANES:
        line = lane_lines.get(lane_name, "")
        lane_match = re.search(r"\blane=([A-Za-z0-9_]+)", line)
        reason_values = _match_tuple(
            line,
            r"\breasons=(\d+)/(\d+)/(\d+)/(\d+)/"
            r"(\d+)/(\d+)/(\d+)/(\d+)",
            len(NATIVE_POISON_O1_SCAN_FAILURE_FIELDS),
        )
        lanes[lane_name] = {
            "reportedName": lane_match.group(1) if lane_match else None,
            "calls": _match_named_int(line, "calls"),
            "noOverlap": _match_named_int(line, "N"),
            "overlap": _match_named_int(line, "O"),
            "readFailure": _match_named_int(line, "R"),
            "preLockMutation": _match_named_int(line, "preLockMutation"),
            "lockToKernelMutation": _match_named_int(
                line, "lockToKernelMutation"
            ),
            "failureReasons": dict(zip(
                NATIVE_POISON_O1_SCAN_FAILURE_FIELDS, reason_values,
            )),
            "raw": line,
        }

    physical_verdicts = {
        "noOverlap": _match_named_int(physical_line, "physicalN"),
        "overlap": _match_named_int(physical_line, "physicalO"),
        "notReady": _match_named_int(physical_line, "notReady"),
    }
    unlock_drift_values = _match_tuple(
        physical_line,
        r"\bunlockDrift=(\d+)/(\d+)/(\d+)/(\d+)",
        len(NATIVE_POISON_O1_UNLOCK_COMPONENTS),
    )
    unlock_hard_values = _match_tuple(
        physical_line,
        r"\bunlockHardFirst=(\d+)/(\d+)/(\d+)/(\d+)",
        len(NATIVE_POISON_O1_UNLOCK_COMPONENTS),
    )
    unlock_drifts = dict(zip(
        NATIVE_POISON_O1_UNLOCK_COMPONENTS, unlock_drift_values,
    ))
    unlock_hard_first = dict(zip(
        NATIVE_POISON_O1_UNLOCK_COMPONENTS, unlock_hard_values,
    ))
    physical_closure_values = _match_tuple(
        physical_line, r"\bclosure=(\d+)/(\d+)", 2,
    )
    logical_matrix_only = _match_named_int(
        physical_line, "logicalMatrixOnly"
    )
    physical_authority = _match_named_int(physical_line, "authority")

    lane_scalar_values: List[Optional[int]] = []
    lane_reason_values: List[Optional[int]] = []
    lane_names_exact = True
    for lane_name, lane in lanes.items():
        lane_names_exact = bool(
            lane_names_exact and lane.get("reportedName") == lane_name
        )
        lane_scalar_values.extend(
            lane.get(name)
            for name in (
                "calls", "noOverlap", "overlap", "readFailure",
                "preLockMutation", "lockToKernelMutation",
            )
        )
        lane_reason_values.extend(
            lane["failureReasons"].get(name)
            for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
        )
    values_present = all(
        isinstance(value, int)
        for value in (
            *scalars.values(), *matrix_flat, *closure_values,
            *reasons.values(), *discard_joint.values(),
            discard_joint_closure, discard_joint_authority,
            *scanner.values(), scanner_closure,
            *scanner_failure_reasons.values(), *lane_scalar_values,
            *lane_reason_values, *physical_verdicts.values(),
            *unlock_drifts.values(), *unlock_hard_first.values(),
            *physical_closure_values, logical_matrix_only,
            physical_authority,
        )
    ) and lane_names_exact
    matrix_total = sum(matrix_flat) if values_present else None
    reason_total = sum(reasons.values()) if values_present else None
    off_diagonal_computed = (
        sum(matrix_flat[index] for index in (1, 2, 3, 5, 6, 7))
        if values_present else None
    )
    overlap_column = (
        sum(matrix_flat[index] for index in (1, 4, 7))
        if values_present else None
    )
    matrix = {
        old_state: dict(zip(
            NATIVE_POISON_SHADOW_STATES,
            matrix_flat[row * 3:(row + 1) * 3],
        ))
        for row, old_state in enumerate(NATIVE_POISON_SHADOW_STATES)
    }

    lane_calls_total = None
    lane_state_totals: Dict[str, Optional[int]] = {
        state: None for state in NATIVE_POISON_SHADOW_STATES
    }
    lane_failure_totals: Dict[str, Optional[int]] = {
        name: None for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
    }
    lane_closures: Dict[str, bool] = {
        lane_name: False for lane_name in NATIVE_POISON_O1_LOCK_LANES
    }
    if values_present:
        lane_calls_total = sum(lane["calls"] for lane in lanes.values())
        for state, field in zip(
            NATIVE_POISON_SHADOW_STATES,
            ("noOverlap", "overlap", "readFailure"),
        ):
            lane_state_totals[state] = sum(
                lane[field] for lane in lanes.values()
            )
        for reason_name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS:
            lane_failure_totals[reason_name] = sum(
                lane["failureReasons"][reason_name]
                for lane in lanes.values()
            )
        for lane_name, lane in lanes.items():
            lane_failure_total = sum(lane["failureReasons"].values())
            lane_closures[lane_name] = bool(
                lane["calls"] == lane["noOverlap"] + lane["overlap"]
                    + lane["readFailure"]
                and lane["readFailure"] == lane_failure_total
            )

    physical_total = (
        sum(physical_verdicts.values()) if values_present else None
    )
    unlock_hard_total = (
        sum(unlock_hard_first.values()) if values_present else None
    )
    scanner_failure_total = (
        sum(scanner_failure_reasons.values()) if values_present else None
    )
    scanner_bounds_clean = bool(
        values_present
        and scanner["calls"] <= scalars["created"]
        and scanner["calls"] <= scalars["lockNotifications"]
        and scalars["frozen"] <= scalars["kernelNotifications"]
        and sum(
            lane["preLockMutation"] for lane in lanes.values()
        ) <= scalars["lockNotifications"]
        and sum(
            lane["lockToKernelMutation"] for lane in lanes.values()
        ) <= scalars["frozen"]
        and scanner["differentTarget"] <= scanner["calls"]
        and scanner["storageDiagnosticMismatch"] <= scanner["calls"]
        and scanner["realStorageDrift"] <=
            scanner["storageDiagnosticMismatch"]
        and scanner["mappingStorageDrift"] <=
            scanner["storageDiagnosticMismatch"]
        and scanner["mapAllocationDiagnosticMismatch"] <=
            scanner["storageDiagnosticMismatch"]
    )
    computed_closure = {
        "attempt": bool(
            values_present
            and scalars["attempts"] ==
                scalars["created"] + scalars["overflow"]
        ),
        "lifetime": bool(
            values_present
            and scalars["created"] ==
                scalars["settled"] + scalars["cancelled"]
                + scalars["resetAborted"] + scalars["active"]
        ),
        "settlement": bool(
            values_present
            and scalars["comparable"] == scanner["calls"]
            and scalars["comparable"] ==
                matrix_total + scalars["comparisonMissing"]
            and scalars["comparisonMissing"] == 0
            and scalars["unprovable"] == reason_total
            and scalars["unprovable"] <= scalars["settled"]
            and scalars["wouldClear"] == scanner["overlap"]
            and scalars["wouldClear"] == overlap_column
            and scanner["noOverlap"] == sum(
                matrix_flat[index] for index in (0, 3, 6)
            )
            and scanner["readFailure"] == sum(
                matrix_flat[index] for index in (2, 5, 8)
            )
            and scalars["authority"] == 0
        ),
        "discardJoint": bool(
            values_present
            and discard_joint["oldOToN"] == matrix_flat[3]
            and discard_joint["oldOToN"] ==
                discard_joint["exactDiscard"]
                + discard_joint["otherOldOToN"]
            and discard_joint["otherOldOToN"] == 0
            and discard_joint["exactDiscard"] <=
                discard_joint["discardOldOverlapRetired"]
            and discard_joint["discardOldOverlapRetired"] <=
                discard_joint["discardNotifications"]
            and discard_joint["exactDiscard"] <=
                discard_joint["oldOverlapFrozen"]
            and discard_joint_authority == scalars["authority"] == 0
        ),
        "scanner": bool(
            values_present and all(lane_closures.values())
            and scanner["calls"] == lane_calls_total
            and scanner["noOverlap"] ==
                lane_state_totals["noOverlap"]
            and scanner["overlap"] == lane_state_totals["overlap"]
            and scanner["readFailure"] ==
                lane_state_totals["readFailure"]
            and scanner["calls"] == scanner["noOverlap"]
                + scanner["overlap"] + scanner["readFailure"]
            and scanner["readFailure"] == scanner_failure_total
            and scanner["logicalExact"] ==
                scanner["noOverlap"] + scanner["overlap"]
            and all(
                scanner_failure_reasons[name] ==
                    lane_failure_totals[name]
                for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
            )
            and scanner["wouldClear"] == scanner["overlap"]
            and scanner_bounds_clean
        ),
        "unlockDrift": bool(
            values_present
            and unlock_hard_total == reasons["unlockGeneration"]
            and unlock_hard_first["real"] == 0
            and unlock_hard_first["mapping"] == 0
            and all(
                unlock_hard_first[name] <= unlock_drifts[name]
                for name in NATIVE_POISON_O1_UNLOCK_COMPONENTS
            )
        ),
        "physical": bool(
            values_present and physical_total == scalars["settled"]
            and physical_verdicts["noOverlap"] <= scanner["noOverlap"]
            and physical_verdicts["overlap"] <= scanner["overlap"]
        ),
        "offDiagonal": bool(
            values_present
            and scalars["offDiagonal"] == off_diagonal_computed
        ),
        "legacyMissedOverlap": bool(
            values_present
            and scalars["legacyMissedOverlap"] == matrix_flat[1]
        ),
    }
    reported_closure = dict(zip(
        ("attempt", "lifetime", "settlement"), closure_values,
    ))
    reported_closure_clean = bool(
        values_present and closure_values == [1, 1, 1]
        and discard_joint_closure == 1
        and scanner_closure == 1
        and physical_closure_values == [1, 1]
        and logical_matrix_only == 1
        and discard_joint_authority == physical_authority ==
            scalars["authority"] == 0
    )
    cold_contract_closed = bool(
        values_present and scalars["active"] == 0
        and closure_values[0] in (0, 1)
        and closure_values[1:] == [1, 1]
        and discard_joint_closure == 1
        and scanner_closure == 1
        and physical_closure_values == [1, 1]
        and logical_matrix_only == 1
        and discard_joint_authority == physical_authority ==
            scalars["authority"] == 0
        and all(computed_closure.values())
    )
    contract_closed = bool(
        values_present and reported_closure_clean
        and all(computed_closure.values())
    )
    cumulative_main_names = (
        "attempts", "created", "overflow", "lockNotifications",
        "kernelNotifications", "unlockNotifications", "frozen",
        "settled", "cancelled", "resetAborted", "comparable",
        "unprovable", "comparisonMissing", "wouldClear", "offDiagonal",
        "legacyMissedOverlap",
    )
    cumulative_flat: Dict[str, int] = {}
    if values_present:
        cumulative_flat.update({
            f"main.{name}": scalars[name]
            for name in cumulative_main_names
        })
        cumulative_flat.update({
            f"matrix.{index}": value
            for index, value in enumerate(matrix_flat)
        })
        cumulative_flat.update({
            f"unprovable.{name}": value
            for name, value in reasons.items()
        })
        cumulative_flat.update({
            f"discardJoint.{name}": value
            for name, value in discard_joint.items()
        })
        cumulative_flat.update({
            f"scanner.{name}": value
            for name, value in scanner.items()
        })
        cumulative_flat.update({
            f"scannerReason.{name}": value
            for name, value in scanner_failure_reasons.items()
        })
        for lane_name, lane in lanes.items():
            cumulative_flat.update({
                f"lane.{lane_name}.{name}": lane[name]
                for name in (
                    "calls", "noOverlap", "overlap", "readFailure",
                    "preLockMutation", "lockToKernelMutation",
                )
            })
            cumulative_flat.update({
                f"laneReason.{lane_name}.{name}": value
                for name, value in lane["failureReasons"].items()
            })
        cumulative_flat.update({
            f"physical.{name}": value
            for name, value in physical_verdicts.items()
        })
        cumulative_flat.update({
            f"unlockDrift.{name}": value
            for name, value in unlock_drifts.items()
        })
        cumulative_flat.update({
            f"unlockHardFirst.{name}": value
            for name, value in unlock_hard_first.items()
        })
    return {
        **scalars,
        "matrix": matrix,
        "matrixFlat": matrix_flat,
        "matrixAxis": {
            "rows": "legacyOuterSafeCopyVerdict",
            "columns": "successfulLockLogicalVerdict",
            "order": list(NATIVE_POISON_SHADOW_STATES),
        },
        "matrixTotal": matrix_total,
        "overlapColumnTotal": overlap_column,
        "unprovableReasons": reasons,
        "unprovableReasonTotal": reason_total,
        "discardJoint": {
            **discard_joint,
            "reportedClosure": discard_joint_closure,
            "authority": discard_joint_authority,
        },
        "scanner": {
            **scanner,
            "failureReasons": scanner_failure_reasons,
            "failureReasonTotal": scanner_failure_total,
            "reportedClosure": scanner_closure,
            "laneTotals": {
                "calls": lane_calls_total,
                **lane_state_totals,
                "failureReasons": lane_failure_totals,
            },
            "boundsClean": scanner_bounds_clean,
        },
        "lanes": lanes,
        "laneNamesExact": lane_names_exact,
        "physical": {
            "logicalMatrixOnly": logical_matrix_only,
            "verdicts": physical_verdicts,
            "total": physical_total,
            "unlockDrifts": unlock_drifts,
            "unlockHardFirst": unlock_hard_first,
            "unlockHardFirstTotal": unlock_hard_total,
            "reportedClosure": dict(zip(
                ("unlockDrift", "physical"), physical_closure_values,
            )),
            "authority": physical_authority,
        },
        "reportedClosure": reported_closure,
        "computedClosure": computed_closure,
        "cumulativeFlat": cumulative_flat,
        "present": values_present,
        "contractClosed": contract_closed,
        "coldContractClosed": bool(
            cold_contract_closed and scalars["active"] == 0
            and all(value == 0 for value in cumulative_flat.values())
        ),
        "allCountersZero": bool(
            values_present and scalars["active"] == 0
            and all(value == 0 for value in cumulative_flat.values())
        ),
        "endpointClean": bool(
            contract_closed and scalars["active"] == 0
            and scalars["authority"] == 0
            and scalars["legacyMissedOverlap"] == 0
        ),
        "authorizationAuthority": scalars["authority"],
        "reportOnly": True,
        "independentVerdictRequired": True,
        "raw": {
            "main": main_line,
            "reasons": reasons_line,
            "discardJoint": discard_joint_line,
            "scanner": scanner_line,
            "scannerReasons": scanner_reasons_line,
            "lanes": lane_lines,
            "physical": physical_line,
        },
    }


def _native_poison_o1_shadow_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    cumulative_names = (
        "attempts", "created", "overflow", "lockNotifications",
        "kernelNotifications", "unlockNotifications", "frozen",
        "settled", "cancelled", "resetAborted", "comparable",
        "unprovable", "comparisonMissing", "wouldClear", "offDiagonal",
        "legacyMissedOverlap",
    )
    previous_flat = dict(previous.get("cumulativeFlat", {}) or {})
    current_flat = dict(current.get("cumulativeFlat", {}) or {})
    reason_names = tuple(
        output_name
        for output_name, _ in NATIVE_POISON_O1_SHADOW_REASON_FIELDS
    )
    present = bool(
        previous.get("present") is True
        and current.get("present") is True
        and previous.get("contractClosed") is True
        and current.get("contractClosed") is True
        and previous.get("authority") == 0
        and current.get("authority") == 0
        and previous.get("physical", {}).get("authority") == 0
        and current.get("physical", {}).get("authority") == 0
        and previous_flat and previous_flat.keys() == current_flat.keys()
        and all(
            isinstance(previous.get(name), int)
            and isinstance(current.get(name), int)
            for name in cumulative_names + ("active",)
        )
        and all(isinstance(value, int) for value in previous_flat.values())
        and all(isinstance(value, int) for value in current_flat.values())
    )
    monotonic = bool(
        present
        and all(
            current_flat[name] >= previous_flat[name]
            for name in previous_flat
        )
    )
    if not monotonic:
        return {
            "present": present,
            "monotonic": False,
            "valid": False,
            "reason": "missing, invalid, or non-monotonic O1 shadow counters",
            "authorizationAuthority": 0,
            "reportOnly": True,
        }
    flat_delta = {
        name: current_flat[name] - previous_flat[name]
        for name in previous_flat
    }
    deltas = {name: flat_delta[f"main.{name}"] for name in cumulative_names}
    matrix_delta = [flat_delta[f"matrix.{index}"] for index in range(9)]
    reasons_delta = {
        name: flat_delta[f"unprovable.{name}"]
        for name in reason_names
    }
    discard_joint_delta = {
        name: flat_delta[f"discardJoint.{name}"]
        for name in NATIVE_POISON_O1_DISCARD_JOINT_FIELDS
    }
    scanner_names = (
        "calls", "noOverlap", "overlap", "readFailure",
        "differentTarget", "logicalExact", "storageDiagnosticMismatch",
        "realStorageDrift", "mappingStorageDrift",
        "mapAllocationDiagnosticMismatch", "wouldClear",
    )
    scanner_delta = {
        name: flat_delta[f"scanner.{name}"] for name in scanner_names
    }
    scanner_reason_delta = {
        name: flat_delta[f"scannerReason.{name}"]
        for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
    }
    lanes_delta: Dict[str, Dict[str, Any]] = {}
    for lane_name in NATIVE_POISON_O1_LOCK_LANES:
        lane = {
            name: flat_delta[f"lane.{lane_name}.{name}"]
            for name in (
                "calls", "noOverlap", "overlap", "readFailure",
                "preLockMutation", "lockToKernelMutation",
            )
        }
        lane["failureReasons"] = {
            name: flat_delta[f"laneReason.{lane_name}.{name}"]
            for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
        }
        lanes_delta[lane_name] = lane
    physical_delta = {
        name: flat_delta[f"physical.{name}"]
        for name in ("noOverlap", "overlap", "notReady")
    }
    unlock_drift_delta = {
        name: flat_delta[f"unlockDrift.{name}"]
        for name in NATIVE_POISON_O1_UNLOCK_COMPONENTS
    }
    unlock_hard_delta = {
        name: flat_delta[f"unlockHardFirst.{name}"]
        for name in NATIVE_POISON_O1_UNLOCK_COMPONENTS
    }
    matrix_total = sum(matrix_delta)
    reason_total = sum(reasons_delta.values())
    off_diagonal = sum(
        matrix_delta[index] for index in (1, 2, 3, 5, 6, 7)
    )
    overlap_column = sum(matrix_delta[index] for index in (1, 4, 7))
    endpoints_quiescent = bool(
        previous["active"] == 0 and current["active"] == 0
    )
    lane_calls_total = sum(lane["calls"] for lane in lanes_delta.values())
    lane_state_totals = {
        name: sum(lane[name] for lane in lanes_delta.values())
        for name in ("noOverlap", "overlap", "readFailure")
    }
    lane_failure_totals = {
        name: sum(
            lane["failureReasons"][name] for lane in lanes_delta.values()
        )
        for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
    }
    lane_closures = {
        lane_name: bool(
            lane["calls"] == lane["noOverlap"] + lane["overlap"]
                + lane["readFailure"]
            and lane["readFailure"] == sum(lane["failureReasons"].values())
        )
        for lane_name, lane in lanes_delta.items()
    }
    scanner_bounds_clean = bool(
        scanner_delta["calls"] <= deltas["created"]
        and scanner_delta["calls"] <= deltas["lockNotifications"]
        and deltas["frozen"] <= deltas["kernelNotifications"]
        and sum(
            lane["preLockMutation"] for lane in lanes_delta.values()
        ) <= deltas["lockNotifications"]
        and sum(
            lane["lockToKernelMutation"] for lane in lanes_delta.values()
        ) <= deltas["frozen"]
        and scanner_delta["differentTarget"] <= scanner_delta["calls"]
        and scanner_delta["storageDiagnosticMismatch"] <=
            scanner_delta["calls"]
        and scanner_delta["realStorageDrift"] <=
            scanner_delta["storageDiagnosticMismatch"]
        and scanner_delta["mappingStorageDrift"] <=
            scanner_delta["storageDiagnosticMismatch"]
        and scanner_delta["mapAllocationDiagnosticMismatch"] <=
            scanner_delta["storageDiagnosticMismatch"]
    )
    physical_total = sum(physical_delta.values())
    unlock_hard_total = sum(unlock_hard_delta.values())
    computed_closure = {
        "attempt": deltas["attempts"] ==
            deltas["created"] + deltas["overflow"],
        "lifetime": (
            endpoints_quiescent
            and deltas["created"] ==
                deltas["settled"] + deltas["cancelled"]
                + deltas["resetAborted"]
        ),
        "settlement": (
            deltas["comparable"] == scanner_delta["calls"]
            and deltas["comparable"] ==
                matrix_total + deltas["comparisonMissing"]
            and deltas["comparisonMissing"] == 0
            and deltas["unprovable"] == reason_total
            and deltas["unprovable"] <= deltas["settled"]
            and deltas["wouldClear"] == scanner_delta["overlap"]
            and deltas["wouldClear"] == overlap_column
            and scanner_delta["noOverlap"] == sum(
                matrix_delta[index] for index in (0, 3, 6)
            )
            and scanner_delta["readFailure"] == sum(
                matrix_delta[index] for index in (2, 5, 8)
            )
        ),
        "discardJoint": (
            discard_joint_delta["oldOToN"] == matrix_delta[3]
            and discard_joint_delta["oldOToN"] ==
                discard_joint_delta["exactDiscard"]
                + discard_joint_delta["otherOldOToN"]
            and discard_joint_delta["otherOldOToN"] == 0
            and discard_joint_delta["exactDiscard"] <=
                discard_joint_delta["discardOldOverlapRetired"]
            and discard_joint_delta["discardOldOverlapRetired"] <=
                discard_joint_delta["discardNotifications"]
            and discard_joint_delta["exactDiscard"] <=
                discard_joint_delta["oldOverlapFrozen"]
        ),
        "scanner": (
            all(lane_closures.values())
            and scanner_delta["calls"] == lane_calls_total
            and scanner_delta["noOverlap"] ==
                lane_state_totals["noOverlap"]
            and scanner_delta["overlap"] == lane_state_totals["overlap"]
            and scanner_delta["readFailure"] ==
                lane_state_totals["readFailure"]
            and scanner_delta["calls"] == scanner_delta["noOverlap"]
                + scanner_delta["overlap"] + scanner_delta["readFailure"]
            and scanner_delta["readFailure"] ==
                sum(scanner_reason_delta.values())
            and scanner_delta["logicalExact"] ==
                scanner_delta["noOverlap"] + scanner_delta["overlap"]
            and all(
                scanner_reason_delta[name] == lane_failure_totals[name]
                for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
            )
            and scanner_delta["wouldClear"] == scanner_delta["overlap"]
            and scanner_bounds_clean
        ),
        "unlockDrift": (
            unlock_hard_total == reasons_delta["unlockGeneration"]
            and unlock_hard_delta["real"] == 0
            and unlock_hard_delta["mapping"] == 0
            and all(
                unlock_hard_delta[name] <= unlock_drift_delta[name]
                for name in NATIVE_POISON_O1_UNLOCK_COMPONENTS
            )
        ),
        "physical": (
            physical_total == deltas["settled"]
            and physical_delta["noOverlap"] <= scanner_delta["noOverlap"]
            and physical_delta["overlap"] <= scanner_delta["overlap"]
        ),
        "offDiagonal": deltas["offDiagonal"] == off_diagonal,
        "legacyMissedOverlap": (
            deltas["legacyMissedOverlap"] == matrix_delta[1]
        ),
    }
    valid = bool(endpoints_quiescent and all(computed_closure.values()))
    matrix = {
        old_state: dict(zip(
            NATIVE_POISON_SHADOW_STATES,
            matrix_delta[row * 3:(row + 1) * 3],
        ))
        for row, old_state in enumerate(NATIVE_POISON_SHADOW_STATES)
    }
    return {
        "present": True,
        "monotonic": True,
        "valid": valid,
        "endpointsQuiescent": endpoints_quiescent,
        "deltas": deltas,
        "matrix": matrix,
        "matrixFlat": matrix_delta,
        "matrixAxis": {
            "rows": "legacyOuterSafeCopyVerdict",
            "columns": "successfulLockLogicalVerdict",
            "order": list(NATIVE_POISON_SHADOW_STATES),
        },
        "matrixTotal": matrix_total,
        "overlapColumnTotal": overlap_column,
        "unprovableReasons": reasons_delta,
        "unprovableReasonTotal": reason_total,
        "discardJoint": discard_joint_delta,
        "scanner": {
            **scanner_delta,
            "failureReasons": scanner_reason_delta,
            "boundsClean": scanner_bounds_clean,
        },
        "lanes": lanes_delta,
        "physical": {
            "verdicts": physical_delta,
            "total": physical_total,
            "unlockDrifts": unlock_drift_delta,
            "unlockHardFirst": unlock_hard_delta,
            "unlockHardFirstTotal": unlock_hard_total,
        },
        "computedClosure": computed_closure,
        "cumulativeFlatDelta": flat_delta,
        "allDeltasZero": all(
            value == 0 for value in flat_delta.values()
        ),
        "authorizationAuthority": 0,
        "reportOnly": True,
    }


def _native_poison_o1_lifetime_policy(
    current: Dict[str, Any], fast_path: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    """Exact lifetime gate; old lane/mutation marginals remain diagnostic."""
    fast_path = dict(fast_path or {})
    fast_values = {
        "scan": fast_path.get("poisonScanAttempts"),
        "noOverlap": fast_path.get("poisonNoOverlap"),
        "overlap": fast_path.get("poisonOverlap"),
        "readFailure": fast_path.get("poisonReadFail"),
    }
    matrix = list(current.get("matrixFlat", []) or [])
    scanner = dict(current.get("scanner", {}) or {})
    lanes = dict(current.get("lanes", {}) or {})
    physical = dict(current.get("physical", {}).get("verdicts", {}) or {})
    discard_joint = dict(current.get("discardJoint", {}) or {})
    present = bool(
        current.get("present") is True
        and current.get("contractClosed") is True
        and len(matrix) == 9
        and all(isinstance(value, int) for value in fast_values.values())
        and all(
            lane_name in lanes
            for lane_name in NATIVE_POISON_O1_LOCK_LANES
        )
        and all(
            isinstance(scanner.get(name), int)
            for name in ("calls", "noOverlap", "overlap", "readFailure")
        )
        and all(
            isinstance(physical.get(name), int)
            for name in ("noOverlap", "overlap", "notReady")
        )
        and all(
            isinstance(discard_joint.get(name), int)
            for name in NATIVE_POISON_O1_DISCARD_JOINT_FIELDS
        )
        and isinstance(discard_joint.get("reportedClosure"), int)
        and isinstance(discard_joint.get("authority"), int)
    )
    if not present:
        return {
            "present": False,
            "exact": False,
            "overlapEvidenceExact": False,
            "reason": "missing O1 lifetime or production-fast counters",
        }

    row_totals = [sum(matrix[row * 3:(row + 1) * 3]) for row in range(3)]
    column_totals = [sum(matrix[index::3]) for index in range(3)]
    legacy_rows_exact = row_totals == [
        fast_values["noOverlap"], fast_values["overlap"],
        fast_values["readFailure"],
    ]
    exact_physical_rejects = (
        column_totals[0] - physical["noOverlap"]
        + column_totals[1] - physical["overlap"]
    )
    expected_not_ready = column_totals[2] + exact_physical_rejects
    unprovable = current.get("unprovable")
    physical_partition_exact = bool(
        exact_physical_rejects >= 0
        and physical["notReady"] == expected_not_ready
        and isinstance(unprovable, int)
        and exact_physical_rejects <= unprovable <=
            exact_physical_rejects + column_totals[2]
    )

    non_discard_overlap = (
        lanes["bufferNoOverwrite"]["overlap"]
        + lanes["directNoOverwrite"]["overlap"]
    )
    discard_calls = (
        lanes["bufferDiscard"]["calls"]
        + lanes["directDiscard"]["calls"]
    )
    outside_old_overlap_o = matrix[1] + matrix[7]
    outside_old_overlap_total = row_totals[0] + row_totals[2]
    outside_old_overlap_exact = (
        matrix[0] + matrix[1] + matrix[6] + matrix[7]
    )
    old_overlap_non_discard_o_lb = max(
        0, non_discard_overlap - outside_old_overlap_o,
    )
    old_overlap_discard_lb = max(
        0, discard_calls - outside_old_overlap_total,
    )
    old_overlap_physical_reject_lb = max(
        0, exact_physical_rejects - outside_old_overlap_exact,
    )
    old_overlap_logical_reject = matrix[5]
    discard_joint_exact = bool(
        discard_joint["oldOToN"] == matrix[3]
        and discard_joint["oldOToN"] ==
            discard_joint["exactDiscard"]
            + discard_joint["otherOldOToN"]
        and discard_joint["otherOldOToN"] == 0
        and discard_joint["exactDiscard"] <=
            discard_joint["discardOldOverlapRetired"]
        and discard_joint["discardOldOverlapRetired"] <=
            discard_joint["discardNotifications"]
        and discard_joint["exactDiscard"] <=
            discard_joint["oldOverlapFrozen"]
        and discard_joint["reportedClosure"] == 1
        and discard_joint["authority"] == 0
    )
    # Only the same-probe joint receipt can explain legacy O -> logical N.
    # O -> O is recognized overlap and O -> R remains a conservative reject.
    # The old lane/mutation marginal bounds below are report-only diagnostics.
    overlap_evidence_exact = discard_joint_exact
    normal_return_exact = bool(
        current.get("lockNotifications") == current.get("attempts")
        and current.get("kernelNotifications") == current.get("attempts")
        and current.get("unlockNotifications") == current.get("attempts")
        and current.get("frozen") == current.get("attempts")
    )
    base_exact = bool(
        current.get("attempts") == fast_values["scan"]
        and current.get("created") == fast_values["scan"]
        and current.get("settled") == fast_values["scan"]
        and scanner["calls"] == fast_values["scan"]
        and current.get("overflow") == 0
        and current.get("cancelled") == 0
        and current.get("resetAborted") == 0
        and current.get("active") == 0
        and current.get("comparisonMissing") == 0
        and current.get("legacyMissedOverlap") == 0
        and current.get("authorizationAuthority") == 0
        and fast_values["scan"] == fast_values["noOverlap"]
            + fast_values["overlap"] + fast_values["readFailure"]
        and legacy_rows_exact
        and normal_return_exact
        and physical_partition_exact
        and discard_joint_exact
    )
    return {
        "present": True,
        "exact": bool(base_exact and overlap_evidence_exact),
        "baseExact": base_exact,
        "normalReturnExact": normal_return_exact,
        "legacyRowsExact": legacy_rows_exact,
        "legacyFast": fast_values,
        "legacyRowTotals": dict(zip(
            NATIVE_POISON_SHADOW_STATES, row_totals,
        )),
        "logicalColumnTotals": dict(zip(
            NATIVE_POISON_SHADOW_STATES, column_totals,
        )),
        "physicalPartitionExact": physical_partition_exact,
        "postLockPhysicalRejects": exact_physical_rejects,
        "expectedNotReady": expected_not_ready,
        "discardJointExact": discard_joint_exact,
        "discardJoint": discard_joint,
        "overlapEvidenceExact": overlap_evidence_exact,
        "overlapCohort": fast_values["overlap"],
        "oldOverlapEvidence": {
            "nonDiscardExactOLowerBound": old_overlap_non_discard_o_lb,
            "discardOrExcludedLowerBound": old_overlap_discard_lb,
            "logicalReject": old_overlap_logical_reject,
            "physicalRejectLowerBound": old_overlap_physical_reject_lb,
        },
        "conservativeMarginalLowerBound": False,
        "sameProbeDiscardJointRequired": True,
    }


def _native_poison_o1_shadow_pair_policy(
    previous: Dict[str, Any], current: Dict[str, Any],
    poison_scan_delta: Optional[int], full: bool, light: bool,
    poison_no_overlap_delta: Optional[int] = None,
    poison_overlap_delta: Optional[int] = None,
    poison_read_fail_delta: Optional[int] = None,
    lifetime_fast_path: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    delta = _native_poison_o1_shadow_delta(previous, current)
    deltas = dict(delta.get("deltas", {}) or {})
    matrix = list(delta.get("matrixFlat", []) or [])
    scanner = dict(delta.get("scanner", {}) or {})
    physical = dict(delta.get("physical", {}).get("verdicts", {}) or {})
    discard_joint = dict(delta.get("discardJoint", {}) or {})
    lifetime = _native_poison_o1_lifetime_policy(
        current, lifetime_fast_path,
    )
    recognized_exact = bool(full != light)
    fast_delta_values = (
        poison_scan_delta, poison_no_overlap_delta, poison_overlap_delta,
        poison_read_fail_delta,
    )
    fast_delta_present = all(
        isinstance(value, int) for value in fast_delta_values
    )
    row_totals = (
        [sum(matrix[row * 3:(row + 1) * 3]) for row in range(3)]
        if len(matrix) == 9 else []
    )
    legacy_rows_exact = bool(
        fast_delta_present and len(row_totals) == 3
        and row_totals == [
            poison_no_overlap_delta, poison_overlap_delta,
            poison_read_fail_delta,
        ]
    )
    exact_physical_rejects = None
    expected_not_ready = None
    unprovable_partition_exact = False
    if all(
        isinstance(value, int)
        for value in (
            scanner.get("noOverlap"), scanner.get("overlap"),
            scanner.get("readFailure"), physical.get("noOverlap"),
            physical.get("overlap"), physical.get("notReady"),
            deltas.get("unprovable"),
        )
    ):
        exact_physical_rejects = (
            scanner["noOverlap"] - physical["noOverlap"]
            + scanner["overlap"] - physical["overlap"]
        )
        expected_not_ready = (
            scanner["readFailure"] + exact_physical_rejects
        )
        unprovable_partition_exact = bool(
            exact_physical_rejects >= 0
            and physical["notReady"] == expected_not_ready
            and exact_physical_rejects <= deltas["unprovable"] <=
                exact_physical_rejects + scanner["readFailure"]
        )
    full_exact = bool(
        full and not light and fast_delta_present
        and all(value == 0 for value in fast_delta_values)
        and delta.get("valid") is True
        and delta.get("allDeltasZero") is True
        and previous.get("allCountersZero") is True
        and current.get("allCountersZero") is True
        and previous.get("authority") == current.get("authority") == 0
        and lifetime.get("exact") is True
    )
    normal_return_exact = bool(
        deltas.get("lockNotifications") == poison_scan_delta
        and deltas.get("kernelNotifications") == poison_scan_delta
        and deltas.get("unlockNotifications") == poison_scan_delta
        and deltas.get("frozen") == poison_scan_delta
    )
    discard_joint_delta_present = all(
        isinstance(discard_joint.get(name), int)
        for name in NATIVE_POISON_O1_DISCARD_JOINT_FIELDS
    )
    discard_joint_exact = bool(
        len(matrix) == 9 and discard_joint_delta_present
        and discard_joint.get("oldOToN") == matrix[3]
        and discard_joint.get("oldOToN") ==
            discard_joint.get("exactDiscard", -1)
            + discard_joint.get("otherOldOToN", -1)
        and discard_joint.get("otherOldOToN") == 0
        and discard_joint.get("exactDiscard", -1) <=
            discard_joint.get("discardOldOverlapRetired", -1)
        and discard_joint.get("discardOldOverlapRetired", -1) <=
            discard_joint.get("discardNotifications", -1)
        and discard_joint.get("exactDiscard", -1) <=
            discard_joint.get("oldOverlapFrozen", -1)
    )
    light_exact = bool(
        light and not full
        and fast_delta_present and poison_scan_delta > 0
        and poison_scan_delta == poison_no_overlap_delta
            + poison_overlap_delta + poison_read_fail_delta
        and delta.get("valid") is True
        and len(matrix) == 9
        and deltas.get("attempts") == poison_scan_delta
        and deltas.get("created") == poison_scan_delta
        and deltas.get("settled") == poison_scan_delta
        and scanner.get("calls") == poison_scan_delta
        and deltas.get("overflow") == 0
        and deltas.get("cancelled") == 0
        and deltas.get("resetAborted") == 0
        and deltas.get("comparable") == poison_scan_delta
        and deltas.get("comparisonMissing") == 0
        and deltas.get("legacyMissedOverlap") == 0
        and normal_return_exact
        and discard_joint_exact
        and legacy_rows_exact
        and unprovable_partition_exact
        and physical.get("noOverlap", 0) + physical.get("overlap", 0) > 0
        and previous.get("active") == current.get("active") == 0
        and previous.get("authority") == current.get("authority") == 0
        and lifetime.get("exact") is True
    )
    return {
        "present": bool(
            previous.get("present") is True
            and current.get("present") is True
            and isinstance(poison_scan_delta, int)
        ),
        "recognizedExact": recognized_exact,
        "full": full,
        "light": light,
        "fullExact": full_exact,
        "lightExact": light_exact,
        "exact": bool(
            recognized_exact and (full_exact if full else light_exact)
        ),
        "poisonScanAttemptsDelta": poison_scan_delta,
        "legacyOutcomeDeltas": {
            "noOverlap": poison_no_overlap_delta,
            "overlap": poison_overlap_delta,
            "readFailure": poison_read_fail_delta,
        },
        "legacyRowsExact": legacy_rows_exact,
        "normalReturnExact": normal_return_exact,
        "discardJointExact": discard_joint_exact,
        "discardJointDelta": discard_joint,
        "unprovablePartitionExact": unprovable_partition_exact,
        "postLockPhysicalRejects": exact_physical_rejects,
        "expectedNotReady": expected_not_ready,
        "delta": delta,
        "lifetime": lifetime,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "promotionEligibleShadow": light_exact,
        "note": (
            "O1a-v2 freezes a logical N/O/R verdict at successful Lock, then "
            "separately classifies post-kernel/Unlock physical readiness. "
            "O->R and classified physical rejects remain fail-closed; O->N "
            "requires an exact same-probe DIRECT DISCARD retirement receipt."
        ),
    }


def _native_poison_shadow_policy_pair(
    previous: Dict[str, Any], current: Dict[str, Any],
    poison_scan_delta: Optional[int], full: bool, light: bool,
    enabled: bool,
) -> Dict[str, Any]:
    if full or enabled:
        result = _native_poison_shadow_pair_policy(
            previous, current, poison_scan_delta, full, light,
        )
        result["enabledBySidecarPolicy"] = enabled
        result["disabledCold"] = False
        return result
    previous_cold = dict(previous)
    current_cold = dict(current)
    previous_cold["contractClosed"] = (
        previous.get("coldContractClosed") is True
    )
    current_cold["contractClosed"] = (
        current.get("coldContractClosed") is True
    )
    delta = _native_poison_shadow_delta(previous_cold, current_cold)
    disabled_cold = bool(
        light and not full
        and delta.get("valid") is True
        and delta.get("allDeltasZero") is True
        and previous.get("coldContractClosed") is True
        and current.get("coldContractClosed") is True
        and previous.get("allCountersZero") is True
        and current.get("allCountersZero") is True
        and previous.get("authorizationAuthority") == 0
        and current.get("authorizationAuthority") == 0
    )
    return {
        "present": bool(
            previous.get("present") is True
            and current.get("present") is True
            and isinstance(poison_scan_delta, int)
        ),
        "recognizedExact": bool(full != light),
        "full": full,
        "light": light,
        "fullExact": False,
        "lightExact": disabled_cold,
        "exact": disabled_cold,
        "enabledBySidecarPolicy": False,
        "disabledCold": disabled_cold,
        "poisonScanAttemptsDelta": poison_scan_delta,
        "delta": delta,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "note": (
            "O0 is disabled by the frozen sidecar policy; every cumulative "
            "and clean-pair delta counter must remain numerically cold."
        ),
    }


def _native_poison_o1_shadow_policy_pair(
    previous: Dict[str, Any], current: Dict[str, Any],
    poison_scan_delta: Optional[int], full: bool, light: bool,
    poison_no_overlap_delta: Optional[int],
    poison_overlap_delta: Optional[int],
    poison_read_fail_delta: Optional[int],
    lifetime_fast_path: Optional[Dict[str, Any]],
    enabled: bool,
) -> Dict[str, Any]:
    if full or enabled:
        result = _native_poison_o1_shadow_pair_policy(
            previous, current, poison_scan_delta, full, light,
            poison_no_overlap_delta, poison_overlap_delta,
            poison_read_fail_delta, lifetime_fast_path,
        )
        result["enabledBySidecarPolicy"] = enabled
        result["disabledCold"] = False
        return result
    previous_cold = dict(previous)
    current_cold = dict(current)
    previous_cold["contractClosed"] = (
        previous.get("coldContractClosed") is True
    )
    current_cold["contractClosed"] = (
        current.get("coldContractClosed") is True
    )
    delta = _native_poison_o1_shadow_delta(previous_cold, current_cold)
    disabled_cold = bool(
        light and not full
        and delta.get("valid") is True
        and delta.get("allDeltasZero") is True
        and previous.get("coldContractClosed") is True
        and current.get("coldContractClosed") is True
        and previous.get("allCountersZero") is True
        and current.get("allCountersZero") is True
        and previous.get("authority") == current.get("authority") == 0
        and previous.get("authorizationAuthority") == 0
        and current.get("authorizationAuthority") == 0
    )
    return {
        "present": bool(
            previous.get("present") is True
            and current.get("present") is True
            and isinstance(poison_scan_delta, int)
        ),
        "recognizedExact": bool(full != light),
        "full": full,
        "light": light,
        "fullExact": False,
        "lightExact": disabled_cold,
        "exact": disabled_cold,
        "enabledBySidecarPolicy": False,
        "disabledCold": disabled_cold,
        "poisonScanAttemptsDelta": poison_scan_delta,
        "delta": delta,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "promotionEligibleShadow": False,
        "note": (
            "O1 is disabled by the frozen sidecar policy; every raw, Lock, "
            "Unlock, scanner, joint, physical, and clean-pair delta counter "
            "must remain numerically cold."
        ),
    }


def _native_poison_sidecar_policy_synthetic_self_tests() -> Dict[str, Any]:
    checks: Dict[str, bool] = {}

    def policy_line(name: str, **overrides: int) -> str:
        value = GPU_SKIN_POISON_SIDECAR_POLICIES[name]
        fields = {
            "value": value,
            "o0": 1 if value & 1 else 0,
            "o1": 1 if value & 2 else 0,
            "explicit": 1,
            "invalid": 0,
            "parse": 1,
            "closure": 1,
            "authority": 0,
        }
        fields.update(overrides)
        return (
            "DXVK War3GpuSkin: diag nativePoisonSidecarPolicy "
            f"policy={name} "
            + " ".join(f"{key}={value}" for key, value in fields.items())
        )

    zero_o0 = _parse_native_poison_shadow(
        "DXVK War3GpuSkin: diag nativePoisonShadow attempt=0 created=0 "
        "overflow=0 active=0 locks=0 settled=0 cancel=0 resetAbort=0 "
        "comparable=0 unprovable=0 matrix=0/0/0/0/0/0/0/0/0 "
        "offdiag=0 legacyMissedOverlap=0 closure=1/1/1",
        "DXVK War3GpuSkin: diag nativePoisonShadowReasons noLock=0 "
        "multi=0 owner=0 reentry=0 reset=0 mutation=0 format=0 lock=0 "
        "resource=0 storage=0 range=0 kernel=0",
    )
    zero_o1 = _parse_native_poison_o1_shadow(
        "DXVK War3GpuSkin: diag nativePoisonO1Shadow attempt=0 created=0 "
        "overflow=0 active=0 locks=0 kernels=0 unlocks=0 frozen=0 "
        "settled=0 cancel=0 resetAbort=0 comparable=0 unprovable=0 "
        "comparisonMissing=0 wouldClear=0 authority=0 "
        "matrix=0/0/0/0/0/0/0/0/0 offdiag=0 "
        "legacyMissedOverlap=0 closure=1/1/1",
        "DXVK War3GpuSkin: diag nativePoisonO1ShadowReasons noLock=0 "
        "multipleLocks=0 ownerOrLifo=0 reentry=0 resetOrRetirement=0 "
        "poisonMutation=0 formatOrFvf=0 lockDescriptor=0 "
        "resourceIdentity=0 storageIdentity=0 range=0 kernelNotNormal=0 "
        "kernelNotObserved=0 multipleKernels=0 kernelStateRead=0 "
        "kernelMode=0 kernelFormat=0 kernelMappedDst=0 "
        "unlockNotObserved=0 multipleUnlocks=0 unlockBeforeFreeze=0 "
        "unlockFailed=0 unlockIdentity=0 unlockGeneration=0 outerResult=0",
        "DXVK War3GpuSkin: diag nativePoisonO1Scanner calls=0 N=0 O=0 "
        "R=0 differentTarget=0 logicalExact=0 storageDiagnosticMismatch=0 "
        "realDrift=0 mappingDrift=0 mapAllocationMismatch=0 wouldClear=0 "
        "closure=1",
        "DXVK War3GpuSkin: diag nativePoisonO1ScannerReasons "
        "ledgerIncomplete=0 currentIncomplete=0 poisonIncomplete=0 "
        "partialIdentity=0 device=0 layout=0 resourceGeneration=0 range=0",
        {
            lane: (
                "DXVK War3GpuSkin: diag nativePoisonO1ScannerLane "
                f"lane={lane} calls=0 N=0 O=0 R=0 preLockMutation=0 "
                "lockToKernelMutation=0 reasons=0/0/0/0/0/0/0/0"
            )
            for lane in NATIVE_POISON_O1_LOCK_LANES
        },
        "DXVK War3GpuSkin: diag nativePoisonO1Physical "
        "logicalMatrixOnly=1 physicalN=0 physicalO=0 notReady=0 "
        "unlockDrift=0/0/0/0 unlockHardFirst=0/0/0/0 closure=1/1 "
        "authority=0",
        "DXVK War3GpuSkin: diag nativePoisonO1DiscardJoint "
        "oldOverlapFrozen=0 discardNotifications=0 "
        "discardOldOverlapRetired=0 oldOToN=0 exactDiscard=0 "
        "otherOldOToN=0 closure=1 authority=0",
    )
    checks["zeroO0Fixture"] = bool(
        zero_o0.get("contractClosed") is True
        and zero_o0.get("allCountersZero") is True
    )
    checks["zeroO1Fixture"] = bool(
        zero_o1.get("contractClosed") is True
        and zero_o1.get("allCountersZero") is True
    )

    disabled_report_o0 = _parse_native_poison_shadow(
        zero_o0["raw"]["main"].replace(
            "closure=1/1/1", "closure=0/1/1", 1,
        ),
        zero_o0["raw"]["reasons"],
    )
    disabled_report_o1 = _parse_native_poison_o1_shadow(
        zero_o1["raw"]["main"].replace(
            "closure=1/1/1", "closure=0/1/1", 1,
        ),
        zero_o1["raw"]["reasons"],
        zero_o1["raw"]["scanner"],
        zero_o1["raw"]["scannerReasons"],
        zero_o1["raw"]["lanes"],
        zero_o1["raw"]["physical"],
        zero_o1["raw"]["discardJoint"],
    )
    checks["disabledReportedO0ColdOnly"] = bool(
        disabled_report_o0.get("contractClosed") is False
        and disabled_report_o0.get("coldContractClosed") is True
        and disabled_report_o0.get("allCountersZero") is True
    )
    checks["disabledReportedO1ColdOnly"] = bool(
        disabled_report_o1.get("contractClosed") is False
        and disabled_report_o1.get("coldContractClosed") is True
        and disabled_report_o1.get("allCountersZero") is True
    )

    full_diag = {"fullExact": True, "lightExact": False}
    light_diag = {"fullExact": False, "lightExact": True}
    for name, value in GPU_SKIN_POISON_SIDECAR_POLICIES.items():
        parsed = _parse_native_poison_sidecar_policy(
            policy_line(name), requested_policy=name,
        )
        checks[f"{name}.policyParse"] = parsed.get("contractClosed") is True
        checks[f"{name}.fullOrthogonalCold"] = (
            _native_poison_sidecar_runtime_contract(
                parsed, zero_o0, zero_o1, full_diag,
            ).get("exact") is True
        )
        warm_o0 = dict(zero_o0)
        warm_o1 = dict(zero_o1)
        warm_o0["allCountersZero"] = False
        warm_o1["allCountersZero"] = False
        checks[f"{name}.fullRejectsWarmSidecars"] = (
            _native_poison_sidecar_runtime_contract(
                parsed, warm_o0, warm_o1, full_diag,
            ).get("exact") is False
        )
        active_o0 = dict(zero_o0)
        active_o1 = dict(zero_o1)
        if value & 1:
            active_o0["allCountersZero"] = False
        if value & 2:
            active_o1["allCountersZero"] = False
        checks[f"{name}.lightEnabledVsCold"] = (
            _native_poison_sidecar_runtime_contract(
                parsed, active_o0, active_o1, light_diag,
            ).get("exact") is True
        )
        if not (value & 1):
            contaminated_o0 = dict(active_o0)
            contaminated_o0["allCountersZero"] = False
            checks[f"{name}.o0DisabledRejectsWarm"] = (
                _native_poison_sidecar_runtime_contract(
                    parsed, contaminated_o0, active_o1, light_diag,
                ).get("exact") is False
            )
        if not (value & 2):
            contaminated_o1 = dict(active_o1)
            contaminated_o1["allCountersZero"] = False
            checks[f"{name}.o1DisabledRejectsWarm"] = (
                _native_poison_sidecar_runtime_contract(
                    parsed, active_o0, contaminated_o1, light_diag,
                ).get("exact") is False
            )
        pair = _native_poison_sidecar_pair_contract(parsed, parsed, name)
        checks[f"{name}.cleanPairExact"] = pair.get("exact") is True

    invalid = _parse_native_poison_sidecar_policy(
        policy_line("none", invalid=1, parse=0, closure=0),
        requested_policy="none",
    )
    checks["invalidExplicitRejected"] = invalid.get("contractClosed") is False
    mismatch = _native_poison_sidecar_pair_contract(
        _parse_native_poison_sidecar_policy(
            policy_line("none"), requested_policy="both",
        ),
        _parse_native_poison_sidecar_policy(
            policy_line("both"), requested_policy="both",
        ),
        "both",
    )
    checks["cleanPairPolicyDriftRejected"] = mismatch.get("exact") is False
    none_policy = _parse_native_poison_sidecar_policy(
        policy_line("none"), requested_policy="none",
    )
    o0_policy = _parse_native_poison_sidecar_policy(
        policy_line("o0"), requested_policy="o0",
    )
    checks["disabledReportedRuntimeColdExact"] = (
        _native_poison_sidecar_runtime_contract(
            none_policy, disabled_report_o0, disabled_report_o1, light_diag,
        ).get("exact") is True
    )
    checks["enabledReportedRuntimeRejectsColdAttempt"] = (
        _native_poison_sidecar_runtime_contract(
            o0_policy, disabled_report_o0, zero_o1, light_diag,
        ).get("exact") is False
    )
    checks["fullDiagnosticsAcceptsReportedCold"] = (
        _native_poison_sidecar_runtime_contract(
            o0_policy, disabled_report_o0, disabled_report_o1, full_diag,
        ).get("exact") is True
    )
    o0_disabled_pair = _native_poison_shadow_policy_pair(
        disabled_report_o0, disabled_report_o0, 17,
        False, True, False,
    )
    o1_disabled_pair = _native_poison_o1_shadow_policy_pair(
        disabled_report_o1, disabled_report_o1, 17,
        False, True, 17, 0, 0, {}, False,
    )
    checks["disabledO0PairCold"] = bool(
        o0_disabled_pair.get("exact") is True
        and o0_disabled_pair.get("disabledCold") is True
    )
    checks["disabledO1PairCold"] = bool(
        o1_disabled_pair.get("exact") is True
        and o1_disabled_pair.get("disabledCold") is True
    )
    if not all(checks.values()):
        raise AssertionError(
            "poison sidecar policy synthetic self-test failed: "
            + json.dumps(checks, sort_keys=True)
        )
    return {"ok": True, "count": len(checks), "checks": checks}


def _native_poison_o1_shadow_synthetic_self_tests() -> Dict[str, Any]:
    """Pure O1a-v2 fixtures; never launches, builds, or touches War3."""

    def parse_text(text: str) -> Dict[str, Any]:
        return _parse_gpu_skin_diag(text, {}).get(
            "nativePoisonO1Shadow", {}
        )

    def make_fixture(
        matrix: Optional[List[int]] = None,
        lane_states: Optional[Dict[str, Tuple[int, int, int]]] = None,
        physical: Optional[Tuple[int, int, int]] = None,
        unprovable_reasons: Optional[Dict[str, int]] = None,
        unlock_drifts: Optional[Tuple[int, int, int, int]] = None,
        unlock_hard: Optional[Tuple[int, int, int, int]] = None,
        active: int = 0,
        authority: int = 0,
        scanner_diagnostics: Optional[Dict[str, int]] = None,
        discard_joint_values: Optional[Dict[str, int]] = None,
        discard_joint_closure: int = 1,
    ) -> Tuple[Dict[str, Any], str, Dict[str, int]]:
        matrix = list(matrix if matrix is not None else [0] * 9)
        lane_states = dict(lane_states or {})
        lane_values = {
            lane_name: tuple(lane_states.get(lane_name, (0, 0, 0)))
            for lane_name in NATIVE_POISON_O1_LOCK_LANES
        }
        scan_n = sum(values[0] for values in lane_values.values())
        scan_o = sum(values[1] for values in lane_values.values())
        scan_r = sum(values[2] for values in lane_values.values())
        scan_calls = scan_n + scan_o + scan_r
        physical = tuple(physical or (scan_n, scan_o, scan_r))
        settled = sum(physical)
        attempts = settled + active
        reason_values = {
            name: 0 for name, _ in NATIVE_POISON_O1_SHADOW_REASON_FIELDS
        }
        reason_values.update(dict(unprovable_reasons or {}))
        unprovable = sum(reason_values.values())
        scanner_diagnostics = dict(scanner_diagnostics or {})
        storage_mismatch = scanner_diagnostics.get(
            "storageDiagnosticMismatch", 0
        )
        scanner_values = {
            "differentTarget": scanner_diagnostics.get("differentTarget", 0),
            "storageDiagnosticMismatch": storage_mismatch,
            "realDrift": scanner_diagnostics.get("realDrift", 0),
            "mappingDrift": scanner_diagnostics.get("mappingDrift", 0),
            "mapAllocationMismatch": scanner_diagnostics.get(
                "mapAllocationMismatch", 0
            ),
        }
        unlock_drifts = tuple(unlock_drifts or (0, 0, 0, 0))
        unlock_hard = tuple(unlock_hard or (0, 0, 0, 0))
        matrix_total = sum(matrix)
        overlap_column = sum(matrix[index] for index in (1, 4, 7))
        off_diagonal = sum(
            matrix[index] for index in (1, 2, 3, 5, 6, 7)
        )
        old_o_to_n = matrix[3]
        discard_joint = {
            "oldOverlapFrozen": old_o_to_n,
            "discardNotifications": old_o_to_n,
            "discardOldOverlapRetired": old_o_to_n,
            "oldOToN": old_o_to_n,
            "exactDiscard": old_o_to_n,
            "otherOldOToN": 0,
        }
        discard_joint.update(dict(discard_joint_values or {}))
        main = (
            "DXVK War3GpuSkin: diag nativePoisonO1Shadow "
            f"attempt={attempts} created={attempts} overflow=0 "
            f"active={active} locks={scan_calls} kernels={scan_calls} "
            f"unlocks={scan_calls} frozen={scan_calls} settled={settled} "
            "cancel=0 resetAbort=0 "
            f"comparable={scan_calls} unprovable={unprovable} "
            f"comparisonMissing={max(0, scan_calls - matrix_total)} "
            f"wouldClear={overlap_column} authority={authority} "
            f"matrix={'/'.join(str(value) for value in matrix)} "
            f"offdiag={off_diagonal} legacyMissedOverlap={matrix[1]} "
            "closure=1/1/1"
        )
        reasons_line = (
            "DXVK War3GpuSkin: diag nativePoisonO1ShadowReasons "
            + " ".join(
                f"{log_name}={reason_values[output_name]}"
                for output_name, log_name in
                NATIVE_POISON_O1_SHADOW_REASON_FIELDS
            )
        )
        discard_joint_line = (
            "DXVK War3GpuSkin: diag nativePoisonO1DiscardJoint "
            + " ".join(
                f"{name}={discard_joint[name]}"
                for name in NATIVE_POISON_O1_DISCARD_JOINT_FIELDS
            )
            + f" closure={discard_joint_closure} authority={authority}"
        )
        scanner_line = (
            "DXVK War3GpuSkin: diag nativePoisonO1Scanner "
            f"calls={scan_calls} N={scan_n} O={scan_o} R={scan_r} "
            f"differentTarget={scanner_values['differentTarget']} "
            f"logicalExact={scan_n + scan_o} "
            "storageDiagnosticMismatch="
            f"{scanner_values['storageDiagnosticMismatch']} "
            f"realDrift={scanner_values['realDrift']} "
            f"mappingDrift={scanner_values['mappingDrift']} "
            "mapAllocationMismatch="
            f"{scanner_values['mapAllocationMismatch']} "
            f"wouldClear={scan_o} closure=1"
        )
        aggregate_scan_reasons = {
            name: 0 for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
        }
        lane_lines: List[str] = []
        for lane_name, (lane_n, lane_o, lane_r) in lane_values.items():
            lane_reasons = [0] * len(NATIVE_POISON_O1_SCAN_FAILURE_FIELDS)
            lane_reasons[0] = lane_r
            aggregate_scan_reasons[
                NATIVE_POISON_O1_SCAN_FAILURE_FIELDS[0]
            ] += lane_r
            lane_lines.append(
                "DXVK War3GpuSkin: diag nativePoisonO1ScannerLane "
                f"lane={lane_name} calls={lane_n + lane_o + lane_r} "
                f"N={lane_n} O={lane_o} R={lane_r} "
                "preLockMutation=0 lockToKernelMutation=0 "
                f"reasons={'/'.join(str(value) for value in lane_reasons)}"
            )
        scanner_reasons_line = (
            "DXVK War3GpuSkin: diag nativePoisonO1ScannerReasons "
            + " ".join(
                f"{name}={aggregate_scan_reasons[name]}"
                for name in NATIVE_POISON_O1_SCAN_FAILURE_FIELDS
            )
        )
        physical_line = (
            "DXVK War3GpuSkin: diag nativePoisonO1Physical "
            f"logicalMatrixOnly=1 physicalN={physical[0]} "
            f"physicalO={physical[1]} notReady={physical[2]} "
            f"unlockDrift={'/'.join(str(value) for value in unlock_drifts)} "
            f"unlockHardFirst={'/'.join(str(value) for value in unlock_hard)} "
            f"closure=1/1 authority={authority}"
        )
        text = "\n".join((
            main, discard_joint_line, reasons_line,
            scanner_line, scanner_reasons_line,
            *lane_lines, physical_line,
        ))
        fast = {
            "poisonScanAttempts": scan_calls,
            "poisonNoOverlap": sum(matrix[0:3]),
            "poisonOverlap": sum(matrix[3:6]),
            "poisonReadFail": sum(matrix[6:9]),
        }
        return parse_text(text), text, fast

    def mutate_line(text: str, key: str, old: str, new: str) -> str:
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if f"diag {key} " in line:
                lines[index] = line.replace(old, new, 1)
                break
        return "\n".join(lines)

    def drop_line(text: str, key: str, contains: str = "") -> str:
        return "\n".join(
            line for line in text.splitlines()
            if not (
                f"diag {key} " in line and (not contains or contains in line)
            )
        )

    def light_policy(
        current: Dict[str, Any], fast: Dict[str, int],
    ) -> Dict[str, Any]:
        return _native_poison_o1_shadow_pair_policy(
            zero, current, fast["poisonScanAttempts"], False, True,
            fast["poisonNoOverlap"], fast["poisonOverlap"],
            fast["poisonReadFail"], fast,
        )

    zero, zero_text, zero_fast = make_fixture()
    exact, exact_text, exact_fast = make_fixture(
        matrix=[3, 0, 0, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (3, 1, 0)},
    )
    logical_axis_mismatch, _, _ = make_fixture(
        matrix=[2, 0, 1, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (3, 1, 0)},
    )
    conservative, conservative_text, conservative_fast = make_fixture(
        matrix=[0, 0, 1, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (0, 1, 1)},
        physical=(0, 1, 1),
    )
    physical_reject, physical_reject_text, physical_reject_fast = make_fixture(
        matrix=[2, 0, 0, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (2, 1, 0)},
        physical=(1, 1, 1),
        unprovable_reasons={"poisonMutation": 1},
    )
    soft_drift, _, soft_drift_fast = make_fixture(
        matrix=[2, 0, 0, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (2, 1, 0)},
        unlock_drifts=(0, 1, 1, 0),
        scanner_diagnostics={
            "storageDiagnosticMismatch": 1,
            "realDrift": 1,
            "mappingDrift": 1,
        },
    )
    hard_map_drift, _, hard_map_fast = make_fixture(
        matrix=[1, 0, 0, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (1, 1, 0)},
        physical=(0, 1, 1),
        unprovable_reasons={"unlockGeneration": 1},
        unlock_drifts=(0, 0, 0, 1),
        unlock_hard=(0, 0, 0, 1),
    )
    silent_overlap, _, silent_overlap_fast = make_fixture(
        matrix=[0, 0, 0, 1, 0, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (1, 0, 0)},
        discard_joint_values={
            "discardNotifications": 0,
            "discardOldOverlapRetired": 0,
            "exactDiscard": 0,
            "otherOldOToN": 1,
        },
        discard_joint_closure=0,
    )
    discard_overlap, _, discard_overlap_fast = make_fixture(
        matrix=[0, 0, 0, 1, 0, 0, 0, 0, 0],
        lane_states={"directDiscard": (1, 0, 0)},
    )
    logical_reject, _, logical_reject_fast = make_fixture(
        matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (1, 0, 1)},
        physical=(1, 0, 1),
    )
    overlap_physical_reject, _, overlap_physical_fast = make_fixture(
        matrix=[1, 0, 0, 2, 0, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (3, 0, 0)},
        physical=(1, 0, 2),
        unprovable_reasons={"poisonMutation": 2},
        discard_joint_values={
            "discardNotifications": 0,
            "discardOldOverlapRetired": 0,
            "exactDiscard": 0,
            "otherOldOToN": 2,
        },
        discard_joint_closure=0,
    )
    active, _, active_fast = make_fixture(active=1)
    legacy_missed, _, legacy_fast = make_fixture(
        matrix=[0, 1, 0, 0, 1, 0, 0, 0, 0],
        lane_states={"bufferNoOverwrite": (0, 2, 0)},
    )
    joint_monotonic_previous, _, _ = make_fixture(
        matrix=[0, 0, 0, 2, 0, 0, 0, 0, 0],
        lane_states={"directDiscard": (2, 0, 0)},
    )
    joint_monotonic_current, _, _ = make_fixture(
        matrix=[100, 0, 0, 1, 0, 0, 0, 0, 0],
        lane_states={
            "directNoOverwrite": (100, 0, 0),
            "directDiscard": (1, 0, 0),
        },
    )

    full_policy = _native_poison_o1_shadow_pair_policy(
        zero, zero, 0, True, False, 0, 0, 0, zero_fast,
    )
    exact_policy = light_policy(exact, exact_fast)
    conservative_policy = light_policy(conservative, conservative_fast)
    physical_reject_policy = light_policy(
        physical_reject, physical_reject_fast,
    )
    soft_drift_policy = light_policy(soft_drift, soft_drift_fast)
    hard_map_policy = light_policy(hard_map_drift, hard_map_fast)
    silent_overlap_policy = light_policy(
        silent_overlap, silent_overlap_fast,
    )
    discard_overlap_policy = light_policy(
        discard_overlap, discard_overlap_fast,
    )
    logical_reject_policy = light_policy(
        logical_reject, logical_reject_fast,
    )
    overlap_physical_policy = light_policy(
        overlap_physical_reject, overlap_physical_fast,
    )
    active_policy = light_policy(active, active_fast)
    legacy_policy = light_policy(legacy_missed, legacy_fast)

    missing_scanner = parse_text(drop_line(
        exact_text, "nativePoisonO1Scanner",
    ))
    missing_scanner_reasons = parse_text(drop_line(
        exact_text, "nativePoisonO1ScannerReasons",
    ))
    missing_physical = parse_text(drop_line(
        exact_text, "nativePoisonO1Physical",
    ))
    missing_discard_joint = parse_text(drop_line(
        exact_text, "nativePoisonO1DiscardJoint",
    ))
    missing_lane = parse_text(drop_line(
        exact_text, "nativePoisonO1ScannerLane", "directDiscard",
    ))
    matrix_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Shadow", "comparable=4", "comparable=5",
    ))
    would_clear_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Shadow", "wouldClear=1", "wouldClear=2",
    ))
    scanner_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Scanner", "calls=4", "calls=5",
    ))
    scanner_reason_mismatch = parse_text(mutate_line(
        conservative_text, "nativePoisonO1ScannerReasons",
        "ledgerIncomplete=1", "ledgerIncomplete=0",
    ))
    physical_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Physical", "physicalN=3", "physicalN=4",
    ))
    comparison_missing = parse_text(mutate_line(
        exact_text, "nativePoisonO1Shadow",
        "comparisonMissing=0", "comparisonMissing=1",
    ))
    reported_closure_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Shadow", "closure=1/1/1", "closure=1/1/0",
    ))
    discard_joint_closure_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1DiscardJoint", "closure=1", "closure=0",
    ))
    authority_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1Physical", "authority=0", "authority=1",
    ))
    discard_joint_authority_mismatch = parse_text(mutate_line(
        exact_text, "nativePoisonO1DiscardJoint",
        "authority=0", "authority=1",
    ))
    hard_real_text = mutate_line(
        physical_reject_text, "nativePoisonO1Physical",
        "unlockDrift=0/0/0/0", "unlockDrift=0/1/0/0",
    )
    hard_real_text = mutate_line(
        hard_real_text, "nativePoisonO1Physical",
        "unlockHardFirst=0/0/0/0", "unlockHardFirst=0/1/0/0",
    )
    hard_real_text = mutate_line(
        hard_real_text, "nativePoisonO1ShadowReasons",
        "unlockGeneration=0", "unlockGeneration=1",
    )
    hard_real = parse_text(hard_real_text)
    normal_return_bad_text = mutate_line(
        exact_text, "nativePoisonO1Shadow", "locks=4", "locks=3",
    )
    normal_return_bad = parse_text(normal_return_bad_text)
    poison_scan_mismatch_policy = _native_poison_o1_shadow_pair_policy(
        zero, exact, exact_fast["poisonScanAttempts"] + 1,
        False, True, exact_fast["poisonNoOverlap"],
        exact_fast["poisonOverlap"], exact_fast["poisonReadFail"],
        exact_fast,
    )
    row_mismatch_fast = dict(exact_fast)
    row_mismatch_fast["poisonNoOverlap"] -= 1
    row_mismatch_fast["poisonReadFail"] += 1
    row_mismatch_policy = light_policy(exact, row_mismatch_fast)
    full_nonzero_policy = _native_poison_o1_shadow_pair_policy(
        zero, exact, exact_fast["poisonScanAttempts"], True, False,
        exact_fast["poisonNoOverlap"], exact_fast["poisonOverlap"],
        exact_fast["poisonReadFail"], exact_fast,
    )
    full_joint_nonzero_policy = _native_poison_o1_shadow_pair_policy(
        zero, discard_overlap,
        discard_overlap_fast["poisonScanAttempts"], True, False,
        discard_overlap_fast["poisonNoOverlap"],
        discard_overlap_fast["poisonOverlap"],
        discard_overlap_fast["poisonReadFail"], discard_overlap_fast,
    )
    reverse_delta = _native_poison_o1_shadow_delta(exact, zero)
    joint_non_monotonic_delta = _native_poison_o1_shadow_delta(
        joint_monotonic_previous, joint_monotonic_current,
    )

    exact_lines = exact_text.splitlines()
    lane_contract = _forced_diag_block_contract(exact_text)
    first_lane = next(
        line for line in exact_lines
        if "diag nativePoisonO1ScannerLane " in line
    )
    duplicate_lane_contract = _forced_diag_block_contract(
        exact_text + "\n" + first_lane
    )
    discard_joint_line = next(
        line for line in exact_lines
        if "diag nativePoisonO1DiscardJoint " in line
    )
    duplicate_discard_joint_contract = _forced_diag_block_contract(
        exact_text + "\n" + discard_joint_line
    )
    missing_discard_joint_contract = _forced_diag_block_contract(drop_line(
        exact_text, "nativePoisonO1DiscardJoint",
    ))
    missing_lane_contract = _forced_diag_block_contract(drop_line(
        exact_text, "nativePoisonO1ScannerLane", "directDiscard",
    ))
    lane_violation_prefix = "nativePoisonO1ScannerLane"
    exact_lane_violations = {
        key: value for key, value in lane_contract["violations"].items()
        if key.startswith(lane_violation_prefix)
    }

    checks = {
        "zeroParsed": zero.get("contractClosed") is True,
        "exactParsed": exact.get("contractClosed") is True,
        "latestLineIntegrationParsed": exact.get("present") is True,
        "fullPolicyExact": full_policy.get("exact") is True,
        "lightPolicyExact": exact_policy.get("exact") is True,
        "conservativeOffDiagonalAccepted": (
            conservative_policy.get("exact") is True
            and conservative_policy["delta"]["deltas"]["offDiagonal"] > 0
        ),
        "classifiedPhysicalRejectAccepted": (
            physical_reject_policy.get("exact") is True
            and physical_reject_policy.get("postLockPhysicalRejects") == 1
        ),
        "softRealMappingDriftAccepted": soft_drift_policy.get("exact") is True,
        "hardMapAllocationClassifiedAccepted": hard_map_policy.get("exact") is True,
        "hardRealFirstCauseRejected": hard_real.get("contractClosed") is False,
        "silentOverlapRejected": silent_overlap_policy.get("exact") is False,
        "exactDiscardJointAccepted": discard_overlap_policy.get("exact") is True,
        "logicalRejectOverlapAccepted": logical_reject_policy.get("exact") is True,
        "physicalRejectDoesNotExplainOldOToN": (
            overlap_physical_policy.get("exact") is False
        ),
        "legacyMissedOverlapRejected": legacy_policy.get("exact") is False,
        "activeForcedEndpointRejected": active_policy.get("exact") is False,
        "missingScannerRejected": missing_scanner.get("present") is False,
        "missingScannerReasonsRejected": missing_scanner_reasons.get("present") is False,
        "missingPhysicalRejected": missing_physical.get("present") is False,
        "missingDiscardJointRejected": (
            missing_discard_joint.get("present") is False
        ),
        "missingLaneRejected": missing_lane.get("present") is False,
        "matrixClosureMismatchRejected": matrix_mismatch.get("contractClosed") is False,
        "logicalMatrixAxisMismatchRejected": (
            logical_axis_mismatch.get("contractClosed") is False
        ),
        "wouldClearMismatchRejected": would_clear_mismatch.get("contractClosed") is False,
        "scannerClosureMismatchRejected": scanner_mismatch.get("contractClosed") is False,
        "scannerReasonMismatchRejected": scanner_reason_mismatch.get("contractClosed") is False,
        "physicalClosureMismatchRejected": physical_mismatch.get("contractClosed") is False,
        "comparisonMissingRejected": comparison_missing.get("contractClosed") is False,
        "reportedClosureMismatchRejected": reported_closure_mismatch.get("contractClosed") is False,
        "discardJointClosureMismatchRejected": (
            discard_joint_closure_mismatch.get("contractClosed") is False
        ),
        "authorityRejected": authority_mismatch.get("contractClosed") is False,
        "discardJointAuthorityRejected": (
            discard_joint_authority_mismatch.get("contractClosed") is False
        ),
        "normalReturnMismatchRejected": normal_return_bad.get("contractClosed") is False,
        "poisonScanMismatchRejected": poison_scan_mismatch_policy.get("exact") is False,
        "legacyRowMismatchRejected": row_mismatch_policy.get("exact") is False,
        "fullNonzeroRejected": full_nonzero_policy.get("exact") is False,
        "fullJointNonzeroRejected": (
            full_joint_nonzero_policy.get("exact") is False
        ),
        "nonMonotonicRejected": reverse_delta.get("valid") is False,
        "discardJointNonMonotonicRejected": (
            joint_non_monotonic_delta.get("valid") is False
        ),
        "fourLaneMultiplicityAccepted": not exact_lane_violations,
        "duplicateLaneRejected": any(
            key.startswith(lane_violation_prefix)
            for key in duplicate_lane_contract["violations"]
        ),
        "missingLaneForcedContractRejected": any(
            key.startswith(lane_violation_prefix)
            for key in missing_lane_contract["violations"]
        ),
        "duplicateDiscardJointForcedRejected": (
            duplicate_discard_joint_contract["violations"].get(
                "nativePoisonO1DiscardJoint"
            ) == 2
        ),
        "missingDiscardJointForcedRejected": (
            missing_discard_joint_contract["violations"].get(
                "nativePoisonO1DiscardJoint"
            ) == 0
        ),
        "authorityAlwaysZero": all(
            item.get("authorizationAuthority") == 0
            for item in (
                full_policy, exact_policy, conservative_policy,
                physical_reject_policy, soft_drift_policy,
            )
        ),
    }
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "native poison O1-shadow synthetic self-test failed: "
            + json.dumps(result, sort_keys=True)
        )
    return result


def _manager_dispatch_light_partition_policy(
    manager_dispatch: Dict[str, Any],
) -> bool:
    """Exact production-light Common/Special scope partition."""
    fields = (
        "commonScopes", "specialScopes", "eagerScopes", "neverScopes",
        "nativeCpuOnlyScopes", "evidenceEagerScopes",
    )
    if not all(
        type(manager_dispatch.get(name)) is int
        and manager_dispatch[name] >= 0
        for name in fields
    ):
        return False
    common = manager_dispatch["commonScopes"]
    special = manager_dispatch["specialScopes"]
    eager = manager_dispatch["eagerScopes"]
    never = manager_dispatch["neverScopes"]
    native_cpu_only = manager_dispatch["nativeCpuOnlyScopes"]
    evidence = manager_dispatch["evidenceEagerScopes"]
    return bool(
        eager + native_cpu_only >= common
        and special >= never
        # Exact-negative Common scopes are bridge-local NativeCpuOnly. Every
        # remaining Common and every non-never Special scope is eager, so both
        # set differences still name the same Special population.
        and eager + native_cpu_only - common == special - never
        and native_cpu_only > 0
        and never > 0
        and evidence > 0
        and evidence <= eager
    )


_DISPATCH_CPU_ONLY_SEAL_COUNTER_NAMES = (
    "viewPublishes", "viewQueries", "authorityRejects",
    "candidateRejects", "managerProposals", "proposals",
    "localViewPublishAttempts", "localViewPublishes",
    "localViewRejects", "localViewQueries",
    "localViewAuthorityRejects", "localViewCandidateRejects",
    "localViewCommits", "nativeCpuOnlyScopes", "nativeCpuOnlyEnds",
    "proposalAccepted", "proposalRejected", "proposalAborted",
    "scopeCommits", "scopeEnds", "invalidations",
    "uploadsStarted", "uploadsCompleted", "vertices", "bytes",
    "kernelCalls", "kernelNormalReturns", "dips",
    "dipsWithUpload", "dipsNoUpload", "fanoutZero", "fanoutOne",
    "fanoutMany", "fanoutDipTotal", "markerConflicts",
)


def _dispatch_cpu_only_seal_accounting_contract(
    seal: Dict[str, Any], dispatch_seal_uploads: Any,
) -> bool:
    counters_present = all(
        isinstance(seal.get(name), int)
        for name in _DISPATCH_CPU_ONLY_SEAL_COUNTER_NAMES
    )
    reported_closed = bool(
        seal.get("reportedClosure")
        and all(value == 1 for value in seal["reportedClosure"].values())
    )
    return bool(
        counters_present and reported_closed
        and seal["viewQueries"] == (
            seal["authorityRejects"] + seal["candidateRejects"]
            + seal["managerProposals"]
        )
        and seal["viewPublishes"] == seal["localViewPublishAttempts"]
        and seal["localViewPublishAttempts"] == (
            seal["localViewPublishes"] + seal["localViewRejects"]
        )
        and seal["localViewQueries"] == (
            seal["localViewAuthorityRejects"]
            + seal["localViewCandidateRejects"]
            + seal["localViewCommits"]
        )
        and seal["managerProposals"] == seal["proposals"]
        and seal["proposals"] == (
            seal["proposalAccepted"] + seal["proposalRejected"]
        )
        and seal["proposalAccepted"] + seal["localViewCommits"] == (
            seal["scopeCommits"] + seal["proposalAborted"]
        )
        and seal["scopeCommits"] == seal["scopeEnds"]
        and seal["localViewCommits"] == seal["nativeCpuOnlyScopes"]
        and seal["nativeCpuOnlyScopes"] == seal["nativeCpuOnlyEnds"]
        and seal["uploadsStarted"] == seal["uploadsCompleted"]
        and seal["uploadsCompleted"] == (
            seal["fanoutZero"] + seal["fanoutOne"] + seal["fanoutMany"]
        )
        and seal["kernelCalls"] ==
            seal["kernelNormalReturns"] == seal["uploadsStarted"]
        and seal["dips"] == seal["dipsWithUpload"] + seal["dipsNoUpload"]
        and seal["fanoutDipTotal"] == seal["dipsWithUpload"]
        and seal["uploadsCompleted"] == dispatch_seal_uploads
        and seal["markerConflicts"] == 0
    )


def _manager_dispatch_light_partition_synthetic_self_tests(
) -> Dict[str, Any]:
    """Pure classification fixtures; never touches War3 or the filesystem."""
    def policy(**overrides: int) -> bool:
        values = {
            "commonScopes": 100,
            "specialScopes": 30,
            "eagerScopes": 40,
            "nativeCpuOnlyScopes": 80,
            "neverScopes": 10,
            # Independent fail-closed Special routes make the 20 Special-eager
            # scopes larger than this evidence cohort; that is valid.
            "evidenceEagerScopes": 3,
        }
        values.update(overrides)
        return _manager_dispatch_light_partition_policy(values)

    checks = {
        "validIndependentEagerPopulationAccepted": policy(),
        "evidenceSpansCommonAndSpecialAccepted": policy(
            evidenceEagerScopes=25,
        ),
        "partitionMismatchRejected": not policy(eagerScopes=39),
        "commonExceedsClassifiedRejected": not policy(
            eagerScopes=19, nativeCpuOnlyScopes=80,
        ),
        "zeroNativeCpuOnlyRejected": not policy(
            nativeCpuOnlyScopes=0, eagerScopes=120,
        ),
        "neverExceedsSpecialRejected": not policy(neverScopes=31),
        "zeroNeverRejected": not policy(neverScopes=0, eagerScopes=50),
        "zeroEvidenceRejected": not policy(evidenceEagerScopes=0),
        "evidenceExceedsEagerRejected": not policy(
            evidenceEagerScopes=41,
        ),
        "negativeRejected": not policy(neverScopes=-1),
        "malformedRejected": not _manager_dispatch_light_partition_policy({
            "commonScopes": 100,
            "specialScopes": 30,
            "eagerScopes": True,
            "nativeCpuOnlyScopes": 80,
            "neverScopes": 10,
            "evidenceEagerScopes": 3,
        }),
    }
    valid_seal = {
        "viewPublishes": 10, "viewQueries": 100,
        "authorityRejects": 10, "candidateRejects": 20,
        "managerProposals": 70, "proposals": 70,
        "localViewPublishAttempts": 10, "localViewPublishes": 9,
        "localViewRejects": 1, "localViewQueries": 80,
        "localViewAuthorityRejects": 5,
        "localViewCandidateRejects": 15, "localViewCommits": 60,
        "nativeCpuOnlyScopes": 60, "nativeCpuOnlyEnds": 60,
        "proposalAccepted": 65, "proposalRejected": 5,
        "proposalAborted": 5, "scopeCommits": 120, "scopeEnds": 120,
        "invalidations": 3, "uploadsStarted": 100,
        "uploadsCompleted": 100, "vertices": 1000, "bytes": 32000,
        "kernelCalls": 100, "kernelNormalReturns": 100, "dips": 110,
        "dipsWithUpload": 90, "dipsNoUpload": 20,
        "fanoutZero": 20, "fanoutOne": 70, "fanoutMany": 10,
        "fanoutDipTotal": 90, "markerConflicts": 0,
        "reportedClosure": {str(index): 1 for index in range(7)},
    }
    checks["localSealAccountingAccepted"] = (
        _dispatch_cpu_only_seal_accounting_contract(valid_seal, 100)
    )
    bad_local_partition = dict(valid_seal)
    bad_local_partition["localViewCandidateRejects"] = 14
    checks["localSealQueryMismatchRejected"] = not (
        _dispatch_cpu_only_seal_accounting_contract(
            bad_local_partition, 100,
        )
    )
    bad_native_end = dict(valid_seal)
    bad_native_end["nativeCpuOnlyEnds"] = 59
    checks["localSealNativeEndMismatchRejected"] = not (
        _dispatch_cpu_only_seal_accounting_contract(bad_native_end, 100)
    )
    telemetry_manager_names = (
        "physicalScopes", "physicalEnds", "commonScopes", "specialScopes",
        "semanticScopes", "eagerScopes", "lazyScopes", "neverScopes",
        "nativeCpuOnlyScopes", "nativeCpuOnlyEnds",
        "evidenceEagerScopes", "beginCallbacks", "endCallbacks",
        "eagerBegins", "lazyAdmissionAttempts", "lazyAdmissions",
        "issuedEnds", "noUploadEnds", "neverEnds", "skippedUploads",
        "skippedDips", "skippedFanouts", "rawDips", "outsideDips",
        "noUploadDips", "correlatedDips", "unmatchedDips",
        "outsideDipFastPath", "observerBegins", "observerEnds",
        "readerBegins", "readerEnds", "readerCommits", "readerRejects",
        "readerEvidenceFallbacks", "readerMismatches",
        "fastByFlush", "fastByObserver", "fastByReader", "telemetryFlushes",
        "telemetryBatchedAdds", "telemetryDeltaPending",
        "telemetryDeltaFaulted", "outsideAdmissionAttemptTotal",
        "outsideAdmissionCancellations",
        "outsideAdmissionLifecycleExcluded",
        "outsideAdmissionTrackedResolvedInside",
        "outsideAdmissionUntrackedResolvedOutside",
    )
    telemetry_seal_names = (
        "proposals", "proposalAccepted", "proposalRejected",
        "proposalAborted", "scopeCommits", "scopeEnds",
        "localViewQueries", "localViewAuthorityRejects",
        "localViewCandidateRejects", "localViewCommits", "invalidations",
        "uploadsStarted", "uploadsCompleted", "vertices", "bytes",
        "kernelCalls", "kernelNormalReturns", "dips", "dipsWithUpload",
        "dipsNoUpload", "fanoutZero", "fanoutOne", "fanoutMany",
        "fanoutDipTotal", "markerConflicts",
    )
    telemetry_previous = {name: 0 for name in telemetry_manager_names}
    telemetry_current = dict(telemetry_previous)
    telemetry_current.update({
        "physicalScopes": 1, "physicalEnds": 1, "commonScopes": 1,
        "nativeCpuOnlyScopes": 1, "nativeCpuOnlyEnds": 1,
        "telemetryFlushes": 1, "telemetryBatchedAdds": 9,
    })
    telemetry_seal_previous = {
        name: 0 for name in telemetry_seal_names
    }
    telemetry_seal_current = dict(telemetry_seal_previous)
    telemetry_seal_current.update({
        "scopeCommits": 1, "scopeEnds": 1,
        "localViewQueries": 1, "localViewCommits": 1,
    })
    telemetry_delta = _manager_dispatch_telemetry_delta(
        telemetry_previous, telemetry_current, {
            "valid": True,
            "outsideNativeFastPathDelta": 0,
            "kernelBatchesDelta": 0,
            "deltas": {"dispatchSealUploads": 0},
        }, telemetry_seal_previous, telemetry_seal_current,
    )
    checks["localSealTelemetryExactAdds"] = bool(
        telemetry_delta.get("valid") is True
        and telemetry_delta.get("dispatchSealClosed") is True
        and telemetry_delta.get("exactAdds") is True
        and telemetry_delta.get("expectedBatchedAddsDelta") == 9
    )
    reader_telemetry_current = dict(telemetry_previous)
    reader_telemetry_current.update({
        "rawDips": 1, "outsideDips": 1, "unmatchedDips": 1,
        "outsideDipFastPath": 1, "readerBegins": 1, "readerEnds": 1,
        "readerCommits": 1, "fastByReader": 1,
        "telemetryFlushes": 1, "telemetryBatchedAdds": 8,
    })
    reader_telemetry_delta = _manager_dispatch_telemetry_delta(
        telemetry_previous, reader_telemetry_current, {
            "valid": True,
            "outsideNativeFastPathDelta": 0,
            "kernelBatchesDelta": 0,
            "deltas": {"dispatchSealUploads": 0},
        }, telemetry_seal_previous, telemetry_seal_previous,
    )
    checks["readerTelemetryExactAdds"] = bool(
        reader_telemetry_delta.get("valid") is True
        and reader_telemetry_delta.get("dipSemanticClosed") is True
        and reader_telemetry_delta.get("exactAdds") is True
        and reader_telemetry_delta.get("expectedBatchedAddsDelta") == 8
    )
    reader_cover_mismatch = dict(reader_telemetry_current)
    reader_cover_mismatch["fastByReader"] = 0
    reader_cover_mismatch["fastByFlush"] = 1
    reader_cover_mismatch_delta = _manager_dispatch_telemetry_delta(
        telemetry_previous, reader_cover_mismatch, {
            "valid": True,
            "outsideNativeFastPathDelta": 0,
            "kernelBatchesDelta": 0,
            "deltas": {"dispatchSealUploads": 0},
        }, telemetry_seal_previous, telemetry_seal_previous,
    )
    checks["readerCommitCoverMismatchRejected"] = bool(
        reader_cover_mismatch_delta.get("exactAdds") is True
        and reader_cover_mismatch_delta.get("dipSemanticClosed") is False
    )
    authority_zero = {
        "attempts": 0, "created": 0, "armed": 0, "settled": 0,
        "lock": {"notifications": 0, "noOverlap": 0, "overlap": 0},
        "kernel": {"ready": 0, "normalReturns": 0},
        "unlock": {"notifications": 0, "exact": 0},
        "commit": {
            "noOverlap": 0, "rewrite": 0, "poisonClears": 0,
            "authority": 0,
        },
    }
    authority_normal = {
        "attempts": 1, "created": 1, "armed": 1, "settled": 1,
        "lock": {"notifications": 1, "noOverlap": 1, "overlap": 0},
        "kernel": {"ready": 1, "normalReturns": 1},
        "unlock": {"notifications": 1, "exact": 1},
        "commit": {
            "noOverlap": 1, "rewrite": 0, "poisonClears": 0,
            "authority": 1,
        },
    }
    authority_rewrite = {
        "attempts": 1, "created": 1, "armed": 1, "settled": 1,
        "lock": {"notifications": 1, "noOverlap": 0, "overlap": 1},
        "kernel": {"ready": 1, "normalReturns": 1},
        "unlock": {"notifications": 1, "exact": 1},
        "commit": {
            "noOverlap": 0, "rewrite": 1, "poisonClears": 1,
            "authority": 1,
        },
    }
    authority_sidecar_retained = {
        "attempts": 1, "created": 1, "armed": 1, "settled": 1,
        "retained": 1,
        "lock": {"notifications": 1, "noOverlap": 1, "overlap": 0},
        "kernel": {"ready": 1, "normalReturns": 1},
        "unlock": {"notifications": 1, "exact": 1},
        "commit": {
            "noOverlap": 0, "rewrite": 0, "poisonClears": 0,
            "authority": 0,
        },
    }
    authority_overflow = {
        **authority_zero,
        "attempts": 1,
        "overflow": 1,
    }
    authority_reset = {
        **authority_zero,
        "attempts": 1,
        "created": 1,
        "armed": 1,
        "resetAborted": 1,
    }

    def authority_telemetry_delta(
        current_authority: Dict[str, Any], batched_adds: int,
        faulted: int = 0,
    ) -> Dict[str, Any]:
        telemetry = dict(telemetry_previous)
        telemetry.update({
            "telemetryFlushes": 1,
            "telemetryBatchedAdds": batched_adds,
            "telemetryDeltaFaulted": faulted,
        })
        return _manager_dispatch_telemetry_delta(
            telemetry_previous, telemetry, {
                "valid": True,
                "outsideNativeFastPathDelta": 0,
                "kernelBatchesDelta": 0,
                "deltas": {"dispatchSealUploads": 0},
            }, telemetry_seal_previous, telemetry_seal_previous,
            authority_zero, current_authority,
        )

    normal_authority_telemetry = authority_telemetry_delta(
        authority_normal, 12,
    )
    rewrite_authority_telemetry = authority_telemetry_delta(
        authority_rewrite, 13,
    )
    retained_authority_telemetry = authority_telemetry_delta(
        authority_sidecar_retained, 10,
    )
    overflow_authority_telemetry = authority_telemetry_delta(
        authority_overflow, 1,
    )
    reset_authority_telemetry = authority_telemetry_delta(
        authority_reset, 3,
    )
    zero_authority_telemetry = authority_telemetry_delta(
        authority_zero, 0,
    )
    faulted_authority_telemetry = authority_telemetry_delta(
        authority_normal, 12, faulted=1,
    )
    checks["normalAuthorityTelemetryExactAdds"] = bool(
        normal_authority_telemetry.get("exactAdds") is True
        and normal_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 12
    )
    checks["rewriteAuthorityTelemetryExactAdds"] = bool(
        rewrite_authority_telemetry.get("exactAdds") is True
        and rewrite_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 13
    )
    checks["sidecarRetainedAuthorityTelemetryExactAdds"] = bool(
        retained_authority_telemetry.get("exactAdds") is True
        and retained_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 10
    )
    checks["atomicOverflowExcludedFromAuthorityBatch"] = bool(
        overflow_authority_telemetry.get("exactAdds") is True
        and overflow_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 1
    )
    checks["atomicResetExcludedFromAuthorityBatch"] = bool(
        reset_authority_telemetry.get("exactAdds") is True
        and reset_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 3
    )
    checks["fullZeroAuthorityBatchExact"] = bool(
        zero_authority_telemetry.get("exactAdds") is True
        and zero_authority_telemetry.get(
            "expectedBatchedAddsDelta"
        ) == 0
    )
    checks["authorityTelemetryUnderCountRejected"] = (
        authority_telemetry_delta(
            authority_normal, 11,
        ).get("exactAdds") is not True
    )
    checks["authorityTelemetryOverCountRejected"] = (
        authority_telemetry_delta(
            authority_normal, 13,
        ).get("exactAdds") is not True
    )
    checks["authorityTelemetryFaultRejected"] = bool(
        faulted_authority_telemetry.get("valid") is not True
        and faulted_authority_telemetry.get("endpointsClean") is not True
    )
    full_telemetry_policy = _manager_dispatch_telemetry_policy({
        "valid": True,
        "dipSemanticClosed": True,
        "dispatchSealClosed": True,
        "flushesDelta": 0,
        "batchedAddsDelta": 0,
        "expectedBatchedAddsDelta": 66185,
        "exactAdds": False,
    }, True, False)
    light_telemetry_policy = _manager_dispatch_telemetry_policy(
        normal_authority_telemetry, False, True,
    )
    ambiguous_telemetry_policy = _manager_dispatch_telemetry_policy({
        "valid": True,
        "dipSemanticClosed": True,
        "dispatchSealClosed": True,
        "flushesDelta": 0,
        "batchedAddsDelta": 0,
        "expectedBatchedAddsDelta": 0,
        "exactAdds": True,
    }, True, True)
    checks["fullPolicyDisablesBatchFormulaExactly"] = bool(
        full_telemetry_policy.get("exact") is True
        and full_telemetry_policy.get("batchingEnabled") is False
        and full_telemetry_policy.get("expectedBatchedAddsDelta") == 0
        and full_telemetry_policy.get("exactAdds") is True
        and full_telemetry_policy.get(
            "lightModeExpectedBatchedAddsDelta"
        ) == 66185
    )
    checks["lightPolicyKeepsExactAddFormula"] = bool(
        light_telemetry_policy.get("exact") is True
        and light_telemetry_policy.get("batchingEnabled") is True
        and light_telemetry_policy.get("expectedBatchedAddsDelta") == 12
    )
    checks["ambiguousTelemetryPolicyRejected"] = (
        ambiguous_telemetry_policy.get("exact") is not True
    )
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "manager-dispatch light partition synthetic self-test failed: "
            + json.dumps(result, sort_keys=True)
        )
    return result


def _parse_raw_timing(
    line: str, name: str, frequency: Optional[int],
) -> Dict[str, Any]:
    values = _match_tuple(
        line, rf"\b{re.escape(name)}=(\d+)/(\d+)/(\d+)", 3,
    )
    calls, ticks, max_ticks = values
    present = all(value is not None for value in values)
    shape_valid = bool(
        present
        and calls is not None and ticks is not None and max_ticks is not None
        and max_ticks <= ticks
        and ((calls == 0 and ticks == 0 and max_ticks == 0)
             or (calls > 0
                 and ((ticks == 0 and max_ticks == 0)
                      or (ticks > 0 and max_ticks > 0))))
    )
    frequency_valid = frequency is not None and frequency > 0
    total_ms = (
        ticks * 1000.0 / frequency
        if present and frequency_valid and ticks is not None else None
    )
    average_us = (
        ticks * 1_000_000.0 / (frequency * calls)
        if present and frequency_valid and calls not in (None, 0)
        and ticks is not None else None
    )
    maximum_us = (
        max_ticks * 1_000_000.0 / frequency
        if present and frequency_valid and max_ticks is not None else None
    )
    return {
        "calls": calls,
        "ticks": ticks,
        "maxTicks": max_ticks,
        "totalMs": total_ms,
        "averageUs": average_us,
        "maximumUs": maximum_us,
        "present": present,
        "shapeValid": shape_valid,
        "frequencyValid": frequency_valid,
    }


def _parse_raw_timing_group(
    line: str, names: Tuple[str, ...], frequency: Optional[int],
) -> Dict[str, Dict[str, Any]]:
    return {
        name: _parse_raw_timing(line, name, frequency)
        for name in names
    }


def _parse_split_raw_timing_group(
    calls_line: str, ticks_line: str, max_line: str,
    names: Tuple[str, ...], frequency: Optional[int],
) -> Dict[str, Dict[str, Any]]:
    """Parse timing partitions whose calls/ticks/max use separate lines."""
    result: Dict[str, Dict[str, Any]] = {}
    frequency_valid = isinstance(frequency, int) and frequency > 0
    for name in names:
        calls = _match_named_int(calls_line, name)
        ticks = _match_named_int(ticks_line, name)
        max_ticks = _match_named_int(max_line, name)
        present = all(
            isinstance(value, int) for value in (calls, ticks, max_ticks)
        )
        shape_valid = bool(
            present and calls is not None and ticks is not None
            and max_ticks is not None and 0 <= max_ticks <= ticks
            and ((calls == 0 and ticks == 0 and max_ticks == 0)
                 or (calls > 0
                     and ((ticks == 0 and max_ticks == 0)
                          or (ticks > 0 and max_ticks > 0))))
        )
        result[name] = {
            "calls": calls,
            "ticks": ticks,
            "maxTicks": max_ticks,
            "totalMs": (
                ticks * 1000.0 / frequency
                if present and frequency_valid and ticks is not None
                else None
            ),
            "averageUs": (
                ticks * 1_000_000.0 / (frequency * calls)
                if present and frequency_valid and calls not in (None, 0)
                and ticks is not None else None
            ),
            "maximumUs": (
                max_ticks * 1_000_000.0 / frequency
                if present and frequency_valid and max_ticks is not None
                else None
            ),
            "present": present,
            "shapeValid": shape_valid,
            "frequencyValid": frequency_valid,
        }
    return result


def _timing_partition_contract(
    parent: Dict[str, Any], children: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    fields = ("calls", "ticks", "maxTicks")
    available = bool(
        all(isinstance(parent.get(field), int) for field in fields)
        and all(
            isinstance(record.get(field), int)
            for record in children.values() for field in fields
        )
    )
    child_calls = sum(
        record["calls"] for record in children.values()
        if isinstance(record.get("calls"), int)
    )
    child_ticks = sum(
        record["ticks"] for record in children.values()
        if isinstance(record.get("ticks"), int)
    )
    child_max = max(
        (
            record["maxTicks"] for record in children.values()
            if isinstance(record.get("maxTicks"), int)
        ),
        default=0,
    )
    return {
        "available": available,
        "parent": {
            field: parent.get(field) for field in fields
        },
        "children": {
            "calls": child_calls if available else None,
            "ticks": child_ticks if available else None,
            "maxTicks": child_max if available else None,
        },
        "callsClosed": (
            parent.get("calls") == child_calls if available else None
        ),
        "ticksClosed": (
            parent.get("ticks") == child_ticks if available else None
        ),
        "maxClosed": (
            parent.get("maxTicks") == child_max if available else None
        ),
        "closed": bool(
            available and parent.get("calls") == child_calls
            and parent.get("ticks") == child_ticks
            and parent.get("maxTicks") == child_max
        ),
    }


def _timing_ticks(record: Dict[str, Any]) -> Optional[int]:
    value = record.get("ticks")
    return value if isinstance(value, int) else None


def _timing_contains(
    parent: Dict[str, Any], children: List[Dict[str, Any]],
) -> Dict[str, Any]:
    parent_ticks = _timing_ticks(parent)
    child_ticks = [_timing_ticks(child) for child in children]
    available = parent_ticks is not None and all(
        value is not None for value in child_ticks
    )
    child_sum = sum(value for value in child_ticks if value is not None)
    return {
        "available": available,
        "parentTicks": parent_ticks,
        "childTicks": child_sum if available else None,
        "residualTicks": (
            parent_ticks - child_sum if available else None
        ),
        "contains": parent_ticks >= child_sum if available else None,
    }


def _native_begin_sample_closure(
    stages: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    names = (
        "common", "state", "exact", "scopeRoute", "stateRejectRoute",
        "skinRoute", "smallRoute", "candidateRoute",
    )
    available = all(
        isinstance(stages.get(name, {}).get("calls"), int)
        and isinstance(stages.get(name, {}).get("ticks"), int)
        for name in names
    )
    if not available:
        return {
            "available": False,
            "commonCallsClosed": None,
            "stateCallsClosed": None,
            "exactCallsClosed": None,
            "routeContainsSampledStages": None,
            "contains": None,
        }

    calls = {name: stages[name]["calls"] for name in names}
    ticks = {name: stages[name]["ticks"] for name in names}
    common_calls_closed = calls["common"] == sum(
        calls[name] for name in (
            "scopeRoute", "stateRejectRoute", "skinRoute", "smallRoute",
            "candidateRoute",
        )
    )
    state_calls_closed = calls["state"] == sum(
        calls[name] for name in (
            "stateRejectRoute", "skinRoute", "smallRoute", "candidateRoute",
        )
    )
    exact_calls_closed = calls["exact"] == calls["candidateRoute"]
    route_ticks = sum(
        ticks[name] for name in (
            "scopeRoute", "stateRejectRoute", "skinRoute", "smallRoute",
            "candidateRoute",
        )
    )
    sampled_stage_ticks = sum(ticks[name] for name in (
        "common", "state", "exact",
    ))
    route_contains_stages = route_ticks >= sampled_stage_ticks
    return {
        "available": True,
        "commonCalls": calls["common"],
        "routeCalls": sum(
            calls[name] for name in (
                "scopeRoute", "stateRejectRoute", "skinRoute", "smallRoute",
                "candidateRoute",
            )
        ),
        "commonCallsClosed": common_calls_closed,
        "stateCallsClosed": state_calls_closed,
        "exactCallsClosed": exact_calls_closed,
        "routeTicks": route_ticks,
        "sampledStageTicks": sampled_stage_ticks,
        "routeContainsSampledStages": route_contains_stages,
        "contains": bool(
            common_calls_closed and state_calls_closed and exact_calls_closed
            and route_contains_stages
        ),
    }


def _native_t2_sample_closure(
    exact_stage: Dict[str, Any],
    stages: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    names = (
        "geoSnap", "geoHeader", "posProof", "normalProof", "groupProof",
        "paletteProof",
    )
    exact_calls = exact_stage.get("calls")
    exact_ticks = exact_stage.get("ticks")
    available = bool(
        isinstance(exact_calls, int)
        and isinstance(exact_ticks, int)
        and all(
            isinstance(stages.get(name, {}).get("calls"), int)
            and isinstance(stages.get(name, {}).get("ticks"), int)
            for name in names
        )
    )
    if not available:
        return {
            "available": False,
            "allStageCallsEqualExact": None,
            "exactContainsStageTicks": None,
            "evidencePositive": None,
            "contains": None,
        }

    stage_calls = {name: stages[name]["calls"] for name in names}
    stage_ticks = {name: stages[name]["ticks"] for name in names}
    calls_equal = all(value == exact_calls for value in stage_calls.values())
    stage_tick_sum = sum(stage_ticks.values())
    ticks_contained = exact_ticks >= stage_tick_sum
    return {
        "available": True,
        "exactCalls": exact_calls,
        "stageCalls": stage_calls,
        "allStageCallsEqualExact": calls_equal,
        "exactTicks": exact_ticks,
        "stageTicks": stage_ticks,
        "stageTickSum": stage_tick_sum,
        "residualTicks": exact_ticks - stage_tick_sum,
        "exactContainsStageTicks": ticks_contained,
        # A short window can legitimately contain no prime-period T2 sample.
        # Preserve that distinction as evidence only; zero evidence must not
        # weaken the structural timing contract or a P4 correctness gate.
        "evidencePositive": bool(
            exact_calls > 0 and all(value > 0 for value in stage_calls.values())
        ),
        "contains": bool(calls_equal and ticks_contained),
    }


def _raw_timing_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
    frequency: Optional[int],
) -> Dict[str, Any]:
    previous_calls = previous.get("calls")
    current_calls = current.get("calls")
    previous_ticks = previous.get("ticks")
    current_ticks = current.get("ticks")
    monotonic = bool(
        isinstance(previous_calls, int) and isinstance(current_calls, int)
        and isinstance(previous_ticks, int) and isinstance(current_ticks, int)
        and current_calls >= previous_calls and current_ticks >= previous_ticks
    )
    delta_calls = current_calls - previous_calls if monotonic else None
    delta_ticks = current_ticks - previous_ticks if monotonic else None
    frequency_valid = isinstance(frequency, int) and frequency > 0
    return {
        "calls": delta_calls,
        "ticks": delta_ticks,
        "totalMs": (
            delta_ticks * 1000.0 / frequency
            if monotonic and frequency_valid else None
        ),
        "averageUs": (
            delta_ticks * 1_000_000.0 / (frequency * delta_calls)
            if monotonic and frequency_valid and delta_calls not in (None, 0)
            else None
        ),
        # Max is not subtractable. Preserve the second snapshot's lifetime
        # high-water explicitly rather than mislabelling it as a window max.
        "lifetimeMaxTicks": current.get("maxTicks"),
        "lifetimeMaximumUs": current.get("maximumUs"),
        "monotonic": monotonic,
    }


_PRODUCTION_CALLBACK_NAMES = (
    "flush", "begin", "end", "preflight", "cpuRewrite", "upload",
    "dip", "fanout",
)


def _production_sample_timing_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
    raw_uploads: Optional[int],
) -> Dict[str, Any]:
    previous_frequency = previous.get("frequency", {})
    current_frequency = current.get("frequency", {})
    endpoint_contract_exact = bool(
        previous.get("present") is True and current.get("present") is True
    )
    native_frequency = current_frequency.get("native")
    manager_frequency = current_frequency.get("manager")
    frequency_match = bool(
        isinstance(native_frequency, int) and native_frequency > 0
        and native_frequency == manager_frequency
        and previous_frequency.get("native") == native_frequency
        and previous_frequency.get("manager") == manager_frequency
    )
    period = current.get("period")
    phase = current.get("phase")
    sample_contract_exact = bool(
        period == 256 and phase == 0xA5
        and previous.get("period") == period
        and previous.get("phase") == phase
    )
    previous_writer = previous.get("writerSnapshot", {})
    current_writer = current.get("writerSnapshot", {})
    writer_fields = ("started", "completed", "active", "pending")
    writer_present = all(
        isinstance(snapshot.get(name), int)
        for snapshot in (previous_writer, current_writer)
        for name in writer_fields
    )
    writer_monotonic = bool(
        writer_present
        and current_writer["started"] >= previous_writer["started"]
        and current_writer["completed"] >= previous_writer["completed"]
    )
    writer_delta_started = (
        current_writer["started"] - previous_writer["started"]
        if writer_monotonic else None
    )
    writer_delta_completed = (
        current_writer["completed"] - previous_writer["completed"]
        if writer_monotonic else None
    )
    writer_endpoints_clean = bool(
        writer_present
        and all(
            snapshot["active"] == 0 and snapshot["pending"] == 0
            and snapshot["started"] == snapshot["completed"]
            for snapshot in (previous_writer, current_writer)
        )
    )
    writer_window_closed = bool(
        writer_monotonic
        and writer_delta_started == writer_delta_completed
    )
    group_frequency = {
        "outerStages": native_frequency,
        "dispatchSealStages": native_frequency,
        "kernelStages": native_frequency,
        "eventRootStages": native_frequency,
        "eventSemanticStages": native_frequency,
        "eventDipDeviceStages": native_frequency,
        "eventDipBridgeStages": native_frequency,
        "eventDipResolveStages": native_frequency,
        "bridgePinStages": native_frequency,
        "bridgeBodyStages": native_frequency,
        "bridgeLeaveStages": native_frequency,
        "managerEnterStages": manager_frequency,
        "managerBodyStages": manager_frequency,
        "managerLeaveStages": manager_frequency,
    }
    groups: Dict[str, Dict[str, Any]] = {}
    all_monotonic = writer_monotonic
    all_present = writer_present
    all_shape_valid = True
    for group_name, frequency in group_frequency.items():
        previous_group = previous.get(group_name, {})
        current_group = current.get(group_name, {})
        group_delta: Dict[str, Any] = {}
        for stage_name, current_record in current_group.items():
            previous_record = previous_group.get(stage_name, {})
            delta = _raw_timing_delta(
                previous_record, current_record, frequency,
            )
            maximum_monotonic = bool(
                isinstance(previous_record.get("maxTicks"), int)
                and isinstance(current_record.get("maxTicks"), int)
                and current_record["maxTicks"] >= previous_record["maxTicks"]
            )
            endpoints_shape_valid = bool(
                previous_record.get("shapeValid") is True
                and current_record.get("shapeValid") is True
                and previous_record.get("frequencyValid") is True
                and current_record.get("frequencyValid") is True
            )
            delta["lifetimeMaximumMonotonic"] = maximum_monotonic
            delta["endpointsShapeValid"] = endpoints_shape_valid
            group_delta[stage_name] = delta
            all_monotonic = (
                all_monotonic and bool(delta["monotonic"])
                and maximum_monotonic
            )
            all_shape_valid = all_shape_valid and endpoints_shape_valid
            all_present = all_present and all(
                isinstance(delta.get(name), int)
                for name in ("calls", "ticks")
            ) and isinstance(current_record.get("maxTicks"), int)
            all_present = all_present and isinstance(
                previous_record.get("maxTicks"), int
            )
        groups[group_name] = group_delta

    previous_rejects = previous.get("managerRejected", {})
    current_rejects = current.get("managerRejected", {})
    reject_deltas: Dict[str, Optional[int]] = {}
    rejects_monotonic = True
    for name in _PRODUCTION_CALLBACK_NAMES:
        before = previous_rejects.get(name)
        after = current_rejects.get(name)
        monotonic = bool(
            isinstance(before, int) and isinstance(after, int)
            and after >= before
        )
        reject_deltas[name] = after - before if monotonic else None
        rejects_monotonic = rejects_monotonic and monotonic
    all_monotonic = all_monotonic and rejects_monotonic
    all_present = all_present and all(
        isinstance(value, int) for value in reject_deltas.values()
    )

    def stage(group: str, name: str, field: str) -> Optional[int]:
        value = groups.get(group, {}).get(name, {}).get(field)
        return value if isinstance(value, int) else None

    outer_names = (
        "admitAccepted", "admitRejected", "inclusive", "body",
        "complete", "cancel", "fallbackInclusive", "fallbackBegin",
        "fallbackBody", "fallbackComplete",
    )
    outer_values_present = all(
        stage("outerStages", name, field) is not None
        for name in outer_names for field in ("calls", "ticks")
    )
    outer_population_calls = None
    outer_calls_closed = False
    outer_ticks_contained = False
    fast_child_ticks = None
    fallback_child_ticks = None
    fast_residual_ticks = None
    fallback_residual_ticks = None
    if outer_values_present:
        accepted_calls = stage("outerStages", "admitAccepted", "calls")
        rejected_calls = stage("outerStages", "admitRejected", "calls")
        inclusive_calls = stage("outerStages", "inclusive", "calls")
        body_calls = stage("outerStages", "body", "calls")
        complete_calls = stage("outerStages", "complete", "calls")
        cancel_calls = stage("outerStages", "cancel", "calls")
        fallback_inclusive_calls = stage(
            "outerStages", "fallbackInclusive", "calls"
        )
        fallback_begin_calls = stage(
            "outerStages", "fallbackBegin", "calls"
        )
        fallback_body_calls = stage(
            "outerStages", "fallbackBody", "calls"
        )
        fallback_complete_calls = stage(
            "outerStages", "fallbackComplete", "calls"
        )
        outer_population_calls = accepted_calls + rejected_calls
        outer_calls_closed = bool(
            accepted_calls == inclusive_calls
            and body_calls <= accepted_calls
            and accepted_calls == complete_calls + cancel_calls
            and rejected_calls == fallback_inclusive_calls
            and fallback_begin_calls <= fallback_inclusive_calls
            and fallback_body_calls <= fallback_inclusive_calls
            and fallback_complete_calls <= fallback_inclusive_calls
        )
        fast_child_ticks = sum(
            stage("outerStages", name, "ticks")
            for name in ("admitAccepted", "body", "complete", "cancel")
        )
        fallback_child_ticks = sum(
            stage("outerStages", name, "ticks")
            for name in (
                "fallbackBegin", "fallbackBody", "fallbackComplete",
            )
        )
        outer_ticks_contained = bool(
            stage("outerStages", "inclusive", "ticks") >= fast_child_ticks
            and stage("outerStages", "fallbackInclusive", "ticks") >=
                fallback_child_ticks
        )
        fast_residual_ticks = (
            stage("outerStages", "inclusive", "ticks") - fast_child_ticks
        )
        fallback_residual_ticks = (
            stage("outerStages", "fallbackInclusive", "ticks") -
            fallback_child_ticks
        )
    dispatch_seal_names = (
        "admission", "inclusive", "body", "complete", "cancel",
    )
    dispatch_seal_values_present = all(
        stage("dispatchSealStages", name, field) is not None
        for name in dispatch_seal_names
        for field in ("calls", "ticks", "lifetimeMaxTicks")
    )
    dispatch_seal_calls_closed = False
    dispatch_seal_ticks_contained = False
    dispatch_seal_max_contained = False
    dispatch_seal_cancel_zero = False
    dispatch_seal_calls = None
    if dispatch_seal_values_present:
        dispatch_seal_calls = stage(
            "dispatchSealStages", "admission", "calls"
        )
        dispatch_seal_calls_closed = bool(
            dispatch_seal_calls == stage(
                "dispatchSealStages", "inclusive", "calls"
            )
            and dispatch_seal_calls == stage(
                "dispatchSealStages", "body", "calls"
            )
            and dispatch_seal_calls == (
                stage("dispatchSealStages", "complete", "calls")
                + stage("dispatchSealStages", "cancel", "calls")
            )
        )
        dispatch_seal_ticks_contained = bool(
            stage("dispatchSealStages", "inclusive", "ticks") >= sum(
                stage("dispatchSealStages", name, "ticks")
                for name in ("admission", "body", "complete", "cancel")
            )
        )
        dispatch_seal_max_contained = bool(
            all(
                stage(
                    "dispatchSealStages", "inclusive", "lifetimeMaxTicks"
                ) >= stage(
                    "dispatchSealStages", name, "lifetimeMaxTicks"
                )
                for name in ("admission", "body", "complete", "cancel")
            )
        )
        dispatch_seal_cancel_zero = bool(
            stage("dispatchSealStages", "cancel", "calls") == 0
            and stage("dispatchSealStages", "cancel", "ticks") == 0
        )
        if isinstance(outer_population_calls, int):
            outer_population_calls += dispatch_seal_calls
    dispatch_seal_reported_endpoint_clean = bool(
        all(
            snapshot.get("dispatchSealReportedClosure", {}).get(name) == 1
            for snapshot in (previous, current)
            for name in ("calls", "ticks", "max", "cancelZero")
        )
    )
    cadence_lower = (
        raw_uploads // 256 if isinstance(raw_uploads, int)
        and raw_uploads >= 0 else None
    )
    cadence_upper = (
        (raw_uploads + 255) // 256 if isinstance(raw_uploads, int)
        and raw_uploads >= 0 else None
    )
    outer_cadence_exact = bool(
        isinstance(outer_population_calls, int)
        and isinstance(cadence_lower, int) and isinstance(cadence_upper, int)
        and cadence_lower <= outer_population_calls <= cadence_upper
    )

    kernel_names = ("inclusive", "evaluate", "original", "notify")
    kernel_values_present = all(
        stage("kernelStages", name, field) is not None
        for name in kernel_names for field in ("calls", "ticks")
    )
    kernel_calls_closed = False
    kernel_ticks_contained = False
    if kernel_values_present:
        kernel_calls_closed = bool(
            stage("kernelStages", "inclusive", "calls") ==
                stage("kernelStages", "evaluate", "calls")
            and stage("kernelStages", "notify", "calls") <=
                stage("kernelStages", "original", "calls") <=
                stage("kernelStages", "evaluate", "calls")
        )
        kernel_ticks_contained = bool(
            stage("kernelStages", "inclusive", "ticks") >= sum(
                stage("kernelStages", name, "ticks")
                for name in ("evaluate", "original", "notify")
            )
        )

    event_root_names = (
        "flushRoot", "dispatchSemanticLookup",
        "dispatchBeginRoot", "dispatchEndRoot",
    )
    event_root_values_present = all(
        stage("eventRootStages", name, field) is not None
        for name in event_root_names for field in ("calls", "ticks")
    )
    event_semantic_values_present = all(
        stage("eventSemanticStages", name, field) is not None
        for name in ("semanticInclusive", "semanticOriginal")
        for field in ("calls", "ticks", "lifetimeMaxTicks")
    )
    event_semantic_calls_closed = False
    event_semantic_ticks_contained = False
    event_semantic_max_contained = False
    if event_semantic_values_present:
        event_semantic_calls_closed = bool(
            stage("eventSemanticStages", "semanticInclusive", "calls") ==
            stage("eventSemanticStages", "semanticOriginal", "calls")
        )
        event_semantic_ticks_contained = bool(
            stage("eventSemanticStages", "semanticInclusive", "ticks") >=
            stage("eventSemanticStages", "semanticOriginal", "ticks")
        )
        event_semantic_max_contained = bool(
            stage(
                "eventSemanticStages", "semanticInclusive",
                "lifetimeMaxTicks",
            ) >= stage(
                "eventSemanticStages", "semanticOriginal",
                "lifetimeMaxTicks",
            )
        )

    event_dip_contracts: Dict[str, Any] = {}
    event_dip_calls_closed = True
    event_dip_ticks_contained = True
    event_dip_max_contained = True
    event_dip_values_present = True
    for name in ("outside", "noUpload", "correlated"):
        device_calls = stage("eventDipDeviceStages", name, "calls")
        bridge_calls = stage("eventDipBridgeStages", name, "calls")
        resolve_calls = stage("eventDipResolveStages", name, "calls")
        device_ticks = stage("eventDipDeviceStages", name, "ticks")
        bridge_ticks = stage("eventDipBridgeStages", name, "ticks")
        resolve_ticks = stage("eventDipResolveStages", name, "ticks")
        device_max = stage(
            "eventDipDeviceStages", name, "lifetimeMaxTicks"
        )
        bridge_max = stage(
            "eventDipBridgeStages", name, "lifetimeMaxTicks"
        )
        resolve_max = stage(
            "eventDipResolveStages", name, "lifetimeMaxTicks"
        )
        values_present = all(
            isinstance(value, int) for value in (
                device_calls, bridge_calls, resolve_calls,
                device_ticks, bridge_ticks, resolve_ticks,
                device_max, bridge_max, resolve_max,
            )
        )
        calls_closed = bool(
            values_present and device_calls == bridge_calls
            and resolve_calls <= device_calls
        )
        ticks_contained = bool(
            values_present and device_ticks >= bridge_ticks + resolve_ticks
        )
        max_contained = bool(
            values_present and device_max >= bridge_max
            and device_max >= resolve_max
        )
        event_dip_contracts[name] = {
            "present": values_present,
            "callsClosed": calls_closed,
            "ticksContained": ticks_contained,
            "maxContained": max_contained,
            "deviceCalls": device_calls,
            "bridgeCalls": bridge_calls,
            "resolveCalls": resolve_calls,
            "deviceTicks": device_ticks,
            "bridgeAndResolveTicks": (
                bridge_ticks + resolve_ticks if values_present else None
            ),
            "deviceLifetimeMaxTicks": device_max,
            "bridgeLifetimeMaxTicks": bridge_max,
            "resolveLifetimeMaxTicks": resolve_max,
        }
        event_dip_values_present = (
            event_dip_values_present and values_present
        )
        event_dip_calls_closed = event_dip_calls_closed and calls_closed
        event_dip_ticks_contained = (
            event_dip_ticks_contained and ticks_contained
        )
        event_dip_max_contained = (
            event_dip_max_contained and max_contained
        )
    event_dip_device_calls = sum(
        stage("eventDipDeviceStages", name, "calls") or 0
        for name in ("outside", "noUpload", "correlated")
    )
    event_dip_bridge_calls = sum(
        stage("eventDipBridgeStages", name, "calls") or 0
        for name in ("outside", "noUpload", "correlated")
    )
    event_dip_total_calls_closed = bool(
        event_dip_values_present
        and event_dip_device_calls == event_dip_bridge_calls
    )
    event_evidence_positive = bool(
        event_root_values_present
        and (stage("eventRootStages", "flushRoot", "calls") or 0) > 0
        and (stage(
            "eventRootStages", "dispatchSemanticLookup", "calls"
        ) or 0) > 0
        and (stage(
            "eventRootStages", "dispatchBeginRoot", "calls"
        ) or 0) > 0
        and (stage(
            "eventRootStages", "dispatchEndRoot", "calls"
        ) or 0) > 0
        and (stage(
            "eventSemanticStages", "semanticInclusive", "calls"
        ) or 0) > 0
        and event_dip_device_calls > 0
    )
    def endpoint_event_evidence(snapshot: Dict[str, Any]) -> bool:
        root = snapshot.get("eventRootStages", {})
        semantic = snapshot.get("eventSemanticStages", {})
        dip = snapshot.get("eventDipDeviceStages", {})
        return bool(
            all(
                isinstance(root.get(name, {}).get("calls"), int)
                and root[name]["calls"] > 0
                for name in event_root_names
            )
            and isinstance(
                semantic.get("semanticInclusive", {}).get("calls"), int
            )
            and semantic["semanticInclusive"]["calls"] > 0
            and sum(
                dip.get(name, {}).get("calls", 0)
                if isinstance(dip.get(name, {}).get("calls"), int) else 0
                for name in ("outside", "noUpload", "correlated")
            ) > 0
        )
    event_endpoint_evidence_positive = bool(
        endpoint_event_evidence(previous)
        and endpoint_event_evidence(current)
    )

    callback_contracts: Dict[str, Any] = {}
    callback_calls_closed = True
    callback_ticks_contained = True
    for name in _PRODUCTION_CALLBACK_NAMES:
        pin_calls = stage("bridgePinStages", name, "calls")
        bridge_body_calls = stage("bridgeBodyStages", name, "calls")
        leave_calls = stage("bridgeLeaveStages", name, "calls")
        manager_enter_calls = stage("managerEnterStages", name, "calls")
        manager_body_calls = stage("managerBodyStages", name, "calls")
        manager_leave_calls = stage("managerLeaveStages", name, "calls")
        rejected = reject_deltas.get(name)
        calls_present = all(
            isinstance(value, int) for value in (
                pin_calls, bridge_body_calls, leave_calls,
                manager_enter_calls, manager_body_calls,
                manager_leave_calls, rejected,
            )
        )
        calls_closed = bool(
            calls_present
            and pin_calls == leave_calls
            and bridge_body_calls <= pin_calls
            and bridge_body_calls == manager_enter_calls
            and manager_enter_calls == manager_body_calls + rejected
            and manager_body_calls == manager_leave_calls
        )
        bridge_body_ticks = stage("bridgeBodyStages", name, "ticks")
        manager_phase_ticks = [
            stage(group, name, "ticks") for group in (
                "managerEnterStages", "managerBodyStages",
                "managerLeaveStages",
            )
        ]
        ticks_present = isinstance(bridge_body_ticks, int) and all(
            isinstance(value, int) for value in manager_phase_ticks
        )
        ticks_contained = bool(
            ticks_present and bridge_body_ticks >= sum(manager_phase_ticks)
        )
        callback_contracts[name] = {
            "callsClosed": calls_closed,
            "ticksContained": ticks_contained,
            "bridgeBodyCalls": bridge_body_calls,
            "managerEnterCalls": manager_enter_calls,
            "managerBodyCalls": manager_body_calls,
            "managerLeaveCalls": manager_leave_calls,
            "managerRejected": rejected,
            "bridgeBodyTicks": bridge_body_ticks,
            "managerPhaseTicks": (
                sum(manager_phase_ticks) if ticks_present else None
            ),
        }
        callback_calls_closed = callback_calls_closed and calls_closed
        callback_ticks_contained = (
            callback_ticks_contained and ticks_contained
        )

    kernel_population_calls = stage(
        "kernelStages", "inclusive", "calls"
    )
    bridge_pin_population_calls = sum(
        stage("bridgePinStages", name, "calls") or 0
        for name in _PRODUCTION_CALLBACK_NAMES
    )
    event_root_population_calls = sum(
        stage("eventRootStages", name, "calls") or 0
        for name in event_root_names
    )
    event_semantic_population_calls = stage(
        "eventSemanticStages", "semanticInclusive", "calls"
    )
    writer_expected_delta = (
        outer_population_calls + kernel_population_calls
        + bridge_pin_population_calls + event_root_population_calls
        + event_semantic_population_calls + event_dip_device_calls
        if isinstance(outer_population_calls, int)
        and isinstance(kernel_population_calls, int)
        and isinstance(event_semantic_population_calls, int)
        else None
    )
    writer_population_closed = bool(
        isinstance(writer_expected_delta, int)
        and writer_delta_started == writer_expected_delta
        and writer_delta_completed == writer_expected_delta
    )

    timing_records = [
        record for group in groups.values() for record in group.values()
    ]
    all_zero = bool(
        all_present and all(
            record.get("calls") == 0 and record.get("ticks") == 0
            for record in timing_records
        ) and all(value == 0 for value in reject_deltas.values())
        and writer_delta_started == 0 and writer_delta_completed == 0
    )
    evidence_positive = bool(
        isinstance(outer_population_calls, int) and outer_population_calls > 0
        and isinstance(dispatch_seal_calls, int)
        and dispatch_seal_calls > 0
        and (stage("kernelStages", "inclusive", "calls") or 0) > 0
        and sum(
            stage("bridgeBodyStages", name, "calls") or 0
            for name in _PRODUCTION_CALLBACK_NAMES
        ) > 0
        and sum(
            stage("managerBodyStages", name, "calls") or 0
            for name in _PRODUCTION_CALLBACK_NAMES
        ) > 0
    )
    structural_exact = bool(
        endpoint_contract_exact
        and frequency_match and sample_contract_exact and all_present
        and all_monotonic and writer_endpoints_clean
        and writer_window_closed and writer_population_closed
        and all_shape_valid
        and outer_calls_closed and outer_ticks_contained
        and dispatch_seal_values_present
        and dispatch_seal_calls_closed
        and dispatch_seal_ticks_contained
        and dispatch_seal_max_contained
        and dispatch_seal_cancel_zero
        and dispatch_seal_reported_endpoint_clean
        and kernel_calls_closed and kernel_ticks_contained
        and event_root_values_present
        and event_semantic_calls_closed
        and event_semantic_ticks_contained
        and event_semantic_max_contained
        and event_dip_values_present and event_dip_calls_closed
        and event_dip_ticks_contained and event_dip_max_contained
        and event_dip_total_calls_closed
        and callback_calls_closed and callback_ticks_contained
    )
    return {
        "valid": structural_exact,
        "frequencyMatch": frequency_match,
        "endpointContractExact": endpoint_contract_exact,
        "sampleContractExact": sample_contract_exact,
        "allPresent": all_present,
        "allMonotonic": all_monotonic,
        "allShapeValid": all_shape_valid,
        "frequency": native_frequency,
        "period": period,
        "phase": phase,
        "rawUploads": raw_uploads,
        "writer": {
            "previous": previous_writer,
            "current": current_writer,
            "deltaStarted": writer_delta_started,
            "deltaCompleted": writer_delta_completed,
            "present": writer_present,
            "monotonic": writer_monotonic,
            "endpointsClean": writer_endpoints_clean,
            "windowClosed": writer_window_closed,
            "expectedDelta": writer_expected_delta,
            "populationClosed": writer_population_closed,
        },
        "groups": groups,
        "managerRejected": reject_deltas,
        "outer": {
            "populationCalls": outer_population_calls,
            "callsClosed": outer_calls_closed,
            "ticksContained": outer_ticks_contained,
            "fastChildTicks": fast_child_ticks,
            "fastResidualTicks": fast_residual_ticks,
            "fallbackChildTicks": fallback_child_ticks,
            "fallbackResidualTicks": fallback_residual_ticks,
            "cadenceLower": cadence_lower,
            "cadenceUpper": cadence_upper,
            "cadenceExact": outer_cadence_exact,
        },
        "dispatchSeal": {
            "calls": dispatch_seal_calls,
            "valuesPresent": dispatch_seal_values_present,
            "callsClosed": dispatch_seal_calls_closed,
            "ticksContained": dispatch_seal_ticks_contained,
            "maxContained": dispatch_seal_max_contained,
            "cancelZero": dispatch_seal_cancel_zero,
            "reportedEndpointClean": dispatch_seal_reported_endpoint_clean,
        },
        "kernel": {
            "callsClosed": kernel_calls_closed,
            "ticksContained": kernel_ticks_contained,
        },
        "eventGraph": {
            "rootPresent": event_root_values_present,
            "rootPopulationCalls": event_root_population_calls,
            "semanticCallsClosed": event_semantic_calls_closed,
            "semanticTicksContained": event_semantic_ticks_contained,
            "semanticMaxContained": event_semantic_max_contained,
            "dipClasses": event_dip_contracts,
            "dipTotalDeviceCalls": event_dip_device_calls,
            "dipTotalBridgeCalls": event_dip_bridge_calls,
            "dipTotalCallsClosed": event_dip_total_calls_closed,
            "dipTicksContained": event_dip_ticks_contained,
            "dipMaxContained": event_dip_max_contained,
            "deltaEvidencePositive": event_evidence_positive,
            "endpointEvidencePositive": event_endpoint_evidence_positive,
        },
        "callbacks": callback_contracts,
        "callbackCallsClosed": callback_calls_closed,
        "callbackTicksContained": callback_ticks_contained,
        "allZero": all_zero,
        "evidencePositive": evidence_positive,
        "fullExact": structural_exact and all_zero,
        "lightExact": (
            structural_exact and not all_zero and evidence_positive
            and event_endpoint_evidence_positive
            and outer_cadence_exact
            and isinstance(writer_delta_started, int)
            and writer_delta_started > 0
        ),
    }


def _add_production_event_window_estimates(
    timing_delta: Dict[str, Any], frame_delta: Optional[int],
) -> None:
    frequency = timing_delta.get("frequency")
    period = timing_delta.get("period")
    if not (
        isinstance(frame_delta, int) and frame_delta > 0
        and isinstance(frequency, int) and frequency > 0
        and isinstance(period, int) and period > 0
    ):
        timing_delta["eventWindow"] = {
            "available": False,
            "frameDelta": frame_delta,
            "reason": "clean-pair frame/frequency/sample period unavailable",
        }
        return

    event_group_names = (
        "eventRootStages", "eventSemanticStages",
        "eventDipDeviceStages", "eventDipBridgeStages",
        "eventDipResolveStages",
    )
    groups = timing_delta.get("groups", {})
    estimates: Dict[str, Dict[str, Any]] = {}
    complete = True
    for group_name in event_group_names:
        group = groups.get(group_name, {})
        estimated_group: Dict[str, Any] = {}
        for stage_name, record in group.items():
            ticks = record.get("ticks")
            calls = record.get("calls")
            if not isinstance(ticks, int) or not isinstance(calls, int):
                complete = False
                continue
            estimated_total_ms = ticks * period * 1000.0 / frequency
            estimated_group[stage_name] = {
                "sampledCalls": calls,
                "sampledTicks": ticks,
                "estimatedWindowTotalMs": estimated_total_ms,
                "estimatedMsPerFrame": estimated_total_ms / frame_delta,
                "averageUsPerSampledCall": (
                    ticks * 1_000_000.0 / (frequency * calls)
                    if calls > 0 else None
                ),
            }
        estimates[group_name] = estimated_group

    inclusive = (
        ("eventRootStages", "flushRoot"),
        ("eventRootStages", "dispatchSemanticLookup"),
        ("eventRootStages", "dispatchBeginRoot"),
        ("eventRootStages", "dispatchEndRoot"),
        ("eventSemanticStages", "semanticInclusive"),
        ("eventDipDeviceStages", "outside"),
        ("eventDipDeviceStages", "noUpload"),
        ("eventDipDeviceStages", "correlated"),
    )
    ranked: List[Dict[str, Any]] = []
    for group_name, stage_name in inclusive:
        row = estimates.get(group_name, {}).get(stage_name)
        if not isinstance(row, dict):
            complete = False
            continue
        ranked.append({
            "group": group_name,
            "stage": stage_name,
            **row,
        })
    ranked.sort(
        key=lambda row: row["estimatedMsPerFrame"], reverse=True
    )
    contract_valid = timing_delta.get("valid") is True
    timing_delta["eventWindow"] = {
        "available": bool(complete and contract_valid),
        "reason": (
            None if complete and contract_valid
            else "production timing delta structural contract is not valid"
        ),
        "timeDomain": "forced clean-pair frame delta",
        "statisticalScaling": f"sampled ticks x{period}",
        "frameDelta": frame_delta,
        "frequency": frequency,
        "period": period,
        "groups": estimates,
        "rankedInclusiveEstimatedMsPerFrame": ranked,
    }


def _hot_path_timing_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    group_names = (
        "nativeStages", "nativeBeginSampleStages", "nativeT2SampleStages",
        "managerRootStages", "managerQueueStages",
        "managerBatchStages", "managerProofStages", "managerConsumerStages",
    )
    previous_frequency = previous.get("frequency", {})
    current_frequency = current.get("frequency", {})
    frequency_match = bool(
        previous_frequency.get("native") == current_frequency.get("native")
        and previous_frequency.get("nativeT2") ==
            current_frequency.get("nativeT2")
        and previous_frequency.get("manager") == current_frequency.get("manager")
        and current_frequency.get("native") == current_frequency.get("manager")
        and current_frequency.get("nativeT2") ==
            current_frequency.get("native")
        and isinstance(current_frequency.get("manager"), int)
        and current_frequency["manager"] > 0
    )
    groups: Dict[str, Dict[str, Any]] = {}
    all_monotonic = True
    ranked: List[Dict[str, Any]] = []
    for group_name in group_names:
        previous_group = previous.get(group_name, {})
        current_group = current.get(group_name, {})
        frequency_key = (
            "nativeT2" if group_name == "nativeT2SampleStages"
            else "native" if group_name.startswith("native") else "manager"
        )
        frequency = current_frequency.get(frequency_key)
        group_delta: Dict[str, Any] = {}
        for stage_name, current_record in current_group.items():
            delta = _raw_timing_delta(
                previous_group.get(stage_name, {}), current_record, frequency,
            )
            group_delta[stage_name] = delta
            all_monotonic = all_monotonic and bool(delta["monotonic"])
            # Prime-period sub-stages deliberately represent only one upload
            # in each sampling window. Keep their averages/deltas in the
            # artifact, but do not mix sampled totals into the inclusive
            # lifetime ranking beside full-population timers.
            if (delta["totalMs"] is not None
                    and group_name not in (
                        "nativeBeginSampleStages", "nativeT2SampleStages",
                    )):
                ranked.append({
                    "stage": f"{group_name}.{stage_name}",
                    "calls": delta["calls"],
                    "ticks": delta["ticks"],
                    "totalMs": delta["totalMs"],
                    "averageUs": delta["averageUs"],
                    "lifetimeMaximumUs": delta["lifetimeMaximumUs"],
                })
        groups[group_name] = group_delta

    def record(group: str, stage: str) -> Dict[str, Any]:
        delta = groups[group][stage]
        return {"ticks": delta.get("ticks")}

    closures = {
        "flushAssembly": _timing_contains(
            record("managerRootStages", "flush"),
            [
                record("managerQueueStages", "control"),
                record("managerQueueStages", "static"),
                record("managerRootStages", "prepare"),
                record("managerQueueStages", "collision"),
                record("managerBatchStages", "assemble"),
                record("managerBatchStages", "publish"),
            ],
        ),
        "prepareArray": _timing_contains(
            record("managerRootStages", "prepare"),
            [record("managerQueueStages", "scan")],
        ),
        "queueScan": _timing_contains(
            record("managerQueueStages", "scan"),
            [record("managerQueueStages", "positive")],
        ),
        "positiveCandidate": _timing_contains(
            record("managerQueueStages", "positive"),
            [
                record("managerQueueStages", "binding"),
                record("managerQueueStages", "staticLookup"),
                record("managerQueueStages", "paletteCopy"),
                record("managerQueueStages", "build"),
            ],
        ),
        "assemble": _timing_contains(
            record("managerBatchStages", "assemble"),
            [
                record("managerBatchStages", "lease"),
                record("managerBatchStages", "finalize"),
            ],
        ),
        "finalizeCompute": _timing_contains(
            record("managerBatchStages", "finalize"),
            [record("managerBatchStages", "upload")],
        ),
        "nativeOuter": _timing_contains(
            record("nativeStages", "outer"),
            [
                record("nativeStages", "begin"),
                record("nativeStages", "eval"),
                record("nativeStages", "cpuKernel"),
                record("nativeStages", "notify"),
                record("nativeStages", "complete"),
            ],
        ),
        "preflightEval": _timing_contains(
            record("nativeStages", "eval"),
            [
                record("managerProofStages", "preManager"),
                record("managerProofStages", "preHost"),
                record("managerProofStages", "preFinalize"),
            ],
        ),
        "preflightHostPlan": _timing_contains(
            record("managerProofStages", "preHost"),
            [record("managerConsumerStages", "plan")],
        ),
        "cpuProofNotify": _timing_contains(
            record("nativeStages", "notify"),
            [
                record("managerProofStages", "cpuManager"),
                record("managerProofStages", "cpuHost"),
                record("managerProofStages", "cpuFinalize"),
            ],
        ),
        "completion": _timing_contains(
            record("nativeStages", "complete"),
            [record("managerProofStages", "completion")],
        ),
        "validateStatic": _timing_contains(
            record("managerProofStages", "preManager"),
            [record("managerProofStages", "static")],
        ),
    }
    palette_parent_ticks = sum(
        record("managerProofStages", stage)["ticks"]
        for stage in ("preManager", "completion")
        if record("managerProofStages", stage)["ticks"] is not None
    )
    palette_ticks = record("managerProofStages", "palette")["ticks"]
    palette_available = palette_ticks is not None and all(
        record("managerProofStages", stage)["ticks"] is not None
        for stage in ("preManager", "completion")
    )
    closures["validatePalette"] = {
        "available": palette_available,
        "parentTicks": palette_parent_ticks if palette_available else None,
        "childTicks": palette_ticks if palette_available else None,
        "residualTicks": (
            palette_parent_ticks - palette_ticks if palette_available else None
        ),
        "contains": (
            palette_parent_ticks >= palette_ticks if palette_available else None
        ),
    }
    closures["nativeBeginSample"] = _native_begin_sample_closure(
        groups["nativeBeginSampleStages"]
    )
    closures["nativeT2Sample"] = _native_t2_sample_closure(
        groups["nativeBeginSampleStages"]["exact"],
        groups["nativeT2SampleStages"],
    )
    begin_sample_period_valid = (
        current.get("nativeBeginSamplePeriod") == 127
        and previous.get("nativeBeginSamplePeriod") == 127
    )
    t2_sample_period_valid = (
        current.get("nativeT2SamplePeriod") == 127
        and previous.get("nativeT2SamplePeriod") == 127
    )
    sample_periods_match = bool(
        current.get("nativeBeginSamplePeriod") ==
            current.get("nativeT2SamplePeriod")
        and previous.get("nativeBeginSamplePeriod") ==
            previous.get("nativeT2SamplePeriod")
    )
    all_closures_contain = all(
        closure["contains"] is True for closure in closures.values()
    )
    ranked.sort(key=lambda item: item["totalMs"], reverse=True)
    return {
        "valid": (
            frequency_match and begin_sample_period_valid
            and t2_sample_period_valid and sample_periods_match
            and all_monotonic and all_closures_contain
        ),
        "frequency": current_frequency,
        "frequencyMatch": frequency_match,
        "allMonotonic": all_monotonic,
        "allClosuresContain": all_closures_contain,
        "nativeBeginSamplePeriodValid": begin_sample_period_valid,
        "nativeT2SamplePeriodValid": t2_sample_period_valid,
        "nativeSamplePeriodsMatch": sample_periods_match,
        "nativeT2SampleEvidencePositive": closures[
            "nativeT2Sample"
        ]["evidencePositive"],
        "groups": groups,
        "closures": closures,
        "rankedByInclusiveTotalMs": ranked,
        "nativeBeginSamplePeriod": current.get("nativeBeginSamplePeriod"),
        "nativeT2SamplePeriod": current.get("nativeT2SamplePeriod"),
    }


def _production_fast_partition_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    names = (
        "rejectScope", "rejectState", "rejectSkinFormat", "rejectSmall",
        "rejectInput", "candidates", "dispatchSealUploads",
    )
    telemetry_names = (
        "outsideCallbacksSkipped", "outsideNativeFastPath",
        "directOriginalAttempts", "directOriginalKernelCalls",
        "directOriginalNormalReturns",
        "directOriginalKernelNoNormalReturns",
        "directOriginalCompleted", "directOriginalConflicts",
        "directOriginalCancellations",
        "directOriginalResetCompletedWhileActive",
        "directOriginalLatePoison",
        "kernelBatches", "poisonScanAttempts", "poisonNoOverlap",
        "poisonOverlap", "poisonReadFail", "markerConflicts",
        "coverFlush", "coverSemantic", "coverIndependent",
        "independentPinBegins", "independentPinEnds",
    )
    previous_raw = previous.get("rawUploads")
    current_raw = current.get("rawUploads")
    values_present = (
        isinstance(previous_raw, int) and isinstance(current_raw, int)
        and isinstance(previous.get("directOriginalActive"), int)
        and isinstance(current.get("directOriginalActive"), int)
        and all(
            isinstance(previous.get(name), int)
            and isinstance(current.get(name), int)
            for name in names + telemetry_names
        )
    )
    monotonic = bool(
        values_present and current_raw >= previous_raw
        and all(
            current[name] >= previous[name]
            for name in names + telemetry_names
        )
    )
    if not monotonic:
        return {
            "valid": False,
            "monotonic": False,
            "rawUploads": None,
            "classified": None,
            "closed": False,
        }
    raw_delta = current_raw - previous_raw
    deltas = {
        name: current[name] - previous[name] for name in names
    }
    telemetry_deltas = {
        name: current[name] - previous[name] for name in telemetry_names
    }
    classified = sum(deltas.values())
    return {
        "valid": raw_delta == classified,
        "monotonic": True,
        "rawUploads": raw_delta,
        "classified": classified,
        "closed": raw_delta == classified,
        "deltas": deltas,
        "telemetryDeltas": telemetry_deltas,
        "outsideCallbacksSkippedDelta": telemetry_deltas[
            "outsideCallbacksSkipped"
        ],
        "outsideNativeFastPathDelta": telemetry_deltas[
            "outsideNativeFastPath"
        ],
        "directOriginalDeltas": {
            name: telemetry_deltas[name]
            for name in telemetry_names
            if name.startswith("directOriginal")
        },
        "directOriginalActiveEndpoints": {
            "previous": previous["directOriginalActive"],
            "current": current["directOriginalActive"],
        },
        "kernelBatchesDelta": telemetry_deltas["kernelBatches"],
        "poisonScanAttemptsDelta": telemetry_deltas[
            "poisonScanAttempts"
        ],
        "poisonNoOverlapDelta": telemetry_deltas["poisonNoOverlap"],
        "poisonOverlapDelta": telemetry_deltas["poisonOverlap"],
        "poisonReadFailDelta": telemetry_deltas["poisonReadFail"],
        "markerConflictsDelta": telemetry_deltas["markerConflicts"],
        "coverDeltas": {
            "flush": telemetry_deltas["coverFlush"],
            "semantic": telemetry_deltas["coverSemantic"],
            "independent": telemetry_deltas["coverIndependent"],
        },
        "independentPinBeginsDelta": telemetry_deltas[
            "independentPinBegins"
        ],
        "independentPinEndsDelta": telemetry_deltas[
            "independentPinEnds"
        ],
    }


def _production_sample_timing_policy_contract(
    previous_diag: Dict[str, Any], current_diag: Dict[str, Any],
    timing_delta: Dict[str, Any],
) -> Dict[str, Any]:
    full = bool(
        previous_diag.get("diagnosticPolicy", {}).get("fullExact") is True
        and current_diag.get("diagnosticPolicy", {}).get("fullExact") is True
    )
    light = bool(
        previous_diag.get("diagnosticPolicy", {}).get("lightExact") is True
        and current_diag.get("diagnosticPolicy", {}).get("lightExact") is True
    )
    recognized_exact = full != light
    exact = bool(
        recognized_exact and timing_delta.get("valid") is True
        and (
            full and timing_delta.get("fullExact") is True
            or light and timing_delta.get("lightExact") is True
        )
    )
    return {
        "full": full,
        "light": light,
        "recognizedExact": recognized_exact,
        "exact": exact,
    }


def _clean_pair_production_sample_selection_contract(
    previous_diag: Dict[str, Any], current_diag: Dict[str, Any],
) -> Dict[str, Any]:
    """Require the existing production sampler gate before ending capture.

    A quiescent chronological pair is necessary but not sufficient in light
    mode: the bounded pair can land between two 1/256 dispatch-seal samples.
    Keep collecting in that case instead of selecting a pair which is already
    known to fail the unchanged final productionSampleTimingExact hard gate.
    """
    fast_delta = _production_fast_partition_delta(
        previous_diag.get("productionFastPath", {}),
        current_diag.get("productionFastPath", {}),
    )
    direct_completed = fast_delta.get("directOriginalDeltas", {}).get(
        "directOriginalCompleted"
    )
    timed_raw_uploads = (
        fast_delta.get("rawUploads") - direct_completed
        if isinstance(fast_delta.get("rawUploads"), int)
        and isinstance(direct_completed, int)
        and 0 <= direct_completed <= fast_delta["rawUploads"]
        else None
    )
    timing_delta = _production_sample_timing_delta(
        previous_diag.get("productionSampleTiming", {}),
        current_diag.get("productionSampleTiming", {}),
        timed_raw_uploads,
    )
    policy = _production_sample_timing_policy_contract(
        previous_diag, current_diag, timing_delta,
    )
    dispatch_seal = timing_delta.get("dispatchSeal", {})
    writer = timing_delta.get("writer", {})
    outer = timing_delta.get("outer", {})
    return {
        **policy,
        "fastPartitionValid": fast_delta.get("valid") is True,
        "rawUploads": fast_delta.get("rawUploads"),
        "timingValid": timing_delta.get("valid") is True,
        "fullExact": timing_delta.get("fullExact") is True,
        "lightExact": timing_delta.get("lightExact") is True,
        "allZero": timing_delta.get("allZero") is True,
        "evidencePositive": timing_delta.get("evidencePositive") is True,
        "dispatchSealCalls": dispatch_seal.get("calls"),
        "dispatchSealCallsClosed": dispatch_seal.get("callsClosed") is True,
        "writerDeltaStarted": writer.get("deltaStarted"),
        "writerPopulationClosed": writer.get("populationClosed") is True,
        "outerPopulationCalls": outer.get("populationCalls"),
        "outerCadenceExact": outer.get("cadenceExact") is True,
    }


def _manager_dispatch_telemetry_delta(
    previous: Dict[str, Any], current: Dict[str, Any],
    fast_partition_delta: Dict[str, Any],
    previous_seal: Dict[str, Any], current_seal: Dict[str, Any],
    previous_authority: Optional[Dict[str, Any]] = None,
    current_authority: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    # These are exactly the scalar AddNativeTelemetryCounter call sites. The
    # outside-upload and outside-kernel bundles contribute another exact
    # 11+2 additions per completed outsideNativeFastPath event. A successful
    # generic production-reject kernel bundle contributes its marker plus the
    # two historical kernel counters, for exactly three more additions. The
    # exact outer-admission classifier separately publishes one attempt term,
    # later cancellation/lifecycle-exclusion terms, and the two final-boundary
    # cross-population terminal terms. Production O1's 15 normal-path
    # diagnostic counters are supplied separately and must be added exactly;
    # active/reject/reset/evidence counters remain immediate atomics.
    batched_names = (
        "physicalScopes", "physicalEnds", "commonScopes", "specialScopes",
        "semanticScopes", "eagerScopes", "lazyScopes", "neverScopes",
        "nativeCpuOnlyScopes", "nativeCpuOnlyEnds",
        "evidenceEagerScopes", "beginCallbacks", "endCallbacks",
        "eagerBegins", "lazyAdmissionAttempts", "lazyAdmissions",
        "issuedEnds", "noUploadEnds", "neverEnds", "skippedUploads",
        "skippedDips", "skippedFanouts", "rawDips",
        "outsideDips", "noUploadDips", "correlatedDips", "unmatchedDips",
        "outsideDipFastPath", "observerBegins", "observerEnds",
        "readerBegins", "readerEnds", "readerCommits", "readerRejects",
        "readerEvidenceFallbacks", "fastByFlush", "fastByObserver",
        "fastByReader",
    )
    telemetry_names = ("telemetryFlushes", "telemetryBatchedAdds")
    outer_exact_names = (
        "outsideAdmissionAttemptTotal",
        "outsideAdmissionCancellations",
        "outsideAdmissionLifecycleExcluded",
        "outsideAdmissionTrackedResolvedInside",
        "outsideAdmissionUntrackedResolvedOutside",
    )
    endpoint_state_names = (
        "telemetryDeltaPending", "telemetryDeltaFaulted",
    )
    seal_batched_names = (
        "proposals", "proposalAccepted", "proposalRejected",
        "proposalAborted", "scopeCommits", "scopeEnds",
        "localViewQueries", "localViewAuthorityRejects",
        "localViewCandidateRejects", "localViewCommits",
        "invalidations", "uploadsStarted", "uploadsCompleted",
        "vertices", "bytes", "kernelCalls", "kernelNormalReturns",
        "dips", "dipsWithUpload", "dipsNoUpload", "fanoutZero",
        "fanoutOne", "fanoutMany", "fanoutDipTotal",
        "markerConflicts",
    )
    authority_batched_paths = {
        "attempts": ("attempts",),
        "created": ("created",),
        "armed": ("armed",),
        "settled": ("settled",),
        "lockNotifications": ("lock", "notifications"),
        "lockNoOverlap": ("lock", "noOverlap"),
        "lockOverlap": ("lock", "overlap"),
        "kernelReady": ("kernel", "ready"),
        "normalReturns": ("kernel", "normalReturns"),
        "unlockNotifications": ("unlock", "notifications"),
        "unlockExact": ("unlock", "exact"),
        "committedNoOverlap": ("commit", "noOverlap"),
        "committedRewrite": ("commit", "rewrite"),
        "poisonClears": ("commit", "poisonClears"),
        "authority": ("commit", "authority"),
    }

    def authority_value(
        snapshot: Dict[str, Any], path: Tuple[str, ...],
    ) -> Any:
        value: Any = snapshot
        for name in path:
            if not isinstance(value, dict):
                return None
            value = value.get(name)
        return value

    authority_supplied = bool(
        previous_authority is not None or current_authority is not None
    )
    authority_values_present = bool(
        not authority_supplied
        or isinstance(previous_authority, dict)
        and isinstance(current_authority, dict)
        and all(
            isinstance(authority_value(snapshot, path), int)
            for snapshot in (previous_authority, current_authority)
            for path in authority_batched_paths.values()
        )
    )
    authority_monotonic = bool(
        authority_values_present
        and (
            not authority_supplied
            or all(
                authority_value(current_authority, path) >=
                    authority_value(previous_authority, path)
                for path in authority_batched_paths.values()
            )
        )
    )
    values_present = authority_values_present and all(
        isinstance(snapshot.get(name), int)
        for snapshot in (previous, current)
        for name in (
            batched_names + telemetry_names + endpoint_state_names
            + outer_exact_names
        )
    ) and all(
        isinstance(snapshot.get(name), int)
        for snapshot in (previous_seal, current_seal)
        for name in seal_batched_names
    )
    monotonic = bool(
        values_present and all(
            current[name] >= previous[name]
            for name in batched_names + telemetry_names + outer_exact_names
        )
        and all(
            current_seal[name] >= previous_seal[name]
            for name in seal_batched_names
        )
        and authority_monotonic
    )
    outside_fast_delta = fast_partition_delta.get(
        "outsideNativeFastPathDelta"
    )
    reject_kernel_batch_delta = fast_partition_delta.get(
        "kernelBatchesDelta"
    )
    fast_delta_valid = bool(
        fast_partition_delta.get("valid") is True
        and isinstance(outside_fast_delta, int)
        and outside_fast_delta >= 0
        and isinstance(reject_kernel_batch_delta, int)
        and reject_kernel_batch_delta >= 0
    )
    endpoints_clean = bool(
        values_present and all(
            snapshot[name] == 0
            for snapshot in (previous, current)
            for name in endpoint_state_names
        )
    )
    authority_batched_deltas = (
        {
            name: authority_value(current_authority, path)
                - authority_value(previous_authority, path)
            for name, path in authority_batched_paths.items()
        }
        if authority_supplied and authority_monotonic else {}
    )
    if not monotonic or not fast_delta_valid:
        return {
            "present": values_present,
            "monotonic": monotonic,
            "endpointsClean": endpoints_clean,
            "valid": False,
            "exactAdds": False,
            "deltas": {},
            "flushesDelta": None,
            "batchedAddsDelta": None,
            "outsideNativeFastPathDelta": outside_fast_delta,
            "rejectKernelBatchesDelta": reject_kernel_batch_delta,
            "outsideAdmissionDeltas": {},
            "outsidePoisonAuthorityBatchedDeltas": {},
            "outsidePoisonAuthorityBatchedPresent": (
                authority_supplied and authority_values_present
            ),
            "expectedBatchedAddsDelta": None,
        }
    deltas = {
        name: current[name] - previous[name] for name in batched_names
    }
    outer_exact_deltas = {
        name: current[name] - previous[name] for name in outer_exact_names
    }
    seal_deltas = {
        name: current_seal[name] - previous_seal[name]
        for name in seal_batched_names
    }
    dip_semantic_closed = bool(
        deltas["rawDips"] ==
            deltas["correlatedDips"] + deltas["unmatchedDips"]
        and deltas["outsideDips"] + deltas["noUploadDips"] <=
            deltas["unmatchedDips"]
        and deltas["outsideDipFastPath"] <= deltas["outsideDips"]
        and deltas["outsideDipFastPath"] ==
            deltas["fastByFlush"] + deltas["fastByObserver"]
            + deltas["fastByReader"]
        and deltas["observerBegins"] == deltas["observerEnds"]
        and deltas["readerBegins"] == deltas["readerEnds"]
        and deltas["readerBegins"] == (
            deltas["readerCommits"] + deltas["readerRejects"]
        )
        and deltas["readerEvidenceFallbacks"] <=
            deltas["readerRejects"]
        and deltas["readerCommits"] == deltas["fastByReader"]
    )
    dispatch_seal_closed = bool(
        seal_deltas["proposals"] ==
            seal_deltas["proposalAccepted"]
            + seal_deltas["proposalRejected"]
        and seal_deltas["localViewQueries"] ==
            seal_deltas["localViewAuthorityRejects"]
            + seal_deltas["localViewCandidateRejects"]
            + seal_deltas["localViewCommits"]
        and seal_deltas["proposalAccepted"]
            + seal_deltas["localViewCommits"] ==
            seal_deltas["scopeCommits"]
            + seal_deltas["proposalAborted"]
        and seal_deltas["scopeCommits"] == seal_deltas["scopeEnds"]
        and seal_deltas["localViewCommits"] ==
            deltas["nativeCpuOnlyScopes"]
        and deltas["nativeCpuOnlyScopes"] ==
            deltas["nativeCpuOnlyEnds"]
        and seal_deltas["uploadsStarted"] ==
            seal_deltas["uploadsCompleted"]
        and seal_deltas["uploadsCompleted"] == (
            seal_deltas["fanoutZero"] + seal_deltas["fanoutOne"]
            + seal_deltas["fanoutMany"]
        )
        and seal_deltas["kernelCalls"] ==
            seal_deltas["kernelNormalReturns"] ==
            seal_deltas["uploadsStarted"]
        and seal_deltas["dips"] ==
            seal_deltas["dipsWithUpload"]
            + seal_deltas["dipsNoUpload"]
        and seal_deltas["fanoutDipTotal"] ==
            seal_deltas["dipsWithUpload"]
        and seal_deltas["uploadsCompleted"] ==
            fast_partition_delta.get("deltas", {}).get(
                "dispatchSealUploads"
            )
        and seal_deltas["markerConflicts"] == 0
    )
    flushes_delta = (
        current["telemetryFlushes"] - previous["telemetryFlushes"]
    )
    batched_adds_delta = (
        current["telemetryBatchedAdds"] -
        previous["telemetryBatchedAdds"]
    )
    expected_adds_delta = (
        sum(deltas.values()) + 13 * outside_fast_delta
        + 3 * reject_kernel_batch_delta
        + sum(
            fast_partition_delta.get("directOriginalDeltas", {}).get(
                name, 0
            )
            for name in (
                "directOriginalAttempts",
                "directOriginalKernelCalls",
                "directOriginalNormalReturns",
                "directOriginalKernelNoNormalReturns",
                "directOriginalCompleted",
            )
        )
        + sum(outer_exact_deltas.values())
        + sum(authority_batched_deltas.values())
        + sum(seal_deltas.values())
        # Independent seal counters above omit the global raw aliases that the
        # same physical native upload/kernel/DIP/fanout must still publish.
        + 2 * seal_deltas["kernelCalls"]
        + 5 * seal_deltas["uploadsCompleted"]
        + 2 * seal_deltas["bytes"]
        + sum(fast_partition_delta.get("coverDeltas", {}).values())
        + fast_partition_delta.get("independentPinBeginsDelta", 0)
        + fast_partition_delta.get("independentPinEndsDelta", 0)
    )
    exact_adds = batched_adds_delta == expected_adds_delta
    return {
        "present": True,
        "monotonic": True,
        "endpointsClean": endpoints_clean,
        "valid": endpoints_clean,
        "exactAdds": exact_adds,
        "dipSemanticClosed": dip_semantic_closed,
        "dispatchSealClosed": dispatch_seal_closed,
        "deltas": deltas,
        "flushesDelta": flushes_delta,
        "batchedAddsDelta": batched_adds_delta,
        "outsideNativeFastPathDelta": outside_fast_delta,
        "rejectKernelBatchesDelta": reject_kernel_batch_delta,
        "outsideAdmissionDeltas": outer_exact_deltas,
        "outsidePoisonAuthorityBatchedDeltas": authority_batched_deltas,
        "outsidePoisonAuthorityBatchedPresent": bool(
            authority_supplied and authority_values_present
        ),
        "dispatchSealDeltas": seal_deltas,
        "expectedBatchedAddsDelta": expected_adds_delta,
    }


def _manager_dispatch_telemetry_policy(
    telemetry_delta: Dict[str, Any],
    diagnostic_full: bool,
    diagnostic_light: bool,
) -> Dict[str, Any]:
    """Apply the diagnostic mode's exact TLS-publication contract."""
    result = dict(telemetry_delta)
    recognized_exact = diagnostic_full != diagnostic_light
    result["full"] = diagnostic_full
    result["light"] = diagnostic_light
    result["recognizedExact"] = recognized_exact
    result["lightModeExpectedBatchedAddsDelta"] = result.get(
        "expectedBatchedAddsDelta"
    )
    result["batchingEnabled"] = bool(
        diagnostic_light and not diagnostic_full
    )
    if diagnostic_full and not diagnostic_light:
        # Full diagnostics deliberately disable the TLS batching producer.
        # Ordinary diagnostic counters still advance through immediate
        # atomics, so the light-mode exact-add formula is report-only here.
        result["expectedBatchedAddsDelta"] = 0
        result["exactAdds"] = bool(result.get("batchedAddsDelta") == 0)
    result["exact"] = bool(
        recognized_exact
        and result.get("valid") is True
        and result.get("dipSemanticClosed") is True
        and result.get("dispatchSealClosed") is True
        and (
            diagnostic_full
            and result.get("flushesDelta") == 0
            and result.get("batchedAddsDelta") == 0
            and result.get("exactAdds") is True
            or diagnostic_light
            and result.get("exactAdds") is True
            and isinstance(result.get("flushesDelta"), int)
            and isinstance(result.get("batchedAddsDelta"), int)
            and 0 < result["flushesDelta"] <= result["batchedAddsDelta"]
        )
    )
    return result


def _outside_admission_pair_contract(
    previous: Dict[str, Any], current: Dict[str, Any],
    diagnostic_full: bool, diagnostic_light: bool,
) -> Dict[str, Any]:
    scalar_paths = (
        ("accepted", "noPoison"),
        ("accepted", "withPoison"),
        ("accepted", "total"),
        ("reportedTotals", "rejectNoPoison"),
        ("reportedTotals", "rejectWithPoison"),
        ("reportedTotals", "attempt"),
        ("reportedTotals", "trackedResolvedOutside"),
        ("reportedTotals", "resolvedExpectedOutside"),
        ("reportedTotals", "actualOutside"),
    )

    def scalar(snapshot: Dict[str, Any], owner: str, name: str) -> Any:
        value = snapshot.get(owner, {}).get(name)
        return value

    scalar_pairs = [
        (scalar(previous, owner, name), scalar(current, owner, name))
        for owner, name in scalar_paths
    ]
    scalar_pairs.extend((
        (previous.get("cancellations"), current.get("cancellations")),
        (
            previous.get("lifecycleExcluded"),
            current.get("lifecycleExcluded"),
        ),
        (
            previous.get("trackedResolvedInside"),
            current.get("trackedResolvedInside"),
        ),
        (
            previous.get("untrackedResolvedOutside"),
            current.get("untrackedResolvedOutside"),
        ),
    ))
    for table_name in ("rejectNoPoison", "rejectWithPoison"):
        scalar_pairs.extend(
            (
                previous.get(table_name, {}).get(name),
                current.get(table_name, {}).get(name),
            )
            for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
        )

    alias_pairs: List[Tuple[Any, Any]] = []
    for table_name, names in (
        ("acceptedByClass", _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES),
        ("completeByClass", _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES),
        ("fallbackByReason", _GPU_SKIN_OUTSIDE_REJECT_REASONS),
    ):
        previous_table = previous.get("aliases", {}).get(table_name, {})
        current_table = current.get("aliases", {}).get(table_name, {})
        for name in names:
            for field in ("calls", "ticks", "maxTicks"):
                alias_pairs.append((
                    previous_table.get(name, {}).get(field),
                    current_table.get(name, {}).get(field),
                ))
    all_pairs = scalar_pairs + alias_pairs
    present = bool(
        previous.get("present") is True and current.get("present") is True
        and all(
            isinstance(before, int) and isinstance(after, int)
            for before, after in all_pairs
        )
    )
    monotonic = bool(
        present and all(after >= before for before, after in all_pairs)
    )
    attempt_before = scalar(previous, "reportedTotals", "attempt")
    attempt_after = scalar(current, "reportedTotals", "attempt")
    attempt_delta = (
        attempt_after - attempt_before
        if isinstance(attempt_before, int) and isinstance(attempt_after, int)
        and attempt_after >= attempt_before else None
    )
    endpoint_policy_clean = bool(
        previous.get("policyClean") is True
        and current.get("policyClean") is True
    )
    return {
        "present": present,
        "monotonic": monotonic,
        "full": diagnostic_full,
        "light": diagnostic_light,
        "attemptDelta": attempt_delta,
        "endpointPolicyClean": endpoint_policy_clean,
        "exact": bool(
            endpoint_policy_clean and present and monotonic
            and diagnostic_full != diagnostic_light
            and (
                diagnostic_full and attempt_delta == 0
                or diagnostic_light and isinstance(attempt_delta, int)
                and attempt_delta > 0
            )
        ),
    }


def _native_begin_sample_cadence_contract(
    timing_delta: Dict[str, Any], fast_partition_delta: Dict[str, Any],
) -> Dict[str, Any]:
    raw_uploads = fast_partition_delta.get("rawUploads")
    seal_uploads = fast_partition_delta.get("deltas", {}).get(
        "dispatchSealUploads"
    )
    generic_uploads = (
        raw_uploads - seal_uploads
        if isinstance(raw_uploads, int) and isinstance(seal_uploads, int)
        else None
    )
    common_calls = (
        timing_delta.get("groups", {})
        .get("nativeBeginSampleStages", {})
        .get("common", {})
        .get("calls")
    )
    period = timing_delta.get("nativeBeginSamplePeriod")
    present = bool(
        isinstance(raw_uploads, int)
        and isinstance(seal_uploads, int)
        and isinstance(generic_uploads, int)
        and isinstance(common_calls, int)
        and isinstance(period, int)
    )
    lower = (
        generic_uploads // 127
        if present and generic_uploads >= 0 else None
    )
    upper = (
        (generic_uploads + 126) // 127
        if present and generic_uploads >= 0 else None
    )
    exact = bool(
        present and period == 127 and generic_uploads >= 0
        and common_calls >= 0
        and lower <= common_calls <= upper
    )
    return {
        "present": present,
        "exact": exact,
        "period": period,
        "rawUploads": raw_uploads,
        "dispatchSealUploads": seal_uploads,
        "genericUploads": generic_uploads,
        "commonCalls": common_calls,
        "lowerBound": lower,
        "upperBound": upper,
    }


def _full_population_native_timing_contract(
    timing_delta: Dict[str, Any], fast_partition_delta: Dict[str, Any],
) -> Dict[str, Any]:
    required_stages = ("begin", "eval", "complete", "outer")
    raw_uploads = fast_partition_delta.get("rawUploads")
    native_stages = (
        timing_delta.get("groups", {}).get("nativeStages", {})
    )
    stage_calls = {
        name: native_stages.get(name, {}).get("calls")
        for name in required_stages
    }
    present = bool(
        isinstance(raw_uploads, int)
        and all(isinstance(value, int) for value in stage_calls.values())
    )
    exact = bool(
        present and raw_uploads > 0
        and all(value == raw_uploads for value in stage_calls.values())
    )
    return {
        "present": present,
        "exact": exact,
        "rawUploads": raw_uploads,
        "stageCalls": stage_calls,
    }


_FULL_TIMING_DELTA_STAGES = {
    "nativeStages": (
        "begin", "eval", "complete", "notify", "dip", "outer", "cpuKernel",
    ),
    "managerRootStages": ("flush", "host", "prepare"),
    "managerQueueStages": (
        "control", "static", "scan", "collision", "positive", "binding",
        "staticLookup", "paletteCopy", "build",
    ),
    "managerBatchStages": (
        "query", "hostFinalize", "assemble", "lease", "finalize", "upload",
        "publish",
    ),
    "managerProofStages": (
        "preManager", "preHost", "preFinalize", "palette", "static",
        "cpuManager", "cpuHost", "cpuFinalize", "completion",
    ),
    "managerConsumerStages": (
        "resolve", "shadow", "plan", "commit", "fail", "close",
        "drawResult", "fuse", "terminate",
    ),
}


def _light_zero_timing_contract(
    timing_delta: Dict[str, Any], fast_partition_delta: Dict[str, Any],
) -> Dict[str, Any]:
    raw_uploads = fast_partition_delta.get("rawUploads")
    groups = timing_delta.get("groups", {})
    stage_values: Dict[str, Dict[str, Any]] = {}
    present = True
    all_zero = True
    all_monotonic = True
    for group_name, stage_names in _FULL_TIMING_DELTA_STAGES.items():
        group = groups.get(group_name, {})
        for stage_name in stage_names:
            record = group.get(stage_name, {})
            calls = record.get("calls")
            ticks = record.get("ticks")
            monotonic = record.get("monotonic")
            key = f"{group_name}.{stage_name}"
            stage_values[key] = {
                "calls": calls,
                "ticks": ticks,
                "monotonic": monotonic,
            }
            stage_present = (
                isinstance(calls, int)
                and isinstance(ticks, int)
                and isinstance(monotonic, bool)
            )
            present = present and stage_present
            all_zero = all_zero and stage_present and calls == 0 and ticks == 0
            all_monotonic = all_monotonic and monotonic is True
    active_window = isinstance(raw_uploads, int) and raw_uploads > 0
    fast_partition_clean = bool(
        fast_partition_delta.get("valid") is True
        and fast_partition_delta.get("closed") is True
    )
    exact = bool(
        timing_delta.get("valid") is True
        and present and all_zero and all_monotonic
        and active_window and fast_partition_clean
    )
    return {
        "present": present,
        "exact": exact,
        "allZero": all_zero,
        "allMonotonic": all_monotonic,
        "activeWindow": active_window,
        "fastPartitionClean": fast_partition_clean,
        "rawUploads": raw_uploads,
        "stageValues": stage_values,
    }


def _latest_diag_lines(text: str) -> Dict[str, str]:
    latest: Dict[str, str] = {}
    for raw in _strip_ansi(text).splitlines():
        line = raw.strip()
        match = re.search(r"DXVK War3GpuSkin: diag ([A-Za-z0-9_]+)", line)
        if match:
            key = match.group(1)
            if key == "nativePoisonO1ScannerLane":
                lane = re.search(r"\blane=([A-Za-z0-9_]+)", line)
                lane_name = lane.group(1) if lane else "invalid"
                latest[f"{key}:{lane_name}"] = line
                continue
            if key != "indexTicket":
                latest[key] = line
                continue

            # indexTicket emits a structured main line followed by reason detail
            # lines under the same diagnostic key. Keep them independently so a
            # later reasonA/reasonB line cannot replace the required mask line.
            if re.search(r"\bmask\s*=", line, re.IGNORECASE):
                latest[key] = line
                continue
            reason = re.search(r"\b(reason[AB])\s*=", line, re.IGNORECASE)
            if reason:
                latest[f"{key}Reason{reason.group(1)[-1].upper()}"] = line
            else:
                latest[f"{key}Detail"] = line
    return latest


FORCED_UNIQUE_DIAG_KEYS = (
    "quiescencePre", "mode", "resolve", "compute", "vsRoute",
    "lifetime", "consume",
    "ledger", "ledgerReason", "latePoison", "latePoisonNative",
    "latePoisonStorage", "latePoisonDump", "nativeSafety", "nativePoison",
    "nativePoisonSidecarPolicy",
    "nativePoisonShadow", "nativePoisonShadowReasons",
    "nativePoisonO1Shadow", "nativePoisonO1ShadowReasons",
    "nativePoisonO1Authority", "nativePoisonO1AuthorityEvidence",
    "nativePoisonO1DiscardJoint",
    "nativePoisonO1Scanner", "nativePoisonO1ScannerReasons",
    "nativePoisonO1Physical",
    "nativeFast", "nativeManagerDispatch", "nativeDispatchSeal",
    "nativeTiming",
    "nativeOuterAdmissionExact", "nativeOuterRejectNoPoison",
    "nativeOuterRejectWithPoison",
    "nativeBeginSample", "nativeT2Sample",
    "nativeProdOuter", "nativeProdDispatchSeal", "nativeProdFallback",
    "nativeProdKernel",
    "nativeProdEventRoot", "nativeProdEventSemantic",
    "nativeProdEventDipDevice", "nativeProdEventDipBridge",
    "nativeProdEventDipResolve",
    "nativeProdCallbackPin", "nativeProdCallbackBody",
    "nativeProdCallbackLeave", "managerProdCallbackEnter",
    "nativeProdOuterAdmitClassCalls",
    "nativeProdOuterAdmitClassTicks",
    "nativeProdOuterAdmitClassMax",
    "nativeProdOuterCompleteClassCalls",
    "nativeProdOuterCompleteClassTicks",
    "nativeProdOuterCompleteClassMax",
    "nativeProdFallbackReasonCalls",
    "nativeProdFallbackReasonTicks",
    "nativeProdFallbackReasonMax",
    "nativeProdOuterAliasClosure",
    "managerProdCallbackBody", "managerProdCallbackLeave",
    "managerProdCallbackReject",
    "managerHot", "managerQueueTime",
    "managerBatchTime", "managerProofTime", "managerConsumerTime",
    "nativeRetirement", "nativeReset", "nativeKernelNormal", "tracker",
    "format", "dipFastProbe", "formatFlow", "formatClass",
    "formatReject0", "formatReject1", "formatReject2",
    "formatReject3", "formatReject4", "formatReject5",
    "inputPreflight", "strictReject", "parityFormat",
    "quiescencePost",
)


def _forced_diag_block_contract(text: str) -> Dict[str, Any]:
    counts: Dict[str, int] = {}
    o1_lane_counts: Dict[str, int] = {}
    nested_markers = 0
    index_ticket_main = 0
    for raw in _strip_ansi(text).splitlines():
        line = raw.strip()
        if "DXVK War3GpuSkin: diagSnapshot " in line:
            nested_markers += 1
        match = re.search(r"DXVK War3GpuSkin: diag ([A-Za-z0-9_]+)", line)
        if not match:
            continue
        key = match.group(1)
        counts[key] = counts.get(key, 0) + 1
        if key == "nativePoisonO1ScannerLane":
            lane = re.search(r"\blane=([A-Za-z0-9_]+)", line)
            lane_name = lane.group(1) if lane else "invalid"
            o1_lane_counts[lane_name] = o1_lane_counts.get(lane_name, 0) + 1
        if key == "indexTicket" and re.search(r"\bmask\s*=", line, re.I):
            index_ticket_main += 1
    violations = {
        key: counts.get(key, 0)
        for key in FORCED_UNIQUE_DIAG_KEYS
        if counts.get(key, 0) != 1
    }
    if index_ticket_main != 1:
        violations["indexTicketMain"] = index_ticket_main
    if counts.get("nativePoisonO1ScannerLane", 0) != len(
        NATIVE_POISON_O1_LOCK_LANES
    ):
        violations["nativePoisonO1ScannerLane"] = counts.get(
            "nativePoisonO1ScannerLane", 0
        )
    for lane_name in NATIVE_POISON_O1_LOCK_LANES:
        if o1_lane_counts.get(lane_name, 0) != 1:
            violations[
                f"nativePoisonO1ScannerLane:{lane_name}"
            ] = o1_lane_counts.get(lane_name, 0)
    unexpected_lanes = sorted(
        set(o1_lane_counts) - set(NATIVE_POISON_O1_LOCK_LANES)
    )
    if unexpected_lanes:
        violations["nativePoisonO1ScannerLane:unexpected"] = len(
            unexpected_lanes
        )
    return {
        "ok": not violations and nested_markers == 0,
        "counts": counts,
        "violations": violations,
        "nativePoisonO1ScannerLaneCounts": o1_lane_counts,
        "nestedSnapshotMarkers": nested_markers,
    }


def _extract_forced_diag_block(
    lines: List[str], snapshot_id: str, complete: int = 1,
) -> Dict[str, Any]:
    begin_marker = (
        f"DXVK War3GpuSkin: diagSnapshot begin={snapshot_id}"
        if snapshot_id else ""
    )
    end_marker = (
        f"DXVK War3GpuSkin: diagSnapshot end={snapshot_id} complete={complete}"
        if snapshot_id else ""
    )
    begin_indices = [
        index for index, line in enumerate(lines)
        if begin_marker and _gpu_skin_diag_payload(line) == begin_marker
    ]
    end_indices = [
        index for index, line in enumerate(lines)
        if end_marker and _gpu_skin_diag_payload(line) == end_marker
    ]
    order_valid = bool(
        len(begin_indices) == 1 and len(end_indices) == 1
        and begin_indices[0] < end_indices[0]
    )
    text = ""
    if order_valid:
        text = "\n".join(
            _gpu_skin_diag_payload(line)
            for line in lines[begin_indices[0] + 1:end_indices[0]]
        )
    return {
        "beginMarker": begin_marker,
        "endMarker": end_marker,
        "beginSeen": len(begin_indices) == 1,
        "endSeen": len(end_indices) == 1,
        "orderValid": order_valid,
        "text": text,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest()
        if text else "",
        "contract": _forced_diag_block_contract(text),
    }


def _forced_diag_block_complete(block: Dict[str, Any]) -> bool:
    return bool(
        block.get("orderValid")
        and dict(block.get("contract", {}) or {}).get("ok")
    )


def _parse_late_poison_samples(text: str) -> Dict[str, Any]:
    samples: List[Dict[str, Any]] = []
    seen_ids: set[int] = set()
    duplicate_ids = 0
    for raw in _strip_ansi(text).splitlines():
        line = raw.strip()
        if "DXVK War3GpuSkin: diag latePoisonSample" not in line:
            continue
        sample_id = _match_named_int(line, "id")
        if sample_id is not None:
            if sample_id in seen_ids:
                duplicate_ids += 1
            seen_ids.add(sample_id)
        current_epoch = _match_tuple(
            line, r"\bcurrentEpoch=(\d+)/(\d+)/(\d+)/(\d+)", 4
        )
        poison_epoch = _match_tuple(
            line, r"\bpoisonEpoch=(\d+)/(\d+)/(\d+)", 3
        )
        real = _match_tuple(line, r"\breal=(\d+)/(\d+)", 2)
        mapping = _match_tuple(line, r"\bmapping=(\d+)/(\d+)", 2)
        map_allocation = _match_tuple(
            line, r"\bmapAllocation=(\d+)/(\d+)", 2
        )
        map_mode = _match_tuple(line, r"\bmapMode=(\d+)/(\d+)", 2)
        lock = _match_tuple(
            line,
            r"\block=(\d+):(0x[0-9a-fA-F]+)/(\d+):(0x[0-9a-fA-F]+)",
            4,
        )
        mixed = _match_tuple(
            line, r"\bmixed=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
        )
        vertex = _match_tuple(
            line, r"\bvertex=(-?\d+)/(\d+)/(\d+)", 3
        )
        poison_vertex = _match_tuple(
            line, r"\bpoisonVertex=(\d+)/(\d+)", 2
        )
        index = _match_tuple(
            line, r"\bindex=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
        )
        samples.append({
            "id": sample_id,
            "scope": _match_named_int(line, "scope"),
            "path": _match_named_int(line, "path"),
            "hit": _match_named_int(line, "hit"),
            "stage": _match_int(line, r"\bstage=(-?\d+)"),
            "batch": _match_int(line, r"\bbatch=(-?\d+)"),
            "currentEpoch": dict(zip(
                ("flush", "dispatch", "upload", "dip"), current_epoch
            )),
            "poisonEpoch": dict(zip(
                ("flush", "dispatch", "upload"), poison_epoch
            )),
            "poisonFuseKey": _match_named_int(line, "fuse"),
            "realStorage": dict(zip(("current", "poison"), real)),
            "mappingStorage": dict(zip(("current", "poison"), mapping)),
            "mapAllocation": dict(zip(
                ("current", "poison"), map_allocation
            )),
            "mapMode": dict(zip(("current", "poison"), map_mode)),
            "lock": dict(zip(
                ("currentActive", "currentFlags",
                 "poisonActive", "poisonFlags"), lock
            )),
            "mixed": dict(zip(
                ("real", "mapping", "mapAllocation", "mapMode", "index"),
                mixed,
            )),
            "vertex": dict(zip(("base", "min", "count"), vertex)),
            "poisonVertex": dict(zip(("base", "count"), poison_vertex)),
            "index": dict(zip(
                ("originStart", "originCount", "currentStart",
                 "currentPrimitiveCount", "primitiveType"), index
            )),
            "raw": line,
        })
    return {
        "items": samples,
        "lineCount": len(samples),
        "uniqueIds": len(seen_ids),
        "duplicateIds": duplicate_ids,
    }


def _find_optional_exact_conflict(text: str) -> Optional[int]:
    patterns = (
        r"exactTakeoverConflict(?:s)?=(\d+)",
        r"exact(?:Takeover)?Conflict(?:s)?=(\d+)",
        r"takeoverExactConflict(?:s)?=(\d+)",
    )
    for pattern in patterns:
        value = _match_int(text, pattern)
        if value is not None:
            return value
    return None


def _parse_native_poison_sidecar_policy(
    line: str, requested_policy: Optional[str] = None,
) -> Dict[str, Any]:
    token_match = re.search(
        r"\bpolicy=(none|o0|o1|both|[0-9]+)\b", line, re.IGNORECASE,
    )
    reported_token = token_match.group(1).lower() if token_match else None
    if reported_token is not None and reported_token.isdigit():
        numeric_token = int(reported_token)
        reported_policy = next(
            (
                name for name, value
                in GPU_SKIN_POISON_SIDECAR_POLICIES.items()
                if value == numeric_token
            ),
            None,
        )
    else:
        reported_policy = reported_token
    value = _match_named_int(line, "value", "policyValue")
    o0_enabled = _match_named_int(line, "o0", "o0Enabled")
    o1_enabled = _match_named_int(line, "o1", "o1Enabled")
    explicit = _match_named_int(line, "explicit")
    invalid = _match_named_int(line, "invalid")
    parse_clean = _match_named_int(line, "parse", "parseClean")
    closure_clean = _match_named_int(
        line, "closure", "policyClosure", "policyClosureClean",
    )
    authority = _match_named_int(line, "authority")
    expected_value = GPU_SKIN_POISON_SIDECAR_POLICIES.get(
        reported_policy or ""
    )
    requested_recognized = (
        requested_policy is None
        or requested_policy in GPU_SKIN_POISON_SIDECAR_POLICIES
    )
    requested_exact = bool(
        requested_policy is None
        or reported_policy == requested_policy
    )
    values_present = bool(
        reported_policy in GPU_SKIN_POISON_SIDECAR_POLICIES
        and all(
            isinstance(item, int)
            for item in (
                value, o0_enabled, o1_enabled, explicit, invalid,
                parse_clean, closure_clean, authority,
            )
        )
    )
    bit_contract = bool(
        values_present
        and value == expected_value
        and o0_enabled == (1 if value & 1 else 0)
        and o1_enabled == (1 if value & 2 else 0)
    )
    parse_contract = bool(
        values_present
        and explicit == 1
        and invalid == 0
        and parse_clean == 1
        and closure_clean == 1
        and authority == 0
    )
    contract_closed = bool(
        values_present and bit_contract and parse_contract
        and requested_recognized and requested_exact
    )
    return {
        "policy": reported_policy,
        "value": value,
        "o0Enabled": o0_enabled == 1 if isinstance(o0_enabled, int) else None,
        "o1Enabled": o1_enabled == 1 if isinstance(o1_enabled, int) else None,
        "reportedO0Enabled": o0_enabled,
        "reportedO1Enabled": o1_enabled,
        "explicit": explicit,
        "invalid": invalid,
        "parseClean": parse_clean,
        "reportedClosureClean": closure_clean,
        "authority": authority,
        "requestedPolicy": requested_policy,
        "requestedRecognized": requested_recognized,
        "requestedExact": requested_exact,
        "present": values_present,
        "bitContractClean": bit_contract,
        "parseContractClean": parse_contract,
        "contractClosed": contract_closed,
        "reportOnly": True,
        "authorizationAuthority": authority,
        "raw": line,
    }


def _native_poison_sidecar_runtime_contract(
    policy: Dict[str, Any],
    poison_o0: Dict[str, Any],
    poison_o1: Dict[str, Any],
    diagnostic_policy: Dict[str, Any],
) -> Dict[str, Any]:
    full = diagnostic_policy.get("fullExact") is True
    light = diagnostic_policy.get("lightExact") is True
    recognized_diagnostics = full != light
    o0_enabled = policy.get("o0Enabled") is True
    o1_enabled = policy.get("o1Enabled") is True
    o0_cold = bool(
        poison_o0.get("present") is True
        and poison_o0.get("coldContractClosed") is True
        and poison_o0.get("allCountersZero") is True
        and poison_o0.get("authorizationAuthority") == 0
    )
    o1_cold = bool(
        poison_o1.get("present") is True
        and poison_o1.get("coldContractClosed") is True
        and poison_o1.get("allCountersZero") is True
        and poison_o1.get("authorizationAuthority") == 0
        and poison_o1.get("authority") == 0
    )
    o0_live = bool(
        poison_o0.get("present") is True
        and poison_o0.get("contractClosed") is True
        and poison_o0.get("endpointClean") is True
        and poison_o0.get("authorizationAuthority") == 0
    )
    o1_live = bool(
        poison_o1.get("present") is True
        and poison_o1.get("contractClosed") is True
        and poison_o1.get("endpointClean") is True
        and poison_o1.get("authorizationAuthority") == 0
        and poison_o1.get("authority") == 0
    )
    # Full diagnostics rejects the production-light fast lane before either
    # sidecar can observe work. Policy bits remain frozen and reportable, but
    # both numerical surfaces must stay cold regardless of the selected bits.
    o0_exact = o0_cold if full or not o0_enabled else o0_live
    o1_exact = o1_cold if full or not o1_enabled else o1_live
    return {
        "present": bool(
            policy.get("present") is True
            and poison_o0.get("present") is True
            and poison_o1.get("present") is True
        ),
        "fullDiagnostics": full,
        "lightDiagnostics": light,
        "diagnosticPolicyRecognized": recognized_diagnostics,
        "policy": policy.get("policy"),
        "o0Enabled": o0_enabled,
        "o1Enabled": o1_enabled,
        "o0Cold": o0_cold,
        "o1Cold": o1_cold,
        "o0Exact": o0_exact,
        "o1Exact": o1_exact,
        "authorityZero": bool(
            policy.get("authority") == 0
            and poison_o0.get("authorizationAuthority") == 0
            and poison_o1.get("authorizationAuthority") == 0
            and poison_o1.get("authority") == 0
        ),
        "exact": bool(
            policy.get("contractClosed") is True
            and recognized_diagnostics
            and o0_exact and o1_exact
        ),
        "reportOnly": True,
    }


def _native_poison_sidecar_pair_contract(
    previous: Dict[str, Any],
    current: Dict[str, Any],
    requested_policy: Optional[str],
) -> Dict[str, Any]:
    exact_fields = (
        "policy", "value", "o0Enabled", "o1Enabled", "explicit",
        "invalid", "parseClean", "reportedClosureClean", "authority",
    )
    endpoints_equal = all(
        previous.get(name) == current.get(name) for name in exact_fields
    )
    requested_exact = bool(
        requested_policy in GPU_SKIN_POISON_SIDECAR_POLICIES
        and previous.get("policy") == current.get("policy") ==
            requested_policy
    )
    return {
        "present": bool(
            previous.get("present") is True
            and current.get("present") is True
        ),
        "policy": current.get("policy"),
        "value": current.get("value"),
        "o0Enabled": current.get("o0Enabled"),
        "o1Enabled": current.get("o1Enabled"),
        "requestedPolicy": requested_policy,
        "requestedExact": requested_exact,
        "endpointsEqual": endpoints_equal,
        "authorityZero": bool(
            previous.get("authority") == current.get("authority") == 0
        ),
        "authorizationAuthority": 0,
        "exact": bool(
            previous.get("contractClosed") is True
            and current.get("contractClosed") is True
            and endpoints_equal and requested_exact
        ),
        "reportOnly": True,
    }


def _select_status_fields(value: Any, needle: str, prefix: str = "") -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    if isinstance(value, dict):
        for key, child in value.items():
            name = f"{prefix}.{key}" if prefix else str(key)
            if needle.lower() in str(key).lower():
                result[name] = child
            result.update(_select_status_fields(child, needle, name))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            result.update(_select_status_fields(child, needle, f"{prefix}[{index}]"))
    return result


def _parse_gpu_skin_diag(
    text: str,
    runtime_data: Dict[str, Any],
    requested_sidecar_policy: Optional[str] = None,
) -> Dict[str, Any]:
    latest = _latest_diag_lines(text)
    quiescence_pre = latest.get("quiescencePre", "")
    quiescence_post = latest.get("quiescencePost", "")
    mode = latest.get("mode", "")
    resolve = latest.get("resolve", "")
    compute = latest.get("compute", "")
    vs_route = latest.get("vsRoute", "")
    lifetime = latest.get("lifetime", "")
    consume = latest.get("consume", "")
    ledger = latest.get("ledger", "")
    ledger_reason = latest.get("ledgerReason", "")
    late_poison = latest.get("latePoison", "")
    late_poison_native = latest.get("latePoisonNative", "")
    late_poison_storage = latest.get("latePoisonStorage", "")
    late_poison_dump = latest.get("latePoisonDump", "")
    native = latest.get("nativeSafety", "")
    native_fast = latest.get("nativeFast", "")
    native_manager_dispatch = latest.get("nativeManagerDispatch", "")
    native_dispatch_seal = latest.get("nativeDispatchSeal", "")
    native_timing = latest.get("nativeTiming", "")
    native_outer_admission_exact = latest.get(
        "nativeOuterAdmissionExact", ""
    )
    native_outer_reject_no_poison = latest.get(
        "nativeOuterRejectNoPoison", ""
    )
    native_outer_reject_with_poison = latest.get(
        "nativeOuterRejectWithPoison", ""
    )
    native_begin_sample = latest.get("nativeBeginSample", "")
    native_t2_sample = latest.get("nativeT2Sample", "")
    native_prod_outer = latest.get("nativeProdOuter", "")
    native_prod_dispatch_seal = latest.get(
        "nativeProdDispatchSeal", ""
    )
    native_prod_fallback = latest.get("nativeProdFallback", "")
    native_prod_kernel = latest.get("nativeProdKernel", "")
    native_prod_event_root = latest.get("nativeProdEventRoot", "")
    native_prod_event_semantic = latest.get(
        "nativeProdEventSemantic", ""
    )
    native_prod_event_dip_device = latest.get(
        "nativeProdEventDipDevice", ""
    )
    native_prod_event_dip_bridge = latest.get(
        "nativeProdEventDipBridge", ""
    )
    native_prod_event_dip_resolve = latest.get(
        "nativeProdEventDipResolve", ""
    )
    native_prod_callback_pin = latest.get("nativeProdCallbackPin", "")
    native_prod_callback_body = latest.get("nativeProdCallbackBody", "")
    native_prod_callback_leave = latest.get(
        "nativeProdCallbackLeave", ""
    )
    native_prod_outer_admit_class_calls = latest.get(
        "nativeProdOuterAdmitClassCalls", ""
    )
    native_prod_outer_admit_class_ticks = latest.get(
        "nativeProdOuterAdmitClassTicks", ""
    )
    native_prod_outer_admit_class_max = latest.get(
        "nativeProdOuterAdmitClassMax", ""
    )
    native_prod_outer_complete_class_calls = latest.get(
        "nativeProdOuterCompleteClassCalls", ""
    )
    native_prod_outer_complete_class_ticks = latest.get(
        "nativeProdOuterCompleteClassTicks", ""
    )
    native_prod_outer_complete_class_max = latest.get(
        "nativeProdOuterCompleteClassMax", ""
    )
    native_prod_fallback_reason_calls = latest.get(
        "nativeProdFallbackReasonCalls", ""
    )
    native_prod_fallback_reason_ticks = latest.get(
        "nativeProdFallbackReasonTicks", ""
    )
    native_prod_fallback_reason_max = latest.get(
        "nativeProdFallbackReasonMax", ""
    )
    native_prod_outer_alias_closure = latest.get(
        "nativeProdOuterAliasClosure", ""
    )
    manager_prod_callback_enter = latest.get(
        "managerProdCallbackEnter", ""
    )
    manager_prod_callback_body = latest.get(
        "managerProdCallbackBody", ""
    )
    manager_prod_callback_leave = latest.get(
        "managerProdCallbackLeave", ""
    )
    manager_prod_callback_reject = latest.get(
        "managerProdCallbackReject", ""
    )
    manager_hot = latest.get("managerHot", "")
    manager_queue_time = latest.get("managerQueueTime", "")
    manager_batch_time = latest.get("managerBatchTime", "")
    manager_proof_time = latest.get("managerProofTime", "")
    manager_consumer_time = latest.get("managerConsumerTime", "")
    poison = latest.get("nativePoison", "")
    poison_sidecar_policy_line = latest.get(
        "nativePoisonSidecarPolicy", ""
    )
    poison_shadow_line = latest.get("nativePoisonShadow", "")
    poison_shadow_reasons_line = latest.get(
        "nativePoisonShadowReasons", ""
    )
    poison_o1_shadow_line = latest.get("nativePoisonO1Shadow", "")
    poison_o1_shadow_reasons_line = latest.get(
        "nativePoisonO1ShadowReasons", ""
    )
    poison_o1_authority_line = latest.get(
        "nativePoisonO1Authority", ""
    )
    poison_o1_authority_evidence_line = latest.get(
        "nativePoisonO1AuthorityEvidence", ""
    )
    poison_o1_discard_joint_line = latest.get(
        "nativePoisonO1DiscardJoint", ""
    )
    poison_o1_scanner_line = latest.get("nativePoisonO1Scanner", "")
    poison_o1_scanner_reasons_line = latest.get(
        "nativePoisonO1ScannerReasons", ""
    )
    poison_o1_lane_lines = {
        lane_name: latest.get(
            f"nativePoisonO1ScannerLane:{lane_name}", ""
        )
        for lane_name in NATIVE_POISON_O1_LOCK_LANES
    }
    poison_o1_physical_line = latest.get("nativePoisonO1Physical", "")
    retirement = latest.get("nativeRetirement", "")
    reset = latest.get("nativeReset", "")
    kernel_normal = latest.get("nativeKernelNormal", "")
    ticket = latest.get("indexTicket", "")
    tracker = latest.get("tracker", "")
    format_line = latest.get("format", "")
    dip_fast_probe_line = latest.get("dipFastProbe", "")
    format_flow_line = latest.get("formatFlow", "")
    format_class_line = latest.get("formatClass", "")
    format_reject_lines = [
        latest.get(f"formatReject{index}", "") for index in range(6)
    ]
    strict_reject_line = latest.get("strictReject", "")
    coverage_line = latest.get("coverage", "")

    kernel_calls = _match_tuple(native, r"\bkernel=(\d+)/(\d+)/(\d+)", 3)
    upload_calls = _match_tuple(consume, r"\bnativeCalls=(\d+)/(\d+)", 2)
    upload_bytes = _match_tuple(consume, r"\bnativeBytes=(\d+)/(\d+)", 2)
    kernel_bytes = _match_tuple(
        "\n".join(latest.values()), r"\bkernelBytes=(\d+)/(\d+)", 2
    )
    p2 = _match_tuple(consume, r"\bP2=(\d+)/(\d+)/(\d+)", 3)
    p3_main = _match_tuple(consume, r"\bmain=(\d+)/(\d+)/(\d+)", 3)
    p3_restore = _match_tuple(consume, r"\brestore=(\d+)/(\d+)/(\d+),pending:(\d+)", 4)
    bypass = _match_tuple(consume, r"\bbypass=(\d+)/(\d+)/(\d+)/(\d+)", 4)
    lifetime_pages = _match_tuple(lifetime, r"\bpages=(\d+)/(\d+)\s+output=(\d+)/(\d+)\s+pending=(\d+)", 5)
    lifetime_claims = _match_tuple(lifetime, r"\bclaims=(\d+)/(\d+)", 2)
    poison_values = _match_tuple(
        poison,
        r"\bcreate=(\d+)\s+clear=(\d+)\s+hit=(\d+)\s+overflow=(\d+)\s+resetStale=(\d+)\s+outstanding=(\d+)",
        6,
    )
    direct_discard_values = _match_tuple(
        poison,
        r"\bdirectDiscard=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        5,
    )
    poison_shadow = _parse_native_poison_shadow(
        poison_shadow_line, poison_shadow_reasons_line,
    )
    poison_o1_shadow = _parse_native_poison_o1_shadow(
        poison_o1_shadow_line, poison_o1_shadow_reasons_line,
        poison_o1_scanner_line, poison_o1_scanner_reasons_line,
        poison_o1_lane_lines, poison_o1_physical_line,
        poison_o1_discard_joint_line,
    )
    poison_o1_authority = _parse_native_poison_o1_authority(
        poison_o1_authority_line, poison_o1_authority_evidence_line,
    )
    poison_sidecar_policy = _parse_native_poison_sidecar_policy(
        poison_sidecar_policy_line,
        requested_policy=requested_sidecar_policy,
    )
    ticket_values = _match_tuple(
        ticket,
        r"\bmask=(0x[0-9a-fA-F]+)\s+attempts=(\d+)\s+exact=(\d+)\s+suppressed=(\d+)\s+leaks=(\d+)",
        5,
    )
    tracker_values = _match_tuple(
        tracker, r"\bconflicts=(\d+)\s+tag=(\d+)\s+stage=(\d+)\s+layer=(\d+)", 4
    )
    format_buckets = _match_tuple(
        format_line,
        r"\bformat=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    skin_buckets = _match_tuple(
        format_line,
        r"\bskin=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_flow_eligible = _match_tuple(
        format_flow_line,
        r"\beligible=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_flow_learned = _match_tuple(
        format_flow_line,
        r"\blearned=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_flow_candidate = _match_tuple(
        format_flow_line,
        r"\bcandidate=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_flow_job = _match_tuple(
        format_flow_line,
        r"\bjob=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_class_outside = _match_tuple(
        format_class_line,
        r"\boutside=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_class_inside = _match_tuple(
        format_class_line,
        r"\binside=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_class_eligible = _match_tuple(
        format_class_line,
        r"\beligible=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    format_reject_by_format = [
        _match_tuple(
            line,
            r"\bpath=(\d+)\s+stage=(\d+)\s+skin=(\d+)\s+"
            r"input=(\d+)\s+output=(\d+)\s+identity=(\d+)\s+"
            r"cpu=(\d+)\s+bypass=(\d+)",
            8,
        )
        for line in format_reject_lines
    ]
    native_upload_coverage = _match_tuple(
        format_line, r"\bnativeUpload\s+raw=(\d+)\s+outside=(\d+)", 2
    )
    native_dip_coverage = _match_tuple(
        format_line,
        r"\bnativeDip\s+raw=(\d+)\s+correlated=(\d+)\s+"
        r"unmatched=(\d+)\s+outside=(\d+)\s+noUpload=(\d+)\s+"
        r"fastOutside=(\d+)",
        6,
    )
    dip_fast_probe_period = _match_named_int(
        dip_fast_probe_line, "period"
    )
    dip_fast_probe_phases = _match_tuple(
        dip_fast_probe_line, r"\bphase=(\d+)/(\d+)", 2
    )
    dip_fast_probe_attempts_derived = _match_named_int(
        dip_fast_probe_line, "derived"
    )
    dip_fast_probe_early = _match_tuple(
        dip_fast_probe_line, r"\bearly=(\d+)/(\d+)\s+reject=", 2
    )
    dip_fast_probe_early_rejects = _match_tuple(
        dip_fast_probe_line,
        r"\bearly=\d+/\d+\s+reject="
        r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    dip_fast_probe_late = _match_tuple(
        dip_fast_probe_line, r"\blate=(\d+)/(\d+)\s+reject=", 2
    )
    dip_fast_probe_late_rejects = _match_tuple(
        dip_fast_probe_line,
        r"\blate=\d+/\d+\s+reject="
        r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        8,
    )
    dip_fast_probe_early_local = _match_tuple(
        dip_fast_probe_line,
        r"\bearlyLocal="
        r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        7,
    )
    dip_fast_probe_late_local = _match_tuple(
        dip_fast_probe_line,
        r"\blateLocal="
        r"(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        7,
    )
    dip_fast_probe_observer = _match_tuple(
        dip_fast_probe_line,
        r"\bobserver=(\d+)/(\d+)/(\d+)",
        3,
    )
    dip_fast_probe_reader = _match_tuple(
        dip_fast_probe_line,
        r"\breader=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        len(_GPU_SKIN_DIP_FAST_READER_FIELDS),
    )
    dip_fast_probe_cover = _match_tuple(
        dip_fast_probe_line,
        r"\bcover=(\d+)/(\d+)/(\d+)",
        len(_GPU_SKIN_DIP_FAST_COVER_FIELDS),
    )
    dip_fast_probe_present = all(
        isinstance(value, int)
        for value in (
            dip_fast_probe_period,
            *dip_fast_probe_phases,
            dip_fast_probe_attempts_derived,
            *dip_fast_probe_early,
            *dip_fast_probe_early_rejects,
            *dip_fast_probe_late,
            *dip_fast_probe_late_rejects,
            *dip_fast_probe_early_local,
            *dip_fast_probe_late_local,
            *dip_fast_probe_observer,
            *dip_fast_probe_reader,
            *dip_fast_probe_cover,
        )
    )
    dip_fast_probe_early_closed = bool(
        dip_fast_probe_present
        and dip_fast_probe_early[0] ==
            dip_fast_probe_early[1] + sum(dip_fast_probe_early_rejects)
    )
    dip_fast_probe_late_closed = bool(
        dip_fast_probe_present
        and dip_fast_probe_late[0] ==
            dip_fast_probe_late[1] + sum(dip_fast_probe_late_rejects)
    )
    dip_fast_probe_early_local_closed = bool(
        dip_fast_probe_present
        and sum(dip_fast_probe_early_local) ==
            dip_fast_probe_early_rejects[0]
    )
    dip_fast_probe_late_local_closed = bool(
        dip_fast_probe_present
        and sum(dip_fast_probe_late_local) ==
            dip_fast_probe_late_rejects[0]
    )
    dip_fast_probe_observer_closed = bool(
        dip_fast_probe_present
        and dip_fast_probe_observer[0] == dip_fast_probe_observer[1]
        and dip_fast_probe_observer[2] == 0
    )
    dip_fast_probe_reader_closed = bool(
        dip_fast_probe_present
        and dip_fast_probe_reader[0] == dip_fast_probe_reader[1]
        and dip_fast_probe_reader[0] == (
            dip_fast_probe_reader[2] + dip_fast_probe_reader[3]
        )
        and dip_fast_probe_reader[4] <= dip_fast_probe_reader[3]
        and dip_fast_probe_reader[5] == 0
    )
    dip_fast_probe_reader_cover_exact = bool(
        dip_fast_probe_present
        and dip_fast_probe_reader[2] == dip_fast_probe_cover[2]
    )
    dip_fast_probe_cover_closed = bool(
        dip_fast_probe_present
        and native_dip_coverage[5] == sum(dip_fast_probe_cover)
        and dip_fast_probe_reader_cover_exact
    )
    dip_fast_probe_all_zero = bool(
        dip_fast_probe_present
        and all(value == 0 for value in (
            *dip_fast_probe_early,
            *dip_fast_probe_early_rejects,
            *dip_fast_probe_late,
            *dip_fast_probe_late_rejects,
            *dip_fast_probe_early_local,
            *dip_fast_probe_late_local,
            *dip_fast_probe_reader,
            *dip_fast_probe_cover,
        ))
    )
    native_fanout = _match_tuple(
        format_line,
        r"\bnativeFanout=(\d+)/(\d+)/(\d+),max:(\d+)",
        4,
    )
    strict_reject_values = _match_tuple(
        strict_reject_line,
        r"\bpath=(\d+)\s+stage=(\d+)\s+skin=(\d+)\s+"
        r"input=(\d+)\s+output=(\d+)\s+identity=(\d+)\s+"
        r"cpu=(\d+)\s+bypass=(\d+)",
        8,
    )
    coverage_scopes = _match_tuple(
        coverage_line, r"\bscopes=(\d+),(\d+),(\d+)", 3
    )
    coverage_fallback = _match_tuple(
        coverage_line,
        r"\bfallback=special:(\d+),path:(\d+),transparent:(\d+),"
        r"stage:(\d+),skin:(\d+),preflight:(\d+),layout:(\d+),"
        r"output:(\d+),multi:(\d+)",
        9,
    )
    coverage_available = bool(coverage_line) and all(
        value is not None for value in coverage_scopes + coverage_fallback
    )
    def parse_quiescence(line: str) -> Dict[str, Any]:
        manager_pending = _match_tuple(
            line, r"\bmanagerPending=(\d+)/(\d+)/(\d+)/(\d+)", 4
        )
        resource_pending = _match_tuple(
            line, r"\bresourcePending=(\d+)\s+uploadPages=(\d+)/(\d+)", 3
        )
        bridge_pending = _match_tuple(
            line, r"\bbridgePending=(\d+)/(\d+)/(\d+)", 3
        )
        bridge_active = _match_tuple(
            line,
            r"\bactive=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
            6,
        )
        telemetry = _match_tuple(
            line, r"\btelemetry=(\d+)/(\d+)", 2
        )
        reset_generation = _match_tuple(
            line, r"\breset=(\d+)/(\d+)/(\d+)", 3
        )
        retired = _match_tuple(
            line, r"\bretired=(\d+)/(\d+)", 2
        )
        ingress = _match_tuple(
            line, r"\bingress=(\d+)/(\d+)/(\d+)/(\d+)", 4
        )
        device_pending = _match_tuple(
            line, r"\bdevicePending=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
        )
        return {
            "ready": _match_named_int(line, "ready"),
            "refresh": _match_named_int(line, "refresh"),
            "managerReady": _match_named_int(line, "manager"),
            "resourcesReady": _match_named_int(line, "resources"),
            "bridgeReady": _match_named_int(line, "bridge"),
            "deviceReady": _match_named_int(line, "device"),
            "frame": _match_named_int(line, "frame"),
            "flush": _match_named_int(line, "flush"),
            "mapEpoch": _match_named_int(line, "map"),
            "deviceEpoch": _match_named_int(line, "deviceEpoch"),
            "managerPending": dict(zip(
                ("dispatches", "uploads", "preparedDraws", "submissions"),
                manager_pending,
            )),
            "pendingBypass": _match_named_int(line, "bypass"),
            "pendingBridgeReset": _match_named_int(line, "bridgeReset"),
            "resourceInFlight": _match_named_int(line, "resourceInFlight"),
            "retired": dict(zip(
                ("resourceEpochs", "claims"), retired
            )),
            "resources": dict(zip(
                ("outputPending", "uploadPagesAllocated", "uploadPagesReclaimed"),
                resource_pending,
            )),
            "bridgePending": dict(zip(
                ("kernelAuthorizations", "poisonRanges", "retirementEvents"),
                bridge_pending,
            )),
            "bridgeActive": dict(zip(
                (
                    "callbackPins", "flush", "dispatch", "semantic",
                    "upload", "dipObserver",
                ),
                bridge_active,
            )),
            "telemetry": dict(zip(
                ("pending", "faulted"), telemetry
            )),
            "resetGeneration": dict(zip(
                ("requested", "completed", "ownerRetired"),
                reset_generation,
            )),
            "thread": _match_named_int(line, "thread"),
            "tls": _match_named_int(line, "tls"),
            "retirementFault": _match_named_int(line, "fault"),
            "ingress": dict(zip(
                ("safety", "transaction", "callback", "dipObserver"),
                ingress,
            )),
            "bypassBlocked": _match_named_int(line, "blocked"),
            "devicePending": dict(zip(
                ("frameBatches", "indexTicket", "restore", "parity", "deviceEpoch"),
                device_pending,
            )),
            "raw": line,
        }

    quiescence = {
        "pre": parse_quiescence(quiescence_pre),
        "post": parse_quiescence(quiescence_post),
    }
    native_values = _match_tuple(
        native,
        r"\boverflow=(\d+)/(\d+).*?\bsemantic=(\d+)/(\d+)/(\d+)/(\d+).*?\bpostSkipFallback=(\d+)\s+duplicate=(\d+)\s+irreversible=(\d+)\s+pending=(\d+)",
        10,
    )
    mode_values = _match_tuple(
        mode, r"\bdispatch=(\d+)/(\d+).*?\btruePairErr=(\d+)\s+epochLeak=(\d+)\s+pending=(\d+)/(\d+)/(\d+)", 7
    )
    diagnostic_full = _match_named_int(mode, "diagFull")
    preflight_detail = _match_named_int(mode, "preflightDetail")
    diagnostic_period = _match_named_int(mode, "diagPeriod")
    diagnostic_full_exact = (
        diagnostic_full == 1
        and preflight_detail == 1
        and diagnostic_period == 0
    )
    diagnostic_light_exact = (
        diagnostic_full == 0
        and preflight_detail == 0
        and diagnostic_period == 0
    )
    diagnostic_policy = {
        "full": diagnostic_full,
        "preflightDetail": preflight_detail,
        "periodFrames": diagnostic_period,
        "present": all(
            value is not None for value in (
                diagnostic_full, preflight_detail, diagnostic_period,
            )
        ),
        "fullExact": diagnostic_full_exact,
        "lightExact": diagnostic_light_exact,
        "recognizedExact": diagnostic_full_exact != diagnostic_light_exact,
        # Compatibility alias for existing full-only artifact readers.
        "autoTestExact": diagnostic_full_exact,
    }
    poison_sidecar_runtime_contract = (
        _native_poison_sidecar_runtime_contract(
            poison_sidecar_policy,
            poison_shadow,
            poison_o1_shadow,
            diagnostic_policy,
        )
    )
    poison_sidecar_policy["runtimeContract"] = (
        poison_sidecar_runtime_contract
    )
    ledger_values = _match_tuple(
        ledger,
        r"\bclassified=(\d+)\s+resolved=(\d+)\s+consumed=(\d+)\s+cpuFallback=(\d+)\s+suppressed=(\d+)\s+leak=(\d+)\s+unreserved=(\d+)\s+duplicate=(\d+)\s+planMismatch=(\d+)\s+retireDeferred=(\d+)",
        10,
    )
    ledger_reason_callers = _match_tuple(
        ledger_reason, r"\blookupCaller=(\d+)/(\d+)/(\d+)", 3
    )
    ledger_reason_causes = _match_tuple(
        ledger_reason, r"\blookupCause=(\d+)/(\d+)/(\d+)/(\d+)", 4
    )
    ledger_reason_other = _match_tuple(
        ledger_reason, r"\bother=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
    )
    p4_shadow = _match_tuple(
        consume,
        r"\bp4Shadow=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        7,
    )
    outline_slice = _match_tuple(
        consume, r"\boutlineSlice=(\d+)/(\d+)", 2
    )
    late_poison_match = _match_tuple(
        late_poison, r"\bmatch=(\d+)/(\d+)/(\d+)", 3
    )
    late_poison_correlation = _match_tuple(
        late_poison, r"\bcorrelation=(\d+)/(\d+)", 2
    )
    late_poison_source_bypass = _match_tuple(
        late_poison, r"\bsourceBypass=(\d+)/(\d+)/(\d+)", 3
    )
    late_poison_source_poison_only = _match_tuple(
        late_poison, r"\bsourcePoisonOnly=(\d+)/(\d+)/(\d+)", 3
    )
    late_poison_exact = _match_tuple(
        late_poison, r"\bexact=(\d+)/(\d+)", 2
    )
    late_poison_flags = _match_tuple(
        late_poison,
        r"\bflags=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        6,
    )
    late_poison_masks = _match_tuple(
        late_poison,
        r"\bmasks=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        11,
    )
    late_poison_native_scope = _match_tuple(
        late_poison_native, r"\bscope=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
    )
    late_poison_native_hit = _match_tuple(
        late_poison_native, r"\bhit=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5
    )
    late_poison_native_samples = _match_tuple(
        late_poison_native, r"\bsamples=(\d+)/(\d+)/(\d+)", 3
    )
    late_poison_storage_real = _match_tuple(
        late_poison_storage, r"\breal=(\d+)/(\d+)/(\d+)/(\d+)", 4
    )
    late_poison_storage_mapping = _match_tuple(
        late_poison_storage,
        r"\bmapping=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    late_poison_storage_map = _match_tuple(
        late_poison_storage,
        r"\bmapAllocation=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    late_poison_storage_index = _match_tuple(
        late_poison_storage, r"\bindex=(\d+)/(\d+)/(\d+)/(\d+)", 4
    )
    late_poison_samples = _parse_late_poison_samples(text)
    manager_queue = _match_tuple(
        manager_hot, r"\bqueue=(\d+)/(\d+)/(\d+)", 3
    )
    renderable_bloom = _match_tuple(
        manager_hot, r"\bbloom=(\d+)/(\d+)", 2
    )
    bypass_static_hint = _match_tuple(
        manager_hot, r"\bstaticHint=(\d+)/(\d+)", 2
    )
    bloom_closure_available = all(
        value is not None
        for value in manager_queue[1:3] + renderable_bloom
    )
    bloom_classified = (
        renderable_bloom[0] + renderable_bloom[1]
        if bloom_closure_available else None
    )
    reverse_classified = (
        manager_queue[1] + manager_queue[2]
        if bloom_closure_available else None
    )

    fast_reject_scope = _match_int(
        native_fast, r"\breject=(\d+)/\d+/\d+/\d+"
    )
    fast_reject_state = _match_int(
        native_fast, r"\breject=\d+/(\d+)/\d+/\d+"
    )
    fast_reject_skin = _match_int(
        native_fast, r"\breject=\d+/\d+/(\d+)/\d+"
    )
    fast_reject_input = _match_int(
        native_fast, r"\breject=\d+/\d+/\d+/(\d+)"
    )
    fast_reject_small = _match_named_int(native_fast, "smallCpu")
    fast_candidates = _match_named_int(native_fast, "candidate")
    fast_dispatch_seal_uploads = _match_int(
        native_dispatch_seal, r"\bupload=\d+/(\d+)"
    )
    fast_raw_uploads = native_upload_coverage[0]
    fast_partition_values = (
        fast_reject_scope, fast_reject_state, fast_reject_skin,
        fast_reject_small, fast_reject_input, fast_candidates,
        fast_dispatch_seal_uploads,
    )
    fast_partition_present = (
        fast_raw_uploads is not None
        and all(value is not None for value in fast_partition_values)
    )
    fast_classified = (
        sum(value for value in fast_partition_values if value is not None)
        if fast_partition_present else None
    )
    fast_partition_closed = bool(
        fast_partition_present and fast_classified == fast_raw_uploads
    )

    manager_dispatch_physical = _match_tuple(
        native_manager_dispatch, r"\bphysical=(\d+)/(\d+)", 2
    )
    manager_dispatch_shape = _match_tuple(
        native_manager_dispatch, r"\bshape=(\d+)/(\d+)/(\d+)", 3
    )
    manager_dispatch_scope_class = _match_tuple(
        native_manager_dispatch,
        r"\bscopeClass=(\d+)/(\d+)/(\d+)", 3,
    )
    manager_dispatch_native_cpu_only = _match_tuple(
        native_manager_dispatch,
        r"\bnativeCpuOnly=(\d+)/(\d+)", 2,
    )
    manager_dispatch_callbacks = _match_tuple(
        native_manager_dispatch, r"\bcallbacks=(\d+)/(\d+)", 2
    )
    manager_dispatch_begin_class = _match_tuple(
        native_manager_dispatch,
        r"\bbeginClass=(\d+)/(\d+)/(\d+)", 3,
    )
    manager_dispatch_failures = _match_tuple(
        native_manager_dispatch, r"\bfail=(\d+)/(\d+)/(\d+)", 3
    )
    manager_dispatch_end_class = _match_tuple(
        native_manager_dispatch,
        r"\bendClass=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    manager_dispatch_skipped = _match_tuple(
        native_manager_dispatch, r"\bskip=(\d+)/(\d+)/(\d+)", 3
    )
    manager_dispatch_telemetry = _match_tuple(
        native_manager_dispatch,
        r"\btelemetry=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    manager_dispatch_evidence = _match_named_int(
        native_manager_dispatch, "evidence"
    )
    dispatch_seal_view = _match_tuple(
        native_dispatch_seal,
        r"\bview=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", 5,
    )
    dispatch_seal_local_view = _match_tuple(
        native_dispatch_seal,
        r"\blocalView=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        7,
    )
    dispatch_seal_native_cpu_only = _match_tuple(
        native_dispatch_seal,
        r"\bnativeCpuOnly=(\d+)/(\d+)", 2,
    )
    dispatch_seal_proposal = _match_tuple(
        native_dispatch_seal,
        r"\bproposal=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    dispatch_seal_scope = _match_tuple(
        native_dispatch_seal, r"\bscope=(\d+)/(\d+)/(\d+)", 3,
    )
    dispatch_seal_upload = _match_tuple(
        native_dispatch_seal, r"\bupload=(\d+)/(\d+)", 2,
    )
    dispatch_seal_kernel = _match_tuple(
        native_dispatch_seal, r"\bkernel=(\d+)/(\d+)", 2,
    )
    dispatch_seal_dip = _match_tuple(
        native_dispatch_seal, r"\bdip=(\d+)/(\d+)/(\d+)", 3,
    )
    dispatch_seal_fanout = _match_tuple(
        native_dispatch_seal,
        r"\bfanout=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    dispatch_seal_closure = _match_tuple(
        native_dispatch_seal,
        r"\bclosure=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        7,
    )
    dispatch_seal_vertices = _match_named_int(
        native_dispatch_seal, "vertices"
    )
    dispatch_seal_bytes = _match_named_int(
        native_dispatch_seal, "bytes"
    )
    dispatch_seal_conflicts = _match_named_int(
        native_dispatch_seal, "conflicts"
    )

    outer_admission_values = _match_tuple(
        native_outer_admission_exact,
        r"\baccepted=(\d+)/(\d+)\s+cancel=(\d+)\s+"
        r"lifecycleExcluded=(\d+)\s+trackedInside=(\d+)\s+"
        r"untrackedOutside=(\d+)\s+reject=(\d+)/(\d+)\s+"
        r"attempts=(\d+)\s+trackedOutside=(\d+)\s+"
        r"resolvedOutside=(\d+)/(\d+)\s+"
        r"closure=(\d+)/(\d+)/(\d+)\s+unknownZero=(\d+)",
        16,
    )
    outer_reject_no_poison = {
        name: _match_named_int(native_outer_reject_no_poison, name)
        for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
    }
    outer_reject_with_poison = {
        name: _match_named_int(native_outer_reject_with_poison, name)
        for name in _GPU_SKIN_OUTSIDE_REJECT_REASONS
    }

    native_timing_frequency = _match_named_int(native_timing, "freq")
    native_t2_sample_frequency = _match_named_int(native_t2_sample, "freq")
    manager_timing_frequency = _match_named_int(manager_hot, "freq")
    native_timing_stages = _parse_raw_timing_group(
        native_timing,
        ("begin", "eval", "complete", "notify", "dip", "outer", "cpuKernel"),
        native_timing_frequency,
    )
    native_begin_sample_period = _match_named_int(native_begin_sample, "period")
    native_begin_sample_stages = _parse_raw_timing_group(
        native_begin_sample,
        (
            "common", "state", "exact", "scopeRoute", "stateRejectRoute",
            "skinRoute", "smallRoute", "candidateRoute",
        ),
        native_timing_frequency,
    )
    native_t2_sample_period = _match_named_int(native_t2_sample, "period")
    native_t2_sample_stages = _parse_raw_timing_group(
        native_t2_sample,
        (
            "geoSnap", "geoHeader", "posProof", "normalProof",
            "groupProof", "paletteProof",
        ),
        native_t2_sample_frequency,
    )
    manager_root_stages = _parse_raw_timing_group(
        manager_hot, ("flush", "host", "prepare"), manager_timing_frequency,
    )
    manager_queue_stages = _parse_raw_timing_group(
        manager_queue_time,
        (
            "control", "static", "scan", "collision", "positive", "binding",
            "staticLookup", "paletteCopy", "build",
        ),
        manager_timing_frequency,
    )
    manager_batch_stages = _parse_raw_timing_group(
        manager_batch_time,
        (
            "query", "hostFinalize", "assemble", "lease", "finalize",
            "upload", "publish",
        ),
        manager_timing_frequency,
    )
    manager_proof_stages = _parse_raw_timing_group(
        manager_proof_time,
        (
            "preManager", "preHost", "preFinalize", "palette", "static",
            "cpuManager", "cpuHost", "cpuFinalize", "completion",
        ),
        manager_timing_frequency,
    )
    manager_consumer_stages = _parse_raw_timing_group(
        manager_consumer_time,
        (
            "resolve", "shadow", "plan", "commit", "fail", "close",
            "drawResult", "fuse", "terminate",
        ),
        manager_timing_frequency,
    )

    production_sample_native_frequency = _match_named_int(
        native_prod_outer, "freq"
    )
    production_dispatch_seal_frequency = _match_named_int(
        native_prod_dispatch_seal, "freq"
    )
    production_sample_manager_frequency = _match_named_int(
        manager_prod_callback_enter, "freq"
    )
    production_sample_fallback_frequency = _match_named_int(
        native_prod_fallback, "freq"
    )
    production_sample_event_frequencies = {
        "root": _match_named_int(native_prod_event_root, "freq"),
        "semantic": _match_named_int(native_prod_event_semantic, "freq"),
        "dipDevice": _match_named_int(
            native_prod_event_dip_device, "freq"
        ),
        "dipBridge": _match_named_int(
            native_prod_event_dip_bridge, "freq"
        ),
        "dipResolve": _match_named_int(
            native_prod_event_dip_resolve, "freq"
        ),
    }
    production_sample_period = _match_named_int(
        native_prod_outer, "period"
    )
    production_sample_phase = _match_named_int(
        native_prod_outer, "phase"
    )
    production_dispatch_seal_period = _match_named_int(
        native_prod_dispatch_seal, "period"
    )
    production_dispatch_seal_phase = _match_named_int(
        native_prod_dispatch_seal, "phase"
    )
    production_sample_fallback_period = _match_named_int(
        native_prod_fallback, "period"
    )
    production_sample_fallback_phase = _match_named_int(
        native_prod_fallback, "phase"
    )
    production_sample_event_periods = {
        "root": _match_named_int(native_prod_event_root, "period"),
        "semantic": _match_named_int(
            native_prod_event_semantic, "period"
        ),
        "dipDevice": _match_named_int(
            native_prod_event_dip_device, "period"
        ),
        "dipBridge": _match_named_int(
            native_prod_event_dip_bridge, "period"
        ),
        "dipResolve": _match_named_int(
            native_prod_event_dip_resolve, "period"
        ),
    }
    production_sample_event_phases = {
        "root": _match_named_int(native_prod_event_root, "phase"),
        "semantic": _match_named_int(
            native_prod_event_semantic, "phase"
        ),
        "dipDevice": _match_named_int(
            native_prod_event_dip_device, "phase"
        ),
        "dipBridge": _match_named_int(
            native_prod_event_dip_bridge, "phase"
        ),
        "dipResolve": _match_named_int(
            native_prod_event_dip_resolve, "phase"
        ),
    }
    production_writer_values = _match_tuple(
        native_prod_outer,
        r"\bwriters=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    production_writer_snapshot = {
        "started": production_writer_values[0],
        "completed": production_writer_values[1],
        "active": production_writer_values[2],
        "pending": production_writer_values[3],
    }
    production_outer_stages = _parse_raw_timing_group(
        native_prod_outer,
        (
            "admitAccepted", "admitRejected", "inclusive", "body",
            "complete", "cancel",
        ),
        production_sample_native_frequency,
    )
    production_outer_stages.update(_parse_raw_timing_group(
        native_prod_fallback,
        ("fallbackInclusive", "fallbackBegin", "fallbackBody",
         "fallbackComplete"),
        production_sample_fallback_frequency,
    ))
    production_dispatch_seal_stages = _parse_raw_timing_group(
        native_prod_dispatch_seal,
        ("admission", "inclusive", "body", "complete", "cancel"),
        production_dispatch_seal_frequency,
    )
    production_dispatch_seal_reported_closure = _match_tuple(
        native_prod_dispatch_seal,
        r"\bclosure=(\d+)/(\d+)/(\d+)/(\d+)", 4,
    )
    outer_admission_accepted_class_timing = _parse_split_raw_timing_group(
        native_prod_outer_admit_class_calls,
        native_prod_outer_admit_class_ticks,
        native_prod_outer_admit_class_max,
        _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES,
        production_sample_native_frequency,
    )
    outer_fast_complete_class_timing = _parse_split_raw_timing_group(
        native_prod_outer_complete_class_calls,
        native_prod_outer_complete_class_ticks,
        native_prod_outer_complete_class_max,
        _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES,
        production_sample_native_frequency,
    )
    outer_fallback_reason_timing = _parse_split_raw_timing_group(
        native_prod_fallback_reason_calls,
        native_prod_fallback_reason_ticks,
        native_prod_fallback_reason_max,
        _GPU_SKIN_OUTSIDE_REJECT_REASONS,
        production_sample_native_frequency,
    )
    outer_admission_alias_reported = {
        "snapshotAvailable": _match_named_int(
            native_prod_outer_alias_closure, "available"
        ),
        "acceptedClassClean": _match_named_int(
            native_prod_outer_alias_closure, "accepted"
        ),
        "completeClassClean": _match_named_int(
            native_prod_outer_alias_closure, "complete"
        ),
        "fallbackReasonClean": _match_named_int(
            native_prod_outer_alias_closure, "fallback"
        ),
        "unknownHardZero": _match_named_int(
            native_prod_outer_alias_closure, "unknownZero"
        ),
        "contract": (
            "partition-alias-inclusive-do-not-add-to-parent"
            if "contract=partition-alias-inclusive-do-not-add-to-parent"
            in native_prod_outer_alias_closure else None
        ),
    }
    production_kernel_stages = _parse_raw_timing_group(
        native_prod_kernel,
        ("inclusive", "evaluate", "original", "notify"),
        production_sample_native_frequency,
    )
    production_event_root_stages = _parse_raw_timing_group(
        native_prod_event_root,
        (
            "flushRoot", "dispatchSemanticLookup",
            "dispatchBeginRoot", "dispatchEndRoot",
        ),
        production_sample_event_frequencies["root"],
    )
    production_event_semantic_stages = _parse_raw_timing_group(
        native_prod_event_semantic,
        ("semanticInclusive", "semanticOriginal"),
        production_sample_event_frequencies["semantic"],
    )
    production_event_dip_device_stages = _parse_raw_timing_group(
        native_prod_event_dip_device,
        ("outside", "noUpload", "correlated"),
        production_sample_event_frequencies["dipDevice"],
    )
    production_event_dip_bridge_stages = _parse_raw_timing_group(
        native_prod_event_dip_bridge,
        ("outside", "noUpload", "correlated"),
        production_sample_event_frequencies["dipBridge"],
    )
    production_event_dip_resolve_stages = _parse_raw_timing_group(
        native_prod_event_dip_resolve,
        ("outside", "noUpload", "correlated"),
        production_sample_event_frequencies["dipResolve"],
    )
    production_bridge_pin_stages = _parse_raw_timing_group(
        native_prod_callback_pin, _PRODUCTION_CALLBACK_NAMES,
        production_sample_native_frequency,
    )
    production_bridge_body_stages = _parse_raw_timing_group(
        native_prod_callback_body, _PRODUCTION_CALLBACK_NAMES,
        production_sample_native_frequency,
    )
    production_bridge_leave_stages = _parse_raw_timing_group(
        native_prod_callback_leave, _PRODUCTION_CALLBACK_NAMES,
        production_sample_native_frequency,
    )
    production_manager_enter_stages = _parse_raw_timing_group(
        manager_prod_callback_enter, _PRODUCTION_CALLBACK_NAMES,
        production_sample_manager_frequency,
    )
    production_manager_body_stages = _parse_raw_timing_group(
        manager_prod_callback_body, _PRODUCTION_CALLBACK_NAMES,
        production_sample_manager_frequency,
    )
    production_manager_leave_stages = _parse_raw_timing_group(
        manager_prod_callback_leave, _PRODUCTION_CALLBACK_NAMES,
        production_sample_manager_frequency,
    )
    production_manager_rejected = {
        name: _match_named_int(manager_prod_callback_reject, name)
        for name in _PRODUCTION_CALLBACK_NAMES
    }
    production_timing_records = (
        list(production_outer_stages.values())
        + list(production_dispatch_seal_stages.values())
        + list(production_kernel_stages.values())
        + list(production_event_root_stages.values())
        + list(production_event_semantic_stages.values())
        + list(production_event_dip_device_stages.values())
        + list(production_event_dip_bridge_stages.values())
        + list(production_event_dip_resolve_stages.values())
        + list(production_bridge_pin_stages.values())
        + list(production_bridge_body_stages.values())
        + list(production_bridge_leave_stages.values())
        + list(production_manager_enter_stages.values())
        + list(production_manager_body_stages.values())
        + list(production_manager_leave_stages.values())
    )
    production_sample_snapshot_present = bool(
        production_sample_period == 256
        and production_sample_phase == 0xA5
        and production_sample_native_frequency is not None
        and production_sample_native_frequency > 0
        and production_sample_native_frequency ==
            production_sample_manager_frequency
        and production_dispatch_seal_frequency ==
            production_sample_native_frequency
        and production_dispatch_seal_period == production_sample_period
        and production_dispatch_seal_phase == production_sample_phase
        and production_sample_fallback_frequency ==
            production_sample_native_frequency
        and production_sample_fallback_period == production_sample_period
        and production_sample_fallback_phase == production_sample_phase
        and all(
            value == production_sample_native_frequency
            for value in production_sample_event_frequencies.values()
        )
        and all(
            value == production_sample_period
            for value in production_sample_event_periods.values()
        )
        and all(
            value == production_sample_phase
            for value in production_sample_event_phases.values()
        )
        and all(record["present"] for record in production_timing_records)
        and all(
            record["shapeValid"] and record["frequencyValid"]
            for record in production_timing_records
        )
        and all(value == 1 for value in
                production_dispatch_seal_reported_closure)
        and all(
            isinstance(value, int)
            for value in production_manager_rejected.values()
        )
        and all(
            isinstance(value, int)
            for value in production_writer_snapshot.values()
        )
    )

    timing_records = (
        list(native_timing_stages.values())
        + list(native_begin_sample_stages.values())
        + list(native_t2_sample_stages.values())
        + list(manager_root_stages.values())
        + list(manager_queue_stages.values())
        + list(manager_batch_stages.values())
        + list(manager_proof_stages.values())
        + list(manager_consumer_stages.values())
    )
    timing_shape_contract = {
        "diagnosticPolicyPresent": diagnostic_policy["present"],
        "diagnosticPolicyFullExact": diagnostic_policy["fullExact"],
        "diagnosticPolicyLightExact": diagnostic_policy["lightExact"],
        "diagnosticPolicyRecognizedExact": diagnostic_policy[
            "recognizedExact"
        ],
        # Compatibility alias for the full diagnostics gate.
        "diagnosticPolicyExact": diagnostic_policy["fullExact"],
        "allPresent": all(record["present"] for record in timing_records),
        "allShapeValid": all(record["shapeValid"] for record in timing_records),
        "allFrequencyValid": all(
            record["frequencyValid"] for record in timing_records
        ),
        "frequencyMatch": (
            native_timing_frequency == manager_timing_frequency
            and native_t2_sample_frequency == native_timing_frequency
            if native_timing_frequency is not None
            and native_t2_sample_frequency is not None
            and manager_timing_frequency is not None else None
        ),
        "nativeBeginSamplePeriodValid": native_begin_sample_period == 127,
        "nativeT2SamplePeriodValid": native_t2_sample_period == 127,
        "nativeSamplePeriodsMatch": (
            native_begin_sample_period == native_t2_sample_period
            if native_begin_sample_period is not None
            and native_t2_sample_period is not None else None
        ),
    }
    timing_closures = {
        # prepare is already a child of flush, and positive is already a child
        # of scan.  Keep those nesting levels separate to prevent double-count.
        "flushAssembly": _timing_contains(
            manager_root_stages["flush"],
            [
                manager_queue_stages["control"],
                manager_queue_stages["static"],
                manager_root_stages["prepare"],
                manager_queue_stages["collision"],
                manager_batch_stages["assemble"],
                manager_batch_stages["publish"],
            ],
        ),
        "prepareArray": _timing_contains(
            manager_root_stages["prepare"],
            [manager_queue_stages["scan"]],
        ),
        "queueScan": _timing_contains(
            manager_queue_stages["scan"],
            [manager_queue_stages["positive"]],
        ),
        "positiveCandidate": _timing_contains(
            manager_queue_stages["positive"],
            [
                manager_queue_stages["binding"],
                manager_queue_stages["staticLookup"],
                manager_queue_stages["paletteCopy"],
                manager_queue_stages["build"],
            ],
        ),
        "assemble": _timing_contains(
            manager_batch_stages["assemble"],
            [manager_batch_stages["lease"], manager_batch_stages["finalize"]],
        ),
        "finalizeCompute": _timing_contains(
            manager_batch_stages["finalize"],
            [manager_batch_stages["upload"]],
        ),
        "nativeOuter": _timing_contains(
            native_timing_stages["outer"],
            [
                native_timing_stages["begin"], native_timing_stages["eval"],
                native_timing_stages["cpuKernel"],
                native_timing_stages["notify"],
                native_timing_stages["complete"],
            ],
        ),
        "preflightEval": _timing_contains(
            native_timing_stages["eval"],
            [
                manager_proof_stages["preManager"],
                manager_proof_stages["preHost"],
                manager_proof_stages["preFinalize"],
            ],
        ),
        "preflightHostPlan": _timing_contains(
            manager_proof_stages["preHost"],
            [manager_consumer_stages["plan"]],
        ),
        "cpuProofNotify": _timing_contains(
            native_timing_stages["notify"],
            [
                manager_proof_stages["cpuManager"],
                manager_proof_stages["cpuHost"],
                manager_proof_stages["cpuFinalize"],
            ],
        ),
        "completion": _timing_contains(
            native_timing_stages["complete"],
            [manager_proof_stages["completion"]],
        ),
    }
    validate_palette_ticks = _timing_ticks(manager_proof_stages["palette"])
    validate_palette_parents = [
        _timing_ticks(manager_proof_stages["preManager"]),
        _timing_ticks(manager_proof_stages["completion"]),
    ]
    palette_closure_available = (
        validate_palette_ticks is not None
        and all(value is not None for value in validate_palette_parents)
    )
    palette_parent_ticks = sum(
        value for value in validate_palette_parents if value is not None
    )
    timing_closures["validatePalette"] = {
        "available": palette_closure_available,
        "parentTicks": palette_parent_ticks if palette_closure_available else None,
        "childTicks": validate_palette_ticks if palette_closure_available else None,
        "residualTicks": (
            palette_parent_ticks - validate_palette_ticks
            if palette_closure_available else None
        ),
        "contains": (
            palette_parent_ticks >= validate_palette_ticks
            if palette_closure_available else None
        ),
    }
    timing_closures["validateStatic"] = _timing_contains(
        manager_proof_stages["preManager"],
        [manager_proof_stages["static"]],
    )
    timing_closures["nativeBeginSample"] = _native_begin_sample_closure(
        native_begin_sample_stages
    )
    timing_closures["nativeT2Sample"] = _native_t2_sample_closure(
        native_begin_sample_stages["exact"], native_t2_sample_stages,
    )
    timing_shape_contract["allClosuresContain"] = all(
        closure["contains"] is True for closure in timing_closures.values()
    )

    outer_admission_accepted = {
        "noPoison": outer_admission_values[0],
        "withPoison": outer_admission_values[1],
    }
    outer_admission_accepted["total"] = (
        sum(outer_admission_accepted.values())
        if all(isinstance(value, int)
               for value in outer_admission_accepted.values()) else None
    )
    outer_admission_cancellations = outer_admission_values[2]
    outer_admission_lifecycle_excluded = outer_admission_values[3]
    outer_admission_tracked_resolved_inside = outer_admission_values[4]
    outer_admission_untracked_resolved_outside = outer_admission_values[5]
    outer_admission_reported_totals = {
        "rejectNoPoison": outer_admission_values[6],
        "rejectWithPoison": outer_admission_values[7],
        "attempt": outer_admission_values[8],
        "trackedResolvedOutside": outer_admission_values[9],
        "resolvedExpectedOutside": outer_admission_values[10],
        "actualOutside": outer_admission_values[11],
    }
    outer_admission_reported_closure = {
        "snapshotAvailable": outer_admission_values[12],
        "acceptedResolutionClean": outer_admission_values[13],
        "outsideClean": outer_admission_values[14],
        "unknownHardZero": outer_admission_values[15],
    }
    outer_admission_main_present = all(
        isinstance(value, int) and 0 <= value <= (1 << 64) - 1
        for value in outer_admission_values
    )
    outer_admission_reason_present = all(
        isinstance(value, int)
        and 0 <= value <= (1 << 64) - 1
        for table in (
            outer_reject_no_poison, outer_reject_with_poison,
        )
        for value in table.values()
    )
    reject_no_poison_total = (
        sum(outer_reject_no_poison.values())
        if outer_admission_reason_present else None
    )
    reject_with_poison_total = (
        sum(outer_reject_with_poison.values())
        if outer_admission_reason_present else None
    )
    reject_total = (
        reject_no_poison_total + reject_with_poison_total
        if isinstance(reject_no_poison_total, int)
        and isinstance(reject_with_poison_total, int) else None
    )
    attempt_computed = (
        outer_admission_accepted["total"] + reject_total
        if isinstance(outer_admission_accepted["total"], int)
        and isinstance(reject_total, int) else None
    )
    tracked_resolved_computed = (
        attempt_computed - outer_admission_cancellations
        - outer_admission_lifecycle_excluded
        - outer_admission_tracked_resolved_inside
        if isinstance(attempt_computed, int)
        and attempt_computed <= (1 << 64) - 1
        and isinstance(outer_admission_cancellations, int)
        and isinstance(outer_admission_lifecycle_excluded, int)
        and isinstance(outer_admission_tracked_resolved_inside, int)
        and attempt_computed >= (
            outer_admission_cancellations
            + outer_admission_lifecycle_excluded
            + outer_admission_tracked_resolved_inside
        ) else None
    )
    resolved_computed = (
        tracked_resolved_computed
        + outer_admission_untracked_resolved_outside
        if isinstance(tracked_resolved_computed, int)
        and isinstance(outer_admission_untracked_resolved_outside, int)
        and outer_admission_untracked_resolved_outside <=
            (1 << 64) - 1 - tracked_resolved_computed
        else None
    )
    outside_native_fast_parent = _match_named_int(
        native_fast, "outsideNativeFast"
    )
    direct_original_values = _match_tuple(
        native_fast,
        r"\bdirectOriginal=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/"
        r"(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
        10,
    )
    poison_scan_parent = _match_named_int(native_fast, "poisonScan")
    poison_no_overlap_parent = _match_named_int(
        native_fast, "poisonNoOverlap"
    )
    poison_overlap_parent = _match_named_int(
        native_fast, "poisonOverlap"
    )
    poison_read_fail_parent = _match_named_int(
        native_fast, "poisonReadFail"
    )
    poison_read_reason_total = (
        outer_reject_with_poison.get("poisonReadFailure", 0)
        + outer_reject_with_poison.get("poisonPostScanRevalidation", 0)
        + outer_reject_with_poison.get("independentPinRevalidation", 0)
        if all(isinstance(outer_reject_with_poison.get(name), int)
               for name in (
                   "poisonReadFailure", "poisonPostScanRevalidation",
                   "independentPinRevalidation",
               )) else None
    )
    poison_accepted_partition = _outside_poison_accepted_partition_contract(
        outer_admission_accepted.get("withPoison"),
        poison_no_overlap_parent,
        poison_overlap_parent,
        poison_read_fail_parent,
        poison_o1_authority.get("attempts"),
        poison_sidecar_policy,
    )
    poison_scan_authority_partition = (
        _outside_poison_scan_authority_contract(
            poison_scan_parent,
            poison_o1_authority.get("attempts"),
            poison_o1_authority.get("evidence", {}).get("attempts"),
            poison_sidecar_policy,
        )
    )
    outer_admission_computed_closure = {
        "accepted": bool(
            isinstance(outer_admission_accepted["total"], int)
            and outer_admission_accepted["total"] ==
                outer_admission_accepted["noPoison"]
                + outer_admission_accepted["withPoison"]
        ),
        "rejectNoPoison": bool(
            isinstance(reject_no_poison_total, int)
            and outer_admission_reported_totals["rejectNoPoison"] ==
                reject_no_poison_total
        ),
        "rejectWithPoison": bool(
            isinstance(reject_with_poison_total, int)
            and outer_admission_reported_totals["rejectWithPoison"] ==
                reject_with_poison_total
        ),
        "attempt": bool(
            isinstance(attempt_computed, int)
            and outer_admission_reported_totals["attempt"] ==
                attempt_computed
        ),
        "trackedResolved": bool(
            isinstance(tracked_resolved_computed, int)
            and outer_admission_reported_totals[
                "trackedResolvedOutside"
            ] == tracked_resolved_computed
        ),
        "resolvedExpected": bool(
            isinstance(resolved_computed, int)
            and outer_admission_reported_totals[
                "resolvedExpectedOutside"
            ] == resolved_computed
        ),
        "unknownHardZero": bool(
            outer_reject_no_poison.get("unknown") == 0
            and outer_reject_with_poison.get("unknown") == 0
        ),
        "populationSentinelsHardZero": bool(
            all(
                table.get(name) == 0
                for table in (
                    outer_reject_no_poison,
                    outer_reject_with_poison,
                )
                for name in (
                    "unknown", "modeNotBypass", "fullDiagnostics",
                    "dispatchOwned",
                )
            )
        ),
        "acceptedResolution": bool(
            isinstance(outer_admission_accepted["total"], int)
            and outer_admission_accepted["total"] ==
                outside_native_fast_parent
        ),
        "outside": bool(
            isinstance(resolved_computed, int)
            and resolved_computed ==
                outer_admission_reported_totals["actualOutside"]
            and resolved_computed == native_upload_coverage[1]
        ),
        "poisonScan": bool(
            all(isinstance(value, int) for value in (
                poison_scan_parent, poison_no_overlap_parent,
                poison_overlap_parent, poison_read_fail_parent,
            ))
            and poison_scan_parent == poison_no_overlap_parent
                + poison_overlap_parent + poison_read_fail_parent
        ),
        "poisonAccepted": poison_accepted_partition["exact"],
        "poisonScanAuthority": poison_scan_authority_partition["exact"],
        "poisonReason": bool(
            outer_reject_no_poison.get("poisonReadFailure") == 0
            and outer_reject_no_poison.get("poisonOverlap") == 0
            and outer_reject_no_poison.get(
                "poisonPostScanRevalidation"
            ) == 0
            and outer_reject_with_poison.get("poisonOverlap") ==
                poison_overlap_parent
            and poison_read_reason_total == poison_read_fail_parent
        ),
    }
    outer_admission_alias_partitions = {
        "acceptedClass": _timing_partition_contract(
            production_outer_stages["admitAccepted"],
            outer_admission_accepted_class_timing,
        ),
        "completeClass": _timing_partition_contract(
            production_outer_stages["complete"],
            outer_fast_complete_class_timing,
        ),
        "fallbackReason": _timing_partition_contract(
            production_outer_stages["fallbackInclusive"],
            outer_fallback_reason_timing,
        ),
    }
    outer_alias_records = (
        list(outer_admission_accepted_class_timing.values())
        + list(outer_fast_complete_class_timing.values())
        + list(outer_fallback_reason_timing.values())
    )
    outer_alias_present = bool(
        all(record.get("present") is True for record in outer_alias_records)
        and all(
            value is not None
            for value in outer_admission_alias_reported.values()
        )
    )
    outer_alias_shape_valid = all(
        record.get("shapeValid") is True
        and record.get("frequencyValid") is True
        for record in outer_alias_records
    )
    outer_alias_unknown_zero = bool(
        all(
            outer_fallback_reason_timing["unknown"].get(field) == 0
            for field in ("calls", "ticks", "maxTicks")
        )
    )
    outer_alias_class_calls_match = bool(
        all(
            outer_admission_accepted_class_timing[name].get("calls") ==
                outer_fast_complete_class_timing[name].get("calls")
            for name in _GPU_SKIN_OUTSIDE_ADMISSION_CLASSES
        )
    )
    outer_alias_all_zero = bool(
        all(
            record.get(field) == 0
            for record in outer_alias_records
            for field in ("calls", "ticks", "maxTicks")
        )
    )
    outer_exact_fields_zero = bool(
        outer_admission_accepted.get("total") == 0
        and outer_admission_cancellations == 0
        and outer_admission_lifecycle_excluded == 0
        and outer_admission_tracked_resolved_inside == 0
        and outer_admission_untracked_resolved_outside == 0
        and reject_total == 0
        and outer_admission_reported_totals.get("rejectNoPoison") == 0
        and outer_admission_reported_totals.get("rejectWithPoison") == 0
        and outer_admission_reported_totals.get("attempt") == 0
        and outer_admission_reported_totals.get(
            "trackedResolvedOutside"
        ) == 0
        and outer_admission_reported_totals.get(
            "resolvedExpectedOutside"
        ) == 0
    )
    outer_light_policy_clean = bool(
        diagnostic_light_exact and outer_admission_main_present
        and outer_admission_reason_present and outer_alias_present
        and outer_alias_shape_valid
        and all(outer_admission_computed_closure.values())
        and outer_admission_cancellations == 0
        and outer_admission_lifecycle_excluded == 0
        and all(
            outer_admission_reported_closure.get(name) == 1
            for name in (
                "snapshotAvailable", "acceptedResolutionClean",
                "outsideClean", "unknownHardZero",
            )
        )
        and all(
            contract.get("closed") is True
            for contract in outer_admission_alias_partitions.values()
        )
        and outer_alias_class_calls_match
        and outer_alias_unknown_zero
        and outer_admission_alias_reported.get("snapshotAvailable") == 1
        and outer_admission_alias_reported.get("acceptedClassClean") == 1
        and outer_admission_alias_reported.get("completeClassClean") == 1
        and outer_admission_alias_reported.get("fallbackReasonClean") == 1
        and outer_admission_alias_reported.get("unknownHardZero") == 1
        and outer_admission_alias_reported.get("contract") ==
            "partition-alias-inclusive-do-not-add-to-parent"
        and isinstance(outer_admission_accepted.get("total"), int)
        and outer_admission_accepted["total"] > 0
    )
    outer_full_policy_clean = bool(
        diagnostic_full_exact and outer_admission_main_present
        and outer_admission_reason_present and outer_alias_present
        and outer_alias_shape_valid and outer_exact_fields_zero
        and outer_alias_all_zero
        and all(
            contract.get("closed") is True
            for contract in outer_admission_alias_partitions.values()
        )
        and outer_alias_class_calls_match
        and outer_admission_reported_totals.get("actualOutside") ==
            native_upload_coverage[1]
        and all(
            outer_admission_reported_closure.get(name) == 0
            for name in (
                "snapshotAvailable", "acceptedResolutionClean",
                "outsideClean", "unknownHardZero",
            )
        )
        and all(
            outer_admission_alias_reported.get(name) == 0
            for name in (
                "snapshotAvailable", "acceptedClassClean",
                "completeClassClean", "fallbackReasonClean",
                "unknownHardZero",
            )
        )
        and outer_admission_alias_reported.get("contract") ==
            "partition-alias-inclusive-do-not-add-to-parent"
    )
    outside_admission_attribution = {
        "populationContract": (
            "bypass-light-entry-dispatch-depth-zero-first-terminal-"
            "plus-final-boundary-v2"
        ),
        "attemptContract": "accepted-plus-first-reject-disjoint",
        "outsideClosureDenominator": (
            "attempt-minus-cancellation-minus-lifecycleExcluded-minus-"
            "trackedResolvedInside-plus-untrackedResolvedOutside"
        ),
        "reasonOrder": list(_GPU_SKIN_OUTSIDE_REJECT_REASONS),
        "accepted": outer_admission_accepted,
        "cancellations": outer_admission_cancellations,
        "lifecycleExcluded": outer_admission_lifecycle_excluded,
        "trackedResolvedInside": outer_admission_tracked_resolved_inside,
        "untrackedResolvedOutside": (
            outer_admission_untracked_resolved_outside
        ),
        "rejectNoPoison": outer_reject_no_poison,
        "rejectWithPoison": outer_reject_with_poison,
        "reportedTotals": outer_admission_reported_totals,
        "computedTotals": {
            "rejectNoPoison": reject_no_poison_total,
            "rejectWithPoison": reject_with_poison_total,
            "reject": reject_total,
            "attempt": attempt_computed,
            "trackedResolvedOutside": tracked_resolved_computed,
            "resolvedExpectedOutside": resolved_computed,
            "telemetryBatchedAddTerm": (
                attempt_computed + outer_admission_cancellations
                + outer_admission_lifecycle_excluded
                + outer_admission_tracked_resolved_inside
                + outer_admission_untracked_resolved_outside
                if isinstance(attempt_computed, int)
                and isinstance(outer_admission_cancellations, int)
                and isinstance(outer_admission_lifecycle_excluded, int)
                and isinstance(
                    outer_admission_tracked_resolved_inside, int
                )
                and isinstance(
                    outer_admission_untracked_resolved_outside, int
                )
                else None
            ),
        },
        "reportedClosure": outer_admission_reported_closure,
        "computedClosure": outer_admission_computed_closure,
        "poisonAcceptedPartition": poison_accepted_partition,
        "poisonScanAuthorityPartition": poison_scan_authority_partition,
        "aliases": {
            "contract": outer_admission_alias_reported.get("contract"),
            "acceptedByClass": outer_admission_accepted_class_timing,
            "completeByClass": outer_fast_complete_class_timing,
            "fallbackByReason": outer_fallback_reason_timing,
            "reportedClosure": outer_admission_alias_reported,
            "computedPartitions": outer_admission_alias_partitions,
            "acceptedCompleteClassCallsMatch": (
                outer_alias_class_calls_match
            ),
            "unknownHardZero": outer_alias_unknown_zero,
            "allZero": outer_alias_all_zero,
            "note": (
                "Inclusive aliases partition parents; do not add aliases "
                "to parent timings."
            ),
        },
        "present": bool(
            outer_admission_main_present
            and outer_admission_reason_present and outer_alias_present
        ),
        "lightPolicyClean": outer_light_policy_clean,
        "fullPolicyClean": outer_full_policy_clean,
        "policyClean": bool(
            outer_light_policy_clean or outer_full_policy_clean
        ),
        "raw": {
            "main": native_outer_admission_exact,
            "rejectNoPoison": native_outer_reject_no_poison,
            "rejectWithPoison": native_outer_reject_with_poison,
            "aliasClosure": native_prod_outer_alias_closure,
        },
    }

    exact_conflict = _find_optional_exact_conflict(text)
    return {
        "rawLatest": latest,
        "diagnosticPolicy": diagnostic_policy,
        "nativePoisonSidecarPolicy": poison_sidecar_policy,
        "quiescence": quiescence,
        "protocol": {
            "flushCallbacks": _match_named_int(mode, "flush"),
            "dispatchBegin": mode_values[0], "dispatchEnd": mode_values[1],
            "truePairErr": mode_values[2], "epochLeak": mode_values[3],
            "pending": mode_values[4:7],
        },
        "managerDispatch": {
            "physicalScopes": manager_dispatch_physical[0],
            "physicalEnds": manager_dispatch_physical[1],
            "commonScopes": manager_dispatch_shape[0],
            "specialScopes": manager_dispatch_shape[1],
            "semanticScopes": manager_dispatch_shape[2],
            "eagerScopes": manager_dispatch_scope_class[0],
            "lazyScopes": manager_dispatch_scope_class[1],
            "neverScopes": manager_dispatch_scope_class[2],
            "nativeCpuOnlyScopes": manager_dispatch_native_cpu_only[0],
            "nativeCpuOnlyEnds": manager_dispatch_native_cpu_only[1],
            "evidenceEagerScopes": manager_dispatch_evidence,
            "beginCallbacks": manager_dispatch_callbacks[0],
            "endCallbacks": manager_dispatch_callbacks[1],
            "eagerBegins": manager_dispatch_begin_class[0],
            "lazyAdmissionAttempts": manager_dispatch_begin_class[1],
            "lazyAdmissions": manager_dispatch_begin_class[2],
            "eagerAdmissionFailures": manager_dispatch_failures[0],
            "lazyAdmissionFailures": manager_dispatch_failures[1],
            "neverSafetyFailures": manager_dispatch_failures[2],
            "issuedEnds": manager_dispatch_end_class[0],
            "noUploadEnds": manager_dispatch_end_class[1],
            "neverEnds": manager_dispatch_end_class[2],
            "failedEnds": manager_dispatch_end_class[3],
            "skippedUploads": manager_dispatch_skipped[0],
            "skippedDips": manager_dispatch_skipped[1],
            "skippedFanouts": manager_dispatch_skipped[2],
            "telemetryFlushes": manager_dispatch_telemetry[0],
            "telemetryBatchedAdds": manager_dispatch_telemetry[1],
            "telemetryDeltaPending": manager_dispatch_telemetry[2],
            "telemetryDeltaFaulted": manager_dispatch_telemetry[3],
            "rawDips": native_dip_coverage[0],
            "correlatedDips": native_dip_coverage[1],
            "unmatchedDips": native_dip_coverage[2],
            "outsideDips": native_dip_coverage[3],
            "noUploadDips": native_dip_coverage[4],
            "outsideDipFastPath": native_dip_coverage[5],
            "observerBegins": dip_fast_probe_observer[0],
            "observerEnds": dip_fast_probe_observer[1],
            "observerMismatches": dip_fast_probe_observer[2],
            "readerBegins": dip_fast_probe_reader[0],
            "readerEnds": dip_fast_probe_reader[1],
            "readerCommits": dip_fast_probe_reader[2],
            "readerRejects": dip_fast_probe_reader[3],
            "readerEvidenceFallbacks": dip_fast_probe_reader[4],
            "readerMismatches": dip_fast_probe_reader[5],
            "fastByFlush": dip_fast_probe_cover[0],
            "fastByObserver": dip_fast_probe_cover[1],
            "fastByReader": dip_fast_probe_cover[2],
            "outsideAdmissionAttemptTotal": outer_admission_values[8],
            "outsideAdmissionCancellations": outer_admission_values[2],
            "outsideAdmissionLifecycleExcluded": outer_admission_values[3],
            "outsideAdmissionTrackedResolvedInside": (
                outer_admission_values[4]
            ),
            "outsideAdmissionUntrackedResolvedOutside": (
                outer_admission_values[5]
            ),
            "raw": native_manager_dispatch,
        },
        "dispatchCpuOnlySeal": {
            "viewPublishes": dispatch_seal_view[0],
            "viewQueries": dispatch_seal_view[1],
            "authorityRejects": dispatch_seal_view[2],
            "candidateRejects": dispatch_seal_view[3],
            "managerProposals": dispatch_seal_view[4],
            "localViewPublishAttempts": dispatch_seal_local_view[0],
            "localViewPublishes": dispatch_seal_local_view[1],
            "localViewRejects": dispatch_seal_local_view[2],
            "localViewQueries": dispatch_seal_local_view[3],
            "localViewAuthorityRejects": dispatch_seal_local_view[4],
            "localViewCandidateRejects": dispatch_seal_local_view[5],
            "localViewCommits": dispatch_seal_local_view[6],
            "nativeCpuOnlyScopes": dispatch_seal_native_cpu_only[0],
            "nativeCpuOnlyEnds": dispatch_seal_native_cpu_only[1],
            "proposals": dispatch_seal_proposal[0],
            "proposalAccepted": dispatch_seal_proposal[1],
            "proposalRejected": dispatch_seal_proposal[2],
            "proposalAborted": dispatch_seal_proposal[3],
            "scopeCommits": dispatch_seal_scope[0],
            "scopeEnds": dispatch_seal_scope[1],
            "invalidations": dispatch_seal_scope[2],
            "uploadsStarted": dispatch_seal_upload[0],
            "uploadsCompleted": dispatch_seal_upload[1],
            "vertices": dispatch_seal_vertices,
            "bytes": dispatch_seal_bytes,
            "kernelCalls": dispatch_seal_kernel[0],
            "kernelNormalReturns": dispatch_seal_kernel[1],
            "dips": dispatch_seal_dip[0],
            "dipsWithUpload": dispatch_seal_dip[1],
            "dipsNoUpload": dispatch_seal_dip[2],
            "fanoutZero": dispatch_seal_fanout[0],
            "fanoutOne": dispatch_seal_fanout[1],
            "fanoutMany": dispatch_seal_fanout[2],
            "fanoutDipTotal": dispatch_seal_fanout[3],
            "markerConflicts": dispatch_seal_conflicts,
            "reportedClosure": {
                "managerView": dispatch_seal_closure[0],
                "managerBridge": dispatch_seal_closure[1],
                "proposal": dispatch_seal_closure[2],
                "scope": dispatch_seal_closure[3],
                "upload": dispatch_seal_closure[4],
                "kernel": dispatch_seal_closure[5],
                "dip": dispatch_seal_closure[6],
            },
            "raw": native_dispatch_seal,
        },
        "outsideAdmissionAttribution": outside_admission_attribution,
        "dipFastProbe": {
            "period": dip_fast_probe_period,
            "phases": {
                "early": dip_fast_probe_phases[0],
                "late": dip_fast_probe_phases[1],
            },
            "present": dip_fast_probe_present,
            "attemptsDerived": dip_fast_probe_attempts_derived == 1,
            "closureKind": "derived-outcome-sum-v1",
            "early": {
                "attempts": dip_fast_probe_early[0],
                "successes": dip_fast_probe_early[1],
                "rejects": dict(zip(
                    _GPU_SKIN_DIP_FAST_REJECT_REASONS,
                    dip_fast_probe_early_rejects,
                )),
                "closed": dip_fast_probe_early_closed,
                "localRejects": dict(zip(
                    _GPU_SKIN_DIP_FAST_LOCAL_REJECT_REASONS,
                    dip_fast_probe_early_local,
                )),
                "localClosed": dip_fast_probe_early_local_closed,
            },
            "late": {
                "attempts": dip_fast_probe_late[0],
                "successes": dip_fast_probe_late[1],
                "rejects": dict(zip(
                    _GPU_SKIN_DIP_FAST_REJECT_REASONS,
                    dip_fast_probe_late_rejects,
                )),
                "closed": dip_fast_probe_late_closed,
                "localRejects": dict(zip(
                    _GPU_SKIN_DIP_FAST_LOCAL_REJECT_REASONS,
                    dip_fast_probe_late_local,
                )),
                "localClosed": dip_fast_probe_late_local_closed,
            },
            "contractClosed": (
                dip_fast_probe_early_closed and dip_fast_probe_late_closed
                and dip_fast_probe_early_local_closed
                and dip_fast_probe_late_local_closed
                and dip_fast_probe_observer_closed
                and dip_fast_probe_reader_closed
                and dip_fast_probe_cover_closed
            ),
            "allZero": dip_fast_probe_all_zero,
            "observer": {
                "begins": dip_fast_probe_observer[0],
                "ends": dip_fast_probe_observer[1],
                "mismatches": dip_fast_probe_observer[2],
                "closed": dip_fast_probe_observer_closed,
            },
            "reader": {
                **dict(zip(
                    _GPU_SKIN_DIP_FAST_READER_FIELDS,
                    dip_fast_probe_reader,
                )),
                "closed": dip_fast_probe_reader_closed,
                "evidenceWithinRejects": bool(
                    dip_fast_probe_present
                    and dip_fast_probe_reader[4] <= dip_fast_probe_reader[3]
                ),
                "commitCoverExact": dip_fast_probe_reader_cover_exact,
            },
            "cover": {
                **dict(zip(
                    _GPU_SKIN_DIP_FAST_COVER_FIELDS,
                    dip_fast_probe_cover,
                )),
                "readerCommitExact": dip_fast_probe_reader_cover_exact,
                "closed": dip_fast_probe_cover_closed,
            },
        },
        "compute": {
            "batch": _match_tuple(compute, r"\bbatch=(\d+)/(\d+)/(\d+)", 3),
            "jobs": _match_tuple(compute, r"\bjobs=(\d+)/(\d+)", 2),
            "dispatch": _match_tuple(compute, r"\bdispatch=(\d+)/(\d+)", 2),
            "palette": _match_tuple(compute, r"\bpalette=(\d+)/(\d+)", 2),
        },
        "VSRoute": {
            "route": _match_named_int(vs_route, "route"),
            "explicit": _match_named_int(vs_route, "explicit"),
            "invalid": _match_named_int(vs_route, "invalid"),
            "inputPrepared": _match_tuple(
                vs_route, r"\binputPrepared=(\d+)/(\d+)", 2,
            ),
            "inputSubmitted": _match_tuple(
                vs_route, r"\binputSubmitted=(\d+)/(\d+)", 2,
            ),
            "main": _match_tuple(
                vs_route, r"\bmain=(\d+)/(\d+)/(\d+)/(\d+)", 4,
            ),
            "cleared": _match_named_int(vs_route, "cleared"),
            "inputConsumers": _match_tuple(
                vs_route, r"\binputConsumers=(\d+)/(\d+)/(\d+)", 3,
            ),
            "inputOnly": _match_tuple(
                vs_route,
                r"\binputOnly=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
                6,
            ),
            "shadowCapture": _match_tuple(
                vs_route,
                r"\bshadowCapture=(\d+)/(\d+)/(\d+)/(\d+)", 4,
            ),
            "shadowDirect": _match_tuple(
                vs_route,
                r"\bshadowDirect=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)",
                5,
            ),
            "shadowReplay": _match_tuple(
                vs_route, r"\bshadowReplay=(\d+)/(\d+)/(\d+)", 3,
            ),
            "raw": vs_route,
        },
        "productionFastPath": {
            "rejectScope": fast_reject_scope,
            "rejectState": fast_reject_state,
            "rejectSkinFormat": fast_reject_skin,
            "rejectInput": fast_reject_input,
            "rejectSmall": fast_reject_small,
            "unknownState": _match_named_int(native_fast, "unknownState"),
            "candidates": fast_candidates,
            "dispatchSealUploads": fast_dispatch_seal_uploads,
            "poisonRetirementOnly": _match_named_int(
                native_fast, "poisonRetireOnly"
            ),
            "outsideCallbacksSkipped": _match_named_int(
                native_fast, "outsideCallbackSkip"
            ),
            "outsideNativeFastPath": _match_named_int(
                native_fast, "outsideNativeFast"
            ),
            "directOriginalAttempts": direct_original_values[0],
            "directOriginalKernelCalls": direct_original_values[1],
            "directOriginalNormalReturns": direct_original_values[2],
            "directOriginalKernelNoNormalReturns": (
                direct_original_values[3]
            ),
            "directOriginalCompleted": direct_original_values[4],
            "directOriginalConflicts": direct_original_values[5],
            "directOriginalCancellations": direct_original_values[6],
            "directOriginalActive": direct_original_values[7],
            "directOriginalResetCompletedWhileActive": (
                direct_original_values[8]
            ),
            "directOriginalLatePoison": direct_original_values[9],
            "kernelBatches": _match_named_int(
                native_fast, "rejectKernelBatch"
            ),
            "poisonScanAttempts": _match_named_int(
                native_fast, "poisonScan"
            ),
            "poisonNoOverlap": _match_named_int(
                native_fast, "poisonNoOverlap"
            ),
            "poisonOverlap": _match_named_int(
                native_fast, "poisonOverlap"
            ),
            "poisonReadFail": _match_named_int(
                native_fast, "poisonReadFail"
            ),
            "markerConflicts": _match_named_int(
                native_fast, "markerConflict"
            ),
            "coverFlush": _match_tuple(
                native_fast, r"\bcoverBegin=(\d+)/(\d+)/(\d+)", 3,
            )[0],
            "coverSemantic": _match_tuple(
                native_fast, r"\bcoverBegin=(\d+)/(\d+)/(\d+)", 3,
            )[1],
            "coverIndependent": _match_tuple(
                native_fast, r"\bcoverBegin=(\d+)/(\d+)/(\d+)", 3,
            )[2],
            "independentPinBegins": _match_tuple(
                native_fast, r"\bindependentPin=(\d+)/(\d+)", 2,
            )[0],
            "independentPinEnds": _match_tuple(
                native_fast, r"\bindependentPin=(\d+)/(\d+)", 2,
            )[1],
            "rawUploads": fast_raw_uploads,
            "classified": fast_classified,
            "partitionClosed": fast_partition_closed,
            "strictInputIncludesIntentionalSmall": fast_reject_small,
            "raw": native_fast,
        },
        "productionSampleTiming": {
            "present": production_sample_snapshot_present,
            "period": production_sample_period,
            "phase": production_sample_phase,
            "frequency": {
                "native": production_sample_native_frequency,
                "manager": production_sample_manager_frequency,
            },
            "writerSnapshot": production_writer_snapshot,
            "outerStages": production_outer_stages,
            "dispatchSealStages": production_dispatch_seal_stages,
            "dispatchSealReportedClosure": {
                "calls": production_dispatch_seal_reported_closure[0],
                "ticks": production_dispatch_seal_reported_closure[1],
                "max": production_dispatch_seal_reported_closure[2],
                "cancelZero": production_dispatch_seal_reported_closure[3],
            },
            "kernelStages": production_kernel_stages,
            "eventRootStages": production_event_root_stages,
            "eventSemanticStages": production_event_semantic_stages,
            "eventDipDeviceStages": production_event_dip_device_stages,
            "eventDipBridgeStages": production_event_dip_bridge_stages,
            "eventDipResolveStages": production_event_dip_resolve_stages,
            "bridgePinStages": production_bridge_pin_stages,
            "bridgeBodyStages": production_bridge_body_stages,
            "bridgeLeaveStages": production_bridge_leave_stages,
            "managerEnterStages": production_manager_enter_stages,
            "managerBodyStages": production_manager_body_stages,
            "managerLeaveStages": production_manager_leave_stages,
            "managerRejected": production_manager_rejected,
            "outerAdmissionAliases": {
                "acceptedByClass": outer_admission_accepted_class_timing,
                "completeByClass": outer_fast_complete_class_timing,
                "fallbackByReason": outer_fallback_reason_timing,
                "closure": outer_admission_alias_reported,
            },
            "raw": {
                "outer": native_prod_outer,
                "dispatchSeal": native_prod_dispatch_seal,
                "fallback": native_prod_fallback,
                "kernel": native_prod_kernel,
                "eventRoot": native_prod_event_root,
                "eventSemantic": native_prod_event_semantic,
                "eventDipDevice": native_prod_event_dip_device,
                "eventDipBridge": native_prod_event_dip_bridge,
                "eventDipResolve": native_prod_event_dip_resolve,
                "bridgePin": native_prod_callback_pin,
                "bridgeBody": native_prod_callback_body,
                "bridgeLeave": native_prod_callback_leave,
                "managerEnter": manager_prod_callback_enter,
                "managerBody": manager_prod_callback_body,
                "managerLeave": manager_prod_callback_leave,
                "managerRejected": manager_prod_callback_reject,
            },
        },
        "hotPathTiming": {
            "nativeRaw": native_timing,
            "nativeBeginSampleRaw": native_begin_sample,
            "nativeT2SampleRaw": native_t2_sample,
            "managerRaw": manager_hot,
            "managerQueueRaw": manager_queue_time,
            "managerBatchRaw": manager_batch_time,
            "managerProofRaw": manager_proof_time,
            "managerConsumerRaw": manager_consumer_time,
            "frequency": {
                "native": native_timing_frequency,
                "nativeT2": native_t2_sample_frequency,
                "manager": manager_timing_frequency,
            },
            "nativeStages": native_timing_stages,
            "nativeBeginSamplePeriod": native_begin_sample_period,
            "nativeBeginSampleStages": native_begin_sample_stages,
            "nativeT2SamplePeriod": native_t2_sample_period,
            "nativeT2SampleStages": native_t2_sample_stages,
            "nativeT2SampleEvidencePositive": timing_closures[
                "nativeT2Sample"
            ]["evidencePositive"],
            "managerRootStages": manager_root_stages,
            "managerQueueStages": manager_queue_stages,
            "managerBatchStages": manager_batch_stages,
            "managerProofStages": manager_proof_stages,
            "managerConsumerStages": manager_consumer_stages,
            "contracts": timing_shape_contract,
            "closures": timing_closures,
            "renderQueue": dict(zip(
                ("visited", "reverseHits", "reverseMisses"), manager_queue,
            )),
            "renderableBloom": dict(zip(
                ("rejects", "maybes"),
                renderable_bloom,
            )),
            "renderableBloomClosure": {
                "available": bloom_closure_available,
                "bloomClassified": bloom_classified,
                "reverseClassified": reverse_classified,
                "exact": (
                    bloom_classified == reverse_classified
                    if bloom_closure_available else None
                ),
            },
            "bypassStaticHint": dict(zip(
                ("hits", "misses"),
                bypass_static_hint,
            )),
            "cpuPreferredSmallJobs": _match_named_int(
                manager_hot, "cpuSmall"
            ),
        },
        "formatCoverage": {
            "formatBuckets": dict(zip(
                ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                format_buckets,
            )),
            "skinBuckets": dict(zip(
                ("s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"),
                skin_buckets,
            )),
            "nativeUploadRaw": native_upload_coverage[0],
            "nativeUploadOutside": native_upload_coverage[1],
            "nativeFanout": dict(zip(
                ("zero", "one", "many", "max"), native_fanout,
            )),
            "eligible": _match_named_int(mode, "eligible"),
            "strictReject": dict(zip(
                (
                    "path", "stage", "skin", "input", "output",
                    "identity", "cpu", "bypass",
                ),
                strict_reject_values,
            )),
            "flow": {
                "eligible": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_flow_eligible,
                )),
                "learned": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_flow_learned,
                )),
                "candidate": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_flow_candidate,
                )),
                "job": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_flow_job,
                )),
                "raw": format_flow_line,
            },
            "classificationByFormat": {
                "outside": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_class_outside,
                )),
                "inside": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_class_inside,
                )),
                "eligible": dict(zip(
                    ("f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7"),
                    format_class_eligible,
                )),
                "reject": {
                    f"f{index}": dict(zip(
                        (
                            "path", "stage", "skin", "input", "output",
                            "identity", "cpu", "bypass",
                        ),
                        format_reject_by_format[index],
                    ))
                    for index in range(6)
                },
                "raw": format_class_line,
            },
        },
        # This diagnostic is intentionally report-only. Special/transparent/
        # unsupported fallbacks are expected fail-open behavior, not a P4
        # promotion failure. Older artifacts may not emit the line at all.
        "fallbackCoverage": {
            "lineSeen": bool(coverage_line),
            "available": coverage_available,
            "scopes": dict(zip(
                ("all", "common", "special"), coverage_scopes
            )),
            "fallback": dict(zip(
                (
                    "special", "path", "transparent", "stage", "skin",
                    "preflight", "layout", "output", "multi",
                ),
                coverage_fallback,
            )),
            "raw": coverage_line,
        },
        "kernel": {
            "hookCalls": kernel_calls[0], "originalCalls": kernel_calls[1],
            "bypassedCalls": kernel_calls[2], "originalBytes": kernel_bytes[0],
            "bypassedBytes": kernel_bytes[1],
            "bytesAvailable": all(value is not None for value in kernel_bytes),
            "bytesNote": "P4 diagnostics require runtime-emitted kernelBytes; nativeUpload carries separate upload byte counters.",
        },
        "nativeUpload": {
            "originalCalls": upload_calls[0], "bypassedCalls": upload_calls[1],
            "originalBytes": upload_bytes[0], "bypassedBytes": upload_bytes[1],
        },
        "nativePoison": dict(zip(("create", "clear", "hit", "overflow", "resetStale", "outstanding"), poison_values)),
        "nativePoisonShadow": poison_shadow,
        "nativePoisonO1Shadow": poison_o1_shadow,
        "nativePoisonO1Authority": poison_o1_authority,
        "nativeDirectDiscard": {
            "events": direct_discard_values[0],
            "eventsWithPoison": direct_discard_values[1],
            "noPoisonEvents": direct_discard_values[2],
            "rangesCleared": direct_discard_values[3],
            "invalid": direct_discard_values[4],
            "crossMapMerge": _match_named_int(poison, "crossMapMerge"),
        },
        "nativeRetirement": {
            "published": _match_named_int(retirement, "published"),
            "consumed": _match_named_int(retirement, "consumed"),
            "cleared": _match_named_int(retirement, "cleared"),
            "overflow": _match_named_int(retirement, "overflow"),
            "invalid": _match_named_int(retirement, "invalid"),
            "pending": _match_named_int(retirement, "pending"),
            "fault": _match_named_int(retirement, "fault"),
        },
        "nativeReset": {
            "requests": _match_named_int(reset, "requests", "request", "resetRequests"),
            "completions": _match_named_int(reset, "completions", "completed", "complete", "resetCompleted"),
            "acknowledgements": _match_named_int(reset, "acknowledgements", "acknowledged", "ack", "resetAcknowledged"),
            "requestedGeneration": _match_named_int(reset, "requestedGeneration", "requestedGen", "requestGeneration", "requestGen"),
            "completedGeneration": _match_named_int(reset, "completedGeneration", "completedGen", "completeGeneration", "completeGen"),
            "acknowledgedGeneration": _match_named_int(reset, "acknowledgedGeneration", "acknowledgedGen", "ackGeneration", "ackGen"),
            "pendingRetirement": _match_named_int(reset, "pendingRetirement", "retirementPending"),
            "deferred": _match_named_int(reset, "deferred"),
            "wrongThread": _match_named_int(reset, "wrongThread"),
            "active": _match_named_int(reset, "active"),
            "owner": _match_named_int(reset, "owner"),
            "poison": _match_named_int(reset, "poison"),
            "retirementFault": _match_named_int(reset, "retirementFault"),
            "failClosed": _match_named_int(reset, "failClosed"),
        },
        "nativeKernelNormal": {
            "overflow": _match_named_int(kernel_normal, "overflow"),
            "invalid": _match_named_int(kernel_normal, "invalid"),
            "fault": _match_named_int(kernel_normal, "fault"),
            "ackMismatch": _match_named_int(kernel_normal, "ackMismatch", "ack_mismatch"),
            "commitBlocked": _match_named_int(kernel_normal, "commitBlocked", "commit_blocked"),
        },
        "indexTicket": {
            "mask": ticket_values[0], "attempts": ticket_values[1], "exact": ticket_values[2],
            "suppressed": ticket_values[3], "leaks": ticket_values[4],
        },
        "nativeSafety": {
            "dispatchOverflow": native_values[0], "semanticOverflow": native_values[1],
            "semanticQueryMiss": native_values[2], "semanticConflict": native_values[3],
            "semanticUnknown": native_values[4], "semanticLayerMismatch": native_values[5],
            "postSkipFallback": native_values[6], "duplicate": native_values[7],
            "irreversible": native_values[8], "pending": native_values[9],
        },
        "P2": {"backingHit": p2[0], "backingReject": p2[1], "backingFallback": p2[2]},
        "P3": {
            "hit": p3_main[0], "reject": p3_main[1], "submitted": p3_main[2],
            "outlineSubmitted": _match_int(consume, r"\boutline=(\d+)"),
            "outlineSameSlice": outline_slice[0],
            "outlineSliceMismatch": outline_slice[1],
            "restoreArms": p3_restore[0], "restoreRebinds": p3_restore[1],
            "restoreOverlap": p3_restore[2], "restorePending": p3_restore[3],
            "bypassAttempts": bypass[0], "bypassAuthorizations": bypass[1],
            "bypassCommits": bypass[2], "bypassFallbacks": bypass[3],
            "bypassMismatch": _match_int(consume, r"\bmismatch=(\d+)"),
            "bypassStale": _match_int(consume, r"\bstale=(\d+)"),
            "bypassPending": _match_int(consume, r"\bpending=(\d+).*?\brestoreFail"),
            "bypassHostAuthorizationMismatch": _match_named_int(
                consume, "authMismatch"
            ),
            "restoreFail": _match_int(consume, r"\brestoreFail=(\d+)"),
        },
        "P4Shadow": {
            "leasesConsumed": _match_int(consume, r"\bshadow=(\d+)"),
            "preflightIndexReject": p4_shadow[0],
            "preflightUvReject": p4_shadow[1],
            "finalPositionReject": p4_shadow[2],
            "finalIndexReject": p4_shadow[3],
            "finalUvReject": p4_shadow[4],
            "finalCommitReject": p4_shadow[5],
            "bypassCommits": p4_shadow[6],
        },
        "ledger": dict(zip(("classified", "resolved", "consumed", "cpuFallback", "suppressed", "leak", "unreserved", "duplicate", "planMismatch", "retireDeferred"), ledger_values)),
        "ledgerReason": {
            "lookupCaller": dict(zip(("plan", "commit", "fail"), ledger_reason_callers)),
            "lookupCause": dict(zip(("tokenMiss", "keyMismatch", "leaseMismatch", "notSubmitted"), ledger_reason_causes)),
            "other": dict(zip(("settlementInvalid", "reserveMaskInvalid", "reserveMissingKnown", "fuseUnknownConsumer", "terminateUnmatched"), ledger_reason_other)),
        },
        "latePoison": {
            "total": _match_int(late_poison, r"\btotal=(\d+)"),
            "matchedOpen": late_poison_match[0],
            "matchedTerminal": late_poison_match[1],
            "unmatched": late_poison_match[2],
            "correlated": late_poison_correlation[0],
            "uncorrelated": late_poison_correlation[1],
            "uploadEpochZero": _match_int(late_poison, r"\bupload0=(\d+)"),
            "zeroGeometry": _match_int(
                late_poison, r"\bzeroGeometry=(\d+)"
            ),
            "sourceBypass": dict(zip(
                ("matchedOpen", "matchedTerminal", "unmatched"),
                late_poison_source_bypass,
            )),
            "sourcePoisonOnly": dict(zip(
                ("matchedOpen", "matchedTerminal", "unmatched"),
                late_poison_source_poison_only,
            )),
            "exact": late_poison_exact[0],
            "inexact": late_poison_exact[1],
            "flagIncidence": dict(zip(
                ("none", "debugSkip", "autoInstancing", "indexSplit", "recursive", "nonMainPass"),
                late_poison_flags,
            )),
            "flagMask": dict(zip(
                (
                    "none", "debug", "autoInstancing", "indexSplit",
                    "recursive", "debugRecursive", "nonMainPass",
                    "debugNonMainPass", "recursiveNonMainPass",
                    "debugRecursiveNonMainPass", "unknown",
                ),
                late_poison_masks,
            )),
        },
        "latePoisonNative": {
            "total": _match_int(late_poison_native, r"\btotal=(\d+)"),
            "scope": dict(zip(
                ("activeUpload", "outsideDispatch", "dispatchNoUpload",
                 "scopeHazard", "unknown"),
                late_poison_native_scope,
            )),
            "hit": dict(zip(
                ("incompleteIdentity", "layoutMismatch", "inexactRange",
                 "exactRangeOverlap", "unknown"),
                late_poison_native_hit,
            )),
            "sampleCount": late_poison_native_samples[0],
            "sampleOverflow": late_poison_native_samples[1],
            "sampleCapacity": late_poison_native_samples[2],
            "storage": {
                "real": dict(zip(
                    ("same", "different", "mixed", "unknown"),
                    late_poison_storage_real,
                )),
                "mapping": dict(zip(
                    ("same", "different", "mixed", "unknown"),
                    late_poison_storage_mapping,
                )),
                "mapAllocation": dict(zip(
                    ("same", "different", "mixed", "unknown"),
                    late_poison_storage_map,
                )),
                "index": dict(zip(
                    ("same", "different", "mixed", "unknown"),
                    late_poison_storage_index,
                )),
            },
            "storageTotal": _match_int(
                late_poison_storage, r"\btotal=(\d+)"
            ),
            "dump": {
                "stored": _match_named_int(late_poison_dump, "stored"),
                "emitted": _match_named_int(late_poison_dump, "emitted"),
                "truncated": _match_named_int(
                    late_poison_dump, "truncated"
                ),
                "overflow": _match_named_int(late_poison_dump, "overflow"),
            },
            "samples": late_poison_samples,
        },
        "forcedSnapshot": {
            "attempted": False,
            "ok": False,
            "beginSeen": False,
            "endSeen": False,
            "blockOrderValid": False,
        },
        "lifetime": {
            "backpressure": _match_int(lifetime, r"\bbackpressure=(\d+)"),
            "limitViolation": _match_int(lifetime, r"\blimitViolation=(\d+)"),
            "claims": lifetime_claims[0], "claimLimit": lifetime_claims[1],
            "uploadPagesAllocated": lifetime_pages[0], "uploadPagesReclaimed": lifetime_pages[1],
            "outputPagesAllocated": lifetime_pages[2], "outputLeaseRetired": lifetime_pages[3],
            "outputPending": lifetime_pages[4],
        },
        "tracker": {
            "conflicts": tracker_values[0], "tag": tracker_values[1],
            "stage": tracker_values[2], "layer": tracker_values[3],
            "exactTakeoverConflict": exact_conflict,
            "exactTakeoverConflictAvailable": exact_conflict is not None,
            "note": "Global tracker conflicts are reported only. A runtime-provided exact-takeover conflict is the hard gate.",
        },
        "callback": {
            "runtimeFields": _select_status_fields(runtime_data, "callback"),
            "logLines": [line for line in _strip_ansi(text).splitlines() if "callback" in line.lower()][-40:],
        },
        "device": {
            "runtimeFields": _select_status_fields(runtime_data, "device"),
            "resetOrLostLines": [line for line in _strip_ansi(text).splitlines() if re.search(r"device.*(?:reset|lost)|reset.*device", line, re.I)][-40:],
        },
    }


def _quiescence_pair_clean(quiescence: Dict[str, Any]) -> bool:
    pre = dict(quiescence.get("pre", {}) or {})
    post = dict(quiescence.get("post", {}) or {})
    scalar_fields = (
        "ready", "refresh", "managerReady", "resourcesReady",
        "bridgeReady", "deviceReady", "frame", "flush", "mapEpoch",
        "deviceEpoch", "pendingBypass", "pendingBridgeReset", "thread",
        "tls", "retirementFault", "bypassBlocked", "resourceInFlight",
    )
    group_fields = (
        "managerPending", "retired", "resources", "bridgePending",
        "bridgeActive", "telemetry", "resetGeneration", "ingress",
        "devicePending",
    )
    if not all(
        snapshot.get(name) is not None
        for snapshot in (pre, post)
        for name in scalar_fields
    ):
        return False
    if not all(
        isinstance(snapshot.get(group), dict)
        and all(value is not None for value in snapshot[group].values())
        for snapshot in (pre, post)
        for group in group_fields
    ):
        return False
    if not all(
        snapshot[name] == 1
        for snapshot in (pre, post)
        for name in (
            "ready", "refresh", "managerReady", "resourcesReady",
            "bridgeReady", "deviceReady", "thread", "tls",
        )
    ):
        return False
    if not all(
        value == 0
        for snapshot in (pre, post)
        for group in (
            "managerPending", "retired", "bridgePending", "bridgeActive",
            "telemetry", "devicePending",
        )
        for value in snapshot[group].values()
    ):
        return False
    for snapshot in (pre, post):
        resources = snapshot["resources"]
        ingress = snapshot["ingress"]
        reset = snapshot["resetGeneration"]
        if not (
            resources["outputPending"] == 0
            and snapshot["resourceInFlight"] == 0
            and resources["uploadPagesAllocated"] ==
                resources["uploadPagesReclaimed"]
            and snapshot["pendingBypass"] == 0
            and snapshot["pendingBridgeReset"] == 0
            and snapshot["retirementFault"] == 0
            and snapshot["bypassBlocked"] == 0
            and ingress["safety"] == 1
            and ingress["transaction"] == 1
            and ingress["callback"] == 1
            and ingress["dipObserver"] == 1
            and reset["requested"] == reset["completed"] ==
                reset["ownerRetired"]
        ):
            return False
    return bool(
        pre["flush"] == post["flush"]
        and pre["frame"] == post["frame"]
        and pre["mapEpoch"] == post["mapEpoch"]
        and pre["deviceEpoch"] == post["deviceEpoch"]
        and pre["resetGeneration"] == post["resetGeneration"]
    )


def _quiescence_diag_consistent(diag: Dict[str, Any]) -> bool:
    quiescence = diag["quiescence"]
    if not _quiescence_pair_clean(quiescence):
        return False
    protocol = diag["protocol"]
    lifetime = diag["lifetime"]
    poison = diag["nativePoison"]
    poison_shadow = diag.get("nativePoisonShadow", {})
    poison_o1_shadow = diag.get("nativePoisonO1Shadow", {})
    poison_o1_authority = diag.get("nativePoisonO1Authority", {})
    poison_sidecar_policy = diag.get("nativePoisonSidecarPolicy", {})
    poison_sidecar_runtime = poison_sidecar_policy.get(
        "runtimeContract", {},
    )
    production_writer = diag.get("productionSampleTiming", {}).get(
        "writerSnapshot", {}
    )
    values = (
        protocol["flushCallbacks"], lifetime["outputPending"],
        lifetime["uploadPagesAllocated"], lifetime["uploadPagesReclaimed"],
        poison["outstanding"],
        production_writer.get("started"),
        production_writer.get("completed"),
        production_writer.get("active"),
        production_writer.get("pending"),
    )
    if not all(value is not None for value in values):
        return False
    if not (
        poison_sidecar_policy.get("contractClosed") is True
        and poison_sidecar_runtime.get("present") is True
        and poison_sidecar_runtime.get("exact") is True
        and poison_sidecar_runtime.get("authorityZero") is True
        and poison_shadow.get("present") is True
        and poison_shadow.get("active") == 0
        and poison_shadow.get("authorizationAuthority") == 0
    ):
        return False
    if not (
        poison_o1_shadow.get("present") is True
        and poison_o1_shadow.get("active") == 0
        and poison_o1_shadow.get("authority") == 0
        and poison_o1_shadow.get("authorizationAuthority") == 0
    ):
        return False
    if not (
        poison_o1_authority.get("present") is True
        and poison_o1_authority.get("contractClosed") is True
        and poison_o1_authority.get("active") == 0
    ):
        return False
    if not (
        production_writer["started"] == production_writer["completed"]
        and production_writer["active"] == 0
        and production_writer["pending"] == 0
    ):
        return False
    return all(
        snapshot["flush"] == protocol["flushCallbacks"]
        and snapshot["resources"]["outputPending"] ==
            lifetime["outputPending"]
        and snapshot["resources"]["uploadPagesAllocated"] ==
            lifetime["uploadPagesAllocated"]
        and snapshot["resources"]["uploadPagesReclaimed"] ==
            lifetime["uploadPagesReclaimed"]
        and snapshot["bridgePending"]["poisonRanges"] ==
            poison["outstanding"]
        for snapshot in (quiescence["pre"], quiescence["post"])
    )


def _clean_diag_progressed(previous: Dict[str, Any],
                           current: Dict[str, Any]) -> bool:
    prev_q = previous["quiescence"]["post"]
    curr_q = current["quiescence"]["post"]
    same_epoch = (
        prev_q.get("mapEpoch") == curr_q.get("mapEpoch")
        and prev_q.get("deviceEpoch") == curr_q.get("deviceEpoch")
        and prev_q.get("resetGeneration") == curr_q.get("resetGeneration")
    )
    previous_cumulative = (
        previous["protocol"].get("flushCallbacks"),
        previous["compute"]["jobs"][1],
        previous["kernel"].get("bypassedCalls"),
        previous["nativePoison"].get("create"),
        previous["nativePoison"].get("hit"),
        previous["managerDispatch"].get("telemetryFlushes"),
        previous["managerDispatch"].get("telemetryBatchedAdds"),
    )
    current_cumulative = (
        current["protocol"].get("flushCallbacks"),
        current["compute"]["jobs"][1],
        current["kernel"].get("bypassedCalls"),
        current["nativePoison"].get("create"),
        current["nativePoison"].get("hit"),
        current["managerDispatch"].get("telemetryFlushes"),
        current["managerDispatch"].get("telemetryBatchedAdds"),
    )
    cumulative_present = all(
        value is not None
        for value in previous_cumulative + current_cumulative
    )
    cumulative_non_decreasing = cumulative_present and all(
        current_value >= previous_value
        for previous_value, current_value in zip(
            previous_cumulative, current_cumulative
        )
    )
    return bool(
        same_epoch
        and curr_q.get("flush") is not None
        and prev_q.get("flush") is not None
        and curr_q["flush"] > prev_q["flush"]
        and curr_q.get("frame") is not None
        and prev_q.get("frame") is not None
        and curr_q["frame"] > prev_q["frame"]
        and cumulative_non_decreasing
    )


def _clean_pair_across_transients_contract(
    previous: Optional[Tuple[Dict[str, Any], Dict[str, Any]]],
    current: Optional[Tuple[Dict[str, Any], Dict[str, Any]]],
    attempts: List[Dict[str, Any]],
) -> Dict[str, Any]:
    previous_attempt, previous_diag = previous or ({}, {})
    current_attempt, current_diag = current or ({}, {})
    previous_index = previous_attempt.get("index")
    current_index = current_attempt.get("index")
    indices_valid = bool(
        isinstance(previous_index, int) and not isinstance(previous_index, bool)
        and isinstance(current_index, int) and not isinstance(current_index, bool)
        and previous_index > 0 and current_index > previous_index
    )
    previous_id = str(previous_attempt.get("snapshotId", "") or "")
    current_id = str(current_attempt.get("snapshotId", "") or "")
    endpoint_ids_valid = bool(
        previous_id and current_id and previous_id != current_id
    )
    endpoint_evidence_valid = all(
        attempt.get("clean") is True
        and attempt.get("requestIdValid") is True
        and attempt.get("transportExact") is True
        and attempt.get("launchFingerprintExact") is True
        and attempt.get("evidenceSource") == "exact-pid-dbwin"
        and attempt.get("invocationOk") is True
        and bool(str(attempt.get("blockSha256", "") or ""))
        for attempt in (previous_attempt, current_attempt)
    )
    intermediate_attempts = []
    expected_indices: List[int] = []
    if indices_valid:
        expected_indices = list(range(previous_index + 1, current_index))
        intermediate_attempts = sorted(
            (
                attempt for attempt in attempts
                if isinstance(attempt.get("index"), int)
                and not isinstance(attempt.get("index"), bool)
                and previous_index < attempt["index"] < current_index
            ),
            key=lambda attempt: attempt["index"],
        )
    intermediate_indices = [
        attempt["index"] for attempt in intermediate_attempts
    ]
    intermediate_complete = indices_valid and (
        intermediate_indices == expected_indices
    )
    intermediate_all_nonclean = bool(
        intermediate_complete
        and all(attempt.get("clean") is not True
                for attempt in intermediate_attempts)
    )
    progress_valid = bool(
        indices_valid and endpoint_ids_valid and endpoint_evidence_valid
        and intermediate_all_nonclean
        and _clean_diag_progressed(previous_diag, current_diag)
    )
    gap_attempts = current_index - previous_index - 1 if indices_valid else None
    return {
        "valid": bool(
            indices_valid and endpoint_ids_valid and endpoint_evidence_valid
            and intermediate_all_nonclean and progress_valid
        ),
        "chronological": indices_valid,
        "endpointIdsValid": endpoint_ids_valid,
        "endpointEvidenceValid": endpoint_evidence_valid,
        "intermediateComplete": intermediate_complete,
        "intermediateAllNonClean": intermediate_all_nonclean,
        "progressValid": progress_valid,
        "gapAttempts": gap_attempts,
        "endpointAttemptIndices": [previous_index, current_index],
        "intermediateAttemptIndices": intermediate_indices,
    }


def _all_equal_zero(values: Iterable[Optional[int]]) -> bool:
    return all(value == 0 for value in values)


def _dip_fast_probe_policy_contract(
    dip_fast_probe: Dict[str, Any], diagnostic_policy: Dict[str, Any],
) -> bool:
    reader = dip_fast_probe.get("reader", {})
    cover = dip_fast_probe.get("cover", {})
    reader_all_zero = all(
        reader.get(name) == 0 for name in _GPU_SKIN_DIP_FAST_READER_FIELDS
    )
    cover_all_zero = all(
        cover.get(name) == 0 for name in _GPU_SKIN_DIP_FAST_COVER_FIELDS
    )
    full_exact = bool(
        diagnostic_policy.get("fullExact") is True
        and dip_fast_probe.get("allZero") is True
        and reader_all_zero and cover_all_zero
    )
    light_exact = bool(
        diagnostic_policy.get("lightExact") is True
        and dip_fast_probe.get("early", {}).get("attempts", 0) > 0
        and dip_fast_probe.get("late", {}).get("attempts", 0) > 0
        and dip_fast_probe.get("observer", {}).get("closed") is True
        and reader.get("closed") is True
        and reader.get("commits", 0) > 0
        and reader.get("evidenceFallbacks", 0) > 0
        # The current reader has one intentional reject cohort only. Retain
        # the general evidence<=reject parser closure, but require this
        # production-light run to prove that every reject is that cohort.
        and reader.get("rejects") == reader.get("evidenceFallbacks")
        and reader.get("commitCoverExact") is True
        and cover.get("closed") is True
        and cover.get("observer", 0) > 0
        and cover.get("reader", 0) > 0
        and cover.get("readerCommitExact") is True
        and dip_fast_probe.get("early", {}).get(
            "localRejects", {}
        ).get("noTransactionCover") == 0
        and dip_fast_probe.get("late", {}).get(
            "localRejects", {}
        ).get("noTransactionCover") == 0
    )
    return bool(full_exact or light_exact)


def _dip_fast_probe_synthetic_self_tests() -> Dict[str, Any]:
    """Pure dip-fast parser/policy fixtures; never touches War3 or files."""
    def fixture(
        reader: Tuple[int, int, int, int, int, int],
        cover: Tuple[int, int, int],
        *,
        outside_fast: Optional[int] = None,
        populated: bool = True,
        include_reader: bool = True,
    ) -> Dict[str, Any]:
        if outside_fast is None:
            outside_fast = sum(cover)
        if populated:
            early = "3/2 reject=1,0,0,0,0,0,0,0"
            late = "4/3 reject=1,0,0,0,0,0,0,0"
            early_local = "0,1,0,0,0,0,0"
            late_local = "0,1,0,0,0,0,0"
            observer = "5/5/0"
        else:
            early = "0/0 reject=0,0,0,0,0,0,0,0"
            late = "0/0 reject=0,0,0,0,0,0,0,0"
            early_local = "0,0,0,0,0,0,0"
            late_local = "0,0,0,0,0,0,0"
            observer = "0/0/0"
        reader_field = (
            " reader=" + "/".join(str(value) for value in reader)
            if include_reader else ""
        )
        probe_line = (
            "DXVK War3GpuSkin: diag dipFastProbe "
            "period=256 phase=57/215 derived=1 "
            f"early={early} late={late} "
            f"earlyLocal={early_local} lateLocal={late_local} "
            f"observer={observer}{reader_field} "
            f"cover={'/'.join(str(value) for value in cover)}"
        )
        format_line = (
            "DXVK War3GpuSkin: diag format "
            f"nativeDip raw={outside_fast} correlated=0 "
            f"unmatched={outside_fast} outside={outside_fast} "
            f"noUpload=0 fastOutside={outside_fast}"
        )
        return _parse_gpu_skin_diag(
            format_line + "\n" + probe_line, {},
        ).get("dipFastProbe", {})

    full = fixture((0, 0, 0, 0, 0, 0), (0, 0, 0), populated=False)
    light = fixture((8, 8, 6, 2, 2, 0), (3, 5, 6))
    missing_reader = fixture(
        (8, 8, 6, 2, 2, 0), (3, 5, 6), include_reader=False,
    )
    begin_end_mismatch = fixture((8, 7, 6, 2, 2, 0), (3, 5, 6))
    outcome_mismatch = fixture((8, 8, 5, 2, 2, 0), (3, 5, 5))
    evidence_over_reject = fixture((8, 8, 6, 2, 3, 0), (3, 5, 6))
    mismatch_nonzero = fixture((8, 8, 6, 2, 2, 1), (3, 5, 6))
    reader_cover_mismatch = fixture((8, 8, 6, 2, 2, 0), (3, 5, 5))
    fast_partition_mismatch = fixture(
        (8, 8, 6, 2, 2, 0), (3, 5, 6), outside_fast=15,
    )
    no_commit = fixture((2, 2, 0, 2, 2, 0), (3, 5, 0))
    no_evidence = fixture((6, 6, 6, 0, 0, 0), (3, 5, 6))
    conservative_reject = fixture((8, 8, 6, 2, 1, 0), (3, 5, 6))
    full_policy = {"fullExact": True, "lightExact": False}
    light_policy = {"fullExact": False, "lightExact": True}
    checks = {
        "fullParsedAndClosed": bool(
            full.get("present") is True
            and full.get("contractClosed") is True
            and full.get("allZero") is True
        ),
        "fullPolicyAccepted": _dip_fast_probe_policy_contract(
            full, full_policy,
        ),
        "lightParsedAndClosed": bool(
            light.get("present") is True
            and light.get("contractClosed") is True
            and light.get("reader", {}).get("closed") is True
            and light.get("reader", {}).get("commits") == 6
            and light.get("reader", {}).get("rejects") == 2
            and light.get("reader", {}).get("evidenceFallbacks") == 2
            and light.get("cover", {}).get("reader") == 6
        ),
        "lightPolicyAccepted": _dip_fast_probe_policy_contract(
            light, light_policy,
        ),
        "missingReaderRejected": missing_reader.get("present") is False,
        "beginEndMismatchRejected": (
            begin_end_mismatch.get("reader", {}).get("closed") is False
        ),
        "outcomeMismatchRejected": (
            outcome_mismatch.get("reader", {}).get("closed") is False
        ),
        "evidenceOverRejectRejected": (
            evidence_over_reject.get("reader", {}).get("closed") is False
        ),
        "readerMismatchRejected": (
            mismatch_nonzero.get("reader", {}).get("closed") is False
        ),
        "readerCommitCoverMismatchRejected": bool(
            reader_cover_mismatch.get("reader", {}).get(
                "commitCoverExact"
            ) is False
            and reader_cover_mismatch.get("contractClosed") is False
        ),
        "threeWayFastPartitionMismatchRejected": (
            fast_partition_mismatch.get("cover", {}).get("closed") is False
        ),
        "lightZeroCommitRejected": not _dip_fast_probe_policy_contract(
            no_commit, light_policy,
        ),
        "lightZeroEvidenceRejected": not _dip_fast_probe_policy_contract(
            no_evidence, light_policy,
        ),
        "generalEvidenceSubsetStillCloses": bool(
            conservative_reject.get("reader", {}).get("closed") is True
            and conservative_reject.get("contractClosed") is True
        ),
        "lightUnclassifiedRejectRejected": not (
            _dip_fast_probe_policy_contract(conservative_reject, light_policy)
        ),
        "fullNonzeroReaderRejected": not _dip_fast_probe_policy_contract(
            light, full_policy,
        ),
    }
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "dip-fast reader synthetic self-test failed: "
            + json.dumps(result, sort_keys=True)
        )
    return result


def _evaluate_gates(
    diag: Dict[str, Any],
    process_alive: Any,
    screenshots: List[Dict[str, Any]],
    crash_matches: List[str],
    require_outline_all: bool = False,
) -> Dict[str, Any]:
    process_authority = (
        dict(process_alive) if isinstance(process_alive, dict) else {}
    )
    protocol = diag["protocol"]
    manager_dispatch = diag["managerDispatch"]
    dispatch_seal = diag.get("dispatchCpuOnlySeal", {})
    outside_admission_attribution = diag.get(
        "outsideAdmissionAttribution", {}
    )
    compute = diag["compute"]
    format_coverage = diag["formatCoverage"]
    fast_path = diag["productionFastPath"]
    p3 = diag["P3"]
    p4_shadow = diag["P4Shadow"]
    lifetime = diag["lifetime"]
    ledger = diag["ledger"]
    ledger_reason = diag["ledgerReason"]
    late_poison = diag["latePoison"]
    late_poison_native = diag["latePoisonNative"]
    diagnostic_policy = diag["diagnosticPolicy"]
    dip_fast_probe = diag.get("dipFastProbe", {})
    production_sample_timing = diag.get("productionSampleTiming", {})
    dip_fast_probe_phases = dip_fast_probe.get("phases", {})
    dip_fast_probe_phase_clean = bool(
        isinstance(dip_fast_probe.get("period"), int)
        and isinstance(dip_fast_probe_phases.get("early"), int)
        and isinstance(dip_fast_probe_phases.get("late"), int)
        and isinstance(production_sample_timing.get("phase"), int)
        and dip_fast_probe.get("period") ==
            production_sample_timing.get("period")
        and 0 <= dip_fast_probe_phases["early"] <
            dip_fast_probe["period"]
        and 0 <= dip_fast_probe_phases["late"] <
            dip_fast_probe["period"]
        and dip_fast_probe_phases["early"] == 0x39
        and dip_fast_probe_phases["late"] == 0xD7
        and dip_fast_probe_phases["early"] !=
            dip_fast_probe_phases["late"]
        and dip_fast_probe_phases["early"] !=
            production_sample_timing["phase"]
        and dip_fast_probe_phases["late"] !=
            production_sample_timing["phase"]
    )
    dip_fast_probe_policy_clean = _dip_fast_probe_policy_contract(
        dip_fast_probe, diagnostic_policy,
    )
    timing_contract = diag["hotPathTiming"]["contracts"]
    bloom_timing_contract = diag["hotPathTiming"]["renderableBloomClosure"]
    forced_snapshot = dict(diag.get("forcedSnapshot", {}) or {})
    hard_process_evidence = _hard_process_evidence_contract(
        process_authority,
        bool(
            diag.get("crashEvidenceAuthorityExact") is True
            and forced_snapshot.get(
                "collectionLaunchFingerprintExact"
            ) is True
            and forced_snapshot.get("failureClassificationAuthority") == 1
        ),
        crash_matches,
    )
    process_alive_exact = hard_process_evidence["processAlive"]
    crash_evidence_authority = hard_process_evidence[
        "evidenceAuthorityExact"
    ]
    poison = diag["nativePoison"]
    poison_sidecar_policy = diag.get("nativePoisonSidecarPolicy", {})
    poison_sidecar_runtime = poison_sidecar_policy.get(
        "runtimeContract", {},
    )
    poison_shadow = diag.get("nativePoisonShadow", {})
    poison_o1_shadow = diag.get("nativePoisonO1Shadow", {})
    poison_o1_authority = diag.get("nativePoisonO1Authority", {})
    direct_discard = diag["nativeDirectDiscard"]
    direct_discard_present = all(
        value is not None for value in direct_discard.values()
    )
    retirement = diag["nativeRetirement"]
    reset = diag["nativeReset"]
    ticket = diag["indexTicket"]
    native = diag["nativeSafety"]
    kernel = diag["kernel"]
    native_upload = diag["nativeUpload"]
    kernel_normal = diag["nativeKernelNormal"]
    tracker = diag["tracker"]
    required_present = all(
        value is not None
        for value in (
            protocol["flushCallbacks"], protocol["dispatchBegin"],
            protocol["dispatchEnd"], protocol["truePairErr"],
            protocol["epochLeak"], *protocol["pending"],
            manager_dispatch["physicalScopes"],
            manager_dispatch["physicalEnds"],
            manager_dispatch["commonScopes"],
            manager_dispatch["specialScopes"],
            manager_dispatch["semanticScopes"],
            manager_dispatch["eagerScopes"],
            manager_dispatch["lazyScopes"],
            manager_dispatch["neverScopes"],
            manager_dispatch["evidenceEagerScopes"],
            manager_dispatch["beginCallbacks"],
            manager_dispatch["endCallbacks"],
            manager_dispatch["eagerBegins"],
            manager_dispatch["lazyAdmissionAttempts"],
            manager_dispatch["lazyAdmissions"],
            manager_dispatch["eagerAdmissionFailures"],
            manager_dispatch["lazyAdmissionFailures"],
            manager_dispatch["neverSafetyFailures"],
            manager_dispatch["issuedEnds"],
            manager_dispatch["noUploadEnds"],
            manager_dispatch["neverEnds"],
            manager_dispatch["failedEnds"],
            manager_dispatch["skippedUploads"],
            manager_dispatch["skippedDips"],
            manager_dispatch["skippedFanouts"],
            manager_dispatch["telemetryFlushes"],
            manager_dispatch["telemetryBatchedAdds"],
            manager_dispatch["telemetryDeltaPending"],
            manager_dispatch["telemetryDeltaFaulted"],
            manager_dispatch["rawDips"],
            manager_dispatch["correlatedDips"],
            manager_dispatch["unmatchedDips"],
            manager_dispatch["outsideDips"],
            manager_dispatch["noUploadDips"],
            manager_dispatch["outsideDipFastPath"],
            manager_dispatch["observerBegins"],
            manager_dispatch["observerEnds"],
            manager_dispatch["observerMismatches"],
            manager_dispatch["readerBegins"],
            manager_dispatch["readerEnds"],
            manager_dispatch["readerCommits"],
            manager_dispatch["readerRejects"],
            manager_dispatch["readerEvidenceFallbacks"],
            manager_dispatch["readerMismatches"],
            manager_dispatch["fastByFlush"],
            manager_dispatch["fastByObserver"],
            manager_dispatch["fastByReader"],
            manager_dispatch["outsideAdmissionAttemptTotal"],
            manager_dispatch["outsideAdmissionCancellations"],
            manager_dispatch["outsideAdmissionLifecycleExcluded"],
            manager_dispatch["outsideAdmissionTrackedResolvedInside"],
            manager_dispatch["outsideAdmissionUntrackedResolvedOutside"],
            dispatch_seal["viewPublishes"],
            dispatch_seal["viewQueries"],
            dispatch_seal["authorityRejects"],
            dispatch_seal["candidateRejects"],
            dispatch_seal["managerProposals"],
            dispatch_seal["proposals"],
            dispatch_seal["proposalAccepted"],
            dispatch_seal["proposalRejected"],
            dispatch_seal["proposalAborted"],
            dispatch_seal["scopeCommits"],
            dispatch_seal["scopeEnds"],
            dispatch_seal["invalidations"],
            dispatch_seal["uploadsStarted"],
            dispatch_seal["uploadsCompleted"],
            dispatch_seal["vertices"], dispatch_seal["bytes"],
            dispatch_seal["kernelCalls"],
            dispatch_seal["kernelNormalReturns"],
            dispatch_seal["dips"], dispatch_seal["dipsWithUpload"],
            dispatch_seal["dipsNoUpload"],
            dispatch_seal["fanoutZero"], dispatch_seal["fanoutOne"],
            dispatch_seal["fanoutMany"],
            dispatch_seal["fanoutDipTotal"],
            dispatch_seal["markerConflicts"],
            *dispatch_seal["reportedClosure"].values(),
            diagnostic_policy["full"],
            diagnostic_policy["preflightDetail"],
            diagnostic_policy["periodFrames"],
            *compute["batch"], *compute["jobs"], *compute["dispatch"],
            *compute["palette"],
            *format_coverage["formatBuckets"].values(),
            *format_coverage["skinBuckets"].values(),
            format_coverage["nativeUploadRaw"],
            format_coverage["nativeUploadOutside"],
            *format_coverage["nativeFanout"].values(),
            format_coverage["eligible"],
            *format_coverage["strictReject"].values(),
            fast_path["rejectScope"], fast_path["rejectState"],
            fast_path["rejectSkinFormat"], fast_path["rejectSmall"],
            fast_path["rejectInput"], fast_path["candidates"],
            fast_path["dispatchSealUploads"],
            fast_path["unknownState"],
            fast_path["kernelBatches"],
            fast_path["poisonScanAttempts"],
            fast_path["poisonNoOverlap"],
            fast_path["poisonOverlap"],
            fast_path["poisonReadFail"],
            fast_path["markerConflicts"],
            fast_path["directOriginalAttempts"],
            fast_path["directOriginalKernelCalls"],
            fast_path["directOriginalNormalReturns"],
            fast_path["directOriginalKernelNoNormalReturns"],
            fast_path["directOriginalCompleted"],
            fast_path["directOriginalConflicts"],
            fast_path["directOriginalCancellations"],
            fast_path["directOriginalActive"],
            fast_path["directOriginalResetCompletedWhileActive"],
            fast_path["directOriginalLatePoison"],
            kernel["hookCalls"], kernel["originalCalls"],
            kernel["bypassedCalls"], poison["create"], poison["hit"],
            native_upload["originalCalls"],
            native_upload["bypassedCalls"],
            poison["clear"], poison["overflow"], poison["outstanding"],
            *direct_discard.values(),
            kernel["originalBytes"], kernel["bypassedBytes"],
            ticket["mask"], ticket["attempts"], ticket["exact"], ticket["suppressed"], ticket["leaks"],
            native["postSkipFallback"], native["duplicate"], native["irreversible"], native["pending"],
            p3["restoreArms"], p3["restoreRebinds"], p3["restoreOverlap"], p3["restorePending"], p3["restoreFail"],
            p3["outlineSubmitted"], p3["outlineSameSlice"],
            p3["outlineSliceMismatch"],
            p3["bypassCommits"], p3["bypassMismatch"],
            p3["bypassHostAuthorizationMismatch"],
            p4_shadow["leasesConsumed"], p4_shadow["bypassCommits"],
            p4_shadow["finalPositionReject"], p4_shadow["finalIndexReject"],
            p4_shadow["finalUvReject"], p4_shadow["finalCommitReject"],
            ledger["resolved"], ledger["consumed"], ledger["cpuFallback"], ledger["suppressed"],
            ledger["leak"], ledger["unreserved"], ledger["duplicate"], ledger["planMismatch"], ledger["retireDeferred"],
            *ledger_reason["lookupCaller"].values(),
            *ledger_reason["lookupCause"].values(),
            *ledger_reason["other"].values(),
            late_poison["total"], late_poison["matchedOpen"],
            late_poison["matchedTerminal"], late_poison["unmatched"],
            late_poison["correlated"], late_poison["uncorrelated"],
            late_poison["exact"], late_poison["inexact"],
            late_poison["uploadEpochZero"], late_poison["zeroGeometry"],
            *late_poison["sourceBypass"].values(),
            *late_poison["sourcePoisonOnly"].values(),
            *late_poison["flagIncidence"].values(),
            *late_poison["flagMask"].values(),
            late_poison_native["total"],
            *late_poison_native["scope"].values(),
            *late_poison_native["hit"].values(),
            late_poison_native["sampleCount"],
            late_poison_native["sampleOverflow"],
            late_poison_native["sampleCapacity"],
            late_poison_native["storageTotal"],
            *late_poison_native["storage"]["real"].values(),
            *late_poison_native["storage"]["mapping"].values(),
            *late_poison_native["storage"]["mapAllocation"].values(),
            *late_poison_native["storage"]["index"].values(),
            *late_poison_native["dump"].values(),
            lifetime["limitViolation"], lifetime["outputLeaseRetired"],
            lifetime["outputPending"], lifetime["uploadPagesAllocated"],
            lifetime["uploadPagesReclaimed"],
            kernel_normal["overflow"], kernel_normal["invalid"], kernel_normal["fault"],
            kernel_normal["ackMismatch"], kernel_normal["commitBlocked"],
            retirement["overflow"], retirement["invalid"], retirement["pending"], retirement["fault"],
            reset["wrongThread"], reset["retirementFault"], reset["pendingRetirement"],
        )
    )
    required_present = bool(
        required_present and diag["productionSampleTiming"].get("present")
        and poison_shadow.get("present") is True
    )
    ticket_mask = ticket["mask"] if isinstance(ticket["mask"], int) else None
    p3_restore_clean = (
        p3["restoreArms"] == p3["restoreRebinds"]
        and _all_equal_zero((p3["restoreOverlap"], p3["restorePending"], p3["restoreFail"], p3["bypassPending"]))
    )
    lifetime_clean = (
        _all_equal_zero((lifetime["limitViolation"], lifetime["outputPending"]))
        and lifetime["uploadPagesAllocated"] == lifetime["uploadPagesReclaimed"]
    )
    exact_conflict = tracker["exactTakeoverConflict"]
    lookup_callers = tuple(ledger_reason["lookupCaller"].values())
    lookup_causes = tuple(ledger_reason["lookupCause"].values())
    ledger_other = tuple(ledger_reason["other"].values())
    ledger_reason_consistent = (
        all(value is not None for value in lookup_callers + lookup_causes + ledger_other)
        and sum(lookup_callers) == sum(lookup_causes)
        and ledger["unreserved"] == sum(lookup_causes) + sum(ledger_other)
    )
    late_poison_partition_values = (
        late_poison["total"], late_poison["matchedOpen"],
        late_poison["matchedTerminal"], late_poison["unmatched"],
        late_poison["correlated"], late_poison["uncorrelated"],
        late_poison["exact"], late_poison["inexact"],
        late_poison["uploadEpochZero"], late_poison["zeroGeometry"],
        *late_poison["sourceBypass"].values(),
        *late_poison["sourcePoisonOnly"].values(),
        *late_poison["flagIncidence"].values(),
        *late_poison["flagMask"].values(),
    )
    late_poison_partitions_present = all(
        value is not None for value in late_poison_partition_values
    )
    late_poison_source_total = (
        sum(late_poison["sourceBypass"].values())
        + sum(late_poison["sourcePoisonOnly"].values())
        if late_poison_partitions_present else None
    )
    late_poison_partitions_closed = (
        late_poison_partitions_present
        and late_poison["total"] == (
            late_poison["matchedOpen"]
            + late_poison["matchedTerminal"]
            + late_poison["unmatched"]
        )
        and late_poison["total"] == (
            late_poison["correlated"] + late_poison["uncorrelated"]
        )
        and late_poison["total"] == (
            late_poison["exact"] + late_poison["inexact"]
        )
        and late_poison["uploadEpochZero"] <= late_poison["total"]
        and late_poison["zeroGeometry"] <= late_poison["total"]
        and late_poison["total"] == late_poison_source_total
        and late_poison["matchedOpen"] == (
            late_poison["sourceBypass"]["matchedOpen"]
            + late_poison["sourcePoisonOnly"]["matchedOpen"]
        )
        and late_poison["matchedTerminal"] == (
            late_poison["sourceBypass"]["matchedTerminal"]
            + late_poison["sourcePoisonOnly"]["matchedTerminal"]
        )
        and late_poison["unmatched"] == (
            late_poison["sourceBypass"]["unmatched"]
            + late_poison["sourcePoisonOnly"]["unmatched"]
        )
        and late_poison["total"] == sum(late_poison["flagMask"].values())
    )
    late_poison_flag_incidence_consistent = (
        late_poison_partitions_present
        and late_poison["flagIncidence"]["none"] ==
            late_poison["flagMask"]["none"]
        and late_poison["flagIncidence"]["debugSkip"] == (
            late_poison["flagMask"]["debug"]
            + late_poison["flagMask"]["debugRecursive"]
            + late_poison["flagMask"]["debugNonMainPass"]
            + late_poison["flagMask"]["debugRecursiveNonMainPass"]
        )
        and late_poison["flagIncidence"]["autoInstancing"] ==
            late_poison["flagMask"]["autoInstancing"]
        and late_poison["flagIncidence"]["indexSplit"] ==
            late_poison["flagMask"]["indexSplit"]
        and late_poison["flagIncidence"]["recursive"] == (
            late_poison["flagMask"]["recursive"]
            + late_poison["flagMask"]["debugRecursive"]
            + late_poison["flagMask"]["recursiveNonMainPass"]
            + late_poison["flagMask"]["debugRecursiveNonMainPass"]
        )
        and late_poison["flagIncidence"]["nonMainPass"] == (
            late_poison["flagMask"]["nonMainPass"]
            + late_poison["flagMask"]["debugNonMainPass"]
            + late_poison["flagMask"]["recursiveNonMainPass"]
            + late_poison["flagMask"]["debugRecursiveNonMainPass"]
        )
    )

    native_scope_values = tuple(late_poison_native["scope"].values())
    native_hit_values = tuple(late_poison_native["hit"].values())
    native_storage_groups = tuple(
        tuple(late_poison_native["storage"][name].values())
        for name in ("real", "mapping", "mapAllocation", "index")
    )
    native_partition_values = (
        late_poison_native["total"], late_poison_native["storageTotal"],
        late_poison_native["sampleCount"],
        late_poison_native["sampleOverflow"],
        late_poison_native["sampleCapacity"],
        *native_scope_values, *native_hit_values,
        *(value for group in native_storage_groups for value in group),
    )
    native_partitions_present = all(
        value is not None for value in native_partition_values
    )
    manager_poison_only_total = (
        sum(late_poison["sourcePoisonOnly"].values())
        if late_poison_partitions_present else None
    )
    late_poison_native_partitions_closed = (
        native_partitions_present
        and late_poison_native["total"] == manager_poison_only_total
        and late_poison_native["total"] == late_poison_native["storageTotal"]
        and late_poison_native["total"] == sum(native_scope_values)
        and late_poison_native["total"] == sum(native_hit_values)
        and late_poison_native["scope"]["unknown"] == 0
        and late_poison_native["hit"]["unknown"] == 0
        and all(
            late_poison_native["total"] == sum(group)
            for group in native_storage_groups
        )
        and all(
            late_poison_native["storage"][name]["unknown"] == 0
            for name in ("real", "mapping", "mapAllocation", "index")
        )
    )

    native_samples = late_poison_native["samples"]
    dump = late_poison_native["dump"]
    sample_count = late_poison_native["sampleCount"]
    sample_overflow = late_poison_native["sampleOverflow"]
    sample_capacity = late_poison_native["sampleCapacity"]
    late_poison_sample_dump_closed = (
        native_partitions_present
        and all(value is not None for value in dump.values())
        and sample_capacity == 32
        and sample_overflow == 0
        and sample_count == late_poison_native["total"]
        and sample_count + sample_overflow == late_poison_native["total"]
        and dump["stored"] == sample_count
        and dump["emitted"] == sample_count
        and dump["truncated"] == 0
        and dump["overflow"] == sample_overflow
        and native_samples["lineCount"] == sample_count
        and native_samples["uniqueIds"] == sample_count
        and native_samples["duplicateIds"] == 0
        and {item["id"] for item in native_samples["items"]} ==
            set(range(1, (sample_count or 0) + 1))
    )

    def _sample_values_present(sample: Dict[str, Any]) -> bool:
        groups = (
            sample["currentEpoch"], sample["poisonEpoch"],
            sample["realStorage"], sample["mappingStorage"],
            sample["mapAllocation"], sample["mapMode"], sample["lock"],
            sample["mixed"], sample["vertex"], sample["poisonVertex"],
            sample["index"],
        )
        return all(
            sample.get(name) is not None
            for name in (
                "id", "scope", "path", "hit", "stage", "batch",
                "poisonFuseKey",
            )
        ) and all(
            value is not None for group in groups for value in group.values()
        )

    sample_geometry_consistent = late_poison_sample_dump_closed
    scope_counts = {name: 0 for name in late_poison_native["scope"]}
    hit_counts = {name: 0 for name in late_poison_native["hit"]}
    scope_names = {
        1: "activeUpload", 2: "outsideDispatch",
        3: "dispatchNoUpload", 4: "scopeHazard",
    }
    hit_names = {
        1: "incompleteIdentity", 2: "layoutMismatch",
        3: "inexactRange", 4: "exactRangeOverlap",
    }
    for sample in native_samples["items"]:
        if not _sample_values_present(sample):
            sample_geometry_consistent = False
            continue
        scope_name = scope_names.get(sample["scope"], "unknown")
        hit_name = hit_names.get(sample["hit"], "unknown")
        scope_counts[scope_name] += 1
        hit_counts[hit_name] += 1
        current_epoch = sample["currentEpoch"]
        if sample["scope"] == 2 and (
            current_epoch["dispatch"] != 0 or current_epoch["upload"] != 0
        ):
            sample_geometry_consistent = False
        if sample["scope"] == 3 and (
            current_epoch["dispatch"] == 0 or current_epoch["upload"] != 0
        ):
            sample_geometry_consistent = False
        if sample["scope"] == 1 and current_epoch["upload"] == 0:
            sample_geometry_consistent = False
        if sample["scope"] == 4 and current_epoch["upload"] == 0:
            sample_geometry_consistent = False
        if sample["poisonFuseKey"] == 0:
            sample_geometry_consistent = False
        vertex = sample["vertex"]
        poison_vertex = sample["poisonVertex"]
        begin = vertex["base"] + vertex["min"]
        end = begin + vertex["count"]
        range_exact = (
            begin >= 0 and vertex["count"] != 0 and end <= (1 << 32)
        )
        if sample["hit"] == 3 and range_exact:
            sample_geometry_consistent = False
        if sample["hit"] == 4:
            poison_begin = poison_vertex["base"]
            poison_end = poison_begin + poison_vertex["count"]
            if (not range_exact or poison_vertex["count"] == 0 or
                    end <= poison_begin or begin >= poison_end):
                sample_geometry_consistent = False
    sample_geometry_consistent = (
        sample_geometry_consistent
        and scope_counts == late_poison_native["scope"]
        and hit_counts == late_poison_native["hit"]
    )

    def _generation_relation(
        current: int, poison_value: int, mixed: int
    ) -> str:
        if mixed:
            return "mixed"
        if current == 0 or poison_value == 0:
            return "unknown"
        return "same" if current == poison_value else "different"

    sample_storage_consistent = late_poison_sample_dump_closed
    derived_storage = {
        name: {relation: 0 for relation in ("same", "different", "mixed", "unknown")}
        for name in ("real", "mapping", "mapAllocation", "index")
    }
    for sample in native_samples["items"]:
        if not _sample_values_present(sample):
            sample_storage_consistent = False
            continue
        if (any(value not in (0, 1) for value in sample["mixed"].values()) or
                sample["lock"]["currentActive"] not in (0, 1) or
                sample["lock"]["poisonActive"] not in (0, 1) or
                sample["mapMode"]["current"] not in (0, 1) or
                sample["mapMode"]["poison"] not in (0, 1)):
            sample_storage_consistent = False
        for name, source in (
            ("real", sample["realStorage"]),
            ("mapping", sample["mappingStorage"]),
            ("mapAllocation", sample["mapAllocation"]),
        ):
            relation = _generation_relation(
                source["current"], source["poison"],
                sample["mixed"][name] or sample["mixed"]["mapMode"],
            )
            derived_storage[name][relation] += 1
        index = sample["index"]
        if sample["mixed"]["index"]:
            index_relation = "mixed"
        elif index["originCount"] == 0 or index["originCount"] % 3:
            index_relation = "unknown"
        elif (
            index["primitiveType"] == 4
            and sample["vertex"]["min"] == 0
            and index["currentStart"] == index["originStart"]
            and index["currentPrimitiveCount"] == index["originCount"] // 3
        ):
            index_relation = "same"
        else:
            index_relation = "different"
        derived_storage["index"][index_relation] += 1
    sample_storage_consistent = (
        sample_storage_consistent
        and all(
            derived_storage[name] == late_poison_native["storage"][name]
            for name in derived_storage
        )
    )
    protocol_values = (
        protocol["dispatchBegin"], protocol["dispatchEnd"],
        protocol["truePairErr"], protocol["epochLeak"], *protocol["pending"],
    )
    compute_values = (
        *compute["batch"], *compute["jobs"], *compute["dispatch"],
        *compute["palette"],
    )
    protocol_accounting_closed = (
        all(value is not None for value in protocol_values)
        and protocol["dispatchBegin"] == protocol["dispatchEnd"]
        and protocol["truePairErr"] == 0
        and protocol["epochLeak"] == 0
        and all(value == 0 for value in protocol["pending"])
        and fast_path.get("partitionClosed") is True
        and forced_snapshot.get(
            "cleanPairProductionFastDelta", {}
        ).get("valid") is True
    )
    manager_dispatch_values = tuple(
        manager_dispatch[name]
        for name in (
            "physicalScopes", "physicalEnds", "eagerScopes",
            "commonScopes", "specialScopes", "semanticScopes",
            "lazyScopes", "neverScopes", "nativeCpuOnlyScopes",
            "nativeCpuOnlyEnds", "evidenceEagerScopes",
            "beginCallbacks", "endCallbacks", "eagerBegins",
            "lazyAdmissionAttempts", "lazyAdmissions",
            "eagerAdmissionFailures", "lazyAdmissionFailures",
            "neverSafetyFailures", "issuedEnds", "noUploadEnds",
            "neverEnds", "failedEnds", "skippedUploads",
            "skippedDips", "skippedFanouts",
            "rawDips", "outsideDips", "noUploadDips",
            "correlatedDips", "unmatchedDips", "outsideDipFastPath",
            "observerBegins", "observerEnds", "observerMismatches",
            "readerBegins", "readerEnds", "readerCommits",
            "readerRejects", "readerEvidenceFallbacks", "readerMismatches",
            "fastByFlush", "fastByObserver", "fastByReader",
            "telemetryFlushes", "telemetryBatchedAdds",
            "telemetryDeltaPending", "telemetryDeltaFaulted",
        )
    )
    manager_dispatch_present = all(
        isinstance(value, int) for value in manager_dispatch_values
    )
    manager_dispatch_accounting_closed = bool(
        manager_dispatch_present
        and manager_dispatch["physicalScopes"] ==
            manager_dispatch["physicalEnds"]
        and manager_dispatch["physicalScopes"] == (
            manager_dispatch["eagerScopes"]
            + manager_dispatch["lazyScopes"]
            + manager_dispatch["neverScopes"]
            + manager_dispatch["nativeCpuOnlyScopes"]
        )
        and manager_dispatch["physicalEnds"] == (
            manager_dispatch["issuedEnds"]
            + manager_dispatch["noUploadEnds"]
            + manager_dispatch["neverEnds"]
            + manager_dispatch["nativeCpuOnlyEnds"]
            + manager_dispatch["failedEnds"]
        )
        and manager_dispatch["nativeCpuOnlyScopes"] ==
            manager_dispatch["nativeCpuOnlyEnds"]
        and manager_dispatch["beginCallbacks"] == (
            manager_dispatch["eagerBegins"]
            + manager_dispatch["lazyAdmissions"]
        )
        and manager_dispatch["lazyAdmissionAttempts"] == (
            manager_dispatch["lazyAdmissions"]
            + manager_dispatch["lazyAdmissionFailures"]
        )
        and manager_dispatch["endCallbacks"] ==
            manager_dispatch["issuedEnds"]
        and manager_dispatch["beginCallbacks"] ==
            manager_dispatch["endCallbacks"]
        and manager_dispatch["evidenceEagerScopes"] <=
            manager_dispatch["eagerScopes"]
        and manager_dispatch["skippedUploads"] ==
            manager_dispatch["skippedFanouts"]
        and manager_dispatch["rawDips"] == (
            manager_dispatch["correlatedDips"]
            + manager_dispatch["unmatchedDips"]
        )
        and manager_dispatch["outsideDips"] +
            manager_dispatch["noUploadDips"] <=
            manager_dispatch["unmatchedDips"]
        and manager_dispatch["outsideDipFastPath"] <=
            manager_dispatch["outsideDips"]
        and manager_dispatch["outsideDipFastPath"] == (
            manager_dispatch["fastByFlush"]
            + manager_dispatch["fastByObserver"]
            + manager_dispatch["fastByReader"]
        )
        and manager_dispatch["observerBegins"] ==
            manager_dispatch["observerEnds"]
        and manager_dispatch["observerMismatches"] == 0
        and manager_dispatch["readerBegins"] ==
            manager_dispatch["readerEnds"]
        and manager_dispatch["readerBegins"] == (
            manager_dispatch["readerCommits"]
            + manager_dispatch["readerRejects"]
        )
        and manager_dispatch["readerEvidenceFallbacks"] <=
            manager_dispatch["readerRejects"]
        and manager_dispatch["readerMismatches"] == 0
        and manager_dispatch["readerCommits"] ==
            manager_dispatch["fastByReader"]
        and manager_dispatch["telemetryDeltaPending"] == 0
        and manager_dispatch["telemetryDeltaFaulted"] == 0
        and _all_equal_zero((
            manager_dispatch["eagerAdmissionFailures"],
            manager_dispatch["lazyAdmissionFailures"],
            manager_dispatch["neverSafetyFailures"],
            manager_dispatch["failedEnds"],
        ))
    )
    manager_dispatch_policy_clean = bool(
        manager_dispatch_accounting_closed
        and forced_snapshot.get(
            "cleanPairTelemetryDelta", {}
        ).get("exact") is True
        and (
            diagnostic_policy.get("fullExact") is True
            and manager_dispatch["eagerScopes"] ==
                manager_dispatch["physicalScopes"]
            and _all_equal_zero((
                manager_dispatch["lazyScopes"],
                manager_dispatch["neverScopes"],
                manager_dispatch["nativeCpuOnlyScopes"],
                manager_dispatch["nativeCpuOnlyEnds"],
                manager_dispatch["evidenceEagerScopes"],
                manager_dispatch["lazyAdmissionAttempts"],
                manager_dispatch["lazyAdmissions"],
                manager_dispatch["noUploadEnds"],
                manager_dispatch["neverEnds"],
                manager_dispatch["skippedUploads"],
                manager_dispatch["skippedDips"],
                manager_dispatch["skippedFanouts"],
                manager_dispatch["readerBegins"],
                manager_dispatch["readerEnds"],
                manager_dispatch["readerCommits"],
                manager_dispatch["readerRejects"],
                manager_dispatch["readerEvidenceFallbacks"],
                manager_dispatch["readerMismatches"],
                manager_dispatch["fastByReader"],
                manager_dispatch["telemetryFlushes"],
                manager_dispatch["telemetryBatchedAdds"],
            ))
            or diagnostic_policy.get("lightExact") is True
            and _manager_dispatch_light_partition_policy(manager_dispatch)
            and manager_dispatch["eagerBegins"] ==
                manager_dispatch["eagerScopes"]
            and manager_dispatch["neverEnds"] ==
                manager_dispatch["neverScopes"]
            and manager_dispatch["nativeCpuOnlyEnds"] ==
                manager_dispatch["nativeCpuOnlyScopes"]
            and manager_dispatch["nativeCpuOnlyScopes"] > 0
            and manager_dispatch["skippedUploads"] > 0
            and manager_dispatch["telemetryFlushes"] > 0
            and _all_equal_zero((
                manager_dispatch["lazyScopes"],
                manager_dispatch["lazyAdmissionAttempts"],
                manager_dispatch["lazyAdmissions"],
                manager_dispatch["noUploadEnds"],
            ))
            and isinstance(
                fast_path.get("outsideNativeFastPath"), int
            )
            and manager_dispatch["telemetryBatchedAdds"] >=
                13 * fast_path["outsideNativeFastPath"]
        )
    )
    dispatch_seal_counter_names = _DISPATCH_CPU_ONLY_SEAL_COUNTER_NAMES
    dispatch_seal_accounting_closed = (
        _dispatch_cpu_only_seal_accounting_contract(
            dispatch_seal, fast_path.get("dispatchSealUploads"),
        )
    )
    dispatch_seal_timing = production_sample_timing.get(
        "dispatchSealStages", {}
    )
    dispatch_seal_timing_records = tuple(
        dispatch_seal_timing.get(name, {})
        for name in ("admission", "inclusive", "body", "complete", "cancel")
    )
    dispatch_seal_timing_present = all(
        record.get("present") is True and
        record.get("shapeValid") is True and
        record.get("frequencyValid") is True
        for record in dispatch_seal_timing_records
    )
    dispatch_seal_timing_all_zero = bool(
        dispatch_seal_timing_present
        and all(
            record.get("calls") == 0 and record.get("ticks") == 0
            and record.get("maxTicks") == 0
            for record in dispatch_seal_timing_records
        )
    )
    dispatch_seal_timing_positive = bool(
        dispatch_seal_timing_present
        and dispatch_seal_timing["admission"].get("calls", 0) > 0
        and dispatch_seal_timing["admission"].get("calls") ==
            dispatch_seal_timing["inclusive"].get("calls") ==
            dispatch_seal_timing["body"].get("calls") ==
            dispatch_seal_timing["complete"].get("calls")
        and dispatch_seal_timing["cancel"].get("calls") == 0
        and dispatch_seal_timing["cancel"].get("ticks") == 0
        and all(
            value == 1 for value in production_sample_timing.get(
                "dispatchSealReportedClosure", {}
            ).values()
        )
    )
    pair_dispatch_seal_timing = forced_snapshot.get(
        "cleanPairProductionSampleTiming", {}
    ).get("dispatchSeal", {})
    dispatch_seal_policy_clean = bool(
        dispatch_seal_accounting_closed
        and (
            diagnostic_policy.get("fullExact") is True
            and all(
                dispatch_seal[name] == 0
                for name in dispatch_seal_counter_names
            )
            and dispatch_seal_timing_all_zero
            or diagnostic_policy.get("lightExact") is True
            and dispatch_seal["viewPublishes"] > 0
            and dispatch_seal["viewQueries"] > 0
            and dispatch_seal["localViewPublishAttempts"] > 0
            and dispatch_seal["localViewPublishes"] > 0
            and dispatch_seal["localViewQueries"] > 0
            and dispatch_seal["localViewCommits"] > 0
            and dispatch_seal["nativeCpuOnlyScopes"] > 0
            and dispatch_seal["scopeCommits"] > 0
            and dispatch_seal["uploadsCompleted"] > 0
            and dispatch_seal["vertices"] > 0
            and dispatch_seal["bytes"] > 0
            and dispatch_seal["dips"] > 0
            # Rejected/aborted proposals and a committed seal invalidated before
            # the next independent event are normal fail-closed fallbacks under
            # transient poison, reset, retirement, or generic-path discovery.
            # An active upload marker conflict is the irreversible boundary.
            and dispatch_seal["markerConflicts"] == 0
            and dispatch_seal_timing_positive
            and pair_dispatch_seal_timing.get("calls", 0) > 0
        )
    )
    compute_accounting_closed = (
        all(value is not None for value in compute_values)
        and compute["batch"][0] == compute["batch"][1] + compute["batch"][2]
        and compute["jobs"][0] == compute["jobs"][1]
        and compute["dispatch"][0] == compute["dispatch"][1]
        and compute["palette"][0] == compute["palette"][1]
    )
    kernel_accounting_closed = (
        all(value is not None for value in (
            kernel["hookCalls"], kernel["originalCalls"],
            kernel["bypassedCalls"],
        ))
        and kernel["hookCalls"] == kernel["originalCalls"] +
            kernel["bypassedCalls"]
    )
    format_values = tuple(format_coverage["formatBuckets"].values())
    skin_values = tuple(format_coverage["skinBuckets"].values())
    strict_reject_values = tuple(format_coverage["strictReject"].values())
    raw_uploads = format_coverage["nativeUploadRaw"]
    outside_uploads = format_coverage["nativeUploadOutside"]
    eligible_uploads = format_coverage["eligible"]
    format_values_present = all(value is not None for value in format_values)
    skin_values_present = all(value is not None for value in skin_values)
    strict_values_present = all(
        value is not None for value in strict_reject_values
    )
    inside_uploads = (
        raw_uploads - outside_uploads
        if raw_uploads is not None and outside_uploads is not None
        else None
    )
    strict_reject_total = (
        sum(strict_reject_values) if strict_values_present else None
    )
    native_inside_range_valid = bool(
        raw_uploads is not None
        and outside_uploads is not None
        and 0 <= outside_uploads <= raw_uploads
    )
    format_histogram_closed = bool(
        format_values_present
        and raw_uploads is not None
        and sum(format_values) == raw_uploads
    )
    skin_histogram_closed = bool(
        skin_values_present
        and raw_uploads is not None
        and sum(skin_values) == raw_uploads
    )
    format_buckets_known = bool(
        format_values_present
        and skin_values_present
        and fast_path["unknownState"] is not None
        and isinstance(fast_path.get("dispatchSealUploads"), int)
        and format_coverage["formatBuckets"]["f6"] == 0
        and format_coverage["skinBuckets"]["s6"] == 0
        and format_coverage["formatBuckets"]["f7"] ==
            fast_path["unknownState"] +
                fast_path["dispatchSealUploads"]
        and format_coverage["skinBuckets"]["s7"] ==
            fast_path["unknownState"] +
                fast_path["dispatchSealUploads"]
    )
    strict_upload_classification_closed = bool(
        native_inside_range_valid
        and eligible_uploads is not None
        and strict_reject_total is not None
        and isinstance(manager_dispatch.get("skippedUploads"), int)
        and isinstance(fast_path.get("dispatchSealUploads"), int)
        and inside_uploads == (
            eligible_uploads + strict_reject_total
            + manager_dispatch["skippedUploads"]
            + fast_path["dispatchSealUploads"]
        )
    )
    outside_native_fast = fast_path.get("outsideNativeFastPath")
    reject_kernel_batches = fast_path.get("kernelBatches")
    poison_scan_attempts = fast_path.get("poisonScanAttempts")
    poison_no_overlap = fast_path.get("poisonNoOverlap")
    poison_overlap = fast_path.get("poisonOverlap")
    poison_read_fail = fast_path.get("poisonReadFail")
    marker_conflicts = fast_path.get("markerConflicts")
    cover_values = (
        fast_path.get("coverFlush"), fast_path.get("coverSemantic"),
        fast_path.get("coverIndependent"),
    )
    independent_pin_begins = fast_path.get("independentPinBegins")
    independent_pin_ends = fast_path.get("independentPinEnds")
    direct_original = {
        name: fast_path.get("directOriginal" + suffix)
        for name, suffix in (
            ("attempts", "Attempts"),
            ("kernelCalls", "KernelCalls"),
            ("normalReturns", "NormalReturns"),
            ("kernelNoNormalReturns", "KernelNoNormalReturns"),
            ("completed", "Completed"),
            ("conflicts", "Conflicts"),
            ("cancellations", "Cancellations"),
            ("active", "Active"),
            ("resetCompletedWhileActive", "ResetCompletedWhileActive"),
            ("latePoison", "LatePoison"),
        )
    }
    direct_original_present = all(
        isinstance(value, int) for value in direct_original.values()
    )
    direct_original_contract_clean = bool(
        direct_original_present
        and isinstance(outside_native_fast, int)
        and isinstance(
            outside_admission_attribution.get("accepted", {}).get(
                "noPoison"
            ), int
        )
        and direct_original["attempts"] ==
            direct_original["completed"] +
            direct_original["cancellations"]
        and direct_original["kernelCalls"] ==
            direct_original["normalReturns"] +
            direct_original["kernelNoNormalReturns"]
        and direct_original["kernelCalls"] == direct_original["completed"]
        and direct_original["completed"] <= outside_native_fast
        and direct_original["attempts"] <=
            outside_admission_attribution.get("accepted", {}).get(
                "noPoison", -1
            )
        and direct_original["conflicts"] == 0
        and direct_original["cancellations"] == 0
        and direct_original["active"] == 0
        and direct_original["resetCompletedWhileActive"] == 0
        and direct_original["latePoison"] == 0
        and (
            diagnostic_policy.get("fullExact") is True
            and all(value == 0 for value in direct_original.values())
            or diagnostic_policy.get("lightExact") is True
            and direct_original["attempts"] > 0
        )
    )
    native_fanout = format_coverage["nativeFanout"]
    native_fanout_values_present = all(
        isinstance(value, int) for value in native_fanout.values()
    )
    native_upload_exactly_once = bool(
        isinstance(raw_uploads, int)
        and isinstance(native_upload.get("originalCalls"), int)
        and isinstance(native_upload.get("bypassedCalls"), int)
        and native_upload["originalCalls"] == raw_uploads
        and native_upload["bypassedCalls"] == 0
    )
    native_fanout_accounting_closed = bool(
        native_fanout_values_present
        and isinstance(raw_uploads, int)
        and isinstance(outside_uploads, int)
        and isinstance(outside_native_fast, int)
        and native_fanout["zero"] + native_fanout["one"] +
            native_fanout["many"] == raw_uploads
        and outside_uploads <= native_fanout["zero"]
        and outside_native_fast <= native_fanout["zero"]
        and (raw_uploads != 0 or native_fanout["max"] == 0)
        and (native_fanout["many"] != 0 or native_fanout["max"] <= 1)
        and (native_fanout["many"] == 0 or native_fanout["max"] >= 2)
    )
    pair_fast_policy = forced_snapshot.get(
        "cleanPairOutsideNativeFastPathPolicy", {}
    )
    begin_sample_cadence = forced_snapshot.get(
        "cleanPairNativeBeginSampleCadence", {}
    )
    outside_native_fast_policy_clean = bool(
        outside_native_fast is not None
        and fast_path.get("rejectScope") is not None
        and fast_path.get("outsideCallbacksSkipped") is not None
        and outside_uploads is not None
        and isinstance(reject_kernel_batches, int)
        and all(isinstance(value, int) for value in (
            poison_scan_attempts, poison_no_overlap, poison_overlap,
            poison_read_fail, marker_conflicts, *cover_values,
            independent_pin_begins, independent_pin_ends,
        ))
        and poison_scan_attempts == (
            poison_no_overlap + poison_overlap + poison_read_fail
        )
        and poison_no_overlap <= outside_native_fast
        and marker_conflicts == 0
        and sum(cover_values) + direct_original["attempts"] ==
            outside_admission_attribution.get(
            "accepted", {}
        ).get("total")
        and independent_pin_begins == independent_pin_ends
        and independent_pin_begins >= fast_path.get(
            "coverIndependent", 0
        )
        and 0 <= outside_native_fast <= fast_path["rejectScope"]
        and 0 <= reject_kernel_batches <= outside_uploads
        and outside_native_fast <= fast_path["outsideCallbacksSkipped"]
        and outside_native_fast <= outside_uploads
        and (
            (diagnostic_policy.get("fullExact") is True and
             outside_native_fast == 0 and reject_kernel_batches == 0
             and poison_scan_attempts == 0 and poison_no_overlap == 0
             and poison_overlap == 0 and poison_read_fail == 0
             and sum(cover_values) == 0
             and independent_pin_begins == 0
             and independent_pin_ends == 0)
            or
            (diagnostic_policy.get("lightExact") is True and
             outside_native_fast > 0 and poison_scan_attempts > 0
             and poison_no_overlap > 0
             and fast_path.get("coverSemantic", 0) > 0)
        )
        and pair_fast_policy.get("exact") is True
    )
    pair_poison_shadow_policy = forced_snapshot.get(
        "cleanPairNativePoisonShadow", {}
    )
    pair_poison_sidecar_policy = forced_snapshot.get(
        "cleanPairNativePoisonSidecarPolicy", {}
    )
    native_poison_sidecar_policy_contract_clean = bool(
        poison_sidecar_policy.get("present") is True
        and poison_sidecar_policy.get("contractClosed") is True
        and poison_sidecar_policy.get("authority") == 0
        and poison_sidecar_policy.get("authorizationAuthority") == 0
        and poison_sidecar_policy.get("reportOnly") is True
        and poison_sidecar_runtime.get("present") is True
        and poison_sidecar_runtime.get("exact") is True
        and poison_sidecar_runtime.get("authorityZero") is True
        and poison_sidecar_runtime.get("reportOnly") is True
        and pair_poison_sidecar_policy.get("present") is True
        and pair_poison_sidecar_policy.get("exact") is True
        and pair_poison_sidecar_policy.get("authorityZero") is True
        and pair_poison_sidecar_policy.get("reportOnly") is True
    )
    native_poison_shadow_contract_clean = bool(
        poison_shadow.get("present") is True
        and poison_shadow.get("active") == 0
        and poison_shadow.get("authorizationAuthority") == 0
        and poison_shadow.get("reportOnly") is True
        and pair_poison_shadow_policy.get("exact") is True
        and pair_poison_shadow_policy.get("authorizationAuthority") == 0
        and pair_poison_shadow_policy.get("reportOnly") is True
        and poison_sidecar_runtime.get("o0Exact") is True
    )
    pair_poison_o1_shadow_policy = forced_snapshot.get(
        "cleanPairNativePoisonO1Shadow", {}
    )
    native_poison_o1_shadow_contract_clean = bool(
        poison_o1_shadow.get("present") is True
        and poison_o1_shadow.get("active") == 0
        and poison_o1_shadow.get("authority") == 0
        and poison_o1_shadow.get("authorizationAuthority") == 0
        and poison_o1_shadow.get("reportOnly") is True
        and poison_o1_shadow.get("independentVerdictRequired") is True
        and pair_poison_o1_shadow_policy.get("exact") is True
        and pair_poison_o1_shadow_policy.get(
            "authorizationAuthority"
        ) == 0
        and pair_poison_o1_shadow_policy.get("reportOnly") is True
        and poison_sidecar_runtime.get("o1Exact") is True
        and (
            diagnostic_policy.get("lightExact") is not True
            or poison_sidecar_policy.get("o1Enabled") is not True
            or pair_poison_o1_shadow_policy.get(
                "promotionEligibleShadow"
            ) is True
        )
    )
    pair_poison_o1_authority_policy = forced_snapshot.get(
        "cleanPairNativePoisonO1Authority", {}
    )
    native_poison_o1_authority_contract_clean = bool(
        poison_o1_authority.get("present") is True
        and poison_o1_authority.get("contractClosed") is True
        and poison_o1_authority.get("active") == 0
        and poison_o1_authority.get("authorizationAuthority") ==
            poison_o1_authority.get("authority")
        and poison_o1_authority.get("reportOnly") is False
        and pair_poison_o1_authority_policy.get("present") is True
        and pair_poison_o1_authority_policy.get("exact") is True
        and pair_poison_o1_authority_policy.get("reportOnly") is False
    )
    resource_accounting_closed = (
        bool(forced_snapshot.get("cleanPairResourceDeltaClosed"))
        and lifetime["outputPending"] == 0
    )
    forced_quiescence_consistent = _quiescence_diag_consistent(diag)
    timing_structure_clean = bool(
        timing_contract.get("diagnosticPolicyPresent")
        and timing_contract.get("diagnosticPolicyRecognizedExact")
        and timing_contract.get("allPresent")
        and timing_contract.get("allShapeValid")
        and timing_contract.get("allFrequencyValid")
        and timing_contract.get("frequencyMatch")
        and timing_contract.get("nativeBeginSamplePeriodValid")
        and timing_contract.get("nativeT2SamplePeriodValid")
        and timing_contract.get("nativeSamplePeriodsMatch")
        and timing_contract.get("allClosuresContain")
        and bloom_timing_contract.get("exact") is True
    )
    clean_pair_policy = forced_snapshot.get(
        "cleanPairDiagnosticPolicy", {}
    )
    return {
        "diagnosticsPresent": required_present,
        "forcedDiagnosticsSnapshot": (
            bool(forced_snapshot.get("attempted"))
            and forced_snapshot.get("ok") is True
            and forced_snapshot.get("sessionProcessAuthorityExact") is True
            and bool(forced_snapshot.get("selectedBlockComplete"))
            and bool(forced_snapshot.get("beginSeen"))
            and bool(forced_snapshot.get("endSeen"))
            and bool(forced_snapshot.get("blockOrderValid"))
            and bool(forced_snapshot.get("blockSeenInCollectedEvidence"))
        ),
        "forcedDiagnosticsQuiescent": (
            forced_quiescence_consistent
            and forced_snapshot.get("ok") is True
            and forced_snapshot.get("sessionProcessAuthorityExact") is True
            and bool(forced_snapshot.get("quiescent"))
            and bool(forced_snapshot.get("twoCleanSnapshots"))
            and bool(forced_snapshot.get("progressValid"))
            and bool(forced_snapshot.get(
                "cleanPairRevalidatedInCollectedEvidence"
            ))
        ),
        "dipFastProbeContractClean": bool(
            dip_fast_probe.get("present") is True
            and dip_fast_probe.get("attemptsDerived") is True
            and dip_fast_probe.get("contractClosed") is True
            and dip_fast_probe_phase_clean
            and dip_fast_probe_policy_clean
        ),
        "hotPathTimingContractClean": bool(
            diagnostic_policy.get("fullExact") is True
            and timing_contract.get("diagnosticPolicyFullExact") is True
            and timing_structure_clean
            and clean_pair_policy.get("full") is True
            and forced_snapshot.get(
                "cleanPairFullPopulationTiming", {}
            ).get("exact") is True
            and forced_snapshot.get("cleanPairTimingDeltaValid") is True
        ),
        "lightDiagnosticsContractClean": bool(
            diagnostic_policy.get("lightExact") is True
            and timing_contract.get("diagnosticPolicyLightExact") is True
            and timing_structure_clean
            and clean_pair_policy.get("light") is True
            and forced_snapshot.get(
                "cleanPairLightZeroTiming", {}
            ).get("exact") is True
            and forced_snapshot.get("cleanPairTimingDeltaValid") is True
        ),
        "protocolAccountingClosed": protocol_accounting_closed,
        "managerDispatchAccountingClosed": (
            manager_dispatch_accounting_closed
        ),
        "managerDispatchPolicyClean": manager_dispatch_policy_clean,
        "dispatchCpuOnlySealContractClean": (
            dispatch_seal_policy_clean
        ),
        "telemetryBatchingExact": forced_snapshot.get(
            "cleanPairTelemetryDelta", {}
        ).get("exact") is True,
        "productionSampleTimingExact": forced_snapshot.get(
            "cleanPairProductionSampleTiming", {}
        ).get("exact") is True,
        "computeAccountingClosed": compute_accounting_closed,
        "kernelAccountingClosed": kernel_accounting_closed,
        "formatHistogramClosed": format_histogram_closed,
        "skinHistogramClosed": skin_histogram_closed,
        "formatBucketsKnown": format_buckets_known,
        "nativeInsideUploadRangeValid": native_inside_range_valid,
        "strictUploadClassificationClosed": (
            strict_upload_classification_closed
        ),
        "outsideNativeFastPathPolicyClean": (
            outside_native_fast_policy_clean
        ),
        "outsideNoPoisonDirectOriginalContractClean": (
            direct_original_contract_clean
        ),
        "nativePoisonSidecarPolicyContractClean": (
            native_poison_sidecar_policy_contract_clean
        ),
        "nativePoisonShadowContractClean": (
            native_poison_shadow_contract_clean
        ),
        "nativePoisonO1ShadowContractClean": (
            native_poison_o1_shadow_contract_clean
        ),
        "nativePoisonO1AuthorityContractClean": (
            native_poison_o1_authority_contract_clean
        ),
        "outsideAdmissionAttributionClean": bool(
            outside_admission_attribution.get("policyClean") is True
            and forced_snapshot.get(
                "cleanPairOutsideAdmissionAttribution", {}
            ).get("exact") is True
        ),
        "nativeUploadExactlyOnce": native_upload_exactly_once,
        "nativeFanoutAccountingClosed": native_fanout_accounting_closed,
        "nativeBeginSamplerCadenceClean": (
            begin_sample_cadence.get("exact") is True
        ),
        "formatCoverageReport": {
            **format_coverage,
            "insideUploads": inside_uploads,
            "strictRejectTotal": strict_reject_total,
            "dispatchSealUploads": fast_path.get(
                "dispatchSealUploads"
            ),
            "expectedFallbackPolicy": (
                "Non-zero path/output/fallback counts are report-only; "
                "formats 1/3/5 and non-Common paths remain native."
            ),
        },
        "fallbackCoverageReport": diag["fallbackCoverage"],
        "resourceAccountingClosed": resource_accounting_closed,
        "poisonDiscardAccountingCovered": (
            poison["clear"] is not None
            and direct_discard["rangesCleared"] is not None
            and poison["clear"] >= direct_discard["rangesCleared"]
        ),
        "processAlive": process_alive_exact,
        "twoScreenshots": len(screenshots) == 2 and all(item.get("ok") for item in screenshots),
        "bypassedKernelCallsPositive": (kernel["bypassedCalls"] or 0) > 0,
        "bypassedKernelBytesPositive": (kernel["bypassedBytes"] or 0) > 0,
        "poisonCreateAndHitPositive": (poison["create"] or 0) > 0 and (poison["hit"] or 0) > 0,
        "poisonHitCreateExact": (
            (poison["create"] or 0) > 0
            and poison["hit"] == poison["create"]
            and poison["create"] == kernel["bypassedCalls"]
        ),
        "poisonOverflowZero": poison["overflow"] == 0,
        "poisonResetStaleZero": poison["resetStale"] == 0,
        "poisonOutstandingZero": poison["outstanding"] == 0,
        "nativeDirectDiscardPathExercised": (
            direct_discard_present
            and (direct_discard["events"] or 0) > 0
            and (direct_discard["eventsWithPoison"] or 0) > 0
            and (direct_discard["rangesCleared"] or 0) > 0
        ),
        "nativeDirectDiscardAccountingClosed": (
            direct_discard_present
            and direct_discard["invalid"] == 0
            and direct_discard["events"] == (
                direct_discard["eventsWithPoison"]
                + direct_discard["noPoisonEvents"]
            )
            and direct_discard["rangesCleared"] >=
                direct_discard["eventsWithPoison"]
        ),
        "nativeCrossBackingPoisonMergeZero": (
            direct_discard_present
            and direct_discard["crossMapMerge"] == 0
        ),
        "indexTicketClean": ticket_mask == 0 and ticket["suppressed"] == 0 and ticket["leaks"] == 0,
        "indexTicketExact": (ticket["attempts"] or 0) > 0 and ticket["attempts"] == ticket["exact"],
        "takeoverCountsExact": (
            (kernel["bypassedCalls"] or 0) > 0
            and kernel["bypassedCalls"] == p3["bypassCommits"]
            and p3["bypassCommits"] == ticket["attempts"] == ticket["exact"]
        ),
        "postSkipClean": _all_equal_zero((native["postSkipFallback"], native["duplicate"], native["irreversible"], native["pending"])),
        "p3RestoreClean": p3_restore_clean,
        "bypassMismatchZero": p3["bypassMismatch"] == 0,
        "bypassHostAuthorizationMismatchZero": (
            p3["bypassHostAuthorizationMismatch"] == 0
        ),
        "bypassShadowConsumerPositive": (p4_shadow["bypassCommits"] or 0) > 0,
        "p4ShadowFinalClean": _all_equal_zero((
            p4_shadow["finalPositionReject"], p4_shadow["finalIndexReject"],
            p4_shadow["finalUvReject"], p4_shadow["finalCommitReject"],
        )),
        "outlineSubmittedPositive": not require_outline_all or (p3["outlineSubmitted"] or 0) > 0,
        "outlineSameSliceExact": (
            not require_outline_all or (
                (p3["outlineSubmitted"] or 0) > 0
                and p3["outlineSameSlice"] == p3["outlineSubmitted"]
                and p3["outlineSliceMismatch"] == 0
            )
        ),
        "outlineAllRequired": require_outline_all,
        "outlineSubmittedReport": p3["outlineSubmitted"],
        "outlineSameSliceReport": p3["outlineSameSlice"],
        "outlineSliceMismatchReport": p3["outlineSliceMismatch"],
        "ledgerClean": _all_equal_zero((ledger["leak"], ledger["unreserved"], ledger["duplicate"], ledger["planMismatch"], ledger["retireDeferred"])),
        "ledgerTerminalClean": (
            (ledger["resolved"] or 0) > 0
            and ledger["resolved"] == ledger["consumed"]
            and ledger["cpuFallback"] == 0
            and ledger["suppressed"] == 0
        ),
        "ledgerReasonConsistent": ledger_reason_consistent,
        "latePoisonPartitionsClosed": late_poison_partitions_closed,
        "latePoisonFlagIncidenceConsistent": (
            late_poison_flag_incidence_consistent
        ),
        "latePoisonKnownFlagMasks": (
            late_poison_partitions_present
            and late_poison["flagMask"]["unknown"] == 0
        ),
        "latePoisonNativePartitionsClosed": (
            late_poison_native_partitions_closed
        ),
        "latePoisonSampleDumpClosed": late_poison_sample_dump_closed,
        "latePoisonSampleGeometryConsistent": (
            sample_geometry_consistent
        ),
        "latePoisonSampleStorageConsistent": (
            sample_storage_consistent
        ),
        "latePoisonNoOpenOrUnmatched": (
            late_poison_partitions_present
            and late_poison["matchedOpen"] == 0
            and late_poison["unmatched"] == 0
        ),
        # Every late poison event suppresses a real DIP. A terminal ledger only
        # proves settlement idempotence, not that the suppressed draw was
        # visually redundant, so promotion requires no late poison at all.
        "latePoisonTotalZero": (
            late_poison_partitions_present and late_poison["total"] == 0
        ),
        "lifetimeClean": lifetime_clean,
        "exactTakeoverConflictClean": exact_conflict is None or exact_conflict == 0,
        "exactTakeoverConflictAvailability": tracker["exactTakeoverConflictAvailable"],
        "globalTrackerReportOnly": {"conflicts": tracker["conflicts"], "tag": tracker["tag"], "stage": tracker["stage"], "layer": tracker["layer"]},
        "nativeKernelNormalClean": _all_equal_zero((
            kernel_normal["overflow"], kernel_normal["invalid"], kernel_normal["fault"],
            kernel_normal["ackMismatch"], kernel_normal["commitBlocked"],
        )),
        "nativeRetirementClean": _all_equal_zero((
            retirement["overflow"], retirement["invalid"],
            retirement["pending"], retirement["fault"],
        )),
        "nativeResetFaultClean": _all_equal_zero((
            reset["wrongThread"], reset["retirementFault"],
            reset["pendingRetirement"],
        )),
        "crashScanClean": hard_process_evidence["crashScanClean"],
        "processAuthorityExact": process_alive_exact,
        "crashEvidenceAuthorityExact": crash_evidence_authority,
        "hardProcessEvidenceContract": hard_process_evidence,
    }


def _evaluate_vs_route_gates(
    diag: Dict[str, Any], expected_route: str,
    launch: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """验证默认 Compute 冷态，或显式 VS-S1 的输入与阴影提交闭合。"""
    if expected_route not in GPU_SKIN_EXECUTION_ROUTES:
        raise ValueError(f"unsupported GPU-skin execution route: {expected_route}")

    record = dict(diag.get("VSRoute", {}) or {})

    def exact_tuple(name: str, length: int) -> tuple[Any, ...]:
        value = record.get(name)
        if isinstance(value, (tuple, list)) and len(value) == length:
            return tuple(value)
        return (None,) * length

    prepared = exact_tuple("inputPrepared", 2)
    submitted = exact_tuple("inputSubmitted", 2)
    main = exact_tuple("main", 4)
    input_consumers = exact_tuple("inputConsumers", 3)
    input_only = exact_tuple("inputOnly", 6)
    if all(value is None for value in input_only) and record.get("route") in (
        0, 1,
    ):
        # 兼容读取 VS-B0 字段加入前的 Compute/VS-A 历史 artifact；新路线
        # route=2 绝不接受缺失字段。
        input_only = (0, 0, 0, 0, 0, 0)
    shadow_capture = exact_tuple("shadowCapture", 4)
    shadow_direct = exact_tuple("shadowDirect", 5)
    shadow_replay = exact_tuple("shadowReplay", 3)
    cleared = record.get("cleared")
    compute = dict(diag.get("compute", {}) or {})

    def compute_tuple(name: str) -> tuple[Any, ...]:
        value = compute.get(name)
        if isinstance(value, (tuple, list)) and len(value) == 2:
            return tuple(value)
        return (None, None)

    compute_jobs = compute_tuple("jobs")
    compute_dispatch = compute_tuple("dispatch")
    p4_shadow = dict(diag.get("P4Shadow", {}) or {})
    shadow_leases_consumed = p4_shadow.get("leasesConsumed")
    shadow_bypass_commits = p4_shadow.get("bypassCommits")
    p3 = dict(diag.get("P3", {}) or {})
    kernel = dict(diag.get("kernel", {}) or {})
    poison = dict(diag.get("nativePoison", {}) or {})
    direct_discard = dict(diag.get("nativeDirectDiscard", {}) or {})
    ticket = dict(diag.get("indexTicket", {}) or {})
    kernel_normal = dict(diag.get("nativeKernelNormal", {}) or {})
    ledger = dict(diag.get("ledger", {}) or {})
    pair_record = dict(
        dict(diag.get("forcedSnapshot", {}) or {}).get(
            "cleanPairVsShadowRoute", {}
        ) or {}
    )
    forced_snapshot = dict(diag.get("forcedSnapshot", {}) or {})

    def non_negative_int(value: Any) -> bool:
        return bool(
            isinstance(value, int) and not isinstance(value, bool)
            and value >= 0
        )

    present = bool(
        len(prepared) == 2 and len(submitted) == 2 and len(main) == 4
        and len(input_consumers) == 3 and len(shadow_capture) == 4
        and len(shadow_direct) == 5 and len(shadow_replay) == 3
        and all(
            non_negative_int(value)
            for value in (
                prepared + submitted + main + input_consumers
                + input_only
                + shadow_capture + shadow_direct + shadow_replay
                + (cleared,)
            )
        )
        and non_negative_int(record.get("route"))
        and non_negative_int(record.get("explicit"))
        and non_negative_int(record.get("invalid"))
    )
    compute_present = bool(
        len(compute_jobs) == 2 and len(compute_dispatch) == 2
        and all(non_negative_int(value) for value in (
            compute_jobs + compute_dispatch
        ))
    )
    launch_record = dict(launch or {})
    effective_raw = launch_record.get("effectiveWar3Environment")
    effective_environment = (
        dict(effective_raw) if isinstance(effective_raw, dict) else {}
    )
    host_isolation = dict(
        launch_record.get("executionRouteHostEnvironmentIsolation", {}) or {}
    )
    environment_reported = isinstance(effective_raw, dict)
    host_isolation_exact = bool(
        host_isolation.get("environmentName") == GPU_SKIN_EXECUTION_ROUTE_ENV
        and host_isolation.get("hostEnvironmentRestored") is True
    )
    if expected_route in (
        "vertex_shader", "vertex_shader_input_only", "vertex_shader_bypass",
    ):
        expected_route_id = {
            "vertex_shader": 1,
            "vertex_shader_input_only": 2,
            "vertex_shader_bypass": 3,
        }[expected_route]
        config_exact = bool(
            present and record["route"] == expected_route_id
            and record["explicit"] == 1 and record["invalid"] == 0
        )
        environment_exact = bool(
            environment_reported and host_isolation_exact
            and effective_environment.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                expected_route
        )
        input_contract = bool(
            present and prepared[0] > 0
            and prepared[1] > 0
            and prepared[1] % GPU_SKIN_PALETTE_MATRIX_BYTES == 0
            and prepared[1] >=
                prepared[0] * GPU_SKIN_PALETTE_MATRIX_BYTES
            and prepared == submitted
        )
        main_contract = bool(
            present and main[0] > 0 and main[3] > 0
            and main[0] == main[1] + main[2] + main[3]
            and cleared == main[3]
        )
        if expected_route == "vertex_shader":
            compute_retained = bool(
                compute_present and compute_jobs[0] > 0
                and compute_jobs[0] == compute_jobs[1] == prepared[0]
                and compute_dispatch[0] > 0
                and compute_dispatch[0] == compute_dispatch[1]
                and input_only == (0, 0, 0, 0, 0, 0)
            )
            input_consumer_contract = bool(
                present
                and input_consumers == (
                    GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK, prepared[0], 0,
                )
            )
            shadow_capture_contract = bool(
                present and shadow_capture[0] > 0
                and shadow_capture[1] == 0 and shadow_capture[2] == 0
                and shadow_capture[3] > 0
                and shadow_capture[0] == (
                    shadow_capture[1] + shadow_capture[2] + shadow_capture[3]
                )
                and non_negative_int(shadow_leases_consumed)
                and non_negative_int(shadow_bypass_commits)
                and shadow_capture[3] == shadow_leases_consumed
                and shadow_capture[3] == shadow_bypass_commits
            )
            shadow_direct_contract = bool(
                present and shadow_direct[0] > 0
                and shadow_direct[1] == 0 and shadow_direct[2] == 0
                and shadow_direct[3] > 0
                and shadow_direct[0] == (
                    shadow_direct[1] + shadow_direct[2] + shadow_direct[3]
                )
                and shadow_direct[3] == shadow_direct[4]
                and shadow_replay[2] == 0
                and shadow_direct[3] ==
                    shadow_replay[0] + shadow_replay[1]
            )
            p4_authority_contract = True
            forced_snapshot_contract = True
            ledger_terminal_contract = True
        elif expected_route == "vertex_shader_input_only":
            # VS-B0 的硬证据是 input lease 已提交、相同数量的 compute
            # job/output 被省略，并晋级 Main/Shadow input consumer。CPU
            # kernel 必须保留，且本路线不得获得任何 P4 bypass authority。
            compute_retained = bool(
                compute_present
                and compute_jobs[0] == compute_jobs[1]
                and compute_dispatch[0] == compute_dispatch[1]
                and input_only[0] == prepared[0]
                and input_only[1] == prepared[1]
                and input_only[2] == prepared[0]
                and input_only[3] > 0
                and input_only[4] > 0
                and input_only[4] <= main[3]
                and input_only[5] >= 0
            )
            input_consumer_contract = bool(
                present and input_consumers == (
                    GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK, prepared[0], 0,
                )
            )
            shadow_capture_contract = bool(
                present and shadow_capture[0] > 0
                and shadow_capture[1] == 0 and shadow_capture[2] == 0
                and shadow_capture[3] > 0
                and shadow_capture[0] == (
                    shadow_capture[1] + shadow_capture[2] + shadow_capture[3]
                )
                and non_negative_int(shadow_leases_consumed)
                and non_negative_int(shadow_bypass_commits)
                and shadow_capture[3] == shadow_leases_consumed
                and shadow_bypass_commits == 0
            )
            shadow_direct_contract = bool(
                present and shadow_direct[0] > 0
                and shadow_direct[1] == 0 and shadow_direct[2] == 0
                and shadow_direct[3] > 0
                and shadow_direct[0] == (
                    shadow_direct[1] + shadow_direct[2] + shadow_direct[3]
                )
                and shadow_direct[3] == shadow_direct[4]
                and shadow_replay[2] == 0
                and shadow_direct[3] ==
                    shadow_replay[0] + shadow_replay[1]
            )
            p4_authority_contract = bool(
                non_negative_int(p3.get("bypassAttempts"))
                and p3["bypassAttempts"] > 0
                and non_negative_int(p3.get("bypassFallbacks"))
                and p3["bypassFallbacks"] > 0
                and p3.get("bypassAuthorizations") == 0
                and p3.get("bypassCommits") == 0
                and kernel.get("originalCalls", 0) > 0
                and kernel.get("originalBytes", 0) > 0
                and kernel.get("bypassedCalls") == 0
                and kernel.get("bypassedBytes") == 0
                and poison.get("create") == 0
                and poison.get("hit") == 0
                and poison.get("outstanding") == 0
                and ticket.get("attempts") == 0
                and ticket.get("exact") == 0
                and p4_shadow.get("bypassCommits") == 0
                and all(
                    direct_discard.get(name) == 0
                    for name in (
                        "events", "eventsWithPoison", "rangesCleared",
                        "invalid", "crossMapMerge",
                    )
                )
                and all(
                    kernel_normal.get(name) == 0
                    for name in (
                        "overflow", "invalid", "fault", "ackMismatch",
                        "commitBlocked",
                    )
                )
            )
            outside_pair = dict(
                forced_snapshot.get(
                    "cleanPairOutsideNativeFastPathPolicy", {}
                ) or {}
            )
            direct_original = dict(
                outside_pair.get("directOriginalDeltas", {}) or {}
            )
            o1_pair = dict(
                forced_snapshot.get(
                    "cleanPairNativePoisonO1Authority", {}
                ) or {}
            )
            o1_delta = dict(o1_pair.get("delta", {}) or {})
            direct_positive = tuple(
                direct_original.get(name)
                for name in (
                    "directOriginalAttempts",
                    "directOriginalKernelCalls",
                    "directOriginalNormalReturns",
                    "directOriginalCompleted",
                )
            )
            forced_snapshot_contract = bool(
                forced_snapshot.get("attempted") is True
                and forced_snapshot.get("beginSeen") is True
                and forced_snapshot.get("endSeen") is True
                and forced_snapshot.get("blockOrderValid") is True
                and forced_snapshot.get("blockAttributionValid") is True
                and forced_snapshot.get("selectedBlockComplete") is True
                and forced_snapshot.get("quiescent") is True
                and forced_snapshot.get("twoCleanSnapshots") is True
                and forced_snapshot.get("progressValid") is True
                and forced_snapshot.get(
                    "cleanPairResourceDeltaClosed"
                ) is True
                and pair_record.get("exact") is True
                and outside_pair.get("present") is True
                and outside_pair.get("light") is True
                and all(
                    isinstance(value, int) and value > 0
                    for value in direct_positive
                )
                and len(set(direct_positive)) == 1
                and all(
                    direct_original.get(name) == 0
                    for name in (
                        "directOriginalKernelNoNormalReturns",
                        "directOriginalConflicts",
                        "directOriginalCancellations",
                        "directOriginalResetCompletedWhileActive",
                        "directOriginalLatePoison",
                    )
                )
                and all(
                    outside_pair.get(name) == 0
                    for name in (
                        "poisonScanAttemptsDelta", "poisonNoOverlapDelta",
                        "poisonOverlapDelta", "poisonReadFailDelta",
                        "markerConflictsDelta", "independentPinBeginsDelta",
                        "independentPinEndsDelta",
                    )
                )
                and all(
                    value == 0 for value in dict(
                        outside_pair.get("coverDeltas", {}) or {}
                    ).values()
                )
                and all(
                    value == 0 for value in dict(
                        outside_pair.get(
                            "directOriginalActiveEndpoints", {}
                        ) or {}
                    ).values()
                )
                and o1_pair.get("present") is True
                and o1_pair.get("recognizedExact") is True
                and o1_pair.get("light") is True
                and o1_pair.get("sidecarEnabled") is False
                and o1_pair.get("authorizationAuthority") == 0
                and o1_delta.get("present") is True
                and o1_delta.get("monotonic") is True
                and o1_delta.get("valid") is True
                and o1_delta.get("endpointsQuiescent") is True
                and o1_delta.get("allDeltasZero") is True
                and o1_delta.get("authorizationAuthority") == 0
            )
            resolved_consumers = input_only[4] * 2
            consumed_consumers = main[3] + shadow_capture[3]
            ledger_terminal_contract = bool(
                input_only[4] > 0
                and shadow_capture[3] <= input_only[4]
                and ledger.get("classified") == prepared[0] * 3
                and ledger.get("resolved") == resolved_consumers
                and ledger.get("consumed") == consumed_consumers
                and ledger.get("cpuFallback") ==
                    resolved_consumers - consumed_consumers
                and ledger.get("suppressed") == 0
                and all(
                    ledger.get(name) == 0
                    for name in (
                        "leak", "unreserved", "duplicate", "planMismatch",
                        "retireDeferred",
                    )
                )
            )
        else:
            # VS-B1 与 B0 一样不创建 compute output/job，但它通过独立的
            # input-capability preflight 获得 P4 权限。Outline 在授权前被拒绝；
            # Main 与 Shadow 都只能消费 generation-pinned static+palette。
            compute_retained = bool(
                compute_present
                and compute_jobs[0] == compute_jobs[1]
                and compute_dispatch[0] == compute_dispatch[1]
                and input_only[0] == prepared[0]
                and input_only[1] == prepared[1]
                and input_only[2] == prepared[0]
                and input_only[3] > 0
                and input_only[4] > 0
                and input_only[4] <= main[3]
                and input_only[5] >= 0
            )
            input_consumer_contract = bool(
                present and input_consumers == (
                    GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK, prepared[0], 0,
                )
            )
            shadow_capture_contract = bool(
                present and shadow_capture[0] > 0
                and shadow_capture[1] == 0 and shadow_capture[2] == 0
                and shadow_capture[3] > 0
                and shadow_capture[0] == shadow_capture[3]
                and non_negative_int(shadow_leases_consumed)
                and non_negative_int(shadow_bypass_commits)
                and shadow_capture[3] == shadow_leases_consumed
                and shadow_capture[3] == shadow_bypass_commits
            )
            shadow_direct_contract = bool(
                present and shadow_direct[0] > 0
                and shadow_direct[1] == 0 and shadow_direct[2] == 0
                and shadow_direct[3] > 0
                and shadow_direct[0] == shadow_direct[3]
                and shadow_direct[3] == shadow_direct[4]
                and shadow_replay[2] == 0
                and shadow_direct[3] ==
                    shadow_replay[0] + shadow_replay[1]
            )
            p4_authority_contract = bool(
                non_negative_int(p3.get("bypassAttempts"))
                and p3["bypassAttempts"] > 0
                and non_negative_int(p3.get("bypassAuthorizations"))
                and p3["bypassAuthorizations"] > 0
                and p3.get("bypassCommits") == p3["bypassAuthorizations"]
                and kernel.get("bypassedCalls", 0) > 0
                and kernel.get("bypassedBytes", 0) > 0
                and poison.get("create", 0) > 0
                and poison.get("create") == poison.get("hit")
                and poison.get("outstanding") == 0
                and ticket.get("attempts", 0) > 0
                and ticket.get("attempts") == ticket.get("exact")
                and p4_shadow.get("bypassCommits", 0) > 0
            )
            forced_snapshot_contract = bool(
                forced_snapshot.get("attempted") is True
                and forced_snapshot.get("beginSeen") is True
                and forced_snapshot.get("endSeen") is True
                and forced_snapshot.get("quiescent") is True
                and forced_snapshot.get("twoCleanSnapshots") is True
                and pair_record.get("exact") is True
            )
            ledger_terminal_contract = bool(
                ledger.get("consumed", 0) > 0
                and all(
                    ledger.get(name) == 0
                    for name in (
                        "leak", "unreserved", "duplicate", "planMismatch",
                        "retireDeferred",
                    )
                )
            )
    else:
        config_exact = bool(
            present and record["route"] == 0
            and record["explicit"] == 0 and record["invalid"] == 0
        )
        environment_exact = bool(
            environment_reported and host_isolation_exact
            and GPU_SKIN_EXECUTION_ROUTE_ENV not in effective_environment
        )
        input_contract = bool(
            present and prepared == (0, 0) and submitted == (0, 0)
            and input_only == (0, 0, 0, 0, 0, 0)
        )
        main_contract = bool(
            present and main == (0, 0, 0, 0) and cleared == 0
        )
        compute_retained = present
        input_consumer_contract = bool(
            present and input_consumers == (0, 0, 0)
        )
        shadow_capture_contract = bool(
            present and shadow_capture == (0, 0, 0, 0)
        )
        shadow_direct_contract = bool(
            present and shadow_direct == (0, 0, 0, 0, 0)
            and shadow_replay == (0, 0, 0)
        )
        p4_authority_contract = True
        forced_snapshot_contract = True
        ledger_terminal_contract = True
    pair_delta_contract = bool(
        pair_record.get("exact") is True
        and pair_record.get("route") == expected_route
        and pair_record.get("replayCommitEqualityRequired") is False
    )
    return {
        "vsRouteConfigExact": config_exact,
        "vsRouteEnvironmentExact": environment_exact,
        "vsRouteInputContract": input_contract,
        "vsRouteMainContract": main_contract,
        "vsRouteComputeRetained": compute_retained,
        "vsRouteInputConsumerContract": input_consumer_contract,
        "vsRouteShadowCaptureContract": shadow_capture_contract,
        "vsRouteShadowDirectContract": shadow_direct_contract,
        "vsRouteShadowPairDeltaContract": pair_delta_contract,
        "vsRouteP4AuthorityContract": p4_authority_contract,
        "vsRouteForcedSnapshotContract": forced_snapshot_contract,
        "vsRouteLedgerTerminalContract": ledger_terminal_contract,
        "vsRouteExpected": expected_route,
        "vsRouteReport": record,
        "vsRouteShadowPairDeltaReport": pair_record,
        "vsRouteEnvironmentReport": {
            "effectiveEnvironmentReported": environment_reported,
            "effectiveValue": effective_environment.get(
                GPU_SKIN_EXECUTION_ROUTE_ENV
            ),
            "hostIsolation": host_isolation,
        },
    }


def _gpu_skin_resource_delta_closed(
    previous_jobs: Any, current_jobs: Any,
    previous_input_storage: Any, current_input_storage: Any,
    previous_retired: Any, current_retired: Any,
    previous_pending: Any, current_pending: Any,
    previous_input_only_jobs: Any = 0,
    current_input_only_jobs: Any = 0,
) -> bool:
    """闭合 compute 输出与 VS palette storage 的累计退休增量。"""
    values = (
        previous_jobs, current_jobs,
        previous_input_storage, current_input_storage,
        previous_retired, current_retired,
        previous_pending, current_pending,
        previous_input_only_jobs, current_input_only_jobs,
    )
    if not all(
        isinstance(value, int) and not isinstance(value, bool) and value >= 0
        for value in values
    ):
        return False

    job_delta = current_jobs - previous_jobs
    input_storage_delta = current_input_storage - previous_input_storage
    input_only_job_delta = (
        current_input_only_jobs - previous_input_only_jobs
    )
    retired_delta = current_retired - previous_retired
    return bool(
        job_delta >= 0
        and input_storage_delta >= 0
        and input_storage_delta <= job_delta
        and input_only_job_delta >= 0
        and input_only_job_delta <= job_delta
        and retired_delta >= 0
        and previous_pending == 0
        and current_pending == 0
        and retired_delta == (
            job_delta - input_only_job_delta + input_storage_delta
        )
    )


def _vs_shadow_route_pair_contract(
    previous: Dict[str, Any], current: Dict[str, Any],
) -> Dict[str, Any]:
    """按 lease 域和 replay draw 域分别闭合 VS-S1 clean-pair。"""
    previous_route = dict(previous.get("VSRoute", {}) or {})
    current_route = dict(current.get("VSRoute", {}) or {})
    previous_compute = dict(previous.get("compute", {}) or {})
    current_compute = dict(current.get("compute", {}) or {})
    previous_shadow = dict(previous.get("P4Shadow", {}) or {})
    current_shadow = dict(current.get("P4Shadow", {}) or {})

    def fixed_tuple(
        source: Dict[str, Any], name: str, length: int,
    ) -> tuple[Any, ...]:
        value = source.get(name)
        if isinstance(value, (tuple, list)) and len(value) == length:
            return tuple(value)
        return (None,) * length

    def valid_counter(value: Any) -> bool:
        return bool(
            isinstance(value, int) and not isinstance(value, bool)
            and value >= 0
        )

    def deltas(
        before: tuple[Any, ...], after: tuple[Any, ...],
    ) -> tuple[Any, ...]:
        if not all(valid_counter(value) for value in before + after):
            return (None,) * len(before)
        return tuple(after[index] - before[index]
                     for index in range(len(before)))

    route_fields = ("route", "explicit", "invalid")
    previous_config = tuple(previous_route.get(name) for name in route_fields)
    current_config = tuple(current_route.get(name) for name in route_fields)
    compute_pair = previous_config == current_config == (0, 0, 0)
    vertex_pair = previous_config == current_config == (1, 1, 0)
    input_only_pair = previous_config == current_config == (2, 1, 0)
    bypass_pair = previous_config == current_config == (3, 1, 0)

    tuple_specs = {
        "inputPrepared": 2,
        "inputSubmitted": 2,
        "main": 4,
        "inputConsumers": 3,
        "inputOnly": 6,
        "shadowCapture": 4,
        "shadowDirect": 5,
        "shadowReplay": 3,
    }
    previous_values = {
        name: fixed_tuple(previous_route, name, length)
        for name, length in tuple_specs.items()
    }
    current_values = {
        name: fixed_tuple(current_route, name, length)
        for name, length in tuple_specs.items()
    }
    if compute_pair or vertex_pair:
        for values in (previous_values, current_values):
            if all(value is None for value in values["inputOnly"]):
                values["inputOnly"] = (0, 0, 0, 0, 0, 0)
    previous_cleared = previous_route.get("cleared")
    current_cleared = current_route.get("cleared")

    if compute_pair:
        cold_values = tuple(
            value
            for name in tuple_specs
            for value in previous_values[name] + current_values[name]
        ) + (previous_cleared, current_cleared)
        cold = bool(
            all(valid_counter(value) for value in cold_values)
            and all(value == 0 for value in cold_values)
        )
        return {
            "present": cold,
            "exact": cold,
            "route": "compute",
            "allMonotonic": cold,
            "allVsFieldsCold": cold,
            "replayCommitEqualityRequired": False,
            "authorizationAuthority": 0,
        }

    if not vertex_pair and not input_only_pair and not bypass_pair:
        return {
            "present": False,
            "exact": False,
            "route": "invalid",
            "allMonotonic": False,
            "replayCommitEqualityRequired": False,
            "authorizationAuthority": 0,
            "reason": "clean-pair 路线配置不一致或不是受支持路线",
        }

    previous_jobs = fixed_tuple(previous_compute, "jobs", 2)
    current_jobs = fixed_tuple(current_compute, "jobs", 2)
    previous_dispatch = fixed_tuple(previous_compute, "dispatch", 2)
    current_dispatch = fixed_tuple(current_compute, "dispatch", 2)
    job_delta = deltas(previous_jobs, current_jobs)
    dispatch_delta = deltas(previous_dispatch, current_dispatch)
    route_deltas = {
        name: deltas(previous_values[name], current_values[name])
        for name in tuple_specs
    }
    cleared_delta = (
        current_cleared - previous_cleared
        if valid_counter(previous_cleared) and valid_counter(current_cleared)
        else None
    )
    previous_shadow_pair = (
        previous_shadow.get("leasesConsumed"),
        previous_shadow.get("bypassCommits"),
    )
    current_shadow_pair = (
        current_shadow.get("leasesConsumed"),
        current_shadow.get("bypassCommits"),
    )
    shadow_commit_delta = deltas(
        previous_shadow_pair, current_shadow_pair,
    )

    monotonic_values = (
        job_delta + dispatch_delta + shadow_commit_delta
        + tuple(
            value
            for name, values in route_deltas.items()
            for index, value in enumerate(values)
            if not (name == "inputConsumers" and index == 0)
        )
        + (cleared_delta,)
    )
    all_monotonic = bool(
        all(isinstance(value, int) and value >= 0
            for value in monotonic_values)
    )

    def cumulative_capture_closed(values: Dict[str, tuple[Any, ...]]) -> bool:
        capture = values["shadowCapture"]
        return bool(
            all(valid_counter(value) for value in capture)
            and capture[1] == 0 and capture[2] == 0
            and capture[0] == capture[1] + capture[2] + capture[3]
        )

    def cumulative_direct_closed(values: Dict[str, tuple[Any, ...]]) -> bool:
        direct = values["shadowDirect"]
        replay = values["shadowReplay"]
        return bool(
            all(valid_counter(value) for value in direct + replay)
            and direct[1] == 0 and direct[2] == 0
            and direct[0] == direct[1] + direct[2] + direct[3]
            and direct[3] == direct[4]
            and replay[2] == 0
            and direct[3] == replay[0] + replay[1]
        )

    input_prepared_delta = route_deltas["inputPrepared"]
    input_submitted_delta = route_deltas["inputSubmitted"]
    input_consumer_delta = route_deltas["inputConsumers"]
    input_only_delta = route_deltas["inputOnly"]
    capture_delta = route_deltas["shadowCapture"]
    direct_delta = route_deltas["shadowDirect"]
    replay_delta = route_deltas["shadowReplay"]
    if input_only_pair or bypass_pair:
        main_delta = route_deltas["main"]
        input_only_exact = bool(
            all_monotonic
            and input_prepared_delta == input_submitted_delta
            and input_prepared_delta[0] > 0
            and input_prepared_delta[1] > 0
            and input_prepared_delta[1] %
                GPU_SKIN_PALETTE_MATRIX_BYTES == 0
            and previous_values["inputConsumers"][0] ==
                GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK
            and current_values["inputConsumers"][0] ==
                GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK
            and input_consumer_delta[1] == input_prepared_delta[0]
            and previous_values["inputConsumers"][2] == 0
            and current_values["inputConsumers"][2] == 0
            and input_only_delta[0] == input_prepared_delta[0]
            and input_only_delta[1] == input_prepared_delta[1]
            and input_only_delta[2] == input_prepared_delta[0]
            and input_only_delta[3] > 0
            and input_only_delta[4] > 0
            and input_only_delta[4] <= main_delta[3]
            and main_delta[0] ==
                main_delta[1] + main_delta[2] + main_delta[3]
            and cleared_delta == main_delta[3]
            and cumulative_capture_closed(previous_values)
            and cumulative_capture_closed(current_values)
            and capture_delta[0] > 0
            and capture_delta[1] == 0 and capture_delta[2] == 0
            and capture_delta[0] == capture_delta[3]
            and shadow_commit_delta[0] == capture_delta[3]
            and (
                (input_only_pair and previous_shadow_pair[1] == 0
                 and current_shadow_pair[1] == 0
                 and shadow_commit_delta[1] == 0)
                or
                (bypass_pair
                 and shadow_commit_delta[1] == capture_delta[3]
                 and previous_values["shadowCapture"][3] ==
                     previous_shadow_pair[0] == previous_shadow_pair[1]
                 and current_values["shadowCapture"][3] ==
                     current_shadow_pair[0] == current_shadow_pair[1])
            )
            and cumulative_direct_closed(previous_values)
            and cumulative_direct_closed(current_values)
            and direct_delta[0] >= 0
            and direct_delta[1] == 0 and direct_delta[2] == 0
            and direct_delta[0] == direct_delta[3]
            and direct_delta[3] == direct_delta[4]
            and replay_delta[2] == 0
            and direct_delta[3] == replay_delta[0] + replay_delta[1]
            and job_delta[0] == job_delta[1]
            and dispatch_delta[0] == dispatch_delta[1]
        )
        return {
            "present": True,
            "exact": input_only_exact,
            "route": (
                "vertex_shader_input_only" if input_only_pair
                else "vertex_shader_bypass"
            ),
            "allMonotonic": all_monotonic,
            "computeRetained": False,
            "computeSkipExact": input_only_exact,
            "inputContract": input_only_exact,
            "captureContract": input_only_exact,
            "directContract": input_only_exact,
            "replayCommitEqualityRequired": False,
            "authorizationAuthority": 0 if input_only_pair else None,
            "deltas": {
                "computeJobs": list(job_delta),
                "computeDispatch": list(dispatch_delta),
                "inputPrepared": list(input_prepared_delta),
                "inputSubmitted": list(input_submitted_delta),
                "inputConsumers": list(input_consumer_delta),
                "inputOnly": list(input_only_delta),
                "main": list(main_delta),
                "shadowCapture": list(capture_delta),
                "shadowDirect": list(direct_delta),
                "shadowReplay": list(replay_delta),
                "shadowCommits": list(shadow_commit_delta),
                "mainCleared": cleared_delta,
            },
        }
    input_contract = bool(
        all_monotonic and job_delta[0] > 0
        and job_delta[0] == job_delta[1]
        and dispatch_delta[0] > 0
        and dispatch_delta[0] == dispatch_delta[1]
        and input_prepared_delta[0] == job_delta[0]
        and input_submitted_delta[0] == job_delta[0]
        and input_prepared_delta == input_submitted_delta
        and input_prepared_delta[1] > 0
        and input_prepared_delta[1] % GPU_SKIN_PALETTE_MATRIX_BYTES == 0
        and previous_values["inputConsumers"][0] ==
            GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK
        and current_values["inputConsumers"][0] ==
            GPU_SKIN_MAIN_SHADOW_CONSUMER_MASK
        and input_consumer_delta[1] == job_delta[0]
        and previous_values["inputConsumers"][2] == 0
        and current_values["inputConsumers"][2] == 0
        and input_consumer_delta[2] == 0
    )
    capture_contract = bool(
        all_monotonic
        and cumulative_capture_closed(previous_values)
        and cumulative_capture_closed(current_values)
        and capture_delta[0] > 0
        and capture_delta[1] == 0 and capture_delta[2] == 0
        and capture_delta[0] == capture_delta[3]
        and shadow_commit_delta[0] == capture_delta[3]
        and shadow_commit_delta[1] == capture_delta[3]
        and previous_values["shadowCapture"][3] ==
            previous_shadow_pair[0] == previous_shadow_pair[1]
        and current_values["shadowCapture"][3] ==
            current_shadow_pair[0] == current_shadow_pair[1]
    )
    direct_contract = bool(
        all_monotonic
        and cumulative_direct_closed(previous_values)
        and cumulative_direct_closed(current_values)
        # Shadow replay 不保证落在强制快照的短间隔内。生命周期总门已经
        # 单独要求 direct>0；clean-pair 这里只要求零进展时所有相关增量
        # 同为零，不能把合法静止窗口误判为运行失败。
        and direct_delta[0] >= 0
        and direct_delta[1] == 0 and direct_delta[2] == 0
        and direct_delta[0] == direct_delta[3]
        and direct_delta[3] == direct_delta[4]
        and replay_delta[2] == 0
        and direct_delta[3] == replay_delta[0] + replay_delta[1]
    )
    exact = bool(
        input_contract and capture_contract and direct_contract
    )
    return {
        "present": True,
        "exact": exact,
        "route": "vertex_shader",
        "allMonotonic": all_monotonic,
        "computeRetained": input_contract,
        "inputContract": input_contract,
        "captureContract": capture_contract,
        "directContract": direct_contract,
        "replayCommitEqualityRequired": False,
        "authorizationAuthority": 0,
        "deltas": {
            "computeJobs": list(job_delta),
            "computeDispatch": list(dispatch_delta),
            "inputPrepared": list(input_prepared_delta),
            "inputSubmitted": list(input_submitted_delta),
            "inputConsumers": list(input_consumer_delta),
            "shadowCapture": list(capture_delta),
            "shadowDirect": list(direct_delta),
            "shadowReplay": list(replay_delta),
            "shadowCommits": list(shadow_commit_delta),
            "mainCleared": cleared_delta,
        },
    }


def _vs_route_synthetic_self_tests() -> Dict[str, Any]:
    """纯合成验证路线解析、字节契约、强制唯一性与宿主环境隔离。"""
    inherited_present = GPU_SKIN_EXECUTION_ROUTE_ENV in os.environ
    inherited_value = os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV)

    def fake_launcher(**kwargs: Any) -> Dict[str, Any]:
        effective = {
            str(key): str(value)
            for key, value in os.environ.items()
            if str(key).upper().startswith("DXVK_WAR3_")
        }
        effective.update(json.loads(kwargs.get("env_overrides_json", "{}")))
        return {"ok": True, "effectiveWar3Environment": effective}

    try:
        os.environ[GPU_SKIN_EXECUTION_ROUTE_ENV] = "inherited_bad_route"
        compute_launch = _launch_war3_with_execution_route_isolation(
            execution_route="compute", _launcher=fake_launcher,
            env_overrides_json=json.dumps(
                _p4_environment("light", "none", "compute")
            ),
        )
        compute_host_restored = (
            os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                "inherited_bad_route"
        )
        vertex_launch = _launch_war3_with_execution_route_isolation(
            execution_route="vertex_shader", _launcher=fake_launcher,
            env_overrides_json=json.dumps(
                _p4_environment("light", "none", "vertex_shader")
            ),
        )
        vertex_host_restored = (
            os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                "inherited_bad_route"
        )
        input_only_launch = _launch_war3_with_execution_route_isolation(
            execution_route="vertex_shader_input_only",
            _launcher=fake_launcher,
            env_overrides_json=json.dumps(
                _p4_environment(
                    "light", "none", "vertex_shader_input_only"
                )
            ),
        )
        input_only_host_restored = (
            os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                "inherited_bad_route"
        )
        bypass_launch = _launch_war3_with_execution_route_isolation(
            execution_route="vertex_shader_bypass",
            _launcher=fake_launcher,
            env_overrides_json=json.dumps(
                _p4_environment(
                    "light", "none", "vertex_shader_bypass"
                )
            ),
        )
        bypass_host_restored = (
            os.environ.get(GPU_SKIN_EXECUTION_ROUTE_ENV) ==
                "inherited_bad_route"
        )
    finally:
        if inherited_present:
            os.environ[GPU_SKIN_EXECUTION_ROUTE_ENV] = str(
                inherited_value if inherited_value is not None else ""
            )
        else:
            os.environ.pop(GPU_SKIN_EXECUTION_ROUTE_ENV, None)

    compute_diag = {
        "VSRoute": {
            "route": 0, "explicit": 0, "invalid": 0,
            "inputPrepared": [0, 0], "inputSubmitted": [0, 0],
            "main": [0, 0, 0, 0], "cleared": 0,
            "inputConsumers": [0, 0, 0],
            "shadowCapture": [0, 0, 0, 0],
            "shadowDirect": [0, 0, 0, 0, 0],
            "shadowReplay": [0, 0, 0],
        },
        "compute": {"jobs": [10, 10], "dispatch": [2, 2]},
        "P4Shadow": {"leasesConsumed": 0, "bypassCommits": 0},
        "forcedSnapshot": {
            "cleanPairVsShadowRoute": {
                "exact": True, "route": "compute",
                "replayCommitEqualityRequired": False,
            },
        },
    }
    vertex_diag = {
        "VSRoute": {
            "route": 1, "explicit": 1, "invalid": 0,
            "inputPrepared": [2, 144], "inputSubmitted": [2, 144],
            "main": [3, 1, 1, 1], "cleared": 1,
            "inputConsumers": [3, 2, 0],
            "shadowCapture": [1, 0, 0, 1],
            "shadowDirect": [7, 0, 0, 7, 7],
            "shadowReplay": [4, 3, 0],
        },
        "compute": {"jobs": [2, 2], "dispatch": [1, 1]},
        "P4Shadow": {"leasesConsumed": 1, "bypassCommits": 1},
        "forcedSnapshot": {
            "cleanPairVsShadowRoute": {
                "exact": True, "route": "vertex_shader",
                "replayCommitEqualityRequired": False,
            },
        },
    }
    input_only_diag = {
        "VSRoute": {
            "route": 2, "explicit": 1, "invalid": 0,
            "inputPrepared": [4, 288], "inputSubmitted": [4, 288],
            "main": [3, 0, 0, 3], "cleared": 3,
            "inputConsumers": [3, 4, 0],
            "inputOnly": [4, 288, 4, 4096, 3, 1],
            "shadowCapture": [3, 0, 0, 3],
            "shadowDirect": [7, 0, 0, 7, 7],
            "shadowReplay": [4, 3, 0],
        },
        "compute": {"jobs": [1, 1], "dispatch": [1, 1]},
        "P3": {
            "bypassAttempts": 4, "bypassFallbacks": 4,
            "bypassAuthorizations": 0, "bypassCommits": 0,
        },
        "kernel": {
            "originalCalls": 4, "originalBytes": 4096,
            "bypassedCalls": 0, "bypassedBytes": 0,
        },
        "nativePoison": {"create": 0, "hit": 0, "outstanding": 0},
        "nativeDirectDiscard": {
            "events": 0, "eventsWithPoison": 0, "rangesCleared": 0,
            "invalid": 0, "crossMapMerge": 0,
        },
        "indexTicket": {"attempts": 0, "exact": 0},
        "nativeKernelNormal": {
            "overflow": 0, "invalid": 0, "fault": 0,
            "ackMismatch": 0, "commitBlocked": 0,
        },
        "P4Shadow": {"leasesConsumed": 3, "bypassCommits": 0},
        "ledger": {
            "classified": 12, "resolved": 6, "consumed": 6,
            "cpuFallback": 0, "suppressed": 0, "leak": 0,
            "unreserved": 0, "duplicate": 0, "planMismatch": 0,
            "retireDeferred": 0,
        },
        "forcedSnapshot": {
            "attempted": True, "beginSeen": True, "endSeen": True,
            "blockOrderValid": True, "blockAttributionValid": True,
            "selectedBlockComplete": True, "quiescent": True,
            "twoCleanSnapshots": True, "progressValid": True,
            "cleanPairResourceDeltaClosed": True,
            "cleanPairVsShadowRoute": {
                "exact": True, "route": "vertex_shader_input_only",
                "replayCommitEqualityRequired": False,
            },
            "cleanPairOutsideNativeFastPathPolicy": {
                "present": True, "light": True,
                "poisonScanAttemptsDelta": 0,
                "poisonNoOverlapDelta": 0, "poisonOverlapDelta": 0,
                "poisonReadFailDelta": 0, "markerConflictsDelta": 0,
                "independentPinBeginsDelta": 0,
                "independentPinEndsDelta": 0,
                "coverDeltas": {"flush": 0, "semantic": 0},
                "directOriginalActiveEndpoints": {
                    "previous": 0, "current": 0,
                },
                "directOriginalDeltas": {
                    "directOriginalAttempts": 5,
                    "directOriginalKernelCalls": 5,
                    "directOriginalNormalReturns": 5,
                    "directOriginalKernelNoNormalReturns": 0,
                    "directOriginalCompleted": 5,
                    "directOriginalConflicts": 0,
                    "directOriginalCancellations": 0,
                    "directOriginalResetCompletedWhileActive": 0,
                    "directOriginalLatePoison": 0,
                },
            },
            "cleanPairNativePoisonO1Authority": {
                "present": True, "recognizedExact": True,
                "light": True, "sidecarEnabled": False,
                "authorizationAuthority": 0,
                "delta": {
                    "present": True, "monotonic": True, "valid": True,
                    "endpointsQuiescent": True, "allDeltasZero": True,
                    "authorizationAuthority": 0,
                },
            },
        },
    }
    bypass_diag = json.loads(json.dumps(input_only_diag))
    bypass_diag["VSRoute"]["route"] = 3
    bypass_diag["P3"] = {
        "bypassAttempts": 4, "bypassFallbacks": 1,
        "bypassAuthorizations": 3, "bypassCommits": 3,
    }
    bypass_diag["kernel"] = {
        "originalCalls": 1, "originalBytes": 1024,
        "bypassedCalls": 3, "bypassedBytes": 3072,
    }
    bypass_diag["nativePoison"] = {
        "create": 3, "hit": 3, "outstanding": 0,
    }
    bypass_diag["indexTicket"] = {"attempts": 3, "exact": 3}
    bypass_diag["P4Shadow"] = {
        "leasesConsumed": 3, "bypassCommits": 3,
    }
    bypass_diag["forcedSnapshot"]["cleanPairVsShadowRoute"] = {
        "exact": True, "route": "vertex_shader_bypass",
        "replayCommitEqualityRequired": False,
    }
    zero_byte_diag = json.loads(json.dumps(vertex_diag))
    zero_byte_diag["VSRoute"]["inputPrepared"] = [1, 0]
    zero_byte_diag["VSRoute"]["inputSubmitted"] = [1, 0]
    misaligned_diag = json.loads(json.dumps(vertex_diag))
    misaligned_diag["VSRoute"]["inputPrepared"] = [1, 49]
    misaligned_diag["VSRoute"]["inputSubmitted"] = [1, 49]
    main_only_mask_diag = json.loads(json.dumps(vertex_diag))
    main_only_mask_diag["VSRoute"]["inputConsumers"] = [1, 2, 0]
    input_consumer_mismatch_diag = json.loads(json.dumps(vertex_diag))
    input_consumer_mismatch_diag["VSRoute"]["inputConsumers"] = [3, 1, 1]
    compute_not_retained_diag = json.loads(json.dumps(vertex_diag))
    compute_not_retained_diag["compute"]["jobs"] = [1, 1]
    capture_reject_diag = json.loads(json.dumps(vertex_diag))
    capture_reject_diag["VSRoute"]["shadowCapture"] = [2, 1, 0, 1]
    capture_commit_mismatch_diag = json.loads(json.dumps(vertex_diag))
    capture_commit_mismatch_diag["P4Shadow"]["bypassCommits"] = 0
    direct_clear_mismatch_diag = json.loads(json.dumps(vertex_diag))
    direct_clear_mismatch_diag["VSRoute"]["shadowDirect"] = [7, 0, 0, 7, 6]
    replay_partition_mismatch_diag = json.loads(json.dumps(vertex_diag))
    replay_partition_mismatch_diag["VSRoute"]["shadowReplay"] = [3, 3, 0]
    unknown_replay_diag = json.loads(json.dumps(vertex_diag))
    unknown_replay_diag["VSRoute"]["shadowReplay"] = [4, 2, 1]
    missing_shadow_field_diag = json.loads(json.dumps(vertex_diag))
    del missing_shadow_field_diag["VSRoute"]["shadowDirect"]
    compute_leak_diag = json.loads(json.dumps(compute_diag))
    compute_leak_diag["VSRoute"]["shadowCapture"] = [1, 0, 0, 1]

    compute_gates = _evaluate_vs_route_gates(
        compute_diag, "compute", compute_launch,
    )
    vertex_gates = _evaluate_vs_route_gates(
        vertex_diag, "vertex_shader", vertex_launch,
    )
    input_only_gates = _evaluate_vs_route_gates(
        input_only_diag, "vertex_shader_input_only", input_only_launch,
    )
    bypass_gates = _evaluate_vs_route_gates(
        bypass_diag, "vertex_shader_bypass", bypass_launch,
    )
    bypass_missing_shadow_commit = json.loads(json.dumps(bypass_diag))
    bypass_missing_shadow_commit["P4Shadow"]["bypassCommits"] = 0
    bypass_missing_shadow_commit_gates = _evaluate_vs_route_gates(
        bypass_missing_shadow_commit,
        "vertex_shader_bypass", bypass_launch,
    )
    input_only_shadow_fallback_diag = json.loads(json.dumps(input_only_diag))
    input_only_shadow_fallback_diag["VSRoute"]["shadowCapture"] = [
        2, 0, 0, 2,
    ]
    input_only_shadow_fallback_diag["P4Shadow"]["leasesConsumed"] = 2
    input_only_shadow_fallback_diag["ledger"]["consumed"] = 5
    input_only_shadow_fallback_diag["ledger"]["cpuFallback"] = 1
    input_only_shadow_fallback_gates = _evaluate_vs_route_gates(
        input_only_shadow_fallback_diag,
        "vertex_shader_input_only", input_only_launch,
    )
    zero_byte_gates = _evaluate_vs_route_gates(
        zero_byte_diag, "vertex_shader", vertex_launch,
    )
    misaligned_gates = _evaluate_vs_route_gates(
        misaligned_diag, "vertex_shader", vertex_launch,
    )
    missing_environment_gates = _evaluate_vs_route_gates(
        vertex_diag, "vertex_shader", {},
    )
    main_only_mask_gates = _evaluate_vs_route_gates(
        main_only_mask_diag, "vertex_shader", vertex_launch,
    )
    input_consumer_mismatch_gates = _evaluate_vs_route_gates(
        input_consumer_mismatch_diag, "vertex_shader", vertex_launch,
    )
    compute_not_retained_gates = _evaluate_vs_route_gates(
        compute_not_retained_diag, "vertex_shader", vertex_launch,
    )
    capture_reject_gates = _evaluate_vs_route_gates(
        capture_reject_diag, "vertex_shader", vertex_launch,
    )
    capture_commit_mismatch_gates = _evaluate_vs_route_gates(
        capture_commit_mismatch_diag, "vertex_shader", vertex_launch,
    )
    direct_clear_mismatch_gates = _evaluate_vs_route_gates(
        direct_clear_mismatch_diag, "vertex_shader", vertex_launch,
    )
    replay_partition_mismatch_gates = _evaluate_vs_route_gates(
        replay_partition_mismatch_diag, "vertex_shader", vertex_launch,
    )
    unknown_replay_gates = _evaluate_vs_route_gates(
        unknown_replay_diag, "vertex_shader", vertex_launch,
    )
    missing_shadow_field_gates = _evaluate_vs_route_gates(
        missing_shadow_field_diag, "vertex_shader", vertex_launch,
    )
    compute_leak_gates = _evaluate_vs_route_gates(
        compute_leak_diag, "compute", compute_launch,
    )
    duplicate_contract = _forced_diag_block_contract("\n".join((
        "DXVK War3GpuSkin: diag vsRoute route=0 explicit=0 invalid=0 "
        "inputPrepared=0/0 inputSubmitted=0/0 main=0/0/0/0 cleared=0 "
        "inputConsumers=0/0/0 shadowCapture=0/0/0/0 "
        "shadowDirect=0/0/0/0/0 shadowReplay=0/0/0",
        "DXVK War3GpuSkin: diag vsRoute route=1 explicit=1 invalid=0 "
        "inputPrepared=1/48 inputSubmitted=1/48 main=1/0/0/1 cleared=1 "
        "inputConsumers=3/1/0 shadowCapture=1/0/0/1 "
        "shadowDirect=4/0/0/4/4 shadowReplay=4/0/0",
    )))
    parsed_route_line = (
        "DXVK War3GpuSkin: diag vsRoute route=1 explicit=1 invalid=0 "
        "inputPrepared=2/144 inputSubmitted=2/144 main=3/1/1/1 "
        "cleared=1 inputConsumers=3/2/0 shadowCapture=1/0/0/1 "
        "shadowDirect=7/0/0/7/7 shadowReplay=4/3/0"
    )
    parsed_route = _parse_gpu_skin_diag(parsed_route_line, {}).get(
        "VSRoute", {}
    )

    pair_previous = {
        "VSRoute": {
            "route": 1, "explicit": 1, "invalid": 0,
            "inputPrepared": [100, 288000],
            "inputSubmitted": [100, 288000],
            "main": [80, 0, 0, 80], "cleared": 80,
            "inputConsumers": [3, 100, 0],
            "shadowCapture": [80, 0, 0, 80],
            "shadowDirect": [400, 0, 0, 400, 400],
            "shadowReplay": [300, 100, 0],
        },
        "compute": {"jobs": [100, 100], "dispatch": [20, 20]},
        "P4Shadow": {"leasesConsumed": 80, "bypassCommits": 80},
    }
    pair_current = {
        "VSRoute": {
            "route": 1, "explicit": 1, "invalid": 0,
            "inputPrepared": [102, 293760],
            "inputSubmitted": [102, 293760],
            "main": [82, 0, 0, 82], "cleared": 82,
            "inputConsumers": [3, 102, 0],
            "shadowCapture": [82, 0, 0, 82],
            "shadowDirect": [414, 0, 0, 414, 414],
            "shadowReplay": [308, 106, 0],
        },
        "compute": {"jobs": [102, 102], "dispatch": [21, 21]},
        "P4Shadow": {"leasesConsumed": 82, "bypassCommits": 82},
    }
    input_only_pair_previous = json.loads(json.dumps(input_only_diag))
    input_only_pair_previous["VSRoute"].update({
        "inputPrepared": [100, 7200],
        "inputSubmitted": [100, 7200],
        "main": [80, 0, 0, 80],
        "cleared": 80,
        "inputConsumers": [3, 100, 0],
        "inputOnly": [100, 7200, 100, 102400, 80, 20],
        "shadowCapture": [80, 0, 0, 80],
        "shadowDirect": [400, 0, 0, 400, 400],
        "shadowReplay": [300, 100, 0],
    })
    input_only_pair_previous["compute"] = {
        "jobs": [10, 10], "dispatch": [3, 3],
    }
    input_only_pair_previous["P4Shadow"] = {
        "leasesConsumed": 80, "bypassCommits": 0,
    }
    input_only_pair_current = json.loads(json.dumps(input_only_pair_previous))
    input_only_pair_current["VSRoute"].update({
        "inputPrepared": [104, 7488],
        "inputSubmitted": [104, 7488],
        "main": [83, 0, 0, 83],
        "cleared": 83,
        "inputConsumers": [3, 104, 0],
        "inputOnly": [104, 7488, 104, 106496, 83, 21],
        "shadowCapture": [83, 0, 0, 83],
        "shadowDirect": [414, 0, 0, 414, 414],
        "shadowReplay": [308, 106, 0],
    })
    input_only_pair_current["compute"] = {
        "jobs": [11, 11], "dispatch": [4, 4],
    }
    input_only_pair_current["P4Shadow"] = {
        "leasesConsumed": 83, "bypassCommits": 0,
    }
    input_only_pair = _vs_shadow_route_pair_contract(
        input_only_pair_previous, input_only_pair_current,
    )
    bypass_pair_previous = json.loads(json.dumps(input_only_pair_previous))
    bypass_pair_current = json.loads(json.dumps(input_only_pair_current))
    bypass_pair_previous["VSRoute"]["route"] = 3
    bypass_pair_current["VSRoute"]["route"] = 3
    bypass_pair_previous["P4Shadow"]["bypassCommits"] = 80
    bypass_pair_current["P4Shadow"]["bypassCommits"] = 83
    bypass_pair = _vs_shadow_route_pair_contract(
        bypass_pair_previous, bypass_pair_current,
    )
    multi_replay_pair = _vs_shadow_route_pair_contract(
        pair_previous, pair_current,
    )
    pair_zero_direct_progress = json.loads(json.dumps(pair_current))
    pair_zero_direct_progress["VSRoute"]["shadowDirect"] = [
        400, 0, 0, 400, 400,
    ]
    pair_zero_direct_progress["VSRoute"]["shadowReplay"] = [300, 100, 0]
    zero_direct_progress_pair = _vs_shadow_route_pair_contract(
        pair_previous, pair_zero_direct_progress,
    )
    pair_capture_reject = json.loads(json.dumps(pair_current))
    pair_capture_reject["VSRoute"]["shadowCapture"] = [83, 1, 0, 82]
    capture_reject_pair = _vs_shadow_route_pair_contract(
        pair_previous, pair_capture_reject,
    )
    pair_replay_mismatch = json.loads(json.dumps(pair_current))
    pair_replay_mismatch["VSRoute"]["shadowReplay"] = [307, 106, 0]
    replay_mismatch_pair = _vs_shadow_route_pair_contract(
        pair_previous, pair_replay_mismatch,
    )
    pair_counter_decrease = json.loads(json.dumps(pair_current))
    pair_counter_decrease["VSRoute"]["shadowDirect"] = [399, 0, 0, 399, 399]
    decreasing_pair = _vs_shadow_route_pair_contract(
        pair_previous, pair_counter_decrease,
    )
    compute_cold_pair = _vs_shadow_route_pair_contract(
        compute_diag, json.loads(json.dumps(compute_diag)),
    )
    compute_resource_delta = _gpu_skin_resource_delta_closed(
        100, 168, 0, 0, 200, 268, 0, 0,
    )
    vertex_resource_delta = _gpu_skin_resource_delta_closed(
        100, 168, 50, 118, 200, 336, 0, 0,
    )
    vertex_legacy_single_lease_delta = _gpu_skin_resource_delta_closed(
        100, 168, 50, 118, 200, 268, 0, 0,
    )
    multi_replay_resource_delta = _gpu_skin_resource_delta_closed(
        100, 102, 50, 52, 200, 204, 0, 0,
    )
    replay_counted_as_retirement_delta = _gpu_skin_resource_delta_closed(
        100, 102, 50, 52, 200, 218, 0, 0,
    )
    input_only_resource_delta = _gpu_skin_resource_delta_closed(
        100, 104, 50, 54, 200, 204, 0, 0, 100, 104,
    )

    checks = {
        "computeExact": all(
            compute_gates.get(name) is True
            for name in VS_ROUTE_HARD_GATE_NAMES
        ),
        "vertexExact": all(
            vertex_gates.get(name) is True
            for name in VS_ROUTE_HARD_GATE_NAMES
        ),
        "inputOnlyExact": all(
            input_only_gates.get(name) is True
            for name in VS_ROUTE_HARD_GATE_NAMES
        ),
        "bypassExact": all(
            bypass_gates.get(name) is True
            for name in VS_ROUTE_HARD_GATE_NAMES
        ),
        "bypassMissingShadowCommitRejected": (
            bypass_missing_shadow_commit_gates.get(
                "vsRouteShadowCaptureContract"
            ) is False
            and bypass_missing_shadow_commit_gates.get(
                "vsRouteP4AuthorityContract"
            ) is False
        ),
        "inputOnlyShadowCpuFallbackExact": all(
            input_only_shadow_fallback_gates.get(name) is True
            for name in VS_ROUTE_HARD_GATE_NAMES
        ),
        "inputOnlyPairExact": input_only_pair.get("exact") is True,
        "bypassPairExact": (
            bypass_pair.get("exact") is True
            and bypass_pair.get("route") == "vertex_shader_bypass"
        ),
        "zeroBytesRejected": (
            zero_byte_gates.get("vsRouteInputContract") is False
        ),
        "misalignedBytesRejected": (
            misaligned_gates.get("vsRouteInputContract") is False
        ),
        "missingEffectiveEnvironmentRejected": (
            missing_environment_gates.get("vsRouteEnvironmentExact") is False
        ),
        "mainOnlyInputMaskRejected": (
            main_only_mask_gates.get(
                "vsRouteInputConsumerContract"
            ) is False
        ),
        "inputConsumerMismatchRejected": (
            input_consumer_mismatch_gates.get(
                "vsRouteInputConsumerContract"
            ) is False
        ),
        "computeRetentionMismatchRejected": (
            compute_not_retained_gates.get("vsRouteComputeRetained") is False
        ),
        "shadowCaptureRejectRejected": (
            capture_reject_gates.get(
                "vsRouteShadowCaptureContract"
            ) is False
        ),
        "shadowCaptureCommitMismatchRejected": (
            capture_commit_mismatch_gates.get(
                "vsRouteShadowCaptureContract"
            ) is False
        ),
        "shadowDirectClearMismatchRejected": (
            direct_clear_mismatch_gates.get(
                "vsRouteShadowDirectContract"
            ) is False
        ),
        "shadowReplayPartitionMismatchRejected": (
            replay_partition_mismatch_gates.get(
                "vsRouteShadowDirectContract"
            ) is False
        ),
        "shadowUnknownReplayRejected": (
            unknown_replay_gates.get(
                "vsRouteShadowDirectContract"
            ) is False
        ),
        "missingShadowFieldRejected": (
            missing_shadow_field_gates.get(
                "vsRouteShadowDirectContract"
            ) is False
        ),
        "computeVsFieldLeakRejected": (
            compute_leak_gates.get(
                "vsRouteShadowCaptureContract"
            ) is False
        ),
        "routeRegexFieldsExact": (
            tuple(parsed_route.get("inputConsumers", ())) == (3, 2, 0)
            and tuple(parsed_route.get("shadowCapture", ())) ==
                (1, 0, 0, 1)
            and tuple(parsed_route.get("shadowDirect", ())) ==
                (7, 0, 0, 7, 7)
            and tuple(parsed_route.get("shadowReplay", ())) == (4, 3, 0)
        ),
        "duplicateVsRouteRejected": (
            duplicate_contract.get("violations", {}).get("vsRoute") == 2
        ),
        "computeInheritedValueRemoved": (
            GPU_SKIN_EXECUTION_ROUTE_ENV not in
            compute_launch.get("effectiveWar3Environment", {})
        ),
        "vertexOverrideExact": (
            vertex_launch.get("effectiveWar3Environment", {}).get(
                GPU_SKIN_EXECUTION_ROUTE_ENV
            ) == "vertex_shader"
        ),
        "inputOnlyOverrideExact": (
            input_only_launch.get("effectiveWar3Environment", {}).get(
                GPU_SKIN_EXECUTION_ROUTE_ENV
            ) == "vertex_shader_input_only"
        ),
        "bypassOverrideExact": (
            bypass_launch.get("effectiveWar3Environment", {}).get(
                GPU_SKIN_EXECUTION_ROUTE_ENV
            ) == "vertex_shader_bypass"
        ),
        "hostEnvironmentRestored": (
            compute_host_restored and vertex_host_restored
            and input_only_host_restored and bypass_host_restored
        ),
        "computeResourceDeltaExact": compute_resource_delta,
        "vertexResourceDeltaExact": vertex_resource_delta,
        "vertexLegacySingleLeaseDeltaRejected": (
            vertex_legacy_single_lease_delta is False
        ),
        "multiReplayDoesNotAddResourceLeases": (
            multi_replay_resource_delta is True
            and replay_counted_as_retirement_delta is False
        ),
        "inputOnlyResourceDeltaExact": input_only_resource_delta,
        "computeColdPairExact": (
            compute_cold_pair.get("exact") is True
            and compute_cold_pair.get("route") == "compute"
        ),
        "multiReplayPairExactWithoutOneToOne": (
            multi_replay_pair.get("exact") is True
            and multi_replay_pair.get(
                "replayCommitEqualityRequired"
            ) is False
            and multi_replay_pair.get("deltas", {}).get(
                "shadowCapture"
            ) == [2, 0, 0, 2]
            and multi_replay_pair.get("deltas", {}).get(
                "shadowDirect"
            ) == [14, 0, 0, 14, 14]
        ),
        "zeroDirectProgressPairExact": (
            zero_direct_progress_pair.get("exact") is True
            and zero_direct_progress_pair.get("directContract") is True
            and zero_direct_progress_pair.get("deltas", {}).get(
                "shadowDirect"
            ) == [0, 0, 0, 0, 0]
            and zero_direct_progress_pair.get("deltas", {}).get(
                "shadowReplay"
            ) == [0, 0, 0]
        ),
        "pairCaptureRejectRejected": (
            capture_reject_pair.get("exact") is False
        ),
        "pairReplayMismatchRejected": (
            replay_mismatch_pair.get("exact") is False
        ),
        "pairCounterDecreaseRejected": (
            decreasing_pair.get("exact") is False
            and decreasing_pair.get("allMonotonic") is False
        ),
    }
    result = {"ok": all(checks.values()), "checks": checks}
    if not result["ok"]:
        raise AssertionError(
            "GPU 蒙皮执行路线纯合成自测失败："
            + json.dumps(result, ensure_ascii=False, sort_keys=True)
        )
    return result


def _hard_gate_names(
    phase: str, require_outline_all: bool = False,
    diagnostics: str = "full",
    execution_route: str = "compute",
) -> Tuple[str, ...]:
    if diagnostics not in DIAGNOSTIC_HARD_GATE_NAMES:
        raise ValueError(f"unsupported diagnostics hard-gate mode: {diagnostics}")
    names = HARD_GATE_NAMES["base"] + DIAGNOSTIC_HARD_GATE_NAMES[diagnostics]
    if execution_route == "vertex_shader_input_only":
        names = tuple(
            name for name in names
            if name not in VS_INPUT_ONLY_P4_EXERCISE_GATE_NAMES
        )
    if phase == "lifecycle":
        names += HARD_GATE_NAMES["lifecycle"]
    if require_outline_all:
        names += (
            "outlineControlApplied",
            "outlineControlRestored",
            "outlineMaterialReady",
            "outlineSingleStageActivationClean",
            "outlineSubmittedPositive",
            "outlineSameSliceExact",
        )
    return names


def _hard_gate_pass(
    gates: Dict[str, Any], phase: str, require_outline_all: bool = False,
    diagnostics: str = "full",
    execution_route: str = "compute",
) -> bool:
    return all(
        bool(gates.get(name, False))
        for name in _hard_gate_names(
            phase, require_outline_all, diagnostics, execution_route,
        )
    )


def _runtime_poll(
    pid: int, elapsed_sec: float,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    status = read_runtime_status(war3_dir=str(WAR3_DIR), pid=pid)
    data = dict(status.get("data", {}) or {})
    runtime = dict(data.get("runtime", {}) or {})
    process_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    return {
        "elapsedSec": round(elapsed_sec, 3),
        "running": process_authority.get("aliveExact") is True,
        "terminatedExact": (
            process_authority.get("terminatedExact") is True
        ),
        "processIdentityIndeterminate": (
            process_authority.get("indeterminate") is True
        ),
        "processAuthority": process_authority,
        "statusOk": bool(
            status.get("ok")
            and process_authority.get("aliveExact") is True
        ),
        "statusOkReportOnly": bool(status.get("ok")),
        "statusMode": status.get("mode", "file"),
        "frameIndex": runtime.get("frameIndex", data.get("frameIndex")),
        "isInGame": runtime.get("gameStarted", data.get("gameStarted")),
        "timestampMs": data.get("timestampMs"),
        "data": data,
    }


def _scan_crash_text(text: str) -> List[str]:
    return [line for line in _strip_ansi(text).splitlines() if CRASH_RE.search(line)][-100:]


def _powershell_json(
    script: str,
    timeout_sec: int,
    executable: str = "powershell",
) -> Dict[str, Any]:
    query = _run(
        [executable, "-NoProfile", "-NonInteractive", "-Command", script],
        timeout_sec=timeout_sec,
    )
    payload: Any = None
    parse_error = ""
    output = str(query.get("output", "") or "").strip()
    if output:
        for candidate in reversed(output.splitlines()):
            try:
                payload = json.loads(candidate)
                break
            except json.JSONDecodeError as exc:
                parse_error = str(exc)
    return {
        "ok": query.get("returncode") == 0 and payload is not None,
        "payload": payload,
        "parseError": parse_error if payload is None else "",
        "query": query,
    }


def _process_snapshot_powershell_script(
    pid: int, module_backend: str,
) -> str:
    return rf'''
$ErrorActionPreference = "SilentlyContinue"
$targetPid = {int(pid)}
$process = Get-Process -Id $targetPid -ErrorAction SilentlyContinue
if ($null -eq $process) {{
  [ordered]@{{
    found = $false
    pid = $targetPid
    hasExited = $true
    exitCode = $null
    exitCodeAvailable = $false
    exitCodeUnavailableReason = "process no longer queryable; launcher did not retain a process handle"
    moduleEnumerationBackend = "{module_backend}"
    moduleEnumerationComplete = $false
    moduleEnumerationError = "process no longer queryable"
    modules = @()
  }} | ConvertTo-Json -Depth 6 -Compress
  exit 0
}}
$modules = @()
$moduleError = ""
$moduleComplete = $false
try {{
  $modules = @($process.Modules | ForEach-Object {{
    [ordered]@{{
      name = [string]$_.ModuleName
      path = [string]$_.FileName
      baseAddress = ("0x{{0:X}}" -f [Int64]$_.BaseAddress)
      sizeBytes = [Int64]$_.ModuleMemorySize
      fileVersion = [string]$_.FileVersionInfo.FileVersion
    }}
  }})
  $moduleComplete = $true
}} catch {{
  $moduleError = [string]$_.Exception.Message
}}
$hasExited = $false
try {{ $hasExited = [bool]$process.HasExited }} catch {{ $hasExited = $true }}
$exitCode = $null
$exitCodeAvailable = $false
if ($hasExited) {{
  try {{
    $exitCode = [int]$process.ExitCode
    $exitCodeAvailable = $true
  }} catch {{}}
}}
$startTimeUtc = ""
$startTimeEpochMs = 0
try {{
  $startTime = $process.StartTime.ToUniversalTime()
  $startTimeUtc = $startTime.ToString("o")
  $startTimeEpochMs = [DateTimeOffset]::new($startTime).ToUnixTimeMilliseconds()
}} catch {{}}
[ordered]@{{
  found = $true
  pid = [int]$process.Id
  processName = [string]$process.ProcessName
  path = [string]$process.Path
  hasExited = $hasExited
  exitCode = $exitCode
  exitCodeAvailable = $exitCodeAvailable
  exitCodeUnavailableReason = $(if ($exitCodeAvailable) {{ "" }} elseif ($hasExited) {{ "exit code query failed" }} else {{ "process was still active at capture time" }})
  responding = [bool]$process.Responding
  startTimeUtc = $startTimeUtc
  startTimeEpochMs = [Int64]$startTimeEpochMs
  cpuSeconds = [double]$process.CPU
  workingSetBytes = [Int64]$process.WorkingSet64
  privateMemoryBytes = [Int64]$process.PrivateMemorySize64
  handleCount = [int]$process.HandleCount
  threadCount = [int]$process.Threads.Count
  mainWindowHandle = ("0x{{0:X}}" -f [Int64]$process.MainWindowHandle)
  mainWindowTitle = [string]$process.MainWindowTitle
  moduleCount = [int]$modules.Count
  moduleError = $moduleError
  moduleEnumerationBackend = "{module_backend}"
  moduleEnumerationComplete = [bool]$moduleComplete
  moduleEnumerationError = $moduleError
  modules = $modules
}} | ConvertTo-Json -Depth 6 -Compress
'''


def _process_snapshot_toolhelp_script(pid: int) -> str:
    return rf'''
$ErrorActionPreference = "SilentlyContinue"
$targetPid = {int(pid)}
$process = Get-Process -Id $targetPid -ErrorAction SilentlyContinue
if ($null -eq $process) {{
  [ordered]@{{
    found = $false
    pid = $targetPid
    hasExited = $true
    exitCode = $null
    exitCodeAvailable = $false
    exitCodeUnavailableReason = "process no longer queryable; launcher did not retain a process handle"
    moduleEnumerationBackend = "toolhelp32-snapshot"
    moduleEnumerationComplete = $false
    moduleEnumerationError = "process no longer queryable"
    moduleEnumerationTerminalError = $null
    modules = @()
  }} | ConvertTo-Json -Depth 6 -Compress
  exit 0
}}
$source = @"
using System;
using System.Runtime.InteropServices;

public static class War3Toolhelp32 {{
  public const uint TH32CS_SNAPMODULE = 0x00000008;
  public const uint TH32CS_SNAPMODULE32 = 0x00000010;

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct MODULEENTRY32 {{
    public uint dwSize;
    public uint th32ModuleID;
    public uint th32ProcessID;
    public uint GlblcntUsage;
    public uint ProccntUsage;
    public IntPtr modBaseAddr;
    public uint modBaseSize;
    public IntPtr hModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
    public string szModule;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
    public string szExePath;
  }}

  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint pid);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool Module32FirstW(IntPtr snapshot, ref MODULEENTRY32 entry);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool Module32NextW(IntPtr snapshot, ref MODULEENTRY32 entry);
  [DllImport("kernel32.dll", SetLastError = true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool CloseHandle(IntPtr handle);
}}
"@
$modules = @()
$moduleError = ""
$moduleComplete = $false
$moduleTerminalError = $null
try {{
  Add-Type -TypeDefinition $source -ErrorAction Stop
  $flags = [War3Toolhelp32]::TH32CS_SNAPMODULE -bor [War3Toolhelp32]::TH32CS_SNAPMODULE32
  $snapshot = [War3Toolhelp32]::CreateToolhelp32Snapshot($flags, [uint32]$targetPid)
  if ($snapshot.ToInt64() -eq -1) {{
    throw "CreateToolhelp32Snapshot failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
  }}
  try {{
    $entry = New-Object 'War3Toolhelp32+MODULEENTRY32'
    $entry.dwSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf($entry)
    $more = [War3Toolhelp32]::Module32FirstW($snapshot, [ref]$entry)
    if (-not $more) {{
      $moduleTerminalError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    }}
    while ($more) {{
      $modules += [ordered]@{{
        name = [string]$entry.szModule
        path = [string]$entry.szExePath
        baseAddress = ("0x{{0:X}}" -f $entry.modBaseAddr.ToInt64())
        sizeBytes = [Int64]$entry.modBaseSize
        fileVersion = ""
      }}
      $entry.dwSize = [uint32][Runtime.InteropServices.Marshal]::SizeOf($entry)
      $more = [War3Toolhelp32]::Module32NextW($snapshot, [ref]$entry)
      if (-not $more) {{
        $moduleTerminalError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      }}
    }}
    if ($moduleTerminalError -ne {TOOLHELP_ERROR_NO_MORE_FILES}) {{
      throw "Module32FirstW/NextW terminal error: $moduleTerminalError (expected {TOOLHELP_ERROR_NO_MORE_FILES})"
    }}
    $moduleComplete = $true
  }} finally {{
    [void][War3Toolhelp32]::CloseHandle($snapshot)
  }}
}} catch {{
  $moduleError = [string]$_.Exception.Message
}}
$hasExited = $false
try {{ $hasExited = [bool]$process.HasExited }} catch {{ $hasExited = $true }}
$exitCode = $null
$exitCodeAvailable = $false
if ($hasExited) {{
  try {{
    $exitCode = [int]$process.ExitCode
    $exitCodeAvailable = $true
  }} catch {{}}
}}
$startTimeUtc = ""
$startTimeEpochMs = 0
try {{
  $startTime = $process.StartTime.ToUniversalTime()
  $startTimeUtc = $startTime.ToString("o")
  $startTimeEpochMs = [DateTimeOffset]::new($startTime).ToUnixTimeMilliseconds()
}} catch {{}}
$processPath = ""
try {{ $processPath = [string]$process.Path }} catch {{}}
[ordered]@{{
  found = $true
  pid = [int]$process.Id
  processName = [string]$process.ProcessName
  path = $processPath
  hasExited = $hasExited
  exitCode = $exitCode
  exitCodeAvailable = $exitCodeAvailable
  exitCodeUnavailableReason = $(if ($exitCodeAvailable) {{ "" }} elseif ($hasExited) {{ "exit code query failed" }} else {{ "process was still active at capture time" }})
  responding = [bool]$process.Responding
  startTimeUtc = $startTimeUtc
  startTimeEpochMs = [Int64]$startTimeEpochMs
  cpuSeconds = [double]$process.CPU
  workingSetBytes = [Int64]$process.WorkingSet64
  privateMemoryBytes = [Int64]$process.PrivateMemorySize64
  handleCount = [int]$process.HandleCount
  threadCount = [int]$process.Threads.Count
  mainWindowHandle = ("0x{{0:X}}" -f [Int64]$process.MainWindowHandle)
  mainWindowTitle = [string]$process.MainWindowTitle
  moduleCount = [int]$modules.Count
  moduleError = $moduleError
  moduleEnumerationBackend = "toolhelp32-snapshot"
  moduleEnumerationComplete = [bool]$moduleComplete
  moduleEnumerationError = $moduleError
  moduleEnumerationTerminalError = $moduleTerminalError
  modules = $modules
}} | ConvertTo-Json -Depth 6 -Compress
'''


def _parse_utc_epoch_ms(value: Any) -> Optional[int]:
    text = str(value or "").strip()
    if not text:
        return None
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        return None
    return int(parsed.astimezone(timezone.utc).timestamp() * 1000.0)


def _war3_process_identity(name: Any, path: Any = "") -> bool:
    candidates = (
        os.path.basename(str(name or "")).strip().lower(),
        os.path.basename(str(path or "")).strip().lower(),
    )
    return any(candidate in ("war3", "war3.exe") for candidate in candidates)


def _canonical_windows_path(value: Any) -> str:
    text = str(value or "").strip().strip('"')
    if not text:
        return ""
    return os.path.normcase(os.path.normpath(os.path.abspath(text)))


def _capture_process_identity_snapshot(pid: int) -> Dict[str, Any]:
    target_pid = int(pid)
    payload: Dict[str, Any] = {
        "found": False,
        "pid": target_pid,
        "processName": "",
        "path": "",
        "startTimeUtc": "",
        "startTimeEpochMs": 0,
    }
    result: Dict[str, Any] = {
        "ok": False,
        "payload": payload,
        "backend": "kernel32-process-identity",
        "error": "",
    }
    if target_pid <= 0:
        result["error"] = "invalid target PID"
        return result
    process_query_limited_information = 0x1000
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (
        wintypes.DWORD, wintypes.BOOL, wintypes.DWORD,
    )
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.GetProcessTimes.argtypes = (
        wintypes.HANDLE,
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME),
    )
    kernel32.GetProcessTimes.restype = wintypes.BOOL
    kernel32.QueryFullProcessImageNameW.argtypes = (
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    )
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel32.CloseHandle.restype = wintypes.BOOL
    handle = kernel32.OpenProcess(
        process_query_limited_information, False, target_pid,
    )
    if not handle:
        result["error"] = (
            f"OpenProcess failed: {ctypes.get_last_error()}"
        )
        return result
    try:
        creation = wintypes.FILETIME()
        exit_time = wintypes.FILETIME()
        kernel_time = wintypes.FILETIME()
        user_time = wintypes.FILETIME()
        if not kernel32.GetProcessTimes(
            handle, ctypes.byref(creation), ctypes.byref(exit_time),
            ctypes.byref(kernel_time), ctypes.byref(user_time),
        ):
            result["error"] = (
                f"GetProcessTimes failed: {ctypes.get_last_error()}"
            )
            return result
        path_buffer = ctypes.create_unicode_buffer(32768)
        path_length = wintypes.DWORD(len(path_buffer))
        if not kernel32.QueryFullProcessImageNameW(
            handle, 0, path_buffer, ctypes.byref(path_length),
        ):
            result["error"] = (
                "QueryFullProcessImageNameW failed: "
                f"{ctypes.get_last_error()}"
            )
            return result
        creation_ticks = (
            (int(creation.dwHighDateTime) << 32)
            | int(creation.dwLowDateTime)
        )
        windows_to_unix_100ns = 116_444_736_000_000_000
        creation_epoch_ms = max(
            0,
            (creation_ticks - windows_to_unix_100ns) // 10_000,
        )
        process_path = str(path_buffer.value)
        payload.update({
            "found": True,
            "pid": target_pid,
            "processName": os.path.basename(process_path),
            "path": process_path,
            "startTimeUtc": (
                datetime.fromtimestamp(
                    creation_epoch_ms / 1000.0, timezone.utc,
                ).isoformat().replace("+00:00", "Z")
                if creation_epoch_ms > 0 else ""
            ),
            "startTimeEpochMs": int(creation_epoch_ms),
        })
        result["ok"] = bool(process_path and creation_epoch_ms > 0)
        if not result["ok"]:
            result["error"] = "incomplete native process identity"
        return result
    finally:
        kernel32.CloseHandle(handle)


def _launch_instance_binding_contract(
    fingerprint: Dict[str, Any],
    current_identity: Dict[str, Any],
    target_pid: int,
) -> Dict[str, Any]:
    payload_value = current_identity.get("payload")
    payload = dict(payload_value) if isinstance(payload_value, dict) else {}
    fingerprint_pid = fingerprint.get("pid")
    fingerprint_creation = fingerprint.get("creationEpochMs")
    fingerprint_path = _canonical_windows_path(
        fingerprint.get("canonicalExePath")
    )
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    fingerprint_shape_exact = bool(
        fingerprint.get("available") is True
        and fingerprint.get("launcherMode") == "direct"
        and isinstance(fingerprint_pid, int)
        and not isinstance(fingerprint_pid, bool)
        and fingerprint_pid == target_pid
        and isinstance(fingerprint_creation, int)
        and not isinstance(fingerprint_creation, bool)
        and fingerprint_creation > 0
        and fingerprint_path == expected_path
    )
    raw_current_pid = payload.get("pid")
    current_pid_exact = bool(
        isinstance(raw_current_pid, int)
        and not isinstance(raw_current_pid, bool)
        and raw_current_pid == target_pid
    )
    raw_current_creation = payload.get("startTimeEpochMs")
    current_creation_exact = bool(
        isinstance(raw_current_creation, int)
        and not isinstance(raw_current_creation, bool)
        and isinstance(fingerprint_creation, int)
        and raw_current_creation == fingerprint_creation
    )
    current_path = _canonical_windows_path(payload.get("path"))
    current_path_exact = bool(
        current_path and current_path == fingerprint_path == expected_path
    )
    current_identity_war3 = _war3_process_identity(
        payload.get("processName"), payload.get("path"),
    )
    current_exact = bool(
        current_identity.get("ok") is True
        and payload.get("found") is True
        and fingerprint_shape_exact and current_pid_exact
        and current_creation_exact and current_path_exact
        and current_identity_war3
    )
    # Never retain the caller-owned identity container here.  Module snapshot
    # validation passes its mutable ``snapshot`` as ``current_identity.payload``
    # and subsequently publishes this binding back into that same snapshot.
    # Keeping the original container would therefore form
    # snapshot -> moduleEvidence -> binding -> currentIdentity -> snapshot and
    # make every final artifact non-serializable.  The binding only needs the
    # primitive identity fields below; freeze those values explicitly.
    current_identity_report = {
        "ok": current_identity.get("ok") is True,
        "found": payload.get("found") is True,
        "pid": _coerce_pid(raw_current_pid),
        "processName": str(payload.get("processName", "") or ""),
        "canonicalExePath": current_path,
        "startTimeEpochMs": (
            raw_current_creation
            if isinstance(raw_current_creation, int)
            and not isinstance(raw_current_creation, bool)
            else None
        ),
    }
    return {
        "exact": current_exact,
        "fingerprintAvailable": fingerprint.get("available") is True,
        "fingerprintShapeExact": fingerprint_shape_exact,
        "targetPid": target_pid,
        "fingerprintPid": fingerprint_pid,
        "currentPid": _coerce_pid(raw_current_pid),
        "numericPidExact": current_pid_exact,
        "fingerprintCreationEpochMs": fingerprint_creation,
        "currentCreationEpochMs": raw_current_creation,
        "creationEpochExact": current_creation_exact,
        "expectedCanonicalExePath": expected_path,
        "fingerprintCanonicalExePath": fingerprint_path,
        "currentCanonicalExePath": current_path,
        "canonicalExePathExact": current_path_exact,
        "currentIdentityWar3": current_identity_war3,
        "launcherModeExact": fingerprint.get("launcherMode") == "direct",
        "currentIdentity": current_identity_report,
        "reportOnly": not current_exact,
        "failureClassificationAuthority": 0 if not current_exact else 1,
    }


def _freeze_launch_instance_fingerprint(
    pid: int, launch_result: Dict[str, Any], launch_epoch_ms: int,
) -> Dict[str, Any]:
    # Copy primitive STATE fields immediately. Never retain the mutable STATE
    # object because lifecycle relaunch overwrites it.
    state_copy = {
        "war3Pid": STATE.war3_pid,
        "launcherPid": STATE.launcher_pid,
        "launcherCreatedAtMs": STATE.launcher_created_at_ms,
        "launcherMode": str(STATE.launcher_mode or ""),
        "launcherExe": str(STATE.launcher_exe or ""),
        "stateLaunchEpochMsReportOnly": STATE.launch_epoch_ms,
    }
    state_native = getattr(STATE, "retained_native_process", None)
    try:
        state_native_snapshot = (
            dict(state_native.snapshot())
            if state_native is not None else {}
        )
    except Exception as exc:
        state_native_snapshot = {
            "available": False,
            "error": f"{type(exc).__name__}: {exc}",
        }
    launch_native_snapshot = dict(
        launch_result.get("nativeProcessWitness", {}) or {}
    )
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    state_created = state_copy["launcherCreatedAtMs"]
    preliminary_available = bool(
        isinstance(pid, int) and not isinstance(pid, bool) and pid > 0
        and isinstance(state_copy["war3Pid"], int)
        and not isinstance(state_copy["war3Pid"], bool)
        and state_copy["war3Pid"] == pid
        and isinstance(state_copy["launcherPid"], int)
        and not isinstance(state_copy["launcherPid"], bool)
        and state_copy["launcherPid"] == pid
        and isinstance(state_created, int)
        and not isinstance(state_created, bool) and state_created > 0
        and state_copy["launcherMode"] == "direct"
        and _canonical_windows_path(state_copy["launcherExe"]) == expected_path
        and launch_result.get("launcherMode") == "direct"
        and _coerce_pid(launch_result.get("pid")) == pid
        and _coerce_pid(launch_result.get("launcherPid")) == pid
        and _canonical_windows_path(launch_result.get("launcherExe")) ==
            expected_path
        and state_native_snapshot.get("available") is True
        and state_native_snapshot.get("ownsNativeHandle") is True
        and state_native_snapshot.get("closed") is False
        and state_native_snapshot.get("pid") == pid
        and state_native_snapshot.get("creationEpochMs") == state_created
        and _canonical_windows_path(
            state_native_snapshot.get("canonicalExePath")
        ) == expected_path
        and launch_native_snapshot.get("available") is True
        and launch_native_snapshot.get("ownsNativeHandle") is True
        and launch_native_snapshot.get("closed") is False
        and launch_native_snapshot.get("pid") == pid
        and launch_native_snapshot.get("creationEpochMs") == state_created
        and _canonical_windows_path(
            launch_native_snapshot.get("canonicalExePath")
        ) == expected_path
    )
    fingerprint: Dict[str, Any] = {
        "available": preliminary_available,
        "immutableCopy": True,
        "capturedBeforeReadyWait": True,
        "pid": pid,
        "creationEpochMs": state_created if isinstance(state_created, int) else 0,
        "canonicalExePath": expected_path,
        "launcherMode": state_copy["launcherMode"],
        "runnerLaunchEpochMs": launch_epoch_ms,
        "stateCopy": dict(state_copy),
        "stateNativeProcessWitness": state_native_snapshot,
        "launchResultCopy": {
            "pid": launch_result.get("pid"),
            "gamePid": launch_result.get("gamePid"),
            "launcherPid": launch_result.get("launcherPid"),
            "launcherMode": launch_result.get("launcherMode"),
            "launcherExe": launch_result.get("launcherExe"),
            "nativeProcessWitness": launch_native_snapshot,
        },
    }
    immediate_identity = _capture_process_identity_snapshot(pid)
    immediate_binding = _launch_instance_binding_contract(
        fingerprint, immediate_identity, pid,
    )
    fingerprint["available"] = bool(
        preliminary_available and immediate_binding.get("exact") is True
    )
    fingerprint["immediateBinding"] = immediate_binding
    fingerprint["unavailableReasons"] = [
        name for name, clean in (
            ("stateOrLaunchShape", preliminary_available),
            (
                "retainedNativeProcessWitness",
                state_native_snapshot.get("available") is True
                and launch_native_snapshot.get("available") is True,
            ),
            ("immediateProcessBinding", immediate_binding.get("exact") is True),
        ) if not clean
    ]
    return json.loads(json.dumps(fingerprint, ensure_ascii=False))


def _freeze_launch_cleanup_capability(
    pid: int, launch_result: Dict[str, Any], label: str,
) -> Dict[str, Any]:
    """Freeze cleanup identity before any query, JSON write, or duplicate."""
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    launch_native = dict(
        launch_result.get("nativeProcessWitness", {}) or {}
    )
    state_native = getattr(STATE, "retained_native_process", None)
    state_snapshot: Dict[str, Any] = {}
    snapshot_error = ""
    try:
        if state_native is not None:
            state_snapshot = dict(state_native.snapshot())
    except Exception as exc:
        snapshot_error = f"{type(exc).__name__}: {exc}"
    creation = launch_native.get("creationEpochMs")
    available = bool(
        isinstance(pid, int) and not isinstance(pid, bool) and pid > 0
        and launch_result.get("ok") is True
        and launch_result.get("launcherMode") == "direct"
        and _coerce_pid(launch_result.get("pid")) == pid
        and _coerce_pid(launch_result.get("launcherPid")) == pid
        and _canonical_windows_path(
            launch_result.get("launcherExe")
        ) == expected_path
        and launch_native.get("available") is True
        and launch_native.get("ownsNativeHandle") is True
        and launch_native.get("closed") is False
        and launch_native.get("pid") == pid
        and isinstance(creation, int)
        and not isinstance(creation, bool) and creation > 0
        and _canonical_windows_path(
            launch_native.get("canonicalExePath")
        ) == expected_path
        and STATE.war3_pid == pid
        and STATE.launcher_pid == pid
        and STATE.launcher_created_at_ms == creation
        and str(STATE.launcher_mode or "") == "direct"
        and _canonical_windows_path(STATE.launcher_exe) == expected_path
        and state_snapshot.get("available") is True
        and state_snapshot.get("ownsNativeHandle") is True
        and state_snapshot.get("closed") is False
        and state_snapshot.get("pid") == pid
        and state_snapshot.get("creationEpochMs") == creation
        and _canonical_windows_path(
            state_snapshot.get("canonicalExePath")
        ) == expected_path
    )
    return {
        "available": available,
        "immutableCopy": True,
        "capturedBeforeIdentityQuery": True,
        "capturedBeforeJsonWrite": True,
        "capturedBeforeRunnerDuplicate": True,
        "label": str(label),
        "pid": int(pid),
        "creationEpochMs": (
            int(creation)
            if isinstance(creation, int)
            and not isinstance(creation, bool) else 0
        ),
        "canonicalExePath": expected_path,
        "launcherMode": "direct",
        "stateNativeProcessWitness": state_snapshot,
        "launchNativeProcessWitness": launch_native,
        "snapshotError": snapshot_error,
        "failureClassificationAuthority": 1 if available else 0,
    }


def _exact_handle_cleanup_stop(
    pid: int,
    cleanup_capability: Optional[Dict[str, Any]],
    retained_witness: Optional[Dict[str, Any]],
    *,
    allow_detached_state: bool = False,
    mode: str = "exact-retained-native-handle",
) -> Dict[str, Any]:
    """Terminate only through an immutable process HANDLE capability."""
    capability = dict(cleanup_capability or {})
    expected_path = _canonical_windows_path(
        capability.get("canonicalExePath")
    )
    expected_creation = capability.get("creationEpochMs")
    capability_exact = bool(
        capability.get("available") is True
        and capability.get("pid") == pid
        and isinstance(expected_creation, int)
        and not isinstance(expected_creation, bool)
        and expected_creation > 0
        and expected_path == _canonical_windows_path(
            WAR3_DIR / "war3.exe"
        )
        and capability.get("launcherMode") == "direct"
    )
    attempts: List[Dict[str, Any]] = []
    exact_termination: Dict[str, Any] = {}

    runner = dict(retained_witness or {})
    runner_native = runner.get("_nativeProcessWitness")
    runner_exact = bool(
        capability_exact
        and runner.get("shapeExact") is True
        and runner.get("retainedProcessPid") == pid
        and runner.get("retainedProcessCreationEpochMs") ==
            expected_creation
        and _canonical_windows_path(
            runner.get("retainedProcessCanonicalExePath")
        ) == expected_path
        and callable(getattr(runner_native, "terminate_exact", None))
    )
    if runner_exact:
        try:
            result = dict(runner_native.terminate_exact(
                pid, int(expected_creation), expected_path,
                wait_timeout_sec=10.0,
            ))
        except Exception as exc:
            result = {
                "ok": False,
                "exact": False,
                "error": f"{type(exc).__name__}: {exc}",
            }
        result["source"] = "runner-launch-frozen-duplicate"
        result["pidTerminationCommandIssued"] = False
        attempts.append(result)
        if result.get("exact") is True:
            exact_termination = result

    # Duplicate acquisition/freeze failure must not orphan an already-started
    # process. The STATE-owned original hProcess is an independent cleanup
    # capability and is used only after exact immutable binding.
    state_native = getattr(STATE, "retained_native_process", None)
    state_snapshot: Dict[str, Any] = {}
    try:
        if state_native is not None:
            state_snapshot = dict(state_native.snapshot())
    except Exception as exc:
        state_snapshot = {
            "available": False,
            "error": f"{type(exc).__name__}: {exc}",
        }
    state_exact = bool(
        capability_exact
        and STATE.war3_pid == pid
        and STATE.launcher_pid == pid
        and STATE.launcher_created_at_ms == expected_creation
        and str(STATE.launcher_mode or "") == "direct"
        and _canonical_windows_path(STATE.launcher_exe) == expected_path
        and state_snapshot.get("available") is True
        and state_snapshot.get("ownsNativeHandle") is True
        and state_snapshot.get("closed") is False
        and state_snapshot.get("pid") == pid
        and state_snapshot.get("creationEpochMs") == expected_creation
        and _canonical_windows_path(
            state_snapshot.get("canonicalExePath")
        ) == expected_path
        and callable(getattr(state_native, "terminate_exact", None))
    )
    if not exact_termination and state_exact:
        try:
            result = dict(state_native.terminate_exact(
                pid, int(expected_creation), expected_path,
                wait_timeout_sec=10.0,
            ))
        except Exception as exc:
            result = {
                "ok": False,
                "exact": False,
                "error": f"{type(exc).__name__}: {exc}",
            }
        result["source"] = "state-original-native-handle"
        result["pidTerminationCommandIssued"] = False
        attempts.append(result)
        if result.get("exact") is True:
            exact_termination = result

    termination_exact = bool(
        capability_exact
        and exact_termination.get("exact") is True
        and exact_termination.get("bindingExact") is True
        and exact_termination.get("handleSignaled") is True
    )
    process_authority = _process_liveness_authority_contract(
        {
            "exact": False,
            "fingerprintAvailable": False,
            "reason": "current PID query not used for cleanup authority",
            "reportOnly": True,
            "failureClassificationAuthority": 0,
        },
        {
            "ok": True,
            "running": False,
            "source": "exact retained native HANDLE signal",
        },
        {
            "exact": termination_exact,
            "bindingExact": termination_exact,
            "handleSignaled": termination_exact,
            "nativeTerminationProof": exact_termination,
            "reportOnly": not termination_exact,
            "failureClassificationAuthority": (
                1 if termination_exact else 0
            ),
        },
    )

    state_finalize: Dict[str, Any]
    if termination_exact and state_exact:
        state_finalize = _finalize_state_after_exact_native_termination(
            pid, int(expected_creation), expected_path,
        )
    elif termination_exact and allow_detached_state and STATE.war3_pid != pid:
        state_finalize = {
            "ok": True,
            "skipped": True,
            "reason": "launch STATE already detached after exact prior cleanup",
        }
    else:
        state_finalize = {
            "ok": False,
            "skipped": True,
            "reason": (
                "exact termination unavailable"
                if not termination_exact
                else "exact STATE cleanup capability unavailable"
            ),
        }
    cleanup_exact = bool(
        process_authority.get("terminatedExact") is True
        and state_finalize.get("ok") is True
    )
    final_process = {
        "ok": cleanup_exact,
        "running": False if termination_exact else None,
        "pid": pid,
        "livenessSource": "exact retained native HANDLE",
        "processAuthority": process_authority,
    }
    return {
        "ok": cleanup_exact,
        "stopped": cleanup_exact,
        "pid": pid,
        "mode": mode,
        "cleanupCapabilityExact": capability_exact,
        "runnerDuplicateExact": runner_exact,
        "stateOriginalExact": state_exact,
        "attempts": attempts,
        "exactTermination": exact_termination,
        "stateFinalize": state_finalize,
        "finalProcess": final_process,
        "finalProcessAuthority": process_authority,
        "pidTerminationCommandIssued": False,
    }


def _validate_launch_instance_fingerprint(
    fingerprint: Dict[str, Any], pid: int,
) -> Dict[str, Any]:
    frozen_copy = json.loads(json.dumps(dict(fingerprint or {}), ensure_ascii=False))
    if frozen_copy.get("available") is not True:
        return {
            "exact": False,
            "fingerprintAvailable": False,
            "targetPid": pid,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
            "reason": "immutable launch fingerprint unavailable",
        }
    return _launch_instance_binding_contract(
        frozen_copy, _capture_process_identity_snapshot(pid), pid,
    )


def _process_liveness_authority_contract(
    fingerprint_validation: Dict[str, Any],
    raw_process_state: Dict[str, Any],
    retained_termination_proof: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    validation = dict(fingerprint_validation or {})
    raw_state = dict(raw_process_state or {})
    termination = dict(retained_termination_proof or {})
    raw_state_available = bool(
        raw_state.get("ok") is True and "running" in raw_state
    )
    raw_running = bool(raw_state.get("running")) if raw_state_available else None
    current_exact = validation.get("exact") is True
    retained_terminated_exact = termination.get("exact") is True
    # A current exact instance and a signaled retained launch handle are
    # mutually exclusive. Treat contradictory evidence as indeterminate.
    evidence_conflict = bool(current_exact and retained_terminated_exact)
    alive_exact = bool(
        not evidence_conflict and current_exact
        and raw_state_available and raw_running is True
    )
    terminated_exact = bool(
        not evidence_conflict and not alive_exact
        and retained_terminated_exact
    )
    state = (
        "alive" if alive_exact
        else "terminated" if terminated_exact
        else "indeterminate"
    )
    return {
        "state": state,
        "aliveExact": alive_exact,
        "terminatedExact": terminated_exact,
        "indeterminate": state == "indeterminate",
        "hardAuthority": state != "indeterminate",
        "failureClassificationAuthority": (
            1 if state != "indeterminate" else 0
        ),
        "rawProcessState": raw_state,
        "rawProcessStateAvailable": raw_state_available,
        "rawRunningReportOnly": raw_running,
        "immutableLaunchFingerprintValidation": validation,
        "immutableLaunchFingerprintCurrentExact": current_exact,
        "retainedTerminationProof": termination,
        "retainedTerminationExact": retained_terminated_exact,
        "evidenceConflict": evidence_conflict,
        "reportOnly": state == "indeterminate",
    }


def _hard_process_evidence_contract(
    process_authority: Dict[str, Any],
    evidence_fingerprint_exact: bool,
    crash_matches: Iterable[str],
) -> Dict[str, Any]:
    authority = dict(process_authority or {})
    alive_exact = authority.get("aliveExact") is True
    evidence_authority = bool(alive_exact and evidence_fingerprint_exact)
    crashes = list(crash_matches)
    return {
        "processAlive": alive_exact,
        "evidenceAuthorityExact": evidence_authority,
        "crashScanClean": bool(evidence_authority and not crashes),
        "crashCount": len(crashes),
        "reportOnly": not evidence_authority,
        "failureClassificationAuthority": (
            1 if evidence_authority else 0
        ),
    }


def _capture_retained_launch_handle_witness(
    fingerprint: Dict[str, Any], pid: int,
) -> Dict[str, Any]:
    frozen = json.loads(json.dumps(
        dict(fingerprint or {}), ensure_ascii=False,
    ))
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    state_copy = {
        "war3Pid": STATE.war3_pid,
        "launcherPid": STATE.launcher_pid,
        "launcherCreatedAtMs": STATE.launcher_created_at_ms,
        "launcherMode": str(STATE.launcher_mode or ""),
        "launcherExe": str(STATE.launcher_exe or ""),
    }
    native_process = getattr(STATE, "retained_native_process", None)
    duplicate: Any = None
    duplicate_error = ""
    try:
        state_native_snapshot = (
            dict(native_process.snapshot())
            if native_process is not None else {}
        )
    except Exception as exc:
        state_native_snapshot = {}
        duplicate_error = f"snapshot: {type(exc).__name__}: {exc}"
    pre_duplicate_shape_exact = bool(
        frozen.get("available") is True
        and frozen.get("launcherMode") == "direct"
        and frozen.get("pid") == pid
        and isinstance(frozen.get("creationEpochMs"), int)
        and not isinstance(frozen.get("creationEpochMs"), bool)
        and frozen.get("creationEpochMs") > 0
        and _canonical_windows_path(frozen.get("canonicalExePath")) ==
            expected_path
        and state_copy["war3Pid"] == pid
        and state_copy["launcherPid"] == pid
        and state_copy["launcherCreatedAtMs"] ==
            frozen.get("creationEpochMs")
        and state_copy["launcherMode"] == "direct"
        and _canonical_windows_path(state_copy["launcherExe"]) ==
            expected_path
        and state_native_snapshot.get("available") is True
        and state_native_snapshot.get("ownsNativeHandle") is True
        and state_native_snapshot.get("closed") is False
        and state_native_snapshot.get("pid") == pid
        and state_native_snapshot.get("creationEpochMs") ==
            frozen.get("creationEpochMs")
        and _canonical_windows_path(
            state_native_snapshot.get("canonicalExePath")
        ) == expected_path
        and callable(getattr(native_process, "duplicate", None))
        and callable(getattr(native_process, "termination_exact", None))
        and callable(getattr(native_process, "close", None))
    )
    if pre_duplicate_shape_exact:
        try:
            duplicate = native_process.duplicate()
        except Exception as exc:
            duplicate_error = f"{type(exc).__name__}: {exc}"
    try:
        duplicate_snapshot = (
            dict(duplicate.snapshot()) if duplicate is not None else {}
        )
    except Exception as exc:
        duplicate_snapshot = {}
        duplicate_error = f"duplicate snapshot: {type(exc).__name__}: {exc}"
    shape_exact = bool(
        pre_duplicate_shape_exact
        and duplicate_snapshot.get("available") is True
        and duplicate_snapshot.get("ownsNativeHandle") is True
        and duplicate_snapshot.get("closed") is False
        and duplicate_snapshot.get("pid") == pid
        and duplicate_snapshot.get("creationEpochMs") ==
            frozen.get("creationEpochMs")
        and _canonical_windows_path(
            duplicate_snapshot.get("canonicalExePath")
        ) == expected_path
        and callable(getattr(duplicate, "termination_exact", None))
        and callable(getattr(duplicate, "close", None))
    )
    if duplicate is not None and not shape_exact:
        try:
            duplicate.close()
        except Exception:
            pass
        duplicate = None
    return {
        "shapeExact": shape_exact,
        "targetPid": pid,
        "fingerprintCreationEpochMs": frozen.get("creationEpochMs"),
        "stateCopy": state_copy,
        "stateWar3ProcIsNone": STATE.war3_proc is None,
        "stateNativeProcessWitness": state_native_snapshot,
        "retainedProcessPid": duplicate_snapshot.get("pid"),
        "retainedProcessCreationEpochMs": duplicate_snapshot.get(
            "creationEpochMs"
        ),
        "retainedProcessCanonicalExePath": duplicate_snapshot.get(
            "canonicalExePath"
        ),
        "duplicateError": duplicate_error,
        # Internal-only object: callers must never serialize the witness.
        "_nativeProcessWitness": duplicate if shape_exact else None,
    }


def _retained_launch_handle_termination_proof(
    fingerprint: Dict[str, Any], pid: int,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    witness = dict(retained_witness or {})
    shape_exact = witness.get("shapeExact") is True
    native_process = witness.get("_nativeProcessWitness")
    proc_pid = witness.get("retainedProcessPid")
    frozen = dict(fingerprint or {})
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    binding_exact = bool(
        shape_exact
        and frozen.get("available") is True
        and frozen.get("pid") == pid
        and witness.get("fingerprintCreationEpochMs") ==
            frozen.get("creationEpochMs")
        and witness.get("retainedProcessCreationEpochMs") ==
            frozen.get("creationEpochMs")
        and _canonical_windows_path(
            witness.get("retainedProcessCanonicalExePath")
        ) == expected_path
        and _canonical_windows_path(
            frozen.get("canonicalExePath")
        ) == expected_path
        and callable(getattr(native_process, "termination_exact", None))
    )
    native_proof: Dict[str, Any] = {}
    proof_error = ""
    if binding_exact:
        try:
            native_proof = dict(native_process.termination_exact(
                pid,
                int(frozen.get("creationEpochMs", 0) or 0),
                expected_path,
            ))
        except Exception as exc:
            proof_error = f"{type(exc).__name__}: {exc}"
    terminated = bool(
        binding_exact
        and not proof_error
        and native_proof.get("exact") is True
        and native_proof.get("bindingExact") is True
        and native_proof.get("handleSignaled") is True
    )
    return {
        "exact": terminated,
        "shapeExact": shape_exact,
        "bindingExact": binding_exact,
        "targetPid": pid,
        "fingerprintCreationEpochMs": witness.get(
            "fingerprintCreationEpochMs"
        ),
        "stateCopy": dict(witness.get("stateCopy", {}) or {}),
        "retainedProcessPid": proc_pid,
        "retainedProcessCreationEpochMs": witness.get(
            "retainedProcessCreationEpochMs"
        ),
        "retainedProcessCanonicalExePath": witness.get(
            "retainedProcessCanonicalExePath"
        ),
        "returnCode": native_proof.get("exitCode"),
        "pollError": proof_error or native_proof.get("pollError", ""),
        "handleSignaled": terminated,
        "nativeTerminationProof": native_proof,
        "reportOnly": not terminated,
        "failureClassificationAuthority": 1 if terminated else 0,
    }


def _terminate_retained_launch_handle_witness(
    fingerprint: Dict[str, Any], pid: int,
    retained_witness: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    witness = dict(retained_witness or {})
    frozen = dict(fingerprint or {})
    native_process = witness.get("_nativeProcessWitness")
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    binding_exact = bool(
        witness.get("shapeExact") is True
        and frozen.get("available") is True
        and frozen.get("pid") == pid
        and witness.get("fingerprintCreationEpochMs") ==
            frozen.get("creationEpochMs")
        and witness.get("retainedProcessPid") == pid
        and witness.get("retainedProcessCreationEpochMs") ==
            frozen.get("creationEpochMs")
        and _canonical_windows_path(
            witness.get("retainedProcessCanonicalExePath")
        ) == expected_path
        and _canonical_windows_path(
            frozen.get("canonicalExePath")
        ) == expected_path
        and callable(getattr(native_process, "terminate_exact", None))
    )
    if not binding_exact:
        return {
            "ok": False,
            "exact": False,
            "bindingExact": False,
            "handleSignaled": False,
            "terminatedByHandle": False,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
            "error": "retained native witness binding mismatch",
        }
    try:
        result = dict(native_process.terminate_exact(
            pid,
            int(frozen.get("creationEpochMs", 0) or 0),
            expected_path,
            wait_timeout_sec=10.0,
        ))
    except Exception as exc:
        return {
            "ok": False,
            "exact": False,
            "bindingExact": True,
            "handleSignaled": False,
            "terminatedByHandle": False,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
            "error": f"{type(exc).__name__}: {exc}",
        }
    result["pidTerminationCommandIssued"] = False
    result["authority"] = (
        "launch-frozen exact native process HANDLE"
    )
    return result


def _close_retained_launch_handle_witness(
    retained_witness: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    witness = retained_witness if isinstance(retained_witness, dict) else {}
    native_process = witness.get("_nativeProcessWitness")
    if native_process is None:
        return {
            "ok": True,
            "closed": True,
            "skipped": True,
            "reason": "no retained native process witness",
        }
    try:
        result = dict(native_process.close())
    except Exception as exc:
        result = {
            "ok": False,
            "closed": False,
            "error": f"{type(exc).__name__}: {exc}",
        }
    # Remove the object immediately. A closed HANDLE integer is never reused.
    witness["_nativeProcessWitness"] = None
    witness["closed"] = True
    return result


def _retained_launch_handle_witness_report(
    retained_witness: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    return {
        key: value for key, value in dict(retained_witness or {}).items()
        if not str(key).startswith("_")
    }


def _retained_native_process_witness_synthetic_self_tests() -> Dict[str, Any]:
    """Exercise the real native HANDLE path without launching Warcraft III."""
    global WAR3_DIR
    if (
        getattr(STATE, "retained_native_process", None) is not None
        or STATE.war3_pid is not None
        or STATE.war3_proc is not None
        or STATE.launcher_pid is not None
        or int(STATE.desktop_handle or 0) != 0
        or bool(STATE.video_restore_snapshot)
    ):
        raise RuntimeError(
            "native witness synthetic requires an idle AutoTest STATE"
        )
    saved_war3_dir = WAR3_DIR
    saved_state = {
        "war3_proc": STATE.war3_proc,
        "war3_pid": STATE.war3_pid,
        "launcher_pid": STATE.launcher_pid,
        "launcher_created_at_ms": STATE.launcher_created_at_ms,
        "launcher_mode": STATE.launcher_mode,
        "launcher_exe": STATE.launcher_exe,
    }
    first_child: Optional[subprocess.Popen] = None
    second_child: Optional[subprocess.Popen] = None
    first_runner_witness: Dict[str, Any] = {}
    second_runner_witness: Dict[str, Any] = {}
    checks: Dict[str, bool] = {}
    with tempfile.TemporaryDirectory(
        prefix="codex_native_process_witness_"
    ) as temp_text:
        temp_dir = Path(temp_text)
        synthetic_exe = temp_dir / "war3.exe"
        system_cmd = (
            Path(os.environ.get("WINDIR", r"C:\Windows"))
            / "System32" / "cmd.exe"
        )
        shutil.copy2(system_cmd, synthetic_exe)
        WAR3_DIR = temp_dir

        def start_child() -> subprocess.Popen:
            return subprocess.Popen(
                [str(synthetic_exe), "/d", "/q"],
                cwd=str(temp_dir),
                # An open pipe keeps cmd.exe itself in its input loop. No
                # ping/timeout or other child process is ever created.
                stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=0x08000000,
            )

        def install_state(
            child: subprocess.Popen, native_process: Any,
        ) -> Tuple[Dict[str, Any], Dict[str, Any]]:
            snapshot = dict(native_process.snapshot())
            STATE.war3_proc = None
            STATE.war3_pid = int(child.pid)
            STATE.launcher_pid = int(child.pid)
            STATE.launcher_created_at_ms = int(
                snapshot.get("creationEpochMs", 0) or 0
            )
            STATE.launcher_mode = "direct"
            STATE.launcher_exe = str(synthetic_exe)
            launch_result = {
                "ok": True,
                "pid": int(child.pid),
                "gamePid": int(child.pid),
                "launcherPid": int(child.pid),
                "launcherMode": "direct",
                "launcherExe": str(synthetic_exe),
                "nativeProcessWitness": snapshot,
            }
            fingerprint = _freeze_launch_instance_fingerprint(
                int(child.pid), launch_result,
                int(time.time() * 1000),
            )
            return fingerprint, launch_result

        try:
            first_child = start_child()
            first_native, first_acquisition = (
                _open_native_process_witness(
                    first_child.pid,
                    synthetic_exe,
                    "synthetic-first-isolated-native-handle",
                )
            )
            if first_native is None:
                raise AssertionError(first_acquisition)
            first_previous_close = (
                _replace_state_retained_native_process(first_native)
            )
            first_fingerprint, _ = install_state(
                first_child, first_native,
            )
            first_runner_witness = (
                _capture_retained_launch_handle_witness(
                    first_fingerprint, first_child.pid,
                )
            )
            first_unsignaled = _retained_launch_handle_termination_proof(
                first_fingerprint, first_child.pid,
                first_runner_witness,
            )
            current_query_indeterminate = (
                _process_liveness_authority_contract(
                    {
                        "exact": False,
                        "fingerprintAvailable": True,
                        "reason": "synthetic current query unavailable",
                    },
                    {"ok": True, "running": False},
                    first_unsignaled,
                )
            )
            wrong_pid = _retained_launch_handle_termination_proof(
                first_fingerprint, first_child.pid + 1,
                first_runner_witness,
            )
            wrong_creation_fingerprint = dict(first_fingerprint)
            wrong_creation_fingerprint["creationEpochMs"] = (
                int(first_fingerprint["creationEpochMs"]) + 1
            )
            wrong_creation = _retained_launch_handle_termination_proof(
                wrong_creation_fingerprint, first_child.pid,
                first_runner_witness,
            )
            wrong_path_fingerprint = dict(first_fingerprint)
            wrong_path_fingerprint["canonicalExePath"] = str(
                temp_dir / "other.exe"
            )
            wrong_path = _retained_launch_handle_termination_proof(
                wrong_path_fingerprint, first_child.pid,
                first_runner_witness,
            )

            second_child = start_child()
            second_native, second_acquisition = (
                _open_native_process_witness(
                    second_child.pid,
                    synthetic_exe,
                    "synthetic-second-isolated-native-handle",
                )
            )
            if second_native is None:
                raise AssertionError(second_acquisition)
            old_state_close = _replace_state_retained_native_process(
                second_native
            )
            second_state_published_after_old_close = bool(
                STATE.retained_native_process is second_native
                and first_native.snapshot().get("closed") is True
            )
            first_duplicate_after_overwrite = (
                _retained_launch_handle_termination_proof(
                    first_fingerprint, first_child.pid,
                    first_runner_witness,
                )
            )
            second_fingerprint, _ = install_state(
                second_child, second_native,
            )
            second_runner_witness = (
                _capture_retained_launch_handle_witness(
                    second_fingerprint, second_child.pid,
                )
            )
            second_unsignaled = _retained_launch_handle_termination_proof(
                second_fingerprint, second_child.pid,
                second_runner_witness,
            )

            first_handle_termination = (
                _terminate_retained_launch_handle_witness(
                    first_fingerprint,
                    first_child.pid,
                    first_runner_witness,
                )
            )
            first_child.wait(timeout=10)
            if first_child.stdin is not None:
                first_child.stdin.close()
            first_signaled = _retained_launch_handle_termination_proof(
                first_fingerprint, first_child.pid,
                first_runner_witness,
            )
            second_handle_termination = (
                _terminate_retained_launch_handle_witness(
                    second_fingerprint,
                    second_child.pid,
                    second_runner_witness,
                )
            )
            second_child.wait(timeout=10)
            if second_child.stdin is not None:
                second_child.stdin.close()
            second_signaled = _retained_launch_handle_termination_proof(
                second_fingerprint, second_child.pid,
                second_runner_witness,
            )

            first_runner_close = _close_retained_launch_handle_witness(
                first_runner_witness
            )
            first_after_close = _retained_launch_handle_termination_proof(
                first_fingerprint, first_child.pid,
                first_runner_witness,
            )
            second_runner_close = _close_retained_launch_handle_witness(
                second_runner_witness
            )
            state_second_close = (
                _finalize_state_after_exact_native_termination(
                    second_child.pid,
                    int(second_fingerprint["creationEpochMs"]),
                    str(second_fingerprint["canonicalExePath"]),
                )
            )

            checks = {
                "firstNativeAcquisitionExact": bool(
                    first_acquisition.get("ok") is True
                    and first_fingerprint.get("available") is True
                ),
                "isolatedStateUsesNativeNotPopen": bool(
                    STATE.war3_proc is None
                    and first_runner_witness.get(
                        "stateWar3ProcIsNone"
                    ) is True
                ),
                "firstRunnerDuplicateExact": (
                    first_runner_witness.get("shapeExact") is True
                ),
                "unsignaledNeverAuthorizesTermination": bool(
                    first_unsignaled.get("exact") is False
                    and first_unsignaled.get("bindingExact") is True
                    and first_unsignaled.get("handleSignaled") is False
                ),
                "unsignaledExactHandleWithIndeterminateQuery": bool(
                    current_query_indeterminate.get("indeterminate") is True
                    and current_query_indeterminate.get(
                        "terminatedExact"
                    ) is False
                ),
                "wrongPidRejected": wrong_pid.get("exact") is False,
                "wrongCreationRejected": (
                    wrong_creation.get("exact") is False
                ),
                "wrongPathRejected": wrong_path.get("exact") is False,
                "firstPublishHadNoOldHandle": bool(
                    first_previous_close.get("ok") is True
                    and first_previous_close.get("skipped") is True
                ),
                "relaunchClosesOldStateHandleBeforeOverwrite": bool(
                    old_state_close.get("ok") is True
                    and old_state_close.get("closed") is True
                    and second_state_published_after_old_close
                ),
                "firstDuplicateSurvivesStateOverwrite": bool(
                    first_duplicate_after_overwrite.get("exact") is False
                    and first_duplicate_after_overwrite.get(
                        "bindingExact"
                    ) is True
                    and first_duplicate_after_overwrite.get(
                        "pollError"
                    ) == ""
                ),
                "secondNativeAcquisitionExact": bool(
                    second_acquisition.get("ok") is True
                    and second_fingerprint.get("available") is True
                    and second_runner_witness.get("shapeExact") is True
                ),
                "secondUnsignaledNeverAuthorizes": (
                    second_unsignaled.get("exact") is False
                ),
                "firstSignaledExact": first_signaled.get("exact") is True,
                "secondSignaledExact": second_signaled.get("exact") is True,
                "firstExactDeathWithoutPidCommand": bool(
                    first_handle_termination.get("exact") is True
                    and first_handle_termination.get(
                        "pidTerminationCommandIssued"
                    ) is False
                ),
                "secondExactDeathWithoutPidCommand": bool(
                    second_handle_termination.get("exact") is True
                    and second_handle_termination.get(
                        "pidTerminationCommandIssued"
                    ) is False
                ),
                "firstRunnerHandleClosed": bool(
                    first_runner_close.get("ok") is True
                    and first_runner_close.get("closed") is True
                ),
                "secondRunnerHandleClosed": bool(
                    second_runner_close.get("ok") is True
                    and second_runner_close.get("closed") is True
                ),
                "closedHandleCannotBeReused": bool(
                    first_after_close.get("exact") is False
                    and first_after_close.get("bindingExact") is False
                    and first_after_close.get("handleSignaled") is False
                ),
                "stateSecondHandleClosed": bool(
                    state_second_close.get("ok") is True
                    and state_second_close.get("finalized") is True
                    and second_native.snapshot().get("closed") is True
                ),
            }
            if not all(checks.values()):
                raise AssertionError(
                    "retained native process witness synthetic failed: "
                    + json.dumps(checks, sort_keys=True)
                )
            return {
                "ok": True,
                "count": len(checks),
                "checks": checks,
            }
        finally:
            _close_retained_launch_handle_witness(
                first_runner_witness
            )
            _close_retained_launch_handle_witness(
                second_runner_witness
            )
            _replace_state_retained_native_process(None)
            for child in (second_child, first_child):
                if child is not None and child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=10)
                if child is not None and child.stdin is not None:
                    try:
                        child.stdin.close()
                    except Exception:
                        pass
            STATE.war3_proc = saved_state["war3_proc"]
            STATE.war3_pid = saved_state["war3_pid"]
            STATE.launcher_pid = saved_state["launcher_pid"]
            STATE.launcher_created_at_ms = saved_state[
                "launcher_created_at_ms"
            ]
            STATE.launcher_mode = saved_state["launcher_mode"]
            STATE.launcher_exe = saved_state["launcher_exe"]
            WAR3_DIR = saved_war3_dir


def _capture_fresh_process_authority(
    pid: int, fingerprint: Optional[Dict[str, Any]],
    raw_process_state: Optional[Dict[str, Any]] = None,
    retained_termination_proof: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    raw_state = (
        dict(raw_process_state)
        if isinstance(raw_process_state, dict)
        else _best_effort(
            "is_war3_running",
            lambda: is_war3_running(pid=pid),
        )
    )
    validation = _validate_launch_instance_fingerprint(
        dict(fingerprint or {}), pid,
    )
    termination = (
        dict(retained_termination_proof)
        if isinstance(retained_termination_proof, dict)
        else (
            _retained_launch_handle_termination_proof(
                dict(fingerprint or {}), pid, retained_witness,
            )
            if isinstance(retained_witness, dict)
            else {
                "exact": False,
                "shapeExact": False,
                "bindingExact": False,
                "handleSignaled": False,
                "reportOnly": True,
                "failureClassificationAuthority": 0,
                "reason": "no launch-frozen retained native witness",
            }
        )
    )
    result = _process_liveness_authority_contract(
        validation, raw_state, termination,
    )
    result["capturedAtEpochMs"] = int(time.time() * 1000)
    return result


def _module_snapshot_contract(
    result: Dict[str, Any], pid: int, attempts: List[Dict[str, Any]],
    launch_epoch_ms: Optional[int] = None,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    payload = result.get("payload")
    snapshot = dict(payload) if isinstance(payload, dict) else {}
    raw_reported_pid = snapshot.get("pid")
    reported_pid = _coerce_pid(raw_reported_pid)
    numeric_pid_exact = bool(
        isinstance(raw_reported_pid, int)
        and not isinstance(raw_reported_pid, bool)
        and raw_reported_pid == pid
    )
    exact_pid = numeric_pid_exact
    modules_value = snapshot.get("modules", [])
    if isinstance(modules_value, dict):
        modules = [dict(modules_value)]
    elif isinstance(modules_value, list):
        modules = [dict(row) for row in modules_value if isinstance(row, dict)]
    else:
        modules = []
    observed_names = sorted({
        str(row.get("name", "") or "").strip().lower()
        for row in modules
        if str(row.get("name", "") or "").strip()
    })
    missing = [
        name for name in READY_EVIDENCE_REQUIRED_MODULES
        if name not in observed_names
    ]
    backend = str(snapshot.get("moduleEnumerationBackend", "") or "")
    terminal_error = _coerce_pid(
        snapshot.get("moduleEnumerationTerminalError")
    )
    toolhelp_terminal_clean = bool(
        backend != "toolhelp32-snapshot"
        or terminal_error == TOOLHELP_ERROR_NO_MORE_FILES
    )
    enumeration_complete = bool(
        result.get("ok")
        and exact_pid
        and snapshot.get("moduleEnumerationComplete") is True
        and toolhelp_terminal_clean
    )
    enumeration_error = str(
        snapshot.get("moduleEnumerationError", "")
        or snapshot.get("moduleError", "")
        or result.get("parseError", "")
        or result.get("query", {}).get("error", "")
        or ""
    )
    snapshot["modules"] = modules
    snapshot["moduleCount"] = len(modules)
    start_time_utc = str(snapshot.get("startTimeUtc", "") or "")
    raw_start_epoch_ms = snapshot.get("startTimeEpochMs")
    start_epoch_ms = (
        raw_start_epoch_ms
        if isinstance(raw_start_epoch_ms, int)
        and not isinstance(raw_start_epoch_ms, bool)
        and raw_start_epoch_ms > 0
        else _parse_utc_epoch_ms(start_time_utc)
    )
    launch_epoch_valid = bool(
        isinstance(launch_epoch_ms, int)
        and not isinstance(launch_epoch_ms, bool)
        and launch_epoch_ms > 0
    )
    process_identity_war3 = _war3_process_identity(
        snapshot.get("processName"), snapshot.get("path"),
    )
    start_at_or_after_launch = bool(
        launch_epoch_valid
        and isinstance(start_epoch_ms, int)
        and start_epoch_ms + PROCESS_INSTANCE_CLOCK_TOLERANCE_MS >=
            int(launch_epoch_ms)
    )
    fingerprint_binding = _launch_instance_binding_contract(
        dict(launch_fingerprint or {}),
        {
            "ok": bool(result.get("ok")),
            "payload": snapshot,
        },
        pid,
    )
    exact_launch_process_instance = fingerprint_binding.get("exact") is True
    snapshot["moduleEvidence"] = {
        "targetPid": pid,
        "reportedPid": reported_pid,
        "numericPidExact": numeric_pid_exact,
        "exactPid": exact_pid,
        "backend": backend,
        "backendError": enumeration_error,
        "terminalError": terminal_error,
        "expectedTerminalError": (
            TOOLHELP_ERROR_NO_MORE_FILES
            if backend == "toolhelp32-snapshot" else None
        ),
        "toolhelpTerminalClean": toolhelp_terminal_clean,
        "enumerationComplete": enumeration_complete,
        "requiredModuleSetComplete": enumeration_complete and not missing,
        "targetProcessModuleEvidenceComplete": bool(
            enumeration_complete and not missing
            and exact_launch_process_instance
        ),
        "requiredModules": list(READY_EVIDENCE_REQUIRED_MODULES),
        "observedModuleNames": observed_names,
        "missingRequiredModules": missing,
        "reportedProcessName": snapshot.get("processName"),
        "reportedProcessPath": snapshot.get("path"),
        "processIdentityWar3": process_identity_war3,
        "launchEpochMs": launch_epoch_ms if launch_epoch_valid else None,
        "processStartTimeUtc": start_time_utc,
        "processStartEpochMs": start_epoch_ms,
        "processStartAtOrAfterLaunch": start_at_or_after_launch,
        "processInstanceClockToleranceMs": (
            PROCESS_INSTANCE_CLOCK_TOLERANCE_MS
        ),
        "exactLaunchProcessInstance": exact_launch_process_instance,
        "immutableLaunchFingerprintAvailable": bool(
            dict(launch_fingerprint or {}).get("available") is True
        ),
        "immutableLaunchFingerprintCurrentExact": (
            fingerprint_binding.get("exact") is True
        ),
        "immutableLaunchFingerprintBinding": fingerprint_binding,
        "usableForTargetProcessAttribution": bool(
            enumeration_complete and exact_launch_process_instance
        ),
        "attempts": attempts,
        "reportOnly": True,
        "failureClassificationAuthority": 0,
    }
    result["payload"] = snapshot
    result["moduleEvidence"] = snapshot["moduleEvidence"]
    return result


def _capture_process_snapshot(
    pid: int, launch_epoch_ms: Optional[int] = None,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    target_pid = int(pid)
    attempts: List[Dict[str, Any]] = []
    if target_pid <= 0:
        return _module_snapshot_contract(
            {
                "ok": False,
                "payload": {
                    "found": False,
                    "pid": target_pid,
                    "moduleEnumerationBackend": "none",
                    "moduleEnumerationComplete": False,
                    "moduleEnumerationError": "invalid exact launch PID",
                    "modules": [],
                },
                "parseError": "invalid exact launch PID",
                "query": {},
            },
            target_pid,
            attempts,
            launch_epoch_ms,
            launch_fingerprint,
        )

    if POWERSHELL_X86.is_file():
        primary = _powershell_json(
            _process_snapshot_powershell_script(
                target_pid, "syswow64-get-process-modules",
            ),
            timeout_sec=15,
            executable=str(POWERSHELL_X86),
        )
        primary_payload = primary.get("payload")
        primary_snapshot = (
            primary_payload if isinstance(primary_payload, dict) else {}
        )
        attempts.append({
            "backend": "syswow64-get-process-modules",
            "ok": bool(primary.get("ok")),
            "found": primary_snapshot.get("found"),
            "exactPid": bool(
                isinstance(primary_snapshot.get("pid"), int)
                and not isinstance(primary_snapshot.get("pid"), bool)
                and primary_snapshot.get("pid") == target_pid
            ),
            "enumerationComplete": primary_snapshot.get(
                "moduleEnumerationComplete"
            ) is True,
            "error": str(
                primary_snapshot.get("moduleEnumerationError", "")
                or primary.get("parseError", "")
                or primary.get("query", {}).get("error", "")
                or ""
            ),
        })
        if (
            primary.get("ok")
            and isinstance(primary_snapshot.get("pid"), int)
            and not isinstance(primary_snapshot.get("pid"), bool)
            and primary_snapshot.get("pid") == target_pid
            and primary_snapshot.get("moduleEnumerationComplete") is True
        ):
            return _module_snapshot_contract(
                primary, target_pid, attempts, launch_epoch_ms,
                launch_fingerprint,
            )
    else:
        attempts.append({
            "backend": "syswow64-get-process-modules",
            "ok": False,
            "found": None,
            "exactPid": False,
            "enumerationComplete": False,
            "error": f"32-bit PowerShell missing: {POWERSHELL_X86}",
        })

    fallback = _powershell_json(
        _process_snapshot_toolhelp_script(target_pid),
        timeout_sec=20,
    )
    fallback_payload = fallback.get("payload")
    fallback_snapshot = (
        fallback_payload if isinstance(fallback_payload, dict) else {}
    )
    attempts.append({
        "backend": "toolhelp32-snapshot",
        "ok": bool(fallback.get("ok")),
        "found": fallback_snapshot.get("found"),
        "exactPid": bool(
            isinstance(fallback_snapshot.get("pid"), int)
            and not isinstance(fallback_snapshot.get("pid"), bool)
            and fallback_snapshot.get("pid") == target_pid
        ),
        "enumerationComplete": fallback_snapshot.get(
            "moduleEnumerationComplete"
        ) is True,
        "terminalError": fallback_snapshot.get(
            "moduleEnumerationTerminalError"
        ),
        "error": str(
            fallback_snapshot.get("moduleEnumerationError", "")
            or fallback.get("parseError", "")
            or fallback.get("query", {}).get("error", "")
            or ""
        ),
    })
    return _module_snapshot_contract(
        fallback, target_pid, attempts, launch_epoch_ms,
        launch_fingerprint,
    )


def _module_snapshot_contract_synthetic_self_tests() -> Dict[str, Any]:
    target_pid = 4242
    launch_epoch_ms = _parse_utc_epoch_ms("2026-07-15T10:00:00Z")
    assert isinstance(launch_epoch_ms, int)
    fingerprint_creation_ms = launch_epoch_ms + 1_000
    launch_fingerprint = {
        "available": True,
        "immutableCopy": True,
        "capturedBeforeReadyWait": True,
        "pid": target_pid,
        "creationEpochMs": fingerprint_creation_ms,
        "canonicalExePath": str(WAR3_DIR / "war3.exe"),
        "launcherMode": "direct",
    }

    def sample(
        reported_pid: Any = target_pid,
        modules: Iterable[str] = READY_EVIDENCE_REQUIRED_MODULES,
        complete: bool = True,
        error: str = "",
        backend: str = "syswow64-get-process-modules",
        terminal_error: Optional[int] = None,
        process_name: str = "war3",
        start_time_utc: str = "2026-07-15T10:00:01Z",
        start_epoch_ms: Optional[int] = fingerprint_creation_ms,
        process_path: Optional[str] = None,
    ) -> Dict[str, Any]:
        return {
            "ok": True,
            "payload": {
                "found": True,
                "pid": reported_pid,
                "processName": process_name,
                "path": process_path or rf"E:\Work\War3\{process_name}.exe",
                "startTimeUtc": start_time_utc,
                "startTimeEpochMs": start_epoch_ms,
                "moduleEnumerationBackend": backend,
                "moduleEnumerationComplete": complete,
                "moduleEnumerationError": error,
                "moduleEnumerationTerminalError": terminal_error,
                "modules": [
                    {"name": name, "path": rf"E:\Work\War3\{name}"}
                    for name in modules
                ],
            },
            "parseError": "",
            "query": {"returncode": 0},
        }

    checks: Dict[str, bool] = {}
    exact = _module_snapshot_contract(
        sample(), target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["exactComplete"] = bool(
        exact["moduleEvidence"]["numericPidExact"]
        and exact["moduleEvidence"]["exactPid"]
        and exact["moduleEvidence"]["enumerationComplete"]
        and exact["moduleEvidence"]["requiredModuleSetComplete"]
        and exact["moduleEvidence"]["missingRequiredModules"] == []
        and exact["moduleEvidence"]["exactLaunchProcessInstance"]
        and exact["moduleEvidence"]["targetProcessModuleEvidenceComplete"]
        and exact["moduleEvidence"]["usableForTargetProcessAttribution"]
    )
    exact_serialized = json.dumps(exact, ensure_ascii=False, sort_keys=True)
    exact_round_trip = json.loads(exact_serialized)
    exact_current_identity = exact_round_trip["moduleEvidence"][
        "immutableLaunchFingerprintBinding"
    ]["currentIdentity"]
    checks["exactSnapshotJsonSerializable"] = bool(
        exact_round_trip["moduleEvidence"]["exactLaunchProcessInstance"]
        and exact_current_identity == {
            "ok": True,
            "found": True,
            "pid": target_pid,
            "processName": "war3",
            "canonicalExePath": _canonical_windows_path(
                WAR3_DIR / "war3.exe"
            ),
            "startTimeEpochMs": fingerprint_creation_ms,
        }
        and "payload" not in exact_current_identity
        and "moduleEvidence" not in exact_current_identity
    )
    missing = _module_snapshot_contract(
        sample(modules=("war3.exe", "d3d9.dll")), target_pid, [],
        launch_epoch_ms, launch_fingerprint,
    )
    checks["successfulButIncompleteRequiredSet"] = bool(
        missing["moduleEvidence"]["enumerationComplete"]
        and not missing["moduleEvidence"]["requiredModuleSetComplete"]
        and missing["moduleEvidence"]["missingRequiredModules"] ==
            ["game.dll"]
    )
    failed = _module_snapshot_contract(
        sample(modules=(), complete=False, error="access denied"),
        target_pid,
        [],
        launch_epoch_ms,
        launch_fingerprint,
    )
    checks["enumerationFailureReportOnly"] = bool(
        not failed["moduleEvidence"]["enumerationComplete"]
        and failed["moduleEvidence"]["backendError"] == "access denied"
        and failed["moduleEvidence"]["reportOnly"] is True
        and failed["moduleEvidence"]["failureClassificationAuthority"] == 0
    )
    wrong_pid = _module_snapshot_contract(
        sample(reported_pid=target_pid + 1), target_pid, [], launch_epoch_ms,
        launch_fingerprint,
    )
    checks["wrongPidRejected"] = bool(
        not wrong_pid["moduleEvidence"]["exactPid"]
        and not wrong_pid["moduleEvidence"]["enumerationComplete"]
        and not wrong_pid["moduleEvidence"]["requiredModuleSetComplete"]
    )
    string_pid = _module_snapshot_contract(
        sample(reported_pid=str(target_pid)), target_pid, [], launch_epoch_ms,
        launch_fingerprint,
    )
    checks["numericPidExactRequired"] = bool(
        string_pid["moduleEvidence"]["reportedPid"] == target_pid
        and string_pid["moduleEvidence"]["numericPidExact"] is False
        and string_pid["moduleEvidence"]["enumerationComplete"] is False
    )
    old_instance = _module_snapshot_contract(
        sample(
            start_time_utc="2026-07-15T09:59:59Z",
            start_epoch_ms=launch_epoch_ms - 1_000,
        ),
        target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["prelaunchProcessInstanceRejected"] = bool(
        old_instance["moduleEvidence"]["processStartAtOrAfterLaunch"] is False
        and old_instance["moduleEvidence"]["exactLaunchProcessInstance"] is False
        and old_instance["moduleEvidence"][
            "usableForTargetProcessAttribution"
        ] is False
    )
    wrong_identity = _module_snapshot_contract(
        sample(process_name="worldeditydwe"),
        target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["worldEditorProcessIdentityRejected"] = bool(
        wrong_identity["moduleEvidence"]["processIdentityWar3"] is False
        and wrong_identity["moduleEvidence"]["exactLaunchProcessInstance"] is False
    )
    toolhelp_complete = _module_snapshot_contract(
        sample(
            backend="toolhelp32-snapshot",
            terminal_error=TOOLHELP_ERROR_NO_MORE_FILES,
        ),
        target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["toolhelpNoMoreFilesAccepted"] = bool(
        toolhelp_complete["moduleEvidence"]["toolhelpTerminalClean"] is True
        and toolhelp_complete["moduleEvidence"]["enumerationComplete"] is True
    )
    toolhelp_partial_error = _module_snapshot_contract(
        sample(
            backend="toolhelp32-snapshot", terminal_error=5,
            modules=("war3.exe", "game.dll", "d3d9.dll"), complete=True,
        ),
        target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["toolhelpPartialErrorRejected"] = bool(
        toolhelp_partial_error["moduleEvidence"][
            "toolhelpTerminalClean"
        ] is False
        and toolhelp_partial_error["moduleEvidence"][
            "enumerationComplete"
        ] is False
        and toolhelp_partial_error["moduleEvidence"]["reportOnly"] is True
    )
    same_path_replacement = _module_snapshot_contract(
        sample(start_epoch_ms=fingerprint_creation_ms + 1),
        target_pid, [], launch_epoch_ms, launch_fingerprint,
    )
    checks["samePathReplacementCreationRejected"] = bool(
        same_path_replacement["moduleEvidence"][
            "immutableLaunchFingerprintAvailable"
        ] is True
        and same_path_replacement["moduleEvidence"][
            "immutableLaunchFingerprintCurrentExact"
        ] is False
        and same_path_replacement["moduleEvidence"][
            "usableForTargetProcessAttribution"
        ] is False
    )
    unavailable_fingerprint = _module_snapshot_contract(
        sample(), target_pid, [], launch_epoch_ms, {},
    )
    checks["unavailableFingerprintNeverAuthorizes"] = bool(
        unavailable_fingerprint["moduleEvidence"][
            "immutableLaunchFingerprintAvailable"
        ] is False
        and unavailable_fingerprint["moduleEvidence"][
            "exactLaunchProcessInstance"
        ] is False
        and unavailable_fingerprint["moduleEvidence"][
            "failureClassificationAuthority"
        ] == 0
    )
    if not all(checks.values()):
        raise AssertionError(
            "module snapshot contract synthetic self-test failed: "
            + json.dumps(checks, sort_keys=True)
        )
    return {"ok": True, "count": len(checks), "checks": checks}


def _capture_windows_error_events(
    pid: int,
    launch_epoch_ms: int,
    process_start_epoch_ms: Optional[int] = None,
) -> Dict[str, Any]:
    authorization_floor_ms = max(
        int(launch_epoch_ms),
        int(process_start_epoch_ms)
        if isinstance(process_start_epoch_ms, int)
        and not isinstance(process_start_epoch_ms, bool)
        else int(launch_epoch_ms),
    )
    script = rf'''
$ErrorActionPreference = "SilentlyContinue"
$targetPid = {int(pid)}
$launch = [DateTimeOffset]::FromUnixTimeMilliseconds({int(launch_epoch_ms)})
$authorizationStart = [DateTimeOffset]::FromUnixTimeMilliseconds({authorization_floor_ms})
# The five-second lookback is collection-only. It is never an authorization
# window; Python revalidates exact PID, process instance, identity and time.
$queryStart = $launch.LocalDateTime.AddSeconds(-5)
$queryError = ""
$rows = @()
try {{
  $rows = @(Get-WinEvent -FilterHashtable @{{LogName="Application"; StartTime=$queryStart}} -MaxEvents 400 |
    Where-Object {{
      $_.Level -le 3 -and (
        $_.ProviderName -in @("Application Error", "Windows Error Reporting", "Application Hang", ".NET Runtime", "SideBySide") -or
        $_.Message -match "war3(?:[.]exe)?|Game[.]dll|d3d9[.]dll|dxvk|War3GpuSkin"
      )
    }} |
    Select-Object -First 100 |
    ForEach-Object {{
      $eventData = [ordered]@{{}}
      $eventDataError = ""
      try {{
        [xml]$eventXml = $_.ToXml()
        foreach ($node in @($eventXml.Event.EventData.Data)) {{
          $name = [string]$node.Name
          if (-not [string]::IsNullOrWhiteSpace($name)) {{
            $eventData[$name] = [string]$node.'#text'
          }}
        }}
      }} catch {{
        $eventDataError = [string]$_.Exception.Message
      }}

      $faultingProcessId = $null
      $faultingProcessIdSource = ""
      foreach ($name in @("ProcessId", "FaultingProcessId", "AppProcessId", "ApplicationProcessId")) {{
        $pidText = [string]$eventData[$name]
        if ([string]::IsNullOrWhiteSpace($pidText)) {{ continue }}
        try {{
          if ($pidText -match '^0[xX]([0-9A-Fa-f]+)$') {{
            $faultingProcessId = [Convert]::ToInt64($Matches[1], 16)
          }} elseif ($pidText -match '^\d+$') {{
            $faultingProcessId = [Convert]::ToInt64($pidText, 10)
          }}
        }} catch {{
          $faultingProcessId = $null
        }}
        if ($null -ne $faultingProcessId) {{
          $faultingProcessIdSource = "EventData.$name"
          break
        }}
      }}

      [ordered]@{{
        timeCreated = $(if ($_.TimeCreated) {{ $_.TimeCreated.ToUniversalTime().ToString("o") }} else {{ "" }})
        id = [int]$_.Id
        level = [string]$_.LevelDisplayName
        provider = [string]$_.ProviderName
        processId = [int]$_.ProcessId
        eventRecordProcessId = [int]$_.ProcessId
        targetPid = $targetPid
        faultingProcessId = $faultingProcessId
        faultingProcessIdSource = $faultingProcessIdSource
        targetPidMatch = ($null -ne $faultingProcessId -and $faultingProcessId -eq $targetPid)
        atOrAfterAuthorizationFloor = ($null -ne $_.TimeCreated -and $_.TimeCreated.ToUniversalTime() -ge $authorizationStart.UtcDateTime)
        appName = [string]$eventData["AppName"]
        moduleName = [string]$eventData["ModuleName"]
        appPath = [string]$eventData["AppPath"]
        modulePath = [string]$eventData["ModulePath"]
        eventDataError = $eventDataError
        eventData = $eventData
        message = [string]$_.Message
      }}
    }})
}} catch {{
  $queryError = [string]$_.Exception.Message
}}
[ordered]@{{
  queried = $true
  logName = "Application"
  queryStartTime = $queryStart.ToUniversalTime().ToString("o")
  launchEpochMs = {int(launch_epoch_ms)}
  processStartEpochMs = {int(process_start_epoch_ms) if isinstance(process_start_epoch_ms, int) and not isinstance(process_start_epoch_ms, bool) else '$null'}
  authorizationFloorEpochMs = {authorization_floor_ms}
  lookbackReportOnly = $true
  targetPid = $targetPid
  queryError = $queryError
  eventCount = [int]$rows.Count
  events = $rows
}} | ConvertTo-Json -Depth 6 -Compress
'''
    return _powershell_json(script, timeout_sec=20)


def _best_effort(label: str, callback: Any) -> Dict[str, Any]:
    try:
        value = callback()
        return value if isinstance(value, dict) else {"ok": True, "value": value}
    except Exception as exc:
        return {"ok": False, "stage": label, "error": f"{type(exc).__name__}: {exc}"}


def _coerce_pid(value: Any) -> Optional[int]:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        return None


def _get_runtime_events_paginated(since_id: int, total_limit: int = 20000) -> Dict[str, Any]:
    cursor = max(0, int(since_id))
    rows: List[Dict[str, Any]] = []
    pages: List[Dict[str, Any]] = []
    error = ""
    while len(rows) < max(1, total_limit):
        request_limit = min(1000, total_limit - len(rows))
        page = get_runtime_events(since_id=cursor, limit=request_limit)
        batch = [row for row in list(page.get("events", []) or []) if isinstance(row, dict)]
        pages.append({
            "sinceId": cursor,
            "requestLimit": request_limit,
            "ok": bool(page.get("ok")),
            "count": len(batch),
            "latestId": page.get("latestId"),
        })
        if not page.get("ok"):
            error = str(page.get("error", "runtime event query failed"))
            break
        if not batch:
            break

        accepted: List[Dict[str, Any]] = []
        next_cursor = cursor
        for row in batch:
            event_id = _coerce_pid(row.get("id"))
            if event_id is None or event_id <= cursor:
                continue
            accepted.append(row)
            next_cursor = max(next_cursor, event_id)
        rows.extend(accepted)
        if next_cursor <= cursor:
            error = "runtime event pagination did not advance"
            break
        cursor = next_cursor
        if len(batch) < request_limit:
            break

    return {
        "ok": not error,
        "error": error,
        "count": len(rows),
        "events": rows,
        "latestId": cursor,
        "pageCount": len(pages),
        "pages": pages,
        "truncated": len(rows) >= max(1, total_limit),
    }


def _ready_process_instance_contract(
    process_snapshot: Dict[str, Any],
    target_pid: int,
    launch_epoch_ms: int,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    payload_value = process_snapshot.get("payload")
    payload = dict(payload_value) if isinstance(payload_value, dict) else {}
    module_value = process_snapshot.get("moduleEvidence")
    module_evidence = (
        dict(module_value) if isinstance(module_value, dict)
        else dict(payload.get("moduleEvidence", {}) or {})
    )
    snapshot_found = payload.get("found") is True
    raw_pid = payload.get("pid")
    numeric_pid_exact = bool(
        isinstance(raw_pid, int) and not isinstance(raw_pid, bool)
        and raw_pid == target_pid
    )
    process_identity_war3 = _war3_process_identity(
        payload.get("processName"), payload.get("path"),
    )
    raw_process_start_epoch_ms = payload.get("startTimeEpochMs")
    process_start_epoch_ms = (
        raw_process_start_epoch_ms
        if isinstance(raw_process_start_epoch_ms, int)
        and not isinstance(raw_process_start_epoch_ms, bool)
        and raw_process_start_epoch_ms > 0
        else _parse_utc_epoch_ms(payload.get("startTimeUtc"))
    )
    launch_epoch_valid = bool(
        isinstance(launch_epoch_ms, int)
        and not isinstance(launch_epoch_ms, bool)
        and launch_epoch_ms > 0
    )
    start_at_or_after_launch = bool(
        launch_epoch_valid
        and isinstance(process_start_epoch_ms, int)
        and process_start_epoch_ms + PROCESS_INSTANCE_CLOCK_TOLERANCE_MS >=
            launch_epoch_ms
    )
    frozen_fingerprint = dict(launch_fingerprint or {})
    fingerprint_binding = _launch_instance_binding_contract(
        frozen_fingerprint,
        {
            "ok": process_snapshot.get("ok") is True,
            "payload": payload,
        },
        target_pid,
    )
    fingerprint_available = frozen_fingerprint.get("available") is True
    exact_when_available = fingerprint_binding.get("exact") is True
    # Absence is itself non-authoritative. A same-PID process can be replaced
    # after launch, so neither basename nor a post-launch start time can serve
    # as a fallback identity proof.
    known_instance_mismatch = not exact_when_available
    fingerprint_creation_ms = frozen_fingerprint.get("creationEpochMs")
    authorization_floor_ms = (
        max(launch_epoch_ms, fingerprint_creation_ms)
        if launch_epoch_valid
        and isinstance(fingerprint_creation_ms, int)
        and not isinstance(fingerprint_creation_ms, bool)
        and fingerprint_creation_ms > 0
        else None
    )
    loaded_module_paths = sorted({
        _canonical_windows_path(row.get("path"))
        for row in list(payload.get("modules", []) or [])
        if isinstance(row, dict) and _canonical_windows_path(row.get("path"))
    })
    deployed_d3d9_path = _canonical_windows_path(DEPLOYED_DLL)
    deployed_d3d9_loaded = bool(
        module_evidence.get("enumerationComplete") is True
        and module_evidence.get("numericPidExact") is True
        and module_evidence.get("exactLaunchProcessInstance") is True
        and fingerprint_available and exact_when_available
        and deployed_d3d9_path in loaded_module_paths
    )
    return {
        "targetPid": target_pid,
        "launchEpochMs": launch_epoch_ms if launch_epoch_valid else None,
        "snapshotAvailable": snapshot_found,
        "reportedPid": _coerce_pid(raw_pid),
        "numericPidExact": numeric_pid_exact,
        "reportedProcessName": payload.get("processName"),
        "reportedProcessPath": payload.get("path"),
        "processIdentityWar3": process_identity_war3,
        "processStartTimeUtc": payload.get("startTimeUtc"),
        "processStartEpochMs": process_start_epoch_ms,
        "processStartAtOrAfterLaunch": start_at_or_after_launch,
        "clockToleranceMs": PROCESS_INSTANCE_CLOCK_TOLERANCE_MS,
        "exactWhenAvailable": exact_when_available,
        "knownInstanceMismatch": known_instance_mismatch,
        "launchOnlyFallback": False,
        "immutableLaunchFingerprintAvailable": fingerprint_available,
        "immutableLaunchFingerprintCurrentExact": exact_when_available,
        "immutableLaunchFingerprint": json.loads(json.dumps(
            frozen_fingerprint, ensure_ascii=False,
        )),
        "immutableLaunchFingerprintBinding": fingerprint_binding,
        "authorizationFloorEpochMs": authorization_floor_ms,
        "moduleEvidence": module_evidence,
        "loadedModulePaths": loaded_module_paths,
        "deployedD3d9Path": deployed_d3d9_path,
        "deployedD3d9LoadedForExactInstance": deployed_d3d9_loaded,
        "reportOnly": not exact_when_available,
        "failureClassificationAuthority": (
            1 if exact_when_available else 0
        ),
    }


def _windows_event_process_instance_contract(
    row: Dict[str, Any],
    process_instance: Dict[str, Any],
    target_pid: int,
) -> Dict[str, Any]:
    raw_faulting_pid = row.get("faultingProcessId")
    numeric_pid_exact = bool(
        isinstance(raw_faulting_pid, int)
        and not isinstance(raw_faulting_pid, bool)
        and raw_faulting_pid == target_pid
    )
    event_epoch_ms = _parse_utc_epoch_ms(row.get("timeCreated"))
    authorization_floor_ms = process_instance.get(
        "authorizationFloorEpochMs"
    )
    time_exact = bool(
        isinstance(event_epoch_ms, int)
        and isinstance(authorization_floor_ms, int)
        and event_epoch_ms >= authorization_floor_ms
    )
    identity_text = " ".join(
        str(row.get(name, "") or "")
        for name in ("appName", "appPath", "message")
    )
    world_editor = bool(re.search(
        r"(?:worldeditydwe|world\s*editor)", identity_text,
        re.IGNORECASE,
    ))
    app_identity_war3 = bool(
        _war3_process_identity(row.get("appName"), row.get("appPath"))
        or re.search(r"\bwar3[.]exe\b", identity_text, re.IGNORECASE)
    )
    process_instance_usable = bool(
        process_instance.get("immutableLaunchFingerprintAvailable") is True
        and process_instance.get(
            "immutableLaunchFingerprintCurrentExact"
        ) is True
    )
    exact = bool(
        numeric_pid_exact and time_exact and app_identity_war3
        and not world_editor and process_instance_usable
    )
    reasons: List[str] = []
    if not numeric_pid_exact:
        reasons.append("faultingPidNotNumericExact")
    if not time_exact:
        reasons.append("beforeProcessInstanceAuthorizationFloor")
    if not app_identity_war3:
        reasons.append("appIdentityNotWar3")
    if world_editor:
        reasons.append("worldEditorIdentity")
    if not process_instance_usable:
        reasons.append("immutableLaunchFingerprintUnavailableOrMismatch")
    return {
        "exact": exact,
        "targetPid": target_pid,
        "reportedFaultingPid": _coerce_pid(raw_faulting_pid),
        "numericPidExact": numeric_pid_exact,
        "eventTimeCreated": row.get("timeCreated"),
        "eventEpochMs": event_epoch_ms,
        "authorizationFloorEpochMs": authorization_floor_ms,
        "timeExact": time_exact,
        "appIdentityWar3": app_identity_war3,
        "worldEditorIdentity": world_editor,
        "processInstanceUsable": process_instance_usable,
        "rejectionReasons": reasons,
        "reportOnly": not exact,
        "failureClassificationAuthority": 1 if exact else 0,
    }


def _windows_event_gpu_skin_contract(
    row: Dict[str, Any], process_instance: Dict[str, Any],
) -> Dict[str, Any]:
    marker_text = " ".join(
        str(row.get(name, "") or "")
        for name in ("message", "moduleName", "modulePath")
    )
    explicit_marker = bool(re.search(
        r"(?:War3GpuSkin|\bdxvk\b)", marker_text, re.IGNORECASE,
    ))
    module_name = os.path.basename(
        str(row.get("moduleName", "") or "")
    ).lower()
    module_path = _canonical_windows_path(row.get("modulePath"))
    deployed_path = str(process_instance.get("deployedD3d9Path", "") or "")
    deployed_module_exact = bool(
        module_name == "d3d9.dll"
        and module_path and module_path == deployed_path
    )
    loaded_proof = process_instance.get(
        "deployedD3d9LoadedForExactInstance"
    ) is True
    exact = bool(explicit_marker or deployed_module_exact and loaded_proof)
    return {
        "exact": exact,
        "explicitWar3GpuSkinOrDxvkMarker": explicit_marker,
        "moduleName": module_name,
        "modulePath": module_path,
        "deployedModulePathExact": deployed_module_exact,
        "currentExactProcessModuleLoaded": loaded_proof,
        "systemOrUnprovedD3d9ReportOnlyForGpuAttribution": bool(
            module_name == "d3d9.dll" and not exact
        ),
    }


def _ready_failure_classification(
    ready: Dict[str, Any],
    process_state: Dict[str, Any],
    target_text: str,
    windows_events: Dict[str, Any],
    target_pid: int,
    launch_epoch_ms: int,
    process_snapshot: Dict[str, Any],
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    current_fingerprint_validation: Optional[Dict[str, Any]] = None,
    retained_termination_proof: Optional[Dict[str, Any]] = None,
    unattributed_text: str = "",
) -> Dict[str, Any]:
    clean_text = _strip_ansi(target_text)
    clean_unattributed_text = _strip_ansi(unattributed_text)
    raw_target_crash_lines = _scan_crash_text(clean_text)
    raw_target_gpu_skin_failure_lines = [
        line for line in clean_text.splitlines()
        if "GpuSkin" in line and GPU_SKIN_FAILURE_RE.search(line)
    ][-100:]
    event_payload = windows_events.get("payload")
    raw_event_rows = event_payload.get("events", []) if isinstance(event_payload, dict) else []
    if isinstance(raw_event_rows, dict):
        event_rows = [raw_event_rows]
    elif isinstance(raw_event_rows, list):
        event_rows = [row for row in raw_event_rows if isinstance(row, dict)]
    else:
        event_rows = []
    process_instance = _ready_process_instance_contract(
        process_snapshot, target_pid, launch_epoch_ms, launch_fingerprint,
    )
    live_validation_supplied = isinstance(
        current_fingerprint_validation, dict,
    )
    live_validation = dict(current_fingerprint_validation or {})
    live_validation_exact = bool(
        live_validation_supplied
        and live_validation.get("exact") is True
    )
    snapshot_and_current_exact = bool(
        process_instance.get(
            "immutableLaunchFingerprintCurrentExact"
        ) is True
        and live_validation_exact
    )
    process_instance["postCollectionFingerprintValidation"] = (
        live_validation if live_validation_supplied else {
            "notSuppliedFailClosed": True,
            "exact": False,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
        }
    )
    process_instance["postCollectionFingerprintExact"] = (
        live_validation_exact
    )
    authority_validation = (
        live_validation if live_validation_supplied else {
            "exact": False,
            "fingerprintAvailable": False,
            "currentValidationNotSupplied": True,
            "reportOnly": True,
            "failureClassificationAuthority": 0,
        }
    )
    process_authority = _process_liveness_authority_contract(
        authority_validation, process_state, retained_termination_proof,
    )
    hard_current_exact = bool(
        snapshot_and_current_exact
        and process_authority.get("aliveExact") is True
    )
    process_instance["immutableLaunchFingerprintCurrentExact"] = (
        hard_current_exact
    )
    process_instance["exactWhenAvailable"] = hard_current_exact
    process_instance["knownInstanceMismatch"] = not hard_current_exact
    process_instance["reportOnly"] = not hard_current_exact
    process_instance["failureClassificationAuthority"] = (
        1 if hard_current_exact else 0
    )
    process_instance["processLivenessAuthority"] = process_authority
    if not hard_current_exact:
        process_instance["deployedD3d9LoadedForExactInstance"] = False
    target_dbwin_authoritative = bool(
        process_instance.get("immutableLaunchFingerprintAvailable") is True
        and process_instance.get(
            "immutableLaunchFingerprintCurrentExact"
        ) is True
    )
    crash_lines = (
        raw_target_crash_lines if target_dbwin_authoritative else []
    )
    gpu_skin_failure_lines = (
        raw_target_gpu_skin_failure_lines
        if target_dbwin_authoritative else []
    )
    target_windows_events: List[Dict[str, Any]] = []
    same_pid_report_only_events: List[Dict[str, Any]] = []
    foreign_windows_events: List[Dict[str, Any]] = []
    unattributed_windows_events: List[Dict[str, Any]] = []
    gpu_windows_events: List[Dict[str, Any]] = []
    for raw_row in event_rows:
        row = dict(raw_row)
        instance_contract = _windows_event_process_instance_contract(
            row, process_instance, target_pid,
        )
        row["targetProcessInstanceContract"] = instance_contract
        faulting_pid = _coerce_pid(row.get("faultingProcessId"))
        if instance_contract.get("exact") is True:
            gpu_contract = _windows_event_gpu_skin_contract(
                row, process_instance,
            )
            row["gpuSkinAttributionContract"] = gpu_contract
            target_windows_events.append(row)
            if gpu_contract.get("exact") is True:
                gpu_windows_events.append(row)
        elif faulting_pid == target_pid:
            same_pid_report_only_events.append(row)
        elif faulting_pid is None:
            unattributed_windows_events.append(row)
        else:
            foreign_windows_events.append(row)
    process_state_available = bool(process_state.get("ok")) and "running" in process_state
    process_alive = process_authority.get("aliveExact") is True
    process_terminated = process_authority.get("terminatedExact") is True
    gpu_skin_runtime_failure = bool(gpu_skin_failure_lines or gpu_windows_events)
    runtime_process_failure = bool(
        process_terminated or crash_lines or target_windows_events
    )
    ready_infrastructure_failure = bool(
        not ready.get("ok")
        and process_alive
        and not gpu_skin_runtime_failure
        and not runtime_process_failure
    )
    if gpu_skin_runtime_failure:
        primary = "gpuSkinRuntimeFailure"
    elif runtime_process_failure:
        primary = "runtimeProcessFailure"
    elif ready_infrastructure_failure:
        primary = "readyInfrastructureFailure"
    else:
        primary = "indeterminateReadyFailure"
    return {
        "primary": primary,
        "readyInfrastructureFailure": ready_infrastructure_failure,
        "gpuSkinRuntimeFailure": gpu_skin_runtime_failure,
        "runtimeProcessFailure": runtime_process_failure,
        "processAliveAtEvidenceCapture": process_alive,
        "processTerminatedAtEvidenceCapture": process_terminated,
        "processLivenessIndeterminate": (
            process_authority.get("indeterminate") is True
        ),
        "processLivenessAuthority": process_authority,
        "processStateAvailable": process_state_available,
        "readyMode": ready.get("mode"),
        "readyError": ready.get("error"),
        "targetPid": target_pid,
        "immutableLaunchFingerprintCurrentExact": (
            target_dbwin_authoritative
        ),
        "eventAndDbwinFailureClassificationAuthority": (
            1 if target_dbwin_authoritative else 0
        ),
        "crashLines": crash_lines,
        "gpuSkinFailureLines": gpu_skin_failure_lines,
        "samePidDbwinCrashLinesReportOnly": (
            [] if target_dbwin_authoritative
            else raw_target_crash_lines
        ),
        "samePidDbwinGpuSkinFailureLinesReportOnly": (
            [] if target_dbwin_authoritative
            else raw_target_gpu_skin_failure_lines
        ),
        "targetDbwinFailureAuthority": (
            1 if target_dbwin_authoritative else 0
        ),
        "unattributedCrashLines": _scan_crash_text(clean_unattributed_text),
        "unattributedGpuSkinFailureLines": [
            line for line in clean_unattributed_text.splitlines()
            if "GpuSkin" in line and GPU_SKIN_FAILURE_RE.search(line)
        ][-100:],
        "relevantWindowsEvents": target_windows_events,
        "targetWindowsEvents": target_windows_events,
        "samePidReportOnlyWindowsEvents": same_pid_report_only_events,
        "foreignWindowsEvents": foreign_windows_events,
        "unattributedWindowsEvents": unattributed_windows_events,
        "gpuWindowsEvents": gpu_windows_events,
        "windowsEventAttribution": {
            "targetPid": target_pid,
            "launchEpochMs": launch_epoch_ms,
            "exactFaultingPidRequired": True,
            "exactProcessInstanceTimeRequired": True,
            "immutableLaunchFingerprintRequired": True,
            "war3ApplicationIdentityRequired": True,
            "targetEventsAuthoritative": bool(
                process_instance.get(
                    "immutableLaunchFingerprintCurrentExact"
                ) is True
            ),
            "failureClassificationAuthority": (
                1 if process_instance.get(
                    "immutableLaunchFingerprintCurrentExact"
                ) is True else 0
            ),
            "samePidRejectedEventsReportOnly": True,
            "foreignEventsReportOnly": True,
            "unattributedEventsReportOnly": True,
            "targetEventCount": len(target_windows_events),
            "samePidReportOnlyEventCount": len(
                same_pid_report_only_events
            ),
            "foreignEventCount": len(foreign_windows_events),
            "unattributedEventCount": len(unattributed_windows_events),
            "gpuEventCount": len(gpu_windows_events),
            "processInstance": process_instance,
        },
    }


def _ready_failure_attribution_synthetic_self_tests() -> Dict[str, Any]:
    target_pid = 39976
    launch_epoch_ms = 1_752_572_800_000
    fingerprint_creation_ms = launch_epoch_ms + 100
    launch_fingerprint = {
        "available": True,
        "immutableCopy": True,
        "capturedBeforeReadyWait": True,
        "pid": target_pid,
        "creationEpochMs": fingerprint_creation_ms,
        "canonicalExePath": str(WAR3_DIR / "war3.exe"),
        "launcherMode": "direct",
    }
    ready = {"ok": False, "mode": "pipe", "error": "ready timeout"}
    process_state = {"ok": True, "running": True}

    def iso(epoch_ms: int) -> str:
        return datetime.fromtimestamp(
            epoch_ms / 1000.0, timezone.utc,
        ).isoformat().replace("+00:00", "Z")

    def snapshot(
        start_epoch_ms: int = fingerprint_creation_ms,
        include_deployed_d3d9: bool = True,
        process_path: Optional[str] = None,
        fingerprint: Optional[Dict[str, Any]] = launch_fingerprint,
    ) -> Dict[str, Any]:
        modules = [
            {"name": "war3.exe", "path": str(WAR3_DIR / "war3.exe")},
            {"name": "Game.dll", "path": str(GAME_DLL)},
        ]
        if include_deployed_d3d9:
            modules.append({
                "name": "d3d9.dll", "path": str(DEPLOYED_DLL),
            })
        return _module_snapshot_contract({
            "ok": True,
            "payload": {
                "found": True,
                "pid": target_pid,
                "processName": "war3",
                "path": process_path or str(WAR3_DIR / "war3.exe"),
                "startTimeUtc": iso(start_epoch_ms),
                "startTimeEpochMs": start_epoch_ms,
                "moduleEnumerationBackend": "syswow64-get-process-modules",
                "moduleEnumerationComplete": True,
                "moduleEnumerationError": "",
                "modules": modules,
            },
            "parseError": "",
            "query": {"returncode": 0},
        }, target_pid, [], launch_epoch_ms, fingerprint)

    exact_snapshot = snapshot()
    default_current_validation = {
        "exact": True,
        "fingerprintAvailable": True,
        "syntheticFreshValidation": True,
        "failureClassificationAuthority": 1,
    }
    current_validation_default = object()

    def classify(
        *rows: Dict[str, Any],
        process: Optional[Dict[str, Any]] = None,
        fingerprint: Optional[Dict[str, Any]] = launch_fingerprint,
        current_validation: Any = current_validation_default,
        state: Optional[Dict[str, Any]] = None,
        retained: Optional[Dict[str, Any]] = None,
        target_text: str = "",
    ) -> Dict[str, Any]:
        effective_current_validation = (
            default_current_validation
            if current_validation is current_validation_default
            else current_validation
        )
        return _ready_failure_classification(
            ready, state or process_state, target_text,
            {"payload": {"events": list(rows)}},
            target_pid, launch_epoch_ms, process or exact_snapshot,
            fingerprint,
            effective_current_validation,
            retained,
        )

    prelaunch_same_pid_world_editor = classify({
        "timeCreated": iso(launch_epoch_ms - 5_000),
        "faultingProcessId": target_pid,
        "appName": "worldeditydwe.exe",
        "moduleName": "RenderEdge_Widescreen.mix",
        "message": "Application Error world editor",
    })
    prelaunch_same_pid_war3 = classify({
        "timeCreated": iso(launch_epoch_ms - 1),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    })
    foreign_world_editor = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid + 1,
        "appName": "worldeditydwe.exe",
        "moduleName": "RenderEdge_Widescreen.mix",
        "message": "Application Error",
    })
    unattributed_world_editor = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": None,
        "appName": "worldeditydwe.exe",
        "moduleName": "RenderEdge_Widescreen.mix",
        "message": "Application Error",
    })
    exact_game = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error",
    })
    exact_d3d9 = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "d3d9.dll",
        "modulePath": str(DEPLOYED_DLL),
        "message": "Application Error war3.exe",
    })
    system_d3d9 = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "d3d9.dll",
        "modulePath": r"C:\Windows\SysWOW64\d3d9.dll",
        "message": "Application Error war3.exe",
    })
    deployed_d3d9_without_loaded_proof = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "d3d9.dll",
        "modulePath": str(DEPLOYED_DLL),
        "message": "Application Error war3.exe",
    }, process=snapshot(include_deployed_d3d9=False))
    explicit_dxvk = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "unknown.dll",
        "message": "DXVK War3GpuSkin fatal marker",
    })
    reused_pid = classify({
        "timeCreated": iso(launch_epoch_ms + 1_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, process=snapshot(launch_epoch_ms - 10_000), current_validation={
        "exact": False,
        "fingerprintAvailable": True,
        "reason": "same PID creation predates launch fingerprint",
    })
    postlaunch_foreign_path = classify({
        "timeCreated": iso(launch_epoch_ms + 2_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "appPath": r"C:\Temp\war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, process=snapshot(
        launch_epoch_ms + 1_000,
        process_path=r"C:\Temp\war3.exe",
    ), current_validation={
        "exact": False,
        "fingerprintAvailable": True,
        "reason": "current canonical path mismatch",
    })
    same_path_replacement = classify({
        "timeCreated": iso(launch_epoch_ms + 2_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "appPath": str(WAR3_DIR / "war3.exe"),
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, process=snapshot(fingerprint_creation_ms + 1), current_validation={
        "exact": False,
        "fingerprintAvailable": True,
        "reason": "current creation epoch mismatch",
    })
    snapshot_unavailable = classify({
        "timeCreated": iso(launch_epoch_ms + 2_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, process={"ok": False, "payload": {}, "moduleEvidence": {}})
    fingerprint_unavailable = classify({
        "timeCreated": iso(launch_epoch_ms + 2_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, fingerprint={}, current_validation={
        "exact": False,
        "fingerprintAvailable": False,
        "reason": "immutable fingerprint unavailable",
    })
    changed_after_snapshot = classify({
        "timeCreated": iso(launch_epoch_ms + 2_000),
        "faultingProcessId": target_pid,
        "appName": "war3.exe",
        "moduleName": "Game.dll",
        "message": "Application Error war3.exe",
    }, current_validation={
        "exact": False,
        "fingerprintAvailable": True,
        "reason": "same PID was replaced after module snapshot",
    })
    unavailable_and_not_running = classify(
        fingerprint={},
        state={"ok": True, "running": False},
    )
    mismatch_and_not_running = classify(
        current_validation={
            "exact": False,
            "fingerprintAvailable": True,
            "reason": "same PID instance mismatch",
        },
        state={"ok": True, "running": False},
    )
    retained_termination = classify(
        current_validation={
            "exact": False,
            "fingerprintAvailable": True,
            "reason": "original process no longer queryable",
        },
        state={"ok": True, "running": False},
        retained={
            "exact": True,
            "shapeExact": True,
            "handleSignaled": True,
            "returnCode": 0,
        },
    )
    collection_mismatch_dbwin = classify(
        current_validation={
            "exact": False,
            "fingerprintAvailable": True,
            "reason": "replacement observed during collection",
        },
        state={"ok": True, "running": True},
        target_text="DXVK War3GpuSkin fatal access violation",
    )
    clean_then_replacement_alive = classify(
        current_validation={
            "exact": False,
            "fingerprintAvailable": True,
            "reason": "clean evidence belongs to prior instance",
        },
        state={"ok": True, "running": True},
    )
    replacement_clean_gate = _hard_process_evidence_contract(
        clean_then_replacement_alive.get(
            "processLivenessAuthority", {}
        ),
        True,
        [],
    )
    exact_clean_gate = _hard_process_evidence_contract(
        exact_game.get("processLivenessAuthority", {}),
        True,
        [],
    )
    missing_current_validation = classify(current_validation=None)
    checks = {
        "prelaunchSamePidWorldEditorReportOnly": bool(
            prelaunch_same_pid_world_editor.get(
                "readyInfrastructureFailure"
            ) is True
            and prelaunch_same_pid_world_editor.get(
                "runtimeProcessFailure"
            ) is False
            and len(prelaunch_same_pid_world_editor.get(
                "samePidReportOnlyWindowsEvents", []
            )) == 1
        ),
        "prelaunchSamePidOldWar3ReportOnly": bool(
            prelaunch_same_pid_war3.get("readyInfrastructureFailure") is True
            and prelaunch_same_pid_war3.get("runtimeProcessFailure") is False
            and len(prelaunch_same_pid_war3.get(
                "samePidReportOnlyWindowsEvents", []
            )) == 1
        ),
        "pidReuseKnownOldInstanceReportOnly": bool(
            reused_pid.get("primary") == "indeterminateReadyFailure"
            and reused_pid.get("readyInfrastructureFailure") is False
            and reused_pid.get("runtimeProcessFailure") is False
            and reused_pid.get("windowsEventAttribution", {}).get(
                "processInstance", {}
            ).get("knownInstanceMismatch") is True
        ),
        "postlaunchForeignPathSamePidReportOnly": bool(
            postlaunch_foreign_path.get("primary") ==
                "indeterminateReadyFailure"
            and postlaunch_foreign_path.get(
                "readyInfrastructureFailure"
            ) is False
            and postlaunch_foreign_path.get("runtimeProcessFailure") is False
            and len(postlaunch_foreign_path.get(
                "samePidReportOnlyWindowsEvents", []
            )) == 1
            and postlaunch_foreign_path.get(
                "windowsEventAttribution", {}
            ).get("processInstance", {}).get(
                "immutableLaunchFingerprintCurrentExact"
            ) is False
        ),
        "samePathReplacementCreationReportOnly": bool(
            same_path_replacement.get("primary") ==
                "indeterminateReadyFailure"
            and same_path_replacement.get(
                "readyInfrastructureFailure"
            ) is False
            and same_path_replacement.get("runtimeProcessFailure") is False
            and len(same_path_replacement.get(
                "samePidReportOnlyWindowsEvents", []
            )) == 1
        ),
        "snapshotUnavailableNeverAuthorizes": bool(
            snapshot_unavailable.get("primary") ==
                "readyInfrastructureFailure"
            and snapshot_unavailable.get(
                "readyInfrastructureFailure"
            ) is True
            and snapshot_unavailable.get("runtimeProcessFailure") is False
            and snapshot_unavailable.get(
                "windowsEventAttribution", {}
            ).get("processInstance", {}).get(
                "failureClassificationAuthority"
            ) == 0
        ),
        "fingerprintUnavailableNeverAuthorizes": bool(
            fingerprint_unavailable.get("primary") ==
                "indeterminateReadyFailure"
            and fingerprint_unavailable.get(
                "readyInfrastructureFailure"
            ) is False
            and fingerprint_unavailable.get("runtimeProcessFailure") is False
            and fingerprint_unavailable.get(
                "windowsEventAttribution", {}
            ).get("processInstance", {}).get(
                "immutableLaunchFingerprintAvailable"
            ) is False
        ),
        "replacementAfterSnapshotNeverAuthorizes": bool(
            changed_after_snapshot.get("primary") ==
                "indeterminateReadyFailure"
            and changed_after_snapshot.get(
                "readyInfrastructureFailure"
            ) is False
            and changed_after_snapshot.get("runtimeProcessFailure") is False
            and changed_after_snapshot.get(
                "windowsEventAttribution", {}
            ).get("failureClassificationAuthority") == 0
        ),
        "foreignWorldEditorReportOnly": bool(
            foreign_world_editor.get("readyInfrastructureFailure") is True
            and foreign_world_editor.get("runtimeProcessFailure") is False
            and foreign_world_editor.get("gpuSkinRuntimeFailure") is False
            and len(foreign_world_editor.get("foreignWindowsEvents", [])) == 1
        ),
        "unattributedWorldEditorReportOnly": bool(
            unattributed_world_editor.get("readyInfrastructureFailure") is True
            and unattributed_world_editor.get("runtimeProcessFailure") is False
            and unattributed_world_editor.get("gpuSkinRuntimeFailure") is False
            and len(
                unattributed_world_editor.get("unattributedWindowsEvents", [])
            ) == 1
        ),
        "exactGameIsRuntimeOnly": bool(
            exact_game.get("runtimeProcessFailure") is True
            and exact_game.get("gpuSkinRuntimeFailure") is False
        ),
        "exactDeployedD3d9LoadedIsGpuSkinRuntime": bool(
            exact_d3d9.get("runtimeProcessFailure") is True
            and exact_d3d9.get("gpuSkinRuntimeFailure") is True
        ),
        "systemD3d9IsRuntimeOnly": bool(
            system_d3d9.get("runtimeProcessFailure") is True
            and system_d3d9.get("gpuSkinRuntimeFailure") is False
        ),
        "deployedD3d9RequiresCurrentLoadedProof": bool(
            deployed_d3d9_without_loaded_proof.get(
                "runtimeProcessFailure"
            ) is True
            and deployed_d3d9_without_loaded_proof.get(
                "gpuSkinRuntimeFailure"
            ) is False
        ),
        "explicitDxvkMarkerIsGpuSkinRuntime": bool(
            explicit_dxvk.get("runtimeProcessFailure") is True
            and explicit_dxvk.get("gpuSkinRuntimeFailure") is True
        ),
        "unavailableAndNotRunningIndeterminate": bool(
            unavailable_and_not_running.get("primary") ==
                "indeterminateReadyFailure"
            and unavailable_and_not_running.get(
                "runtimeProcessFailure"
            ) is False
            and unavailable_and_not_running.get(
                "processLivenessAuthority", {}
            ).get("failureClassificationAuthority") == 0
        ),
        "mismatchAndNotRunningIndeterminate": bool(
            mismatch_and_not_running.get("primary") ==
                "indeterminateReadyFailure"
            and mismatch_and_not_running.get(
                "runtimeProcessFailure"
            ) is False
            and mismatch_and_not_running.get(
                "processLivenessIndeterminate"
            ) is True
        ),
        "retainedHandleTerminationAuthorizesDeath": bool(
            retained_termination.get("primary") ==
                "runtimeProcessFailure"
            and retained_termination.get("runtimeProcessFailure") is True
            and retained_termination.get(
                "processTerminatedAtEvidenceCapture"
            ) is True
        ),
        "collectionMismatchDbwinReportOnly": bool(
            collection_mismatch_dbwin.get("gpuSkinRuntimeFailure") is False
            and collection_mismatch_dbwin.get(
                "runtimeProcessFailure"
            ) is False
            and len(collection_mismatch_dbwin.get(
                "samePidDbwinCrashLinesReportOnly", []
            )) == 1
            and collection_mismatch_dbwin.get(
                "targetDbwinFailureAuthority"
            ) == 0
        ),
        "cleanEvidenceThenReplacementAliveCannotPassAlive": bool(
            clean_then_replacement_alive.get(
                "processAliveAtEvidenceCapture"
            ) is False
            and clean_then_replacement_alive.get(
                "processLivenessIndeterminate"
            ) is True
            and clean_then_replacement_alive.get(
                "runtimeProcessFailure"
            ) is False
            and replacement_clean_gate.get("processAlive") is False
            and replacement_clean_gate.get("crashScanClean") is False
            and replacement_clean_gate.get(
                "failureClassificationAuthority"
            ) == 0
        ),
        "exactAliveCleanEvidenceCanAuthorize": bool(
            exact_clean_gate.get("processAlive") is True
            and exact_clean_gate.get("crashScanClean") is True
            and exact_clean_gate.get(
                "failureClassificationAuthority"
            ) == 1
        ),
        "missingCurrentValidationFailsClosed": bool(
            missing_current_validation.get("primary") ==
                "indeterminateReadyFailure"
            and missing_current_validation.get(
                "readyInfrastructureFailure"
            ) is False
            and missing_current_validation.get(
                "runtimeProcessFailure"
            ) is False
            and missing_current_validation.get(
                "processLivenessIndeterminate"
            ) is True
            and missing_current_validation.get(
                "eventAndDbwinFailureClassificationAuthority"
            ) == 0
        ),
    }
    if not all(checks.values()):
        raise AssertionError(
            "ready-failure attribution synthetic self-test failed: "
            + json.dumps(checks, sort_keys=True)
        )
    return {"ok": True, "count": len(checks), "checks": checks}


def _run_bounded_hidden_python_helper(
    command: List[str], total_timeout_sec: float,
) -> Dict[str, Any]:
    """在严格时限内运行隐藏的 Python helper，并确保回收子进程。"""
    started = time.monotonic()
    budget = max(0.25, float(total_timeout_sec))
    deadline = started + budget
    # 预留 20% 给 terminate/wait 与 kill/wait，不让查询阻塞吃完全部时限。
    execution_budget = max(0.05, budget * 0.80)
    terminate_budget = max(0.05, budget * 0.10)
    kill_budget = max(0.05, budget - execution_budget - terminate_budget)
    report: Dict[str, Any] = {
        "diagnosticOnly": True,
        "authority": False,
        "classificationAuthority": 0,
        "command": [str(part) for part in command],
        "shell": False,
        "hiddenRequested": True,
        "closeFds": True,
        "totalTimeoutSec": budget,
        "executionTimeoutSec": execution_budget,
        "terminateWaitSec": terminate_budget,
        "killWaitSec": kill_budget,
        "started": False,
        "timedOut": False,
        "terminateIssued": False,
        "terminateWaitCompleted": False,
        "killIssued": False,
        "killWaitCompleted": False,
        "helperResidual": False,
    }
    process: Optional[subprocess.Popen] = None
    stdout_text = ""
    try:
        startupinfo = None
        creationflags = 0
        if os.name == "nt":
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = 0
            creationflags |= int(
                getattr(subprocess, "CREATE_NO_WINDOW", 0)
            )
        report["hiddenApplied"] = bool(
            os.name == "nt" and creationflags != 0
        )
        process = subprocess.Popen(
            [str(part) for part in command],
            cwd=str(ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            shell=False,
            close_fds=True,
            startupinfo=startupinfo,
            creationflags=creationflags,
        )
        report["started"] = True
        report["helperPid"] = int(process.pid)
        remaining_after_start = max(0.01, deadline - time.monotonic())
        effective_execution_budget = max(
            0.01,
            remaining_after_start - terminate_budget - kill_budget,
        )
        report["effectiveExecutionTimeoutSec"] = (
            effective_execution_budget
        )
        try:
            stdout_text, _ = process.communicate(
                timeout=effective_execution_budget,
            )
        except subprocess.TimeoutExpired:
            report["timedOut"] = True
            try:
                process.terminate()
                report["terminateIssued"] = True
            except Exception as exc:
                report["terminateError"] = (
                    f"{type(exc).__name__}: {exc}"
                )
            remaining_for_cleanup = max(0.01, deadline - time.monotonic())
            effective_terminate_budget = max(
                0.01,
                min(
                    terminate_budget,
                    remaining_for_cleanup
                    - min(kill_budget, remaining_for_cleanup * 0.5),
                ),
            )
            report["effectiveTerminateWaitSec"] = (
                effective_terminate_budget
            )
            try:
                process.wait(timeout=effective_terminate_budget)
                report["terminateWaitCompleted"] = True
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                    report["killIssued"] = True
                except Exception as exc:
                    report["killError"] = (
                        f"{type(exc).__name__}: {exc}"
                    )
                effective_kill_budget = max(
                    0.01,
                    min(kill_budget, deadline - time.monotonic()),
                )
                report["effectiveKillWaitSec"] = effective_kill_budget
                try:
                    process.wait(timeout=effective_kill_budget)
                    report["killWaitCompleted"] = True
                except subprocess.TimeoutExpired:
                    report["killWaitTimedOut"] = True
            if (
                process.poll() is not None
                and process.stdout is not None
                and not process.stdout.closed
            ):
                try:
                    stdout_text = process.stdout.read()
                except Exception as exc:
                    report["stdoutReadError"] = (
                        f"{type(exc).__name__}: {exc}"
                    )
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        # 任何启动、超时或读取异常都不得留下 Python helper。
        if process is not None and process.poll() is None:
            report["emergencyCleanup"] = True
            if not report.get("terminateIssued"):
                try:
                    process.terminate()
                    report["terminateIssued"] = True
                except Exception as exc:
                    report["emergencyTerminateError"] = (
                        f"{type(exc).__name__}: {exc}"
                    )
                try:
                    process.wait(timeout=0.25)
                    report["terminateWaitCompleted"] = True
                except subprocess.TimeoutExpired:
                    pass
            if process.poll() is None:
                try:
                    process.kill()
                    report["killIssued"] = True
                except Exception as exc:
                    report["emergencyKillError"] = (
                        f"{type(exc).__name__}: {exc}"
                    )
                try:
                    process.wait(timeout=1.0)
                    report["killWaitCompleted"] = True
                except subprocess.TimeoutExpired:
                    report["emergencyKillWaitTimedOut"] = True
        if process is not None:
            report["returncode"] = process.poll()
            report["helperResidual"] = process.poll() is None
            if process.stdout is not None:
                try:
                    if not process.stdout.closed:
                        process.stdout.close()
                except Exception:
                    pass
        report["stdout"] = str(stdout_text or "")[-65536:]
        report["durationMs"] = round(
            (time.monotonic() - started) * 1000.0, 3,
        )
        report["deadlineExceededForMandatoryCleanup"] = bool(
            report["durationMs"] > budget * 1000.0
        )
        report["ok"] = bool(
            report.get("started") is True
            and report.get("timedOut") is not True
            and report.get("returncode") == 0
            and report.get("helperResidual") is False
            and report.get("deadlineExceededForMandatoryCleanup") is False
        )
    return report


def _wait_chain_retained_handle_unsignaled_exact(
    authority: Dict[str, Any],
    fingerprint: Dict[str, Any],
    witness: Dict[str, Any],
    pid: int,
) -> bool:
    termination = dict(
        authority.get("retainedTerminationProof", {}) or {}
    )
    native_termination = dict(
        termination.get("nativeTerminationProof", {}) or {}
    )
    validation = dict(
        authority.get("immutableLaunchFingerprintValidation", {}) or {}
    )
    expected_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    frozen_creation = fingerprint.get("creationEpochMs")
    return bool(
        fingerprint.get("available") is True
        and fingerprint.get("launcherMode") == "direct"
        and fingerprint.get("pid") == int(pid)
        and isinstance(frozen_creation, int)
        and not isinstance(frozen_creation, bool)
        and frozen_creation > 0
        and _canonical_windows_path(
            fingerprint.get("canonicalExePath")
        ) == expected_path
        # current query 允许暂时不可用；但它返回的目标数字 PID
        # 与冻结指纹形状仍必须闭合，不得接受 PID 漂移。
        and validation.get("fingerprintAvailable") is True
        and validation.get("fingerprintShapeExact") is True
        and validation.get("numericPidExact") is True
        and witness.get("shapeExact") is True
        and witness.get("targetPid") == int(pid)
        and witness.get("fingerprintCreationEpochMs") == frozen_creation
        and witness.get("retainedProcessPid") == int(pid)
        and witness.get("retainedProcessCreationEpochMs") == frozen_creation
        and _canonical_windows_path(
            witness.get("retainedProcessCanonicalExePath")
        ) == expected_path
        and witness.get("_nativeProcessWitness") is not None
        and termination.get("shapeExact") is True
        and termination.get("bindingExact") is True
        and termination.get("targetPid") == int(pid)
        and termination.get("fingerprintCreationEpochMs") == frozen_creation
        and termination.get("retainedProcessPid") == int(pid)
        and termination.get("retainedProcessCreationEpochMs") == frozen_creation
        and _canonical_windows_path(
            termination.get("retainedProcessCanonicalExePath")
        ) == expected_path
        and termination.get("handleSignaled") is False
        and termination.get("exact") is False
        and not str(termination.get("pollError", "") or "")
        and native_termination.get("bindingExact") is True
        and native_termination.get("handleSignaled") is False
        and native_termination.get("exact") is False
        and native_termination.get("exitCode") is None
        and not str(native_termination.get("pollError", "") or "")
    )


def _capture_ready_failure_wait_chains(
    output_path: Path,
    pid: int,
    launch_fingerprint: Optional[Dict[str, Any]],
    retained_witness: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    """在目标还由启动句柄精确证明存活时，用短命 helper 采集 WCT。"""
    report: Dict[str, Any] = {
        "diagnosticOnly": True,
        "authority": False,
        "classificationAuthority": 0,
        "reason": "等待链只用于定位阻塞，不参与 P4 成败分类",
        "pid": int(pid),
        "outputPath": str(output_path),
        "collector": str(WAIT_CHAIN_COLLECTOR),
        "helperTimeoutSec": WAIT_CHAIN_HELPER_TIMEOUT_SEC,
        "maxThreads": WAIT_CHAIN_HELPER_MAX_THREADS,
        "ok": False,
    }
    fingerprint = dict(launch_fingerprint or {})
    witness = retained_witness if isinstance(retained_witness, dict) else {}
    native_process = witness.get("_nativeProcessWitness")
    witness_before = _retained_launch_handle_witness_report(witness)
    stale_cleanup: List[Dict[str, Any]] = []
    candidates = [output_path]
    try:
        candidates.extend(output_path.parent.glob(
            f".{output_path.name}.*.tmp"
        ))
    except OSError as exc:
        stale_cleanup.append({
            "ok": False,
            "path": str(output_path.parent),
            "error": f"{type(exc).__name__}: {exc}",
        })
    for candidate in candidates:
        try:
            candidate.unlink(missing_ok=True)
            stale_cleanup.append({"ok": True, "path": str(candidate)})
        except OSError as exc:
            stale_cleanup.append({
                "ok": False,
                "path": str(candidate),
                "error": f"{type(exc).__name__}: {exc}",
            })
    report["staleOutputCleanup"] = stale_cleanup

    pre_authority = _capture_fresh_process_authority(
        pid, fingerprint, retained_witness=witness,
    )
    report["preProcessAuthorityReportOnly"] = pre_authority
    report["retainedWitnessBefore"] = witness_before
    report["preRetainedHandleBindingExact"] = (
        _wait_chain_retained_handle_unsignaled_exact(
            pre_authority, fingerprint, witness, pid,
        )
    )
    if not report["preRetainedHandleBindingExact"]:
        report["skipped"] = True
        report["skipReason"] = (
            "采集前无法由冻结指纹与 retained HANDLE 独立闭合 "
            "PID/创建时间/路径/未触发状态"
        )
        report["postProcessAuthorityReportOnly"] = (
            _capture_fresh_process_authority(
                pid, fingerprint, retained_witness=witness,
            )
        )
        return report

    helper_command = [
        sys.executable,
        str(WAIT_CHAIN_COLLECTOR),
        "--pid", str(int(pid)),
        "--output", str(output_path),
        "--max-threads", str(WAIT_CHAIN_HELPER_MAX_THREADS),
    ]
    helper = _run_bounded_hidden_python_helper(
        helper_command, WAIT_CHAIN_HELPER_TIMEOUT_SEC,
    )
    report["helper"] = helper

    orphan_cleanup: List[Dict[str, Any]] = []
    try:
        orphan_candidates = list(output_path.parent.glob(
            f".{output_path.name}.*.tmp"
        ))
    except OSError as exc:
        orphan_candidates = []
        orphan_cleanup.append({
            "ok": False,
            "path": str(output_path.parent),
            "error": f"{type(exc).__name__}: {exc}",
        })
    for candidate in orphan_candidates:
        try:
            candidate.unlink(missing_ok=True)
            orphan_cleanup.append({"ok": True, "path": str(candidate)})
        except OSError as exc:
            orphan_cleanup.append({
                "ok": False,
                "path": str(candidate),
                "error": f"{type(exc).__name__}: {exc}",
            })
    report["orphanTemporaryOutputCleanup"] = orphan_cleanup

    payload: Dict[str, Any] = {}
    payload_error = ""
    if output_path.is_file():
        try:
            loaded = json.loads(output_path.read_text(
                encoding="utf-8", errors="strict",
            ))
            if isinstance(loaded, dict):
                payload = loaded
            else:
                payload_error = "等待链 JSON 根节点不是对象"
        except Exception as exc:
            payload_error = f"{type(exc).__name__}: {exc}"
    else:
        payload_error = "helper 未发布原子 JSON 输出"
    report["payload"] = payload
    report["payloadError"] = payload_error

    post_authority = _capture_fresh_process_authority(
        pid, fingerprint, retained_witness=witness,
    )
    witness_after = _retained_launch_handle_witness_report(witness)
    report["postProcessAuthorityReportOnly"] = post_authority
    report["retainedWitnessAfter"] = witness_after
    report["postRetainedHandleBindingExact"] = (
        _wait_chain_retained_handle_unsignaled_exact(
            post_authority, fingerprint, witness, pid,
        )
    )
    report["retainedHandleObjectStable"] = bool(
        native_process is not None
        and witness.get("_nativeProcessWitness") is native_process
    )
    report["retainedWitnessStable"] = witness_before == witness_after
    report["sameRetainedHandleExact"] = bool(
        report["preRetainedHandleBindingExact"]
        and report["postRetainedHandleBindingExact"]
        and report["retainedHandleObjectStable"]
        and report["retainedWitnessStable"]
    )
    report["payloadPidExact"] = bool(payload.get("pid") == int(pid))
    report["payloadDiagnosticOnly"] = bool(
        payload.get("diagnosticOnly") is True
        and payload.get("authority") is False
        and payload.get("classificationAuthority") == 0
    )
    payload_summary = dict(payload.get("summary", {}) or {})
    report["payloadAttributionExactCount"] = int(
        payload_summary.get("attributionExact", 0) or 0
    )
    report["payloadHasExactWaitChain"] = bool(
        report["payloadAttributionExactCount"] >= 1
    )
    report["ok"] = bool(
        helper.get("ok") is True
        and helper.get("helperResidual") is False
        and not payload_error
        and payload.get("ok") is True
        and report["payloadPidExact"]
        and report["payloadDiagnosticOnly"]
        and report["payloadHasExactWaitChain"]
        and report["sameRetainedHandleExact"]
    )
    return report


def _wait_chain_helper_synthetic_self_tests() -> Dict[str, Any]:
    """只启动短命 Python 子进程，验证 helper 失败与超时回收。"""
    failure = _run_bounded_hidden_python_helper(
        [sys.executable, "-c", "raise SystemExit(7)"], 1.0,
    )
    timeout = _run_bounded_hidden_python_helper(
        [sys.executable, "-c", "import time; time.sleep(60)"], 0.5,
    )

    class _SyntheticStdout:
        def __init__(self) -> None:
            self.closed = False

        def read(self) -> str:
            return ""

        def close(self) -> None:
            self.closed = True

    class _SyntheticStubbornProcess:
        """纯合成不响应 terminate 的子进程，覆盖 kill/wait 分支。"""

        def __init__(self) -> None:
            self.pid = 424242
            self.returncode: Optional[int] = None
            self.stdout = _SyntheticStdout()
            self.terminate_called = False
            self.kill_called = False

        def communicate(self, timeout: float) -> Tuple[str, None]:
            raise subprocess.TimeoutExpired("synthetic-helper", timeout)

        def terminate(self) -> None:
            self.terminate_called = True

        def wait(self, timeout: float) -> int:
            if not self.kill_called:
                raise subprocess.TimeoutExpired("synthetic-helper", timeout)
            self.returncode = -9
            return self.returncode

        def kill(self) -> None:
            self.kill_called = True

        def poll(self) -> Optional[int]:
            return self.returncode

    original_popen = subprocess.Popen
    stubborn_process = _SyntheticStubbornProcess()
    try:
        subprocess.Popen = lambda *args, **kwargs: stubborn_process
        stubborn = _run_bounded_hidden_python_helper(
            [sys.executable, "-c", "pass"], 0.25,
        )
    finally:
        subprocess.Popen = original_popen

    synthetic_pid = 1234
    synthetic_creation = 987654321
    synthetic_path = _canonical_windows_path(WAR3_DIR / "war3.exe")
    clean_fingerprint = {
        "available": True,
        "launcherMode": "direct",
        "pid": synthetic_pid,
        "creationEpochMs": synthetic_creation,
        "canonicalExePath": synthetic_path,
    }
    clean_witness = {
        "shapeExact": True,
        "targetPid": synthetic_pid,
        "fingerprintCreationEpochMs": synthetic_creation,
        "retainedProcessPid": synthetic_pid,
        "retainedProcessCreationEpochMs": synthetic_creation,
        "retainedProcessCanonicalExePath": synthetic_path,
        "_nativeProcessWitness": object(),
    }
    clean_unsignaled_authority = {
        # 模拟 current PID query 暂时不可用，retained HANDLE 仍独立闭合。
        "aliveExact": False,
        "immutableLaunchFingerprintValidation": {
            "exact": False,
            "fingerprintAvailable": True,
            "fingerprintShapeExact": True,
            "numericPidExact": True,
        },
        "retainedTerminationProof": {
            "shapeExact": True,
            "bindingExact": True,
            "targetPid": synthetic_pid,
            "fingerprintCreationEpochMs": synthetic_creation,
            "retainedProcessPid": synthetic_pid,
            "retainedProcessCreationEpochMs": synthetic_creation,
            "retainedProcessCanonicalExePath": synthetic_path,
            "handleSignaled": False,
            "exact": False,
            "pollError": "",
            "nativeTerminationProof": {
                "bindingExact": True,
                "handleSignaled": False,
                "exact": False,
                "exitCode": None,
                "pollError": "",
            },
        },
    }
    nested_poll_error_authority = json.loads(json.dumps(
        clean_unsignaled_authority,
    ))
    nested_poll_error_authority[
        "retainedTerminationProof"
    ]["nativeTerminationProof"]["pollError"] = "合成 poll 失败"
    pid_mismatch_fingerprint = dict(clean_fingerprint)
    pid_mismatch_fingerprint["pid"] = synthetic_pid + 1
    creation_mismatch_fingerprint = dict(clean_fingerprint)
    creation_mismatch_fingerprint["creationEpochMs"] = synthetic_creation + 1
    path_mismatch_fingerprint = dict(clean_fingerprint)
    path_mismatch_fingerprint["canonicalExePath"] = str(
        Path(synthetic_path).with_name("foreign.exe")
    )

    checks = {
        "failureDidNotPass": failure.get("ok") is False,
        "failureReturnCode": failure.get("returncode") == 7,
        "failureNoResidual": failure.get("helperResidual") is False,
        "timeoutDetected": timeout.get("timedOut") is True,
        "timeoutTerminateIssued": timeout.get("terminateIssued") is True,
        "timeoutWaited": bool(
            timeout.get("terminateWaitCompleted") is True
            or timeout.get("killWaitCompleted") is True
        ),
        "timeoutNoResidual": timeout.get("helperResidual") is False,
        "stubbornTerminateIssued": bool(
            stubborn.get("terminateIssued") is True
            and stubborn_process.terminate_called
        ),
        "stubbornTerminateWaited": bool(
            stubborn.get("effectiveTerminateWaitSec", 0) > 0
            and stubborn.get("terminateWaitCompleted") is False
        ),
        "stubbornKillIssued": bool(
            stubborn.get("killIssued") is True
            and stubborn_process.kill_called
        ),
        "stubbornKillWaited": stubborn.get("killWaitCompleted") is True,
        "stubbornNoResidual": stubborn.get("helperResidual") is False,
        "currentQueryUnavailableRetainedExactAccepted": (
            _wait_chain_retained_handle_unsignaled_exact(
                clean_unsignaled_authority,
                clean_fingerprint,
                clean_witness,
                synthetic_pid,
            ) is True
        ),
        "nestedPollErrorFailClosed": (
            _wait_chain_retained_handle_unsignaled_exact(
                nested_poll_error_authority,
                clean_fingerprint,
                clean_witness,
                synthetic_pid,
            ) is False
        ),
        "frozenPidMismatchFailClosed": (
            _wait_chain_retained_handle_unsignaled_exact(
                clean_unsignaled_authority,
                pid_mismatch_fingerprint,
                clean_witness,
                synthetic_pid,
            ) is False
        ),
        "frozenCreationMismatchFailClosed": (
            _wait_chain_retained_handle_unsignaled_exact(
                clean_unsignaled_authority,
                creation_mismatch_fingerprint,
                clean_witness,
                synthetic_pid,
            ) is False
        ),
        "frozenPathMismatchFailClosed": (
            _wait_chain_retained_handle_unsignaled_exact(
                clean_unsignaled_authority,
                path_mismatch_fingerprint,
                clean_witness,
                synthetic_pid,
            ) is False
        ),
        "bothShellFalse": bool(
            failure.get("shell") is False
            and timeout.get("shell") is False
        ),
    }
    if not all(checks.values()):
        raise AssertionError(
            "等待链 helper 纯合成自检失败: "
            + json.dumps(checks, ensure_ascii=False, sort_keys=True)
        )
    return {
        "ok": True,
        "checks": checks,
        "failure": failure,
        "timeout": timeout,
        "stubborn": stubborn,
    }


def _capture_ready_failure_minidump(
    output_path: Path,
    pid: int,
    retained_witness: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    """用启动时保留的 HANDLE 抓紧凑转储；失败只影响取证，不影响分类。"""
    report: Dict[str, Any] = {
        "ok": False,
        "path": str(output_path),
        "pid": int(pid),
        "authority": "launch-frozen duplicated native process HANDLE",
        "classificationAuthority": 0,
    }
    witness = dict(retained_witness or {})
    native_process = witness.get("_nativeProcessWitness")
    if witness.get("shapeExact") is not True or native_process is None:
        report["error"] = "精确原生进程句柄不可用"
        return report

    duplicate = None
    try:
        duplicate = native_process.duplicate()
        snapshot = dict(duplicate.snapshot() or {})
        handle = int(getattr(duplicate, "_handle", 0) or 0)
        binding_exact = bool(
            snapshot.get("available") is True
            and snapshot.get("pid") == int(pid)
            and handle > 0
        )
        report["bindingExact"] = binding_exact
        report["witness"] = snapshot
        if not binding_exact:
            report["error"] = "转储句柄与目标 PID 绑定不精确"
            return report

        output_path.parent.mkdir(parents=True, exist_ok=True)
        dbghelp = ctypes.WinDLL("Dbghelp.dll", use_last_error=True)
        write_dump = dbghelp.MiniDumpWriteDump
        write_dump.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.HANDLE,
            wintypes.DWORD,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        write_dump.restype = wintypes.BOOL
        # Normal dump 已含线程栈；再附带线程信息、进程线程数据和已卸载模块。
        dump_type = 0x00001000 | 0x00000100 | 0x00000020
        with output_path.open("w+b") as stream:
            file_handle = msvcrt.get_osfhandle(stream.fileno())
            ctypes.set_last_error(0)
            ok = bool(write_dump(
                wintypes.HANDLE(handle),
                wintypes.DWORD(int(pid)),
                wintypes.HANDLE(file_handle),
                wintypes.DWORD(dump_type),
                None,
                None,
                None,
            ))
            win32_error = int(ctypes.get_last_error())
            stream.flush()
        size = output_path.stat().st_size if output_path.exists() else 0
        report.update({
            "ok": ok and size > 0,
            "dumpType": dump_type,
            "size": size,
            "win32Error": win32_error,
        })
        if not report["ok"]:
            report["error"] = "MiniDumpWriteDump 失败或生成空文件"
            if output_path.exists():
                output_path.unlink()
        return report
    except Exception as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        try:
            if output_path.exists():
                output_path.unlink()
        except OSError:
            pass
        return report
    finally:
        if duplicate is not None:
            try:
                report["witnessClose"] = duplicate.close()
            except Exception as exc:
                report["witnessClose"] = {
                    "ok": False,
                    "error": f"{type(exc).__name__}: {exc}",
                }


def _collect_ready_failure_evidence(
    out_dir: Path,
    tag: str,
    pid: int,
    desktop_name: str,
    ready: Dict[str, Any],
    log_offsets: Dict[str, Dict[str, Any]],
    launch_epoch_ms: int,
    event_since_id: int,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    wait_chains = _best_effort(
        "ready_failure_wait_chains",
        lambda: _capture_ready_failure_wait_chains(
            out_dir / f"{tag}_wait_chains_raw.json",
            pid,
            launch_fingerprint,
            retained_witness,
        ),
    )
    # 即使采集器本身抛异常，外层记录也必须保持零分类权限。
    wait_chains["diagnosticOnly"] = True
    wait_chains["authority"] = False
    wait_chains["classificationAuthority"] = 0
    minidump = _capture_ready_failure_minidump(
        out_dir / f"{tag}_pre_stop.dmp", pid, retained_witness,
    )
    process_state = _best_effort("is_war3_running", lambda: is_war3_running(pid=pid))
    process_snapshot = _best_effort(
        "process_snapshot",
        lambda: _capture_process_snapshot(
            pid, launch_epoch_ms, launch_fingerprint,
        ),
    )
    window_snapshot = _best_effort(
        "window_snapshot",
        lambda: query_war3_window(pid=pid, wait_sec=1, require_visible=False),
    )
    control_plane = _best_effort(
        "control_plane_status",
        lambda: read_runtime_status(war3_dir=str(WAR3_DIR), pid=pid),
    )
    screenshot = _best_effort(
        "isolated_screenshot",
        lambda: capture_war3_screenshot(
            output_path=str(out_dir / f"{tag}_isolated.png"),
            pid=pid,
            war3_dir=str(WAR3_DIR),
            prefer_internal=False,
            timeout_sec=6,
            fallback_to_window_capture=True,
        ),
    )
    debug = _best_effort(
        "sync_all_debug",
        lambda: sync_all_debug(
            war3_dir=str(WAR3_DIR), event_limit=20000, tail_lines=20000,
            include_dbwin_events=True, include_perf_reports=False, include_log_files=True,
        ),
    )
    event_result = _best_effort(
        "runtime_events",
        lambda: _get_runtime_events_paginated(
            since_id=event_since_id, total_limit=20000
        ),
    )
    event_rows = list(event_result.get("events", []) or []) if event_result.get("ok") else []
    target_event_rows = [row for row in event_rows if _coerce_pid(row.get("pid")) == pid]
    infrastructure_event_rows = [row for row in event_rows if _coerce_pid(row.get("pid")) == 0]
    foreign_event_rows = [
        row for row in event_rows
        if _coerce_pid(row.get("pid")) not in (None, 0, pid)
    ]
    event_lines = [str(row.get("msg", "")) for row in target_event_rows]
    log_copies = _best_effort(
        "incremental_logs",
        lambda: {"ok": True, "files": _copy_new_log_bytes(log_offsets, out_dir, tag)},
    )
    windows_events = _best_effort(
        "windows_error_events",
        lambda: _capture_windows_error_events(
            pid, launch_epoch_ms,
            _ready_process_instance_contract(
                process_snapshot, pid, launch_epoch_ms,
                launch_fingerprint,
            ).get("processStartEpochMs"),
        ),
    )

    unattributed_parts: List[str] = []
    copied_files = dict(log_copies.get("files", {}) or {})
    for entry in copied_files.values():
        try:
            unattributed_parts.append(Path(str(entry["output"])).read_text(encoding="utf-8", errors="replace"))
        except Exception as exc:
            unattributed_parts.append(f"ready evidence log read failed: {type(exc).__name__}: {exc}")
    target_text = "\n".join(event_lines)
    unattributed_text = "\n".join(unattributed_parts)
    runtime_data = dict(control_plane.get("data", {}) or {}) if control_plane.get("ok") else {}
    diag = _parse_gpu_skin_diag(target_text, runtime_data)
    unattributed_diag = _parse_gpu_skin_diag(unattributed_text, {})
    classification_fingerprint_validation = (
        _validate_launch_instance_fingerprint(
            dict(launch_fingerprint or {}), pid,
        )
    )
    retained_termination_proof = _retained_launch_handle_termination_proof(
        dict(launch_fingerprint or {}), pid, retained_witness,
    )
    classification = _ready_failure_classification(
        ready,
        process_state,
        target_text,
        windows_events,
        target_pid=pid,
        launch_epoch_ms=launch_epoch_ms,
        process_snapshot=process_snapshot,
        launch_fingerprint=launch_fingerprint,
        current_fingerprint_validation=(
            classification_fingerprint_validation
        ),
        retained_termination_proof=retained_termination_proof,
        unattributed_text=unattributed_text,
    )
    crash_matches = list(classification.get("crashLines", []) or [])
    unattributed_crash_matches = list(classification.get("unattributedCrashLines", []) or [])

    evidence = {
        "tag": tag,
        "pid": pid,
        "immutableLaunchFingerprint": json.loads(json.dumps(
            dict(launch_fingerprint or {}), ensure_ascii=False,
        )),
        "classificationLaunchFingerprintValidation": (
            classification_fingerprint_validation
        ),
        "retainedLaunchHandleTerminationProof": (
            retained_termination_proof
        ),
        "preStopWaitChains": wait_chains,
        "preStopMinidump": minidump,
        "desktop": desktop_name,
        "ready": ready,
        "processState": process_state,
        "processSnapshot": process_snapshot,
        "windowSnapshot": window_snapshot,
        "isolatedScreenshot": screenshot,
        "controlPlaneStatus": control_plane,
        "debug": debug,
        "runtimeEvents": event_result,
        "targetRuntimeEvents": target_event_rows,
        "infrastructureRuntimeEvents": infrastructure_event_rows,
        "foreignRuntimeEvents": foreign_event_rows,
        "incrementalLogs": log_copies,
        "windowsErrorEvents": windows_events,
        "crashMatches": crash_matches,
        "unattributedCrashMatches": unattributed_crash_matches,
        "diagnostics": diag,
        "unattributedDiagnosticsReportOnly": unattributed_diag,
        "classification": classification,
    }
    for name, value in (
        (f"{tag}_process_state.json", process_state),
        (f"{tag}_wait_chains_capture.json", wait_chains),
        (f"{tag}_pre_stop_minidump.json", minidump),
        (f"{tag}_process_snapshot.json", process_snapshot),
        (f"{tag}_window_snapshot.json", window_snapshot),
        (f"{tag}_isolated_screenshot_result.json", screenshot),
        (f"{tag}_control_plane_status.json", control_plane),
        (f"{tag}_debug_all.json", debug),
        (f"{tag}_runtime_events.json", event_result),
        (f"{tag}_windows_error_events.json", windows_events),
        (f"{tag}_gpu_skin_diag.json", diag),
        (f"{tag}_classification.json", classification),
    ):
        _json_write(out_dir / name, value)
    _text_write(out_dir / f"{tag}_debugview.log", "\n".join(event_lines) + "\n")
    _text_write(out_dir / f"{tag}_crash_exception_scan.log", "\n".join(crash_matches) + "\n")
    _text_write(
        out_dir / f"{tag}_unattributed_crash_exception_scan.log",
        "\n".join(unattributed_crash_matches) + "\n",
    )
    _json_write(out_dir / f"{tag}_evidence.json", evidence)
    return evidence


def _stop_after_ready_failure(
    pid: int, launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
    cleanup_capability: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    del launch_fingerprint  # Evidence identity is not cleanup authority.
    return _exact_handle_cleanup_stop(
        pid,
        cleanup_capability,
        retained_witness,
        mode="ready-failure-exact-native-handle",
    )


def _poll_runtime_steps(
    pid: int, count: int, wait_sec: float,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> List[Dict[str, Any]]:
    polls: List[Dict[str, Any]] = []
    start = time.monotonic()
    for _ in range(max(1, count)):
        poll = _runtime_poll(
            pid, time.monotonic() - start, launch_fingerprint,
            retained_witness,
        )
        polls.append(poll)
        if not poll["running"]:
            break
        time.sleep(max(0.1, wait_sec))
    return polls


def _poll_runtime_until_gpu_skin_gate(
    pid: int,
    log_offsets: Dict[str, Dict[str, Any]],
    event_since_id: int,
    timeout_sec: float = 30.0,
    wait_sec: float = 2.0,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> List[Dict[str, Any]]:
    """Bound the relaunch smoke by evidence, not by an assumed frame rate."""
    polls: List[Dict[str, Any]] = []
    start = time.monotonic()
    event_cursor = max(0, event_since_id)
    event_lines: List[str] = []
    probe_names = tuple(
        name for name in BASE_HARD_GATE_NAMES
        if name not in (
            "forcedDiagnosticsSnapshot", "forcedDiagnosticsQuiescent",
            "managerDispatchPolicyClean", "telemetryBatchingExact",
            "dispatchCpuOnlySealContractClean",
            "productionSampleTimingExact",
            "protocolAccountingClosed",
            "outsideNativeFastPathPolicyClean",
            "nativePoisonSidecarPolicyContractClean",
            "nativePoisonShadowContractClean",
            "nativePoisonO1ShadowContractClean",
            "nativePoisonO1AuthorityContractClean",
            "outsideAdmissionAttributionClean",
            "nativeBeginSamplerCadenceClean",
            "hotPathTimingContractClean",
            "resourceAccountingClosed",
            "twoScreenshots", "crashScanClean",
            "postStopTerminationExact",
        )
    )
    while True:
        elapsed = time.monotonic() - start
        poll = _runtime_poll(
            pid, elapsed, launch_fingerprint, retained_witness,
        )
        log_text = _read_new_log_text(log_offsets)
        event_result = _best_effort(
            "gpu_skin_gate_events",
            lambda: _get_runtime_events_paginated(
                since_id=event_cursor, total_limit=20000
            ),
        )
        for row in list(event_result.get("events", []) or []):
            event_id = _coerce_pid(row.get("id"))
            if event_id is not None:
                event_cursor = max(event_cursor, event_id)
            if _coerce_pid(row.get("pid")) == pid:
                event_lines.append(str(row.get("msg", "")))
        if len(event_lines) > 20000:
            event_lines = event_lines[-20000:]
        event_text = "\n".join(event_lines)
        # 只有 launch cursor 之后的 exact-PID DBWIN 能授权 PASS。共享日志
        # 没有 PID，可能来自前台 World Editor/其他 War3，只做 report-only。
        probe_diag = _parse_gpu_skin_diag(event_text, poll.get("data", {}))
        shared_log_diag = _parse_gpu_skin_diag(log_text, {})
        probe_gates = _evaluate_gates(
            probe_diag, dict(poll.get("processAuthority", {}) or {}), [], []
        )
        failed = [name for name in probe_names if not probe_gates.get(name, False)]
        poll["gpuSkinGateProbe"] = {
            "ready": not failed,
            "failed": failed,
            "diagnosticsPresent": probe_gates.get("diagnosticsPresent", False),
            "sharedLogDiagnosticsPresentReportOnly": bool(
                shared_log_diag.get("rawLatest")
            ),
        }
        polls.append(poll)
        if not poll.get("running") or not failed:
            break
        remaining = timeout_sec - (time.monotonic() - start)
        if remaining <= 0.0:
            break
        time.sleep(min(max(0.1, wait_sec), remaining))
    return polls


def _event_lines_since(since_id: int, pid: int) -> Tuple[List[str], int]:
    events = _get_runtime_events_paginated(since_id=since_id, total_limit=20000)
    rows = list(events.get("events", []) or [])
    newest_id = since_id
    for row in rows:
        try:
            newest_id = max(newest_id, int(row.get("id", 0)))
        except (TypeError, ValueError):
            pass
    if pid <= 0:
        return [], newest_id
    return [
        str(row.get("msg", ""))
        for row in rows
        if _coerce_pid(row.get("pid")) == pid
    ], newest_id


def _exact_control_plane_pipe_name(pid: int) -> str:
    return rf"\\.\pipe\War3ControlPlane_{int(pid)}"


def _wait_for_game_ready_with_retained_handle(
    *,
    timeout_sec: int,
    pid: int,
    launch_fingerprint: Dict[str, Any],
    retained_witness: Dict[str, Any],
) -> Dict[str, Any]:
    """只用启动时冻结的原生进程句柄判活，并等待控制管道建立。"""
    started = time.monotonic()
    deadline = started + max(1.0, float(timeout_sec))
    attempts = 0
    first_transport_error = ""
    last_transport: Dict[str, Any] = {}
    last_handle_proof: Dict[str, Any] = {}

    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        request_timeout = max(1, int(remaining + 0.999))
        raw = _control_plane_request(
            pid=int(pid),
            command="wait_until",
            payload={
                "timeoutSec": request_timeout,
                "pollIntervalMs": 50,
            },
            timeout_sec=max(2.0, remaining + 2.0),
        )
        attempts += 1
        last_transport = dict(raw or {})
        if raw.get("transportOk"):
            runtime_status = dict(
                ((raw.get("result", {}) or {}).get(
                    "runtimeStatus", {}
                )) or {}
            )
            return {
                "ok": raw.get("ok") is True,
                "error": str(
                    raw.get("error", "")
                    or ("" if raw.get("ok") is True
                        else "control plane wait_until 失败")
                ),
                "mode": "control-plane",
                "pid": int(pid),
                "elapsedSec": round(time.monotonic() - started, 3),
                "runtimeStatus": runtime_status,
                "transportAttempts": attempts,
                "firstTransportError": first_transport_error,
                "detail": raw,
            }

        current_error = str(raw.get("error", "") or "")
        if not first_transport_error:
            first_transport_error = current_error
        last_handle_proof = _retained_launch_handle_termination_proof(
            launch_fingerprint,
            int(pid),
            retained_witness,
        )
        if last_handle_proof.get("exact") is True:
            return {
                "ok": False,
                "error": "进程在控制管道建立前已由精确原生句柄证明退出",
                "mode": "control-plane-required",
                "pid": int(pid),
                "elapsedSec": round(time.monotonic() - started, 3),
                "transportAttempts": attempts,
                "firstTransportError": first_transport_error,
                "detail": raw,
                "retainedHandleTerminationProof": last_handle_proof,
            }

        # WaitForSingleObject 返回未 signaled 且绑定精确时，才允许继续等待。
        # 普通 PID 查询在隔离桌面启动初期可能短暂误报，不能承担判死权限。
        handle_proves_alive = bool(
            last_handle_proof.get("bindingExact") is True
            and last_handle_proof.get("handleSignaled") is False
            and not str(last_handle_proof.get("pollError", "") or "")
        )
        if not handle_proves_alive:
            return {
                "ok": False,
                "error": "控制管道不可用，且原生进程句柄判活权限不完整",
                "mode": "control-plane-required",
                "pid": int(pid),
                "elapsedSec": round(time.monotonic() - started, 3),
                "transportAttempts": attempts,
                "firstTransportError": first_transport_error,
                "detail": raw,
                "retainedHandleTerminationProof": last_handle_proof,
            }
        time.sleep(min(0.2, max(0.01, remaining)))

    return {
        "ok": False,
        "error": f"控制管道在 {int(timeout_sec)} 秒内未建立",
        "mode": "control-plane-required",
        "pid": int(pid),
        "elapsedSec": round(time.monotonic() - started, 3),
        "transportAttempts": attempts,
        "firstTransportError": first_transport_error,
        "detail": last_transport,
        "retainedHandleTerminationProof": last_handle_proof,
    }


def _invoke_internal_test_api_exact(
    command: str,
    payload: Dict[str, Any],
    pid: int,
    timeout_sec: int,
) -> Dict[str, Any]:
    target_pid = int(pid)
    raw = _control_plane_request(
        pid=target_pid,
        command="invoke_test_command",
        payload={
            "command": str(command or "").strip(),
            "payload": dict(payload),
            "timeoutMs": max(1000, int(timeout_sec) * 1000),
        },
        timeout_sec=max(2.0, float(timeout_sec) + 1.0),
    )
    request = dict(raw.get("request", {}) or {})
    response = dict(raw.get("response", {}) or {})
    result = dict(raw.get("result", {}) or {})
    issued_request_id = str(request.get("requestId", "") or "")
    response_request_id = str(response.get("requestId", "") or "")
    return {
        "ok": bool(raw.get("transportOk") and raw.get("ok")),
        "transportOk": bool(raw.get("transportOk")),
        "mode": (
            "control-plane" if raw.get("transportOk")
            else "control-plane-unavailable"
        ),
        "command": str(command or "").strip(),
        "targetPid": target_pid,
        "callerPid": os.getpid(),
        "pipeName": str(raw.get("pipeName", "") or ""),
        "request": request,
        "response": response,
        "result": result,
        "callerIssuedRequestId": issued_request_id,
        "issuedRequestId": issued_request_id,
        "responseRequestId": response_request_id,
        "error": str(raw.get("error", "") or ""),
        "elapsedSec": raw.get("elapsedSec"),
    }


def _forced_diag_transport_contract(
    invocation: Dict[str, Any], pid: int, snapshot_id: str,
    fingerprint_validation: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    request = dict(invocation.get("request", {}) or {})
    response = dict(invocation.get("response", {}) or {})
    request_payload = dict(request.get("payload", {}) or {})
    issued_request_id = str(request.get("requestId", "") or "")
    caller_issued_request_id = str(
        invocation.get("callerIssuedRequestId", "") or ""
    )
    response_request_id = str(response.get("requestId", "") or "")
    reported_snapshot_id = str(snapshot_id or "")
    expected_pipe = _exact_control_plane_pipe_name(pid)
    numeric_target_pid_exact = bool(
        isinstance(invocation.get("targetPid"), int)
        and not isinstance(invocation.get("targetPid"), bool)
        and invocation.get("targetPid") == pid
    )
    numeric_request_pid_exact = bool(
        isinstance(request.get("pid"), int)
        and not isinstance(request.get("pid"), bool)
        and request.get("pid") == pid
    )
    id_chain_exact = bool(
        issued_request_id and reported_snapshot_id
        and caller_issued_request_id == issued_request_id
        and issued_request_id == response_request_id == reported_snapshot_id
    )
    fingerprint_value = dict(fingerprint_validation or {})
    fingerprint_available = (
        fingerprint_value.get("fingerprintAvailable") is True
    )
    fingerprint_pre_exact = bool(
        dict(fingerprint_value.get("pre", {}) or {}).get("exact") is True
    )
    fingerprint_post_exact = bool(
        dict(fingerprint_value.get("post", {}) or {}).get("exact") is True
    )
    fingerprint_exact = bool(
        fingerprint_available and fingerprint_pre_exact
        and fingerprint_post_exact
        and fingerprint_value.get("exact") is True
    )
    exact = bool(
        invocation.get("transportOk") is True
        and invocation.get("mode") == "control-plane"
        and str(invocation.get("pipeName", "") or "") == expected_pipe
        and numeric_target_pid_exact and numeric_request_pid_exact
        and request.get("command") == "invoke_test_command"
        and request_payload.get("command") == "gpu_skin.log_diagnostics"
        and invocation.get("command") == "gpu_skin.log_diagnostics"
        and id_chain_exact
        and fingerprint_exact
    )
    return {
        "exact": exact,
        "transportOk": invocation.get("transportOk") is True,
        "mode": invocation.get("mode"),
        "modeExact": invocation.get("mode") == "control-plane",
        "targetPid": pid,
        "invocationTargetPid": invocation.get("targetPid"),
        "numericTargetPidExact": numeric_target_pid_exact,
        "requestPid": request.get("pid"),
        "numericRequestPidExact": numeric_request_pid_exact,
        "expectedPipeName": expected_pipe,
        "pipeName": invocation.get("pipeName"),
        "pipeNameExact": invocation.get("pipeName") == expected_pipe,
        "callerIssuedRequestId": caller_issued_request_id,
        "issuedRequestId": issued_request_id,
        "responseRequestId": response_request_id,
        "snapshotRequestId": reported_snapshot_id,
        "requestIdChainExact": id_chain_exact,
        "immutableLaunchFingerprintAvailable": fingerprint_available,
        "immutableLaunchFingerprintPreExact": fingerprint_pre_exact,
        "immutableLaunchFingerprintPostExact": fingerprint_post_exact,
        "immutableLaunchFingerprintExact": fingerprint_exact,
        "immutableLaunchFingerprintValidation": fingerprint_value,
        "outerCommandExact": request.get("command") ==
            "invoke_test_command",
        "innerCommandExact": request_payload.get("command") ==
            "gpu_skin.log_diagnostics",
        "reportOnly": not exact,
        "failureClassificationAuthority": 1 if exact else 0,
    }


def _forced_diag_block_attribution(
    event_block: Dict[str, Any], log_block: Dict[str, Any],
) -> Dict[str, Any]:
    event_complete = _forced_diag_block_complete(event_block)
    log_complete = _forced_diag_block_complete(log_block)
    return {
        "authoritativeBlock": event_block,
        "authoritativeComplete": event_complete,
        "source": (
            "exact-pid-dbwin" if event_complete
            else "shared-log-report-only" if log_complete
            else "none"
        ),
        "sharedLogCompleteReportOnly": log_complete,
        "sharedLogAuthorizationAuthority": 0,
    }


def _forced_diag_transport_synthetic_self_tests() -> Dict[str, Any]:
    pid = 39976
    request_id = "cp_exact_39976_1"
    exact_fingerprint_validation = {
        "fingerprintAvailable": True,
        "pre": {"exact": True},
        "post": {"exact": True},
        "exact": True,
    }

    def invocation(**overrides: Any) -> Dict[str, Any]:
        value: Dict[str, Any] = {
            "ok": True,
            "transportOk": True,
            "mode": "control-plane",
            "command": "gpu_skin.log_diagnostics",
            "targetPid": pid,
            "pipeName": _exact_control_plane_pipe_name(pid),
            "callerIssuedRequestId": request_id,
            "request": {
                "requestId": request_id,
                "pid": pid,
                "command": "invoke_test_command",
                "payload": {"command": "gpu_skin.log_diagnostics"},
            },
            "response": {"requestId": request_id},
            "result": {"snapshotId": request_id},
        }
        value.update(overrides)
        return value

    checks: Dict[str, bool] = {}
    checks["exactTransportAccepted"] = (
        _forced_diag_transport_contract(
            invocation(), pid, request_id, exact_fingerprint_validation,
        ).get("exact") is True
    )
    checks["wrongPipeRejected"] = (
        _forced_diag_transport_contract(
            invocation(pipeName=_exact_control_plane_pipe_name(pid + 1)),
            pid, request_id, exact_fingerprint_validation,
        ).get("exact") is False
    )
    wrong_issued = invocation(callerIssuedRequestId="cp_wrong")
    checks["wrongIssuedIdRejected"] = (
        _forced_diag_transport_contract(
            wrong_issued, pid, request_id,
            exact_fingerprint_validation,
        ).get("exact") is False
    )
    wrong_response = invocation()
    wrong_response["response"] = {"requestId": "cp_wrong_response"}
    checks["wrongResponseEchoRejected"] = (
        _forced_diag_transport_contract(
            wrong_response, pid, request_id,
            exact_fingerprint_validation,
        ).get("exact") is False
    )
    checks["wrongInvocationPidRejected"] = (
        _forced_diag_transport_contract(
            invocation(targetPid=pid + 1), pid, request_id,
            exact_fingerprint_validation,
        ).get("exact") is False
    )
    wrong_pid = invocation()
    wrong_pid["request"] = dict(wrong_pid["request"])
    wrong_pid["request"]["pid"] = pid + 1
    checks["wrongRequestPidRejected"] = (
        _forced_diag_transport_contract(
            wrong_pid, pid, request_id,
            exact_fingerprint_validation,
        ).get("exact") is False
    )
    checks["fingerprintUnavailableRejected"] = (
        _forced_diag_transport_contract(
            invocation(), pid, request_id, {
                "fingerprintAvailable": False,
                "pre": {"exact": False},
                "post": {"exact": False},
                "exact": False,
            },
        ).get("exact") is False
    )
    checks["fingerprintReplacementRejected"] = (
        _forced_diag_transport_contract(
            invocation(), pid, request_id, {
                "fingerprintAvailable": True,
                "pre": {"exact": True},
                "post": {"exact": False},
                "exact": False,
            },
        ).get("exact") is False
    )
    empty_event = _extract_forced_diag_block([], request_id, 1)
    log_block = {
        "beginSeen": True, "endSeen": True, "orderValid": True,
        "contract": {"ok": True}, "text": "report-only", "sha256": "x",
    }
    log_only = _forced_diag_block_attribution(empty_event, log_block)
    checks["sharedLogFallbackNeverAuthorizes"] = bool(
        log_only.get("authoritativeComplete") is False
        and log_only.get("source") == "shared-log-report-only"
        and log_only.get("sharedLogAuthorizationAuthority") == 0
    )
    light_diags = (
        {"diagnosticPolicy": {"fullExact": False, "lightExact": True}},
        {"diagnosticPolicy": {"fullExact": False, "lightExact": True}},
    )
    first_clean_pair_without_dispatch_sample = {
        "valid": True,
        "fullExact": False,
        "lightExact": False,
        "evidencePositive": False,
        "dispatchSeal": {"calls": 0},
    }
    later_clean_pair_with_dispatch_sample = {
        "valid": True,
        "fullExact": False,
        "lightExact": True,
        "evidencePositive": True,
        "dispatchSeal": {"calls": 1},
    }
    first_pair_policy = _production_sample_timing_policy_contract(
        light_diags[0], light_diags[1],
        first_clean_pair_without_dispatch_sample,
    )
    later_pair_policy = _production_sample_timing_policy_contract(
        light_diags[0], light_diags[1],
        later_clean_pair_with_dispatch_sample,
    )
    checks["firstCleanPairWithoutDispatchSampleRejected"] = bool(
        first_pair_policy.get("exact") is False
    )
    checks["laterCleanPairWithDispatchSampleSelected"] = bool(
        later_pair_policy.get("exact") is True
    )
    full_diags = (
        {"diagnosticPolicy": {"fullExact": True, "lightExact": False}},
        {"diagnosticPolicy": {"fullExact": True, "lightExact": False}},
    )
    full_zero_pair_policy = _production_sample_timing_policy_contract(
        full_diags[0], full_diags[1], {
            "valid": True,
            "fullExact": True,
            "lightExact": False,
            "allZero": True,
        },
    )
    checks["fullAllZeroPairStillSelected"] = bool(
        full_zero_pair_policy.get("exact") is True
    )
    unrecognized_pair_policy = _production_sample_timing_policy_contract(
        {"diagnosticPolicy": {"fullExact": False, "lightExact": False}},
        {"diagnosticPolicy": {"fullExact": False, "lightExact": False}},
        later_clean_pair_with_dispatch_sample,
    )
    checks["unrecognizedPolicyPairRejected"] = bool(
        unrecognized_pair_policy.get("exact") is False
    )
    if not all(checks.values()):
        raise AssertionError(
            "forced diagnostic transport synthetic self-test failed: "
            + json.dumps(checks, sort_keys=True)
        )
    return {"ok": True, "count": len(checks), "checks": checks}


def _force_gpu_skin_diagnostic_snapshot(
    out_dir: Path,
    tag: str,
    pid: int,
    event_since_id: int,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    frozen_fingerprint = json.loads(json.dumps(
        dict(launch_fingerprint or {}), ensure_ascii=False,
    ))
    initial_fingerprint_validation = (
        _validate_launch_instance_fingerprint(frozen_fingerprint, pid)
    )
    result: Dict[str, Any] = {
        "attempted": True,
        "ok": False,
        "beginSeen": False,
        "endSeen": False,
        "blockOrderValid": False,
        "blockAttributionValid": False,
        "selectedBlockComplete": False,
        "quiescent": False,
        "twoCleanSnapshots": False,
        "progressValid": False,
        "cleanSnapshotCount": 0,
        "cleanStreak": 0,
        "attempts": [],
        "pid": pid,
        "immutableLaunchFingerprint": frozen_fingerprint,
        "initialLaunchFingerprintValidation": (
            initial_fingerprint_validation
        ),
        "immutableLaunchFingerprintExact": (
            initial_fingerprint_validation.get("exact") is True
        ),
        "failureClassificationAuthority": (
            1 if initial_fingerprint_validation.get("exact") is True else 0
        ),
    }
    initial_process_authority = _capture_fresh_process_authority(
        pid, frozen_fingerprint, retained_witness=retained_witness,
    )
    result["initialProcessAuthority"] = initial_process_authority
    if pid <= 0 or initial_process_authority.get("aliveExact") is not True:
        result["error"] = (
            "exact launch instance is not authoritatively alive"
        )
        result["runtimeEndedBeforeSnapshot"] = (
            initial_process_authority.get("terminatedExact") is True
        )
        result["processLivenessIndeterminateBeforeSnapshot"] = (
            initial_process_authority.get("indeterminate") is True
        )
        result["failureClassificationAuthority"] = (
            1 if initial_process_authority.get("terminatedExact") is True
            else 0
        )
        _json_write(out_dir / f"forced_gpu_skin_diag_{tag}.json", result)
        return result
    if initial_fingerprint_validation.get("exact") is not True:
        result["error"] = (
            "immutable launch fingerprint unavailable or current process "
            "identity mismatch"
        )
        result["failureClassificationAuthority"] = 0
        _json_write(out_dir / f"forced_gpu_skin_diag_{tag}.json", result)
        return result

    _, snapshot_cursor = _event_lines_since(event_since_id, pid)
    result["eventCursorBefore"] = snapshot_cursor
    deadline = time.monotonic() + 15.0
    max_attempts = 48
    clean_streak: List[Tuple[Dict[str, Any], Dict[str, Any]]] = []
    last_clean_endpoint: Optional[
        Tuple[Dict[str, Any], Dict[str, Any]]
    ] = None
    best_clean: Optional[Dict[str, Any]] = None
    selected: Optional[Dict[str, Any]] = None
    seen_snapshot_ids: set[str] = set()

    for attempt_index in range(1, max_attempts + 1):
        if time.monotonic() >= deadline:
            break
        attempt_process_authority = _capture_fresh_process_authority(
            pid, frozen_fingerprint, retained_witness=retained_witness,
        )
        if attempt_process_authority.get("aliveExact") is not True:
            result["lastProcessAuthority"] = attempt_process_authority
            result["runtimeEndedDuringSnapshot"] = (
                attempt_process_authority.get("terminatedExact") is True
            )
            result["processLivenessIndeterminateDuringSnapshot"] = (
                attempt_process_authority.get("indeterminate") is True
            )
            result["failureClassificationAuthority"] = (
                1 if attempt_process_authority.get(
                    "terminatedExact"
                ) is True else 0
            )
            break

        fingerprint_pre = _validate_launch_instance_fingerprint(
            frozen_fingerprint, pid,
        )
        if fingerprint_pre.get("exact") is not True:
            result["launchFingerprintMismatchDuringSnapshot"] = True
            result["lastLaunchFingerprintValidation"] = fingerprint_pre
            result["failureClassificationAuthority"] = 0
            break

        _, attempt_cursor = _event_lines_since(snapshot_cursor, pid)
        all_attempt_log_offsets = _snapshot_log_offsets()
        primary_log_key = str(LOG_CANDIDATES[0])
        attempt_log_offsets = {
            primary_log_key: all_attempt_log_offsets.get(
                primary_log_key,
                {"exists": False, "size": 0, "mtime": 0.0},
            )
        }
        invocation = _invoke_internal_test_api_exact(
            command="gpu_skin.log_diagnostics",
            payload={"requireQuiescent": True},
            pid=pid,
            timeout_sec=6,
        )
        payload = dict(invocation.get("result", {}) or {})
        snapshot_id = str(payload.get("snapshotId", "") or "")
        invocation_request_id = str(
            dict(invocation.get("request", {}) or {}).get(
                "requestId", "",
            ) or ""
        )
        snapshot_id_unique = bool(
            snapshot_id and snapshot_id not in seen_snapshot_ids
        )
        if snapshot_id:
            seen_snapshot_ids.add(snapshot_id)
        begin_marker = (
            f"DXVK War3GpuSkin: diagSnapshot begin={snapshot_id}"
            if snapshot_id else ""
        )
        complete_value = 1 if payload.get("logged") else 0
        end_marker = (
            f"DXVK War3GpuSkin: diagSnapshot end={snapshot_id} "
            f"complete={complete_value}"
            if snapshot_id else ""
        )
        lines: List[str] = []
        log_lines: List[str] = []
        event_block = _extract_forced_diag_block([], snapshot_id, complete_value)
        log_block = _extract_forced_diag_block([], snapshot_id, complete_value)
        marker_deadline = min(deadline, time.monotonic() + 2.0)
        while begin_marker and end_marker and time.monotonic() < marker_deadline:
            lines, _ = _event_lines_since(attempt_cursor, pid)
            event_block = _extract_forced_diag_block(
                lines, snapshot_id, complete_value
            )
            log_lines = _read_new_log_text(attempt_log_offsets).splitlines()
            log_block = _extract_forced_diag_block(
                log_lines, snapshot_id, complete_value
            )
            if _forced_diag_block_complete(event_block):
                break
            # A non-quiescent request intentionally emits only the exact
            # begin/end marker pair and quiescencePre. Its payload cannot
            # satisfy the full diagnostic contract, so waiting the whole
            # marker deadline only burns the bounded evidence window. Exact
            # uniqueness/order is revalidated from the final collected
            # evidence, and this path can never become a clean snapshot.
            if complete_value == 0 and (
                event_block.get("orderValid")
                or log_block.get("orderValid")
            ):
                break
            time.sleep(0.05)
        fingerprint_post = _validate_launch_instance_fingerprint(
            frozen_fingerprint, pid,
        )
        fingerprint_validation = {
            "fingerprintAvailable": frozen_fingerprint.get(
                "available"
            ) is True,
            "pre": fingerprint_pre,
            "post": fingerprint_post,
            "exact": bool(
                fingerprint_pre.get("exact") is True
                and fingerprint_post.get("exact") is True
            ),
        }
        transport_contract = _forced_diag_transport_contract(
            invocation, pid, snapshot_id, fingerprint_validation,
        )
        request_id_valid = bool(
            transport_contract.get("exact") is True
            and snapshot_id_unique
            and payload.get("command") == "gpu_skin.log_diagnostics"
            and payload.get("handled") is True
        )
        if fingerprint_post.get("exact") is not True:
            result["launchFingerprintMismatchDuringSnapshot"] = True
            result["lastLaunchFingerprintValidation"] = fingerprint_post
            result["failureClassificationAuthority"] = 0
        event_block_complete = _forced_diag_block_complete(event_block)
        log_block_complete = _forced_diag_block_complete(log_block)
        block_attribution = _forced_diag_block_attribution(
            event_block, log_block,
        )
        selected_live_block = dict(
            block_attribution.get("authoritativeBlock", {}) or {}
        )
        evidence_source = str(block_attribution.get("source", "none"))
        block_order_valid = bool(
            block_attribution.get("authoritativeComplete")
        )
        block_attribution_valid = bool(
            block_attribution.get("authoritativeComplete")
        )
        partial_marker_fallback = False
        block_text = str(selected_live_block.get("text", "") or "")
        parsed = _parse_gpu_skin_diag(block_text, {})
        pre_q = parsed["quiescence"]["pre"]
        post_q = parsed["quiescence"]["post"]
        block_contract = _forced_diag_block_contract(block_text)
        clean = bool(
            request_id_valid
            and invocation.get("ok")
            and payload.get("logged")
            and payload.get("requireQuiescent")
            and payload.get("quiescent")
            and complete_value == 1
            and block_attribution_valid
            and block_contract["ok"]
            and _quiescence_diag_consistent(parsed)
        )
        attempt = {
            "index": attempt_index,
            "snapshotId": snapshot_id,
            "beginMarker": begin_marker,
            "endMarker": end_marker,
            "beginSeen": bool(selected_live_block.get("beginSeen")),
            "endSeen": bool(selected_live_block.get("endSeen")),
            "blockOrderValid": block_order_valid,
            "blockAttributionValid": block_attribution_valid,
            "partialMarkerFallback": partial_marker_fallback,
            "evidenceSource": evidence_source,
            "eventBlockOrderValid": bool(event_block.get("orderValid")),
            "eventBlockComplete": event_block_complete,
            "logBlockOrderValid": bool(log_block.get("orderValid")),
            "logBlockComplete": log_block_complete,
            "sharedLogAuthorizationAuthority": 0,
            "requestId": invocation_request_id,
            "requestIdValid": request_id_valid,
            "transportExact": transport_contract.get("exact") is True,
            "transportContract": transport_contract,
            "launchFingerprintValidation": fingerprint_validation,
            "launchFingerprintExact": (
                fingerprint_validation.get("exact") is True
            ),
            "callerIssuedRequestId": transport_contract.get(
                "callerIssuedRequestId"
            ),
            "responseRequestId": transport_contract.get(
                "responseRequestId"
            ),
            "invocationOk": bool(invocation.get("ok")),
            "invocationError": str(invocation.get("error", "") or ""),
            "payload": payload,
            "clean": clean,
            "blockSha256": hashlib.sha256(
                block_text.encode("utf-8")
            ).hexdigest() if block_text else "",
            "blockText": block_text,
            "blockContract": block_contract,
            "quiescencePre": pre_q,
            "quiescencePost": post_q,
            "cumulative": {
                "flush": parsed["protocol"].get("flushCallbacks"),
                "jobsSubmitted": parsed["compute"]["jobs"][1],
                "kernelBypassed": parsed["kernel"].get("bypassedCalls"),
                "poisonCreate": parsed["nativePoison"].get("create"),
                "poisonHit": parsed["nativePoison"].get("hit"),
            },
            "eventCursorBefore": attempt_cursor,
            "frameIndexReportOnly": payload.get("frameIndex"),
        }
        result["attempts"].append(attempt)
        if clean:
            result["cleanSnapshotCount"] += 1
            best_clean = attempt
            if clean_streak and _clean_diag_progressed(
                    clean_streak[-1][1], parsed):
                clean_streak.append((attempt, parsed))
            else:
                clean_streak = [(attempt, parsed)]
            pair_contract = _clean_pair_across_transients_contract(
                last_clean_endpoint, (attempt, parsed), result["attempts"],
            )
            if pair_contract["valid"]:
                sample_selection = (
                    _clean_pair_production_sample_selection_contract(
                        last_clean_endpoint[1], parsed,
                    )
                )
                attempt["cleanPairProductionSampleSelection"] = (
                    sample_selection
                )
                if sample_selection.get("exact") is True:
                    selected = attempt
                    result["twoCleanSnapshots"] = True
                    result["progressValid"] = True
                    result["cleanPairIds"] = [
                        last_clean_endpoint[0]["snapshotId"], snapshot_id,
                    ]
                    result["cleanPairGapAttempts"] = pair_contract[
                        "gapAttempts"
                    ]
                    result["cleanPairAttemptIndices"] = pair_contract[
                        "endpointAttemptIndices"
                    ]
                    result["cleanPairSelectionContract"] = pair_contract
                    result[
                        "cleanPairProductionSampleSelectionContract"
                    ] = sample_selection
                    result["selectionReason"] = (
                        "cleanPairTailProductionSampleExact"
                        if pair_contract["gapAttempts"] == 0
                        else "cleanPairAcrossTransientProductionSampleExact"
                    )
                else:
                    result.setdefault(
                        "cleanPairProductionSampleRejections", []
                    ).append({
                        "endpointAttemptIndices": pair_contract[
                            "endpointAttemptIndices"
                        ],
                        "gapAttempts": pair_contract["gapAttempts"],
                        "contract": sample_selection,
                    })
            last_clean_endpoint = (attempt, parsed)
        else:
            clean_streak = []
        result["cleanStreak"] = len(clean_streak)
        if selected is not None:
            break
        time.sleep(0.1)

    if best_clean is not None:
        result["bestCleanSnapshotId"] = best_clean["snapshotId"]
    if selected is not None:
        result.setdefault("selectionReason", "cleanPairTail")
    elif best_clean is not None:
        # Preserve the strongest report-only evidence when the strict pair
        # gate cannot close. This does not synthesize a pair or make the
        # forced snapshot pass: twoCleanSnapshots/progress/resource-delta
        # remain false and therefore fail closed.
        selected = best_clean
        result["selectionReason"] = "bestCleanFallback"
    elif result["attempts"]:
        selected = result["attempts"][-1]
        result["selectionReason"] = "lastAttemptFallback"
    if selected is not None:
        result["snapshotId"] = selected["snapshotId"]
        result["beginMarker"] = selected["beginMarker"]
        result["endMarker"] = selected["endMarker"]
        result["beginSeen"] = selected["beginSeen"]
        result["endSeen"] = selected["endSeen"]
        result["blockOrderValid"] = selected["blockOrderValid"]
        result["blockAttributionValid"] = selected[
            "blockAttributionValid"
        ]
        result["partialMarkerFallback"] = selected[
            "partialMarkerFallback"
        ]
        result["frameIndexReportOnly"] = selected["frameIndexReportOnly"]
        result["logged"] = bool(selected["payload"].get("logged"))
        result["quiescent"] = bool(selected["payload"].get("quiescent"))
        result["selectedBlockComplete"] = bool(
            selected["requestIdValid"]
            and selected["transportExact"]
            and selected["launchFingerprintExact"]
            and selected["evidenceSource"] == "exact-pid-dbwin"
            and selected["invocationOk"]
            and selected["payload"].get("logged")
            and selected["blockAttributionValid"]
            and selected["blockContract"].get("ok")
        )
        result["invocation"] = {
            "ok": selected["invocationOk"],
            "error": selected["invocationError"],
            "result": selected["payload"],
            "requestId": selected["requestId"],
            "callerIssuedRequestId": selected["callerIssuedRequestId"],
            "responseRequestId": selected["responseRequestId"],
            "transportExact": selected["transportExact"],
            "launchFingerprintExact": selected[
                "launchFingerprintExact"
            ],
            "transportContract": selected["transportContract"],
        }
    result["ok"] = bool(
        result.get("twoCleanSnapshots")
        and result.get("progressValid")
        and result.get("logged")
        and result.get("quiescent")
        and result.get("blockAttributionValid")
        and result.get("selectedBlockComplete")
        and result.get("immutableLaunchFingerprintExact")
        and not result.get("launchFingerprintMismatchDuringSnapshot")
    )
    if not result["ok"] and "error" not in result:
        result["error"] = (
            "quiescence evidence timeout: two chronological clean snapshot "
            "endpoints with flush progress were not observed"
        )
    _json_write(out_dir / f"forced_gpu_skin_diag_{tag}.json", result)
    return result


def _collect_session_evidence(
    out_dir: Path,
    tag: str,
    pid: int,
    log_offsets: Dict[str, Dict[str, Any]],
    event_since_id: int,
    runtime_data: Dict[str, Any],
    requested_sidecar_policy: str,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Tuple[Dict[str, Any], List[str], Dict[str, Any], List[str], Dict[str, Any], int]:
    forced_snapshot = _force_gpu_skin_diagnostic_snapshot(
        out_dir, tag, pid, event_since_id, launch_fingerprint,
        retained_witness,
    )
    debug = sync_all_debug(
        war3_dir=str(WAR3_DIR), event_limit=20000, tail_lines=20000,
        include_dbwin_events=True, include_perf_reports=False, include_log_files=True,
    )
    _json_write(out_dir / f"debug_all_{tag}_pre_stop.json", debug)
    event_lines, newest_event_id = _event_lines_since(event_since_id, pid)
    _text_write(out_dir / f"debugview_{tag}_pid.log", "\n".join(event_lines) + "\n")
    gpu_lines = [line for line in event_lines if "GpuSkin" in line]
    _text_write(out_dir / f"debugview_gpu_skin_{tag}.log", "\n".join(gpu_lines) + "\n")
    log_copies = _copy_new_log_bytes(log_offsets, out_dir, tag)
    unattributed_text = ""
    for entry in log_copies.values():
        unattributed_text += "\n" + Path(str(entry["output"])).read_text(encoding="utf-8", errors="replace")
    marker_log_text = ""
    # Logger may truncate the primary file when the new process opens it. A
    # prelaunch byte offset can therefore cut away the forced block after the
    # new file grows past the old size. Revalidation reads the current complete
    # primary log is retained only for report-only troubleshooting. Hard
    # snapshot attribution requires the exact launch-PID DBWIN block; a shared
    # log can never authorize either clean-pair endpoint.
    if LOG_CANDIDATES[0].is_file():
        marker_log_text = LOG_CANDIDATES[0].read_text(
            encoding="utf-8", errors="replace"
        )
    target_text = "\n".join(event_lines)
    target_diag_text = ""
    snapshot_cursor = int(forced_snapshot.get("eventCursorBefore", event_since_id))
    snapshot_lines, _ = _event_lines_since(snapshot_cursor, pid)
    collection_fingerprint_validation = (
        _validate_launch_instance_fingerprint(
            dict(launch_fingerprint or {}), pid,
        )
    )
    collection_fingerprint_exact = (
        collection_fingerprint_validation.get("exact") is True
    )
    forced_snapshot["collectionLaunchFingerprintValidation"] = (
        collection_fingerprint_validation
    )
    forced_snapshot["collectionLaunchFingerprintExact"] = (
        collection_fingerprint_exact
    )
    forced_snapshot["failureClassificationAuthority"] = (
        1 if collection_fingerprint_exact else 0
    )
    forced_snapshot["reportOnly"] = not collection_fingerprint_exact
    _text_write(
        out_dir / f"debugview_gpu_skin_snapshot_{tag}.log",
        "\n".join(snapshot_lines) + "\n",
    )
    selected_snapshot_id = str(
        forced_snapshot.get("snapshotId", "") or ""
    )
    attempt_by_id = {
        str(attempt.get("snapshotId", "") or ""): attempt
        for attempt in list(forced_snapshot.get("attempts", []) or [])
    }
    selected_complete = 1 if forced_snapshot.get("logged") else 0
    selected_attempt = attempt_by_id.get(selected_snapshot_id, {})
    selected_event_block = _extract_forced_diag_block(
        snapshot_lines, selected_snapshot_id, selected_complete
    )
    selected_log_block = _extract_forced_diag_block(
        _strip_ansi(marker_log_text).splitlines(),
        selected_snapshot_id,
        selected_complete,
    )
    selected_event_complete = _forced_diag_block_complete(
        selected_event_block
    )
    selected_log_complete = _forced_diag_block_complete(selected_log_block)
    selected_attribution = _forced_diag_block_attribution(
        selected_event_block, selected_log_block,
    )
    selected_block = dict(
        selected_attribution.get("authoritativeBlock", {}) or {}
    )
    selected_block_source = str(selected_attribution.get("source", "none"))
    selected_block_attributed = bool(
        selected_attribution.get("authoritativeComplete")
    )
    selected_block_revalidated = selected_block_attributed
    target_diag_text = str(selected_block.get("text", "") or "")
    block_order_valid = bool(selected_block.get("orderValid"))
    selected_block_complete = bool(
        selected_complete == 1
        and selected_block_attributed
        and selected_block_revalidated
        and selected_block["contract"].get("ok")
        and selected_attempt.get("requestIdValid") is True
        and selected_attempt.get("transportExact") is True
        and selected_attempt.get("launchFingerprintExact") is True
        and collection_fingerprint_exact
        and selected_attempt.get("evidenceSource") == "exact-pid-dbwin"
        and selected_attempt.get("invocationOk") is True
        and selected_attempt.get("blockSha256") == selected_block.get("sha256")
    )
    forced_snapshot["beginSeen"] = selected_block["beginSeen"]
    forced_snapshot["endSeen"] = selected_block["endSeen"]
    forced_snapshot["blockOrderValid"] = block_order_valid
    forced_snapshot["blockAttributionValid"] = selected_block_attributed
    forced_snapshot["selectedBlockSource"] = selected_block_source
    forced_snapshot["selectedSharedLogBlockCompleteReportOnly"] = (
        selected_log_complete
    )
    forced_snapshot["sharedLogAuthorizationAuthority"] = 0
    forced_snapshot["blockSeenInCollectedEvidence"] = (
        selected_block_revalidated
    )
    forced_snapshot["selectedBlockComplete"] = selected_block_complete

    pair_ids = list(forced_snapshot.get("cleanPairIds", []) or [])
    pair_blocks: List[Dict[str, Any]] = []
    pair_diags: List[Dict[str, Any]] = []
    pair_attempts: List[Dict[str, Any]] = []
    pair_valid = bool(
        collection_fingerprint_exact
        and len(pair_ids) == 2 and pair_ids[0] != pair_ids[1]
    )
    if pair_valid:
        for snapshot_id in pair_ids:
            attempt = attempt_by_id.get(snapshot_id, {})
            event_block = _extract_forced_diag_block(
                snapshot_lines, snapshot_id, 1
            )
            log_block = _extract_forced_diag_block(
                _strip_ansi(marker_log_text).splitlines(), snapshot_id, 1
            )
            event_complete = _forced_diag_block_complete(event_block)
            log_complete = _forced_diag_block_complete(log_block)
            attribution = _forced_diag_block_attribution(
                event_block, log_block,
            )
            block = dict(attribution.get("authoritativeBlock", {}) or {})
            block_source = str(attribution.get("source", "none"))
            block_attributed = bool(
                attribution.get("authoritativeComplete")
            )
            block_revalidated = block_attributed
            parsed = _parse_gpu_skin_diag(
                str(block.get("text", "") or ""), {},
                requested_sidecar_policy=requested_sidecar_policy,
            )
            block_valid = bool(
                block_attributed
                and block_revalidated
                and block["contract"].get("ok")
                and attempt.get("requestIdValid") is True
                and attempt.get("transportExact") is True
                and attempt.get("launchFingerprintExact") is True
                and collection_fingerprint_exact
                and attempt.get("evidenceSource") == "exact-pid-dbwin"
                and attempt.get("clean") is True
                and attempt.get("blockSha256") == block.get("sha256")
                and _quiescence_diag_consistent(parsed)
            )
            pair_blocks.append({
                "snapshotId": snapshot_id,
                "valid": block_valid,
                "orderValid": bool(block.get("orderValid")),
                "source": block_source,
                "sharedLogCompleteReportOnly": log_complete,
                "sharedLogAuthorizationAuthority": 0,
                "transportExact": attempt.get("transportExact") is True,
                "launchFingerprintExact": attempt.get(
                    "launchFingerprintExact"
                ) is True,
                "sha256": block.get("sha256"),
                "contract": block.get("contract"),
            })
            pair_attempts.append(attempt)
            pair_diags.append(parsed)
            pair_valid = pair_valid and block_valid
    pair_chronology_contract = _clean_pair_across_transients_contract(
        (pair_attempts[0], pair_diags[0])
        if len(pair_attempts) == 2 and len(pair_diags) == 2 else None,
        (pair_attempts[1], pair_diags[1])
        if len(pair_attempts) == 2 and len(pair_diags) == 2 else None,
        list(forced_snapshot.get("attempts", []) or []),
    )
    reported_gap_attempts = forced_snapshot.get("cleanPairGapAttempts")
    pair_metadata_valid = bool(
        pair_chronology_contract.get("valid") is True
        and selected_snapshot_id == (pair_ids[1] if len(pair_ids) == 2 else "")
        and isinstance(reported_gap_attempts, int)
        and not isinstance(reported_gap_attempts, bool)
        and reported_gap_attempts == pair_chronology_contract.get(
            "gapAttempts"
        )
    )
    pair_valid = pair_valid and pair_metadata_valid
    pair_progress = bool(
        pair_valid and pair_chronology_contract.get("progressValid") is True
    )
    pair_resource_delta_closed = False
    pair_vs_shadow_route: Dict[str, Any] = {
        "present": False,
        "exact": False,
        "route": "unavailable",
        "replayCommitEqualityRequired": False,
        "reason": "clean-pair 不可用",
    }
    pair_hot_path_delta: Dict[str, Any] = {
        "valid": False,
        "reason": "clean pair unavailable",
    }
    pair_fast_partition_delta: Dict[str, Any] = {
        "valid": False,
        "reason": "clean pair unavailable",
    }
    pair_telemetry_delta: Dict[str, Any] = {
        "valid": False,
        "exact": False,
        "reason": "clean pair unavailable",
    }
    pair_begin_sample_cadence: Dict[str, Any] = {
        "present": False,
        "exact": False,
        "reason": "clean pair unavailable",
    }
    pair_production_sample_timing: Dict[str, Any] = {
        "valid": False,
        "exact": False,
        "reason": "clean pair unavailable",
    }
    pair_outside_admission_attribution: Dict[str, Any] = {
        "present": False,
        "exact": False,
        "reason": "clean pair unavailable",
    }
    if pair_valid and pair_progress and len(pair_diags) == 2:
        previous_jobs = pair_diags[0]["compute"]["jobs"][1]
        current_jobs = pair_diags[1]["compute"]["jobs"][1]
        previous_retired = pair_diags[0]["lifetime"]["outputLeaseRetired"]
        current_retired = pair_diags[1]["lifetime"]["outputLeaseRetired"]
        previous_pending = pair_diags[0]["lifetime"]["outputPending"]
        current_pending = pair_diags[1]["lifetime"]["outputPending"]
        previous_input_storage = pair_diags[0]["VSRoute"][
            "inputSubmitted"
        ][0]
        current_input_storage = pair_diags[1]["VSRoute"][
            "inputSubmitted"
        ][0]
        previous_input_only_jobs = pair_diags[0]["VSRoute"][
            "inputOnly"
        ][2]
        current_input_only_jobs = pair_diags[1]["VSRoute"][
            "inputOnly"
        ][2]
        pair_resource_delta_closed = _gpu_skin_resource_delta_closed(
            previous_jobs, current_jobs,
            previous_input_storage, current_input_storage,
            previous_retired, current_retired,
            previous_pending, current_pending,
            previous_input_only_jobs, current_input_only_jobs,
        )
        pair_vs_shadow_route = _vs_shadow_route_pair_contract(
            pair_diags[0], pair_diags[1],
        )
        pair_hot_path_delta = _hot_path_timing_delta(
            pair_diags[0]["hotPathTiming"],
            pair_diags[1]["hotPathTiming"],
        )
        pair_fast_partition_delta = _production_fast_partition_delta(
            pair_diags[0]["productionFastPath"],
            pair_diags[1]["productionFastPath"],
        )
        pair_telemetry_delta = _manager_dispatch_telemetry_delta(
            pair_diags[0]["managerDispatch"],
            pair_diags[1]["managerDispatch"],
            pair_fast_partition_delta,
            pair_diags[0]["dispatchCpuOnlySeal"],
            pair_diags[1]["dispatchCpuOnlySeal"],
            pair_diags[0]["nativePoisonO1Authority"],
            pair_diags[1]["nativePoisonO1Authority"],
        )
        pair_begin_sample_cadence = _native_begin_sample_cadence_contract(
            pair_hot_path_delta, pair_fast_partition_delta,
        )
        pair_production_sample_timing = _production_sample_timing_delta(
            pair_diags[0]["productionSampleTiming"],
            pair_diags[1]["productionSampleTiming"],
            (
                pair_fast_partition_delta.get("rawUploads") -
                pair_fast_partition_delta.get(
                    "directOriginalDeltas", {}
                ).get("directOriginalCompleted")
                if isinstance(
                    pair_fast_partition_delta.get("rawUploads"), int
                ) and isinstance(
                    pair_fast_partition_delta.get(
                        "directOriginalDeltas", {}
                    ).get("directOriginalCompleted"), int
                ) and 0 <= pair_fast_partition_delta.get(
                    "directOriginalDeltas", {}
                ).get("directOriginalCompleted") <=
                    pair_fast_partition_delta.get("rawUploads")
                else None
            ),
        )
        previous_frame = pair_diags[0]["quiescence"]["post"].get("frame")
        current_frame = pair_diags[1]["quiescence"]["post"].get("frame")
        pair_frame_delta = (
            current_frame - previous_frame
            if isinstance(previous_frame, int)
            and isinstance(current_frame, int)
            and current_frame > previous_frame else None
        )
        _add_production_event_window_estimates(
            pair_production_sample_timing, pair_frame_delta,
        )
    pair_diagnostic_policy_full = bool(
        len(pair_diags) == 2
        and all(
            item.get("diagnosticPolicy", {}).get("fullExact") is True
            for item in pair_diags
        )
    )
    pair_diagnostic_policy_light = bool(
        len(pair_diags) == 2
        and all(
            item.get("diagnosticPolicy", {}).get("lightExact") is True
            for item in pair_diags
        )
    )
    pair_diagnostic_policy = {
        "full": pair_diagnostic_policy_full,
        "light": pair_diagnostic_policy_light,
        "recognizedExact": (
            pair_diagnostic_policy_full != pair_diagnostic_policy_light
        ),
    }
    if pair_valid and pair_progress and len(pair_diags) == 2:
        pair_outside_admission_attribution = (
            _outside_admission_pair_contract(
                pair_diags[0].get("outsideAdmissionAttribution", {}),
                pair_diags[1].get("outsideAdmissionAttribution", {}),
                pair_diagnostic_policy_full,
                pair_diagnostic_policy_light,
            )
        )
    pair_telemetry_delta = _manager_dispatch_telemetry_policy(
        pair_telemetry_delta,
        pair_diagnostic_policy_full,
        pair_diagnostic_policy_light,
    )
    pair_production_sample_policy = (
        _production_sample_timing_policy_contract(
            pair_diags[0] if len(pair_diags) == 2 else {},
            pair_diags[1] if len(pair_diags) == 2 else {},
            pair_production_sample_timing,
        )
    )
    pair_production_sample_timing.update(pair_production_sample_policy)
    pair_fast_delta = pair_fast_partition_delta.get(
        "outsideNativeFastPathDelta"
    )
    pair_callback_skip_delta = pair_fast_partition_delta.get(
        "outsideCallbacksSkippedDelta"
    )
    pair_reject_scope_delta = pair_fast_partition_delta.get(
        "deltas", {}
    ).get("rejectScope")
    pair_kernel_batch_delta = pair_fast_partition_delta.get(
        "kernelBatchesDelta"
    )
    pair_poison_scan_delta = pair_fast_partition_delta.get(
        "poisonScanAttemptsDelta"
    )
    pair_poison_no_overlap_delta = pair_fast_partition_delta.get(
        "poisonNoOverlapDelta"
    )
    pair_poison_overlap_delta = pair_fast_partition_delta.get(
        "poisonOverlapDelta"
    )
    pair_poison_read_fail_delta = pair_fast_partition_delta.get(
        "poisonReadFailDelta"
    )
    pair_marker_conflict_delta = pair_fast_partition_delta.get(
        "markerConflictsDelta"
    )
    pair_cover_deltas = pair_fast_partition_delta.get("coverDeltas", {})
    pair_independent_pin_begins = pair_fast_partition_delta.get(
        "independentPinBeginsDelta"
    )
    pair_independent_pin_ends = pair_fast_partition_delta.get(
        "independentPinEndsDelta"
    )
    pair_direct_deltas = pair_fast_partition_delta.get(
        "directOriginalDeltas", {}
    )
    pair_direct_active_endpoints = pair_fast_partition_delta.get(
        "directOriginalActiveEndpoints", {}
    )
    pair_raw_uploads = pair_fast_partition_delta.get("rawUploads")
    pair_generic_outside_reject_delta = (
        pair_callback_skip_delta - pair_fast_delta
        if isinstance(pair_callback_skip_delta, int)
        and isinstance(pair_fast_delta, int) else None
    )
    pair_outside_native_fast_policy = {
        "present": all(
            isinstance(value, int) for value in (
                pair_fast_delta, pair_callback_skip_delta,
                pair_reject_scope_delta, pair_kernel_batch_delta,
                pair_generic_outside_reject_delta, pair_raw_uploads,
                pair_poison_scan_delta, pair_poison_no_overlap_delta,
                pair_poison_overlap_delta, pair_poison_read_fail_delta,
                pair_marker_conflict_delta,
                *pair_cover_deltas.values(), pair_independent_pin_begins,
                pair_independent_pin_ends,
                *pair_direct_deltas.values(),
                *pair_direct_active_endpoints.values(),
            )
        ),
        "full": pair_diagnostic_policy_full,
        "light": pair_diagnostic_policy_light,
        "rawUploads": pair_raw_uploads,
        "outsideNativeFastPathDelta": pair_fast_delta,
        "outsideCallbacksSkippedDelta": pair_callback_skip_delta,
        "rejectScopeDelta": pair_reject_scope_delta,
        "genericOutsideRejectDelta": pair_generic_outside_reject_delta,
        "rejectKernelBatchesDelta": pair_kernel_batch_delta,
        "poisonScanAttemptsDelta": pair_poison_scan_delta,
        "poisonNoOverlapDelta": pair_poison_no_overlap_delta,
        "poisonOverlapDelta": pair_poison_overlap_delta,
        "poisonReadFailDelta": pair_poison_read_fail_delta,
        "markerConflictsDelta": pair_marker_conflict_delta,
        "coverDeltas": pair_cover_deltas,
        "independentPinBeginsDelta": pair_independent_pin_begins,
        "independentPinEndsDelta": pair_independent_pin_ends,
        "directOriginalDeltas": pair_direct_deltas,
        "directOriginalActiveEndpoints": pair_direct_active_endpoints,
    }
    pair_outside_native_fast_policy["exact"] = bool(
        pair_outside_native_fast_policy["present"]
        and pair_fast_partition_delta.get("valid") is True
        and pair_raw_uploads > 0
        and 0 <= pair_fast_delta <= pair_reject_scope_delta
        and pair_fast_delta <= pair_callback_skip_delta
        and 0 <= pair_kernel_batch_delta <=
            pair_generic_outside_reject_delta
        and pair_poison_scan_delta == (
            pair_poison_no_overlap_delta + pair_poison_overlap_delta
            + pair_poison_read_fail_delta
        )
        and pair_poison_no_overlap_delta <= pair_fast_delta
        and pair_marker_conflict_delta == 0
        and sum(pair_cover_deltas.values()) + pair_direct_deltas.get(
            "directOriginalCompleted", -1
        ) == pair_fast_delta
        and pair_direct_deltas.get("directOriginalAttempts") ==
            pair_direct_deltas.get("directOriginalCompleted") +
            pair_direct_deltas.get("directOriginalCancellations")
        and pair_direct_deltas.get("directOriginalKernelCalls") ==
            pair_direct_deltas.get("directOriginalNormalReturns") +
            pair_direct_deltas.get(
                "directOriginalKernelNoNormalReturns"
            )
        and pair_direct_deltas.get("directOriginalKernelCalls") ==
            pair_direct_deltas.get("directOriginalCompleted")
        and pair_direct_deltas.get("directOriginalConflicts") == 0
        and pair_direct_deltas.get("directOriginalCancellations") == 0
        and pair_direct_deltas.get(
            "directOriginalResetCompletedWhileActive"
        ) == 0
        and pair_direct_deltas.get("directOriginalLatePoison") == 0
        and pair_direct_active_endpoints.get("previous") == 0
        and pair_direct_active_endpoints.get("current") == 0
        and pair_independent_pin_begins == pair_independent_pin_ends
        and pair_independent_pin_begins >= pair_cover_deltas.get(
            "independent", 0
        )
        and (
            pair_diagnostic_policy_full
            and pair_fast_delta == 0 and pair_kernel_batch_delta == 0
            and pair_poison_scan_delta == 0
            and pair_poison_no_overlap_delta == 0
            and pair_poison_overlap_delta == 0
            and pair_poison_read_fail_delta == 0
            and all(value == 0 for value in pair_direct_deltas.values())
            and sum(pair_cover_deltas.values()) == 0
            and pair_independent_pin_begins == 0
            and pair_independent_pin_ends == 0
            or pair_diagnostic_policy_light
            and pair_fast_delta > 0 and pair_kernel_batch_delta > 0
            and pair_direct_deltas.get("directOriginalCompleted", 0) > 0
            and pair_poison_scan_delta > 0
            and pair_poison_no_overlap_delta > 0
            and pair_cover_deltas.get("semantic", 0) > 0
        )
    )
    pair_native_poison_sidecar_policy: Dict[str, Any] = {
        "present": False,
        "exact": False,
        "requestedPolicy": requested_sidecar_policy,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "reason": "clean pair unavailable",
    }
    pair_native_poison_shadow_policy: Dict[str, Any] = {
        "present": False,
        "recognizedExact": False,
        "full": pair_diagnostic_policy_full,
        "light": pair_diagnostic_policy_light,
        "fullExact": False,
        "lightExact": False,
        "exact": False,
        "poisonScanAttemptsDelta": pair_poison_scan_delta,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "reason": "clean pair unavailable",
    }
    pair_native_poison_o1_shadow_policy: Dict[str, Any] = {
        "present": False,
        "recognizedExact": False,
        "full": pair_diagnostic_policy_full,
        "light": pair_diagnostic_policy_light,
        "fullExact": False,
        "lightExact": False,
        "exact": False,
        "poisonScanAttemptsDelta": pair_poison_scan_delta,
        "authorizationAuthority": 0,
        "reportOnly": True,
        "promotionEligibleShadow": False,
        "reason": "clean pair unavailable",
    }
    pair_native_poison_o1_authority_policy: Dict[str, Any] = {
        "present": False,
        "recognizedExact": False,
        "full": pair_diagnostic_policy_full,
        "light": pair_diagnostic_policy_light,
        "fullExact": False,
        "lightExact": False,
        "exact": False,
        "authorizationAuthority": 0,
        "reportOnly": False,
        "reason": "clean pair unavailable",
    }
    if pair_valid and pair_progress and len(pair_diags) == 2:
        pair_native_poison_sidecar_policy = (
            _native_poison_sidecar_pair_contract(
                pair_diags[0].get("nativePoisonSidecarPolicy", {}),
                pair_diags[1].get("nativePoisonSidecarPolicy", {}),
                requested_sidecar_policy,
            )
        )
        pair_sidecar_value = GPU_SKIN_POISON_SIDECAR_POLICIES.get(
            requested_sidecar_policy, 0,
        )
        pair_native_poison_shadow_policy = (
            _native_poison_shadow_policy_pair(
                pair_diags[0].get("nativePoisonShadow", {}),
                pair_diags[1].get("nativePoisonShadow", {}),
                pair_poison_scan_delta,
                pair_diagnostic_policy_full,
                pair_diagnostic_policy_light,
                bool(pair_sidecar_value & 1),
            )
        )
        pair_native_poison_o1_shadow_policy = (
            _native_poison_o1_shadow_policy_pair(
                pair_diags[0].get("nativePoisonO1Shadow", {}),
                pair_diags[1].get("nativePoisonO1Shadow", {}),
                pair_poison_scan_delta,
                pair_diagnostic_policy_full,
                pair_diagnostic_policy_light,
                pair_poison_no_overlap_delta,
                pair_poison_overlap_delta,
                pair_poison_read_fail_delta,
                pair_diags[1].get("productionFastPath", {}),
                bool(pair_sidecar_value & 2),
            )
        )
        pair_native_poison_o1_authority_policy = (
            _native_poison_o1_authority_pair_policy(
                pair_diags[0].get("nativePoisonO1Authority", {}),
                pair_diags[1].get("nativePoisonO1Authority", {}),
                pair_diagnostic_policy_full,
                pair_diagnostic_policy_light,
                pair_sidecar_value != 0,
                pair_poison_scan_delta,
                pair_poison_no_overlap_delta,
            )
        )
    pair_full_population_timing = _full_population_native_timing_contract(
        pair_hot_path_delta, pair_fast_partition_delta,
    )
    pair_light_zero_timing = _light_zero_timing_contract(
        pair_hot_path_delta, pair_fast_partition_delta,
    )
    pair_hot_path_delta["fullPopulation"] = pair_full_population_timing
    pair_hot_path_delta["lightZeroTiming"] = pair_light_zero_timing
    forced_snapshot["cleanPairCollected"] = pair_blocks
    forced_snapshot["cleanPairChronologyRevalidated"] = (
        pair_chronology_contract
    )
    forced_snapshot["cleanPairGapAttemptsRevalidated"] = (
        pair_chronology_contract.get("gapAttempts")
    )
    forced_snapshot["cleanPairResourceDeltaClosed"] = (
        pair_resource_delta_closed
    )
    forced_snapshot["cleanPairVsShadowRoute"] = pair_vs_shadow_route
    forced_snapshot["cleanPairHotPathDelta"] = pair_hot_path_delta
    forced_snapshot["cleanPairProductionFastDelta"] = (
        pair_fast_partition_delta
    )
    forced_snapshot["cleanPairTelemetryDelta"] = pair_telemetry_delta
    forced_snapshot["cleanPairNativeBeginSampleCadence"] = (
        pair_begin_sample_cadence
    )
    forced_snapshot["cleanPairProductionSampleTiming"] = (
        pair_production_sample_timing
    )
    forced_snapshot["cleanPairOutsideNativeFastPathPolicy"] = (
        pair_outside_native_fast_policy
    )
    forced_snapshot["cleanPairNativePoisonShadow"] = (
        pair_native_poison_shadow_policy
    )
    forced_snapshot["cleanPairNativePoisonSidecarPolicy"] = (
        pair_native_poison_sidecar_policy
    )
    forced_snapshot["cleanPairNativePoisonO1Shadow"] = (
        pair_native_poison_o1_shadow_policy
    )
    forced_snapshot["cleanPairNativePoisonO1Authority"] = (
        pair_native_poison_o1_authority_policy
    )
    forced_snapshot["cleanPairOutsideAdmissionAttribution"] = (
        pair_outside_admission_attribution
    )
    forced_snapshot["cleanPairDiagnosticPolicy"] = pair_diagnostic_policy
    forced_snapshot["cleanPairDiagnosticPolicyExact"] = (
        pair_diagnostic_policy["recognizedExact"]
    )
    forced_snapshot["cleanPairFullPopulationTiming"] = (
        pair_full_population_timing
    )
    forced_snapshot["cleanPairLightZeroTiming"] = pair_light_zero_timing
    forced_snapshot["cleanPairTimingDeltaValid"] = bool(
        pair_hot_path_delta.get("valid")
        and pair_diagnostic_policy["recognizedExact"]
        and (
            pair_diagnostic_policy_full
            and pair_full_population_timing.get("exact") is True
            or pair_diagnostic_policy_light
            and pair_light_zero_timing.get("exact") is True
        )
    )
    forced_snapshot["cleanPairRevalidatedInCollectedEvidence"] = bool(
        pair_valid and pair_progress
    )
    forced_snapshot["ok"] = bool(
        forced_snapshot.get("ok")
        and selected_block_complete
        and collection_fingerprint_exact
        and pair_valid
        and pair_progress
        and pair_resource_delta_closed
        and pair_vs_shadow_route.get("exact") is True
        and forced_snapshot["cleanPairTimingDeltaValid"]
        and pair_fast_partition_delta.get("valid") is True
        and pair_native_poison_sidecar_policy.get("exact") is True
        and pair_native_poison_shadow_policy.get("exact") is True
        and pair_native_poison_o1_shadow_policy.get("exact") is True
        and pair_native_poison_o1_authority_policy.get("exact") is True
        and pair_telemetry_delta.get("exact") is True
        and pair_production_sample_timing.get("exact") is True
    )
    session_process_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    crash_evidence_authority = bool(
        collection_fingerprint_exact
        and session_process_authority.get("aliveExact") is True
    )
    forced_snapshot["sessionProcessAuthority"] = session_process_authority
    forced_snapshot["sessionProcessAuthorityExact"] = (
        session_process_authority.get("aliveExact") is True
    )
    forced_snapshot["ok"] = bool(
        forced_snapshot.get("ok")
        and session_process_authority.get("aliveExact") is True
    )
    if session_process_authority.get("aliveExact") is not True:
        forced_snapshot["failureClassificationAuthority"] = 0
        forced_snapshot["reportOnly"] = True
    _json_write(out_dir / f"forced_gpu_skin_diag_{tag}.json", forced_snapshot)
    raw_target_crash_matches = _scan_crash_text(target_text)
    crash_matches = (
        raw_target_crash_matches if crash_evidence_authority else []
    )
    unattributed_crash_matches = _scan_crash_text(unattributed_text)
    _text_write(out_dir / f"crash_exception_scan_{tag}.log", "\n".join(crash_matches) + "\n")
    _text_write(
        out_dir / f"unattributed_crash_exception_scan_{tag}.log",
        "\n".join(unattributed_crash_matches) + "\n",
    )
    # 晋级诊断只接受 launch cursor 之后、绑定 immutable launch instance
    # fingerprint 的 exact-PID DBWIN block。named-pipe response 只证明请求链；
    # 共享主日志始终 report-only，不能授权 selected block 或 clean pair。
    diag = _parse_gpu_skin_diag(
        target_diag_text, runtime_data,
        requested_sidecar_policy=requested_sidecar_policy,
    )
    diag["forcedSnapshot"] = forced_snapshot
    diag["sessionProcessAuthority"] = session_process_authority
    diag["crashEvidenceAuthorityExact"] = crash_evidence_authority
    diag["samePidDbwinCrashMatchesReportOnly"] = (
        [] if crash_evidence_authority else raw_target_crash_matches
    )
    unattributed_diag = _parse_gpu_skin_diag(unattributed_text, {})
    _json_write(out_dir / f"gpu_skin_diag_{tag}.json", diag)
    _json_write(
        out_dir / f"gpu_skin_diag_{tag}_unattributed_report_only.json",
        unattributed_diag,
    )
    return debug, event_lines, log_copies, crash_matches, diag, newest_event_id


def _lifecycle_reset(
    out_dir: Path,
    pid: int,
    scope: str,
    poll_wait_sec: float,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    request = {
        "command": "gpu_skin.reset_bridge",
        "payload": {"scope": scope},
        "pid": pid,
        "war3Dir": str(WAR3_DIR),
        "timeoutSec": 6,
    }
    _json_write(out_dir / f"lifecycle_reset_{scope}_request.json", request)
    pre_request_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    if pre_request_authority.get("aliveExact") is True:
        result = invoke_internal_test_api(
            command=request["command"],
            payload_json=json.dumps(request["payload"]),
            pid=pid,
            war3_dir=str(WAR3_DIR),
            timeout_sec=request["timeoutSec"],
        )
    else:
        result = {
            "ok": False,
            "skipped": True,
            "error": "launch process identity not exact before reset",
        }
    _json_write(out_dir / f"lifecycle_reset_{scope}_result.json", result)
    polls = _poll_runtime_steps(
        pid, count=3, wait_sec=poll_wait_sec,
        launch_fingerprint=launch_fingerprint,
        retained_witness=retained_witness,
    )
    _json_write(out_dir / f"lifecycle_reset_{scope}_polls.json", polls)
    return {
        "scope": scope,
        "request": request,
        "result": result,
        "preRequestProcessAuthority": pre_request_authority,
        "polls": polls,
        "aliveAfter": bool(polls and polls[-1].get("running")),
    }


def _set_outline_all_for_test(
    out_dir: Path,
    tag: str,
    pid: int,
    enabled: bool = True,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    command = "gpu_skin.outline_test_mode"
    process_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    if process_authority.get("aliveExact") is not True:
        record = {
            "pid": pid,
            "tag": tag,
            "enabled": enabled,
            "applied": False,
            "transportOk": False,
            "commandHandled": False,
            "targetAndSettingsApplied": False,
            "materialReady": False,
            "singleStageActivationClean": False,
            "failureReason": "launchProcessIdentityNotExact",
            "processAuthority": process_authority,
            "invocation": {},
            "payload": {},
        }
        _json_write(out_dir / f"outline_actuation_{tag}.json", record)
        return record
    invocation = invoke_internal_test_api(
        command=command,
        payload_json=json.dumps({"enabled": enabled}),
        pid=pid,
        war3_dir=str(WAR3_DIR),
        timeout_sec=30,
    )
    payload = dict(invocation.get("result", {}) or {})
    response = dict(invocation.get("response", {}) or {})
    request_id = str(invocation.get("requestId", "") or "")
    transport_ok = bool(
        invocation.get("mode") == "control-plane"
        and request_id
        and response.get("requestId") == request_id
        and str(invocation.get("pipeName", "") or "").endswith(f"_{pid}")
    )
    command_handled = bool(
        transport_ok
        and payload.get("handled") is True
        and payload.get("command") == command
        and payload.get("enabled") is enabled
    )
    target_and_settings_applied = bool(
        command_handled
        and payload.get("allObjectsEnabled") is enabled
        and payload.get("settingsAvailable") is True
        and (
            not enabled
            or (
                payload.get("hasOutlineTargets") is True
                and payload.get("settingsOutlineEnabled") is True
            )
        )
    )
    material_ready = bool(
        payload.get("exactPackMatched") is True
        and payload.get("sourcePack") == "Default Override"
        and payload.get("materialExists") is True
        and payload.get("overrideActive") is True
        and payload.get("materialCompiled") is True
        and payload.get("materialCompileFailed") is False
        and payload.get("leaseActive") is True
        and payload.get("leaseConflict") is False
        and payload.get("testModeStateActive") is True
        and payload.get("ownershipRetainedForRetry") is False
        and isinstance(payload.get("leaseId"), int)
        and payload.get("leaseId", 0) > 0
        and isinstance(payload.get("generation"), int)
        and payload.get("generation", 0) > 0
    )
    single_stage_activation_clean = bool(
        payload.get("stageActivationApplied") is True
        and payload.get("activatedStage") == "Outline"
        and payload.get("otherStageOverridesChanged") is False
        and payload.get("packEnabledMutated") is False
        and isinstance(payload.get("worldOverrideBefore"), bool)
        and isinstance(payload.get("worldOverrideAfter"), bool)
        and payload.get("worldOverrideBefore") ==
            payload.get("worldOverrideAfter")
        and isinstance(payload.get("activeStageMaskBefore"), int)
        and isinstance(payload.get("activeStageMaskAfter"), int)
        and payload.get("activeStageMaskAfter") ==
            (payload.get("activeStageMaskBefore") | (1 << 4))
        and (
            payload.get("activeStageMaskAfter") & ~(1 << 4)
        ) == (
            payload.get("activeStageMaskBefore") & ~(1 << 4)
        )
    )
    applied = bool(
        transport_ok
        and command_handled
        and target_and_settings_applied
        and material_ready
        and single_stage_activation_clean
        and invocation.get("ok")
        and payload.get("applied") is True
    )
    if applied:
        failure_reason = ""
    elif not transport_ok:
        failure_reason = "transportOrCorrelationFailed"
    elif not command_handled:
        failure_reason = "commandNotHandled"
    elif not target_and_settings_applied:
        failure_reason = "outlineTargetOrSettingsNotApplied"
    elif not material_ready:
        failure_reason = "outlineMaterialNotReady"
    else:
        failure_reason = (
            "singleStageActivationScopeViolation"
            if not single_stage_activation_clean
            else "reportedApplyFailed"
        )
    record = {
        "pid": pid,
        "tag": tag,
        "enabled": enabled,
        "applied": applied,
        "transportOk": transport_ok,
        "commandHandled": command_handled,
        "targetAndSettingsApplied": target_and_settings_applied,
        "materialReady": material_ready,
        "singleStageActivationClean": single_stage_activation_clean,
        "failureReason": failure_reason,
        "processAuthority": process_authority,
        "authority": "exact-PID internal-test command on the War3 main thread",
        "invocation": invocation,
        "payload": payload,
    }
    _json_write(out_dir / f"outline_actuation_{tag}.json", record)
    return record


def _restore_outline_all_for_test(
    out_dir: Path,
    tag: str,
    pid: int,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    command = "gpu_skin.outline_test_mode"
    process_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    process_alive_at_attempt = (
        process_authority.get("aliveExact") is True
    )
    if not process_alive_at_attempt:
        record = {
            "pid": pid,
            "tag": tag,
            "restored": False,
            "processAliveAtAttempt": False,
            "processAuthority": process_authority,
            "controlPlaneReachable": False,
            "skipped": "target process not alive",
            "authority": "exact-PID internal-test lease restore on the War3 main thread",
            "invocation": {},
            "payload": {},
        }
        _json_write(out_dir / f"outline_restore_{tag}.json", record)
        return record
    invocation = invoke_internal_test_api(
        command=command,
        payload_json=json.dumps({"enabled": False}),
        pid=pid,
        war3_dir=str(WAR3_DIR),
        timeout_sec=10,
    )
    payload = dict(invocation.get("result", {}) or {})
    response = dict(invocation.get("response", {}) or {})
    request_id = str(invocation.get("requestId", "") or "")
    restored = bool(
        invocation.get("mode") == "control-plane"
        and request_id
        and response.get("requestId") == request_id
        and str(invocation.get("pipeName", "") or "").endswith(f"_{pid}")
        and invocation.get("ok")
        and payload.get("handled") is True
        and payload.get("command") == command
        and payload.get("enabled") is False
        and payload.get("applied") is True
        and payload.get("restoreApplied") is True
        and payload.get("shaderRestored") is True
        and payload.get("testLeaseActiveAfterRestore") is False
        and payload.get("restoreScopeClean") is True
        and isinstance(payload.get("restoreStageMaskBefore"), int)
        and isinstance(payload.get("restoreStageMaskAfter"), int)
        and payload.get("restoreStageMaskAfter") ==
            payload.get("restoreStageMaskBefore")
        and payload.get("overrideActive") is False
        and payload.get("leaseActive") is False
        and payload.get("testModeStateActive") is False
        and payload.get("ownershipRetainedForRetry") is False
    )
    record = {
        "pid": pid,
        "tag": tag,
        "restored": restored,
        "processAliveAtAttempt": process_alive_at_attempt,
        "processAuthority": process_authority,
        "controlPlaneReachable": invocation.get("mode") == "control-plane",
        "authority": "exact-PID internal-test lease restore on the War3 main thread",
        "invocation": invocation,
        "payload": payload,
    }
    _json_write(out_dir / f"outline_restore_{tag}.json", record)
    return record


def _run_lifecycle_window_matrix(
    out_dir: Path, pid: int, step_wait_sec: float,
    launch_fingerprint: Optional[Dict[str, Any]] = None,
    retained_witness: Optional[Dict[str, Any]] = None,
) -> List[Dict[str, Any]]:
    marker_authority = _capture_fresh_process_authority(
        pid, launch_fingerprint, retained_witness=retained_witness,
    )
    marker = {
        "step": "marker",
        "pid": pid,
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "process": is_war3_running(pid=pid),
        "processAuthority": marker_authority,
    }
    marker["ok"] = marker_authority.get("aliveExact") is True
    _json_write(out_dir / "lifecycle_marker.json", marker)

    steps: List[Dict[str, Any]] = [marker]
    for name, kwargs in (
        ("resize_client", {"client_w": 1280, "client_h": 720}),
        ("maximize", {}),
        ("restore", {}),
    ):
        pre_action_authority = _capture_fresh_process_authority(
            pid, launch_fingerprint, retained_witness=retained_witness,
        )
        if pre_action_authority.get("aliveExact") is True:
            result = control_war3_window(
                action=name,
                pid=pid,
                wait_sec=step_wait_sec,
                **kwargs,
            )
        else:
            result = {
                "ok": False,
                "skipped": True,
                "error": "launch process identity not exact before action",
            }
        record = {
            "step": name,
            "pid": pid,
            "result": result,
            "process": is_war3_running(pid=pid),
            "preActionProcessAuthority": pre_action_authority,
        }
        record["processAuthority"] = _capture_fresh_process_authority(
            pid, launch_fingerprint, record["process"],
            retained_witness=retained_witness,
        )
        record["ok"] = bool(
            result.get("ok")
            and result.get("aliveAfter")
            and record["processAuthority"].get("aliveExact") is True
        )
        _json_write(out_dir / f"lifecycle_window_{name}.json", record)
        steps.append(record)
    _json_write(out_dir / "lifecycle_window_matrix.json", steps)
    return steps


def _evaluate_lifecycle_gates(
    window_steps: List[Dict[str, Any]],
    resets: List[Dict[str, Any]],
    first_diag: Dict[str, Any],
    first_stop: Dict[str, Any],
    first_final_process: Dict[str, Any],
    second_launch: Dict[str, Any],
    second_ready: Dict[str, Any],
    second_alive: bool,
    second_screenshot: Dict[str, Any],
    second_diag: Dict[str, Any],
    second_crash_matches: List[str],
    second_stop: Dict[str, Any],
    second_final_process: Dict[str, Any],
    first_final_authority: Optional[Dict[str, Any]] = None,
    second_process_authority: Optional[Dict[str, Any]] = None,
    second_final_authority: Optional[Dict[str, Any]] = None,
    require_outline_all: bool = False,
    diagnostics: str = "full",
    execution_route: str = "compute",
    second_outline_control_applied: bool = True,
    second_outline_control_restored: bool = True,
) -> Dict[str, Any]:
    # 首个会话执行 reset；第二个会话只验证干净重启。
    # 第二个会话的 reset 计数可能从零开始，不用它授权 reset 契约。
    reset = first_diag["nativeReset"]
    retirement = first_diag["nativeRetirement"]
    reset_fields = (
        reset["requests"], reset["completions"], reset["acknowledgements"],
        reset["requestedGeneration"], reset["completedGeneration"], reset["acknowledgedGeneration"],
        reset["pendingRetirement"], reset["wrongThread"], reset["retirementFault"],
        retirement["overflow"], retirement["invalid"], retirement["pending"], retirement["fault"],
    )
    reset_diagnostics_present = all(value is not None for value in reset_fields)
    reset_lifecycle_clean = (
        reset_diagnostics_present
        and (reset["requests"] or 0) > 0
        and (reset["completions"] or 0) > 0
        and (reset["acknowledgements"] or 0) > 0
        and reset["requestedGeneration"] == reset["completedGeneration"] == reset["acknowledgedGeneration"]
        and reset["pendingRetirement"] == 0
        and reset["wrongThread"] == 0
        and reset["retirementFault"] == 0
        and _all_equal_zero((
            retirement["overflow"], retirement["invalid"],
            retirement["pending"], retirement["fault"],
        ))
    )
    second_base_gates = _evaluate_gates(
        second_diag, dict(second_process_authority or {}),
        [second_screenshot], second_crash_matches,
        require_outline_all=require_outline_all,
    )
    second_route_gates = _evaluate_vs_route_gates(
        second_diag, execution_route, second_launch,
    )
    second_base_gates.update(second_route_gates)
    second_gpu_skin_gate_names = tuple(
        name for name in _hard_gate_names(
            "base", diagnostics=diagnostics,
            execution_route=execution_route,
        ) if name != "twoScreenshots"
    ) + VS_ROUTE_HARD_GATE_NAMES
    if require_outline_all:
        second_gpu_skin_gate_names += (
            "outlineSubmittedPositive",
            "outlineSameSliceExact",
        )
    second_reset_pending = second_diag["nativeReset"]["pendingRetirement"]
    second_retirement_pending = second_diag["nativeRetirement"]["pending"]
    second_gpu_skin_clean = (
        (not require_outline_all or second_outline_control_applied)
        and all(
            bool(second_base_gates.get(name, False))
            for name in second_gpu_skin_gate_names
        )
        and second_reset_pending == 0
        and second_retirement_pending == 0
    )
    return {
        "lifecycleWindowMatrixClean": bool(window_steps) and all(step.get("ok") for step in window_steps),
        "lifecycleResetRequestsOk": len(resets) == 2 and all(bool(item["result"].get("ok")) for item in resets),
        "lifecycleResetProgressAlive": len(resets) == 2 and all(bool(item["aliveAfter"]) for item in resets),
        "lifecycleResetDiagnosticsPresent": reset_diagnostics_present,
        "lifecycleResetLifecycleClean": reset_lifecycle_clean,
        "lifecycleFirstStopClean": bool(
            first_stop.get("ok")
            and first_stop.get("cleanupCapabilityExact") is True
            and first_stop.get("pidTerminationCommandIssued") is False
            and dict(first_final_authority or {}).get(
                "terminatedExact"
            ) is True
        ),
        "lifecycleRelaunchReady": bool(
            second_launch.get("ok") and second_ready.get("ok")
            and dict(second_process_authority or {}).get(
                "aliveExact"
            ) is True
        ),
        "lifecycleRelaunchAlive": bool(
            dict(second_process_authority or {}).get("aliveExact") is True
        ),
        "lifecycleRelaunchScreenshot": bool(second_screenshot.get("ok")),
        "lifecycleRelaunchDiagnosticsPresent": second_base_gates["diagnosticsPresent"],
        "lifecycleRelaunchGpuSkinClean": second_gpu_skin_clean,
        "lifecycleRelaunchOutlineControlApplied": (
            not require_outline_all or second_outline_control_applied
        ),
        "lifecycleRelaunchOutlineControlRestored": (
            not require_outline_all or second_outline_control_restored
        ),
        "lifecycleRelaunchCrashScanClean": bool(
            second_base_gates.get("crashScanClean") is True
        ),
        "lifecycleSecondStopClean": bool(
            second_stop.get("ok")
            and second_stop.get("cleanupCapabilityExact") is True
            and second_stop.get("pidTerminationCommandIssued") is False
            and dict(second_final_authority or {}).get(
                "terminatedExact"
            ) is True
        ),
        "lifecycleResetObserved": {"nativeReset": reset, "nativeRetirement": retirement},
        "lifecycleRelaunchObserved": {
            "baseGates": second_base_gates,
            "routeGates": second_route_gates,
            "nativeResetPendingRetirement": second_reset_pending,
            "nativeRetirementPending": second_retirement_pending,
            "processAuthority": dict(second_process_authority or {}),
            "firstFinalAuthority": dict(first_final_authority or {}),
            "secondFinalAuthority": dict(second_final_authority or {}),
        },
    }


def _write_sha256s(out_dir: Path) -> None:
    rows = []
    for path in sorted(out_dir.iterdir()):
        if path.is_file() and path.name != "SHA256SUMS.txt":
            digest = _sha256(path)
            if digest:
                rows.append(f"{digest}  {path.name}")
    _text_write(out_dir / "SHA256SUMS.txt", "\n".join(rows) + ("\n" if rows else ""))


def _run_build_only(args: argparse.Namespace, out_dir: Path) -> int:
    _json_write(out_dir / "map_artifact.json", args.map_metadata)
    before = _dll_hashes()
    # 唯一 Test Conductor 固定使用 -j2，避免并发编译挤占 32 位宿主内存。
    command = [
        "ninja", "-C", "build32", "src/d3d9/d3d9.dll", "-j2",
    ]
    build = _run(command, timeout_sec=1800)
    _text_write(out_dir / "build_output.log", str(build.get("output", "")))
    result = {
        "test": "P4 build-only",
        "artifact": out_dir.name,
        "verdict": "PASS" if build.get("returncode") == 0 else "FAIL",
        "build": build,
        "dllBefore": before,
        "dllAfter": _dll_hashes(),
        "environment": _p4_environment(
            args.diagnostics, args.sidecar_policy, args.execution_route,
        ),
        "requestedPoisonSidecarPolicy": args.sidecar_policy,
        "requestedExecutionRoute": args.execution_route,
        "git": _git_summary(),
        "map": str(args.map_path),
        "mapArtifact": args.map_metadata,
        "caseLabel": args.case_label,
        "caseLabelSafe": args.case_label_safe,
        "isolatedDesktop": True,
        "launchPerformed": False,
        "deployPerformed": False,
    }
    _json_write(out_dir / "p4_result.json", result)
    _text_write(
        out_dir / "test_description.txt",
        "P4 仅构建；未部署、未启动《魔兽争霸 III》、未运行 AutoTest。\n"
        f"mapResolved={args.map_metadata['resolvedPath']}\n"
        f"mapSize={args.map_metadata['size']}\n"
        f"mapSha256={args.map_metadata['sha256']}\n"
        f"caseLabel={args.case_label or ''}\n"
        f"requestedPoisonSidecarPolicy={args.sidecar_policy}\n"
        f"requestedExecutionRoute={args.execution_route}\n",
    )
    _write_sha256s(out_dir)
    print(f"P4 build-only {result['verdict']}: {out_dir}", flush=True)
    return 0 if result["verdict"] == "PASS" else 1


def _run_runtime_phase(args: argparse.Namespace, out_dir: Path) -> int:
    _json_write(out_dir / "map_artifact.json", args.map_metadata)
    offsets = _snapshot_log_offsets()
    _json_write(out_dir / "prelaunch_log_state.json", offsets)
    _json_write(out_dir / "git_diff_summary.json", _git_summary())
    hashes_before = _dll_hashes()
    _json_write(out_dir / "deployment_hashes.json", {"before": hashes_before, "deployRequested": args.deploy})
    outline_shader_hashes = _outline_shader_hashes()
    _json_write(out_dir / "outline_shader_hashes.json", outline_shader_hashes)
    env = _p4_environment(
        args.diagnostics, args.sidecar_policy, args.execution_route,
    )
    lifecycle = args.phase == "lifecycle"
    first_desktop = f"War3GpuSkinP4_{args.phase.replace('-', '')}_first_{_now()}"
    second_desktop = f"War3GpuSkinP4_{args.phase.replace('-', '')}_second_{_now()}"
    launch: Dict[str, Any] = {}
    ready: Dict[str, Any] = {}
    stop: Dict[str, Any] = {}
    first_final_process: Dict[str, Any] = {}
    second_launch: Dict[str, Any] = {}
    first_launch_fingerprint: Dict[str, Any] = {}
    second_launch_fingerprint: Dict[str, Any] = {}
    first_cleanup_capability: Dict[str, Any] = {}
    second_cleanup_capability: Dict[str, Any] = {}
    first_retained_witness: Dict[str, Any] = {}
    second_retained_witness: Dict[str, Any] = {}
    first_retained_witness_close: Dict[str, Any] = {}
    second_retained_witness_close: Dict[str, Any] = {}
    second_ready: Dict[str, Any] = {}
    second_stop: Dict[str, Any] = {}
    second_final_process: Dict[str, Any] = {}
    pid = 0
    second_pid = 0
    runtime_polls: List[Dict[str, Any]] = []
    screenshots: List[Dict[str, Any]] = []
    second_runtime_polls: List[Dict[str, Any]] = []
    second_screenshot: Dict[str, Any] = {}
    debug: Dict[str, Any] = {}
    log_copies: Dict[str, Any] = {}
    second_debug: Dict[str, Any] = {}
    second_log_copies: Dict[str, Any] = {}
    crash_matches: List[str] = []
    second_crash_matches: List[str] = []
    alive_before_stop = False
    second_alive_before_stop = False
    window_steps: List[Dict[str, Any]] = []
    reset_steps: List[Dict[str, Any]] = []
    first_outline_control: Dict[str, Any] = {}
    second_outline_control: Dict[str, Any] = {}
    first_outline_restore: Dict[str, Any] = {}
    second_outline_restore: Dict[str, Any] = {}
    ready_failure_evidence: Dict[str, Any] = {}
    second_ready_failure_evidence: Dict[str, Any] = {}
    runtime_failure_evidence: Dict[str, Any] = {}
    second_runtime_failure_evidence: Dict[str, Any] = {}
    first_pre_evidence_process_snapshot: Dict[str, Any] = {}
    second_pre_evidence_process_snapshot: Dict[str, Any] = {}
    first_process_authority: Dict[str, Any] = {}
    second_process_authority: Dict[str, Any] = {}
    first_final_process_authority: Dict[str, Any] = {}
    second_final_process_authority: Dict[str, Any] = {}
    first_intentional_stop_started = False
    second_intentional_stop_started = False
    first_runtime_death_observed = False
    second_runtime_death_observed = False
    # launch_war3_test clears STATE.debug_events and resets debug_seq to zero
    # before every process start. Cursor zero is therefore a fresh session
    # boundary, not an unbounded historical query; exact PID remains mandatory.
    event_since_id = 0
    launch_epoch_ms = 0
    second_launch_epoch_ms = 0
    error: Optional[str] = None
    infrastructure_failure_reason = ""
    diag = _parse_gpu_skin_diag(
        "", {}, requested_sidecar_policy=args.sidecar_policy,
    )
    second_diag = _parse_gpu_skin_diag(
        "", {}, requested_sidecar_policy=args.sidecar_policy,
    )
    gates: Dict[str, Any] = {}

    try:
        launch_epoch_ms = int(time.time() * 1000)
        launch = _launch_war3_with_execution_route_isolation(
            execution_route=args.execution_route,
            war3_dir=str(WAR3_DIR), map_path=str(args.map_path), windowed=lifecycle,
            use_isolated_desktop=True, desktop_name=first_desktop,
            auto_perf_record=False, deploy_d3d9_before_launch=bool(args.deploy),
            enforce_video_baseline=True, env_overrides_json=json.dumps(env),
            disable_modules=args.disable_modules,
        )
        launched_pid = _coerce_pid(
            launch.get("pid") or launch.get("gamePid")
        )
        if launched_pid is not None and launched_pid > 0:
            pid = launched_pid
        if launch.get("ok") is True and pid > 0:
            first_cleanup_capability = (
                _freeze_launch_cleanup_capability(
                    pid, launch, "first",
                )
            )
        _json_write(out_dir / "launch_result.json", launch)
        if not launch.get("ok"):
            error = f"launch failed: {launch.get('error', launch)}"
            infrastructure_failure_reason = error
            raise RuntimeError(error)
        event_since_id = 0
        _json_write(
            out_dir / "first_event_cursor.json",
            {"sinceId": 0, "source": "launch_war3_test DBWIN reset"},
        )
        if pid <= 0:
            error = "launch succeeded without a valid target PID"
            infrastructure_failure_reason = error
            raise RuntimeError(error)
        if first_cleanup_capability.get("available") is not True:
            infrastructure_failure_reason = (
                "first launch cleanup capability unavailable"
            )
            error = infrastructure_failure_reason
            raise RuntimeError(error)
        _json_write(
            out_dir / "first_launch_cleanup_capability.json",
            first_cleanup_capability,
        )
        # Freeze the exact direct-launch process identity before any ready
        # wait. STATE is mutable and will be overwritten by lifecycle relaunch.
        first_launch_fingerprint = _freeze_launch_instance_fingerprint(
            pid, launch, launch_epoch_ms,
        )
        first_retained_witness = _capture_retained_launch_handle_witness(
            first_launch_fingerprint, pid,
        )
        _json_write(
            out_dir / "first_launch_instance_fingerprint.json",
            first_launch_fingerprint,
        )
        _json_write(
            out_dir / "first_retained_native_process_witness.json",
            _retained_launch_handle_witness_report(
                first_retained_witness
            ),
        )
        if (
            first_launch_fingerprint.get("available") is not True
            or first_retained_witness.get("shapeExact") is not True
        ):
            infrastructure_failure_reason = (
                "first launch retained native process witness unavailable"
            )
            error = infrastructure_failure_reason
            raise RuntimeError(error)
        ready = _wait_for_game_ready_with_retained_handle(
            timeout_sec=args.ready_timeout_sec,
            pid=pid,
            launch_fingerprint=first_launch_fingerprint,
            retained_witness=first_retained_witness,
        )
        _json_write(out_dir / "ready_result.json", ready)
        if not ready.get("ok"):
            ready_failure_evidence = _collect_ready_failure_evidence(
                out_dir=out_dir,
                tag="ready_failure_first",
                pid=pid,
                desktop_name=first_desktop,
                ready=ready,
                log_offsets=offsets,
                launch_epoch_ms=launch_epoch_ms,
                event_since_id=event_since_id,
                launch_fingerprint=first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            first_process_authority = dict(
                ready_failure_evidence.get("classification", {}).get(
                    "processLivenessAuthority", {}
                ) or {}
            )
            alive_before_stop = bool(
                first_process_authority.get("aliveExact") is True
            )
            screenshot_attempt = dict(ready_failure_evidence.get("isolatedScreenshot", {}) or {})
            if screenshot_attempt:
                screenshots.append(screenshot_attempt)
            debug = dict(ready_failure_evidence.get("debug", {}) or {})
            log_copies = dict(
                ready_failure_evidence.get("incrementalLogs", {}).get("files", {}) or {}
            )
            crash_matches = list(ready_failure_evidence.get("crashMatches", []) or [])
            diag = dict(ready_failure_evidence.get("diagnostics", {}) or diag)
            error = f"ready failed: {ready.get('error', ready)}"
            raise RuntimeError(error)

        # Capture the loaded module baseline immediately after the exact-PID
        # control plane reports ready. Later actuation, screenshots, or
        # diagnostic collection may be the operation that kills the process.
        first_pre_evidence_process_snapshot = _best_effort(
            "first_pre_evidence_process_snapshot",
            lambda: _capture_process_snapshot(
                pid, launch_epoch_ms, first_launch_fingerprint,
            ),
        )
        _json_write(
            out_dir / "first_pre_evidence_process_snapshot.json",
            first_pre_evidence_process_snapshot,
        )

        if args.outline_all:
            first_outline_control = _set_outline_all_for_test(
                out_dir, "first", pid, enabled=True,
                launch_fingerprint=first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            if not first_outline_control.get("applied"):
                error = "outline-all internal test actuation failed for first process"
                raise RuntimeError(error)

        if lifecycle:
            stable_polls = _poll_runtime_steps(
                pid, count=3,
                wait_sec=min(2.0, float(args.poll_interval_sec)),
                launch_fingerprint=first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            runtime_polls.extend(stable_polls)
            _json_write(out_dir / "lifecycle_first_stability_polls.json", stable_polls)
            if not stable_polls or not stable_polls[-1].get("running"):
                stable_authority = dict(
                    stable_polls[-1].get("processAuthority", {}) or {}
                ) if stable_polls else {}
                if stable_authority.get("terminatedExact") is True:
                    first_runtime_death_observed = True
                    error = "exact launch instance exited before lifecycle matrix"
                else:
                    infrastructure_failure_reason = (
                        "first launch process identity indeterminate before "
                        "lifecycle matrix"
                    )
                    error = infrastructure_failure_reason
            else:
                window_steps = _run_lifecycle_window_matrix(
                    out_dir, pid, step_wait_sec=1.0,
                    launch_fingerprint=first_launch_fingerprint,
                    retained_witness=first_retained_witness,
                )
                for scope in ("map", "device"):
                    reset_steps.append(_lifecycle_reset(
                        out_dir, pid, scope,
                        poll_wait_sec=min(
                            1.0, float(args.poll_interval_sec),
                        ),
                        launch_fingerprint=first_launch_fingerprint,
                        retained_witness=first_retained_witness,
                    ))
                _json_write(out_dir / "lifecycle_reset_matrix.json", reset_steps)

        start = time.monotonic()
        while time.monotonic() - start < args.duration_sec:
            elapsed = time.monotonic() - start
            poll = _runtime_poll(
                pid, elapsed, first_launch_fingerprint,
                first_retained_witness,
            )
            runtime_polls.append(poll)
            if not poll["running"]:
                if poll.get("terminatedExact") is True:
                    first_runtime_death_observed = True
                    error = "War3 exact launch instance exited during runtime polling"
                else:
                    infrastructure_failure_reason = (
                        "first launch process identity became indeterminate "
                        "during runtime polling"
                    )
                    error = infrastructure_failure_reason
                break
            time.sleep(max(1, args.poll_interval_sec))
        _json_write(out_dir / "runtime_polls.json", runtime_polls)

        first_process_authority = _capture_fresh_process_authority(
            pid, first_launch_fingerprint,
            retained_witness=first_retained_witness,
        )
        if first_process_authority.get("aliveExact") is True:
            first = capture_war3_screenshot(
                output_path=str(out_dir / "war3_p4_a.png"), pid=pid, war3_dir=str(WAR3_DIR), timeout_sec=12
            )
            screenshots.append(first)
            time.sleep(args.screenshot_gap_sec)
            second_capture_authority = _capture_fresh_process_authority(
                pid, first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            if second_capture_authority.get("aliveExact") is True:
                second = capture_war3_screenshot(
                    output_path=str(out_dir / "war3_p4_b.png"), pid=pid, war3_dir=str(WAR3_DIR), timeout_sec=12
                )
                screenshots.append(second)
            else:
                screenshots.append({
                    "ok": False,
                    "pid": pid,
                    "error": (
                        "second screenshot blocked: immutable launch "
                        "fingerprint no longer exact"
                    ),
                    "processAuthority": second_capture_authority,
                })
        _json_write(out_dir / "screenshot_results.json", screenshots)

        first_process_authority = _capture_fresh_process_authority(
            pid, first_launch_fingerprint,
            retained_witness=first_retained_witness,
        )
        alive_before_stop = (
            first_process_authority.get("aliveExact") is True
        )
        if alive_before_stop and not first_pre_evidence_process_snapshot:
            first_pre_evidence_process_snapshot = _best_effort(
                "first_pre_evidence_process_snapshot",
                lambda: _capture_process_snapshot(
                    pid, launch_epoch_ms, first_launch_fingerprint,
                ),
            )
            _json_write(
                out_dir / "first_pre_evidence_process_snapshot.json",
                first_pre_evidence_process_snapshot,
            )
        runtime_data = runtime_polls[-1]["data"] if runtime_polls else {}
        debug, _, log_copies, crash_matches, diag, event_since_id = _collect_session_evidence(
            out_dir, "first", pid, offsets, event_since_id, runtime_data,
            args.sidecar_policy, first_launch_fingerprint,
            first_retained_witness,
        )
        first_process_authority = _capture_fresh_process_authority(
            pid, first_launch_fingerprint,
            retained_witness=first_retained_witness,
        )
        alive_before_stop = (
            first_process_authority.get("aliveExact") is True
        )
        if (
            ready.get("ok")
            and first_process_authority.get("terminatedExact") is True
        ):
            first_runtime_death_observed = True
            runtime_failure_evidence = _collect_ready_failure_evidence(
                out_dir=out_dir,
                tag="runtime_failure_first",
                pid=pid,
                desktop_name=first_desktop,
                ready=ready,
                log_offsets=offsets,
                launch_epoch_ms=launch_epoch_ms,
                # launch_war3_test resets the DBWIN stream for this exact
                # process. Start at zero so a death during evidence capture
                # cannot discard earlier target-PID failure lines.
                event_since_id=0,
                launch_fingerprint=first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            for line in list(
                runtime_failure_evidence.get("crashMatches", []) or []
            ):
                if line not in crash_matches:
                    crash_matches.append(line)
        elif first_process_authority.get("indeterminate") is True:
            infrastructure_failure_reason = (
                infrastructure_failure_reason
                or "first launch process identity indeterminate at evidence gate"
            )
        _json_write(
            out_dir / "first_process_at_evidence_gate.json",
            {
                "pid": pid,
                "running": alive_before_stop,
                "processAuthority": first_process_authority,
            },
        )
        _json_write(out_dir / "gpu_skin_diag.json", diag)
        gates = _evaluate_gates(
            diag,
            first_process_authority,
            screenshots,
            crash_matches,
            require_outline_all=bool(args.outline_all),
        )
        gates.update(_evaluate_vs_route_gates(
            diag, args.execution_route, launch,
        ))

        if args.outline_all and first_outline_control.get("applied"):
            first_outline_restore = _restore_outline_all_for_test(
                out_dir, "first", pid, first_launch_fingerprint,
                first_retained_witness,
            )

        if lifecycle:
            first_pre_stop_state = is_war3_running(pid=pid)
            first_pre_stop_authority = _capture_fresh_process_authority(
                pid, first_launch_fingerprint, first_pre_stop_state,
                retained_witness=first_retained_witness,
            )
            if first_pre_stop_authority.get("aliveExact") is not True:
                if (
                    first_pre_stop_authority.get("terminatedExact") is True
                    and not runtime_failure_evidence
                ):
                    first_runtime_death_observed = True
                    runtime_failure_evidence = _collect_ready_failure_evidence(
                        out_dir=out_dir,
                        tag="runtime_failure_first",
                        pid=pid,
                        desktop_name=first_desktop,
                        ready=ready,
                        log_offsets=offsets,
                        launch_epoch_ms=launch_epoch_ms,
                        event_since_id=0,
                        launch_fingerprint=first_launch_fingerprint,
                        retained_witness=first_retained_witness,
                    )
                    for line in list(
                        runtime_failure_evidence.get(
                            "crashMatches", []
                        ) or []
                    ):
                        if line not in crash_matches:
                            crash_matches.append(line)
                if first_pre_stop_authority.get("terminatedExact") is True:
                    error = error or (
                        "first exact launch instance exited before "
                        "intentional lifecycle stop"
                    )
                else:
                    infrastructure_failure_reason = (
                        "first launch process identity indeterminate before "
                        "lifecycle stop"
                    )
                    error = error or infrastructure_failure_reason
                raise RuntimeError(error)
            first_intentional_stop_started = True
            stop = _exact_handle_cleanup_stop(
                pid,
                first_cleanup_capability,
                first_retained_witness,
                mode="first-lifecycle-exact-native-handle",
            )
            _json_write(out_dir / "first_stop_result.json", stop)
            first_final_process = dict(
                stop.get("finalProcess", {}) or {}
            )
            first_final_process_authority = dict(
                stop.get("finalProcessAuthority", {}) or {}
            )
            _json_write(out_dir / "first_final_process_check.json", first_final_process)
            if (
                not stop.get("ok")
                or first_final_process_authority.get(
                    "terminatedExact"
                ) is not True
            ):
                infrastructure_failure_reason = (
                    "first lifecycle process cleanup was not confirmed; "
                    "second launch blocked"
                )
                error = error or infrastructure_failure_reason
                raise RuntimeError(error)

            second_offsets = _snapshot_log_offsets()
            _json_write(out_dir / "second_prelaunch_log_state.json", second_offsets)
            event_since_id = 0
            second_launch_epoch_ms = int(time.time() * 1000)
            second_launch = _launch_war3_with_execution_route_isolation(
                execution_route=args.execution_route,
                war3_dir=str(WAR3_DIR), map_path=str(args.map_path), windowed=True,
                use_isolated_desktop=True, desktop_name=second_desktop,
                auto_perf_record=False, deploy_d3d9_before_launch=False,
                enforce_video_baseline=True, env_overrides_json=json.dumps(env),
                disable_modules=args.disable_modules,
            )
            launched_second_pid = _coerce_pid(
                second_launch.get("pid") or second_launch.get("gamePid")
            )
            if launched_second_pid is not None and launched_second_pid > 0:
                second_pid = launched_second_pid
            if second_launch.get("ok") is True and second_pid > 0:
                second_cleanup_capability = (
                    _freeze_launch_cleanup_capability(
                        second_pid, second_launch, "second",
                    )
                )
            _json_write(out_dir / "second_launch_result.json", second_launch)
            if not second_launch.get("ok"):
                infrastructure_failure_reason = (
                    "second lifecycle launch failed: "
                    f"{second_launch.get('error', second_launch)}"
                )
                error = error or infrastructure_failure_reason
                raise RuntimeError(error)
            if second_launch.get("ok"):
                event_since_id = 0
                _json_write(
                    out_dir / "second_event_cursor.json",
                    {"sinceId": 0, "source": "launch_war3_test DBWIN reset"},
                )
            if second_launch.get("ok"):
                if second_pid <= 0:
                    infrastructure_failure_reason = (
                        "second lifecycle launch succeeded without a valid "
                        "target PID"
                    )
                    error = error or infrastructure_failure_reason
                    raise RuntimeError(error)
                if (
                    second_cleanup_capability.get("available")
                    is not True
                ):
                    infrastructure_failure_reason = (
                        "second launch cleanup capability unavailable"
                    )
                    error = error or infrastructure_failure_reason
                    raise RuntimeError(error)
                _json_write(
                    out_dir / "second_launch_cleanup_capability.json",
                    second_cleanup_capability,
                )
                second_launch_fingerprint = (
                    _freeze_launch_instance_fingerprint(
                        second_pid, second_launch, second_launch_epoch_ms,
                    )
                )
                second_retained_witness = (
                    _capture_retained_launch_handle_witness(
                        second_launch_fingerprint, second_pid,
                    )
                )
                _json_write(
                    out_dir / "second_launch_instance_fingerprint.json",
                    second_launch_fingerprint,
                )
                _json_write(
                    out_dir / "second_retained_native_process_witness.json",
                    _retained_launch_handle_witness_report(
                        second_retained_witness
                    ),
                )
                if (
                    second_launch_fingerprint.get("available") is not True
                    or second_retained_witness.get("shapeExact") is not True
                ):
                    infrastructure_failure_reason = (
                        "second launch retained native process witness "
                        "unavailable"
                    )
                    error = error or infrastructure_failure_reason
                    raise RuntimeError(error)
                second_ready = _wait_for_game_ready_with_retained_handle(
                    timeout_sec=args.ready_timeout_sec,
                    pid=second_pid,
                    launch_fingerprint=second_launch_fingerprint,
                    retained_witness=second_retained_witness,
                )
                _json_write(out_dir / "second_ready_result.json", second_ready)
                if second_ready.get("ok"):
                    second_pre_evidence_process_snapshot = _best_effort(
                        "second_pre_evidence_process_snapshot",
                        lambda: _capture_process_snapshot(
                            second_pid, second_launch_epoch_ms,
                            second_launch_fingerprint,
                        ),
                    )
                    _json_write(
                        out_dir / "second_pre_evidence_process_snapshot.json",
                        second_pre_evidence_process_snapshot,
                    )
                    if args.outline_all:
                        second_outline_control = _set_outline_all_for_test(
                            out_dir, "second", second_pid, enabled=True,
                            launch_fingerprint=second_launch_fingerprint,
                            retained_witness=second_retained_witness,
                        )
                        if not second_outline_control.get("applied"):
                            error = (
                                "outline-all internal test actuation failed "
                                "for second process"
                            )
                    if not args.outline_all or second_outline_control.get("applied"):
                        second_runtime_polls = _poll_runtime_until_gpu_skin_gate(
                            second_pid,
                            second_offsets,
                            event_since_id,
                            timeout_sec=30.0,
                            wait_sec=min(2.0, float(args.poll_interval_sec)),
                            launch_fingerprint=second_launch_fingerprint,
                            retained_witness=second_retained_witness,
                        )
                        _json_write(out_dir / "second_smoke_polls.json", second_runtime_polls)
                        second_process_authority = (
                            dict(second_runtime_polls[-1].get(
                                "processAuthority", {}
                            ) or {}) if second_runtime_polls else {}
                        )
                        second_alive_before_stop = bool(
                            second_process_authority.get("aliveExact") is True
                        )
                        if second_alive_before_stop:
                            second_process_authority = (
                                _capture_fresh_process_authority(
                                    second_pid, second_launch_fingerprint,
                                    retained_witness=(
                                        second_retained_witness
                                    ),
                                )
                            )
                            second_alive_before_stop = bool(
                                second_process_authority.get(
                                    "aliveExact"
                                ) is True
                            )
                        if second_alive_before_stop:
                            second_screenshot = capture_war3_screenshot(
                                output_path=str(out_dir / "war3_p4_second_smoke.png"), pid=second_pid,
                                war3_dir=str(WAR3_DIR), timeout_sec=12,
                            )
                        else:
                            second_screenshot = {
                                "ok": False,
                                "error": (
                                    "second smoke screenshot blocked: exact "
                                    "launch process not currently proven alive"
                                ),
                                "pid": second_pid,
                                "processAuthority": second_process_authority,
                            }
                    else:
                        second_screenshot = {
                            "ok": False,
                            "error": "outline-all actuation failed before second smoke",
                            "pid": second_pid,
                        }
                else:
                    second_ready_failure_evidence = _collect_ready_failure_evidence(
                        out_dir=out_dir,
                        tag="ready_failure_second",
                        pid=second_pid,
                        desktop_name=second_desktop,
                        ready=second_ready,
                        log_offsets=second_offsets,
                        launch_epoch_ms=second_launch_epoch_ms,
                        event_since_id=event_since_id,
                        launch_fingerprint=second_launch_fingerprint,
                        retained_witness=second_retained_witness,
                    )
                    second_process_authority = dict(
                        second_ready_failure_evidence.get(
                            "classification", {}
                        ).get("processLivenessAuthority", {}) or {}
                    )
                    second_screenshot = dict(
                        second_ready_failure_evidence.get("isolatedScreenshot", {}) or
                        {"ok": False, "error": "second launch was not ready", "pid": second_pid}
                    )
            else:
                second_screenshot = {"ok": False, "error": "second launch failed"}
            _json_write(out_dir / "second_screenshot_result.json", second_screenshot)

            if second_pid:
                second_process_authority = (
                    _capture_fresh_process_authority(
                        second_pid, second_launch_fingerprint,
                        retained_witness=second_retained_witness,
                    )
                )
            if (
                second_process_authority.get("aliveExact") is True
                and not second_pre_evidence_process_snapshot
            ):
                second_pre_evidence_process_snapshot = _best_effort(
                    "second_pre_evidence_process_snapshot",
                    lambda: _capture_process_snapshot(
                        second_pid, second_launch_epoch_ms,
                        second_launch_fingerprint,
                    ),
                )
                _json_write(
                    out_dir / "second_pre_evidence_process_snapshot.json",
                    second_pre_evidence_process_snapshot,
                )
            if second_ready.get("ok"):
                second_runtime_data = (
                    second_runtime_polls[-1]["data"]
                    if second_runtime_polls else {}
                )
                (
                    second_debug, _, second_log_copies,
                    second_crash_matches, second_diag, event_since_id,
                ) = _collect_session_evidence(
                    out_dir, "second", second_pid, second_offsets,
                    event_since_id, second_runtime_data,
                    args.sidecar_policy, second_launch_fingerprint,
                    second_retained_witness,
                )
                second_process_authority = _capture_fresh_process_authority(
                    second_pid, second_launch_fingerprint,
                    retained_witness=second_retained_witness,
                )
                second_alive_before_stop = bool(
                    second_process_authority.get("aliveExact") is True
                )
                if second_process_authority.get("terminatedExact") is True:
                    second_runtime_death_observed = True
                    second_runtime_failure_evidence = (
                        _collect_ready_failure_evidence(
                            out_dir=out_dir,
                            tag="runtime_failure_second",
                            pid=second_pid,
                            desktop_name=second_desktop,
                            ready=second_ready,
                            log_offsets=second_offsets,
                            launch_epoch_ms=second_launch_epoch_ms,
                            event_since_id=0,
                            launch_fingerprint=second_launch_fingerprint,
                            retained_witness=second_retained_witness,
                        )
                    )
                    for line in list(
                        second_runtime_failure_evidence.get(
                            "crashMatches", []
                        ) or []
                    ):
                        if line not in second_crash_matches:
                            second_crash_matches.append(line)
                elif second_process_authority.get("indeterminate") is True:
                    infrastructure_failure_reason = (
                        infrastructure_failure_reason
                        or "second launch process identity indeterminate at evidence gate"
                    )
            else:
                # A non-ready process must not receive forced GPU-skin
                # diagnostics. Preserve the ready-failure evidence as-is and
                # proceed directly to bounded cleanup.
                second_debug = dict(
                    second_ready_failure_evidence.get("debug", {}) or {}
                )
                second_log_copies = dict(
                    second_ready_failure_evidence.get(
                        "incrementalLogs", {}
                    ).get("files", {}) or {}
                )
                second_crash_matches = list(
                    second_ready_failure_evidence.get(
                        "crashMatches", []
                    ) or []
                )
                second_diag = dict(
                    second_ready_failure_evidence.get(
                        "diagnostics", {}
                    ) or second_diag
                )
                second_process_authority = dict(
                    second_ready_failure_evidence.get(
                        "classification", {}
                    ).get("processLivenessAuthority", {}) or {}
                )
                second_alive_before_stop = bool(
                    second_process_authority.get("aliveExact") is True
                )
            if (
                args.outline_all
                and second_outline_control.get("applied")
                and second_alive_before_stop
            ):
                second_outline_restore = _restore_outline_all_for_test(
                    out_dir, "second", second_pid,
                    second_launch_fingerprint,
                    second_retained_witness,
                )
            _json_write(
                out_dir / "second_process_at_evidence_gate.json",
                {
                    "pid": second_pid,
                    "running": second_alive_before_stop,
                    "processAuthority": second_process_authority,
                },
            )
            if second_pid:
                second_pre_stop_state = is_war3_running(pid=second_pid)
                second_pre_stop_authority = (
                    _capture_fresh_process_authority(
                        second_pid, second_launch_fingerprint,
                        second_pre_stop_state,
                        retained_witness=second_retained_witness,
                    )
                )
                if second_pre_stop_authority.get("aliveExact") is not True:
                    second_was_alive_at_ready_failure = bool(
                        second_ready_failure_evidence.get(
                            "classification", {}
                        ).get("processAliveAtEvidenceCapture")
                    )
                    if (
                        second_pre_stop_authority.get(
                            "terminatedExact"
                        ) is True
                        and
                        not second_runtime_failure_evidence
                        and (
                            second_ready.get("ok")
                            or second_was_alive_at_ready_failure
                        )
                    ):
                        second_runtime_failure_evidence = (
                            _collect_ready_failure_evidence(
                                out_dir=out_dir,
                                tag="runtime_failure_second",
                                pid=second_pid,
                                desktop_name=second_desktop,
                                ready=second_ready,
                                log_offsets=second_offsets,
                                launch_epoch_ms=second_launch_epoch_ms,
                                event_since_id=0,
                                launch_fingerprint=(
                                    second_launch_fingerprint
                                ),
                                retained_witness=(
                                    second_retained_witness
                                ),
                            )
                        )
                        for line in list(
                            second_runtime_failure_evidence.get(
                                "crashMatches", []
                            ) or []
                        ):
                            if line not in second_crash_matches:
                                second_crash_matches.append(line)
                    if second_pre_stop_authority.get(
                        "terminatedExact"
                    ) is True:
                        second_runtime_death_observed = True
                        error = error or (
                            "second exact launch instance exited before "
                            "intentional lifecycle stop"
                        )
                    else:
                        infrastructure_failure_reason = (
                            "second launch process identity indeterminate "
                            "before lifecycle stop"
                        )
                        error = error or infrastructure_failure_reason
                    raise RuntimeError(error)
                second_intentional_stop_started = True
                second_stop = (
                    _stop_after_ready_failure(
                        second_pid, second_launch_fingerprint,
                        second_retained_witness,
                        second_cleanup_capability,
                    )
                    if second_ready_failure_evidence
                    else _exact_handle_cleanup_stop(
                        second_pid,
                        second_cleanup_capability,
                        second_retained_witness,
                        mode="second-lifecycle-exact-native-handle",
                    )
                )
                _json_write(out_dir / "second_stop_result.json", second_stop)
                second_final_process = dict(
                    second_stop.get("finalProcess", {}) or {}
                )
                second_final_process_authority = dict(
                    second_stop.get(
                        "finalProcessAuthority", {},
                    ) or {}
                )
                if (
                    not second_stop.get("ok")
                    or second_final_process_authority.get(
                        "terminatedExact"
                    ) is not True
                ):
                    infrastructure_failure_reason = (
                        "second lifecycle process cleanup was not confirmed"
                    )
                    error = error or infrastructure_failure_reason
            else:
                second_final_process = {"ok": True, "running": False, "pid": 0}
            _json_write(out_dir / "second_final_process_check.json", second_final_process)
            gates.update(_evaluate_lifecycle_gates(
                window_steps, reset_steps, diag, stop, first_final_process, second_launch, second_ready,
                second_alive_before_stop, second_screenshot, second_diag, second_crash_matches,
                second_stop, second_final_process,
                first_final_authority=first_final_process_authority,
                second_process_authority=second_process_authority,
                second_final_authority=second_final_process_authority,
                require_outline_all=bool(args.outline_all),
                diagnostics=args.diagnostics,
                execution_route=args.execution_route,
                second_outline_control_applied=bool(
                    second_outline_control.get("applied")
                ),
                second_outline_control_restored=bool(
                    second_outline_restore.get("restored")
                ),
            ))
    except Exception as exc:  # Preserve the artifact even when launch/ready itself fails.
        error = error or f"{type(exc).__name__}: {exc}"
        gates = {"harnessException": error, "processAlive": False}
    finally:
        # PID presence/absence is report-only. Every hard alive/death decision
        # below consumes the immutable launch fingerprint, with a retained
        # exact launch handle as the only permitted post-exit proof.
        first_cleanup_state = (
            is_war3_running(pid=pid)
            if pid else {"ok": True, "running": False, "pid": 0}
        )
        first_cleanup_authority = (
            _capture_fresh_process_authority(
                pid, first_launch_fingerprint, first_cleanup_state,
                retained_witness=first_retained_witness,
            ) if pid else {}
        )
        if pid and not first_intentional_stop_started:
            first_process_authority = first_cleanup_authority
            alive_before_stop = (
                first_cleanup_authority.get("aliveExact") is True
            )
        first_was_alive_at_ready_failure = bool(
            ready_failure_evidence.get("classification", {}).get(
                "processAliveAtEvidenceCapture"
            ) is True
        )
        first_dead_before_cleanup = bool(
            pid
            and not first_intentional_stop_started
            and (ready.get("ok") or first_was_alive_at_ready_failure)
            and first_cleanup_authority.get("terminatedExact") is True
        )
        if first_dead_before_cleanup:
            first_runtime_death_observed = True
        if first_dead_before_cleanup and not runtime_failure_evidence:
            runtime_failure_evidence = _best_effort(
                "runtime_failure_first",
                lambda: _collect_ready_failure_evidence(
                    out_dir=out_dir,
                    tag="runtime_failure_first",
                    pid=pid,
                    desktop_name=first_desktop,
                    ready=ready,
                    log_offsets=offsets,
                    launch_epoch_ms=launch_epoch_ms,
                    event_since_id=0,
                    launch_fingerprint=first_launch_fingerprint,
                    retained_witness=first_retained_witness,
                ),
            )
            for line in list(
                runtime_failure_evidence.get("crashMatches", []) or []
            ):
                if line not in crash_matches:
                    crash_matches.append(line)
        elif (
            pid and not first_intentional_stop_started
            and (ready.get("ok") or first_was_alive_at_ready_failure)
            and first_cleanup_authority.get("indeterminate") is True
        ):
            infrastructure_failure_reason = (
                infrastructure_failure_reason
                or "first launch process identity indeterminate during cleanup"
            )

        second_cleanup_state = (
            is_war3_running(pid=second_pid)
            if second_pid else {"ok": True, "running": False, "pid": 0}
        )
        second_cleanup_authority = (
            _capture_fresh_process_authority(
                second_pid, second_launch_fingerprint,
                second_cleanup_state,
                retained_witness=second_retained_witness,
            ) if second_pid else {}
        )
        if second_pid and not second_intentional_stop_started:
            second_process_authority = second_cleanup_authority
            second_alive_before_stop = (
                second_cleanup_authority.get("aliveExact") is True
            )
        second_was_alive_at_ready_failure = bool(
            second_ready_failure_evidence.get(
                "classification", {}
            ).get("processAliveAtEvidenceCapture") is True
        )
        second_dead_before_cleanup = bool(
            second_pid
            and not second_intentional_stop_started
            and (
                second_ready.get("ok")
                or second_was_alive_at_ready_failure
            )
            and second_cleanup_authority.get("terminatedExact") is True
        )
        if second_dead_before_cleanup:
            second_runtime_death_observed = True
        if second_dead_before_cleanup and not second_runtime_failure_evidence:
            second_runtime_failure_evidence = _best_effort(
                "runtime_failure_second",
                lambda: _collect_ready_failure_evidence(
                    out_dir=out_dir,
                    tag="runtime_failure_second",
                    pid=second_pid,
                    desktop_name=second_desktop,
                    ready=second_ready,
                    log_offsets=second_offsets,
                    launch_epoch_ms=second_launch_epoch_ms,
                    event_since_id=0,
                    launch_fingerprint=second_launch_fingerprint,
                    retained_witness=second_retained_witness,
                ),
            )
            for line in list(
                second_runtime_failure_evidence.get(
                    "crashMatches", []
                ) or []
            ):
                if line not in second_crash_matches:
                    second_crash_matches.append(line)
        elif (
            second_pid and not second_intentional_stop_started
            and (
                second_ready.get("ok")
                or second_was_alive_at_ready_failure
            )
            and second_cleanup_authority.get("indeterminate") is True
        ):
            infrastructure_failure_reason = (
                infrastructure_failure_reason
                or "second launch process identity indeterminate during cleanup"
            )

        if (
            args.outline_all
            and second_pid
            and second_outline_control.get("applied")
            and not second_outline_restore
            and second_cleanup_authority.get("aliveExact") is True
        ):
            second_outline_restore = _restore_outline_all_for_test(
                out_dir, "second_finally", second_pid,
                second_launch_fingerprint,
                second_retained_witness,
            )
        if (
            second_pid
            and second_final_process_authority.get("terminatedExact") is True
        ):
            pass
        elif second_pid:
            second_pre_cleanup_authority = _capture_fresh_process_authority(
                second_pid, second_launch_fingerprint,
                retained_witness=second_retained_witness,
            )
            if second_pre_cleanup_authority.get("terminatedExact") is True:
                second_runtime_death_observed = True
                if not second_runtime_failure_evidence:
                    second_runtime_failure_evidence = _best_effort(
                        "runtime_failure_second_pre_stop",
                        lambda: _collect_ready_failure_evidence(
                            out_dir=out_dir,
                            tag="runtime_failure_second",
                            pid=second_pid,
                            desktop_name=second_desktop,
                            ready=second_ready,
                            log_offsets=second_offsets,
                            launch_epoch_ms=second_launch_epoch_ms,
                            event_since_id=0,
                            launch_fingerprint=(
                                second_launch_fingerprint
                            ),
                            retained_witness=second_retained_witness,
                        ),
                    )
                second_intentional_stop_started = True
                second_stop = _exact_handle_cleanup_stop(
                    second_pid,
                    second_cleanup_capability,
                    second_retained_witness,
                    mode="second-finally-signaled-native-handle",
                )
                second_final_process = dict(
                    second_stop.get("finalProcess", {}) or {}
                )
                second_final_process_authority = dict(
                    second_stop.get(
                        "finalProcessAuthority", {},
                    ) or {}
                )
            elif second_pre_cleanup_authority.get("aliveExact") is True:
                second_intentional_stop_started = True
                second_stop = (
                    _stop_after_ready_failure(
                        second_pid, second_launch_fingerprint,
                        second_retained_witness,
                        second_cleanup_capability,
                    )
                    if second_ready_failure_evidence
                    else _exact_handle_cleanup_stop(
                        second_pid,
                        second_cleanup_capability,
                        second_retained_witness,
                        mode="second-finally-alive-native-handle",
                    )
                )
                second_final_process = dict(
                    second_stop.get("finalProcess", {}) or {}
                )
                second_final_process_authority = dict(
                    second_stop.get(
                        "finalProcessAuthority", {},
                    ) or {}
                )
            else:
                second_intentional_stop_started = True
                second_stop = _exact_handle_cleanup_stop(
                    second_pid,
                    second_cleanup_capability,
                    second_retained_witness,
                    mode="second-finally-indeterminate-native-handle",
                )
                second_final_process = dict(
                    second_stop.get("finalProcess", {}) or {}
                )
                second_final_process_authority = dict(
                    second_stop.get(
                        "finalProcessAuthority", {},
                    ) or {}
                )
            second_final_process["processAuthority"] = (
                second_final_process_authority
            )
            if not second_stop.get("ok"):
                infrastructure_failure_reason = (
                    infrastructure_failure_reason
                    or "second exact HANDLE cleanup failed"
                )
            _json_write(out_dir / "second_stop_result.json", second_stop)
            _json_write(out_dir / "second_final_process_check.json", second_final_process)
        if (
            args.outline_all
            and pid
            and first_outline_control.get("applied")
            and not first_outline_restore
            and first_cleanup_authority.get("aliveExact") is True
        ):
            first_outline_restore = _restore_outline_all_for_test(
                out_dir, "first_finally", pid,
                first_launch_fingerprint,
                first_retained_witness,
            )
        if (
            pid and lifecycle
            and first_final_process_authority.get("terminatedExact") is True
        ):
            final_process = dict(first_final_process)
        elif pid:
            first_pre_cleanup_authority = _capture_fresh_process_authority(
                pid, first_launch_fingerprint,
                retained_witness=first_retained_witness,
            )
            if first_pre_cleanup_authority.get("terminatedExact") is True:
                first_runtime_death_observed = True
                if not runtime_failure_evidence:
                    runtime_failure_evidence = _best_effort(
                        "runtime_failure_first_pre_stop",
                        lambda: _collect_ready_failure_evidence(
                            out_dir=out_dir,
                            tag="runtime_failure_first",
                            pid=pid,
                            desktop_name=first_desktop,
                            ready=ready,
                            log_offsets=offsets,
                            launch_epoch_ms=launch_epoch_ms,
                            event_since_id=0,
                            launch_fingerprint=first_launch_fingerprint,
                            retained_witness=first_retained_witness,
                        ),
                    )
                first_intentional_stop_started = True
                stop = _exact_handle_cleanup_stop(
                    pid,
                    first_cleanup_capability,
                    first_retained_witness,
                    allow_detached_state=lifecycle,
                    mode="first-finally-signaled-native-handle",
                )
                final_process = dict(
                    stop.get("finalProcess", {}) or {}
                )
                first_final_process_authority = dict(
                    stop.get("finalProcessAuthority", {}) or {}
                )
            elif first_pre_cleanup_authority.get("aliveExact") is True:
                first_intentional_stop_started = True
                stop = (
                    _stop_after_ready_failure(
                        pid, first_launch_fingerprint,
                        first_retained_witness,
                        first_cleanup_capability,
                    )
                    if ready_failure_evidence
                    else _exact_handle_cleanup_stop(
                        pid,
                        first_cleanup_capability,
                        first_retained_witness,
                        allow_detached_state=lifecycle,
                        mode="first-finally-alive-native-handle",
                    )
                )
                final_process = dict(
                    stop.get("finalProcess", {}) or {}
                )
                first_final_process_authority = dict(
                    stop.get("finalProcessAuthority", {}) or {}
                )
            else:
                first_intentional_stop_started = True
                stop = _exact_handle_cleanup_stop(
                    pid,
                    first_cleanup_capability,
                    first_retained_witness,
                    allow_detached_state=lifecycle,
                    mode="first-finally-indeterminate-native-handle",
                )
                final_process = dict(
                    stop.get("finalProcess", {}) or {}
                )
                first_final_process_authority = dict(
                    stop.get("finalProcessAuthority", {}) or {}
                )
            final_process["processAuthority"] = (
                first_final_process_authority
            )
            if not stop.get("ok"):
                infrastructure_failure_reason = (
                    infrastructure_failure_reason
                    or "first exact HANDLE cleanup failed"
                )
        _json_write(out_dir / "stop_result.json", stop)
        if not pid:
            final_process = {"ok": True, "running": False, "pid": 0}
        _json_write(out_dir / "final_process_check.json", final_process)
        if lifecycle and not first_final_process:
            first_final_process = dict(final_process)
        # Runner-owned duplicates outlive STATE mutation/stop long enough to
        # prove termination, then are explicitly closed exactly once. The
        # wrapper invalidates its HANDLE before CloseHandle, so a recycled
        # integer can never be reused as authority.
        second_retained_witness_close = (
            _close_retained_launch_handle_witness(
                second_retained_witness
            )
        )
        first_retained_witness_close = (
            _close_retained_launch_handle_witness(
                first_retained_witness
            )
        )
        _json_write(
            out_dir / "retained_native_process_witness_close.json",
            {
                "first": first_retained_witness_close,
                "second": second_retained_witness_close,
            },
        )

    first_post_stop_exact = bool(
        pid > 0
        and first_intentional_stop_started
        and stop.get("cleanupCapabilityExact") is True
        and stop.get("pidTerminationCommandIssued") is False
        and first_final_process_authority.get("terminatedExact") is True
        and stop.get("ok") is True
        and first_retained_witness_close.get("ok") is True
    )
    gates["postStopTerminationExact"] = first_post_stop_exact
    if pid > 0 and not first_post_stop_exact:
        infrastructure_failure_reason = (
            infrastructure_failure_reason
            or "base cleanup termination is indeterminate"
        )
        error = error or infrastructure_failure_reason
    if (
        second_pid > 0
        and second_retained_witness_close.get("ok") is not True
    ):
        infrastructure_failure_reason = (
            infrastructure_failure_reason
            or "second retained native process HANDLE close failed"
        )
        error = error or infrastructure_failure_reason

    first_runtime_failure_confirmed = bool(
        runtime_failure_evidence.get(
            "classification", {}
        ).get("runtimeProcessFailure")
    )
    second_runtime_failure_confirmed = bool(
        second_runtime_failure_evidence.get(
            "classification", {}
        ).get("runtimeProcessFailure")
    )
    if first_runtime_death_observed or first_runtime_failure_confirmed:
        gates["processAlive"] = False
        if lifecycle:
            gates["lifecycleFirstStopClean"] = False
        error = error or (
            "first War3 process exited before intentional cleanup"
        )
    if second_runtime_death_observed or second_runtime_failure_confirmed:
        gates["lifecycleRelaunchAlive"] = False
        gates["lifecycleRelaunchGpuSkinClean"] = False
        gates["lifecycleSecondStopClean"] = False
        error = error or (
            "second War3 process exited before intentional cleanup"
        )

    gates["outlineControlApplied"] = (
        not args.outline_all or bool(first_outline_control.get("applied"))
    )
    gates["outlineControlRestored"] = (
        not args.outline_all or bool(first_outline_restore.get("restored"))
    )
    gates["outlineMaterialReady"] = (
        not args.outline_all or bool(first_outline_control.get("materialReady"))
    )
    gates["outlineSingleStageActivationClean"] = (
        not args.outline_all
        or bool(first_outline_control.get("singleStageActivationClean"))
    )

    failed_hard_gates = [
        name for name in _hard_gate_names(
            args.phase, bool(args.outline_all), args.diagnostics,
            args.execution_route,
        ) if not gates.get(name, False)
    ]
    failed_hard_gates.extend(
        name for name in VS_ROUTE_HARD_GATE_NAMES
        if not gates.get(name, False)
    )
    passed = (
        error is None
        and _hard_gate_pass(
            gates, args.phase, bool(args.outline_all), args.diagnostics,
            args.execution_route,
        )
        and all(gates.get(name, False) for name in VS_ROUTE_HARD_GATE_NAMES)
    )
    evidence_classifications = [
        dict(runtime_failure_evidence.get("classification", {}) or {}),
        dict(second_runtime_failure_evidence.get("classification", {}) or {}),
        dict(ready_failure_evidence.get("classification", {}) or {}),
        dict(second_ready_failure_evidence.get("classification", {}) or {}),
    ]
    failure_classification: Dict[str, Any] = {}
    if first_runtime_death_observed or second_runtime_death_observed:
        # Only a signaled retained launch handle whose PID/creation/path match
        # the immutable fingerprint can reach this branch. PID absence alone
        # remains indeterminate. Preserve a more specific exact GPU-skin
        # failure when one exists.
        failure_classification = next(
            (
                item for item in evidence_classifications
                if item.get("gpuSkinRuntimeFailure") is True
            ),
            {},
        )
        if not failure_classification:
            failure_classification = next(
                (
                    item for item in evidence_classifications
                    if item.get("runtimeProcessFailure") is True
                ),
                {},
            )
        if not failure_classification:
            failure_classification = {
                "primary": "runtimeProcessFailure",
                "readyInfrastructureFailure": False,
                "infrastructureFailure": False,
                "gpuSkinRuntimeFailure": False,
                "testActuationFailure": False,
                "runtimeProcessFailure": True,
                "processAliveAtEvidenceCapture": False,
                "readyMode": ready.get("mode"),
                "readyError": ready.get("error"),
                "evidenceNote": (
                    "retained exact launch handle signaled; supplemental "
                    "classification unavailable"
                ),
            }
    else:
        failure_classification = next(
            (item for item in evidence_classifications if item),
            {},
        )
    if not failure_classification:
        first_restore_actuation_failure = bool(
            first_outline_restore
            and first_outline_restore.get("processAliveAtAttempt") is True
            and first_outline_restore.get("controlPlaneReachable") is True
            and not first_outline_restore.get("restored")
        )
        second_restore_actuation_failure = bool(
            second_outline_restore
            and second_outline_restore.get("processAliveAtAttempt") is True
            and second_outline_restore.get("controlPlaneReachable") is True
            and not second_outline_restore.get("restored")
        )
        runtime_process_failure = bool(
            first_process_authority.get("terminatedExact") is True
            or (
                lifecycle
                and second_process_authority.get(
                    "terminatedExact"
                ) is True
            )
        )
        outline_control_failure = bool(
            not runtime_process_failure
            and args.outline_all
            and (
                (
                    ready.get("ok")
                    and not first_outline_control.get("applied")
                )
                or (
                    lifecycle
                    and second_ready.get("ok")
                    and not second_outline_control.get("applied")
                )
                or (
                    first_outline_control.get("applied")
                    and first_restore_actuation_failure
                )
                or (
                    lifecycle
                    and second_outline_control.get("applied")
                    and second_restore_actuation_failure
                )
            )
        )
        infrastructure_failure = bool(infrastructure_failure_reason)
        gpu_skin_runtime_failure = bool(
            ready.get("ok")
            and not infrastructure_failure
            and not outline_control_failure
            and not runtime_process_failure
            and (error is not None or failed_hard_gates)
        )
        outline_control_failure_reason = ""
        if outline_control_failure:
            if ready.get("ok") and not first_outline_control.get("applied"):
                outline_control_failure_reason = str(
                    first_outline_control.get("failureReason", "") or
                    "firstProcessOutlineActuationFailed"
                )
            elif first_restore_actuation_failure:
                outline_control_failure_reason = "firstProcessOutlineRestoreFailed"
            elif second_restore_actuation_failure:
                outline_control_failure_reason = "secondProcessOutlineRestoreFailed"
            elif second_ready.get("ok"):
                outline_control_failure_reason = str(
                    second_outline_control.get("failureReason", "") or
                    "secondProcessOutlineActuationFailed"
                )
        failure_classification = {
            "primary": (
                "runtimeProcessFailure" if runtime_process_failure
                else "infrastructureFailure" if infrastructure_failure
                else "testActuationFailure" if outline_control_failure
                else "gpuSkinRuntimeFailure" if gpu_skin_runtime_failure
                else "none" if passed
                else "harnessFailure"
            ),
            "readyInfrastructureFailure": False,
            "infrastructureFailure": infrastructure_failure,
            "infrastructureFailureReason": infrastructure_failure_reason,
            "gpuSkinRuntimeFailure": gpu_skin_runtime_failure,
            "testActuationFailure": outline_control_failure,
            "testActuationFailureReason": outline_control_failure_reason,
            "runtimeProcessFailure": runtime_process_failure,
            "processAliveAtEvidenceCapture": alive_before_stop,
            "processLivenessAuthority": first_process_authority,
            "secondProcessLivenessAuthority": second_process_authority,
            "readyMode": ready.get("mode"),
            "readyError": ready.get("error"),
        }
    failure_classification["runtimeDeathObserved"] = {
        "first": first_runtime_death_observed,
        "second": second_runtime_death_observed,
    }
    result = {
        "test": f"P4 {args.phase} isolated",
        "artifact": out_dir.name,
        "verdict": "PASS" if passed else "FAIL",
        "failedHardGates": failed_hard_gates,
        "error": error,
        "failureClassification": failure_classification,
        "readyInfrastructureFailure": bool(failure_classification.get("readyInfrastructureFailure")),
        "infrastructureFailure": bool(failure_classification.get("infrastructureFailure")),
        "gpuSkinRuntimeFailure": bool(failure_classification.get("gpuSkinRuntimeFailure")),
        "testActuationFailure": bool(failure_classification.get("testActuationFailure")),
        "runtimeProcessFailure": bool(failure_classification.get("runtimeProcessFailure")),
        "runtimeDeathObserved": {
            "first": first_runtime_death_observed,
            "second": second_runtime_death_observed,
        },
        "processAuthorities": {
            "firstEvidence": first_process_authority,
            "secondEvidence": second_process_authority,
            "firstFinal": first_final_process_authority,
            "secondFinal": second_final_process_authority,
        },
        "readyFailureEvidence": ready_failure_evidence,
        "runtimeFailureEvidence": runtime_failure_evidence,
        "preEvidenceProcessSnapshot": first_pre_evidence_process_snapshot,
        "launchInstanceFingerprints": {
            "first": first_launch_fingerprint,
            "second": second_launch_fingerprint,
        },
        "launchCleanupCapabilities": {
            "first": first_cleanup_capability,
            "second": second_cleanup_capability,
        },
        "sourceEdited": False,
        "caseLabel": args.case_label,
        "caseLabelSafe": args.case_label_safe,
        "requestedDiagnostics": args.diagnostics,
        "requestedPoisonSidecarPolicy": args.sidecar_policy,
        "requestedExecutionRoute": args.execution_route,
        "requestedDisableModules": args.disable_modules,
        "map": str(args.map_path),
        "mapArtifact": args.map_metadata,
        "buildPerformed": bool(args.build_before_launch),
        "deployRequested": bool(args.deploy),
        "dll": {"before": hashes_before, "after": _dll_hashes()},
        "outlineShaderHashes": outline_shader_hashes,
        "runtime": {
            "pid": pid, "isolatedDesktop": True, "desktop": first_desktop,
            "map": str(args.map_path), "mapArtifact": args.map_metadata,
            "env": env, "durationSec": args.duration_sec, "pollIntervalSec": args.poll_interval_sec,
            "processAuthority": first_process_authority,
        },
        "launch": launch,
        "ready": ready,
        "runtimePollCount": len(runtime_polls),
        "screenshots": screenshots,
        "visualScope": {
            "outlineAllRequested": bool(args.outline_all),
            "outlineControl": first_outline_control,
            "outlineRestore": first_outline_restore,
            "outlineSubmitted": diag["P3"].get("outlineSubmitted"),
            "outlineSameSlice": diag["P3"].get("outlineSameSlice"),
            "outlineSliceMismatch": diag["P3"].get("outlineSliceMismatch"),
            "scope": "两张隔离桌面末帧截图；outline submission 是计数硬门，不是逐帧视觉证明。",
        },
        "logCopies": log_copies,
        "diagnostics": diag,
        "gates": gates,
        "cleanup": {
            "stop": stop,
            "finalProcess": final_process,
            "finalProcessAuthority": first_final_process_authority,
        },
        "lifecycle": {
            "enabled": lifecycle,
            "windowSteps": window_steps,
            "resetSteps": reset_steps,
            "first": {
                "pid": pid, "desktop": first_desktop, "launch": launch, "ready": ready,
                "outlineControl": first_outline_control,
                "outlineRestore": first_outline_restore,
                "polls": runtime_polls, "screenshots": screenshots, "debug": debug,
                "logCopies": log_copies, "diagnostics": diag, "crashMatches": crash_matches,
                "preEvidenceProcessSnapshot": first_pre_evidence_process_snapshot,
                "launchInstanceFingerprint": first_launch_fingerprint,
                "runtimeFailureEvidence": runtime_failure_evidence,
                "processAuthority": first_process_authority,
                "finalProcessAuthority": first_final_process_authority,
                "stop": stop, "finalProcess": first_final_process,
            },
            "second": {
                "pid": second_pid, "desktop": second_desktop, "launch": second_launch, "ready": second_ready,
                "outlineControl": second_outline_control,
                "outlineRestore": second_outline_restore,
                "polls": second_runtime_polls, "aliveBeforeStop": second_alive_before_stop,
                "screenshot": second_screenshot, "debug": second_debug, "logCopies": second_log_copies,
                "diagnostics": second_diag, "crashMatches": second_crash_matches,
                "readyFailureEvidence": second_ready_failure_evidence,
                "preEvidenceProcessSnapshot": second_pre_evidence_process_snapshot,
                "launchInstanceFingerprint": second_launch_fingerprint,
                "runtimeFailureEvidence": second_runtime_failure_evidence,
                "processAuthority": second_process_authority,
                "finalProcessAuthority": second_final_process_authority,
                "stop": second_stop, "finalProcess": second_final_process,
            },
        },
        "git": _git_summary(),
    }
    _json_write(out_dir / "p4_result.json", result)
    _text_write(
        out_dir / "test_description.txt",
        f"P4 {args.phase} 隔离桌面测试。map={args.map_path}\n"
        f"mapResolved={args.map_metadata['resolvedPath']}\n"
        f"mapSize={args.map_metadata['size']}\n"
        f"mapSha256={args.map_metadata['sha256']}\n"
        f"caseLabel={args.case_label or ''}\n"
        f"requestedDiagnostics={args.diagnostics}\n"
        f"requestedPoisonSidecarPolicy={args.sidecar_policy}\n"
        f"requestedExecutionRoute={args.execution_route}\n"
        f"env={json.dumps(env)}\n"
        f"outlineAllRequested={bool(args.outline_all)} "
        f"outlineControlApplied={bool(first_outline_control.get('applied'))} "
        f"outlineSubmitted={diag['P3'].get('outlineSubmitted')} "
        f"outlineSameSlice={diag['P3'].get('outlineSameSlice')} "
        f"outlineSliceMismatch={diag['P3'].get('outlineSliceMismatch')}\n"
        "全局 tracker 冲突只作报告；只有运行时发出的 exact-takeover 冲突才判失败。\n",
    )
    _write_sha256s(out_dir)
    print(f"P4 {args.phase} {result['verdict']}: {out_dir}", flush=True)
    return 0 if passed else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="P4 GPU skin bypass isolated-desktop conductor")
    parser.add_argument("--phase", choices=("build-only", "crash-gate", "lifecycle"), default="crash-gate")
    parser.add_argument("--duration-sec", type=int, default=None)
    parser.add_argument("--poll-interval-sec", type=int, default=5)
    parser.add_argument("--screenshot-gap-sec", type=int, default=4)
    parser.add_argument("--ready-timeout-sec", type=int, default=120)
    parser.add_argument(
        "--diagnostics", choices=("light", "full"), default="full",
        help="Select production-light or full hot-path GPU-skin diagnostics.",
    )
    parser.add_argument(
        "--sidecar-policy",
        choices=tuple(GPU_SKIN_POISON_SIDECAR_POLICIES),
        default="both",
        help=(
            "Select the frozen report-only outside-poison sidecar policy; "
            "this axis is orthogonal to --diagnostics."
        ),
    )
    parser.add_argument(
        "--execution-route", choices=GPU_SKIN_EXECUTION_ROUTES,
        default="compute",
        help="选择默认 Compute，或显式启用 VS-A 语义验证路线。",
    )
    parser.add_argument(
        "--disable-modules", default="",
        help="仅用于隔离二分的 DXVK_WAR3_DISABLE 模块列表；默认不禁用任何模块。",
    )
    parser.add_argument("--no-deploy", dest="deploy", action="store_false", help="Use the already deployed d3d9.dll.")
    parser.add_argument("--build-before-launch", action="store_true", help="Run the build-only phase before a runtime phase.")
    parser.add_argument("--outline-all", action="store_true", help="Force outline coverage and require outline submissions.")
    parser.add_argument(
        "--map", dest="map_path", type=Path, default=Path(LOW_MAP),
        help="Warcraft III .w3x map used by both isolated launches.",
    )
    parser.add_argument(
        "--case-label", default=None,
        help="Optional readable label appended safely to the artifact directory.",
    )
    parser.set_defaults(deploy=True)
    args = parser.parse_args()
    if args.duration_sec is None:
        args.duration_sec = 35 if args.phase == "crash-gate" else 300
    if args.duration_sec < 1 or args.poll_interval_sec < 1 or args.screenshot_gap_sec < 0:
        parser.error("duration/poll/screenshot gap must be non-negative and duration/poll must be positive")
    try:
        args.map_path = args.map_path.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        parser.error(f"--map must resolve to an existing file: {exc}")
    if not args.map_path.is_file() or args.map_path.suffix.lower() != ".w3x":
        parser.error("--map must be an existing .w3x file")
    args.map_metadata = _map_metadata(args.map_path)
    args.case_label_safe = _sanitize_case_label(args.case_label)
    if args.case_label and not args.case_label_safe:
        parser.error("--case-label must contain at least one filename-safe character")

    # 在任何构建、部署或启动前先闭合导体自身的路线判定。
    _vs_route_synthetic_self_tests()

    ARTIFACTS.mkdir(parents=True, exist_ok=True)
    case_suffix = f"_{args.case_label_safe}" if args.case_label_safe else ""
    prefix = (
        f"gpu_skin_p4_{args.phase.replace('-', '_')}_isolated"
        f"_diag_{args.diagnostics}_sidecar_{args.sidecar_policy}"
        f"_route_{args.execution_route}"
        f"{case_suffix}_{_now()}"
    )
    out_dir = ARTIFACTS / prefix
    out_dir.mkdir(parents=True, exist_ok=False)
    _json_write(out_dir / "map_artifact.json", args.map_metadata)

    if args.phase == "build-only":
        return _run_build_only(args, out_dir)
    if args.build_before_launch:
        build_dir = ARTIFACTS / f"{prefix}_build"
        build_dir.mkdir(parents=True, exist_ok=False)
        if _run_build_only(args, build_dir) != 0:
            _text_write(
                out_dir / "test_description.txt",
                "Runtime phase skipped because --build-before-launch failed.\n"
                f"mapResolved={args.map_metadata['resolvedPath']}\n"
                f"mapSize={args.map_metadata['size']}\n"
                f"mapSha256={args.map_metadata['sha256']}\n"
                f"caseLabel={args.case_label or ''}\n",
            )
            return 1
    return _run_runtime_phase(args, out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
