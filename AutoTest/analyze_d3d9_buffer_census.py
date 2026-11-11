#!/usr/bin/env python3
"""Strict reader for the cold D3D9 buffer-census records.

Buffer classes are keyed by the creating caller and the creation arguments;
each Lock is counted under its own caller. Counts are Lock/Unlock attempts,
not successful maps, and the caller split is a return-address observation
(an inlined public entry is attributed to the enclosing frame's caller).
Malformed records are never skipped.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

MARKER = "WARVK_D3D9_BUFFER_CENSUS "
SCHEMA = "warvk-d3d9-buffer-census/v1"
CALLERS = ("game", "self", "other")
KINDS = ("vertex", "index")
LOCK_SIZE_BOUNDS = [256, 1024, 4096, 16384, 65536, 262144, 1048576]
CLASS_CAPACITY = 16
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
TOP_KEYS = {"schema", "deviceOrdinal", "snapshotSequence", "reason", "attribution", "gameModuleKnown",
            "recordBusy", "presents", "lockSizeBounds", "classes", "classesDropped", "unclassifiedLocks",
            "unclassifiedUnlocks", "saturatedUpdates"}
CLASS_KEYS = {"caller", "kind", "length", "usage", "pool", "format", "fvf", "created", "lockCallers", "locks",
              "unlocks", "bytes", "maxBytes", "flags", "sizeZero", "whole", "outOfRange", "nested", "sequential",
              "wrapped", "wrappedDiscard", "beyondWritten", "sizeBins", "framesLocked", "maxLocksPerFrame",
              "maxBytesPerFrame"}
FLAG_KEYS = ("discard", "noOverwrite", "readOnly", "noSysLock", "doNotWait", "noDirtyUpdate", "other")
REASONS = {"present-milestone", "normal-device-destruction"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate key " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError("non-finite constant " + value)


def uint(value, bits=64):
    return type(value) is int and 0 <= value <= (UINT64_MAX if bits == 64 else UINT32_MAX)


def counts(values, length, name):
    require(type(values) is list and len(values) == length and all(uint(v) for v in values), name)


def parse_class(entry: dict, saturated: bool) -> None:
    require(type(entry) is dict and set(entry) == CLASS_KEYS, "buffer class fields")
    require(entry["caller"] in CALLERS and entry["kind"] in KINDS, "creator and kind")
    for key in ("length", "usage", "pool", "format", "fvf", "maxBytes"):
        require(uint(entry[key], 32), key)
    require(entry["length"] > 0, "length")
    if entry["kind"] == "vertex":
        require(entry["format"] == 0, "vertex data has no index format")
    else:
        require(entry["fvf"] == 0, "an index buffer has no FVF")
    for key in ("created", "locks", "unlocks", "bytes", "sizeZero", "whole", "outOfRange", "nested", "sequential",
                "wrapped", "wrappedDiscard", "beyondWritten", "framesLocked", "maxLocksPerFrame", "maxBytesPerFrame"):
        require(uint(entry[key]), key)
    require(entry["created"] > 0, "a class is created by a creation")
    counts(entry["lockCallers"], len(CALLERS), "lock callers")
    counts(entry["sizeBins"], len(LOCK_SIZE_BOUNDS) + 1, "size bins")
    flags = entry["flags"]
    require(type(flags) is dict and tuple(flags) == FLAG_KEYS and all(uint(v) for v in flags.values()), "flag counts")
    require(entry["maxBytes"] <= entry["length"], "a Lock fits the buffer")
    if saturated:
        return
    locks = entry["locks"]
    require(sum(entry["lockCallers"]) == locks, "lock callers balance")
    require(sum(entry["sizeBins"]) + entry["outOfRange"] == locks, "size bins balance")
    require(all(v <= locks for v in flags.values()), "flag counts within locks")
    require(entry["sequential"] + entry["wrapped"] <= locks and entry["wrappedDiscard"] <= entry["wrapped"]
            and entry["flags"]["discard"] >= entry["wrappedDiscard"], "ring counts within locks")
    for key in ("sizeZero", "whole", "outOfRange", "nested", "beyondWritten"):
        require(entry[key] <= locks, key + " within locks")
    require(entry["bytes"] <= locks * entry["maxBytes"], "bytes within the largest Lock")
    require(entry["framesLocked"] <= locks and entry["maxLocksPerFrame"] <= locks
            and bool(locks) == bool(entry["framesLocked"]) and bool(locks) == bool(entry["maxLocksPerFrame"]),
            "frame counts")
    require(entry["maxBytesPerFrame"] <= entry["bytes"] and entry["maxBytes"] <= entry["maxBytesPerFrame"],
            "frame bytes")


def parse_record(line: str) -> dict:
    require(line.startswith(MARKER), "buffer census marker")
    row = json.loads(line[len(MARKER):], object_pairs_hook=reject_duplicate_keys, parse_constant=reject_constant)
    require(type(row) is dict and set(row) == TOP_KEYS, "exact buffer census fields")
    require(row["schema"] == SCHEMA and row["attribution"] == "return-address-module", "schema and attribution")
    require(uint(row["deviceOrdinal"]) and row["deviceOrdinal"] > 0 and uint(row["snapshotSequence"])
            and row["snapshotSequence"] > 0, "device ordinal and sequence")
    require(row["reason"] in REASONS and type(row["gameModuleKnown"]) is bool, "reason and module flag")
    require(uint(row["recordBusy"], 32) and row["lockSizeBounds"] == LOCK_SIZE_BOUNDS, "refusals and bounds")
    for key in ("presents", "classesDropped", "unclassifiedLocks", "unclassifiedUnlocks", "saturatedUpdates"):
        require(uint(row[key]), key)
    classes = row["classes"]
    require(type(classes) is list and len(classes) <= CLASS_CAPACITY, "buffer class capacity")
    require(not row["classesDropped"] or len(classes) == CLASS_CAPACITY, "class drops only when full")
    keys = set()
    for entry in classes:
        parse_class(entry, bool(row["saturatedUpdates"]))
        key = tuple(entry[k] for k in ("caller", "kind", "length", "usage", "pool", "format", "fvf"))
        require(key not in keys, "duplicate buffer class")
        keys.add(key)
        if not row["saturatedUpdates"]:
            require(entry["framesLocked"] <= row["presents"] + 1, "frames within presents")
    return row


def parse_log(text: str) -> list[dict]:
    """Every marker line must parse; sequences strictly increase per device."""
    rows, last = [], {}
    for number, line in enumerate(text.splitlines(), 1):
        if MARKER.rstrip() not in line:
            continue
        start = line.index(MARKER.rstrip())
        row = parse_record(line[start:])
        device = row["deviceOrdinal"]
        require(row["snapshotSequence"] > last.get(device, 0), "sequence regression at line %d" % number)
        last[device] = row["snapshotSequence"]
        rows.append(dict(line=number, record=row))
    return rows


def summary(row: dict) -> dict:
    """Each class with its Lock totals per frame and per Lock."""
    presents = row["presents"]
    classes = []
    for entry in row["classes"]:
        locks = entry["locks"]
        classes.append(dict(entry, locksPerPresent=round(locks / presents, 2) if presents else None,
                            bytesPerPresent=round(entry["bytes"] / presents, 1) if presents else None,
                            bytesPerLock=round(entry["bytes"] / locks, 1) if locks else None))
    return dict(deviceOrdinal=row["deviceOrdinal"], snapshotSequence=row["snapshotSequence"], reason=row["reason"],
                gameModuleKnown=row["gameModuleKnown"], presents=presents, recordBusy=row["recordBusy"],
                classes=classes, classesDropped=row["classesDropped"], unclassifiedLocks=row["unclassifiedLocks"],
                saturatedUpdates=row["saturatedUpdates"])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    args = ap.parse_args()
    rows = parse_log(args.log.read_text("utf-8", errors="strict"))
    require(rows, "no buffer census record")
    print(json.dumps(summary(rows[-1]["record"]), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
