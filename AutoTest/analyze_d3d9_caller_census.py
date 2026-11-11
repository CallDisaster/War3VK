#!/usr/bin/env python3
"""Strict reader for the cold D3D9 caller-census records.

The caller split is a return-address observation: Game.dll, this DLL (WarVK
and DXVK internals) or other. A public method inlined into another frame of
this DLL is attributed to that frame's caller, so this is not proof of origin.
Counts are API attempts, not successful calls, draws that reached the GPU, or
a steady-state frame workload. Malformed records are never skipped.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from analyze_d3d9_migration_census import API_NAMES

MARKER = "WARVK_D3D9_CALLER_CENSUS "
SCHEMA = "warvk-d3d9-caller-census/v1"
CALLERS = ("game", "self", "other")
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
CLASS_CAPACITY = FVF_CAPACITY = 32
TOP_KEYS = {"schema", "deviceOrdinal", "snapshotSequence", "reason", "attribution", "gameModuleKnown",
            "recordBusy", "proxyPrimitiveBound", "callers", "textureClasses", "textureClassesDropped",
            "invalidCallerApis", "saturatedUpdates"}
ACCOUNT_KEYS = {"apiCalls", "drawKinds", "primitiveTypes", "primitiveTypeInvalid", "primitiveCountOverProxyBound",
                "maxPrimitiveCount", "maxVertexCount", "renderStates", "textureStageStates", "samplerStates",
                "stateOutOfRange", "fvfs", "fvfsDropped"}
CLASS_KEYS = {"caller", "kind", "format", "usage", "pool", "count", "fullMipChainRequests",
              "maxWidth", "maxHeight", "maxLevels"}
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


def indices(values, bound, name):
    require(type(values) is list and all(type(v) is int and 0 <= v < bound for v in values)
            and values == sorted(set(values)), name + " must be ascending unique indices below %d" % bound)


def parse_record(line: str) -> dict:
    require(line.startswith(MARKER), "caller census marker")
    row = json.loads(line[len(MARKER):], object_pairs_hook=reject_duplicate_keys, parse_constant=reject_constant)
    require(type(row) is dict and set(row) == TOP_KEYS, "exact caller census fields")
    require(row["schema"] == SCHEMA and row["attribution"] == "return-address-module", "schema and attribution")
    require(uint(row["deviceOrdinal"]) and row["deviceOrdinal"] > 0 and uint(row["snapshotSequence"])
            and row["snapshotSequence"] > 0, "device ordinal and sequence")
    require(row["reason"] in REASONS and type(row["gameModuleKnown"]) is bool, "reason and module flag")
    require(uint(row["recordBusy"], 32) and row["proxyPrimitiveBound"] == 4096, "refusals and bound")
    for key in ("textureClassesDropped", "invalidCallerApis", "saturatedUpdates"):
        require(uint(row[key]), key)
    callers = row["callers"]
    require(type(callers) is dict and tuple(callers) == CALLERS, "exactly game/self/other accounts in order")
    for name, account in callers.items():
        require(type(account) is dict and set(account) == ACCOUNT_KEYS, name + " account fields")
        api = account["apiCalls"]
        require(type(api) is dict and tuple(api) == API_NAMES and all(uint(v) for v in api.values()), name + " api calls")
        require(type(account["drawKinds"]) is list and len(account["drawKinds"]) == 5
                and all(uint(v) for v in account["drawKinds"]) and account["drawKinds"][0] == 0, name + " draw kinds")
        require(type(account["primitiveTypes"]) is list and len(account["primitiveTypes"]) == 7
                and all(uint(v) for v in account["primitiveTypes"]) and account["primitiveTypes"][0] == 0,
                name + " primitive types")
        for key in ("primitiveTypeInvalid", "primitiveCountOverProxyBound", "stateOutOfRange", "fvfsDropped"):
            require(uint(account[key]), name + " " + key)
        for key in ("maxPrimitiveCount", "maxVertexCount"):
            require(uint(account[key], 32), name + " " + key)
        indices(account["renderStates"], 256, name + " render states")
        indices(account["textureStageStates"], 64, name + " texture stage states")
        indices(account["samplerStates"], 32, name + " sampler states")
        fvfs = account["fvfs"]
        require(type(fvfs) is list and len(fvfs) <= FVF_CAPACITY and len(set(fvfs)) == len(fvfs)
                and all(uint(v, 32) for v in fvfs), name + " fvf set")
        require(not account["fvfsDropped"] or len(fvfs) == FVF_CAPACITY, name + " fvf drops only when full")
        draws = sum(api[n] for n in ("DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP"))
        if not row["saturatedUpdates"]:
            require(sum(account["drawKinds"]) == draws
                    and sum(account["primitiveTypes"]) + account["primitiveTypeInvalid"] == draws,
                    name + " draw accounts balance")
    classes = row["textureClasses"]
    require(type(classes) is list and len(classes) <= CLASS_CAPACITY, "texture class capacity")
    require(not row["textureClassesDropped"] or len(classes) == CLASS_CAPACITY, "class drops only when full")
    keys = set()
    for entry in classes:
        require(type(entry) is dict and set(entry) == CLASS_KEYS and entry["caller"] in CALLERS, "texture class fields")
        require(all(uint(entry[k], 32) for k in ("kind", "format", "usage", "pool", "maxWidth", "maxHeight", "maxLevels"))
                and uint(entry["count"]) and entry["count"] > 0 and uint(entry["fullMipChainRequests"])
                and entry["fullMipChainRequests"] <= entry["count"] and entry["kind"] in (1, 2, 3), "texture class values")
        key = (entry["caller"], entry["kind"], entry["format"], entry["usage"], entry["pool"])
        require(key not in keys, "duplicate texture class")
        keys.add(key)
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
    """Nonzero API attempts per caller and the APIs only one caller used."""
    used = {name: {api: count for api, count in account["apiCalls"].items() if count}
            for name, account in row["callers"].items()}
    return dict(
        deviceOrdinal=row["deviceOrdinal"], snapshotSequence=row["snapshotSequence"], reason=row["reason"],
        gameModuleKnown=row["gameModuleKnown"], used=used,
        gameOnly=sorted(set(used["game"]) - set(used["self"]) - set(used["other"])),
        selfOnly=sorted(set(used["self"]) - set(used["game"]) - set(used["other"])),
        draws={name: dict(primitiveTypes=a["primitiveTypes"], overProxyBound=a["primitiveCountOverProxyBound"],
                          maxPrimitiveCount=a["maxPrimitiveCount"], maxVertexCount=a["maxVertexCount"],
                          fvfs=a["fvfs"], renderStates=a["renderStates"],
                          textureStageStates=a["textureStageStates"], samplerStates=a["samplerStates"])
               for name, a in row["callers"].items()},
        textureClasses=row["textureClasses"])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=Path)
    args = ap.parse_args()
    rows = parse_log(args.log.read_text("utf-8", errors="strict"))
    require(rows, "no caller census record")
    print(json.dumps(summary(rows[-1]["record"]), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
