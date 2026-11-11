"""Independent fail-closed readout of an isolated same-PID A→B→A run.

The map-script markers prove loading order, while runtime epochs and reset
serials prove the WarVK retirement path actually executed. Neither alone is
accepted. Static screenshots are not a full visual or release acceptance.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


PHASES = ("A-first", "B", "A-return")
PHASE_EPOCH = {"A-first": 1, "B": 2, "A-return": 3}


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"),
                      object_pairs_hook=strict_object)


def examine(result: dict, rows: list[dict]) -> dict:
    failures = []
    observed = [entry.get("phase") for entry in result.get("phases", [])]
    if observed != list(PHASES):
        failures.append("phase-order-or-count")
    if not result.get("sameProcess") or not result.get("processStillAliveAtEnd"):
        failures.append("same-pid-liveness")
    if not result.get("sourceUnchanged") or not result.get("stop", {}).get("ok"):
        failures.append("source-or-settlement")
    if result.get("globalInputUsed") is not False:
        failures.append("global-input")
    client = result.get("resolution", {}).get("window", {}).get("clientRect", {})
    if (client.get("width"), client.get("height")) != (2560, 1440):
        failures.append("resolution")
    if result.get("newGpuEvents") or result.get("gpuIncidents"):
        failures.append("gpu-events-or-incidents")
    captures = result.get("screenshots", [])
    if [entry.get("phase") for entry in captures] != list(PHASES) or not all(
            entry.get("ok") and entry.get("size", 0) > 0 and entry.get("sha256")
            and entry.get("observedEpoch") == PHASE_EPOCH[entry["phase"]]
            and entry.get("expectedEpoch") == PHASE_EPOCH[entry["phase"]]
            for entry in captures):
        failures.append("stage-image-coverage")

    stats = {}
    for phase in PHASES:
        all_entries = [entry for entry in rows if entry.get("phase") == phase]
        # The JASS phase marker can be written before ChangeLevel finishes.
        # Old-map tail frames remain in the trace but cannot prove this phase.
        entries = [entry for entry in all_entries if
                   entry.get("monitor", {}).get("shadowMapEpoch") ==
                   PHASE_EPOCH[phase]]
        by_frame = {entry.get("frameIndex"): entry for entry in entries
                    if isinstance(entry.get("frameIndex"), int) and
                    entry["frameIndex"] > 0}
        distinct = list(by_frame.values())
        tail = distinct[-2:]
        def number(entry, name):
            value = entry.get("monitor", {}).get(name)
            return value if type(value) is int and value >= 0 else 0
        stats[phase] = {
            "samples": len(entries), "allSamples": len(all_entries),
            "distinctFrames": len(distinct),
            "settledEpoch": PHASE_EPOCH[phase] if entries else 0,
            "epochMax": max((number(x, "shadowMapEpoch") for x in all_entries),
                            default=0),
            "requestedMax": max((number(x, "shadowMapResetRequestedSerial")
                                 for x in all_entries), default=0),
            "appliedMax": max((number(x, "shadowMapResetAppliedSerial")
                               for x in all_entries), default=0),
            "retiredMax": max((number(x, "shadowRetiredSessionCount")
                               for x in all_entries), default=0),
            "collectedMax": max((number(x, "shadowRetiredSessionCollectedCount")
                                 for x in all_entries), default=0),
            "completeTail": sum(number(x, "semanticSceneReceiverHasCompleteShadowMap")
                                for x in tail),
            "readyTail": sum(number(x, "shadowMapProducerReady") for x in tail),
            "drawnMax": max((number(x, "shadowReplayDrawnCasterCount")
                             for x in entries), default=0),
        }
        if (stats[phase]["samples"] < 2 or
                stats[phase]["distinctFrames"] < 2 or
                stats[phase]["completeTail"] != 2 or
                stats[phase]["readyTail"] != 2 or
                stats[phase]["drawnMax"] == 0):
            failures.append(f"{phase}-sustained-shadow")

    a, b, returned = (stats[phase] for phase in PHASES)
    timeline = []
    for row in rows:
        epoch = row.get("monitor", {}).get("shadowMapEpoch")
        if type(epoch) is int and epoch > 0 and (
                not timeline or epoch != timeline[-1]):
            timeline.append(epoch)
    if timeline != [1, 2, 3] or any(
            stats[phase]["settledEpoch"] != PHASE_EPOCH[phase]
            for phase in PHASES):
        failures.append("map-epoch-did-not-advance")
    if not (0 < b["requestedMax"] <= b["appliedMax"] <
            returned["appliedMax"] and
            returned["requestedMax"] <= returned["appliedMax"]):
        failures.append("reset-request-apply-not-closed")
    retirement_seen = any(x["retiredMax"] > 0 or x["collectedMax"] > 0
                          for x in (a, b, returned))
    if not retirement_seen:
        failures.append("retirement-path-uncovered")
    if retirement_seen and returned["retiredMax"] > 0:
        failures.append("retired-session-not-drained")

    # This canary does not parse source-bound perf charge rows or inspect
    # per-caster pixels. Never upgrade it into a release authorization.
    return {
        "ok": not failures,
        "failedChecks": failures,
        "phases": stats,
        "epochTimeline": timeline,
        "samePidMapOrder": observed == list(PHASES) and bool(result.get("sameProcess")),
        "persistentChargeAccountVerified": False,
        "fullVisualAcceptance": False,
        "releaseAuthorized": False,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    args = parser.parse_args()
    result = read_json(args.run_dir / "cross-map-result.json")
    rows = [json.loads(line, object_pairs_hook=strict_object)
            for line in (args.run_dir / "runtime-samples.jsonl")
                .read_text(encoding="utf-8").splitlines() if line]
    analysis = examine(result, rows)
    target = args.run_dir / "cross-map-analysis.json"
    with target.open("x", encoding="utf-8") as stream:
        json.dump(analysis, stream, ensure_ascii=False, indent=2, allow_nan=False)
    print(json.dumps(analysis, ensure_ascii=False, allow_nan=False))
    return 0 if analysis["ok"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
