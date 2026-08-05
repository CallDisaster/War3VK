#!/usr/bin/env python3
"""Offline model for Warcraft III GPU-skin small-job batching.

This tool is deliberately read-only with respect to Warcraft III.  It parses a
completed P4 artifact, combines it with the exact historical all-job histogram
recorded in research issue 28, and emits only algebraic bounds.  It does not
launch, attach to, build, or deploy the game.

The aggregate artifact does not contain individual small-job vertex counts or
per-flush membership.  Results that need those data are explicitly marked as
unknown instead of being filled with an assumed distribution.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACT = REPO_ROOT / (
    "AutoTest/artifacts/"
    "gpu_skin_p4_crash_gate_isolated_diag_light_"
    "outside_admission_v2_tracking_health_light_v1_20260715_073406/"
    "p4_result.json"
)

LOCAL_SIZE_64 = 64
LOCAL_SIZE_32 = 32
MAX_JOBS_PER_BATCH = 512
BUCKET_RANGES = (
    (1, 64),
    (65, 192),
    (193, 448),
    (449, 960),
    (961, 1984),
    (1985, 4032),
    (4033, 8128),
    (8129, 16320),
    (16321, 16384),
)

# Exact all-job capture quoted in issue 28 section 19 and the manager source
# comment.  It predates the 449 production threshold, so it is the only current
# evidence that includes the rejected 1..448 population.
HISTORICAL_JOB_COUNTS = (344, 0, 2908, 1252, 0, 0, 0, 0, 0)
HISTORICAL_ACTUAL_VERTICES = 1_908_631
HISTORICAL_DISPATCHES = 451


@dataclass(frozen=True)
class TimingSample:
    calls: int
    ticks: int
    max_ticks: int
    frequency: int

    @property
    def average_us(self) -> float | None:
        if self.calls == 0 or self.frequency == 0:
            return None
        return self.ticks * 1_000_000.0 / (self.calls * self.frequency)


def pct(numerator: int | float, denominator: int | float) -> float:
    return 0.0 if denominator == 0 else 100.0 * numerator / denominator


def round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def round_down(value: int, alignment: int) -> int:
    return value // alignment * alignment


def require_match(text: str, pattern: str, label: str) -> re.Match[str]:
    match = re.search(pattern, text)
    if match is None:
        raise ValueError(f"missing {label}: {text}")
    return match


def parse_triplet(line: str, name: str, frequency: int) -> TimingSample:
    match = require_match(
        line,
        rf"(?:^|\s){re.escape(name)}=(\d+)/(\d+)/(\d+)(?:\s|$)",
        name,
    )
    return TimingSample(*(int(value) for value in match.groups()), frequency)


def tight_subset_vertex_bounds(
    retained: Iterable[int],
    counts: tuple[int, ...] = HISTORICAL_JOB_COUNTS,
    total_vertices: int = HISTORICAL_ACTUAL_VERTICES,
) -> tuple[int, int]:
    retained_set = set(retained)
    retained_min = sum(
        counts[index] * BUCKET_RANGES[index][0] for index in retained_set
    )
    retained_max = sum(
        counts[index] * BUCKET_RANGES[index][1] for index in retained_set
    )
    excluded_min = sum(
        counts[index] * BUCKET_RANGES[index][0]
        for index in range(len(counts))
        if index not in retained_set
    )
    excluded_max = sum(
        counts[index] * BUCKET_RANGES[index][1]
        for index in range(len(counts))
        if index not in retained_set
    )
    lower = max(retained_min, total_vertices - excluded_max)
    upper = min(retained_max, total_vertices - excluded_min)
    if lower > upper:
        raise ValueError(
            f"infeasible histogram envelope for buckets {sorted(retained_set)}"
        )
    return lower, upper


def threshold_scenario(first_bucket: int) -> dict[str, Any]:
    retained = tuple(range(first_bucket, len(HISTORICAL_JOB_COUNTS)))
    jobs = sum(HISTORICAL_JOB_COUNTS[index] for index in retained)
    lower, upper = tight_subset_vertex_bounds(retained)
    return {
        "minimumGpuVertices": BUCKET_RANGES[first_bucket][0],
        "firstBucket": first_bucket,
        "retainedJobs": jobs,
        "retainedJobPct": pct(jobs, sum(HISTORICAL_JOB_COUNTS)),
        "retainedVerticesLower": lower,
        "retainedVerticesUpper": upper,
        "retainedVertexPctLower": pct(lower, HISTORICAL_ACTUAL_VERTICES),
        "retainedVertexPctUpper": pct(upper, HISTORICAL_ACTUAL_VERTICES),
    }


def parse_runtime(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    raw = document["diagnostics"]["rawLatest"]

    fast = require_match(
        raw["nativeFast"],
        r"reject=(\d+)/(\d+)/(\d+)/(\d+) smallCpu=(\d+) .*?"
        r"unknownState=(\d+).*?candidate=(\d+)",
        "nativeFast partition",
    )
    (
        reject_scope,
        reject_state,
        reject_skin,
        reject_input,
        reject_small,
        unknown_state,
        candidates,
    ) = (int(value) for value in fast.groups())

    safety = require_match(
        raw["nativeSafety"],
        r"kernel=(\d+)/(\d+)/(\d+).*?kernelBytes=(\d+)/(\d+)",
        "nativeSafety kernel accounting",
    )
    (
        kernel_calls,
        original_calls,
        bypassed_calls,
        original_bytes,
        bypassed_bytes,
    ) = (int(value) for value in safety.groups())

    compute = require_match(
        raw["compute"],
        r"batch=(\d+)/(\d+)/(\d+) jobs=(\d+)/(\d+) "
        r"dispatch=(\d+)/(\d+) palette=(\d+)/(\d+)",
        "compute accounting",
    )
    compute_values = [int(value) for value in compute.groups()]

    efficiency = require_match(
        raw["dispatchEfficiency"],
        r"vertices=(\d+)/(\d+) rounded=(\d+)/(\d+) "
        r"launched=(\d+)/(\d+) tailWaste=(\d+)/(\d+) "
        r"crossJobWaste=(\d+)/(\d+) bucketJobsPrepared=([0-9,]+)",
        "dispatch efficiency",
    )
    efficiency_values = [int(value) for value in efficiency.groups()[:10]]
    bucket_jobs = [int(value) for value in efficiency.group(11).split(",")]

    dedup = require_match(
        raw["paletteDedup"],
        r"candidates=(\d+) unique=(\d+) hits=(\d+) bytesSaved=(\d+)",
        "palette dedup",
    )
    dedup_values = [int(value) for value in dedup.groups()]

    consume = require_match(
        raw["consume"],
        r"bypass=(\d+)/(\d+)/(\d+)/(\d+)",
        "bypass result partition",
    )
    bypass_values = [int(value) for value in consume.groups()]

    quiescence = require_match(
        raw["quiescencePost"],
        r"frame=(\d+) flush=(\d+)",
        "quiescent frame/flush",
    )
    frame_serial, flush_serial = (int(value) for value in quiescence.groups())

    begin_line = raw["nativeBeginSample"]
    begin_frequency = int(
        require_match(raw["nativeT2Sample"], r"freq=(\d+)", "QPC frequency").group(1)
    )
    begin_small = parse_triplet(begin_line, "smallRoute", begin_frequency)
    begin_candidate = parse_triplet(begin_line, "candidateRoute", begin_frequency)
    t2_line = raw["nativeT2Sample"]
    t2_names = (
        "geoSnap",
        "geoHeader",
        "posProof",
        "normalProof",
        "groupProof",
        "paletteProof",
    )
    t2_samples = {name: parse_triplet(t2_line, name, begin_frequency) for name in t2_names}
    kernel_line = raw["nativeProdKernel"]
    kernel_frequency = int(
        require_match(kernel_line, r"freq=(\d+)", "kernel QPC frequency").group(1)
    )
    sampled_original_kernel = parse_triplet(
        kernel_line, "original", kernel_frequency
    )

    partition = {
        "scopeReject": reject_scope,
        "stateReject": reject_state,
        "skinFormatReject": reject_skin,
        "inputReject": reject_input,
        "smallCpuPreferred": reject_small,
        "largeCandidate": candidates,
    }
    if sum(partition.values()) != kernel_calls:
        raise ValueError("native fast partition does not close")
    if original_calls + bypassed_calls != kernel_calls:
        raise ValueError("kernel result partition does not close")
    if efficiency_values[0] != efficiency_values[1]:
        raise ValueError("prepared/submitted actual vertices disagree")
    if efficiency_values[2] != efficiency_values[3]:
        raise ValueError("prepared/submitted rounded invocations disagree")
    if efficiency_values[4] != efficiency_values[5]:
        raise ValueError("prepared/submitted launched invocations disagree")
    if len(bucket_jobs) != len(BUCKET_RANGES):
        raise ValueError("dispatch histogram does not have nine buckets")

    return {
        "artifact": str(path.resolve()),
        "artifactLabel": document.get("artifact", ""),
        "frameSerial": frame_serial,
        "flushSerial": flush_serial,
        "kernelCalls": kernel_calls,
        "originalKernelCalls": original_calls,
        "bypassedKernelCalls": bypassed_calls,
        "originalKernelBytes": original_bytes,
        "bypassedKernelBytes": bypassed_bytes,
        "partition": partition,
        "partitionClosed": True,
        "unknownStateDiagnostic": unknown_state,
        "preThresholdSkinFormatEligible": reject_small + candidates,
        "compute": {
            "batchesPrepared": compute_values[0],
            "batchesSubmitted": compute_values[1],
            "batchFallbacks": compute_values[2],
            "jobsPrepared": compute_values[3],
            "jobsSubmitted": compute_values[4],
            "dispatchesPrepared": compute_values[5],
            "dispatchesSubmitted": compute_values[6],
            "palettePrepared": compute_values[7],
            "paletteSubmitted": compute_values[8],
            "actualVertices": efficiency_values[0],
            "roundedInvocations": efficiency_values[2],
            "launchedInvocations": efficiency_values[4],
            "tailWaste": efficiency_values[6],
            "crossJobWaste": efficiency_values[8],
            "bucketJobsPrepared": bucket_jobs,
        },
        "paletteDedup": {
            "candidates": dedup_values[0],
            "unique": dedup_values[1],
            "hits": dedup_values[2],
            "bytesSaved": dedup_values[3],
            "policyNote": (
                "Bypass intentionally disables content hash/dedup. hits=0 is not "
                "evidence that palettes are unique."
            ),
        },
        "bypass": {
            "attempts": bypass_values[0],
            "authorizations": bypass_values[1],
            "commits": bypass_values[2],
            "fallbacks": bypass_values[3],
        },
        "sampledCpuCosts": {
            "frequency": begin_frequency,
            "smallRoute": asdict(begin_small) | {"averageUs": begin_small.average_us},
            "candidateRoute": asdict(begin_candidate)
            | {"averageUs": begin_candidate.average_us},
            "candidateMinusSmallAverageUs": (
                begin_candidate.average_us - begin_small.average_us
                if begin_candidate.average_us is not None
                and begin_small.average_us is not None
                else None
            ),
            "t2Stages": {
                name: asdict(sample) | {"averageUs": sample.average_us}
                for name, sample in t2_samples.items()
            },
            "originalKernelAllRouteSample": asdict(sampled_original_kernel)
            | {"averageUs": sampled_original_kernel.average_us},
            "warning": (
                "Samples are prime-period route observations. The original-kernel "
                "sample mixes all routes and is not a small-SSE timing."
            ),
        },
    }


def build_model(runtime: dict[str, Any]) -> dict[str, Any]:
    compute = runtime["compute"]
    actual = compute["actualVertices"]
    rounded64 = compute["roundedInvocations"]
    launched64 = compute["launchedInvocations"]
    jobs = compute["jobsPrepared"]
    dispatches = compute["dispatchesSubmitted"]
    if rounded64 - actual != compute["tailWaste"]:
        raise ValueError("tail-waste arithmetic does not close")
    if launched64 - rounded64 != compute["crossJobWaste"]:
        raise ValueError("cross-job-waste arithmetic does not close")

    rounded32_lower = round_up(actual, LOCAL_SIZE_32)
    rounded32_upper = min(
        rounded64,
        round_down(actual + (LOCAL_SIZE_32 - 1) * jobs, LOCAL_SIZE_32),
    )
    if rounded32_lower > rounded32_upper:
        raise ValueError("local-size-32 envelope is infeasible")

    threshold_449 = threshold_scenario(3)
    threshold_193 = threshold_scenario(2)
    threshold_1 = threshold_scenario(0)
    bucket0_lower, bucket0_upper = tight_subset_vertex_bounds((0,))
    separate_micro_invocations = HISTORICAL_JOB_COUNTS[0] * LOCAL_SIZE_64
    packed_micro_min = round_up(bucket0_lower, LOCAL_SIZE_64)
    packed_micro_max = round_up(bucket0_upper, LOCAL_SIZE_64)

    small_route = runtime["sampledCpuCosts"]["smallRoute"]["averageUs"]
    candidate_route = runtime["sampledCpuCosts"]["candidateRoute"]["averageUs"]
    incremental_us = candidate_route - small_route
    current_small_calls = runtime["partition"]["smallCpuPreferred"]
    extrapolated_lifetime_ms = current_small_calls * incremental_us / 1000.0
    extrapolated_per_frame_ms = extrapolated_lifetime_ms / runtime["frameSerial"]

    return {
        "schemaVersion": 1,
        "scope": {
            "offlineOnly": True,
            "buildPerformed": False,
            "deployPerformed": False,
            "war3Launched": False,
            "productionCppModified": False,
        },
        "sources": {
            "latestLightArtifact": runtime["artifact"],
            "historicalHistogram": (
                "docs/research/war3_render_issues/28_gpu_skinning_takeover_"
                "feasibility/README.md section 19"
            ),
            "historicalHistogramCounts": list(HISTORICAL_JOB_COUNTS),
            "historicalActualVertices": HISTORICAL_ACTUAL_VERTICES,
            "historicalDispatches": HISTORICAL_DISPATCHES,
        },
        "runtimeExact": runtime,
        "current449DispatchEfficiency": {
            "jobs": jobs,
            "dispatches": dispatches,
            "jobsPerDispatch": jobs / dispatches,
            "actualVertices": actual,
            "roundedInvocations": rounded64,
            "launchedInvocations": launched64,
            "tailWaste": compute["tailWaste"],
            "crossJobWaste": compute["crossJobWaste"],
            "totalWaste": launched64 - actual,
            "activeLaneUtilizationPct": pct(actual, launched64),
            "tailWastePctOfLaunched": pct(compute["tailWaste"], launched64),
            "crossJobWastePctOfLaunched": pct(
                compute["crossJobWaste"], launched64
            ),
            "currentLaunchedWorkgroups64": launched64 // LOCAL_SIZE_64,
            "interpretation": (
                "The latest run contains bucket 3 only. Most padding is cross-job "
                "padding inside the wide 8..15-workgroup bucket, not per-job tail."
            ),
        },
        "thresholdScenariosHistoricalExact": {
            "threshold449": threshold_449,
            "threshold193": threshold_193,
            "threshold1": threshold_1,
            "threshold193DeltaVs449": {
                "additionalJobs": (
                    threshold_193["retainedJobs"] - threshold_449["retainedJobs"]
                ),
                "gpuJobMultiplier": (
                    threshold_193["retainedJobs"] / threshold_449["retainedJobs"]
                ),
                "vertexCoverageGainPctLower": (
                    threshold_193["retainedVertexPctLower"]
                    - threshold_449["retainedVertexPctUpper"]
                ),
                "vertexCoverageGainPctUpper": (
                    threshold_193["retainedVertexPctUpper"]
                    - threshold_449["retainedVertexPctLower"]
                ),
            },
            "proofBoundary": (
                "These are tight bounds for the historical 4504-job capture only. "
                "They cannot be applied as the exact split of the latest 66960 "
                "smallCpu calls without a current per-route vertex histogram."
            ),
        },
        "localSize32EnvelopeOnLatestExactLargeJobs": {
            "perJobRoundedInvocationsLower": rounded32_lower,
            "perJobRoundedInvocationsUpper": rounded32_upper,
            "roundedInvocationSavingLower": rounded64 - rounded32_upper,
            "roundedInvocationSavingUpper": rounded64 - rounded32_lower,
            "roundedInvocationSavingPctLower": pct(
                rounded64 - rounded32_upper, rounded64
            ),
            "roundedInvocationSavingPctUpper": pct(
                rounded64 - rounded32_lower, rounded64
            ),
            "noCrossJobWorkgroups32Lower": rounded32_lower // LOCAL_SIZE_32,
            "noCrossJobWorkgroups32Upper": rounded32_upper // LOCAL_SIZE_32,
            "currentLaunchedWorkgroups64": launched64 // LOCAL_SIZE_64,
            "minimumWorkgroupIncreaseVsCurrentLaunchedPct": pct(
                rounded32_lower // LOCAL_SIZE_32,
                launched64 // LOCAL_SIZE_64,
            )
            - 100.0,
            "cannotDerive": (
                "The new launched-invocation total and dispatch count require the "
                "per-job, per-flush vertex sequence because local_size=32 changes "
                "bucket membership and batch maxima."
            ),
            "decision": (
                "Do not change local_size first. Even an impossible no-cross-job "
                "best case executes more workgroups; split the wide 64-lane bucket "
                "or seal CPU admission before paying twice as many barriers/job-cache loads."
            ),
        },
        "microPackBucket0HistoricalEnvelope": {
            "jobs": HISTORICAL_JOB_COUNTS[0],
            "actualVerticesLower": bucket0_lower,
            "actualVerticesUpper": bucket0_upper,
            "actualVertexSharePctLower": pct(
                bucket0_lower, HISTORICAL_ACTUAL_VERTICES
            ),
            "actualVertexSharePctUpper": pct(
                bucket0_upper, HISTORICAL_ACTUAL_VERTICES
            ),
            "currentSeparateJobInvocations": separate_micro_invocations,
            "perfectGlobalPackInvocationsLower": packed_micro_min,
            "perfectGlobalPackInvocationsUpper": packed_micro_max,
            "invocationSavingLower": separate_micro_invocations - packed_micro_max,
            "invocationSavingUpper": separate_micro_invocations - packed_micro_min,
            "workgroupsSavedLower": (
                HISTORICAL_JOB_COUNTS[0] - packed_micro_max // LOCAL_SIZE_64
            ),
            "workgroupsSavedUpper": (
                HISTORICAL_JOB_COUNTS[0] - packed_micro_min // LOCAL_SIZE_64
            ),
            "maximumSavingVsAllHistoricalVerticesPct": pct(
                separate_micro_invocations - packed_micro_min,
                HISTORICAL_ACTUAL_VERTICES,
            ),
            "ledgerBoundary": (
                "Packing may share a compute workgroup only. Every model keeps its "
                "own output slice, token, DIP signature, poison range, reservation, "
                "and consumer settlement ledger."
            ),
            "decision": (
                "Defer. Bucket 0 is at most 1.153% of historical vertices, and "
                "micro-pack does not remove per-job CPU authorization costs."
            ),
        },
        "fixedCpuCostObservation": {
            "smallRouteAverageUs": small_route,
            "candidateRouteAverageUs": candidate_route,
            "candidateMinusSmallAverageUs": incremental_us,
            "illustrativeAllLatestSmallCallsExtraLifetimeMs": (
                extrapolated_lifetime_ms
            ),
            "illustrativeAllLatestSmallCallsExtraMsPerFrameSerial": (
                extrapolated_per_frame_ms
            ),
            "strictCaveat": (
                "The extrapolation shows scale only. It assumes every current small "
                "call becomes a candidate with the sampled mean and excludes manager, "
                "GPU, and saved SSE time; it is not an FPS prediction."
            ),
            "sharedProofFormula": (
                "For J jobs, U distinct exact palette snapshots, and S distinct exact "
                "static resources: shared work scales with U+S while mappedDst/ring/"
                "index/normal-return/poison/ledger checks remain J."
            ),
            "paletteEvidence": runtime["paletteDedup"]["policyNote"],
        },
        "minimumCppDesign": {
            "phaseARequiredTelemetry": [
                "For every mutually exclusive native route, add calls + vertexCount + outputBytes; split small into buckets 0/1/2.",
                "For each flush/bucket, add job count, actual vertices, palette bytes, exact palette-owner key count, static-resource key count, and submitted dispatch count.",
                "Sample original Game.dll kernel ticks by the same vertex bucket; the existing all-route original sample is insufficient.",
                "Record per-batch max group count and a compact group-count histogram (1..256) so cross-job padding can be replayed exactly.",
            ],
            "phaseBFlushSeal": [
                "Build one immutable flush seal keyed by flush/dispatch epoch plus map/device/reset generation.",
                "Share only exact immutable static-resource proof and exact copied palette snapshots; use O(1) indices from sealed jobs.",
                "Keep every job's output slice, native token, expected DIP signature, index ticket, poison range, and consumer ledger independent.",
                "At the kernel hook, perform O(1) sealed-job lookup and revalidate mappedDst/ring/current palette generation before poison-create and irreversible skip.",
                "Lower the production threshold from 449 to 193 only after the seal closes and new route bytes/ticks show a positive crossover.",
            ],
            "phaseCCompute": [
                "Keep local_size_x=64 initially.",
                "First split bucket 3's 8..15 group-count span into narrower sub-batches if the new per-flush trace confirms the current 34.3% cross-job waste is stable.",
                "Only then prototype bucket-0 micro-pack with a prefix/segment table; scatter to the original independent output slices.",
            ],
        },
        "evidenceBackedConclusions": [
            "The latest native route partition closes exactly at 1,121,900 calls.",
            "The latest 449 path launches 11,176,384 lanes for 6,940,763 vertices; cross-job waste dominates tail waste.",
            "In the historical all-job capture, threshold 193 retains 92.36% of jobs and a tight 98.85%..99.98% of vertices.",
            "Bucket-0 perfect micro-pack can save at most 21,632 lanes, only 1.13% of historical actual vertices.",
            "The latest aggregate cannot prove that the current 66,960 small calls have the historical bucket distribution or that palette/proof reuse exists.",
        ],
        "unknownsBlockingAnFpsClaim": [
            "Current bytes and vertices for small buckets 0/1/2.",
            "Bucket-specific Game.dll SSE kernel time and output format.",
            "Per-flush job sequence and maxima after threshold 193.",
            "Exact palette-owner/static-resource reuse cardinality in Bypass mode.",
            "GPU timestamp cost per bucket/dispatch; whole-frame GPU time cannot isolate skin compute.",
        ],
    }


def self_test() -> None:
    assert sum(HISTORICAL_JOB_COUNTS) == 4504
    assert tight_subset_vertex_bounds((3,)) == (583_831, 1_201_920)
    assert tight_subset_vertex_bounds((2, 3)) == (1_886_615, 1_908_287)
    assert tight_subset_vertex_bounds((0,)) == (344, 22_016)
    assert threshold_scenario(3)["retainedJobs"] == 1252
    assert threshold_scenario(2)["retainedJobs"] == 4160
    if DEFAULT_ARTIFACT.exists():
        runtime = parse_runtime(DEFAULT_ARTIFACT)
        assert runtime["kernelCalls"] == 1_121_900
        assert runtime["partition"]["smallCpuPreferred"] == 66_960
        assert runtime["compute"]["actualVertices"] == 6_940_763
        model = build_model(runtime)
        assert model["current449DispatchEfficiency"]["totalWaste"] == 4_235_621


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", type=Path, default=DEFAULT_ARTIFACT)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stdout", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        if args.output is None and not args.stdout:
            print("gpu-skin small-job batching offline self-test: PASS")
            return 0
    runtime = parse_runtime(args.artifact)
    model = build_model(runtime)
    payload = json.dumps(model, ensure_ascii=False, indent=2) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")
    if args.stdout or args.output is None:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
