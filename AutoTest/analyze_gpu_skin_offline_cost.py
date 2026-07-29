#!/usr/bin/env python3
"""Offline cost model for the Warcraft III GPU-skin takeover.

This script never launches or attaches to Warcraft III.  It combines the
latest perf-report frame count with the cumulative GPU-skin diagnostics, then
benchmarks the exact classes of local-process memory operations used by the
native bridge.  The absolute ctypes timings are calibration data, not a C++
profiler; the operation counts and before/after amplification ratios are the
important outputs.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import platform
import re
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable


DEFAULT_REPORT_DIR = Path(r"E:\Work\War3\WarVK\Log")
DEFAULT_LOG = Path(r"E:\Work\War3\war3_d3d9.log")


@dataclass
class CaptureCounts:
    frames: int
    counter_frames: int
    avg_fps: float
    avg_frame_ms: float
    avg_main_thread_ms: float
    avg_gpu_ms: float
    flushes: int
    raw_uploads: int
    outside_uploads: int
    inside_uploads: int
    eligible_uploads: int
    candidate_jobs: int
    submitted_jobs: int
    compute_dispatches: int
    manager_fallback_events: int
    actual_vertices: int
    launched_vertices: int
    vertex_bucket_jobs: list[int]
    bypassed_kernels: int
    original_kernels: int
    original_kernel_bytes: int
    skipped_kernel_bytes: int
    skin_mode_0: int
    skin_mode_1: int
    poison_created: int
    poison_cleared: int
    normal_proof_attempts: int
    normal_proof_exact: int
    normal_proof_rejected: int


def latest_report(report_dir: Path) -> Path:
    reports = list(report_dir.glob("war3_perf_report_*.html"))
    if not reports:
        raise FileNotFoundError(f"no performance report in {report_dir}")
    return max(reports, key=lambda path: path.stat().st_mtime_ns)


def parse_report(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"\bconst\s+data\s*=\s*(\{.*?\})\s*;", text, re.S)
    if not match:
        raise ValueError(f"could not find embedded report JSON in {path}")
    return json.loads(match.group(1))


def last_match(text: str, pattern: str, label: str) -> re.Match[str]:
    matches = list(re.finditer(pattern, text, re.M))
    if not matches:
        raise ValueError(f"missing GPU-skin diagnostic: {label}")
    return matches[-1]


def parse_capture(report: dict[str, Any], log_text: str) -> CaptureCounts:
    top = last_match(
        log_text,
        r"War3GpuSkin: diag mode=\d+ flush=(\d+).*?upload=(\d+) "
        r"eligible=(\d+)",
        "mode/flush/upload",
    )
    format_line = last_match(
        log_text,
        r"War3GpuSkin: diag format=[^\r\n]*?skin="
        r"(\d+),(\d+),[^\r\n]*?nativeUpload raw=(\d+) outside=(\d+)",
        "format/nativeUpload",
    )
    compute = last_match(
        log_text,
        r"War3GpuSkin: diag compute batch=[^\r\n]*?jobs=(\d+)/(\d+) "
        r"dispatch=(\d+)/(\d+)[^\r\n]*?fallback=(\d+)",
        "compute",
    )
    efficiency = last_match(
        log_text,
        r"War3GpuSkin: diag dispatchEfficiency vertices=(\d+)/(\d+) "
        r"rounded=\d+/\d+ launched=(\d+)/(\d+)",
        "dispatchEfficiency",
    )
    bucket_line = last_match(
        log_text,
        r"War3GpuSkin: diag dispatchEfficiency[^\r\n]*?"
        r"bucketJobsPrepared=([0-9,]+)",
        "dispatchEfficiency vertex buckets",
    )
    vertex_bucket_jobs = [
        int(value) for value in bucket_line.group(1).split(",")
    ]
    if len(vertex_bucket_jobs) != 9:
        raise ValueError(
            "expected nine GPU-skin vertex buckets, got "
            f"{len(vertex_bucket_jobs)}"
        )
    native_safety = last_match(
        log_text,
        r"War3GpuSkin: diag nativeSafety[^\r\n]*?kernel=(\d+)/(\d+)/(\d+)"
        r"[^\r\n]*?kernelBytes=(\d+)/(\d+)",
        "nativeSafety",
    )
    poison = last_match(
        log_text,
        r"War3GpuSkin: diag nativePoison create=(\d+) clear=(\d+)",
        "nativePoison",
    )
    normal = last_match(
        log_text,
        r"War3GpuSkin: diag nativeKernelNormal returns=\d+ rejects=\d+ "
        r"proof=(\d+)/(\d+)/(\d+)",
        "nativeKernelNormal",
    )

    raw_uploads = int(format_line.group(3))
    outside_uploads = int(format_line.group(4))
    total_kernels = int(native_safety.group(1))
    original_kernels = int(native_safety.group(2))
    bypassed_kernels = int(native_safety.group(3))
    if total_kernels != raw_uploads:
        raise ValueError(
            f"kernel/upload contract does not close: {total_kernels} != {raw_uploads}"
        )
    if original_kernels + bypassed_kernels != total_kernels:
        raise ValueError("native kernel classification does not close")

    frames = int(report.get("frameCount", 0))
    if frames <= 0:
        raise ValueError("report has no frames")
    runtime_summary = report.get("shadowRuntimeV2Summary", {})
    counter_frames = int(runtime_summary.get("semanticCoreManifestFrameSerial", 0))
    if counter_frames <= 0:
        raise ValueError(
            "report lacks the process-lifetime frame serial required to normalize "
            "process-lifetime GPU-skin diagnostics"
        )
    return CaptureCounts(
        frames=frames,
        counter_frames=counter_frames,
        avg_fps=float(report.get("avgFps", 0.0)),
        avg_frame_ms=float(report.get("avgFrameTimeMs", 0.0)),
        avg_main_thread_ms=float(report.get("avgMainThreadCpuMs", 0.0)),
        avg_gpu_ms=float(report.get("avgGpuTimeMs", 0.0)),
        flushes=int(top.group(1)),
        raw_uploads=raw_uploads,
        outside_uploads=outside_uploads,
        inside_uploads=raw_uploads - outside_uploads,
        eligible_uploads=int(top.group(3)),
        candidate_jobs=int(compute.group(1)),
        submitted_jobs=int(compute.group(2)),
        compute_dispatches=int(compute.group(3)),
        manager_fallback_events=int(compute.group(5)),
        actual_vertices=int(efficiency.group(1)),
        launched_vertices=int(efficiency.group(3)),
        vertex_bucket_jobs=vertex_bucket_jobs,
        bypassed_kernels=bypassed_kernels,
        original_kernels=original_kernels,
        original_kernel_bytes=int(native_safety.group(4)),
        skipped_kernel_bytes=int(native_safety.group(5)),
        skin_mode_0=int(format_line.group(1)),
        skin_mode_1=int(format_line.group(2)),
        poison_created=int(poison.group(1)),
        poison_cleared=int(poison.group(2)),
        normal_proof_attempts=int(normal.group(1)),
        normal_proof_exact=int(normal.group(2)),
        normal_proof_rejected=int(normal.group(3)),
    )


def ns_per_call(action: Callable[[], None], iterations: int, repeats: int = 5) -> float:
    samples: list[float] = []
    for _ in range(repeats):
        start = time.perf_counter_ns()
        for _ in range(iterations):
            action()
        elapsed = time.perf_counter_ns() - start
        samples.append(elapsed / iterations)
    return statistics.median(samples)


def benchmark_windows_memory(iterations: int) -> dict[str, float]:
    if os.name != "nt":
        return {"available": 0.0}

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    process = kernel32.GetCurrentProcess()
    kernel32.ReadProcessMemory.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel32.ReadProcessMemory.restype = ctypes.c_int

    class MemoryBasicInformation(ctypes.Structure):
        _fields_ = [
            ("BaseAddress", ctypes.c_void_p),
            ("AllocationBase", ctypes.c_void_p),
            ("AllocationProtect", ctypes.c_ulong),
            ("PartitionId", ctypes.c_ushort),
            ("RegionSize", ctypes.c_size_t),
            ("State", ctypes.c_ulong),
            ("Protect", ctypes.c_ulong),
            ("Type", ctypes.c_ulong),
        ]

    kernel32.VirtualQuery.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(MemoryBasicInformation),
        ctypes.c_size_t,
    ]
    kernel32.VirtualQuery.restype = ctypes.c_size_t

    source = ctypes.create_string_buffer(4096)
    destination = ctypes.create_string_buffer(4096)
    bytes_read = ctypes.c_size_t()
    mbi = MemoryBasicInformation()
    src = ctypes.c_void_p(ctypes.addressof(source))
    dst = ctypes.c_void_p(ctypes.addressof(destination))

    def rpm(size: int) -> Callable[[], None]:
        def run() -> None:
            if not kernel32.ReadProcessMemory(process, src, dst, size, ctypes.byref(bytes_read)):
                raise ctypes.WinError(ctypes.get_last_error())
        return run

    def virtual_query() -> None:
        if not kernel32.VirtualQuery(src, ctypes.byref(mbi), ctypes.sizeof(mbi)):
            raise ctypes.WinError(ctypes.get_last_error())

    def memmove_1732() -> None:
        ctypes.memmove(dst, src, 0x6C4)

    return {
        "available": 1.0,
        "rpm_4_ns": ns_per_call(rpm(4), iterations),
        "rpm_8_ns": ns_per_call(rpm(8), iterations),
        "rpm_52_ns": ns_per_call(rpm(52), iterations),
        "rpm_232_ns": ns_per_call(rpm(0xE8), iterations),
        "rpm_1732_ns": ns_per_call(rpm(0x6C4), iterations),
        "virtual_query_ns": ns_per_call(virtual_query, iterations),
        "direct_memmove_1732_ns": ns_per_call(memmove_1732, iterations),
    }


def benchmark_skin_formula(vertex_count: int) -> dict[str, Any]:
    try:
        import numpy as np
    except ImportError:
        return {"available": False, "reason": "numpy is not installed"}

    n = max(64, min(vertex_count, 250_000))
    rng = np.random.default_rng(0x127A)
    positions = rng.normal(size=(n, 3)).astype(np.float32)
    normals = rng.normal(size=(n, 3)).astype(np.float32)
    normal_len = np.linalg.norm(normals, axis=1, keepdims=True)
    normals /= np.maximum(normal_len, np.float32(1e-20))
    slots = rng.integers(0, 192, size=n, dtype=np.uint8)
    palette = rng.normal(size=(192, 3, 4)).astype(np.float32)

    def vectorized() -> tuple[Any, Any]:
        selected = palette[slots]
        out_pos = np.einsum("nij,nj->ni", selected[:, :, :3], positions)
        out_pos += selected[:, :, 3]
        out_normal = np.einsum("nij,nj->ni", selected[:, :, :3], normals)
        return out_pos, out_normal

    out_pos, out_normal = vectorized()
    reference_count = min(n, 512)
    scalar_pos = np.empty((reference_count, 3), dtype=np.float32)
    scalar_normal = np.empty((reference_count, 3), dtype=np.float32)
    for i in range(reference_count):
        matrix = palette[int(slots[i])]
        scalar_pos[i] = matrix[:, :3] @ positions[i] + matrix[:, 3]
        scalar_normal[i] = matrix[:, :3] @ normals[i]
    max_position_error = float(np.max(np.abs(out_pos[:reference_count] - scalar_pos)))
    max_normal_error = float(np.max(np.abs(out_normal[:reference_count] - scalar_normal)))

    repeats = max(5, min(100, math.ceil(1_000_000 / n)))
    samples_ms = []
    for _ in range(repeats):
        start = time.perf_counter_ns()
        vectorized()
        samples_ms.append((time.perf_counter_ns() - start) / 1e6)
    return {
        "available": True,
        "vertices": n,
        "median_vectorized_ms": statistics.median(samples_ms),
        "p95_vectorized_ms": sorted(samples_ms)[max(0, math.ceil(0.95 * len(samples_ms)) - 1)],
        "max_position_error": max_position_error,
        "max_normal_error": max_normal_error,
        "formula": "out.xyz = palette[groupSlot].rows(3x4) * float4(input.xyz, 1); normal uses rows(3x3)",
    }


def operation_model(counts: CaptureCounts, memory: dict[str, float]) -> dict[str, Any]:
    # Current bridge lower bound: before/kernel/after GX snapshots, a geoset
    # header snapshot, global dword reads, and input range probes.  Manager map
    # work, locks, callbacks and proof content reads are deliberately excluded.
    current = {
        "gx_snapshots_1732": counts.raw_uploads * 3,
        "geoset_snapshots_232": counts.inside_uploads,
        "global_snapshots_52": counts.raw_uploads,
        # Positions/normals/groups match only for inside-scope geosets in this
        # capture; palette readability is checked on every fully observed call.
        "range_virtual_queries": counts.raw_uploads + counts.inside_uploads * 3,
        "normal_return_full_proofs": counts.normal_proof_attempts,
    }
    # Proposed production bypass tiers:
    #   T0 outside dispatch: counter/pairing only, no proof;
    #   T1 inside: one state snapshot to classify path/stage/skin;
    #   T2 eligible: range proof plus kernel-time refresh;
    #   T3 actual poison overlap: normal-return rewrite proof.
    optimized = {
        "gx_snapshots_1732": counts.inside_uploads + counts.eligible_uploads,
        "geoset_snapshots_232": counts.eligible_uploads,
        "global_snapshots_52": counts.eligible_uploads,
        "range_virtual_queries": counts.eligible_uploads * 4,
        "normal_return_full_proofs": counts.poison_created,
    }

    def calibrated_ns(ops: dict[str, int]) -> float | None:
        if not memory.get("available"):
            return None
        return (
            ops["gx_snapshots_1732"] * memory["rpm_1732_ns"]
            + ops["geoset_snapshots_232"] * memory["rpm_232_ns"]
            + ops["global_snapshots_52"] * memory["rpm_52_ns"]
            + ops["range_virtual_queries"] * memory["virtual_query_ns"]
        )

    current_ns = calibrated_ns(current)
    optimized_ns = calibrated_ns(optimized)
    ratio = None
    if current_ns is not None and current_ns > 0.0 and optimized_ns is not None:
        ratio = optimized_ns / current_ns
    def frame_bounds(total_ns: float | None) -> dict[str, float | None]:
        return {
            "lifetime_lower_bound_ms_per_frame": (
                None if total_ns is None
                else total_ns / counts.counter_frames / 1e6
            ),
            "report_window_upper_bound_ms_per_frame": (
                None if total_ns is None
                else total_ns / counts.frames / 1e6
            ),
        }

    return {
        "scope": "lower bound; excludes mutex/map/callback/proof-content costs",
        "current": current,
        "tiered_early_gate": optimized,
        "normalization": (
            "GPU-skin diagnostics are process-lifetime cumulative values. "
            "The exact report-window deltas were not captured, so both a "
            "lifetime/frameSerial lower bound and an all-events-in-report "
            "upper bound are reported."
        ),
        "calibrated_current_bounds": frame_bounds(current_ns),
        "calibrated_tiered_bounds": frame_bounds(optimized_ns),
        "calibrated_remaining_ratio": ratio,
        "full_proof_entry_reduction_pct": 100.0 * (
            1.0 - counts.eligible_uploads / max(counts.raw_uploads, 1)
        ),
        "normal_rewrite_proof_reduction_pct": 100.0 * (
            1.0 - counts.poison_created / max(counts.normal_proof_attempts, 1)
        ),
    }


def normalized_rates(counts: CaptureCounts, frames: int) -> dict[str, float]:
    return {
        "flushes_per_frame": counts.flushes / frames,
        "raw_uploads_per_frame": counts.raw_uploads / frames,
        "outside_uploads_per_frame": counts.outside_uploads / frames,
        "inside_uploads_per_frame": counts.inside_uploads / frames,
        "eligible_uploads_per_frame": counts.eligible_uploads / frames,
        "candidate_jobs_per_frame": counts.candidate_jobs / frames,
        "bypassed_kernels_per_frame": counts.bypassed_kernels / frames,
        "actual_vertices_per_frame": counts.actual_vertices / frames,
        "skipped_kernel_bytes_per_frame": counts.skipped_kernel_bytes / frames,
        "compute_dispatches_per_frame": counts.compute_dispatches / frames,
        "manager_fallback_events_per_frame": (
            counts.manager_fallback_events / frames
        ),
    }


def rates_and_selectivity(counts: CaptureCounts) -> dict[str, Any]:
    return {
        "normalization_note": (
            "Exact in-report rates require begin/end GPU-skin counter "
            "snapshots, which this report did not record."
        ),
        "lifetime_lower_bound": normalized_rates(counts, counts.counter_frames),
        "report_window_upper_bound": normalized_rates(counts, counts.frames),
        "exact_selectivity": {
        "outside_share_pct": 100.0 * counts.outside_uploads / counts.raw_uploads,
        "skin0_share_pct": 100.0 * counts.skin_mode_0 / counts.raw_uploads,
        "eligible_share_pct": 100.0 * counts.eligible_uploads / counts.raw_uploads,
        "bypass_share_pct": 100.0 * counts.bypassed_kernels / counts.raw_uploads,
        "launch_waste_pct": 100.0 * (
            counts.launched_vertices - counts.actual_vertices
        ) / max(counts.launched_vertices, 1),
        "proof_attempts_per_bypass": counts.normal_proof_attempts / max(counts.bypassed_kernels, 1),
        },
    }


def queue_admission_model(
    counts: CaptureCounts, memory: dict[str, float]
) -> dict[str, Any]:
    # The aggregate fallback counter includes failures outside prepareElement,
    # so this is explicitly a selectivity proxy, not an exact queue length.
    # It is still useful for comparing the old mandatory two VirtualQuery
    # probes with the new reverse-index-first + one 8-byte RPM admission path.
    proxy_entries = counts.manager_fallback_events + counts.candidate_jobs
    current_queries = proxy_entries * 2
    indexed_snapshots = counts.candidate_jobs
    current_ns = None
    indexed_ns = None
    if memory.get("available"):
        current_ns = current_queries * memory["virtual_query_ns"]
        indexed_ns = indexed_snapshots * memory["rpm_8_ns"]

    def bounds(total_ns: float | None) -> dict[str, float | None]:
        return {
            "lifetime_lower_bound_ms_per_frame": (
                None if total_ns is None
                else total_ns / counts.counter_frames / 1e6
            ),
            "report_window_upper_bound_ms_per_frame": (
                None if total_ns is None
                else total_ns / counts.frames / 1e6
            ),
        }

    return {
        "scope": (
            "selectivity proxy only: aggregate manager fallback events are "
            "not an exact prepareElement scan count"
        ),
        "proxy_entries": proxy_entries,
        "accepted_candidates": counts.candidate_jobs,
        "proxy_entries_per_accept": (
            proxy_entries / max(counts.candidate_jobs, 1)
        ),
        "old_two_virtual_queries": current_queries,
        "reverse_index_eight_byte_snapshots": indexed_snapshots,
        "old_calibrated_bounds": bounds(current_ns),
        "reverse_index_calibrated_bounds": bounds(indexed_ns),
        "calibrated_remaining_ratio": (
            None if current_ns in (None, 0.0) or indexed_ns is None
            else indexed_ns / current_ns
        ),
    }


def vertex_bucket_range(bucket: int) -> tuple[int, int]:
    """Return the exact vertex interval for floor(log2(ceil(v/64)))."""
    if bucket < 0 or bucket >= 9:
        raise ValueError(f"invalid vertex bucket {bucket}")
    group_min = 1 if bucket == 0 else 1 << bucket
    group_max = (1 << (bucket + 1)) - 1
    vertex_min = (group_min - 1) * 64 + 1
    vertex_max = min(group_max * 64, 16_384)
    return vertex_min, vertex_max


def hybrid_threshold_model(counts: CaptureCounts) -> dict[str, Any]:
    """Bound hybrid CPU/GPU routing using the exact logged bucket histogram.

    The log has exact job counts and a total vertex count, but not per-bucket
    vertex totals.  For each bucket-aligned threshold we therefore solve tight
    retained-vertex bounds under both constraints instead of inventing an
    average model.
    """
    bucket_jobs = counts.vertex_bucket_jobs
    if sum(bucket_jobs) != counts.candidate_jobs:
        raise ValueError(
            "vertex bucket/job contract does not close: "
            f"{sum(bucket_jobs)} != {counts.candidate_jobs}"
        )

    ranges = [vertex_bucket_range(index) for index in range(len(bucket_jobs))]
    total_min = sum(
        jobs * ranges[index][0] for index, jobs in enumerate(bucket_jobs)
    )
    total_max = sum(
        jobs * ranges[index][1] for index, jobs in enumerate(bucket_jobs)
    )
    if not total_min <= counts.actual_vertices <= total_max:
        raise ValueError(
            "actual vertex count is outside the logged bucket envelope: "
            f"{total_min} <= {counts.actual_vertices} <= {total_max}"
        )

    scenarios: list[dict[str, Any]] = []
    thresholds = [ranges[index][0] for index in range(len(ranges))]
    for first_retained_bucket, threshold in enumerate(thresholds):
        retained_jobs = sum(bucket_jobs[first_retained_bucket:])
        retained_min = sum(
            bucket_jobs[index] * ranges[index][0]
            for index in range(first_retained_bucket, len(bucket_jobs))
        )
        retained_max = sum(
            bucket_jobs[index] * ranges[index][1]
            for index in range(first_retained_bucket, len(bucket_jobs))
        )
        excluded_min = total_min - retained_min
        excluded_max = total_max - retained_max
        retained_lower = max(
            retained_min, counts.actual_vertices - excluded_max
        )
        retained_upper = min(
            retained_max, counts.actual_vertices - excluded_min
        )
        scenarios.append(
            {
                "minimum_gpu_vertices": threshold,
                "first_retained_bucket": first_retained_bucket,
                "retained_jobs": retained_jobs,
                "retained_job_pct": (
                    100.0 * retained_jobs / max(counts.candidate_jobs, 1)
                ),
                "retained_vertices_lower": retained_lower,
                "retained_vertices_upper": retained_upper,
                "retained_vertex_pct_lower": (
                    100.0 * retained_lower / max(counts.actual_vertices, 1)
                ),
                "retained_vertex_pct_upper": (
                    100.0 * retained_upper / max(counts.actual_vertices, 1)
                ),
            }
        )

    return {
        "bucket_contract": (
            "bucket=floor(log2(ceil(vertices/64))); intervals and job counts "
            "are exact, retained vertex totals are tight bounds"
        ),
        "bucket_ranges": [
            {
                "bucket": index,
                "min_vertices": ranges[index][0],
                "max_vertices": ranges[index][1],
                "jobs": bucket_jobs[index],
            }
            for index in range(len(bucket_jobs))
        ],
        "actual_vertices": counts.actual_vertices,
        "bucket_vertex_envelope": {
            "minimum": total_min,
            "maximum": total_max,
        },
        "threshold_scenarios": scenarios,
        "routing_note": (
            "A production threshold only saves admission/proof work when it "
            "is applied before static-source hashing, palette upload and GPU "
            "job construction; a late dispatch-only filter is insufficient."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path)
    parser.add_argument("--report-dir", type=Path, default=DEFAULT_REPORT_DIR)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--iterations", type=int, default=50_000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report_path = args.report or latest_report(args.report_dir)
    report = parse_report(report_path)
    log_text = args.log.read_text(encoding="utf-8", errors="replace")
    counts = parse_capture(report, log_text)
    memory = benchmark_windows_memory(max(1_000, args.iterations))
    lower_vertices = max(
        64, round(counts.actual_vertices / counts.counter_frames)
    )
    upper_vertices = max(64, round(counts.actual_vertices / counts.frames))
    skin_formula = {
        "lifetime_lower_bound": benchmark_skin_formula(lower_vertices),
        "report_window_upper_bound": benchmark_skin_formula(upper_vertices),
    }
    result = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "host": {
            "python": sys.version,
            "platform": platform.platform(),
        },
        "inputs": {
            "report": str(report_path),
            "log": str(args.log),
        },
        "capture": asdict(counts),
        "per_frame_rate_bounds_and_selectivity": rates_and_selectivity(counts),
        "local_process_memory_microbench": memory,
        "operation_model": operation_model(counts, memory),
        "queue_admission_model": queue_admission_model(counts, memory),
        "hybrid_cpu_gpu_threshold_model": hybrid_threshold_model(counts),
        "war3_group_palette_formula_benchmark": skin_formula,
        "conclusion": [
            "The native SSE kernel is not the dominant candidate: only the bypass-share of uploads and roughly the listed vertices/frame are saved.",
            "GPU-skin counters are process-lifetime values; without begin/end snapshots the true report-window rate lies between the reported normalization scenarios.",
            "The bridge currently performs full observation/proof work before learning that most uploads are outside the dispatch scope or skinMode 0.",
            "The producer also scans sparse render queues; a learned renderable/layer reverse index can reject unknown entries before native-memory probes.",
            "Production bypass should use fail-closed T0/T1/T2 gates and only run CPU-rewrite proof when the current output range can overlap a live poison range.",
            "ctypes absolute timings include Python FFI overhead; use the operation ratios, not those milliseconds, as the implementation target.",
        ],
    }

    if args.output is None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_dir = Path(__file__).resolve().parent / "artifacts" / f"gpu_skin_offline_cost_{stamp}"
        output_path = output_dir / "result.json"
    else:
        output_path = args.output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")

    print(json.dumps(result, indent=2, ensure_ascii=False))
    print(f"\nartifact={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
