#!/usr/bin/env python3
"""Offline verifier for a future GPU-skin outside-Lock shadow proof.

The current production path predicts the next native vertex-ring slice from a
fault-safe GX snapshot.  A future O0 experiment may observe the independently
successful D3D9 vertex Lock and compare its exact resource/range proof with that
authoritative result.  This script models that comparison without changing an
authorization decision and without launching, attaching to, building, or
deploying Warcraft III.

The model deliberately distinguishes three outcomes:

* N: the predicted/locked slice does not overlap a live poison range;
* O: it overlaps a live poison range;
* R: the proof is unreadable, incomplete, invalidated, or otherwise must fall
  back to the existing fail-closed path.

The old GX proof cannot observe D3D9CommonBuffer identity generations, so an
old O versus exact-Lock N result can be a safe conservative refinement after a
COM-pointer ABA.  Old N versus exact-Lock O is unsafe and must remain zero in
the stable comparison cohort.  DISCARD is only the 0x4000-ring wrap lock mode;
it is never treated as resource retirement or poison clearance.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from dataclasses import dataclass, replace
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Any, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ARTIFACT_ROOT = (
    REPO_ROOT / "AutoTest" / "artifacts"
)

RING_CAPACITY_VERTICES = 0x4000
U64_MASK = (1 << 64) - 1

# Exact Game.dll/DXVK format contract used by the native bridge.
FORMAT_FVF = (0x012, 0x052, 0x112, 0x152, 0x212, 0x252)
FORMAT_STRIDE = (24, 28, 32, 36, 40, 44)

D3DLOCK_NOSYSLOCK = 0x0800
D3DLOCK_NOOVERWRITE = 0x1000
D3DLOCK_DISCARD = 0x2000
LOCK_FLAGS_NOOVERWRITE = D3DLOCK_NOSYSLOCK | D3DLOCK_NOOVERWRITE
LOCK_FLAGS_DISCARD = D3DLOCK_NOSYSLOCK | D3DLOCK_DISCARD

MATRIX_LABELS = ("N", "O", "R")
MAX_SAVED_EXAMPLES = 24
MAX_FUZZ_POISONS = 6


class Outcome(str, Enum):
    NO_OVERLAP = "N"
    OVERLAP = "O"
    READ_FAILURE = "R"


@dataclass(frozen=True)
class PoisonKey:
    native_device: int
    common_resource: int
    com_vertex_buffer: int
    resource_generation: int
    output_format: int


@dataclass(frozen=True)
class PoisonRange:
    key: PoisonKey
    base_vertex: int
    vertex_count: int

    @property
    def end_vertex(self) -> int:
        return self.base_vertex + self.vertex_count


@dataclass(frozen=True)
class GxSnapshot:
    readable: bool
    output_format: int
    native_device: int
    vertex_buffers: tuple[int, ...]
    ring_next: tuple[int, ...]


@dataclass(frozen=True)
class LockProof:
    lock_succeeded: bool
    active_lock: bool
    lock_count: int
    native_device: int
    common_resource: int
    com_vertex_buffer: int
    resource_generation: int
    output_format: int
    vertex_stride: int
    fvf: int
    owner_identity: int
    offset_bytes: int
    size_bytes: int
    flags: int
    mapped_base: int
    mapped_dst: int
    real_storage_generation: int
    mapping_storage_generation: int
    map_allocation_generation: int
    reset_requested: int
    reset_completed: int

    @property
    def key(self) -> PoisonKey:
        return PoisonKey(
            native_device=self.native_device,
            common_resource=self.common_resource,
            com_vertex_buffer=self.com_vertex_buffer,
            resource_generation=self.resource_generation,
            output_format=self.output_format,
        )


@dataclass(frozen=True)
class Scenario:
    name: str
    vertex_count: int
    entry_poisons: tuple[PoisonRange, ...]
    settle_poisons: tuple[PoisonRange, ...]
    gx: GxSnapshot
    lock: LockProof
    poison_generation_entry: int = 1
    poison_generation_settle: int = 1
    reset_requested_entry: int = 1
    reset_completed_entry: int = 1
    reset_requested_settle: int = 1
    reset_completed_settle: int = 1
    retirement_generation_entry: int = 1
    retirement_generation_settle: int = 1
    reentry: bool = False
    cpu_kernel_normal_return: bool = True
    outer_call_succeeded: bool = True
    source: str = "deterministic"


@dataclass(frozen=True)
class Evaluation:
    outcome: Outcome
    reason: str
    begin_vertex: int | None = None
    end_vertex: int | None = None
    physical_target: tuple[int, int, int] | None = None
    exact_key: PoisonKey | None = None
    flags: int | None = None

    @property
    def interval(self) -> tuple[int, int] | None:
        if self.begin_vertex is None or self.end_vertex is None:
            return None
        return (self.begin_vertex, self.end_vertex)


@dataclass(frozen=True)
class CaseResult:
    old: Evaluation
    lock: Evaluation
    settlement_reason: str
    comparison_eligible: bool
    strict_exact_identity: bool
    mutation_generation_violation: bool
    reset_generation_violation: bool
    lock_mode: str


def advance_generation(value: int) -> int:
    """Match the planned monotonic uint64 generation with zero skipped."""

    value = (value + 1) & U64_MASK
    return 1 if value == 0 else value


def interval_overlaps(
    lhs_begin: int, lhs_end: int, rhs_begin: int, rhs_end: int
) -> bool:
    return lhs_end > rhs_begin and lhs_begin < rhs_end


def ring_interval(ring_next: int, vertex_count: int) -> tuple[int, int, bool] | None:
    if (
        vertex_count <= 0
        or vertex_count > RING_CAPACITY_VERTICES
        or ring_next < 0
        or ring_next > RING_CAPACITY_VERTICES
    ):
        return None
    # IDA 0x6F0EE5D0: wrap only when oldNext + count exceeds 0x4000.
    wraps = ring_next > RING_CAPACITY_VERTICES - vertex_count
    begin = 0 if wraps else ring_next
    return (begin, begin + vertex_count, wraps)


def valid_poison(poison: PoisonRange) -> bool:
    key = poison.key
    return (
        key.native_device != 0
        and key.common_resource != 0
        and key.com_vertex_buffer != 0
        and key.resource_generation != 0
        and 0 <= key.output_format < len(FORMAT_FVF)
        and poison.vertex_count > 0
        and poison.base_vertex >= 0
        and poison.end_vertex <= RING_CAPACITY_VERTICES
    )


def valid_ledger(poisons: Sequence[PoisonRange]) -> bool:
    return all(valid_poison(poison) for poison in poisons)


def old_gx_safe_copy_outcome(
    scenario: Scenario,
) -> Evaluation:
    """Model current ProveOutsideCpuUploadNoPoisonOverlap semantics."""

    poisons = scenario.entry_poisons
    if not poisons:
        return Evaluation(Outcome.NO_OVERLAP, "emptyLedger")
    if not valid_ledger(poisons):
        return Evaluation(Outcome.READ_FAILURE, "incompletePoison")
    if scenario.vertex_count <= 0 or scenario.vertex_count > RING_CAPACITY_VERTICES:
        return Evaluation(Outcome.READ_FAILURE, "invalidVertexCount")
    gx = scenario.gx
    if not gx.readable:
        return Evaluation(Outcome.READ_FAILURE, "safeCopyFailure")
    if (
        len(gx.vertex_buffers) != len(FORMAT_FVF)
        or len(gx.ring_next) != len(FORMAT_FVF)
        or gx.output_format < 0
        or gx.output_format >= len(FORMAT_FVF)
        or gx.native_device == 0
    ):
        return Evaluation(Outcome.READ_FAILURE, "invalidGxSnapshot")
    com_vertex_buffer = gx.vertex_buffers[gx.output_format]
    ring_next = gx.ring_next[gx.output_format]
    predicted = ring_interval(ring_next, scenario.vertex_count)
    if com_vertex_buffer == 0 or predicted is None:
        return Evaluation(Outcome.READ_FAILURE, "invalidGxTarget")
    begin, end, wraps = predicted
    physical = (gx.native_device, com_vertex_buffer, gx.output_format)
    for poison in poisons:
        poison_physical = (
            poison.key.native_device,
            poison.key.com_vertex_buffer,
            poison.key.output_format,
        )
        # GX cannot identify commonResource/resourceGeneration.  The old proof
        # deliberately treats all complete poisons with this physical target as
        # potentially live, including COM-pointer ABA generations.
        if poison_physical != physical:
            continue
        if interval_overlaps(begin, end, poison.base_vertex, poison.end_vertex):
            return Evaluation(
                Outcome.OVERLAP,
                "physicalPoisonOverlap",
                begin,
                end,
                physical,
                flags=LOCK_FLAGS_DISCARD if wraps else LOCK_FLAGS_NOOVERWRITE,
            )
    return Evaluation(
        Outcome.NO_OVERLAP,
        "physicalNoOverlap",
        begin,
        end,
        physical,
        flags=LOCK_FLAGS_DISCARD if wraps else LOCK_FLAGS_NOOVERWRITE,
    )


def validate_lock_proof(
    proof: LockProof, vertex_count: int
) -> tuple[bool, str, int | None, int | None]:
    if not proof.lock_succeeded:
        return (False, "lockFailed", None, None)
    if not proof.active_lock or proof.lock_count != 1:
        return (False, "activeLockMismatch", None, None)
    if (
        proof.native_device == 0
        or proof.common_resource == 0
        or proof.com_vertex_buffer == 0
        or proof.resource_generation == 0
        or proof.owner_identity != proof.com_vertex_buffer
    ):
        return (False, "resourceIdentityIncomplete", None, None)
    if proof.output_format < 0 or proof.output_format >= len(FORMAT_FVF):
        return (False, "formatInvalid", None, None)
    if (
        proof.vertex_stride != FORMAT_STRIDE[proof.output_format]
        or proof.fvf != FORMAT_FVF[proof.output_format]
    ):
        return (False, "formatLayoutMismatch", None, None)
    if vertex_count <= 0 or vertex_count > RING_CAPACITY_VERTICES:
        return (False, "vertexCountInvalid", None, None)
    expected_size = vertex_count * proof.vertex_stride
    if (
        proof.offset_bytes < 0
        or proof.size_bytes != expected_size
        or proof.offset_bytes % proof.vertex_stride != 0
    ):
        return (False, "lockRangeMismatch", None, None)
    begin = proof.offset_bytes // proof.vertex_stride
    end = begin + vertex_count
    if end > RING_CAPACITY_VERTICES:
        return (False, "lockRangeOverflow", None, None)
    if proof.flags not in (LOCK_FLAGS_NOOVERWRITE, LOCK_FLAGS_DISCARD):
        return (False, "lockFlagsInvalid", None, None)
    if proof.flags == LOCK_FLAGS_DISCARD and begin != 0:
        return (False, "discardOffsetNonzero", None, None)
    if (
        proof.mapped_base == 0
        or proof.mapped_dst != proof.mapped_base + proof.offset_bytes
    ):
        return (False, "mappedPointerMismatch", None, None)
    if (
        proof.real_storage_generation == 0
        or proof.mapping_storage_generation == 0
        or proof.map_allocation_generation == 0
    ):
        return (False, "storageGenerationIncomplete", None, None)
    if (
        proof.reset_requested == 0
        or proof.reset_completed == 0
        or proof.reset_requested != proof.reset_completed
    ):
        return (False, "resetNotQuiescent", None, None)
    return (True, "exactSuccessfulLock", begin, end)


def independent_lock_outcome(scenario: Scenario) -> Evaluation:
    """Classify poison using only the exact successful D3D9 Lock proof."""

    poisons = scenario.settle_poisons
    if not poisons:
        return Evaluation(Outcome.NO_OVERLAP, "emptyLedger")
    if not valid_ledger(poisons):
        return Evaluation(Outcome.READ_FAILURE, "incompletePoison")
    valid, reason, begin, end = validate_lock_proof(
        scenario.lock, scenario.vertex_count
    )
    if not valid or begin is None or end is None:
        return Evaluation(Outcome.READ_FAILURE, reason)
    key = scenario.lock.key
    physical = (key.native_device, key.com_vertex_buffer, key.output_format)
    for poison in poisons:
        if poison.key != key:
            continue
        if interval_overlaps(begin, end, poison.base_vertex, poison.end_vertex):
            return Evaluation(
                Outcome.OVERLAP,
                "exactPoisonOverlap",
                begin,
                end,
                physical,
                key,
                scenario.lock.flags,
            )
    return Evaluation(
        Outcome.NO_OVERLAP,
        "exactNoOverlap",
        begin,
        end,
        physical,
        key,
        scenario.lock.flags,
    )


def poison_content_key(poisons: Iterable[PoisonRange]) -> tuple[Any, ...]:
    return tuple(
        sorted(
            (
                poison.key.native_device,
                poison.key.common_resource,
                poison.key.com_vertex_buffer,
                poison.key.resource_generation,
                poison.key.output_format,
                poison.base_vertex,
                poison.vertex_count,
            )
            for poison in poisons
        )
    )


def targets_match(old: Evaluation, lock: Evaluation) -> bool:
    return (
        old.physical_target is not None
        and old.physical_target == lock.physical_target
        and old.interval is not None
        and old.interval == lock.interval
    )


def strict_exact_poison_identity(
    scenario: Scenario, old: Evaluation, lock: Evaluation
) -> bool:
    if not targets_match(old, lock) or lock.exact_key is None:
        return False
    assert old.interval is not None
    begin, end = old.interval
    for poison in scenario.entry_poisons:
        physical = (
            poison.key.native_device,
            poison.key.com_vertex_buffer,
            poison.key.output_format,
        )
        if physical != old.physical_target:
            continue
        if not interval_overlaps(begin, end, poison.base_vertex, poison.end_vertex):
            continue
        if poison.key != lock.exact_key:
            return False
    return poison_content_key(scenario.entry_poisons) == poison_content_key(
        scenario.settle_poisons
    )


def classify_case(scenario: Scenario) -> CaseResult:
    old = old_gx_safe_copy_outcome(scenario)
    lock = independent_lock_outcome(scenario)
    content_changed = poison_content_key(
        scenario.entry_poisons
    ) != poison_content_key(scenario.settle_poisons)
    generation_changed = (
        scenario.poison_generation_entry != scenario.poison_generation_settle
    )
    mutation_generation_violation = content_changed and not generation_changed
    reset_changed = (
        scenario.reset_requested_entry != scenario.reset_requested_settle
        or scenario.reset_completed_entry != scenario.reset_completed_settle
        or scenario.reset_requested_entry != scenario.reset_completed_entry
        or scenario.reset_requested_settle != scenario.reset_completed_settle
    )
    # A completed reset owns a poison-ledger epoch transition even when the
    # ledger is empty.  A reset change with no generation advance is detectable
    # as a contract violation in this offline model.
    reset_generation_violation = reset_changed and not generation_changed

    if mutation_generation_violation:
        settlement = "mutationGenerationViolation"
    elif reset_generation_violation:
        settlement = "resetGenerationViolation"
    elif reset_changed:
        settlement = "reset"
    elif generation_changed:
        settlement = "poisonMutation"
    elif (
        scenario.retirement_generation_entry
        != scenario.retirement_generation_settle
    ):
        settlement = "retirement"
    elif not scenario.entry_poisons:
        # The current outside path reads no GX poison snapshot when the ledger
        # is empty.  Such calls are useful population data but are not part of
        # the proposed old-GX-versus-Lock shadow comparison cohort.
        settlement = "noPoisonFastPath"
    elif scenario.reentry:
        settlement = "reentry"
    elif not scenario.cpu_kernel_normal_return:
        settlement = "noNormalReturn"
    elif not scenario.outer_call_succeeded:
        settlement = "outerFailure"
    elif old.outcome == Outcome.READ_FAILURE:
        settlement = "oldReadFailure"
    elif lock.outcome == Outcome.READ_FAILURE:
        settlement = "lockProofFailure"
    elif not targets_match(old, lock) and scenario.entry_poisons:
        settlement = "targetDrift"
    else:
        settlement = "eligibleShadowComparison"

    eligible = settlement == "eligibleShadowComparison"
    strict = eligible and strict_exact_poison_identity(scenario, old, lock)
    if lock.flags == LOCK_FLAGS_DISCARD:
        lock_mode = "discard"
    elif lock.flags == LOCK_FLAGS_NOOVERWRITE:
        lock_mode = "noOverwrite"
    else:
        lock_mode = "invalid"
    return CaseResult(
        old=old,
        lock=lock,
        settlement_reason=settlement,
        comparison_eligible=eligible,
        strict_exact_identity=strict,
        mutation_generation_violation=mutation_generation_violation,
        reset_generation_violation=reset_generation_violation,
        lock_mode=lock_mode,
    )


def empty_matrix() -> dict[str, dict[str, int]]:
    return {
        old: {lock: 0 for lock in MATRIX_LABELS}
        for old in MATRIX_LABELS
    }


def add_matrix(
    matrix: dict[str, dict[str, int]], old: Outcome, lock: Outcome
) -> None:
    matrix[old.value][lock.value] += 1


def matrix_summary(matrix: dict[str, dict[str, int]]) -> dict[str, Any]:
    total = sum(sum(row.values()) for row in matrix.values())
    rows = {old: sum(matrix[old].values()) for old in MATRIX_LABELS}
    columns = {
        lock: sum(matrix[old][lock] for old in MATRIX_LABELS)
        for lock in MATRIX_LABELS
    }
    diagonal = sum(matrix[label][label] for label in MATRIX_LABELS)
    return {
        "cells": matrix,
        "total": total,
        "rowTotals": rows,
        "columnTotals": columns,
        "diagonal": diagonal,
        "offDiagonal": total - diagonal,
        "unsafeOldNLockO": matrix["N"]["O"],
        "safeConservativeOldOLockN": matrix["O"]["N"],
        "closureClean": (
            sum(rows.values()) == total
            and sum(columns.values()) == total
        ),
    }


class Aggregate:
    def __init__(self) -> None:
        self.total = 0
        self.all_matrix = empty_matrix()
        self.eligible_matrix = empty_matrix()
        self.strict_matrix = empty_matrix()
        self.invalidated_matrix = empty_matrix()
        self.settlement_reasons: dict[str, int] = {}
        self.lock_modes: dict[str, int] = {}
        self.old_reasons: dict[str, int] = {}
        self.lock_reasons: dict[str, int] = {}
        self.mutation_generation_violations = 0
        self.reset_generation_violations = 0
        self.max_live_poisons = 0
        self.saved_examples: list[dict[str, Any]] = []

    @staticmethod
    def _increment(target: dict[str, int], key: str) -> None:
        target[key] = target.get(key, 0) + 1

    def add(self, scenario: Scenario, result: CaseResult) -> None:
        self.total += 1
        add_matrix(self.all_matrix, result.old.outcome, result.lock.outcome)
        if result.comparison_eligible:
            add_matrix(
                self.eligible_matrix, result.old.outcome, result.lock.outcome
            )
        else:
            add_matrix(
                self.invalidated_matrix, result.old.outcome, result.lock.outcome
            )
        if result.strict_exact_identity:
            add_matrix(self.strict_matrix, result.old.outcome, result.lock.outcome)
        self._increment(self.settlement_reasons, result.settlement_reason)
        self._increment(self.lock_modes, result.lock_mode)
        self._increment(self.old_reasons, result.old.reason)
        self._increment(self.lock_reasons, result.lock.reason)
        self.mutation_generation_violations += int(
            result.mutation_generation_violation
        )
        self.reset_generation_violations += int(
            result.reset_generation_violation
        )
        self.max_live_poisons = max(
            self.max_live_poisons,
            len(scenario.entry_poisons),
            len(scenario.settle_poisons),
        )
        interesting = (
            result.old.outcome != result.lock.outcome
            or result.settlement_reason != "eligibleShadowComparison"
        )
        if interesting and len(self.saved_examples) < MAX_SAVED_EXAMPLES:
            self.saved_examples.append(
                {
                    "name": scenario.name,
                    "source": scenario.source,
                    "old": result.old.outcome.value,
                    "oldReason": result.old.reason,
                    "lock": result.lock.outcome.value,
                    "lockReason": result.lock.reason,
                    "settlement": result.settlement_reason,
                    "poisonCountEntry": len(scenario.entry_poisons),
                    "poisonCountSettle": len(scenario.settle_poisons),
                    "lockMode": result.lock_mode,
                }
            )

    def result(self) -> dict[str, Any]:
        all_summary = matrix_summary(self.all_matrix)
        eligible_summary = matrix_summary(self.eligible_matrix)
        strict_summary = matrix_summary(self.strict_matrix)
        invalidated_summary = matrix_summary(self.invalidated_matrix)
        partition_total = sum(self.settlement_reasons.values())
        eligible_total = self.settlement_reasons.get(
            "eligibleShadowComparison", 0
        )
        return {
            "totalCases": self.total,
            "allObservedMatrix": all_summary,
            "eligibleShadowMatrix": eligible_summary,
            "strictExactIdentityMatrix": strict_summary,
            "invalidatedOrFallbackMatrix": invalidated_summary,
            "settlementReasons": dict(sorted(self.settlement_reasons.items())),
            "lockModes": dict(sorted(self.lock_modes.items())),
            "oldReasons": dict(sorted(self.old_reasons.items())),
            "lockReasons": dict(sorted(self.lock_reasons.items())),
            "mutationGenerationViolations": self.mutation_generation_violations,
            "resetGenerationViolations": self.reset_generation_violations,
            "maxLivePoisons": self.max_live_poisons,
            "savedExamples": self.saved_examples,
            "closures": {
                "settlementPartition": partition_total == self.total,
                "eligibleCount": eligible_total == eligible_summary["total"],
                "eligiblePlusInvalidated": (
                    eligible_summary["total"] + invalidated_summary["total"]
                    == self.total
                ),
                "allMatrices": all(
                    summary["closureClean"]
                    for summary in (
                        all_summary,
                        eligible_summary,
                        strict_summary,
                        invalidated_summary,
                    )
                ),
            },
        }


def format_key(output_format: int, generation: int = 7) -> PoisonKey:
    return PoisonKey(
        native_device=0x10010001,
        common_resource=0x20020000 + output_format * 0x100,
        com_vertex_buffer=0x30030000 + output_format * 0x100,
        resource_generation=generation,
        output_format=output_format,
    )


def build_gx(
    output_format: int,
    ring_next: int,
    key: PoisonKey | None = None,
    readable: bool = True,
) -> GxSnapshot:
    target = key if key is not None else format_key(output_format)
    vertex_buffers = [
        0x40040000 + index * 0x100 for index in range(len(FORMAT_FVF))
    ]
    ring_next_slots = [0 for _ in FORMAT_FVF]
    if 0 <= output_format < len(FORMAT_FVF):
        vertex_buffers[output_format] = target.com_vertex_buffer
        ring_next_slots[output_format] = ring_next
    return GxSnapshot(
        readable=readable,
        output_format=output_format,
        native_device=target.native_device,
        vertex_buffers=tuple(vertex_buffers),
        ring_next=tuple(ring_next_slots),
    )


def build_lock(
    key: PoisonKey,
    ring_next: int,
    vertex_count: int,
    *,
    reset_generation: int = 1,
) -> LockProof:
    predicted = ring_interval(ring_next, vertex_count)
    if predicted is None:
        begin = 0
        wraps = False
    else:
        begin, _, wraps = predicted
    stride = FORMAT_STRIDE[key.output_format]
    offset = begin * stride
    mapped_base = 0x50050000 + key.output_format * 0x100000
    return LockProof(
        lock_succeeded=True,
        active_lock=True,
        lock_count=1,
        native_device=key.native_device,
        common_resource=key.common_resource,
        com_vertex_buffer=key.com_vertex_buffer,
        resource_generation=key.resource_generation,
        output_format=key.output_format,
        vertex_stride=stride,
        fvf=FORMAT_FVF[key.output_format],
        owner_identity=key.com_vertex_buffer,
        offset_bytes=offset,
        size_bytes=max(vertex_count, 0) * stride,
        flags=LOCK_FLAGS_DISCARD if wraps else LOCK_FLAGS_NOOVERWRITE,
        mapped_base=mapped_base,
        mapped_dst=mapped_base + offset,
        real_storage_generation=11,
        mapping_storage_generation=13,
        map_allocation_generation=17,
        reset_requested=reset_generation,
        reset_completed=reset_generation,
    )


def build_scenario(
    name: str,
    output_format: int,
    ring_next: int,
    vertex_count: int,
    poisons: Sequence[PoisonRange],
    **changes: Any,
) -> Scenario:
    key = changes.pop("key", format_key(output_format))
    gx = changes.pop("gx", build_gx(output_format, ring_next, key))
    lock = changes.pop("lock", build_lock(key, ring_next, vertex_count))
    settle_poisons = changes.pop("settle_poisons", tuple(poisons))
    scenario = Scenario(
        name=name,
        vertex_count=vertex_count,
        entry_poisons=tuple(poisons),
        settle_poisons=tuple(settle_poisons),
        gx=gx,
        lock=lock,
        **changes,
    )
    return scenario


@dataclass(frozen=True)
class DeterministicExpectation:
    scenario: Scenario
    old: Outcome
    lock: Outcome
    settlement: str
    strict: bool | None = None
    lock_mode: str | None = None


def deterministic_cases() -> list[DeterministicExpectation]:
    cases: list[DeterministicExpectation] = []
    for output_format in range(len(FORMAT_FVF)):
        key = format_key(output_format)
        cases.append(
            DeterministicExpectation(
                build_scenario(
                    f"format{output_format}_nonwrap_no_overlap",
                    output_format,
                    128,
                    32,
                    (PoisonRange(key, 512, 16),),
                ),
                Outcome.NO_OVERLAP,
                Outcome.NO_OVERLAP,
                "eligibleShadowComparison",
                True,
            )
        )
        cases.append(
            DeterministicExpectation(
                build_scenario(
                    f"format{output_format}_nonwrap_overlap",
                    output_format,
                    128,
                    32,
                    (PoisonRange(key, 140, 8),),
                ),
                Outcome.OVERLAP,
                Outcome.OVERLAP,
                "eligibleShadowComparison",
                True,
            )
        )

    key = format_key(0)
    cases.extend(
        [
            DeterministicExpectation(
                build_scenario(
                    "ring_exact_end_no_wrap",
                    0,
                    RING_CAPACITY_VERTICES - 4,
                    4,
                    (PoisonRange(key, RING_CAPACITY_VERTICES - 2, 2),),
                ),
                Outcome.OVERLAP,
                Outcome.OVERLAP,
                "eligibleShadowComparison",
                True,
                "noOverwrite",
            ),
            DeterministicExpectation(
                build_scenario(
                    "ring_exceeds_end_discard_overlap_zero",
                    0,
                    RING_CAPACITY_VERTICES - 3,
                    4,
                    (PoisonRange(key, 0, 1),),
                ),
                Outcome.OVERLAP,
                Outcome.OVERLAP,
                "eligibleShadowComparison",
                True,
                "discard",
            ),
            DeterministicExpectation(
                build_scenario(
                    "half_open_adjacency_is_not_overlap",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 120, 10),),
                ),
                Outcome.NO_OVERLAP,
                Outcome.NO_OVERLAP,
                "eligibleShadowComparison",
                True,
            ),
            DeterministicExpectation(
                build_scenario(
                    "gx_safecopy_failure",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    gx=build_gx(0, 100, key, readable=False),
                ),
                Outcome.READ_FAILURE,
                Outcome.OVERLAP,
                "oldReadFailure",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "empty_ledger_needs_no_shadow_comparison",
                    0,
                    100,
                    20,
                    (),
                ),
                Outcome.NO_OVERLAP,
                Outcome.NO_OVERLAP,
                "noPoisonFastPath",
                False,
            ),
        ]
    )

    aba_key = replace(
        key,
        common_resource=key.common_resource + 0x80,
        resource_generation=key.resource_generation + 1,
    )
    cases.append(
        DeterministicExpectation(
            build_scenario(
                "com_pointer_aba_is_safe_old_O_lock_N",
                0,
                100,
                20,
                (PoisonRange(aba_key, 105, 2),),
            ),
            Outcome.OVERLAP,
            Outcome.NO_OVERLAP,
            "eligibleShadowComparison",
            False,
        )
    )

    mutated = (PoisonRange(key, 105, 2),)
    cases.extend(
        [
            DeterministicExpectation(
                build_scenario(
                    "ledger_mutation_invalidates_shadow",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 500, 2),),
                    settle_poisons=mutated,
                    poison_generation_settle=2,
                ),
                Outcome.NO_OVERLAP,
                Outcome.OVERLAP,
                "poisonMutation",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "reset_pending_invalidates_lock",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    lock=replace(
                        build_lock(key, 100, 20),
                        reset_requested=2,
                        reset_completed=1,
                    ),
                    poison_generation_settle=2,
                    reset_requested_settle=2,
                    reset_completed_settle=1,
                ),
                Outcome.OVERLAP,
                Outcome.READ_FAILURE,
                "reset",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "completed_reset_still_invalidates_shadow_epoch",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    lock=build_lock(key, 100, 20, reset_generation=2),
                    poison_generation_settle=2,
                    reset_requested_settle=2,
                    reset_completed_settle=2,
                ),
                Outcome.OVERLAP,
                Outcome.OVERLAP,
                "reset",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "reentry_target_drift_is_excluded",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 205, 2),),
                    lock=build_lock(key, 200, 20),
                    reentry=True,
                ),
                Outcome.NO_OVERLAP,
                Outcome.OVERLAP,
                "reentry",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "normal_return_required_for_settlement",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    cpu_kernel_normal_return=False,
                ),
                Outcome.OVERLAP,
                Outcome.OVERLAP,
                "noNormalReturn",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "failed_lock_is_R",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    lock=replace(
                        build_lock(key, 100, 20), lock_succeeded=False
                    ),
                ),
                Outcome.OVERLAP,
                Outcome.READ_FAILURE,
                "lockProofFailure",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "mapped_pointer_mismatch_is_R",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    lock=replace(
                        build_lock(key, 100, 20), mapped_dst=0xDEADBEEF
                    ),
                ),
                Outcome.OVERLAP,
                Outcome.READ_FAILURE,
                "lockProofFailure",
                False,
            ),
            DeterministicExpectation(
                build_scenario(
                    "discard_does_not_retire_high_poison",
                    0,
                    RING_CAPACITY_VERTICES - 3,
                    4,
                    (PoisonRange(key, RING_CAPACITY_VERTICES - 3, 3),),
                ),
                Outcome.NO_OVERLAP,
                Outcome.NO_OVERLAP,
                "eligibleShadowComparison",
                True,
            ),
            DeterministicExpectation(
                build_scenario(
                    "invalid_discard_offset_is_R",
                    0,
                    100,
                    20,
                    (PoisonRange(key, 105, 2),),
                    lock=replace(
                        build_lock(key, 100, 20), flags=LOCK_FLAGS_DISCARD
                    ),
                ),
                Outcome.OVERLAP,
                Outcome.READ_FAILURE,
                "lockProofFailure",
                False,
            ),
        ]
    )
    return cases


def run_deterministic() -> dict[str, Any]:
    aggregate = Aggregate()
    failures: list[dict[str, Any]] = []
    discard_preserved = False
    for expected in deterministic_cases():
        result = classify_case(expected.scenario)
        aggregate.add(expected.scenario, result)
        checks = {
            "old": result.old.outcome == expected.old,
            "lock": result.lock.outcome == expected.lock,
            "settlement": result.settlement_reason == expected.settlement,
            "strict": (
                expected.strict is None
                or result.strict_exact_identity == expected.strict
            ),
            "lockMode": (
                expected.lock_mode is None
                or result.lock_mode == expected.lock_mode
            ),
        }
        if not all(checks.values()):
            failures.append(
                {
                    "name": expected.scenario.name,
                    "checks": checks,
                    "actualOld": result.old.outcome.value,
                    "actualLock": result.lock.outcome.value,
                    "actualSettlement": result.settlement_reason,
                    "actualStrict": result.strict_exact_identity,
                }
            )
        if expected.scenario.name == "discard_does_not_retire_high_poison":
            discard_preserved = (
                expected.scenario.entry_poisons
                == expected.scenario.settle_poisons
                and expected.scenario.poison_generation_entry
                == expected.scenario.poison_generation_settle
                and result.lock_mode == "discard"
            )

    # Negative fault injections prove that the model detects a missed ledger
    # generation bump and a reset transition that failed to bump the ledger
    # generation.  They are intentionally outside normal cohort matrices.
    key = format_key(0)
    missed_mutation = build_scenario(
        "negative_missed_mutation_generation",
        0,
        100,
        20,
        (PoisonRange(key, 500, 2),),
        settle_poisons=(PoisonRange(key, 105, 2),),
    )
    missed_reset = build_scenario(
        "negative_reset_without_poison_generation",
        0,
        100,
        20,
        (PoisonRange(key, 500, 2),),
        reset_requested_settle=2,
        reset_completed_settle=2,
        lock=replace(
            build_lock(key, 100, 20, reset_generation=2),
            reset_requested=2,
            reset_completed=2,
        ),
    )
    missed_mutation_result = classify_case(missed_mutation)
    missed_reset_result = classify_case(missed_reset)
    negative_detectors = {
        "missedMutationGenerationDetected": (
            missed_mutation_result.mutation_generation_violation
            and missed_mutation_result.settlement_reason
            == "mutationGenerationViolation"
            and not missed_mutation_result.comparison_eligible
        ),
        "resetGenerationBumpDetected": (
            missed_reset_result.reset_generation_violation
            and missed_reset_result.settlement_reason
            == "resetGenerationViolation"
            and not missed_reset_result.comparison_eligible
        ),
        "uint64WrapSkipsZero": advance_generation(U64_MASK) == 1,
        "discardDoesNotRetirePoison": discard_preserved,
    }
    aggregate_result = aggregate.result()
    passed = (
        not failures
        and all(negative_detectors.values())
        and aggregate_result["strictExactIdentityMatrix"]["offDiagonal"] == 0
        and aggregate_result["eligibleShadowMatrix"]["unsafeOldNLockO"] == 0
        and all(aggregate_result["closures"].values())
    )
    return {
        "passed": passed,
        "caseCount": len(deterministic_cases()),
        "failures": failures,
        "negativeDetectors": negative_detectors,
        "aggregate": aggregate_result,
    }


def random_boundary_count(rng: random.Random) -> int:
    boundaries = (
        1,
        2,
        3,
        4,
        31,
        32,
        63,
        64,
        65,
        192,
        193,
        448,
        449,
        960,
        4096,
        RING_CAPACITY_VERTICES,
    )
    if rng.random() < 0.70:
        return rng.choice(boundaries)
    return rng.randint(1, RING_CAPACITY_VERTICES)


def random_ring_next(rng: random.Random, vertex_count: int) -> int:
    boundary = max(0, RING_CAPACITY_VERTICES - vertex_count)
    candidates = {
        0,
        1,
        boundary,
        min(RING_CAPACITY_VERTICES, boundary + 1),
        RING_CAPACITY_VERTICES - 1,
        RING_CAPACITY_VERTICES,
    }
    if rng.random() < 0.75:
        return rng.choice(tuple(sorted(candidates)))
    return rng.randint(0, RING_CAPACITY_VERTICES)


def range_for_relation(
    rng: random.Random, begin: int, end: int, relation: str
) -> tuple[int, int]:
    if relation == "overlap":
        point = rng.randint(begin, max(begin, end - 1))
        base = max(0, point - rng.randint(0, 3))
        length = min(
            RING_CAPACITY_VERTICES - base,
            max(1, rng.randint(1, 16)),
        )
        return (base, length)
    if relation == "adjacent":
        if end < RING_CAPACITY_VERTICES:
            return (end, min(8, RING_CAPACITY_VERTICES - end))
        return (max(0, begin - 8), min(8, begin))
    # Select a disjoint range.  Prefer the opposite side of the ring.
    if end <= RING_CAPACITY_VERTICES // 2:
        base = rng.randint(max(end, RING_CAPACITY_VERTICES // 2),
                           RING_CAPACITY_VERTICES - 1)
        return (base, min(rng.randint(1, 16), RING_CAPACITY_VERTICES - base))
    if begin > 0:
        base = rng.randint(0, begin - 1)
        return (base, min(rng.randint(1, 16), begin - base))
    return (end, max(1, min(8, RING_CAPACITY_VERTICES - end)))


def fuzz_scenario(index: int, rng: random.Random) -> Scenario:
    output_format = rng.randrange(len(FORMAT_FVF))
    vertex_count = random_boundary_count(rng)
    ring_next = random_ring_next(rng, vertex_count)
    predicted = ring_interval(ring_next, vertex_count)
    assert predicted is not None
    begin, end, _ = predicted
    generation = rng.randint(1, 100000)
    key = format_key(output_format, generation)
    key = replace(
        key,
        native_device=0x10000000 + rng.randrange(1, 32),
        common_resource=0x20000000 + rng.randrange(1, 256) * 0x10,
        com_vertex_buffer=0x30000000 + rng.randrange(1, 256) * 0x10,
    )
    poison_count = rng.randint(0, MAX_FUZZ_POISONS)
    poisons: list[PoisonRange] = []
    for _ in range(poison_count):
        key_kind = rng.choices(
            ("exact", "aba", "otherCom", "otherFormat", "otherDevice"),
            weights=(48, 18, 14, 10, 10),
            k=1,
        )[0]
        poison_key = key
        if key_kind == "aba":
            poison_key = replace(
                key,
                common_resource=key.common_resource + 0x8000,
                resource_generation=advance_generation(key.resource_generation),
            )
        elif key_kind == "otherCom":
            poison_key = replace(
                key,
                common_resource=key.common_resource + 0x9000,
                com_vertex_buffer=key.com_vertex_buffer + 0x9000,
                resource_generation=advance_generation(key.resource_generation),
            )
        elif key_kind == "otherFormat":
            other_format = (output_format + rng.randint(1, 5)) % 6
            poison_key = replace(key, output_format=other_format)
        elif key_kind == "otherDevice":
            poison_key = replace(key, native_device=key.native_device + 0x100)
        relation = rng.choices(
            ("overlap", "disjoint", "adjacent"),
            weights=(50, 35, 15),
            k=1,
        )[0]
        base, length = range_for_relation(rng, begin, end, relation)
        poisons.append(PoisonRange(poison_key, base, length))

    base = build_scenario(
        f"fuzz_{index}",
        output_format,
        ring_next,
        vertex_count,
        tuple(poisons),
        key=key,
        source="fuzz",
        poison_generation_entry=generation,
        poison_generation_settle=generation,
        reset_requested_entry=generation,
        reset_completed_entry=generation,
        reset_requested_settle=generation,
        reset_completed_settle=generation,
        retirement_generation_entry=generation,
        retirement_generation_settle=generation,
        lock=build_lock(key, ring_next, vertex_count, reset_generation=generation),
    )
    event = rng.choices(
        (
            "stable",
            "mutation",
            "reset",
            "reentry",
            "noNormalReturn",
            "outerFailure",
            "lockFailure",
            "lockCorrupt",
            "gxReadFailure",
            "retirement",
        ),
        weights=(53, 8, 6, 7, 5, 3, 5, 4, 5, 4),
        k=1,
    )[0]
    if event == "mutation":
        mutated = list(base.settle_poisons)
        relation = "overlap" if rng.random() < 0.5 else "disjoint"
        pbase, plen = range_for_relation(rng, begin, end, relation)
        if mutated and rng.random() < 0.5:
            mutated.pop(rng.randrange(len(mutated)))
        else:
            mutated.append(PoisonRange(key, pbase, plen))
        return replace(
            base,
            settle_poisons=tuple(mutated),
            poison_generation_settle=advance_generation(generation),
        )
    if event == "reset":
        reset_generation = advance_generation(generation)
        pending = rng.random() < 0.5
        completed = generation if pending else reset_generation
        return replace(
            base,
            poison_generation_settle=reset_generation,
            reset_requested_settle=reset_generation,
            reset_completed_settle=completed,
            lock=replace(
                base.lock,
                reset_requested=reset_generation,
                reset_completed=completed,
            ),
        )
    if event == "reentry":
        new_ring = (ring_next + max(1, vertex_count)) % RING_CAPACITY_VERTICES
        return replace(
            base,
            reentry=True,
            lock=build_lock(
                key, new_ring, vertex_count, reset_generation=generation
            ),
        )
    if event == "noNormalReturn":
        return replace(base, cpu_kernel_normal_return=False)
    if event == "outerFailure":
        return replace(base, outer_call_succeeded=False)
    if event == "lockFailure":
        return replace(base, lock=replace(base.lock, lock_succeeded=False))
    if event == "lockCorrupt":
        corruption = rng.choice(("flags", "pointer", "generation", "layout"))
        if corruption == "flags":
            lock = replace(base.lock, flags=D3DLOCK_NOOVERWRITE)
        elif corruption == "pointer":
            lock = replace(base.lock, mapped_dst=base.lock.mapped_dst + 4)
        elif corruption == "generation":
            lock = replace(base.lock, resource_generation=0)
        else:
            lock = replace(base.lock, vertex_stride=base.lock.vertex_stride + 4)
        return replace(base, lock=lock)
    if event == "gxReadFailure":
        return replace(base, gx=replace(base.gx, readable=False))
    if event == "retirement":
        return replace(
            base,
            retirement_generation_settle=advance_generation(generation),
        )
    return base


def run_fuzz(case_count: int, seed: int) -> dict[str, Any]:
    rng = random.Random(seed)
    aggregate = Aggregate()
    for index in range(case_count):
        scenario = fuzz_scenario(index, rng)
        aggregate.add(scenario, classify_case(scenario))
    result = aggregate.result()
    strict = result["strictExactIdentityMatrix"]
    eligible = result["eligibleShadowMatrix"]
    passed = (
        aggregate.mutation_generation_violations == 0
        and aggregate.reset_generation_violations == 0
        and strict["offDiagonal"] == 0
        and eligible["unsafeOldNLockO"] == 0
        and aggregate.max_live_poisons <= MAX_FUZZ_POISONS + 1
        and all(result["closures"].values())
    )
    return {
        "passed": passed,
        "seed": seed,
        "caseCount": case_count,
        "aggregate": result,
    }


def contract_report() -> dict[str, Any]:
    return {
        "ringCapacityVertices": RING_CAPACITY_VERTICES,
        "wrapRule": "wrap iff oldNext > 16384 - vertexCount",
        "rangeRule": "half-open [begin,end)",
        "formats": [
            {
                "outputFormat": index,
                "fvf": f"0x{FORMAT_FVF[index]:03X}",
                "stride": FORMAT_STRIDE[index],
            }
            for index in range(len(FORMAT_FVF))
        ],
        "poisonExactKey": [
            "nativeD3DDevice",
            "D3D9CommonBuffer",
            "COMVertexBuffer",
            "resourceGeneration",
            "outputFormat",
        ],
        "oldGxPhysicalKey": [
            "nativeD3DDevice",
            "COMVertexBuffer",
            "outputFormat",
        ],
        "lockProofRequired": [
            "successful active Lock with lockCount=1",
            "exact common/COM/resource generation identity",
            "exact FVF/stride/format",
            "exact byte offset/size and mapped pointer",
            "NOOVERWRITE|NOSYSLOCK or DISCARD|NOSYSLOCK",
            "nonzero storage/map allocation generations",
            "quiescent exact reset generation",
        ],
        "shadowOnly": True,
        "authorizationChanges": 0,
        "discardIsRetirement": False,
        "runtimePerformanceClaim": False,
    }


def parse_int(value: str) -> int:
    return int(value, 0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fuzz-cases",
        type=int,
        default=50_000,
        help="bounded deterministic fuzz case count (default: 50000)",
    )
    parser.add_argument(
        "--seed",
        type=parse_int,
        default=0x127A5EED,
        help="integer seed, decimal or 0x-prefixed",
    )
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=DEFAULT_ARTIFACT_ROOT,
        help="directory under which the JSON artifact is created",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.fuzz_cases < 0 or args.fuzz_cases > 1_000_000:
        raise ValueError("--fuzz-cases must be in [0, 1000000]")
    deterministic = run_deterministic()
    fuzz = run_fuzz(args.fuzz_cases, args.seed)
    overall_pass = deterministic["passed"] and fuzz["passed"]
    report = {
        "schemaVersion": 1,
        "model": "gpu_skin_outside_d3d9_lock_shadow",
        "generatedAtLocal": datetime.now().isoformat(timespec="seconds"),
        "contract": contract_report(),
        "bounds": {
            "fuzzCases": args.fuzz_cases,
            "maxFuzzPoisonsPerEntry": MAX_FUZZ_POISONS,
            "maxSavedExamples": MAX_SAVED_EXAMPLES,
            "childProcessesSpawned": 0,
            "war3Launched": False,
            "buildPerformed": False,
            "deployPerformed": False,
        },
        "deterministic": deterministic,
        "fuzz": fuzz,
        "verdict": {
            "passed": overall_pass,
            "meaning": (
                "Offline contract/model closure only; no runtime authorization "
                "or performance conclusion."
            ),
            "promotionPrerequisites": [
                "runtime shadow-only old/new matrix and lifecycle counters close",
                "eligible old-N/Lock-O remains zero",
                "mutation/reset/reentry/normal-return invalidations are fail-closed",
                "no authorization change before a separate reviewed promotion",
            ],
        },
    }

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    artifact_dir = args.artifact_root / (
        f"gpu_skin_outside_lock_shadow_offline_{timestamp}"
    )
    artifact_dir.mkdir(parents=True, exist_ok=False)
    result_path = artifact_dir / "result.json"
    result_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    summary = {
        "artifact": str(result_path),
        "passed": overall_pass,
        "deterministicCases": deterministic["caseCount"],
        "fuzzCases": args.fuzz_cases,
        "eligibleMatrix": fuzz["aggregate"]["eligibleShadowMatrix"]["cells"],
        "strictOffDiagonal": fuzz["aggregate"][
            "strictExactIdentityMatrix"
        ]["offDiagonal"],
        "eligibleUnsafeOldNLockO": fuzz["aggregate"][
            "eligibleShadowMatrix"
        ]["unsafeOldNLockO"],
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0 if overall_pass else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # Keep an offline model failure explicit in CI.
        print(f"error: {exc}", file=sys.stderr)
        raise
