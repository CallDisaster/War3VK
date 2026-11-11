#!/usr/bin/env python3
"""Strict post-exit reader for the independent fixed-size startup witness.

The runner must establish exit using its retained exact process/Job identity.
``process_exited=True`` is an explicit caller assertion, NOT a liveness check.
PID and creation FILETIME are mandatory, even for offline fixtures. A fresh
CREATE_NEW output path and exact owner identity prevent accidental run mixing.

This journal proves only which instrumented boundaries were observed. An open
span does not certify a deadlock, crash or cause; a forced exit may interrupt
perfectly valid work. No events or lost/partial events can certify absence.
QPC is sampled outside the try-cell, so cross-thread commit order need not be
timestamp order. Pipe existence is witnessed only by a successful PipeCreate
End, NOT ControlServerLoop entry, a running flag or an "online" log.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
from typing import Any

SCHEMA = "warvk-startup-witness/v1"
MAGIC = 0x53545731
VERSION = 1
HEADER_WORDS = 64
EVENT_WORDS = 16
CAPACITY = 256
FILE_BYTES = 16640
HEADER_BYTES = HEADER_WORDS * 4
EVENT_BYTES = EVENT_WORDS * 4
UINT32_MAX = 0xFFFFFFFF
STAGES = {
    1: "Bootstrap", 2: "BootstrapGate", 3: "GameModuleValidation",
    4: "MinHookInitialization", 5: "EarlyHooks", 6: "JassHookInstallation",
    7: "LifecycleHookInstallation", 8: "NativeLightInstallation",
    9: "EarlyModelInstallation", 10: "MainRunner", 11: "MainRunnerEarlyHooks",
    12: "MainRunnerOriginal", 13: "MainRunnerAlt", 14: "MainRunnerAltEarlyHooks",
    15: "MainRunnerAltOriginal", 16: "RuntimeActivation",
    17: "RuntimeActivationGate", 18: "RuntimeCore", 19: "War3Initialize",
    20: "ShaderReload", 21: "NetEventInitialize", 22: "ControlPlaneInitialize",
    23: "ControlServerStart", 24: "ControlServerLoop", 25: "PipeCreate",
    26: "PipeConnect", 27: "StormRuntimeInitialize", 28: "War3PreInit",
    29: "JassRoot", 30: "War3Init", 31: "JassReadyCallbacks",
    32: "ModulesInitialize", 33: "GameHooksInstall", 34: "RuntimeReady",
    35: "MainRunnerComplete", 36: "MainRunnerAltComplete",
}
PHASES = {1: "Begin", 2: "End", 3: "Note"}
COUNTERS = {10: "busy", 11: "busyOverflow", 12: "full", 13: "invalid",
            14: "counterOverflow", 15: "clockFailure"}
_HEADER = struct.Struct("<64I")
_EVENT = struct.Struct("<16I")


class WitnessFormatError(ValueError):
    """Invalid ownership or a malformed startup-witness contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise WitnessFormatError(message)


def _event(words: tuple[int, ...], slot: int) -> dict[str, Any]:
    prefix = f"event[{slot}]"
    _require(words[1] in STAGES, f"{prefix}: unknown stage")
    _require(words[2] in PHASES, f"{prefix}: unknown phase")
    _require(words[3] != 0, f"{prefix}: zero thread id")
    _require(not any(words[9:]), f"{prefix}: nonzero reserved words")
    if words[2] == 1:
        _require(words[4] == words[0], f"{prefix}: Begin token must equal sequence")
    elif words[2] == 2:
        _require(0 < words[4] < words[0], f"{prefix}: End token is not earlier Begin")
        if words[1] in (25, 26):
            _require(words[7] in (0, 1), f"{prefix}: pipe result must be boolean")
            _require(words[7] == 0 or words[8] == 0,
                     f"{prefix}: successful pipe operation has an error")
    else:
        _require(words[4] == 0, f"{prefix}: Note token must be zero")
        if words[1] in (25, 26):
            _require(words[7] == 0, f"{prefix}: a pipe Note can only report failure")
    return dict(sequence=words[0], stage=STAGES[words[1]], phase=PHASES[words[2]],
                threadId=words[3], token=words[4], qpc=words[5] | words[6] << 32,
                detail=words[7], extra=words[8])


def analyze_bytes(data: bytes, *, process_exited: bool,
                  expected_pid: int, expected_creation_time: int) -> dict[str, Any]:
    _require(process_exited is True, "exact process-exit assertion is required")
    _require(type(expected_pid) is int and 0 < expected_pid <= UINT32_MAX,
             "expected PID must be positive uint32")
    _require(type(expected_creation_time) is int and 0 < expected_creation_time < 1 << 64,
             "expected creation FILETIME must be positive uint64")
    _require(len(data) == FILE_BYTES, "exact fixed file size is required")
    h = _HEADER.unpack_from(data)
    _require(h[:5] == (MAGIC, VERSION, HEADER_WORDS, EVENT_WORDS, CAPACITY),
             "magic/version/dimensions differ from startup-witness v1")
    _require(h[5] <= CAPACITY, "committed event count exceeds capacity")
    _require(h[6] == expected_pid, "PID mismatch")
    creation = h[7] | h[8] << 32
    _require(creation == expected_creation_time, "creation FILETIME mismatch")
    _require(h[9] == 1, "initialization is not published")
    _require(h[11] in (0, 1) and h[14] in (0, 1), "overflow flags must be boolean")
    _require(not any(h[18:]), "nonzero reserved header words")
    frequency = h[16] | h[17] << 32
    _require(frequency != 0 or h[15] or h[14],
             "unknown QPC frequency without clock-loss accounting")

    gaps: list[dict[str, Any]] = []
    events: list[dict[str, Any]] = []
    prefix_open = True
    for index in range(CAPACITY):
        words = _EVENT.unpack_from(data, HEADER_BYTES + index * EVENT_BYTES)
        if index < h[5]:
            if words[0] == 0:
                gaps.append(dict(reason="partialPublication", slot=index))
                prefix_open = False
                continue
            _require(words[0] == index + 1, "duplicate/nonmonotonic commit sequence")
            row = _event(words, index)
            if prefix_open:
                events.append(row)
            else:
                gaps.append(dict(reason="afterBrokenPrefix", slot=index))
        elif any(words):
            gaps.append(dict(reason="undeclaredEventData", slot=index,
                             committed=words[0] != 0))

    begins: dict[int, dict[str, Any]] = {}
    completed: list[dict[str, Any]] = []
    finished: set[int] = set()
    for event in events:
        token = event["token"]
        if event["phase"] == "Begin":
            begins[token] = event
        elif event["phase"] == "End":
            _require(token in begins, "End has no recorded Begin")
            begin = begins[token]
            _require(token not in finished, "duplicate End")
            _require(begin["stage"] == event["stage"], "End stage differs from Begin")
            _require(begin["threadId"] == event["threadId"], "End thread differs from Begin")
            if begin["qpc"] and event["qpc"]:
                _require(event["qpc"] >= begin["qpc"], "same-span QPC moves backwards")
            finished.add(token)
            completed.append(dict(token=token, stage=begin["stage"],
                                  threadId=begin["threadId"], begin=begin, end=event))
    open_spans = [dict(token=token, stage=begin["stage"],
                       threadId=begin["threadId"], beginSequence=begin["sequence"],
                       returnObserved=False)
                  for token, begin in begins.items() if token not in finished]
    counters = {name: h[index] for index, name in COUNTERS.items()}
    loss_free = not any(counters.values()) and not gaps
    ready = any(e["stage"] == "RuntimeReady" and e["phase"] == "Note" for e in events)
    pipe_ends = [e for e in events if e["stage"] == "PipeCreate" and e["phase"] == "End"]
    pipe_failures = [e for e in events if e["stage"] == "PipeCreate" and
                     e["phase"] in ("End", "Note") and e["detail"] == 0]
    originals = ("MainRunnerOriginal", "MainRunnerAltOriginal")
    return {
        "schema": SCHEMA, "pid": h[6], "creationTime100ns": creation,
        "identityMatched": True, "processExitedAssertion": True,
        "fileBytes": len(data), "qpcFrequency": frequency,
        "declaredCount": h[5], "eventCount": len(events), "capacity": CAPACITY,
        "counters": counters, "gaps": gaps,
        "recordingComplete": bool(events) and loss_free,
        "stageObservationAvailable": bool(events),
        "runtimeReadyObserved": ready,
        "controlThreadEntryObserved": any(e["stage"] == "ControlServerLoop" and
                                          e["phase"] in ("Begin", "Note") for e in events),
        "pipeCreateSuccessObserved": any(e["detail"] == 1 for e in pipe_ends),
        "pipeCreateFailures": [dict(sequence=e["sequence"], win32Error=e["extra"])
                               for e in pipe_failures],
        "mainRunnerOriginalCalls": [dict(stage=b["stage"], token=t,
                                         threadId=b["threadId"], returnObserved=t in finished)
                                    for t, b in begins.items() if b["stage"] in originals],
        "completedSpans": completed, "openSpans": open_spans,
        "events": events,
        "latestCommittedEvent": events[-1] if events else None,
        "causeCertified": False, "deadlockCertified": False,
        "normalExitProven": False, "gameplayAccepted": False,
        "limitations": [
            "Only explicit startup boundaries after witness initialization are observed.",
            "Open spans mean no return witness before exit, not a certified hang or cause.",
            "Busy/full/partial records prevent negative coverage claims.",
            "Exit assertion does not prove normal exit, ready, rendering or visual acceptance.",
            "QPC sampling precedes cell acquisition; cross-thread sequence is not clock order.",
        ],
    }


def analyze_file(path: Path | str, *, process_exited: bool,
                 expected_pid: int, expected_creation_time: int) -> dict[str, Any]:
    # Check the caller's proof assertion BEFORE opening the mapping's file.
    _require(process_exited is True, "exact process-exit assertion is required")
    path = Path(path)
    with path.open("rb") as stream:
        data = stream.read(FILE_BYTES + 1)
    result = analyze_bytes(data, process_exited=process_exited,
                           expected_pid=expected_pid,
                           expected_creation_time=expected_creation_time)
    result["source"] = dict(path=str(path.resolve()), bytes=len(data),
                            sha256=hashlib.sha256(data).hexdigest().upper())
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--process-exited", action="store_true")
    parser.add_argument("--expected-pid", type=int, required=True)
    parser.add_argument("--expected-creation-time", type=int, required=True)
    args = parser.parse_args()
    result = analyze_file(args.input, process_exited=args.process_exited,
                          expected_pid=args.expected_pid,
                          expected_creation_time=args.expected_creation_time)
    text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
