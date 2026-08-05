#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P1B GPU-skin Dual parity gate on an isolated desktop.

This conductor is intentionally narrow: it launches one map in Dual mode,
collects exact-PID evidence and two forced quiescent diagnostic snapshots, and
proves that GPU output matches the original CPU output without enabling any
P2/P3/P4 consumer takeover. It is not a performance benchmark.

Only the repository's Test Conductor may execute this script.
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

import run_gpu_skin_p4_isolated as p4


DEFAULT_MAP = Path(
    r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
)
HARD_GATE_NAMES: Tuple[str, ...] = (
    "diagnosticsPresent",
    "modeDual",
    "ready",
    "exactPidEvidence",
    "processAlive",
    "twoScreenshots",
    "forcedDiagnosticsSnapshot",
    "forcedDiagnosticsQuiescent",
    "protocolAccountingClosed",
    "computeAccountingClosed",
    "kernelAccountingClosed",
    "globalParityExact",
    "formatFlowClosed",
    "parityFormatClosed",
    "oddFormatEligible",
    "oddFormatParityCovered",
    "kernelBypassZero",
    "p3TakeoverZero",
    "shadowTakeoverZero",
    "lifetimeClean",
    "quiescenceClean",
    "crashScanClean",
    "cleanupClean",
)


def _all_present(values: Iterable[Optional[int]]) -> bool:
    return all(value is not None for value in values)


def _all_zero(values: Iterable[Optional[int]]) -> bool:
    return all(value == 0 for value in values)


def _screenshot_pid(item: Dict[str, Any]) -> int:
    direct = int(item.get("pid", 0) or 0)
    if direct:
        return direct
    details = dict(item.get("details", {}) or {})
    request = dict(details.get("request", {}) or {})
    return int(request.get("pid", 0) or 0)


def _parse_dual_diagnostics(
    base: Dict[str, Any],
) -> Dict[str, Any]:
    latest = dict(base.get("rawLatest", {}) or {})
    mode_line = str(latest.get("mode", "") or "")
    consume_line = str(latest.get("consume", "") or "")
    parity_format_line = str(latest.get("parityFormat", "") or "")

    global_parity = p4._match_tuple(
        consume_line, r"\bparity=(\d+)/(\d+)/(\d+)", 3
    )
    parity_skip = p4._match_tuple(
        consume_line, r"\bparitySkip=(\d+)/(\d+)", 2
    )
    parity_in_flight = p4._match_tuple(
        consume_line, r"\binFlight=(\d+)/(\d+)", 2
    )
    samples = p4._match_tuple(
        parity_format_line,
        r"\bsamples=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        6,
    )
    matches = p4._match_tuple(
        parity_format_line,
        r"\bmatches=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        6,
    )
    mismatches = p4._match_tuple(
        parity_format_line,
        r"\bmismatch=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)",
        6,
    )
    base["dualParity"] = {
        "mode": p4._match_named_int(mode_line, "mode"),
        "global": dict(zip(
            ("samples", "matches", "mismatches"), global_parity
        )),
        "skipped": dict(zip(("budget", "source"), parity_skip)),
        "inFlight": dict(zip(("count", "bytes"), parity_in_flight)),
        "byFormat": {
            "samples": dict(zip(
                ("f0", "f1", "f2", "f3", "f4", "f5"), samples
            )),
            "matches": dict(zip(
                ("f0", "f1", "f2", "f3", "f4", "f5"), matches
            )),
            "mismatches": dict(zip(
                ("f0", "f1", "f2", "f3", "f4", "f5"), mismatches
            )),
        },
        "raw": parity_format_line,
    }
    return base


def _empty_diagnostics() -> Dict[str, Any]:
    return _parse_dual_diagnostics(p4._parse_gpu_skin_diag("", {}))


def _evaluate_gates(
    diag: Dict[str, Any],
    *,
    pid: int,
    ready: Dict[str, Any],
    process_alive: bool,
    screenshots: List[Dict[str, Any]],
    crash_matches: List[str],
) -> Dict[str, Any]:
    dual = diag["dualParity"]
    parity = dual["global"]
    samples = tuple(dual["byFormat"]["samples"].values())
    matches = tuple(dual["byFormat"]["matches"].values())
    mismatches = tuple(dual["byFormat"]["mismatches"].values())
    format_flow = diag["formatCoverage"]["flow"]
    format_class = diag["formatCoverage"]["classificationByFormat"]
    raw_by_format = tuple(
        diag["formatCoverage"]["formatBuckets"].values()
    )
    outside_by_format = tuple(format_class["outside"].values())
    inside_by_format = tuple(format_class["inside"].values())
    eligible_by_format_class = tuple(format_class["eligible"].values())
    reject_by_format = tuple(
        tuple(format_class["reject"][f"f{index}"].values())
        for index in range(6)
    )
    strict_reject_global = tuple(
        diag["formatCoverage"]["strictReject"].values()
    )
    flow_eligible = tuple(format_flow["eligible"].values())
    flow_learned = tuple(format_flow["learned"].values())
    flow_candidate = tuple(format_flow["candidate"].values())
    flow_job = tuple(format_flow["job"].values())
    protocol = diag["protocol"]
    compute = diag["compute"]
    kernel = diag["kernel"]
    native_upload = diag["nativeUpload"]
    p2 = diag["P2"]
    p3 = diag["P3"]
    p4_shadow = diag["P4Shadow"]
    lifetime = diag["lifetime"]
    forced = dict(diag.get("forcedSnapshot", {}) or {})

    protocol_values = (
        protocol["dispatchBegin"], protocol["dispatchEnd"],
        protocol["truePairErr"], protocol["epochLeak"],
        *protocol["pending"],
    )
    compute_values = (
        *compute["batch"], *compute["jobs"], *compute["dispatch"],
        *compute["palette"],
    )
    parity_values = (
        *parity.values(), *dual["skipped"].values(),
        *dual["inFlight"].values(), *samples, *matches, *mismatches,
        *flow_eligible, *flow_learned, *flow_candidate, *flow_job,
        *raw_by_format, *outside_by_format, *inside_by_format,
        *eligible_by_format_class,
        *(value for row in reject_by_format for value in row),
    )
    p3_takeover_values = (
        p3["hit"], p3["reject"], p3["submitted"],
        p3["outlineSubmitted"], p3["outlineSameSlice"],
        p3["outlineSliceMismatch"], p3["restoreArms"],
        p3["restoreRebinds"], p3["restoreOverlap"],
        p3["restorePending"], p3["bypassAttempts"],
        p3["bypassAuthorizations"], p3["bypassCommits"],
        p3["bypassFallbacks"], p3["bypassMismatch"],
        p3["bypassPending"], p3["bypassHostAuthorizationMismatch"],
        p3["restoreFail"],
    )
    shadow_takeover_values = (
        *p2.values(), *p4_shadow.values(),
    )
    lifetime_values = (
        lifetime["backpressure"], lifetime["limitViolation"],
        lifetime["claims"], lifetime["uploadPagesAllocated"],
        lifetime["uploadPagesReclaimed"], lifetime["outputPending"],
    )
    diagnostics_present = _all_present((
        dual["mode"], *protocol_values, *compute_values,
        kernel["hookCalls"], kernel["originalCalls"],
        kernel["bypassedCalls"], kernel["bypassedBytes"],
        native_upload["bypassedCalls"], native_upload["bypassedBytes"],
        *parity_values, *p3_takeover_values, *shadow_takeover_values,
        *lifetime_values,
    ))
    protocol_closed = bool(
        _all_present(protocol_values)
        and protocol["dispatchBegin"] == protocol["dispatchEnd"]
        and protocol["truePairErr"] == 0
        and protocol["epochLeak"] == 0
        and _all_zero(protocol["pending"])
    )
    compute_closed = bool(
        _all_present(compute_values)
        and compute["batch"][0] == compute["batch"][1] + compute["batch"][2]
        and compute["jobs"][0] == compute["jobs"][1]
        and compute["dispatch"][0] == compute["dispatch"][1]
        and compute["palette"][0] == compute["palette"][1]
    )
    kernel_closed = bool(
        _all_present((
            kernel["hookCalls"], kernel["originalCalls"],
            kernel["bypassedCalls"],
        ))
        and kernel["hookCalls"] ==
            kernel["originalCalls"] + kernel["bypassedCalls"]
    )
    global_parity_exact = bool(
        _all_present(parity.values())
        and (parity["samples"] or 0) > 0
        and parity["samples"] == parity["matches"]
        and parity["mismatches"] == 0
        and _all_zero(dual["inFlight"].values())
    )
    parity_format_closed = bool(
        _all_present((*samples, *matches, *mismatches, *parity.values()))
        and all(
            sample == match + mismatch
            for sample, match, mismatch in zip(samples, matches, mismatches)
        )
        and sum(samples) == parity["samples"]
        and sum(matches) == parity["matches"]
        and sum(mismatches) == parity["mismatches"]
    )
    format_flow_closed = bool(
        _all_present((
            *flow_eligible, *flow_learned, *flow_candidate, *flow_job,
            *raw_by_format, *outside_by_format, *inside_by_format,
            *eligible_by_format_class,
            *(value for row in reject_by_format for value in row),
            diag["formatCoverage"]["eligible"], compute["jobs"][0],
            diag["formatCoverage"]["nativeUploadRaw"],
            diag["formatCoverage"]["nativeUploadOutside"],
            *strict_reject_global,
        ))
        and sum(flow_eligible) == diag["formatCoverage"]["eligible"]
        and sum(raw_by_format) ==
            diag["formatCoverage"]["nativeUploadRaw"]
        and sum(outside_by_format) ==
            diag["formatCoverage"]["nativeUploadOutside"]
        and sum(inside_by_format) == (
            diag["formatCoverage"]["nativeUploadRaw"] -
            diag["formatCoverage"]["nativeUploadOutside"]
        )
        and tuple(flow_eligible) == tuple(eligible_by_format_class)
        and sum(flow_job) == compute["jobs"][0]
        and all(
            learned == eligible
            for learned, eligible in zip(flow_learned, flow_eligible)
        )
        and all(
            job <= candidate
            for job, candidate in zip(flow_job, flow_candidate)
        )
        and diag["formatCoverage"]["nativeUploadRaw"] >=
            diag["formatCoverage"]["nativeUploadOutside"]
        and (
            diag["formatCoverage"]["nativeUploadRaw"] -
            diag["formatCoverage"]["nativeUploadOutside"]
        ) == (
            diag["formatCoverage"]["eligible"] +
            sum(strict_reject_global)
        )
        and all(
            raw_by_format[index] ==
                outside_by_format[index] + inside_by_format[index]
            and inside_by_format[index] ==
                eligible_by_format_class[index] + sum(reject_by_format[index])
            for index in range(6)
        )
        and all(
            raw_by_format[index] == 0
            and outside_by_format[index] == 0
            and inside_by_format[index] == 0
            and eligible_by_format_class[index] == 0
            for index in (6, 7)
        )
        and all(
            flow_candidate[index] == 0 and flow_job[index] == 0
            for index in (6, 7)
        )
        and all(
            samples[index] <= flow_job[index]
            for index in range(6)
        )
        and sum(strict_reject_global) ==
            sum(value for row in reject_by_format for value in row)
        and all(
            sum(row[reason] for row in reject_by_format) ==
                strict_reject_global[reason]
            for reason in range(len(strict_reject_global))
        )
    )
    eligible_odd_indices = tuple(
        index for index in (1, 3, 5)
        if flow_eligible[index] is not None and flow_eligible[index] > 0
    )
    odd_format_eligible = bool(
        _all_present(flow_eligible) and eligible_odd_indices
    )
    odd_format_covered = bool(
        odd_format_eligible
        and _all_present((
            *flow_learned, *flow_candidate, *flow_job,
            *samples, *matches, *mismatches,
        ))
        and all(
            flow_learned[index] == flow_eligible[index]
            and flow_candidate[index] > 0
            and flow_job[index] > 0
            and samples[index] > 0
            and matches[index] == samples[index]
            and mismatches[index] == 0
            for index in eligible_odd_indices
        )
    )
    kernel_bypass_zero = bool(
        _all_zero((
            kernel["bypassedCalls"], kernel["bypassedBytes"],
            native_upload["bypassedCalls"], native_upload["bypassedBytes"],
        ))
    )
    lifetime_clean = bool(
        _all_present(lifetime_values)
        and lifetime["backpressure"] == 0
        and lifetime["limitViolation"] == 0
        and lifetime["claims"] == 0
        and lifetime["uploadPagesAllocated"] ==
            lifetime["uploadPagesReclaimed"]
        and lifetime["outputPending"] == 0
        and forced.get("cleanPairResourceDeltaClosed") is True
    )
    forced_snapshot_clean = bool(
        forced.get("ok")
        and forced.get("selectedBlockComplete")
        and forced.get("twoCleanSnapshots")
        and forced.get("progressValid")
        and forced.get("cleanPairRevalidatedInCollectedEvidence")
    )
    quiescence_clean = p4._quiescence_diag_consistent(diag)
    screenshot_pid_exact = bool(
        len(screenshots) == 2
        and all(item.get("ok") for item in screenshots)
        and all(_screenshot_pid(item) == pid for item in screenshots)
    )
    exact_pid = bool(
        pid > 0
        and int(ready.get("pid", 0) or 0) == pid
        and int(forced.get("pid", 0) or 0) == pid
        and screenshot_pid_exact
    )
    return {
        "diagnosticsPresent": diagnostics_present,
        "modeDual": dual["mode"] == 2,
        "ready": bool(ready.get("ok")),
        "exactPidEvidence": exact_pid,
        "processAlive": process_alive,
        "twoScreenshots": screenshot_pid_exact,
        "forcedDiagnosticsSnapshot": forced_snapshot_clean,
        "forcedDiagnosticsQuiescent": quiescence_clean,
        "protocolAccountingClosed": protocol_closed,
        "computeAccountingClosed": compute_closed,
        "kernelAccountingClosed": kernel_closed,
        "globalParityExact": global_parity_exact,
        "formatFlowClosed": format_flow_closed,
        "parityFormatClosed": parity_format_closed,
        "oddFormatEligible": odd_format_eligible,
        "oddFormatParityCovered": odd_format_covered,
        "kernelBypassZero": kernel_bypass_zero,
        "p3TakeoverZero": (
            _all_present(p3_takeover_values)
            and _all_zero(p3_takeover_values)
        ),
        "shadowTakeoverZero": (
            _all_present(shadow_takeover_values)
            and _all_zero(shadow_takeover_values)
        ),
        "lifetimeClean": lifetime_clean,
        "quiescenceClean": quiescence_clean,
        "crashScanClean": not crash_matches,
        "cleanupClean": False,
        "parityReport": dual,
        "formatFlowReport": format_flow,
        "takeoverReport": {
            "kernel": kernel,
            "nativeUpload": native_upload,
            "P2": p2,
            "P3": p3,
            "P4Shadow": p4_shadow,
        },
    }


def _resolve_map(parser: argparse.ArgumentParser, value: Path) -> Path:
    try:
        resolved = value.expanduser().resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        parser.error(f"--map must resolve to an existing file: {exc}")
    if not resolved.is_file() or resolved.suffix.lower() != ".w3x":
        parser.error("--map must be an existing .w3x file")
    return resolved


def _run(args: argparse.Namespace, out_dir: Path) -> int:
    map_artifact = p4._map_metadata(args.map_path)
    environment = {
        "DXVK_WAR3_GPU_SKIN_MODE": "dual",
        "DXVK_WAR3_GPU_SKIN_DIFF_EVERY_N": str(args.diff_every_n),
        "DXVK_WAR3_GPU_SKIN_DIAGNOSTICS": "full",
        "DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES": "0",
    }
    p4._json_write(out_dir / "map_artifact.json", map_artifact)
    log_offsets = p4._snapshot_log_offsets()
    p4._json_write(out_dir / "prelaunch_log_state.json", log_offsets)
    dll_before = p4._dll_hashes()
    p4._json_write(
        out_dir / "deployment_hashes.json",
        {"before": dll_before, "deployRequested": bool(args.deploy)},
    )

    desktop_name = f"War3GpuSkinDual_{p4._now()}"
    launch: Dict[str, Any] = {}
    ready: Dict[str, Any] = {}
    ready_failure: Dict[str, Any] = {}
    stop: Dict[str, Any] = {}
    final_process: Dict[str, Any] = {"ok": True, "running": False, "pid": 0}
    runtime_polls: List[Dict[str, Any]] = []
    screenshots: List[Dict[str, Any]] = []
    debug: Dict[str, Any] = {}
    log_copies: Dict[str, Any] = {}
    crash_matches: List[str] = []
    diag = _empty_diagnostics()
    gates: Dict[str, Any] = {name: False for name in HARD_GATE_NAMES}
    pid = 0
    event_since_id = 0
    launch_epoch_ms = 0
    alive_at_evidence = False
    error: Optional[str] = None

    try:
        launch_epoch_ms = int(time.time() * 1000)
        launch = p4.launch_war3_test(
            war3_dir=str(p4.WAR3_DIR),
            map_path=str(args.map_path),
            windowed=False,
            use_isolated_desktop=True,
            desktop_name=desktop_name,
            auto_perf_record=False,
            deploy_d3d9_before_launch=bool(args.deploy),
            enforce_video_baseline=True,
            env_overrides_json=json.dumps(environment),
        )
        p4._json_write(out_dir / "launch_result.json", launch)
        if not launch.get("ok"):
            error = f"launch failed: {launch.get('error', launch)}"
            raise RuntimeError(error)
        pid = int(launch["pid"])
        p4._json_write(
            out_dir / "exact_pid.json",
            {"pid": pid, "source": "launch_war3_test", "eventCursor": 0},
        )
        ready = p4.wait_for_game_ready(
            timeout_sec=args.ready_timeout_sec,
            pid=pid,
            allow_fallback=False,
        )
        p4._json_write(out_dir / "ready_result.json", ready)
        if not ready.get("ok"):
            ready_failure = p4._collect_ready_failure_evidence(
                out_dir=out_dir,
                tag="ready_failure",
                pid=pid,
                desktop_name=desktop_name,
                ready=ready,
                log_offsets=log_offsets,
                launch_epoch_ms=launch_epoch_ms,
                event_since_id=event_since_id,
            )
            error = f"ready failed: {ready.get('error', ready)}"
            raise RuntimeError(error)

        start = time.monotonic()
        while time.monotonic() - start < args.duration_sec:
            poll = p4._runtime_poll(pid, time.monotonic() - start)
            runtime_polls.append(poll)
            if not poll["running"]:
                error = "War3 exited during Dual runtime polling"
                break
            time.sleep(max(1, args.poll_interval_sec))
        p4._json_write(out_dir / "runtime_polls.json", runtime_polls)

        if p4.is_war3_running(pid=pid).get("running"):
            for name in ("a", "b"):
                screenshots.append(p4.capture_war3_screenshot(
                    output_path=str(out_dir / f"war3_dual_{name}.png"),
                    pid=pid,
                    war3_dir=str(p4.WAR3_DIR),
                    timeout_sec=12,
                ))
                if name == "a":
                    time.sleep(args.screenshot_gap_sec)
        p4._json_write(out_dir / "screenshot_results.json", screenshots)

        runtime_data = runtime_polls[-1]["data"] if runtime_polls else {}
        (
            debug,
            _,
            log_copies,
            crash_matches,
            diag,
            event_since_id,
        ) = p4._collect_session_evidence(
            out_dir,
            "dual",
            pid,
            log_offsets,
            event_since_id,
            runtime_data,
        )
        diag = _parse_dual_diagnostics(diag)
        p4._json_write(out_dir / "gpu_skin_dual_diag.json", diag)
        alive_at_evidence = bool(p4.is_war3_running(pid=pid).get("running"))
        gates = _evaluate_gates(
            diag,
            pid=pid,
            ready=ready,
            process_alive=alive_at_evidence,
            screenshots=screenshots,
            crash_matches=crash_matches,
        )
    except Exception as exc:
        error = error or f"{type(exc).__name__}: {exc}"
    finally:
        if pid:
            stop = (
                p4._stop_after_ready_failure(pid)
                if ready_failure
                else p4.stop_war3(
                    pid=pid,
                    graceful_wait_sec=3,
                    force=True,
                    avoid_foreground_switch=True,
                )
            )
            final_process = p4.is_war3_running(pid=pid)
        p4._json_write(out_dir / "stop_result.json", stop)
        p4._json_write(out_dir / "final_process_check.json", final_process)

    gates["cleanupClean"] = bool(
        pid > 0
        and stop.get("ok")
        and final_process.get("ok")
        and not final_process.get("running")
    )
    failed = [name for name in HARD_GATE_NAMES if not gates.get(name, False)]
    passed = error is None and not failed
    result = {
        "test": "P1B GPU skin Dual isolated parity",
        "artifact": out_dir.name,
        "verdict": "PASS" if passed else "FAIL",
        "failedHardGates": failed,
        "error": error,
        "pid": pid,
        "exactPid": pid,
        "isolatedDesktop": True,
        "desktop": desktop_name,
        "map": str(args.map_path),
        "mapArtifact": map_artifact,
        "environment": environment,
        "durationSec": args.duration_sec,
        "diffEveryN": args.diff_every_n,
        "deployRequested": bool(args.deploy),
        "dll": {"before": dll_before, "after": p4._dll_hashes()},
        "launch": launch,
        "ready": ready,
        "readyFailureEvidence": ready_failure,
        "runtimePollCount": len(runtime_polls),
        "screenshots": screenshots,
        "diagnostics": diag,
        "gates": gates,
        "crashMatches": crash_matches,
        "evidence": {"debug": debug, "logCopies": log_copies},
        "cleanup": {"stop": stop, "finalProcess": final_process},
        "git": p4._git_summary(),
        "performanceVerdict": "NOT_APPLICABLE_ISOLATED_DESKTOP",
    }
    p4._json_write(out_dir / "dual_result.json", result)
    p4._text_write(
        out_dir / "test_description.txt",
        "P1B GPU-skin Dual parity on an isolated desktop; not a performance test.\n"
        f"mapResolved={map_artifact['resolvedPath']}\n"
        f"mapSize={map_artifact['size']}\n"
        f"mapSha256={map_artifact['sha256']}\n"
        f"dllBefore={dll_before.get('deployedD3D9')}\n"
        f"dllAfter={result['dll']['after'].get('deployedD3D9')}\n"
        f"pid={pid}\n"
        f"mode=dual diffEveryN={args.diff_every_n}\n"
        f"ready={bool(ready.get('ok'))} screenshots={len(screenshots)}\n"
        f"verdict={result['verdict']} failedHardGates={','.join(failed)}\n",
    )
    p4._write_sha256s(out_dir)
    print(f"Dual parity {result['verdict']}: {out_dir}", flush=True)
    return 0 if passed else 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="P1B GPU-skin Dual parity isolated-desktop conductor"
    )
    parser.add_argument("--map", type=Path, default=DEFAULT_MAP)
    parser.add_argument("--duration-sec", type=int, default=60)
    parser.add_argument("--poll-interval-sec", type=int, default=5)
    parser.add_argument("--screenshot-gap-sec", type=int, default=4)
    parser.add_argument("--ready-timeout-sec", type=int, default=120)
    parser.add_argument(
        "--diff-every-n", type=int, default=1,
        help="GPU/CPU byte-diff sampling period; P1B coverage defaults to 1.",
    )
    parser.add_argument(
        "--no-deploy", dest="deploy", action="store_false",
        help="Use the already deployed d3d9.dll.",
    )
    parser.add_argument("--case-label", default="")
    parser.set_defaults(deploy=True)
    args = parser.parse_args()
    if (
        args.duration_sec < 1
        or args.poll_interval_sec < 1
        or args.screenshot_gap_sec < 0
        or args.ready_timeout_sec < 1
        or args.diff_every_n < 1
    ):
        parser.error(
            "duration/poll/ready/diff must be positive; screenshot gap must be non-negative"
        )
    args.map_path = _resolve_map(parser, args.map)
    safe_label = p4._sanitize_case_label(args.case_label)
    if args.case_label and not safe_label:
        parser.error("--case-label must contain a filename-safe character")

    p4.ARTIFACTS.mkdir(parents=True, exist_ok=True)
    label_suffix = f"_{safe_label}" if safe_label else ""
    out_dir = p4.ARTIFACTS / (
        f"gpu_skin_dual_isolated{label_suffix}_{p4._now()}"
    )
    out_dir.mkdir(parents=True, exist_ok=False)
    return _run(args, out_dir)


if __name__ == "__main__":
    raise SystemExit(main())
