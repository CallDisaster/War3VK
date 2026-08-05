#!/usr/bin/env python3
"""Warcraft III CPU skin MT producer 的纯离线合同与成本模型。

该脚本不启动/附加 Warcraft III，不创建子进程，也不把 Python wall time
外推为 C++ 性能。它只做三件事：

1. 按 Game.dll 已确认的 float32 运算顺序生成 0..5 六种 FVF；
2. 验证 coarse multi-job / disjoint-range / generation-cancel 合同；
3. 对 freeze+enqueue+barrier+batch-upload 固定成本做参数敏感性分析。
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


FVF_STRIDES = (24, 28, 32, 36, 40, 44)
MAX_NATIVE_VERTICES = 16_384
PALETTE_FLOATS = 12
MXCSR_CONTROL_MASK = 0xFFC0
MXCSR_EXCEPTION_MASK = 0x1F80
MAX_CANCEL_CHECK_VERTICES = 64


def f32(value: float) -> float:
    """每一步都收敛为 IEEE754 binary32，禁止 Python double 隐式重排。"""

    return struct.unpack("<f", struct.pack("<f", value))[0]


def fmul(lhs: float, rhs: float) -> float:
    return f32(f32(lhs) * f32(rhs))


def fadd(lhs: float, rhs: float) -> float:
    return f32(f32(lhs) + f32(rhs))


def transform3(
    matrix: Sequence[float], vector: Sequence[float], translation: bool
) -> tuple[float, float, float]:
    """复刻 native/compute 的 parenthesized mul/add，不 normalize、不 FMA。"""

    x01 = fadd(fmul(matrix[0], vector[0]), fmul(matrix[3], vector[1]))
    x2 = fadd(x01, fmul(matrix[6], vector[2]))
    y01 = fadd(fmul(matrix[1], vector[0]), fmul(matrix[4], vector[1]))
    y2 = fadd(y01, fmul(matrix[7], vector[2]))
    z01 = fadd(fmul(matrix[2], vector[0]), fmul(matrix[5], vector[1]))
    z2 = fadd(z01, fmul(matrix[8], vector[2]))
    if translation:
        x2 = fadd(x2, matrix[9])
        y2 = fadd(y2, matrix[10])
        z2 = fadd(z2, matrix[11])
    return x2, y2, z2


@dataclass(frozen=True)
class StaticInputs:
    positions: tuple[tuple[float, float, float], ...]
    normals: tuple[tuple[float, float, float], ...]
    group_slots: tuple[int, ...]
    uv0: tuple[tuple[float, float], ...]
    uv1: tuple[tuple[float, float], ...]
    diffuse: tuple[int, ...] | None

    @property
    def vertex_count(self) -> int:
        return len(self.positions)


def encode_vertex(
    static: StaticInputs,
    palette: Sequence[Sequence[float]],
    output_format: int,
    vertex: int,
) -> bytes:
    if output_format < 0 or output_format >= len(FVF_STRIDES):
        raise ValueError("invalid FVF format")
    if vertex < 0 or vertex >= static.vertex_count:
        raise ValueError("invalid vertex")
    slot = static.group_slots[vertex]
    if slot < 0 or slot >= len(palette):
        raise ValueError("group slot outside palette")
    matrix = palette[slot]
    if len(matrix) != PALETTE_FLOATS:
        raise ValueError("palette row is not 3x4")

    position = transform3(matrix, static.positions[vertex], True)
    normal = transform3(matrix, static.normals[vertex], False)
    result = bytearray(struct.pack("<6f", *(position + normal)))
    if output_format & 1:
        diffuse = 0xFFFFFFFF if static.diffuse is None else static.diffuse[vertex]
        result += struct.pack("<I", diffuse & 0xFFFFFFFF)
    uv_layers = output_format // 2
    if uv_layers >= 1:
        result += struct.pack("<2f", *static.uv0[vertex])
    if uv_layers >= 2:
        result += struct.pack("<2f", *static.uv1[vertex])
    assert len(result) == FVF_STRIDES[output_format]
    return bytes(result)


def encode_range(
    static: StaticInputs,
    palette: Sequence[Sequence[float]],
    output_format: int,
    first: int,
    count: int,
    destination: bytearray,
) -> None:
    stride = FVF_STRIDES[output_format]
    if first < 0 or count <= 0 or first + count > static.vertex_count:
        raise ValueError("invalid range")
    if len(destination) < static.vertex_count * stride:
        raise ValueError("short output")
    for vertex in range(first, first + count):
        begin = vertex * stride
        destination[begin : begin + stride] = encode_vertex(
            static, palette, output_format, vertex
        )


def make_fixture(vertex_count: int = 257, group_count: int = 7) -> tuple[
    StaticInputs, tuple[tuple[float, ...], ...]
]:
    positions = []
    normals = []
    groups = []
    uv0 = []
    uv1 = []
    diffuse = []
    for i in range(vertex_count):
        positions.append(
            (f32(i * 0.03125 - 3.0), f32((i % 17) * -0.0625), f32(i / 97.0))
        )
        normals.append(
            (f32(((i * 3) % 11) / 10.0), f32(1.0 - (i % 5) / 8.0), f32(-0.25))
        )
        groups.append((i * 5 + 3) % group_count)
        uv0.append((f32((i % 19) / 18.0), f32((i % 23) / 22.0)))
        uv1.append((f32((i % 13) / 12.0), f32(1.0 - (i % 29) / 28.0)))
        diffuse.append((0x10203040 + i * 0x01010101) & 0xFFFFFFFF)
    palette = []
    for group in range(group_count):
        palette.append(
            tuple(
                f32(
                    (1.0 if index in (0, 4, 8) else 0.0)
                    + group * 0.0078125
                    + index * 0.0009765625
                )
                for index in range(PALETTE_FLOATS)
            )
        )
    return (
        StaticInputs(
            positions=tuple(positions),
            normals=tuple(normals),
            group_slots=tuple(groups),
            uv0=tuple(uv0),
            uv1=tuple(uv1),
            diffuse=tuple(diffuse),
        ),
        tuple(palette),
    )


def make_coarse_tasks(job_vertices: Sequence[int], target_vertices: int) -> list[tuple[int, int]]:
    if not job_vertices or target_vertices <= 0 or any(v <= 0 for v in job_vertices):
        raise ValueError("invalid task input")
    tasks: list[tuple[int, int]] = []
    first = 0
    total = 0
    for index, vertices in enumerate(job_vertices):
        total += vertices
        if total >= target_vertices or index + 1 == len(job_vertices):
            tasks.append((first, index - first + 1))
            first = index + 1
            total = 0
    return tasks


def make_disjoint_ranges(vertex_count: int, chunk_vertices: int) -> list[tuple[int, int]]:
    if vertex_count <= 0 or chunk_vertices <= 0:
        raise ValueError("invalid range input")
    return [
        (first, min(chunk_vertices, vertex_count - first))
        for first in range(0, vertex_count, chunk_vertices)
    ]


def validate_exact_coverage(lengths: Sequence[int], ranges: Iterable[tuple[int, int]]) -> bool:
    coverage = [0] * len(lengths)
    for first, count in ranges:
        if first < 0 or count <= 0 or first + count > len(lengths):
            return False
        for index in range(first, first + count):
            coverage[index] += 1
    return all(value == 1 for value in coverage)


def mxcsr_control(mxcsr: int) -> int:
    return mxcsr & MXCSR_CONTROL_MASK


def mxcsr_supported(mxcsr: int) -> bool:
    return (mxcsr_control(mxcsr) & MXCSR_EXCEPTION_MASK) == MXCSR_EXCEPTION_MASK


def clamp_cancel_check_period(requested: int) -> int:
    # C++ config is uint32_t; model the conversion before std::clamp.
    unsigned = requested & 0xFFFFFFFF
    return min(max(unsigned, 1), MAX_CANCEL_CHECK_VERTICES)


def run_contract_tests() -> dict[str, object]:
    static, palette = make_fixture()
    format_results = []
    for output_format, stride in enumerate(FVF_STRIDES):
        reference = b"".join(
            encode_vertex(static, palette, output_format, i)
            for i in range(static.vertex_count)
        )
        assert len(reference) == static.vertex_count * stride

        for chunk in (1, 7, 64, 127, 1024):
            candidate = bytearray(len(reference))
            for first, count in make_disjoint_ranges(static.vertex_count, chunk):
                encode_range(
                    static, palette, output_format, first, count, candidate
                )
            assert bytes(candidate) == reference
        format_results.append(
            {
                "format": output_format,
                "stride": stride,
                "bytes": len(reference),
                "chunkingsExact": 5,
                "diffusePolicy": "explicit" if output_format & 1 else "not-present",
                "uvLayers": output_format // 2,
            }
        )

    default_diffuse = StaticInputs(
        positions=static.positions,
        normals=static.normals,
        group_slots=static.group_slots,
        uv0=static.uv0,
        uv1=static.uv1,
        diffuse=None,
    )
    for output_format in (1, 3, 5):
        encoded = encode_vertex(default_diffuse, palette, output_format, 0)
        assert struct.unpack_from("<I", encoded, 24)[0] == 0xFFFFFFFF

    invalid_groups = StaticInputs(
        positions=static.positions,
        normals=static.normals,
        group_slots=(len(palette),) + static.group_slots[1:],
        uv0=static.uv0,
        uv1=static.uv1,
        diffuse=static.diffuse,
    )
    try:
        encode_vertex(invalid_groups, palette, 2, 0)
        raise AssertionError("out-of-range group unexpectedly accepted")
    except ValueError:
        pass

    job_vertices = [193 + ((i * 37) % 256) for i in range(53)]
    tasks = make_coarse_tasks(job_vertices, 2048)
    assert validate_exact_coverage(job_vertices, tasks)
    assert len(tasks) < len(job_vertices)

    vertex_ranges = make_disjoint_ranges(MAX_NATIVE_VERTICES, 1024)
    vertex_coverage = [0] * MAX_NATIVE_VERTICES
    for first, count in vertex_ranges:
        for vertex in range(first, first + count):
            vertex_coverage[vertex] += 1
    assert all(value == 1 for value in vertex_coverage)

    generation = 11
    submitted_generation = generation
    generation += 1
    running_job_completed = True
    lease_publishable = running_job_completed and submitted_generation == generation
    assert not lease_publishable

    queued, running, ready, cpu_fallback = range(4)

    # Interleaving A: owner 先 Queued->CpuFallback；worker 的
    # Queued->Running claim 必须失败，不能覆盖 terminal。
    state = queued
    cancel_flag = False
    cancel_succeeded = state in (queued, running)
    if cancel_succeeded:
        state = cpu_fallback
        cancel_flag = True
    worker_claimed = state == queued
    if worker_claimed:
        state = running
    assert cancel_succeeded and cancel_flag
    assert not worker_claimed and state == cpu_fallback

    # Interleaving B: worker 先 Running->Ready；cancelJob 返回 false，且不能
    # 留下与 Ready 矛盾的 cancel flag。
    state = running
    state = ready
    cancel_flag = False
    cancel_succeeded = state in (queued, running)
    if cancel_succeeded:
        state = cpu_fallback
        cancel_flag = True
    assert not cancel_succeeded and not cancel_flag and state == ready

    # MXCSR status bits 0..5 are sticky diagnostics, not part of the frozen
    # arithmetic contract. DAZ/masks/rounding/FTZ are exact admission state.
    owner_mxcsr = 0x1F80 | 0x8000 | 0x0040
    same_control_different_sticky = owner_mxcsr | 0x0025
    different_rounding = owner_mxcsr | 0x2000
    unmasked_exception = owner_mxcsr & ~0x0080
    assert mxcsr_supported(owner_mxcsr)
    assert mxcsr_control(owner_mxcsr) == mxcsr_control(
        same_control_different_sticky
    )
    assert mxcsr_control(owner_mxcsr) != mxcsr_control(different_rounding)
    assert not mxcsr_supported(unmasked_exception)

    # Synchronous workers only publish an owned staging lease. The producer
    # never receives or touches caller/native mapped output; that copy belongs
    # to a separate native owner/SEH transaction after all ranges settle.
    staging_static, staging_palette = make_fixture(vertex_count=129)
    staging_reference = b"".join(
        encode_vertex(staging_static, staging_palette, 2, i)
        for i in range(staging_static.vertex_count)
    )
    caller_output = bytearray([0xA5] * len(staging_reference))
    staging = bytearray(len(staging_reference))
    for first, count in make_disjoint_ranges(
        staging_static.vertex_count, 64
    ):
        encode_range(
            staging_static, staging_palette, 2, first, count, staging
        )
        assert caller_output == bytearray([0xA5] * len(staging_reference))
    assert bytes(staging) == staging_reference
    published_lease = bytes(staging)
    assert caller_output == bytearray([0xA5] * len(staging_reference))
    caller_output[:] = published_lease
    assert bytes(caller_output) == staging_reference

    # The optimized native core keeps the 64-vertex cancellation cadence but
    # installs MXCSR once per coarse synchronous task, not once per 64-vertex
    # runRange fragment. A consecutive group-slot cache only reloads the 48-B
    # palette matrix when the slot changes.
    sync_vertex_count = 4096
    sync_task_ranges = make_disjoint_ranges(sync_vertex_count, 1024)
    cancel_fragments = make_disjoint_ranges(sync_vertex_count, 64)
    assert len(sync_task_ranges) == 4
    assert len(cancel_fragments) == 64
    grouped_slots = (0,) * 96 + (1,) * 64 + (1,) * 32 + (2,) * 65
    cached_matrix_loads = 1 + sum(
        lhs != rhs for lhs, rhs in zip(grouped_slots, grouped_slots[1:])
    )
    assert cached_matrix_loads == 3
    assert cached_matrix_loads < len(grouped_slots)

    cancel_clamps = {
        requested: clamp_cancel_check_period(requested)
        for requested in (-7, 0, 1, 63, 64, 65, 4096)
    }
    assert cancel_clamps == {
        -7: 64,
        0: 1,
        1: 1,
        63: 63,
        64: 64,
        65: 64,
        4096: 64,
    }

    # Lifecycle is owner-affine, not protected by a cross-thread lifecycle
    # lock. Public reset/shutdown expose checkable wrong-thread failure; the
    # destructor must fail hard because object lifetime is already ending.
    owner_thread = 0x1020
    foreign_thread = 0x3040

    def modeled_reset(caller_thread: int, generation: int) -> int:
        return generation + 1 if caller_thread == owner_thread else 0

    def modeled_shutdown(caller_thread: int) -> bool:
        return caller_thread == owner_thread

    assert modeled_reset(owner_thread, 7) == 8
    assert modeled_reset(foreign_thread, 7) == 0
    assert modeled_shutdown(owner_thread)
    assert not modeled_shutdown(foreign_thread)
    destructor_wrong_thread_policy = "fail-hard"
    assert destructor_wrong_thread_policy == "fail-hard"

    # Synchronous staging and pending async state share maxOwnedBytes.
    max_owned = 32 << 20
    async_owned = max_owned - 1024
    assert async_owned + 2048 > max_owned
    assert async_owned + 1024 <= max_owned

    return {
        "passed": True,
        "formatContracts": format_results,
        "defaultDiffuseOddFormats": 3,
        "groupSlotOutOfRangeRejected": True,
        "coarseTaskCount": len(tasks),
        "coarseJobCount": len(job_vertices),
        "coarseCoverageExact": True,
        "syncRangeCountAt16384": len(vertex_ranges),
        "syncCoverageExact": True,
        "resetPreventsRunningJobPublication": True,
        "cancelVsWorkerInterleavingsExact": 2,
        "mxcsr": {
            "controlMask": MXCSR_CONTROL_MASK,
            "exceptionMask": MXCSR_EXCEPTION_MASK,
            "stickyStatusExcludedFromKey": True,
            "roundingMismatchRejected": True,
            "unmaskedExceptionRejected": True,
        },
        "synchronousOwnedStagingLeaseBeforeNativeCommit": True,
        "producerNeverReceivesExternalOutput": True,
        "nativeOwnerCommitModeledExact": True,
        "mxcsrScopePerCoarseTask": {
            "vertices": sync_vertex_count,
            "coarseTasks": len(sync_task_ranges),
            "cancelFragments": len(cancel_fragments),
        },
        "consecutiveGroupMatrixCache": {
            "vertices": len(grouped_slots),
            "matrixLoads": cached_matrix_loads,
        },
        "cancelCheckClamp": cancel_clamps,
        "lifecycleOwnerAffinityModeled": True,
        "wrongThreadResetReturnsZero": True,
        "wrongThreadShutdownReturnsFalse": True,
        "wrongThreadDestructionPolicy": destructor_wrong_thread_policy,
        "syncAndAsyncShareOwnedByteBudget": True,
    }


@dataclass(frozen=True)
class CostAssumptions:
    workers: int = 4
    synchronous_lanes: int = 5
    synchronous_chunk_vertices: int = 1024
    min_synchronous_vertices: int = 4096
    target_vertices_per_task: int = 2048
    native_call_fixed_us: float = 0.25
    native_vertex_us: float = 0.0496
    mt_vertex_us: float = 0.055
    freeze_batch_fixed_us: float = 1.2
    freeze_job_descriptor_us: float = 0.08
    palette_copy_gib_s: float = 10.0
    enqueue_task_us: float = 0.35
    completion_poll_us: float = 0.12
    fork_join_barrier_us: float = 2.0
    upload_fixed_us: float = 2.5
    upload_copy_gib_s: float = 18.0
    gpu_copy_record_us: float = 0.8
    synchronous_owner_commit_gib_s: float = 18.0


def bytes_to_us(byte_count: int, gib_per_second: float) -> float:
    return byte_count / (gib_per_second * (1024**3)) * 1_000_000.0


def greedy_worker_makespan(task_vertices: Sequence[int], workers: int, vertex_us: float) -> float:
    lanes = [0.0] * max(workers, 1)
    for vertices in sorted(task_vertices, reverse=True):
        lane = min(range(len(lanes)), key=lanes.__getitem__)
        lanes[lane] += vertices * vertex_us
    return max(lanes, default=0.0)


def task_vertex_counts(job_vertices: Sequence[int], target: int) -> list[int]:
    result = []
    for first, count in make_coarse_tasks(job_vertices, target):
        result.append(sum(job_vertices[first : first + count]))
    return result


def model_route(
    job_count: int,
    vertices_per_job: int,
    output_stride: int,
    palette_bytes_per_job: int,
    async_lead_us: float,
    assumptions: CostAssumptions,
) -> dict[str, float | int | bool | None]:
    jobs = [vertices_per_job] * job_count
    tasks = task_vertex_counts(jobs, assumptions.target_vertices_per_task)
    total_vertices = sum(jobs)
    output_bytes = total_vertices * output_stride
    palette_bytes = job_count * palette_bytes_per_job

    native_us = (
        job_count * assumptions.native_call_fixed_us
        + total_vertices * assumptions.native_vertex_us
    )
    freeze_us = (
        assumptions.freeze_batch_fixed_us
        + job_count * assumptions.freeze_job_descriptor_us
        + bytes_to_us(palette_bytes, assumptions.palette_copy_gib_s)
    )
    enqueue_us = len(tasks) * assumptions.enqueue_task_us
    worker_makespan_us = greedy_worker_makespan(
        tasks, assumptions.workers, assumptions.mt_vertex_us
    )
    upload_us = (
        assumptions.upload_fixed_us
        + bytes_to_us(output_bytes, assumptions.upload_copy_gib_s)
        + assumptions.gpu_copy_record_us
    )
    async_wait_us = max(0.0, worker_makespan_us - async_lead_us)
    sync_available = (
        job_count == 1
        and total_vertices >= assumptions.min_synchronous_vertices
    )
    sync_ranges = (
        [
            min(
                assumptions.synchronous_chunk_vertices,
                total_vertices - first,
            )
            for first in range(
                0, total_vertices, assumptions.synchronous_chunk_vertices
            )
        ]
        if sync_available else []
    )
    synchronous_worker_makespan_us = (
        greedy_worker_makespan(
            sync_ranges,
            assumptions.synchronous_lanes,
            assumptions.mt_vertex_us,
        )
        if sync_available else None
    )
    synchronous_owner_commit_us = (
        bytes_to_us(output_bytes, assumptions.synchronous_owner_commit_gib_s)
        if sync_available else None
    )
    async_render_lane_us = (
        freeze_us
        + enqueue_us
        + assumptions.completion_poll_us
        + upload_us
        + async_wait_us
    )
    sync_enqueue_us = (
        len(sync_ranges) * assumptions.enqueue_task_us
        if sync_available else None
    )
    sync_render_lane_us = (
        freeze_us
        + (sync_enqueue_us or 0.0)
        + assumptions.fork_join_barrier_us
        + (synchronous_worker_makespan_us or 0.0)
        + (synchronous_owner_commit_us or 0.0)
        if sync_available else None
    )
    return {
        "jobs": job_count,
        "verticesPerJob": vertices_per_job,
        "totalVertices": total_vertices,
        "tasks": len(tasks),
        "outputBytes": output_bytes,
        "nativeModelUs": round(native_us, 4),
        "freezeUs": round(freeze_us, 4),
        "enqueueUs": round(enqueue_us, 4),
        "workerMakespanUs": round(worker_makespan_us, 4),
        "batchedUploadUs": round(upload_us, 4),
        "synchronousAvailable": sync_available,
        "synchronousRangeCount": len(sync_ranges),
        "synchronousEnqueueUs": (
            round(sync_enqueue_us, 4)
            if sync_enqueue_us is not None else None
        ),
        "synchronousWorkerMakespanUs": (
            round(synchronous_worker_makespan_us, 4)
            if synchronous_worker_makespan_us is not None else None
        ),
        "synchronousOwnerCommitUs": (
            round(synchronous_owner_commit_us, 4)
            if synchronous_owner_commit_us is not None else None
        ),
        "asyncLeadUs": async_lead_us,
        "asyncWaitUs": round(async_wait_us, 4),
        "asyncRenderLaneUs": round(async_render_lane_us, 4),
        "syncRenderLaneUs": (
            round(sync_render_lane_us, 4)
            if sync_render_lane_us is not None else None
        ),
        "asyncDeltaVsNativeUs": round(async_render_lane_us - native_us, 4),
        "syncDeltaVsNativeUs": (
            round(sync_render_lane_us - native_us, 4)
            if sync_render_lane_us is not None else None
        ),
        "asyncWinsInThisAssumption": async_render_lane_us < native_us,
        "syncWinsInThisAssumption": (
            sync_render_lane_us < native_us
            if sync_render_lane_us is not None else None
        ),
    }


def cost_model() -> dict[str, object]:
    assumptions = CostAssumptions()
    scenarios = []
    for jobs in (1, 8, 16, 32, 64):
        for vertices in (32, 224, 320, 589, 2048, 4096):
            scenarios.append(
                model_route(
                    job_count=jobs,
                    vertices_per_job=vertices,
                    output_stride=32,
                    palette_bytes_per_job=96,
                    async_lead_us=200.0,
                    assumptions=assumptions,
                )
            )

    sensitivity = []
    for mt_vertex_us in (0.02, 0.04, 0.055, 0.08):
        tuned = CostAssumptions(mt_vertex_us=mt_vertex_us)
        sensitivity.append(
            {
                "mtVertexUs": mt_vertex_us,
                "medium32x320": model_route(
                    32, 320, 32, 96, 200.0, tuned
                ),
                "large8x2048": model_route(
                    8, 2048, 32, 96, 200.0, tuned
                ),
            }
        )

    runtime = {
        "frameSerial": 3058,
        "kernelCalls": 1_121_900,
        "originalKernelCalls": 1_111_353,
        "originalKernelBytes": 248_651_648,
        "bypassedKernelCalls": 10_547,
        "bypassedKernelBytes": 198_956_224,
        "sampledOriginalKernelAverageUs": 0.5970561177552898,
        "sampledOriginalKernelCaveat": (
            "prime-period all-route sample; not bucket-specific small-SSE time"
        ),
        "originalAverageBytesPerCall": 248_651_648 / 1_111_353,
        "bypassedAverageBytesPerCall": 198_956_224 / 10_547,
    }
    original_calls_per_frame = runtime["originalKernelCalls"] / runtime["frameSerial"]
    sampled_kernel_ceiling_ms_per_frame = (
        original_calls_per_frame * runtime["sampledOriginalKernelAverageUs"] / 1000.0
    )
    isolated_ab = {
        "artifact": "AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260715_133103/summary.json",
        "attributionOnly": True,
        "formalFpsJudgement": False,
        "disabledFrameMs": 11.4635,
        "bypassFrameMs": 14.264,
        "frameDeltaMs": 2.8005,
        "disabledMainMs": 9.4265,
        "bypassMainMs": 12.7975,
        "mainDeltaMs": 3.371,
        "disabledGpuMs": 1.424,
        "bypassGpuMs": 1.412,
    }

    one_call_one_task_floor_us = (
        assumptions.freeze_job_descriptor_us
        + assumptions.enqueue_task_us
        + assumptions.completion_poll_us
    )
    one_call_one_task_route_floor_us = (
        assumptions.freeze_batch_fixed_us
        + one_call_one_task_floor_us
        + assumptions.upload_fixed_us
        + assumptions.gpu_copy_record_us
    )
    return {
        "assumptions": asdict(assumptions),
        "assumptionWarning": (
            "All microsecond constants except the named runtime samples are "
            "sensitivity inputs, not measured C++ or War3 results."
        ),
        "runtimeEvidence": runtime,
        "isolatedAttribution": isolated_ab,
        "sampledKernelCeilingMsPerFrame": sampled_kernel_ceiling_ms_per_frame,
        "ceilingVsCurrentBypassPenaltyRatio": (
            sampled_kernel_ceiling_ms_per_frame / isolated_ab["frameDeltaMs"]
        ),
        "oneCallOneTaskManagementFloorUs": one_call_one_task_floor_us,
        "oneCallOneTaskVsSampledKernelRatio": (
            one_call_one_task_floor_us
            / runtime["sampledOriginalKernelAverageUs"]
        ),
        "oneCallOneTaskRouteFloorBeforeMathAndCopiesUs": (
            one_call_one_task_route_floor_us
        ),
        "oneCallOneTaskRouteFloorVsSampledKernelRatio": (
            one_call_one_task_route_floor_us
            / runtime["sampledOriginalKernelAverageUs"]
        ),
        "scenarios": scenarios,
        "sensitivity": sensitivity,
        "routingConclusion": {
            "tiny": (
                "Keep Game.dll SSE. A plausible descriptor/enqueue/poll floor is "
                "already about 92% of the measured all-route kernel mean; adding "
                "batch setup, math and upload makes one-task-per-call decisively lose."
            ),
            "medium": (
                "Only consider flush-entry async coarse batches with static snapshot "
                "reuse and enough lead time; never enqueue one task per upload."
            ),
            "large": (
                "GPU remains the preferred producer when its exact consumer contract "
                "is available. Phase-A synchronous MT needs a much larger crossover."
            ),
            "currentPenalty": (
                "Even eliminating the sampled original CPU kernel ceiling cannot repay "
                "the +2.8005 ms isolated bypass delta; that delta is predominantly "
                "takeover management, not skin arithmetic."
            ),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        help="可选 JSON 输出；默认只打印 stdout",
    )
    args = parser.parse_args()
    result = {
        "schemaVersion": "war3-cpu-skin-mt-offline-v3",
        "scope": "pure-python-contract-and-parametric-cost-model",
        "launchPerformed": False,
        "deployPerformed": False,
        "autoTestPerformed": False,
        "childProcessesCreated": False,
        "contractTests": run_contract_tests(),
        "costModel": cost_model(),
    }
    payload = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
