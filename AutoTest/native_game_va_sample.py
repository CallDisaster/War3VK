"""Bounded read-only low-VA census through an existing NativeObserver handle.

No process reopen, suspension, payload read, allocation in the target, debugger
or input. VirtualQueryEx describes regions, not allocation owners. One scan is
not an atomic point-in-time map and cannot prove why a past allocation failed.
The deadline is cooperative: a synchronous Windows call cannot be cancelled.
Only a complete, identity-revalidated scan covers the entire stated interval.
State totals and mapping-type totals are separate axes; never add both totals.
"""
from __future__ import annotations

import copy
import ctypes as ct
import math
import os
import time


LOW_BEGIN = 0x10000
LOW_END = 0x80000000
ALLOCATION_ALIGNMENT = 0x10000
MAX_QUERIES = 32768
DEFAULT_BUDGET_SECONDS = 0.250
MAX_BUDGET_SECONDS = 1.0
MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x20000, 0x40000, 0x1000000
STATE_NAMES = {MEM_COMMIT: "committed", MEM_RESERVE: "reserved", MEM_FREE: "free"}
TYPE_NAMES = {MEM_PRIVATE: "private", MEM_MAPPED: "mapped", MEM_IMAGE: "image"}


class _MemoryBasicInformation(ct.Structure):
    _fields_ = [("BaseAddress", ct.c_void_p), ("AllocationBase", ct.c_void_p),
        ("AllocationProtect", ct.c_uint32), ("PartitionId", ct.c_uint16),
        ("RegionSize", ct.c_size_t), ("State", ct.c_uint32),
        ("Protect", ct.c_uint32), ("Type", ct.c_uint32)]


def _require(value, message):
    if not value:
        raise ValueError(message)


class _WindowsVaApi:
    def __init__(self):
        _require(os.name == "nt" and ct.sizeof(ct.c_void_p) == 8 and
            ct.sizeof(_MemoryBasicInformation) == 48 and
            _MemoryBasicInformation.RegionSize.offset == 24,
            "64-bit Windows observer with exact MEMORY_BASIC_INFORMATION layout required")
        self.k = ct.WinDLL("kernel32", use_last_error=True)
        self.k.GetProcessId.restype = ct.c_uint32
        self.k.GetProcessId.argtypes = (ct.c_void_p,)
        self.k.VirtualQueryEx.restype = ct.c_size_t
        self.k.VirtualQueryEx.argtypes = (ct.c_void_p, ct.c_void_p,
            ct.POINTER(_MemoryBasicInformation), ct.c_size_t)
        self._info = _MemoryBasicInformation()

    def pid(self, process):
        return int(self.k.GetProcessId(process))

    def query(self, process, address):
        count = self.k.VirtualQueryEx(process, address, ct.byref(self._info), ct.sizeof(self._info))
        _require(count == ct.sizeof(self._info),
            "VirtualQueryEx failed/incomplete: Win32=" + str(ct.get_last_error()))
        return dict(base=int(self._info.BaseAddress or 0), size=int(self._info.RegionSize),
                    state=int(self._info.State), type=int(self._info.Type))


def _verify(native, api, process, expected):
    _require(native.process == process and native.expected == expected and not native.pending,
             "retained process authority changed or has unsettled IO")
    # Existing owner validates liveness, exact creation time and image path.
    native.verify_process()
    _require(native.process == process and native.expected == expected and not native.pending,
             "retained process authority changed during verification")
    _require(api.pid(process) == expected["pid"], "retained process PID mismatch")


def _region(row, cursor):
    _require(type(row) is dict and all(type(row.get(key)) is int for key in
        ("base", "size", "state", "type")), "malformed VA region")
    base, size = row["base"], row["size"]
    _require(0 <= base < 1 << 64 and 0 < size < 1 << 64 and
        base <= cursor < base + size < 1 << 64, "VA region does not contain/progress past query")
    _require(row["state"] in STATE_NAMES, "unknown VA state")
    if row["state"] == MEM_FREE:
        _require(row["type"] == 0, "free VA region has a mapping type")
    else:
        _require(row["type"] in TYPE_NAMES, "unknown allocated VA mapping type")
    return min(LOW_END, base + size), STATE_NAMES[row["state"]]


def _account_hole(result, begin, end):
    aligned = (begin + ALLOCATION_ALIGNMENT - 1) & ~(ALLOCATION_ALIGNMENT - 1)
    usable = max(0, end - aligned)
    if usable > result["largestAlignedFreeHoleBytes"]:
        result.update(largestAlignedFreeHoleBytes=usable,
                      largestAlignedFreeHoleStart=aligned,
                      largestAlignedFreeHoleEnd=end)


def sample(native, *, budget_seconds=DEFAULT_BUDGET_SECONDS, _api=None, _clock=time.monotonic):
    """Return one bounded metadata-only sample; the caller owns receipt export.

    The retained process is never reopened or closed here. At most one attempt
    per observer is permitted. On timeout, malformed data, query error, identity
    change or query-limit exhaustion, totals are explicitly partial. The largest
    hole has its START rounded up to 64 KiB, not its usable length rounded up.
    No claim about global OOM or TLS ownership follows from this observation.
    """
    result = dict(schema="warvk-native-low-va-sample/v1", complete=False,
        rangeBegin=LOW_BEGIN, rangeEnd=LOW_END, queryLimit=MAX_QUERIES, queries=0,
        coveredBytes=0, nextAddress=LOW_BEGIN, regionCount=0,
        states={name: dict(bytes=0, regions=0) for name in STATE_NAMES.values()},
        types={name: dict(bytes=0, regions=0, committedBytes=0, reservedBytes=0)
               for name in TYPE_NAMES.values()},
        freeBytes=0, largestAlignedFreeHoleBytes=0,
        largestAlignedFreeHoleStart=None, largestAlignedFreeHoleEnd=None,
        alignedHoleSemantics="start rounded up to 64KiB; usable bytes to free-region end",
        identityVerifiedBefore=False, identityVerifiedAfter=False,
        snapshotAtomic=False, allocationFailureCauseProven=False,
        payloadRead=False, targetSuspended=False, processReopened=False,
        deadlineIsCooperative=True, stopReason="qualification")
    process, expected, api, started = None, None, None, None
    cursor, hole_begin, hole_end = LOW_BEGIN, None, None
    try:
        _require(type(budget_seconds) in (int, float) and math.isfinite(budget_seconds) and
            0 < budget_seconds <= MAX_BUDGET_SECONDS, "invalid bounded query deadline")
        result["budgetSeconds"] = budget_seconds
        process, expected = native.process, copy.deepcopy(native.expected)
        _require(type(process) is int and process > 0 and type(expected) is dict and
            type(expected.get("pid")) is int and 0 < expected["pid"] < 1 << 32 and
            type(expected.get("creation100ns")) is int and 0 < expected["creation100ns"] < 1 << 64 and
            type(expected.get("actualExecutable")) is str and expected["actualExecutable"],
            "bound exact process identity required")
        _require(not getattr(native, "_low_va_sample_attempted", False),
                 "low VA sample already attempted for this observer")
        api = _api if _api is not None else _WindowsVaApi()
        _verify(native, api, process, expected)
        result["identityVerifiedBefore"] = True
        native._low_va_sample_attempted = True
        result["processIdentity"] = expected
        started = _clock()
        deadline = started + budget_seconds
        result["stopReason"] = "range-complete"
        while cursor < LOW_END:
            if result["queries"] >= MAX_QUERIES:
                result["stopReason"] = "query-limit"
                break
            if _clock() >= deadline:
                result["stopReason"] = "deadline"
                break
            result["queries"] += 1
            row = api.query(process, cursor)
            end, state = _region(row, cursor)
            count = end - cursor
            result["states"][state]["bytes"] += count
            result["states"][state]["regions"] += 1
            result["regionCount"] += 1
            if state == "free":
                result["freeBytes"] += count
                if hole_begin is None:
                    hole_begin = cursor
                hole_end = end
            else:
                if hole_begin is not None:
                    _account_hole(result, hole_begin, hole_end)
                    hole_begin = hole_end = None
                mapping = result["types"][TYPE_NAMES[row["type"]]]
                mapping["bytes"] += count
                mapping["regions"] += 1
                mapping[state + "Bytes"] += count
            cursor = end
            result["coveredBytes"], result["nextAddress"] = cursor - LOW_BEGIN, cursor
            if _clock() >= deadline:
                # This also rejects a full-span query returning too late. The
                # API is synchronous; no killed-worker timeout is fabricated.
                result["stopReason"] = "deadline"
                break
        if hole_begin is not None:
            _account_hole(result, hole_begin, hole_end)
        _require(sum(row["bytes"] for row in result["states"].values()) == result["coveredBytes"] and
            sum(row["bytes"] for row in result["types"].values()) ==
                result["coveredBytes"] - result["freeBytes"], "VA accounting invariant failed")
    except Exception as error:
        result.update(stopReason="error", error=f"{type(error).__name__}: {error}")
        if hole_begin is not None:
            _account_hole(result, hole_begin, hole_end)
    finally:
        if result["identityVerifiedBefore"]:
            try:
                _verify(native, api, process, expected)
                result["identityVerifiedAfter"] = True
            except Exception as error:
                result.update(stopReason="identity-changed", identityError=f"{type(error).__name__}: {error}")
        if started is not None:
            result["elapsedSeconds"] = max(0.0, _clock() - started)
            if result["elapsedSeconds"] >= budget_seconds and result["stopReason"] == "range-complete":
                result["stopReason"] = "deadline"
    result["complete"] = (cursor == LOW_END and result["stopReason"] == "range-complete" and
        result["identityVerifiedBefore"] and result["identityVerifiedAfter"])
    result["totalsArePartial"] = not result["complete"]
    return result
