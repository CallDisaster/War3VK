#!/usr/bin/env python3
"""Strict POST-EXIT reader for the bounded MDLDATA Game-import journal.

This file is not a live shared-memory reader. The owning runner must establish
that its exact process has exited before calling ``analyze_file``; the CLI's
--process-exited is an explicit assertion, not a process-liveness check.

Observed returns and node fields are not allocator-route or object-liveness
proof. Pre-install history, non-Game imports and other generic unlink callers
are excluded by construction. Therefore certification is ALWAYS false, even
for an internally complete journal with no refusals. A missing Free is never
interpreted as proof that an allocation remained live.

Wire v1: little-endian Header[64] + Event[16384][32], all uint32. Events publish
their sequence last, followed by the header count. Malformed fields raise;
partial publication/count disagreements produce an explicit incomplete report
and only the contiguous declared committed prefix is used for observations.
Header[23..26] stores the EXPECTED PE profile, not observed PE metadata on a
rejected install. A high-bit link field may be a valid complemented sentinel;
numeric equality with any recorded return is not proof that it is a live pointer.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import sys
from typing import Any


SCHEMA = "warvk-mdldata-game-import-trace/v1"
MAGIC = 0x4D444C54
VERSION = 1
HEADER_WORDS = 64
EVENT_WORDS = 32
CAPACITY = 16384
HEADER_BYTES = HEADER_WORDS * 4
EVENT_BYTES = EVENT_WORDS * 4
FILE_BYTES = HEADER_BYTES + CAPACITY * EVENT_BYTES
REQUIRED_HOOK_MASK = 0x3F
REQUIRED_COVERAGE_FLAGS = 0x0F
KNOWN_FACTS = 0x1F
UINT32_MAX = 0xFFFFFFFF
EXAMPLE_LIMIT = 64
NODE_PAYLOAD_FIRST_WORD = 14
# Game+12D951: lea edx,[node+0xC]; +12D95B: mov ecx,[edx+4].
# The global-chain backlink is node+0x10, not bucket next(+4) or backlink(+8).
GLOBAL_BACKLINK_NODE_OFFSET = 0x10
GLOBAL_BACKLINK_EVENT_WORD = NODE_PAYLOAD_FIRST_WORD + GLOBAL_BACKLINK_NODE_OFFSET // 4

KINDS = {
    1: "Alloc", 2: "FreeBegin", 3: "FreeEnd", 4: "ReallocBegin",
    5: "ReallocEnd", 6: "BucketReturn", 7: "InsertBegin", 8: "InsertEnd",
    9: "UnlinkBegin", 10: "UnlinkEnd",
}
INSTALL_STATES = {
    0: "new", 1: "prepared", 2: "installed", 3: "versionRejected",
    4: "installFailed", 5: "partialUnsafe",
}
COUNTERS = {
    13: "busy", 14: "busyOverflow", 15: "journalFull", 16: "directoryFull",
    17: "staleCompletion", 18: "unknownLifetime", 19: "counterOverflow",
    20: "overlapConflict", 21: "readFailures",
}
COVERAGE_EXCLUSIONS = (
    "preinstallUnknown", "gameThunkOnly", "otherUnlinkUnobserved",
    "nativeRouteUnknown",
)
BOUNDARY_KINDS = frozenset((6, 7, 8, 9, 10))
BEGIN_TO_END = {2: 3, 4: 5}
END_TO_BEGIN = {3: 2, 5: 4}
_HEADER = struct.Struct("<64I")
_EVENT = struct.Struct("<32I")


class TraceFormatError(ValueError):
    """The binary cannot be accepted as this wire contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise TraceFormatError(message)


def _high(address: int) -> bool:
    return address >= 0x80000000


def _event_shape(words: tuple[int, ...], slot: int) -> None:
    """Check a published record, without treating its contents as live memory."""
    prefix = f"event[{slot}]"
    kind, facts = words[1], words[13]
    _require(kind in KINDS, f"{prefix}: unknown kind {kind}")
    _require(words[2] != 0, f"{prefix}: zero thread id")
    _require(facts & ~KNOWN_FACTS == 0, f"{prefix}: unknown factsKnown bits")
    _require(not any(words[25:]), f"{prefix}: nonzero reserved event word")
    if not facts & 1:
        _require(not any(words[14:19]), f"{prefix}: node words without node-read fact")
    if not facts & 2:
        _require(not any(words[19:22]), f"{prefix}: table words without table-read fact")
    if kind == 5:
        _require(not facts & 4, f"{prefix}: ReallocEnd cannot carry old-head fields")
        _require(not any(words[22:24]), f"{prefix}: ReallocEnd words22..23 must be zero")
        _require(words[5] == words[12], f"{prefix}: ReallocEnd returned-address mirror mismatch")
        _require(words[5] != 0 or words[24] == 0,
                 f"{prefix}: failed realloc has a new allocation serial")
    else:
        if not facts & 4:
            _require(not any(words[22:25]), f"{prefix}: old-head words without read fact")
        _require(not facts & 16, f"{prefix}: releasedOld fact outside ReallocEnd")
    if kind == 1:
        _require(words[4] == words[12], f"{prefix}: Alloc returned-address mirror mismatch")
        _require(words[7] == 0, f"{prefix}: Alloc must not carry operation token")
        _require(words[4] != 0 or words[6] == 0,
                 f"{prefix}: failed allocation has an allocation serial")
    if kind in BOUNDARY_KINDS:
        _require(words[7] == 0, f"{prefix}: boundary token must be zero")
    if kind in (6, 8):
        _require(words[4] == words[12], f"{prefix}: returned-node mirror mismatch")
    if kind == 7:
        _require(words[4] == 0 and words[6] == 0,
                 f"{prefix}: InsertBegin cannot identify the not-yet-created node")
    if kind in BEGIN_TO_END or kind in END_TO_BEGIN:
        _require(words[7] != 0, f"{prefix}: allocation operation token must be nonzero")


def analyze_bytes(data: bytes, *, process_exited: bool,
                  expected_pid: int | None = None,
                  expected_creation_time: int | None = None) -> dict[str, Any]:
    """Analyze bytes only after the exact owning process has been reaped.

``process_exited`` deliberately has no default. The assertion does not prove
normal exit or a successful map load and is echoed as such in the report.
"""
    _require(process_exited is True, "post-exit ownership assertion is required")
    _require(len(data) == FILE_BYTES,
             f"exact file size required: expected {FILE_BYTES}, got {len(data)}")
    header = _HEADER.unpack_from(data)
    _require(header[0] == MAGIC, "wrong magic")
    _require(header[1] == VERSION, "unsupported version")
    _require(header[2:5] == (HEADER_WORDS, EVENT_WORDS, CAPACITY),
             "header/event/capacity dimensions do not match v1")
    _require(header[5] <= CAPACITY, "committed count exceeds capacity")
    _require(header[11] in INSTALL_STATES, "unknown installStatus")
    _require(header[12] & ~REQUIRED_HOOK_MASK == 0, "unknown hookMask bits")
    _require(header[14] in (0, 1), "busyOverflow must be a sticky boolean")
    _require(header[19] in (0, 1), "counterOverflow must be a sticky boolean")
    _require(header[22] == REQUIRED_COVERAGE_FLAGS,
             "coverage exclusions must be exactly the v1 required flags")
    _require(not any(header[27:]), "nonzero reserved header word")
    creation_time = header[7] | (header[8] << 32)
    if expected_pid is not None:
        _require(type(expected_pid) is int and 0 < expected_pid <= UINT32_MAX,
                 "expected pid must be a positive uint32")
        _require(header[6] == expected_pid, "process pid does not match expected owner")
    if expected_creation_time is not None:
        _require(type(expected_creation_time) is int and 0 < expected_creation_time < 1 << 64,
                 "expected creation time must be a positive uint64")
        _require(creation_time == expected_creation_time,
                 "process creation time does not match expected owner")

    # The count may lag an event's last-word publication after a crash. Never
    # silently include an orphaned event or jump over a broken declared slot.
    gaps: list[dict[str, Any]] = []
    prefix: list[tuple[int, ...]] = []
    prefix_open = True
    undeclared_nonzero = 0
    undeclared_committed = 0
    for index in range(CAPACITY):
        words = _EVENT.unpack_from(data, HEADER_BYTES + index * EVENT_BYTES)
        if index < header[5]:
            if words[0] != index + 1:
                gaps.append({"reason": "commitSequenceMismatch", "slot": index,
                             "expected": index + 1, "observed": words[0]})
                prefix_open = False
            # An entirely missing slot is an incomplete publication, not a
            # fictitious kind=0. Any actually published slot must be well-formed.
            if words[0] != 0:
                _event_shape(words, index)
            elif any(words[25:]):
                raise TraceFormatError(f"event[{index}]: nonzero reserved event word")
            if prefix_open:
                prefix.append(words)
        elif any(words):
            undeclared_nonzero += 1
            if words[0]:
                undeclared_committed += 1
                _event_shape(words, index)
            else:
                _require(not any(words[25:]),
                         f"event[{index}]: nonzero reserved event word")
    if undeclared_nonzero:
        gaps.append({"reason": "dataBeyondCommittedCount",
                     "slots": undeclared_nonzero, "publishedSlots": undeclared_committed})
    if header[11] != 2:
        gaps.append({"reason": "hooksNotInstalled", "state": INSTALL_STATES[header[11]]})
    if header[12] != REQUIRED_HOOK_MASK:
        gaps.append({"reason": "hookCoverageIncomplete", "observedMask": header[12],
                     "requiredMask": REQUIRED_HOOK_MASK})
    if header[6] == 0 or creation_time == 0:
        gaps.append({"reason": "processIdentityMissing"})
    if not all(header[index] for index in (9, 10, 23, 24, 25, 26)):
        gaps.append({"reason": "moduleBaseOrExpectedProfileMissing"})
    counters = {name: header[index] for index, name in COUNTERS.items()}
    for name, value in counters.items():
        if value:
            gaps.append({"reason": "observationCounterNonzero", "counter": name, "value": value})

    kinds: Counter[str] = Counter()
    # History is indexed by serial, never an address-only 'currently live' map.
    # This preserves separate address-reuse incarnations without inventing a
    # physical lifetime from the absence of unobserved frees.
    returns: dict[int, dict[str, int | str]] = {}
    returned_addresses: set[int] = set()
    pending: dict[int, tuple[int, int, int, int]] = {}
    seen_tokens: set[int] = set()
    return_examples: list[dict[str, Any]] = []
    high_return_examples: list[dict[str, Any]] = []
    high_boundary_examples: list[dict[str, Any]] = []
    high_backlink_examples: list[dict[str, Any]] = []
    count: Counter[str] = Counter()

    def issue(reason: str, event: tuple[int, ...], **values: Any) -> None:
        gaps.append({"reason": reason, "sequence": event[0], **values})

    def returned(event: tuple[int, ...], pointer: int, serial: int, name: str) -> None:
        if pointer == 0:
            count[f"failed{name}Returns"] += 1
            return
        count["nonzeroAllocationReturns"] += 1
        returned_addresses.add(pointer)
        is_high = _high(pointer)
        if is_high:
            count[f"high{name}Returns"] += 1
        record = {"sequence": event[0], "kind": name, "pointer": pointer,
                  "requestedBytes": event[8], "allocationSerial": serial,
                  "thread": event[2], "callerRva": event[3],
                  "sourceRva": event[10], "highAddress": is_high,
                  "exclusiveEnd64": pointer + event[8]}
        # This is only the arithmetic request interval. It is not a committed
        # region, usable-block size, allocator ownership, or lifetime extent.
        if pointer + event[8] > 1 << 32:
            issue("requestedIntervalWraps32BitAddressSpace", event)
        if serial == 0:
            issue("returnedAddressWithoutAllocationSerial", event)
        elif serial in returns:
            issue("allocationSerialReused", event, allocationSerial=serial)
        else:
            returns[serial] = record
        if len(return_examples) < EXAMPLE_LIMIT:
            return_examples.append(record)
        if is_high and len(high_return_examples) < EXAMPLE_LIMIT:
            high_return_examples.append(record)

    for event in prefix:
        kind, pointer, serial, token = event[1], event[4], event[6], event[7]
        kinds[KINDS[kind]] += 1
        if kind == 1:
            returned(event, pointer, serial, "Alloc")
        elif kind == 5:
            returned(event, event[5], event[24], "Realloc")
        if kind != 1 and not (kind == 7 and pointer == 0):
            if serial == 0:
                count["eventsWithUnknownAllocationSerial"] += 1
                if pointer:
                    issue("unknownAllocationSerial", event)
            elif serial not in returns:
                count["eventsWithUnmatchedAllocationSerial"] += 1
                issue("allocationSerialNotInRecordedReturnHistory", event, allocationSerial=serial)
            elif returns[serial]["pointer"] != pointer:
                issue("allocationSerialPointerMismatch", event, allocationSerial=serial)
            else:
                count["eventsMatchedToRecordedReturn"] += 1
                if kind in BOUNDARY_KINDS:
                    count["boundaryEventsMatchedToRecordedReturn"] += 1
        if kind in BEGIN_TO_END:
            if token in seen_tokens:
                issue("operationTokenReused", event, operationToken=token)
            else:
                seen_tokens.add(token)
                pending[token] = (kind, pointer, serial, event[0])
        elif kind in END_TO_BEGIN:
            beginning = pending.get(token)
            if beginning is None:
                issue("operationEndWithoutBegin", event, operationToken=token)
            elif beginning[:3] != (END_TO_BEGIN[kind], pointer, serial):
                issue("operationBeginEndMismatch", event, operationToken=token)
            else:
                del pending[token]
                count["matchedOperationPairs"] += 1
        if kind in BOUNDARY_KINDS and _high(pointer):
            count["highNodeBoundaryEvents"] += 1
            if len(high_boundary_examples) < EXAMPLE_LIMIT:
                matched = serial in returns and returns[serial]["pointer"] == pointer
                boundary = {"sequence": event[0], "kind": KINDS[kind],
                    "pointer": pointer, "allocationSerial": serial,
                    "matchedRecordedReturn": matched}
                if matched:
                    boundary["recordedReturnSequence"] = returns[serial]["sequence"]
                    boundary["recordedReturnKind"] = returns[serial]["kind"]
                    boundary["requestedBytes"] = returns[serial]["requestedBytes"]
                high_boundary_examples.append(boundary)
        if kind == 9 and event[13] & 1 and _high(event[GLOBAL_BACKLINK_EVENT_WORD]):
            raw_backlink = event[GLOBAL_BACKLINK_EVENT_WORD]
            count["preUnlinkHighBitGlobalBacklinkEvents"] += 1
            if len(high_backlink_examples) < EXAMPLE_LIMIT:
                high_backlink_examples.append({"sequence": event[0], "node": pointer,
                    "allocationSerial": serial, "nodePlus16": raw_backlink,
                    "nodeWords": list(event[14:19]),
                    "rawHighBitFieldNotNecessarilyPointer": True,
                    "matchesEarlierRecordedReturnedAddress": raw_backlink in returned_addresses})
        if event[13] & 8:
            count["readFailureEvents"] += 1
            issue("eventReadFailure", event)

    pending_ops = [{"operationToken": token, "kind": KINDS[value[0]],
                    "pointer": value[1], "allocationSerial": value[2],
                    "beginSequence": value[3]} for token, value in sorted(pending.items())]
    if pending_ops:
        gaps.append({"reason": "pendingOperationsAtExit", "count": len(pending_ops)})
    if not prefix:
        gaps.append({"reason": "noEventsObserved"})
    for sample in high_backlink_examples:
        sample["matchesAnyRecordedReturnedAddress"] = sample["nodePlus16"] in returned_addresses
    observations = {
        "hasHighAllocationReturn": bool(count["highAllocReturns"]),
        "hasHighReallocationReturn": bool(count["highReallocReturns"]),
        "hasAnyHighReturnedAddress": bool(count["highAllocReturns"] + count["highReallocReturns"]),
        "hasHighNodeBoundary": bool(count["highNodeBoundaryEvents"]),
        "hasPreUnlinkHighBitGlobalBacklinkField": bool(count["preUnlinkHighBitGlobalBacklinkEvents"]),
        "preUnlinkGlobalBacklinkMeaning": "raw node+0x10 high-bit field; may be a valid complemented sentinel, not necessarily a pointer",
        "recordedReturnMatchMeaning": "numeric equality only; any-match includes later returns in the trusted prefix, not liveness or allocation ownership",
        "counts": dict(count),
        "requestIntervalExamples": return_examples,
        "requestIntervalExamplesTruncated": max(0, count["nonzeroAllocationReturns"] - len(return_examples)),
        "highReturnedAddressExamples": high_return_examples,
        "highReturnedAddressExamplesTruncated": max(0, count["highAllocReturns"] + count["highReallocReturns"] - len(high_return_examples)),
        "highNodeBoundaryExamples": high_boundary_examples,
        "highNodeBoundaryExamplesTruncated": max(0, count["highNodeBoundaryEvents"] - len(high_boundary_examples)),
        "preUnlinkHighBitGlobalBacklinkExamples": high_backlink_examples,
        "preUnlinkHighBitGlobalBacklinkExamplesTruncated": max(0, count["preUnlinkHighBitGlobalBacklinkEvents"] - len(high_backlink_examples)),
    }
    return {
        "schema": SCHEMA,
        "inputBytes": len(data), "sha256": hashlib.sha256(data).hexdigest().upper(),
        "process": {"pid": header[6], "creationTime100ns": creation_time,
                    "postExitAssertedByCaller": True, "normalExitProven": False,
                    "expectedIdentityChecked": expected_pid is not None and expected_creation_time is not None},
        "modules": {"gameBase": header[9], "stormBase": header[10],
                    "expectedProfile": {"gameTimeDateStamp": header[23], "gameSizeOfImage": header[24],
                                        "stormTimeDateStamp": header[25], "stormSizeOfImage": header[26]},
                    "expectedProfileVerifiedAtInstall": header[11] == 2},
        "installation": {"status": INSTALL_STATES[header[11]], "hookMask": header[12]},
        "journal": {"capacity": CAPACITY, "declaredCommitted": header[5],
                    "trustedPrefixEvents": len(prefix), "undeclaredNonzeroSlots": undeclared_nonzero,
                    "undeclaredPublishedSlots": undeclared_committed},
        "counters": counters, "eventKinds": dict(kinds),
        "counterAccounting": "independent observation dimensions; do not sum as total losses",
        "observationComplete": not gaps,
        "observationCompleteMeaning": "bounded journal publication and recorded operations only; not complete lifetimes or link coverage",
        "verdict": "INCOMPLETE_OBSERVATION" if gaps else "BOUNDED_OBSERVATION_ONLY",
        "certified": False, "causeCertified": False, "lifetimeCertified": False,
        "allocatorRouteCertified": False,
        "certificationRefusals": list(COVERAGE_EXCLUSIONS),
        "coverageFlags": header[22], "observations": observations,
        "pendingOps": pending_ops, "gaps": gaps,
        "claimsNotSupported": ["nativeArenaRoute", "allocationStillLiveBecauseNoFreeWasRecorded",
                              "allLinkWritesCovered", "highAddressIsRootCause", "gameplayOrRenderSuccess"],
    }


def analyze_file(path: str | Path, *, process_exited: bool,
                 expected_pid: int | None = None,
                 expected_creation_time: int | None = None) -> dict[str, Any]:
    """Read one fixed-size journal, never a live mapping."""
    _require(process_exited is True, "post-exit ownership assertion is required")
    source = Path(path)
    _require(source.stat().st_size == FILE_BYTES, "journal file has wrong size")
    # Bounded read also detects size changes between stat and read.
    with source.open("rb") as stream:
        data = stream.read(FILE_BYTES + 1)
    return analyze_bytes(data, process_exited=process_exited, expected_pid=expected_pid,
                         expected_creation_time=expected_creation_time)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--process-exited", action="store_true", required=True,
                        help="assert exact owned process has exited; not a normal-exit claim")
    parser.add_argument("--expected-pid", type=lambda value: int(value, 0))
    parser.add_argument("--expected-creation-time", type=lambda value: int(value, 0))
    args = parser.parse_args(argv)
    # Never replace the raw evidence with its own report, including aliases.
    same_file = args.input.resolve() == args.output.resolve()
    if not same_file and args.input.exists() and args.output.exists():
        same_file = args.input.samefile(args.output)
    if same_file:
        parser.error("--output must not refer to the raw --input evidence file")
    try:
        report = analyze_file(args.input, process_exited=args.process_exited,
                              expected_pid=args.expected_pid,
                              expected_creation_time=args.expected_creation_time)
    except (OSError, TraceFormatError) as error:
        report = {"schema": SCHEMA, "verdict": "MALFORMED_OR_UNREADABLE",
                  "certified": False, "error": str(error)}
        status = 2
    else:
        status = 0 if report["observationComplete"] else 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()), "verdict": report["verdict"],
                      "certified": False}, ensure_ascii=False))
    return status


if __name__ == "__main__":
    sys.exit(main())
