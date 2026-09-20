"""Strict offline frozen-manifest and business-frame recovery comparator.

This is evidence-structure validation only.  It never proves GPU/visual
recovery and never accepts a run without a frozen writer manifest.  Legacy
sidecar identities are read-only historical inputs and are not certification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any

import analyze_stage11_budget_census as census_reader


class EvidenceError(ValueError):
    """Input cannot be admitted into the offline gate."""


class PhaseMarkerEvidenceError(EvidenceError):
    """Phase marker domain is absent or inconsistent with the producer contract."""

    def __init__(self, message: str, missing: bool = False):
        super().__init__(message)
        self.missing = bool(missing)


SCHEMA_VERSION = 2
FROZEN_BINDING_SCHEMA = 2

REQUIRED_WORKLOAD_COLUMNS = (
    "epoch",
    "businessFrameSerial",
    "shadowMapRenderSerial",
    "replayCasterCount",
    "capturedDrawCount",
    "skippedCasterCap",
    "shadowTaaReceiverExecuted",
    "hasShadowReceiver",
    "semanticSceneSubmitted",
    "reusedLastCompleteShadowMap",
    "renderedCurrentPartialShadowMap",
)
CUMULATIVE_FAILURE_KEYS = (
    "framesIncomplete",
    "framesProducerIncomplete",
    "producerRequiredCasterOmissionCount",
    "producerAllocationFailureCount",
    "drawTimeSnapshotPageCapacityRejectCount",
    "drawTimeSnapshotPageAllocationFailureCount",
)
CUMULATIVE_OBSERVABILITY_KEYS = (
    "drawTimeSnapshotPageCreateCount",
    "drawTimeSnapshotSuballocationCount",
    "drawTimeSnapshotSuballocationBytes",
    "drawTimeVBCacheIndexedUnknownRangeFallbackCount",
    "semanticSceneReceiverNoCompleteShadowMapCount",
)
LAST_VALUE_KEYS = (
    "drawTimeSnapshotPageResidentBytesLast",
    "drawTimeSnapshotPageUsedBytesLast",
    "drawTimeSnapshotPageReclaimedCountLast",
)
SCOPE_KEYS = (
    "producerSealFrameSerialLast",
    "producerSealMapEpochLast",
    "producerSealDeviceEpochLast",
)
HEAVY_FORENSICS_KEYS = (
    "DXVK_WAR3_FRAME_EVIDENCE",
    "DXVK_WAR3_FRAME_EVIDENCE_INPUTS",
    "DXVK_WAR3_FRAME_EVIDENCE_DRAWS",
    "DXVK_WAR3_FRAME_EVIDENCE_CASTERS",
    "DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT",
    "DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED",
    "DXVK_WAR3_NATIVE_MODEL_LIGHTS",
    "DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER",
    "DXVK_WAR3_DEBUG_CONSOLE",
    "DXVK_WAR3_RENDER_LOG",
    "DXVK_WAR3_DATA_COLLECTION_TREE",
)
PHASE_NAMES = (
    "sample-start",
    "pressure-start",
    "pressure-end",
    "relief-start",
    "relief-end",
)
RECOVERY_CONTRACT = {
    "minReliefFrames": 60,
    "minSustainedValidFrames": 60,
    "maxInvalidRunFrames": 6,
    "minFreshCompleteFrames": 8,
}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def _is_int(value: Any) -> bool:
    return type(value) is int


def _is_uint(value: Any) -> bool:
    return _is_int(value) and value >= 0


def _is_finite(value: Any) -> bool:
    return ((type(value) is int) or (type(value) is float)) and math.isfinite(float(value))


def _hex(value: Any, length: int = 64) -> str:
    text = str(value or "").upper()
    _require(re.fullmatch(r"[0-9A-F]{" + str(length) + r"}", text) is not None,
             "invalid hex identity")
    return text


def _strict_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        _require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise EvidenceError("non-finite JSON constant: " + value)


def read_json_strict(path: Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        raise EvidenceError("cannot read " + str(path) + ": " + str(exc)) from exc
    try:
        return json.loads(text, object_pairs_hook=_strict_pairs, parse_constant=_reject_constant)
    except EvidenceError:
        raise
    except Exception as exc:
        raise EvidenceError("invalid strict JSON " + str(path) + ": " + str(exc)) from exc


def sha256_file(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest().upper()


def identity(path: Path) -> dict[str, Any]:
    return {"size": path.stat().st_size, "sha256": sha256_file(path)}


def parse_report_text(text: str) -> dict[str, Any]:
    roots = list(re.finditer(r"\bconst\s+data\s*=\s*(?=\{)", text))
    _require(len(roots) == 1, "exactly one report root required")
    decoder = json.JSONDecoder(object_pairs_hook=_strict_pairs, parse_constant=_reject_constant)
    try:
        data, end = decoder.raw_decode(text, roots[0].end())
    except EvidenceError:
        raise
    except Exception as exc:
        raise EvidenceError("report root JSON invalid: " + str(exc)) from exc
    _require(isinstance(data, dict), "report root must be an object")
    _require(text[end:].lstrip().startswith(";"), "report root must terminate at semicolon")
    return data


def parse_report(path: Path) -> dict[str, Any]:
    return parse_report_text(path.read_text(encoding="utf-8-sig"))


def _resolution(value: Any) -> dict[str, int] | None:
    if not isinstance(value, dict):
        return None
    width = value.get("width")
    height = value.get("height")
    if not (_is_uint(width) and _is_uint(height)):
        return None
    return {"width": int(width), "height": int(height)}


def _validate_route(route: Any) -> dict[str, Any]:
    _require(isinstance(route, dict), "route missing")
    baseline = route.get("baselineSeconds")
    move_sec = route.get("pressureMoveSeconds")
    relief = route.get("reliefSeconds")
    offsets = route.get("offsets")
    angle = route.get("cameraAngleOfAttack")
    duration = route.get("cameraDurationSec")
    _require(_is_uint(baseline) and baseline > 0, "route baselineSeconds")
    _require(_is_uint(move_sec) and move_sec > 0, "route pressureMoveSeconds")
    _require(_is_uint(relief) and relief > 0, "route reliefSeconds")
    _require(isinstance(offsets, list) and offsets, "route offsets")
    normalized_offsets: list[list[int]] = []
    for item in offsets:
        _require(isinstance(item, list) and len(item) == 2 and all(_is_int(value) for value in item),
                 "route offset shape")
        normalized_offsets.append([int(item[0]), int(item[1])])
    _require(_is_int(angle), "route cameraAngleOfAttack")
    _require(_is_finite(duration) and float(duration) > 0.0, "route cameraDurationSec")
    return {
        "baselineSeconds": int(baseline),
        "pressureMoveSeconds": int(move_sec),
        "reliefSeconds": int(relief),
        "offsets": normalized_offsets,
        "cameraAngleOfAttack": int(angle),
        "cameraDurationSec": float(duration),
    }


def _manifest_binding(manifest: dict[str, Any]) -> dict[str, Any]:
    _require(isinstance(manifest, dict), "binding manifest must be an object")
    _require(manifest.get("schema") == FROZEN_BINDING_SCHEMA,
             "frozen binding schema must be " + str(FROZEN_BINDING_SCHEMA))
    run_id = str(manifest.get("runId", "")).strip()
    _require(run_id != "", "binding runId missing")
    env = manifest.get("env")
    _require(isinstance(env, dict), "binding env missing")
    for key in HEAVY_FORENSICS_KEYS:
        _require(key in env, "binding env missing heavy key: " + key)
        _require(str(env[key]) == "0", "heavy forensics must be off: " + key)
    _require(str(env.get("DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB", "")) == "384",
             "binding must freeze 384 MiB cap")
    _require(str(env.get("DXVK_WAR3_PROFILE", "")) == "full_default",
             "binding must freeze full_default profile")
    scenario = str(env.get("DXVK_WAR3_SCENARIO", "")).strip()
    _require(scenario != "", "binding scenario missing")
    _require(str(env.get("DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE", "")) in ("0", "1"),
             "binding P2 upload index range must be explicit 0/1")
    _require(str(env.get("DXVK_WAR3_STAGE11_BUDGET_CENSUS", "")) == "1",
             "binding census must be on")
    _require(str(env.get("DISABLE_VULKAN_OBS_CAPTURE", "")) == "1",
             "binding OBS capture must be off")
    frozen_manifest_sha = _hex(manifest.get("frozenManifestSha256"))
    profile = str(manifest.get("profile", "")).strip()
    _require(profile == "full_default", "binding profile must be full_default")
    matrix = str(manifest.get("matrix", "")).strip()
    _require(matrix != "", "binding matrix missing")
    resolution = _resolution(manifest.get("resolution"))
    _require(resolution == {"width": 2560, "height": 1440},
             "binding resolution must be 2560x1440")
    route = _validate_route(manifest.get("route"))
    identity_fields = (
        ("candidateSha256", "candidateSize"),
        ("liveRecoverySha256", "liveRecoverySize"),
        ("mapSha256", "mapSize"),
        ("gameSha256", "gameSize"),
        ("exeSha256", "exeSize"),
    )
    for sha_key, size_key in identity_fields:
        _hex(manifest.get(sha_key))
        _require(_is_uint(manifest.get(size_key)) and manifest[size_key] > 0,
                 "invalid binding size: " + size_key)
    _require(manifest.get("isolatedDesktop") is True, "isolated desktop must be frozen true")
    _require(manifest.get("globalInputUsed") is False, "global input must be frozen false")
    binding = {
        "source": "frozen-run-manifest",
        "frozenManifest": True,
        "runId": run_id,
        "scenario": scenario,
        "profile": profile,
        "matrix": matrix,
        "resolution": resolution,
        "route": route,
        "candidateSha256": _hex(manifest.get("candidateSha256")),
        "candidateSize": int(manifest["candidateSize"]),
        "liveRecoverySha256": _hex(manifest.get("liveRecoverySha256")),
        "liveRecoverySize": int(manifest["liveRecoverySize"]),
        "mapSha256": _hex(manifest.get("mapSha256")),
        "mapSize": int(manifest["mapSize"]),
        "gameSha256": _hex(manifest.get("gameSha256")),
        "gameSize": int(manifest["gameSize"]),
        "exeSha256": _hex(manifest.get("exeSha256")),
        "exeSize": int(manifest["exeSize"]),
        "isolatedDesktop": True,
        "globalInputUsed": False,
        "heavyForensicsOff": True,
        "snapshotResidentCapMb": "384",
        "frozenManifestSha256": frozen_manifest_sha,
        "env": dict(env),
    }
    return binding


def validate_binding(binding: dict[str, Any]) -> None:
    _require(isinstance(binding, dict), "binding must be an object")
    _require(binding.get("source") == "frozen-run-manifest" and binding.get("frozenManifest") is True,
             "frozen run manifest required")
    _manifest_binding({
        "schema": FROZEN_BINDING_SCHEMA,
        "runId": binding.get("runId"),
        "env": binding.get("env"),
        "profile": binding.get("profile"),
        "matrix": binding.get("matrix"),
        "resolution": binding.get("resolution"),
        "route": binding.get("route"),
        "candidateSha256": binding.get("candidateSha256"),
        "candidateSize": binding.get("candidateSize"),
        "liveRecoverySha256": binding.get("liveRecoverySha256"),
        "liveRecoverySize": binding.get("liveRecoverySize"),
        "mapSha256": binding.get("mapSha256"),
        "mapSize": binding.get("mapSize"),
        "gameSha256": binding.get("gameSha256"),
        "gameSize": binding.get("gameSize"),
        "exeSha256": binding.get("exeSha256"),
        "exeSize": binding.get("exeSize"),
        "isolatedDesktop": binding.get("isolatedDesktop"),
        "globalInputUsed": binding.get("globalInputUsed"),
        "frozenManifestSha256": binding.get("frozenManifestSha256"),
    })


def load_binding_from_dir(run_dir: Path) -> dict[str, Any]:
    run_dir = Path(run_dir)
    binding_path = run_dir / "binding.json"
    _require(binding_path.is_file(), "frozen binding.json missing; legacy sidecars are not accepted")
    manifest = read_json_strict(binding_path)
    binding = _manifest_binding(manifest)
    sidecar = run_dir / "frozen-manifest.json"
    _require(sidecar.is_file(), "frozen manifest sidecar missing")
    _require(identity(sidecar)["sha256"] == binding["frozenManifestSha256"],
             "frozen manifest sidecar hash mismatch")
    receipt_path = run_dir / "receipt.json"
    if receipt_path.is_file():
        receipt = read_json_strict(receipt_path)
        binding["reportHashes"] = {
            Path(str(item.get("path", ""))).name: {
                "size": item.get("size"),
                "sha256": str(item.get("sha256", "")).upper(),
            }
            for item in receipt.get("reports", [])
            if isinstance(item, dict) and item.get("path")
        }
        binding["phaseMarkersPath"] = str(run_dir / "phaseMarkers.json")
    return binding


def _meta_binding(data: dict[str, Any], binding: dict[str, Any]) -> dict[str, Any]:
    meta = data.get("meta")
    _require(isinstance(meta, dict), "report meta missing")
    _require(_hex(meta.get("dllSha256")) == binding["candidateSha256"].upper(),
             "report candidate sha mismatch")
    _require(meta.get("dllFileSize") == binding["candidateSize"], "report candidate size mismatch")
    _require(meta.get("runtimeProfile") == binding["profile"], "report profile mismatch")
    expected_env = {
        "DXVK_WAR3_PROFILE": binding["profile"],
        "DXVK_WAR3_SCENARIO": binding["scenario"],
        "DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB": binding["snapshotResidentCapMb"],
        "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE": binding["env"].get("DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE"),
        "DXVK_WAR3_STAGE11_BUDGET_CENSUS": binding["env"].get("DXVK_WAR3_STAGE11_BUDGET_CENSUS"),
        "DISABLE_VULKAN_OBS_CAPTURE": binding["env"].get("DISABLE_VULKAN_OBS_CAPTURE"),
    }
    expected_env.update({key: "0" for key in HEAVY_FORENSICS_KEYS})
    actual_env = meta.get("env")
    if not isinstance(actual_env, dict):
        return {
            "ok": False,
            "checked": [],
            "missing": sorted(expected_env),
            "candidateSha": binding["candidateSha256"],
            "profile": binding["profile"],
        }
    checked: list[str] = []
    missing: list[str] = []
    for key, expected in expected_env.items():
        if key not in actual_env:
            missing.append(key)
            continue
        _require(str(actual_env[key]) == str(expected), "report env mismatch: " + key)
        checked.append(key)
    return {
        "ok": len(missing) == 0,
        "checked": sorted(checked),
        "missing": sorted(missing),
        "candidateSha": binding["candidateSha256"],
        "profile": binding["profile"],
    }


def extract_series(data: dict[str, Any]) -> dict[str, Any]:
    columns = data.get("workloadSeriesColumns")
    rows = data.get("workloadSeries")
    _require(isinstance(columns, list) and all(isinstance(item, str) for item in columns),
             "workloadSeriesColumns invalid")
    _require(len(columns) == len(set(columns)), "duplicate workload column")
    for required in REQUIRED_WORKLOAD_COLUMNS:
        _require(required in columns, "missing workload column: " + required)
    _require(isinstance(rows, list) and rows, "workloadSeries empty")
    _require(data.get("frameCount") == len(rows), "frameCount/workloadSeries mismatch")
    index = {name: position for position, name in enumerate(columns)}
    previous_epoch = -1
    previous_serial = -1
    for row in rows:
        _require(isinstance(row, list) and len(row) == len(columns), "workload row shape")
        for value in row:
            _require(value is None or _is_finite(value), "workload value must be finite number or null")
        epoch = row[index["epoch"]]
        serial = row[index["businessFrameSerial"]]
        _require(_is_uint(epoch) and _is_uint(serial), "epoch/businessFrameSerial must be unsigned ints")
        _require(epoch > previous_epoch, "epoch must strictly increase")
        _require(serial >= previous_serial, "businessFrameSerial must not decrease")
        for required in REQUIRED_WORKLOAD_COLUMNS:
            _require(row[index[required]] is not None, "required workload value missing: " + required)
        previous_epoch = epoch
        previous_serial = serial
    return {"columns": columns, "index": index, "rows": rows, "rowCount": len(rows)}


def extract_aggregate(data: dict[str, Any]) -> dict[str, Any]:
    summary = data.get("shadowBudgetSummary")
    _require(isinstance(summary, dict), "shadowBudgetSummary missing")
    cumulative_failure: dict[str, int] = {}
    cumulative_observation: dict[str, int] = {}
    last_value: dict[str, int] = {}
    scope: dict[str, int] = {}
    for target, keys in (
            (cumulative_failure, CUMULATIVE_FAILURE_KEYS),
            (cumulative_observation, CUMULATIVE_OBSERVABILITY_KEYS),
            (last_value, LAST_VALUE_KEYS),
            (scope, SCOPE_KEYS)):
        for key in keys:
            _require(key in summary, "aggregate missing: " + key)
            _require(_is_uint(summary[key]), "aggregate must be unsigned int: " + key)
            target[key] = int(summary[key])
    accumulation_epoch = summary.get("producerAccumulationEpoch")
    if accumulation_epoch is not None:
        _require(_is_uint(accumulation_epoch), "producerAccumulationEpoch must be unsigned int")
        scope["producerAccumulationEpoch"] = int(accumulation_epoch)
    census = data.get("stage11BudgetCensus")
    census_result = None
    census_error = ""
    if census is not None:
        try:
            census_result = census_reader.validate(census)
        except Exception as exc:
            census_error = str(exc)
    return {
        "cumulativeFailure": cumulative_failure,
        "cumulativeObservation": cumulative_observation,
        "lastValue": last_value,
        "scope": scope,
        "census": census,
        "censusResult": census_result,
        "censusError": census_error,
    }


def _p2a_run_coverage(aggregates: list[dict[str, Any]]) -> dict[str, Any]:
    missing_reports = 0
    invalid_reports = 0
    incomplete_reports = 0
    empty_reports = 0
    cut_conflicts: list[str] = []
    by_key: dict[tuple[int, int, int, int, int], dict[str, Any]] = {}
    for aggregate in aggregates:
        census = aggregate.get("census")
        if census is None:
            missing_reports += 1
            continue
        if aggregate.get("censusError"):
            invalid_reports += 1
            continue
        census_result = aggregate.get("censusResult") or {}
        raw_samples = census.get("samples")
        if not isinstance(raw_samples, list):
            invalid_reports += 1
            continue
        if not raw_samples:
            empty_reports += 1
            continue
        if not census_result.get("covered"):
            incomplete_reports += 1
            continue
        for sample in raw_samples:
            required = ("deviceIdentity", "mapEpoch", "deviceEpoch", "frame", "stage", "uploadRangeHits")
            if not isinstance(sample, dict) or any(not _is_uint(sample.get(key)) for key in required):
                invalid_reports += 1
                continue
            key = (int(sample["deviceIdentity"]), int(sample["mapEpoch"]),
                   int(sample["deviceEpoch"]), int(sample["frame"]), int(sample["stage"]))
            previous = by_key.get(key)
            if previous is not None:
                if previous != sample:
                    cut_conflicts.append(":".join(str(part) for part in key))
                continue
            by_key[key] = sample
    samples = list(by_key.values())
    samples.sort(key=lambda item: (int(item["deviceIdentity"]), int(item["mapEpoch"]),
                                   int(item["deviceEpoch"]), int(item["frame"]), int(item["stage"])))
    identities = sorted({(int(item["deviceIdentity"]), int(item["mapEpoch"]),
                          int(item["deviceEpoch"])) for item in samples})
    invalid_increment_samples: list[str] = []
    last_hits: dict[tuple[int, int, int], int] = {}
    max_hits = 0
    for sample in samples:
        identity = (int(sample["deviceIdentity"]), int(sample["mapEpoch"]), int(sample["deviceEpoch"]))
        hit_count = int(sample["uploadRangeHits"])
        max_hits = max(max_hits, hit_count)
        previous = last_hits.get(identity)
        if previous is not None and hit_count < previous:
            invalid_increment_samples.append(
                ":".join(str(part) for part in (*identity, int(sample["frame"]), int(sample["stage"]))))
        last_hits[identity] = hit_count
    return {
        "reports": len(aggregates),
        "missingCensusReports": missing_reports,
        "invalidCensusReports": invalid_reports,
        "incompleteCensusReports": incomplete_reports,
        "emptyCensusReports": empty_reports,
        "cutConflicts": sorted(set(cut_conflicts)),
        "invalidIncrementSamples": sorted(set(invalid_increment_samples)),
        "epochCount": len(identities),
        "uploadRangeHitsMax": max_hits,
        "canonicalCutCount": len(samples),
        "canonicalSamples": [
            {
                "deviceIdentity": int(item["deviceIdentity"]),
                "mapEpoch": int(item["mapEpoch"]),
                "deviceEpoch": int(item["deviceEpoch"]),
                "frame": int(item["frame"]),
                "stage": int(item["stage"]),
                "uploadRangeHits": int(item["uploadRangeHits"]),
                "unknownCounts": item.get("unknownCounts"),
                "unknownPositionBytes": item.get("unknownPositionBytes"),
            }
            for item in samples
        ],
    }


def compare_p2a_coverage(baseline_aggregates: list[dict[str, Any]],
                         candidate_aggregates: list[dict[str, Any]],
                         baseline_binding: dict[str, Any] | None = None,
                         candidate_binding: dict[str, Any] | None = None) -> dict[str, Any]:
    baseline = _p2a_run_coverage(baseline_aggregates)
    candidate = _p2a_run_coverage(candidate_aggregates)
    failures: list[str] = []
    uncovered: list[str] = []
    if baseline["cutConflicts"] or candidate["cutConflicts"]:
        failures.append("p2a_census_cut_conflict")
    if baseline["invalidIncrementSamples"] or candidate["invalidIncrementSamples"]:
        failures.append("p2a_uploadRangeHits_decreased")
    if baseline["uploadRangeHitsMax"] > 0:
        failures.append("p2a_off_uploadRangeHits_nonzero")
    if candidate["uploadRangeHitsMax"] == 0:
        uncovered.append("p2a_on_zero_uploadRangeHits")
    if baseline["missingCensusReports"] or candidate["missingCensusReports"]:
        uncovered.append("p2a_census_missing_reports")
    if baseline["invalidCensusReports"] or candidate["invalidCensusReports"]:
        uncovered.append("p2a_census_invalid_reports")
    if baseline["incompleteCensusReports"] or candidate["incompleteCensusReports"]:
        uncovered.append("p2a_census_incomplete_reports")
    if baseline["emptyCensusReports"] or candidate["emptyCensusReports"]:
        uncovered.append("p2a_census_empty_samples")
    if baseline["epochCount"] > 1 or candidate["epochCount"] > 1:
        uncovered.append("p2a_census_scope_change")
    if baseline_binding is not None or candidate_binding is not None:
        baseline_upload = None if baseline_binding is None else (baseline_binding.get("env") or {}).get(
            "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE")
        candidate_upload = None if candidate_binding is None else (candidate_binding.get("env") or {}).get(
            "DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE")
        if (baseline_upload is None or candidate_upload is None or
                str(baseline_upload) not in ("0", "1") or str(candidate_upload) not in ("0", "1")):
            uncovered.append("p2a_off_on_binding_missing")
        else:
            if str(baseline_upload) != "0":
                failures.append("p2a_baseline_not_off")
            if str(candidate_upload) != "1":
                failures.append("p2a_candidate_not_on")
    uncovered.append("p2a_benefit_not_derived_from_totals")
    return {
        "verdict": "fail" if failures else ("uncovered" if uncovered else "pass"),
        "failures": sorted(set(failures)),
        "uncovered": sorted(set(uncovered)),
        "metrics": {"baseline": baseline, "candidate": candidate},
        "benefitProven": False,
        "visualRecoveryProven": False,
    }


def _rows_between(series: dict[str, Any], start: int, end: int) -> tuple[list[list[Any]], str]:
    _require(start <= end, "phase interval inverted")
    position = series["index"]["businessFrameSerial"]
    selected = [row for row in series["rows"] if _is_uint(row[position]) and start <= row[position] <= end]
    return selected, "businessFrameSerial"


def _numeric_values(series: dict[str, Any], rows: list[list[Any]], column: str) -> list[float]:
    position = series["index"].get(column)
    _require(position is not None, "missing column: " + column)
    values: list[float] = []
    for row in rows:
        value = row[position]
        _require(value is not None and _is_finite(value), column + " must be present and finite")
        values.append(float(value))
    return values


def normalize_phases(phases: Any) -> dict[str, Any]:
    _require(isinstance(phases, dict), "phases must be an object")
    result: dict[str, Any] = {"source": "explicit-phases"}
    for key in ("pressureStart", "pressureEnd", "reliefStart", "reliefEnd"):
        value = phases.get(key)
        _require(_is_uint(value), "phase frame missing/zero: " + key)
        result[key] = int(value)
    _require(result["pressureStart"] <= result["pressureEnd"] < result["reliefStart"] <= result["reliefEnd"],
             "phase order invalid")
    if "runId" in phases:
        _require(str(phases["runId"]).strip() != "", "phase runId empty")
        result["runId"] = str(phases["runId"])
    if "pid" in phases:
        _require(_is_uint(phases["pid"]) and phases["pid"] > 0, "phase pid invalid")
        result["pid"] = int(phases["pid"])
    for key in ("pressureStartWallUnix", "reliefStartWallUnix"):
        if key in phases:
            _require(_is_finite(phases[key]) and float(phases[key]) > 0.0, "phase wall clock invalid: " + key)
            result[key] = float(phases[key])
    return result


def validate_phase_markers(markers: Any) -> dict[str, Any]:
    _require(isinstance(markers, list), "phaseMarkers must be a list")
    by_name: dict[str, dict[str, Any]] = {}
    for marker in markers:
        _require(isinstance(marker, dict), "phase marker must be an object")
        phase = str(marker.get("phase", ""))
        _require(phase in PHASE_NAMES, "unknown phase marker: " + phase)
        _require(phase not in by_name, "duplicate phase marker: " + phase)
        by_name[phase] = marker
    for name in PHASE_NAMES:
        _require(name in by_name, "missing phase marker: " + name)
    run_id = None
    pid = None
    normalized: dict[str, dict[str, Any]] = {}
    business_serials: list[int] = []
    perf_epochs: list[int] = []
    producer_epochs: list[int] = []
    for name in PHASE_NAMES:
        marker = by_name[name]
        marker_run_id = str(marker.get("runId", "")).strip()
        _require(marker_run_id != "", "phase marker runId missing: " + name)
        if run_id is None:
            run_id = marker_run_id
        _require(marker_run_id == run_id, "phase marker runId changed: " + name)
        marker_pid = marker.get("pid")
        _require(_is_uint(marker_pid) and marker_pid > 0, "phase marker pid invalid: " + name)
        if pid is None:
            pid = int(marker_pid)
        _require(int(marker_pid) == pid, "phase marker pid changed: " + name)
        wall = marker.get("wallUnix")
        _require(_is_finite(wall) and float(wall) > 0.0, "phase marker wallUnix invalid: " + name)
        readiness = marker.get("readiness")
        _require(isinstance(readiness, dict), "phase marker readiness missing: " + name)
        if marker.get("frameDomain") != "workload.businessFrameSerial":
            raise PhaseMarkerEvidenceError(
                "phase marker frameDomain missing/invalid: " + name, missing=True)
        frame_number = marker.get("businessFrameSerial")
        if not (_is_uint(frame_number) and frame_number > 0):
            raise PhaseMarkerEvidenceError(
                "phase marker businessFrameSerial missing/zero: " + name, missing=True)
        perf_epoch = marker.get("perfFrameEpoch")
        if not (_is_uint(perf_epoch) and perf_epoch > 0):
            raise PhaseMarkerEvidenceError(
                "phase marker perfFrameEpoch missing/zero: " + name, missing=True)
        producer_epoch = marker.get("producerAccumulationEpoch")
        if not (_is_uint(producer_epoch) and producer_epoch > 0):
            raise PhaseMarkerEvidenceError(
                "phase marker producerAccumulationEpoch missing/zero: " + name, missing=True)
        business_serials.append(int(frame_number))
        perf_epochs.append(int(perf_epoch))
        producer_epochs.append(int(producer_epoch))
        normalized[name] = marker
    if any(current <= previous for previous, current in zip(business_serials, business_serials[1:])):
        raise PhaseMarkerEvidenceError(
            "phase marker businessFrameSerial must be strictly increasing", missing=False)
    if any(current <= previous for previous, current in zip(perf_epochs, perf_epochs[1:])):
        raise PhaseMarkerEvidenceError(
            "phase marker perfFrameEpoch must be strictly increasing", missing=False)
    if len(set(producer_epochs)) != 1:
        raise PhaseMarkerEvidenceError(
            "phase marker producerAccumulationEpoch changed", missing=False)
    pressure_start = int(normalized["pressure-start"]["businessFrameSerial"]) + 1
    pressure_end = int(normalized["pressure-end"]["businessFrameSerial"])
    relief_start = int(normalized["relief-start"]["businessFrameSerial"]) + 1
    relief_end = int(normalized["relief-end"]["businessFrameSerial"])
    _require(pressure_start <= pressure_end < relief_start <= relief_end, "phase marker order invalid")
    result = {
        "source": "phaseMarkers.json",
        "runId": run_id,
        "pid": pid,
        "pressureStart": pressure_start,
        "pressureEnd": pressure_end,
        "reliefStart": relief_start,
        "reliefEnd": relief_end,
        "producerAccumulationEpoch": producer_epochs[0],
        "perfFrameEpochs": dict(zip(PHASE_NAMES, perf_epochs)),
        "markers": normalized,
    }
    result["pressureStartWallUnix"] = float(normalized["pressure-start"]["wallUnix"])
    result["reliefStartWallUnix"] = float(normalized["relief-start"]["wallUnix"])
    return result


def _load_phases(run_dir: Path, explicit: Path | None) -> dict[str, Any] | None:
    if explicit is not None:
        return normalize_phases(read_json_strict(explicit))
    marker_path = run_dir / "phaseMarkers.json"
    if marker_path.is_file():
        return validate_phase_markers(read_json_strict(marker_path))
    return None


def _invalid_runs(flags: list[bool]) -> list[tuple[int, int]]:
    runs: list[tuple[int, int]] = []
    start = None
    for index, valid in enumerate(flags):
        if not valid and start is None:
            start = index
        if valid and start is not None:
            runs.append((start, index - 1))
            start = None
    if start is not None:
        runs.append((start, len(flags) - 1))
    return runs


def _sustained_tail(flags: list[bool]) -> int:
    length = 0
    for valid in reversed(flags):
        if not valid:
            break
        length += 1
    return length


def evaluate_recovery_window(series: dict[str, Any], phases: dict[str, Any],
                             contract: dict[str, int] | None = None) -> dict[str, Any]:
    contract_values = dict(RECOVERY_CONTRACT)
    if contract:
        contract_values.update(contract)
    contract = contract_values
    pressure_rows, pressure_basis = _rows_between(series, phases["pressureStart"], phases["pressureEnd"])
    relief_rows, relief_basis = _rows_between(series, phases["reliefStart"], phases["reliefEnd"])
    metrics: dict[str, Any] = {
        "pressureFrames": len(pressure_rows),
        "reliefFrames": len(relief_rows),
        "pressureBasis": pressure_basis,
        "reliefBasis": relief_basis,
        "contract": dict(contract),
    }
    failures: list[str] = []
    if not pressure_rows or not relief_rows:
        failures.append("phase_rows_missing")
        return {"verdict": "fail", "failures": failures, "metrics": metrics,
                "visualRecoveryProven": False}
    if len(relief_rows) < contract["minReliefFrames"]:
        failures.append("insufficient_relief_frames")
    pressure_serial = _numeric_values(series, pressure_rows, "shadowMapRenderSerial")
    relief_serial = _numeric_values(series, relief_rows, "shadowMapRenderSerial")
    relief_reuse = [int(value) for value in _numeric_values(series, relief_rows, "reusedLastCompleteShadowMap")]
    relief_partial = [int(value) for value in _numeric_values(series, relief_rows, "renderedCurrentPartialShadowMap")]
    relief_receiver = [int(value) for value in _numeric_values(series, relief_rows, "shadowTaaReceiverExecuted")]
    relief_has_receiver = [int(value) for value in _numeric_values(series, relief_rows, "hasShadowReceiver")]
    relief_caster = [int(value) for value in _numeric_values(series, relief_rows, "replayCasterCount")]
    relief_skip = [int(value) for value in _numeric_values(series, relief_rows, "skippedCasterCap")]
    previous_serial = max(int(value) for value in pressure_serial)
    valid_flags: list[bool] = []
    fresh_flags: list[bool] = []
    for index, (serial, reuse, partial) in enumerate(zip(relief_serial, relief_reuse, relief_partial)):
        serial_int = int(serial)
        fresh = serial_int > previous_serial
        receiver_ok = (relief_receiver[index] == 1
                       and relief_has_receiver[index] == 1
                       and relief_caster[index] > 0)
        valid = (partial == 0 and receiver_ok and relief_skip[index] == 0
                 and (fresh or reuse == 1))
        fresh_flags.append(bool(fresh and partial == 0 and receiver_ok
                                and relief_skip[index] == 0))
        valid_flags.append(bool(valid))
        previous_serial = max(previous_serial, serial_int)
    invalid_runs = _invalid_runs(valid_flags)
    max_invalid_run = max((end - start + 1 for start, end in invalid_runs), default=0)
    sustained_tail = _sustained_tail(valid_flags)
    receiver_active_frames = sum(
        1 for index, value in enumerate(relief_receiver)
        if value == 1 and relief_has_receiver[index] == 1 and relief_caster[index] > 0
    )
    metrics.update({
        "pressureRenderSerialMax": max(int(value) for value in pressure_serial),
        "reliefRenderSerialMax": max(int(value) for value in relief_serial),
        "reliefRenderSerialFirst": int(relief_serial[0]),
        "reliefRenderSerialLast": int(relief_serial[-1]),
        "validFrames": sum(1 for value in valid_flags if value),
        "invalidFrames": sum(1 for value in valid_flags if not value),
        "sustainedTailFrames": sustained_tail,
        "maxInvalidRunFrames": max_invalid_run,
        "invalidRuns": invalid_runs,
        "freshCompleteFrames": sum(1 for value in fresh_flags if value),
        "reuseFrames": sum(1 for value in relief_reuse if value == 1),
        "partialFrames": sum(1 for value in relief_partial if value == 1),
        "receiverActiveFrames": receiver_active_frames,
        "casterMin": min(relief_caster),
        "casterMean": sum(relief_caster) / len(relief_caster),
        "skipCasterCapTotal": sum(relief_skip),
    })
    if any(serial < previous for serial, previous in zip(relief_serial[1:], relief_serial[:-1])):
        failures.append("render_serial_decreased")
    if max_invalid_run > contract["maxInvalidRunFrames"]:
        failures.append("relief_invalid_run_exceeds_contract")
    if sustained_tail < contract["minSustainedValidFrames"]:
        failures.append("relief_not_sustained_to_end")
    if metrics["freshCompleteFrames"] < contract["minFreshCompleteFrames"]:
        failures.append("relief_no_fresh_complete_render")
    if not valid_flags[-1]:
        failures.append("relief_last_frame_invalid")
    if receiver_active_frames <= 0:
        failures.append("relief_receiver_inactive")
    if any(value <= 0 for value in relief_caster):
        failures.append("caster_zero")
    if any(value != 0 for value in relief_skip):
        failures.append("caster_cap_skip_nonzero")
    if any(value != 0 for value in relief_partial):
        failures.append("partial_shadow_publication")
    return {"verdict": "fail" if failures else "pass", "failures": failures,
            "metrics": metrics, "visualRecoveryProven": False}


def evaluate_report(data: dict[str, Any], binding: dict[str, Any], phases: dict[str, Any] | None,
                    require_census: bool = False) -> dict[str, Any]:
    failures: list[str] = []
    uncovered: list[str] = []
    metrics: dict[str, Any] = {}
    meta = _meta_binding(data, binding)
    metrics["metaBinding"] = meta
    if not meta["ok"]:
        uncovered.append("report_meta_env_coverage_missing:" + ",".join(meta["missing"]))
    series = None
    aggregate = None
    try:
        series = extract_series(data)
        aggregate = extract_aggregate(data)
        metrics["workloadRows"] = series["rowCount"]
        metrics["aggregate"] = aggregate
    except EvidenceError as exc:
        failures.append("workload_or_aggregate:" + str(exc))
    if series is not None and phases is not None:
        recovery = evaluate_recovery_window(series, phases)
        metrics["recovery"] = recovery
        failures.extend(recovery.get("failures", []))
    elif series is not None:
        uncovered.append("phase_markers_missing")
    if aggregate is not None:
        if require_census:
            if aggregate.get("census") is None:
                failures.append("census_missing")
            elif aggregate.get("censusError"):
                failures.append("census_invalid:" + str(aggregate["censusError"]))
            elif not (aggregate.get("censusResult") or {}).get("covered"):
                failures.append("census_incomplete")
        else:
            uncovered.append("census_not_requested")
        if any(value != 0 for value in aggregate["cumulativeFailure"].values()):
            uncovered.append("post_relief_aggregate_delta_unavailable")
    verdict = "fail" if failures else ("uncovered" if uncovered else "pass")
    sources = {
        "candidateAndProfile": "raw report meta",
        "mapAndResolutionAndSidecarEnvironment": "frozen binding.json",
        "backbufferClientRect": "runner resolution.json (must be bound by frozen manifest)",
        "phaseOwnership": "phaseMarkers.json businessFrameSerial and runId/pid",
        "receiverAndCaster": "raw workloadSeries",
        "aggregateFailureSemantics": "war3_perf_monitor.cpp cumulative adders only; Last gauges never subtracted",
    }
    return {
        "verdict": verdict,
        "failures": sorted(set(failures)),
        "uncovered": sorted(set(uncovered)),
        "metrics": metrics,
        "evidenceSources": sources,
        "visualRecoveryProven": False,
    }


def _report_frame_range(series: dict[str, Any]) -> tuple[int, int]:
    position = series["index"]["businessFrameSerial"]
    values = [int(row[position]) for row in series["rows"]]
    return min(values), max(values)


def _discover_reports(run_dir: Path) -> list[Path]:
    receipt_path = run_dir / "receipt.json"
    reports: list[Path] = []
    if receipt_path.is_file():
        receipt = read_json_strict(receipt_path)
        for item in receipt.get("reports", []):
            if isinstance(item, dict) and item.get("path"):
                path = Path(str(item["path"]))
                if path.is_file():
                    reports.append(path)
    if not reports:
        reports = sorted(run_dir.glob("*.html"))
    return reports


def _load_parsed_reports(run_dir: Path, binding: dict[str, Any]) -> list[dict[str, Any]]:
    parsed: list[dict[str, Any]] = []
    for path in _discover_reports(run_dir):
        data = parse_report(path)
        report_hash = identity(path)
        expected_hash = (binding.get("reportHashes") or {}).get(path.name)
        if expected_hash is not None:
            _require(expected_hash.get("size") == report_hash["size"], "report hash size mismatch: " + path.name)
            _require(str(expected_hash.get("sha256", "")).upper() == report_hash["sha256"],
                     "report hash mismatch: " + path.name)
        series = extract_series(data)
        aggregate = extract_aggregate(data)
        parsed.append({"path": path, "data": data, "series": series, "aggregate": aggregate,
                       "range": _report_frame_range(series)})
    return parsed


def _post_relief_growth(parsed: list[dict[str, Any]], phases: dict[str, Any]) -> dict[str, Any]:
    failure_keys = list(CUMULATIVE_FAILURE_KEYS)
    relief_start = int(phases["reliefStart"])
    relief_end = int(phases["reliefEnd"])
    ordered = sorted(parsed, key=lambda item: (item["range"][0], item["range"][1]))
    scope_changes: list[str] = []
    for previous, current in zip(ordered, ordered[1:]):
        prev_scope = previous["aggregate"].get("scope", {})
        cur_scope = current["aggregate"].get("scope", {})
        if (prev_scope.get("producerSealMapEpochLast") != cur_scope.get("producerSealMapEpochLast") or
                prev_scope.get("producerSealDeviceEpochLast") != cur_scope.get("producerSealDeviceEpochLast")):
            scope_changes.append(previous["path"].name + "->" + current["path"].name)
        if cur_scope.get("producerSealFrameSerialLast", 0) < prev_scope.get("producerSealFrameSerialLast", 0):
            scope_changes.append("frameSerialDecrease:" + previous["path"].name + "->" + current["path"].name)
    straddling = [item for item in ordered if item["range"][0] < relief_start <= item["range"][1]]
    overlaps: list[str] = []
    for previous, current in zip(ordered, ordered[1:]):
        if current["range"][0] <= previous["range"][1]:
            overlaps.append(previous["path"].name + "->" + current["path"].name)
    max_report_end = max((item["range"][1] for item in ordered), default=None)
    metrics = {
        "postReliefReports": [item["path"].name for item in ordered if item["range"][0] >= relief_start],
        "straddlingReports": [item["path"].name for item in straddling],
        "overlaps": overlaps,
        "maxReportEnd": max_report_end,
        "scopeChanges": scope_changes,
        "scopeKeys": [item["aggregate"].get("scope", {}) for item in ordered],
        "scopeSemantics": "producerSealFrameSerialLast is export aggregate last observation, not a completed-frame cut; never use report rolling window start as cumulative cut",
    }
    if scope_changes:
        return {"verdict": "fail", "failures": sorted(set("post_relief_scope_change:" + item
                                                            for item in scope_changes)),
                "uncovered": [], "metrics": metrics}
    if not ordered:
        return {"verdict": "uncovered", "failures": [], "uncovered": ["post_relief_no_reports"],
                "metrics": metrics}
    epochs: list[int] = []
    for item in ordered:
        raw_epoch = item["aggregate"].get("scope", {}).get("producerAccumulationEpoch")
        if raw_epoch is None:
            return {"verdict": "uncovered", "failures": [],
                    "uncovered": ["cumulative_accumulation_epoch_missing"], "metrics": metrics}
        if not _is_uint(raw_epoch) or int(raw_epoch) == 0:
            return {"verdict": "uncovered", "failures": [],
                    "uncovered": ["cumulative_accumulation_epoch_invalid"], "metrics": metrics}
        epochs.append(int(raw_epoch))
    if len(set(epochs)) != 1:
        return {"verdict": "fail", "failures": ["post_relief_accumulation_epoch_change"],
                "uncovered": [], "metrics": metrics}
    phase_epoch = phases.get("producerAccumulationEpoch")
    if phase_epoch is not None:
        if not _is_uint(phase_epoch) or int(phase_epoch) == 0:
            return {"verdict": "uncovered", "failures": [],
                    "uncovered": ["phase_producer_accumulation_epoch_invalid"], "metrics": metrics}
        if any(epoch != int(phase_epoch) for epoch in epochs):
            return {"verdict": "fail", "failures": ["phase_report_accumulation_epoch_mismatch"],
                    "uncovered": [], "metrics": metrics}
    if max_report_end is None or int(max_report_end) < relief_end:
        return {"verdict": "uncovered", "failures": [],
                "uncovered": ["post_relief_export_before_relief_end"], "metrics": metrics}
    all_zero = all(all(item["aggregate"]["cumulativeFailure"][key] == 0 for key in failure_keys)
                   for item in ordered)
    metrics["allCumulativeFailureZero"] = all_zero
    decreases: list[str] = []
    for previous, current in zip(ordered, ordered[1:]):
        for key in failure_keys:
            if (current["aggregate"]["cumulativeFailure"][key] <
                    previous["aggregate"]["cumulativeFailure"][key]):
                decreases.append(key)
    if decreases:
        return {"verdict": "fail",
                "failures": sorted(set("post_relief_counter_reset_or_scope_change:" + key
                                       for key in decreases)),
                "uncovered": [], "metrics": metrics}
    if all_zero:
        return {"verdict": "pass", "failures": [], "uncovered": [], "metrics": metrics}
    metrics["cutProof"] = "missing: producer completed-frame cut/session domain not yet available"
    return {"verdict": "uncovered", "failures": [],
            "uncovered": ["post_relief_cut_proof_missing"], "metrics": metrics}

def validate_report_envelope(data: dict[str, Any], binding: dict[str, Any],
                            require_census: bool) -> dict[str, Any]:
    failures: list[str] = []
    uncovered: list[str] = []
    metrics: dict[str, Any] = {}
    try:
        meta = _meta_binding(data, binding)
        metrics["metaBinding"] = meta
        if not meta["ok"]:
            uncovered.append("report_meta_env_coverage_missing:" + ",".join(meta["missing"]))
        series = extract_series(data)
        aggregate = extract_aggregate(data)
        metrics["workloadRows"] = series["rowCount"]
    except EvidenceError as exc:
        failures.append("report_envelope:" + str(exc))
        return {"verdict": "fail", "failures": failures, "uncovered": uncovered,
                "metrics": metrics, "visualRecoveryProven": False}
    if require_census:
        if aggregate.get("census") is None:
            failures.append("census_missing")
        elif aggregate.get("censusError"):
            failures.append("census_invalid:" + str(aggregate["censusError"]))
        elif not (aggregate.get("censusResult") or {}).get("covered"):
            failures.append("census_incomplete")
    else:
        uncovered.append("census_not_requested")
    verdict = "fail" if failures else ("uncovered" if uncovered else "pass")
    return {"verdict": verdict, "failures": failures, "uncovered": uncovered,
            "metrics": metrics, "series": series, "aggregate": aggregate,
            "visualRecoveryProven": False}


def merge_run_series(parsed: list[dict[str, Any]]) -> dict[str, Any]:
    merged: dict[int, dict[str, Any]] = {}
    duplicate_frames = 0
    conflicts: list[int] = []
    for item in parsed:
        series = item["series"]
        index = series["index"]
        for row in series["rows"]:
            serial = int(row[index["businessFrameSerial"]])
            row_map = {column: row[index[column]] for column in REQUIRED_WORKLOAD_COLUMNS}
            existing = merged.get(serial)
            if existing is None:
                merged[serial] = row_map
                continue
            duplicate_frames += 1
            if existing != row_map:
                conflicts.append(serial)
    if conflicts:
        raise EvidenceError("conflicting duplicate business frames: " +
                            ",".join(str(value) for value in sorted(set(conflicts))[:16]))
    columns = list(REQUIRED_WORKLOAD_COLUMNS)
    index = {column: position for position, column in enumerate(columns)}
    rows = [[merged[serial][column] for column in columns] for serial in sorted(merged)]
    return {"columns": columns, "index": index, "rows": rows, "rowCount": len(rows),
            "sourceReportCount": len(parsed), "duplicateFramesDeduplicated": duplicate_frames,
            "conflictingFrames": []}


def _coverage_gaps(series: dict[str, Any], phases: dict[str, Any]) -> list[list[int]]:
    position = series["index"]["businessFrameSerial"]
    present = {int(row[position]) for row in series["rows"]}
    start = int(phases["pressureStart"])
    end = int(phases["reliefEnd"])
    missing = [frame for frame in range(start, end + 1) if frame not in present]
    gaps: list[list[int]] = []
    for frame in missing:
        if gaps and frame == gaps[-1][1] + 1:
            gaps[-1][1] = frame
        else:
            gaps.append([frame, frame])
    return gaps


def evaluate_union(union: dict[str, Any], phases: dict[str, Any]) -> dict[str, Any]:
    gaps = _coverage_gaps(union, phases)
    recovery = evaluate_recovery_window(union, phases)
    uncovered = list(recovery.get("uncovered", []))
    if gaps:
        uncovered.append("business_frame_coverage_gaps")
    verdict = "fail" if recovery.get("failures") else ("uncovered" if uncovered else "pass")
    return {"verdict": verdict, "failures": list(recovery.get("failures", [])),
            "uncovered": sorted(set(uncovered)), "metrics": recovery.get("metrics", {}),
            "coverageGaps": gaps, "unionRows": union["rowCount"],
            "duplicateFramesDeduplicated": union["duplicateFramesDeduplicated"],
            "visualRecoveryProven": False}


def evaluate_run(run_dir: Path, require_census: bool = False,
                 phases_path: Path | None = None) -> dict[str, Any]:
    run_dir = Path(run_dir)
    failures: list[str] = []
    uncovered: list[str] = []
    binding: dict[str, Any] = {}
    phases = None
    try:
        binding = load_binding_from_dir(run_dir)
        validate_binding(binding)
    except EvidenceError as exc:
        return {"verdict": "fail", "failures": ["frozen_binding:" + str(exc)],
                "uncovered": [], "visualRecoveryProven": False}
    try:
        phases = _load_phases(run_dir, phases_path)
    except PhaseMarkerEvidenceError as exc:
        if exc.missing:
            uncovered.append("phase_marker_domain_missing:" + str(exc))
        else:
            failures.append("phase_markers_invalid:" + str(exc))
    except EvidenceError as exc:
        failures.append("phase_markers_invalid:" + str(exc))
    try:
        parsed = _load_parsed_reports(run_dir, binding)
    except EvidenceError as exc:
        return {"verdict": "fail", "failures": ["report_load:" + str(exc)],
                "uncovered": [], "binding": binding, "visualRecoveryProven": False}
    if not parsed:
        return {"verdict": "fail", "failures": ["no_reports"], "uncovered": [],
                "binding": binding, "visualRecoveryProven": False}
    results: list[dict[str, Any]] = []
    parsed_by_serial: dict[int, dict[str, Any]] = {}
    for item in parsed:
        result = validate_report_envelope(item["data"], binding, require_census)
        result["path"] = str(item["path"])
        result["frameRange"] = item["range"]
        results.append(result)
        failures.extend(result.get("failures", []))
        uncovered.extend(result.get("uncovered", []))
        parsed_by_serial[item["range"][0]] = result
    union = None
    union_result = None
    if phases is None:
        uncovered.append("phase_markers_missing")
    else:
        try:
            union = merge_run_series(parsed)
            union_result = evaluate_union(union, phases)
            failures.extend(union_result.get("failures", []))
            uncovered.extend(union_result.get("uncovered", []))
        except EvidenceError as exc:
            failures.append("run_union:" + str(exc))
        growth = _post_relief_growth(parsed, phases)
        failures.extend(growth.get("failures", []))
        uncovered.extend(growth.get("uncovered", []))
    verdict = "fail" if failures else ("uncovered" if uncovered else "pass")
    return {
        "verdict": verdict,
        "failures": sorted(set(failures)),
        "uncovered": sorted(set(uncovered)),
        "binding": binding,
        "phases": phases,
        "reports": results,
        "union": union_result,
        "visualRecoveryProven": False,
    }


def _caster_stats(values: list[float]) -> dict[str, float]:
    _require(values, "caster values empty")
    ordered = sorted(values)
    return {
        "count": float(len(values)),
        "min": ordered[0],
        "mean": sum(ordered) / len(ordered),
        "p05": ordered[max(0, int(len(ordered) * 0.05) - 1)],
        "max": ordered[-1],
        "sum": sum(ordered),
        "zeroCount": float(sum(1 for value in ordered if value == 0)),
    }


def compare_caster_coverage(baseline_series: dict[str, Any], candidate_series: dict[str, Any],
                            baseline_phases: dict[str, Any], candidate_phases: dict[str, Any]) -> dict[str, Any]:
    failures: list[str] = []
    uncovered: list[str] = []
    metrics: dict[str, Any] = {}
    for phase_name, keys in (("pressure", ("pressureStart", "pressureEnd")),
                             ("relief", ("reliefStart", "reliefEnd"))):
        baseline_rows, _ = _rows_between(baseline_series, baseline_phases[keys[0]], baseline_phases[keys[1]])
        candidate_rows, _ = _rows_between(candidate_series, candidate_phases[keys[0]], candidate_phases[keys[1]])
        if not baseline_rows or not candidate_rows:
            failures.append(phase_name + "_rows_missing")
            continue
        if len(baseline_rows) != len(candidate_rows):
            uncovered.append(phase_name + "_uncomparable_frame_count")
            continue
        baseline_stats = _caster_stats(_numeric_values(baseline_series, baseline_rows, "replayCasterCount"))
        candidate_stats = _caster_stats(_numeric_values(candidate_series, candidate_rows, "replayCasterCount"))
        baseline_skip = sum(_numeric_values(baseline_series, baseline_rows, "skippedCasterCap"))
        candidate_skip = sum(_numeric_values(candidate_series, candidate_rows, "skippedCasterCap"))
        metrics[phase_name] = {
            "baseline": baseline_stats,
            "candidate": candidate_stats,
            "baselineSkipCasterCap": baseline_skip,
            "candidateSkipCasterCap": candidate_skip,
            "comparisonKind": "statistical-reference-not-object-proof",
        }
        if candidate_stats["min"] < baseline_stats["min"] or candidate_stats["mean"] < baseline_stats["mean"]:
            failures.append(phase_name + "_caster_reduced")
        if candidate_stats["zeroCount"] > baseline_stats["zeroCount"]:
            failures.append(phase_name + "_caster_zero_increase")
        if candidate_skip > baseline_skip:
            failures.append(phase_name + "_caster_cap_skip_increase")
    verdict = "fail" if failures else ("uncovered" if uncovered else "pass")
    return {"verdict": verdict, "failures": sorted(set(failures)), "uncovered": sorted(set(uncovered)),
            "metrics": metrics, "perObjectCompletenessProven": False}


def compare_runs(baseline_dir: Path, candidate_dir: Path, require_census: bool = False,
                 phases_path: Path | None = None) -> dict[str, Any]:
    baseline = evaluate_run(baseline_dir, require_census, phases_path)
    candidate = evaluate_run(candidate_dir, require_census, phases_path)
    failures = list(baseline.get("failures", [])) + list(candidate.get("failures", []))
    uncovered = list(baseline.get("uncovered", [])) + list(candidate.get("uncovered", []))
    coverage = {"verdict": "uncovered", "uncovered": ["caster_coverage_not_evaluated"],
                "failures": [], "metrics": {}, "perObjectCompletenessProven": False}
    p2a = {"verdict": "uncovered", "uncovered": ["p2a_coverage_not_evaluated"],
           "failures": [], "metrics": {}, "benefitProven": False, "visualRecoveryProven": False}
    try:
        for key in ("mapSha256", "mapSize", "profile", "resolution", "route", "matrix",
                    "isolatedDesktop", "globalInputUsed", "heavyForensicsOff",
                    "snapshotResidentCapMb", "candidateSha256", "candidateSize"):
            if baseline["binding"].get(key) != candidate["binding"].get(key):
                failures.append("run_binding_mismatch:" + key)
        baseline_phases = baseline.get("phases") or _load_phases(Path(baseline_dir), phases_path)
        candidate_phases = candidate.get("phases") or _load_phases(Path(candidate_dir), phases_path)
        if baseline_phases and candidate_phases:
            baseline_reports = _load_parsed_reports(Path(baseline_dir), baseline["binding"])
            candidate_reports = _load_parsed_reports(Path(candidate_dir), candidate["binding"])
            if baseline_reports and candidate_reports:
                baseline_series = merge_run_series(baseline_reports)
                candidate_series = merge_run_series(candidate_reports)
                coverage = compare_caster_coverage(baseline_series, candidate_series,
                                                   baseline_phases, candidate_phases)
                p2a = compare_p2a_coverage(
                    [item["aggregate"] for item in baseline_reports],
                    [item["aggregate"] for item in candidate_reports],
                    baseline.get("binding"), candidate.get("binding"))
                failures.extend(coverage.get("failures", []))
                uncovered.extend(coverage.get("uncovered", []))
        else:
            uncovered.append("caster_coverage_phase_missing")
    except Exception as exc:
        uncovered.append("caster_coverage_unavailable:" + str(exc))
    failures.extend(p2a.get("failures", []))
    uncovered.extend(p2a.get("uncovered", []))
    verdict = "fail" if failures else ("uncovered" if uncovered else "pass")
    return {"verdict": verdict, "failures": sorted(set(failures)), "uncovered": sorted(set(uncovered)),
            "baseline": baseline, "candidate": candidate, "casterCoverage": coverage,
            "p2aCoverage": p2a, "visualRecoveryProven": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", action="append", default=[], type=Path)
    parser.add_argument("--require-census", action="store_true")
    parser.add_argument("--phases-json", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if len(args.run_dir) == 1:
        result = evaluate_run(args.run_dir[0], args.require_census, args.phases_json)
    elif len(args.run_dir) == 2:
        result = compare_runs(args.run_dir[0], args.run_dir[1], args.require_census, args.phases_json)
    else:
        parser.error("pass one or two --run-dir arguments")
    text = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True)
    if args.output:
        args.output.write_bytes((text + "\n").encode("utf-8"))
    print(text)
    return 0 if result["verdict"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
