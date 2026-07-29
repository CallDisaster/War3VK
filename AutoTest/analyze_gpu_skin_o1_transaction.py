#!/usr/bin/env python3
"""Bounded model for the outside-poison O1 Lock transaction.

This is deliberately independent of Warcraft III.  It models the only new
authority being considered by the native bridge: an outer upload may begin as
a provisional CPU-only transaction, but poison can be removed only after one
exact successful D3D9 Lock, a normal CPU-kernel return, one exact Unlock, and
normal outer settlement.  Unknown paths retain poison and therefore suppress
the affected DIP.
"""

from __future__ import annotations

import argparse
import json
import random
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


@dataclass(frozen=True)
class Interval:
    begin: int
    end: int

    def valid(self) -> bool:
        return 0 <= self.begin < self.end <= 0x4000

    def overlaps(self, other: "Interval") -> bool:
        return self.end > other.begin and self.begin < other.end


@dataclass(frozen=True)
class Scenario:
    poison: Interval
    lock: Interval
    descriptor_exact: bool
    identity_exact: bool
    mapped_pointer_exact: bool
    ledger_stable_lock_to_kernel: bool
    ledger_stable_kernel_to_settle: bool
    kernel_normal_return: bool
    skin_mode_writes: bool
    unlock_exact: bool
    outer_succeeded: bool
    direct_discard_exact: bool
    # The outer hook may be unable to derive bytes from optional source
    # pointers. Successful-Lock FVF/descriptor/range remains independent.
    outer_layout_known: bool = True


@dataclass
class Result:
    lock_verdict: str
    poison_cleared: bool
    dip_suppressed: bool
    stale_poison_consumed: bool
    valid_cpu_rewrite_suppressed: bool


def run_scenario(s: Scenario) -> Result:
    assert s.poison.valid() and s.lock.valid()

    # An exact DIRECT DISCARD retires the old app-visible allocation before the
    # successful Lock evidence is frozen.  The new allocation therefore starts
    # with no old poison interval.
    poison_live = not s.direct_discard_exact
    # outer_layout_known is intentionally absent: the real Lock derives the
    # format and exact vertex range. A known non-zero outer byte count is only
    # an additional equality, never a prerequisite for this authority.
    exact_lock = (
        s.descriptor_exact
        and s.identity_exact
        and s.mapped_pointer_exact
        and s.ledger_stable_lock_to_kernel
    )
    overlaps = poison_live and s.poison.overlaps(s.lock)
    if not exact_lock:
        lock_verdict = "unprovable"
    elif overlaps:
        lock_verdict = "overlap"
    else:
        lock_verdict = "no_overlap"

    cpu_rewrite_valid = (
        exact_lock
        and s.kernel_normal_return
        and s.skin_mode_writes
        and s.unlock_exact
        and s.outer_succeeded
    )
    may_clear = (
        lock_verdict == "overlap"
        and cpu_rewrite_valid
        and s.ledger_stable_kernel_to_settle
    )
    poison_cleared = poison_live and may_clear
    if poison_cleared:
        poison_live = False

    # The draw-side poison gate is authoritative.  Any proof failure retains
    # poison, which can over-suppress a valid CPU rewrite but cannot consume the
    # stale GPU-owned interval.
    dip_suppressed = poison_live and s.poison.overlaps(s.lock)
    stale_poison_consumed = (
        not dip_suppressed
        and s.poison.overlaps(s.lock)
        and not s.direct_discard_exact
        and not cpu_rewrite_valid
    )
    valid_cpu_rewrite_suppressed = dip_suppressed and cpu_rewrite_valid
    return Result(
        lock_verdict=lock_verdict,
        poison_cleared=poison_cleared,
        dip_suppressed=dip_suppressed,
        stale_poison_consumed=stale_poison_consumed,
        valid_cpu_rewrite_suppressed=valid_cpu_rewrite_suppressed,
    )


def deterministic_cases() -> list[tuple[str, Scenario]]:
    p = Interval(100, 180)
    return [
        (
            "exact_no_overlap",
            Scenario(p, Interval(200, 240), True, True, True, True, True,
                     True, True, True, True, False),
        ),
        (
            "exact_overlap_rewrite",
            Scenario(p, Interval(120, 160), True, True, True, True, True,
                     True, True, True, True, False),
        ),
        (
            "kernel_fault_retains",
            Scenario(p, Interval(120, 160), True, True, True, True, True,
                     False, True, True, True, False),
        ),
        (
            "unlock_failure_retains",
            Scenario(p, Interval(120, 160), True, True, True, True, True,
                     True, True, False, True, False),
        ),
        (
            "mutation_retains",
            Scenario(p, Interval(120, 160), True, True, True, True, False,
                     True, True, True, True, False),
        ),
        (
            "identity_mismatch_retains",
            Scenario(p, Interval(120, 160), True, False, True, True, True,
                     True, True, True, True, False),
        ),
        (
            "exact_discard_retires_old",
            Scenario(p, Interval(0, 40), True, True, True, True, True,
                     True, True, True, True, True),
        ),
        (
            "outer_layout_unknown_exact_no_overlap",
            Scenario(p, Interval(200, 240), True, True, True, True, True,
                     True, True, True, True, False, False),
        ),
        (
            "outer_layout_unknown_exact_overlap_rewrite",
            Scenario(p, Interval(120, 160), True, True, True, True, True,
                     True, True, True, True, False, False),
        ),
    ]


def random_interval(rng: random.Random) -> Interval:
    begin = rng.randrange(0, 0x4000)
    end = rng.randrange(begin + 1, 0x4001)
    return Interval(begin, end)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fuzz", type=int, default=100_000)
    parser.add_argument("--seed", type=int, default=0x127A01)
    parser.add_argument("--artifact-root", type=Path,
                        default=Path("AutoTest/artifacts"))
    args = parser.parse_args()

    deterministic = []
    for name, scenario in deterministic_cases():
        result = run_scenario(scenario)
        if result.stale_poison_consumed:
            raise AssertionError(f"unsafe deterministic case: {name}")
        deterministic.append({
            "name": name,
            "scenario": asdict(scenario),
            "result": asdict(result),
        })

    rng = random.Random(args.seed)
    verdicts = {"no_overlap": 0, "overlap": 0, "unprovable": 0}
    poison_clears = 0
    conservative_suppressions = 0
    for i in range(args.fuzz):
        bits = [bool(rng.getrandbits(1)) for _ in range(10)]
        scenario = Scenario(
            poison=random_interval(rng),
            lock=random_interval(rng),
            descriptor_exact=bits[0],
            identity_exact=bits[1],
            mapped_pointer_exact=bits[2],
            ledger_stable_lock_to_kernel=bits[3],
            ledger_stable_kernel_to_settle=bits[4],
            kernel_normal_return=bits[5],
            skin_mode_writes=bits[6],
            unlock_exact=bits[7],
            outer_succeeded=bits[8],
            direct_discard_exact=(rng.randrange(32) == 0),
            outer_layout_known=bits[9],
        )
        result = run_scenario(scenario)
        if result.stale_poison_consumed:
            raise AssertionError(
                f"unsafe fuzz case {i}: {scenario!r} -> {result!r}")
        verdicts[result.lock_verdict] += 1
        poison_clears += int(result.poison_cleared)
        conservative_suppressions += int(result.valid_cpu_rewrite_suppressed)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    artifact = args.artifact_root / f"gpu_skin_o1_transaction_offline_{stamp}"
    artifact.mkdir(parents=True, exist_ok=False)
    payload = {
        "contract": {
            "gpuBypassAuthority": 0,
            "clearRequires": [
                "exact successful D3D9 Lock identity/range",
                "stable poison ledger through settlement",
                "normal original CPU kernel return with skin mode 0/1",
                "exact successful Unlock",
                "successful outer settlement",
            ],
            "failurePolicy": "retain poison and suppress affected DIP",
        },
        "seed": args.seed,
        "fuzzCases": args.fuzz,
        "verdicts": verdicts,
        "poisonClears": poison_clears,
        "conservativeSuppressions": conservative_suppressions,
        "unsafeCases": 0,
        "deterministic": deterministic,
    }
    (artifact / "result.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
    print(artifact)
    print(json.dumps({
        "unsafeCases": 0,
        "fuzzCases": args.fuzz,
        "verdicts": verdicts,
        "poisonClears": poison_clears,
        "conservativeSuppressions": conservative_suppressions,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
