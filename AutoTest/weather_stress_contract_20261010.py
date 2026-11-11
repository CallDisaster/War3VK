"""Pure contract for the real-JASS weather stress suite; no launch/stop code.

Only current backing is certified. Retired in-flight, pipeline/driver, process
private bytes and pixels are not automatically certified by this CPU receipt.
"""
from __future__ import annotations

import math
from pathlib import Path
import re

HARNESS = Path(__file__).with_name("weather_stress_20261010.j")
MAX_RUN_SECONDS = 600
FINAL_STAGE = 69
POSTFX_UNSUPPORTED_FEATURE = 18
NON_ACCEPTANCE = ("ordinaryCurrentResourceAccountingCertified",
                  "gpuCompletionCertified", "pixelOrArtisticAcceptance",
                  "processLeakCertified", "gpuResourceRecreationCertified")
CAPTURES = {0: "ordinary-baseline", 1: "sun-fog-weather-coexistence",
            2: "sun-weather-without-ordinary-fog", 3: "ordinary-fog-restored",
            34: "middle-weather-recovery", 66: "last-weather-rebuild",
            68: "destroyed-tail"}
PARAMS = dict(zip(("snowfall", "windAngle", "windSpeed", "gust", "haze", "lowSnow"),
                  (0.65, 30.0, 160.0, 0.4, 0.45, 0.5)))
FLOAT_TOKEN = r"[-+\d.eE]+"
RECEIPT = re.compile(
    rf"WEATHER_STRESS_STAGE=(\d+);checks=(\d+);errors=(\d+);id=(-?\d+);time=({FLOAT_TOKEN})"
    rf";cycle=(-?\d+);weather=([01]);sun=([01]);ordinary=([01]);played=([01]);query=({FLOAT_TOKEN})"
    rf";oldId=(-?\d+);oldRejected=([01])"
    rf"(?:;postfxRefusalCode=(-?\d+))?")


def _base():
    # Lazy to allow the base launcher to import this suite during selection.
    import run_weather_canary_20261010
    return run_weather_canary_20261010


def expected_stage(stage: int) -> dict:
    if not 0 <= stage <= FINAL_STAGE:
        raise ValueError("stress stage out of range")
    cycle = (stage - 4) // 2 if 4 <= stage <= 67 else 32 if stage >= 68 else -1
    enabled = stage in (1, 2, 3) or 4 <= stage <= 67 and stage % 2 == 0
    rebuilt = 4 <= stage <= 67 and stage % 2 == 0 and (cycle + 1) % 4 == 0
    return {"stage": stage, "cycle": cycle, "weatherEnabled": enabled,
            "sunEnabled": stage < 4, "ordinaryEnabled": stage in (0, 1, 3),
            "played": stage == 1 or 4 <= stage <= 67 and stage % 2 == 0,
            "rebuilt": rebuilt, "staleWriteRefused": rebuilt or stage == 68,
            "duration": 30.0 if stage < 4 else 4.0 if enabled else 2.0 if stage < 68 else 60.0,
            "alive": stage < 68}


def expected_checks(stage: int) -> int:
    total = 0
    for current in range(stage + 1):
        if current == 0:
            total += 23  # Bridge3 + setters9 + create/error/alive/config9 + receipt2.
        elif current == 1:
            total += 14  # settings9 + play/error/query3 + receipt2.
        elif current in (2, 3):
            total += 11  # settings9 + receipt2.
        elif current == 69:
            total += 10  # settings9 + receipt1 (id0 skips alive query).
        elif current == 68:
            total += 14  # settings9 + destroy1/dead2/stale1/receipt1.
        else:
            # Rebuild: settings9+destroy1/dead2/create9/stale1/Play3/receipt2.
            # Off: settings9+disable1/query-error1/time-check1/receipt2.
            total += 27 if expected_stage(current)["rebuilt"] else 14
    return total


def stage_receipts(instance: Path) -> list[dict]:
    result = []
    for path in sorted((Path(instance) / "WarVK/Temp").glob("weather_stress_stage_*.txt")):
        text = path.read_bytes().decode("utf-8", errors="replace")
        matches = list(RECEIPT.finditer(text))
        for match in matches:
            values = match.groups()
            row = dict(zip(("stage", "checks", "errors", "id", "timeSeconds", "cycle",
                            "weatherEnabled", "sunEnabled", "ordinaryEnabled", "played",
                            "querySeconds", "oldId", "oldRejected", "postfxRefusalCode"), values))
            for key in ("stage", "checks", "errors", "id", "cycle", "oldId"):
                row[key] = int(row[key])
            for key in ("timeSeconds", "querySeconds"):
                row[key] = float(row[key])
            for key in ("weatherEnabled", "sunEnabled", "ordinaryEnabled", "played", "oldRejected"):
                row[key] = row[key] == "1"
            # Retain old stress-a receipts for honest re-analysis. Absence
            # never becomes proof of the new explicit unsupported-family gate.
            row["postfxRefusalCode"] = (int(row["postfxRefusalCode"])
                                        if row["postfxRefusalCode"] is not None else None)
            row.update({"path": str(path), "sha256": _base().sha256(path), "raw": text,
                        "recordsInFile": len(matches)})
            result.append(row)
    return sorted(result, key=lambda row: row["stage"])


def _number(value, fallback=float("nan")) -> float:
    try:
        number = float(value)
        return number if math.isfinite(number) else fallback
    except (TypeError, ValueError, OverflowError):
        return fallback


def _integer(value, fallback=0) -> int:
    try:
        number = int(value) if not isinstance(value, bool) else fallback
        return number if not isinstance(value, float) or value == number else fallback
    except (TypeError, ValueError, OverflowError):
        return fallback


def _params_match(params: dict) -> bool:
    # all(<) is intentional: a missing/non-finite value must fail, not slip
    # through any(>=) because comparisons with NaN are false.
    return isinstance(params, dict) and all(
        abs(_number(params.get(key)) - value) < 1.0e-4
        for key, value in PARAMS.items())


def _resource_bytes(value) -> int | None:
    """Known byte accounting must be a typed nonnegative integer, not zero fallback."""
    return value if type(value) is int and value >= 0 else None


def _positive_resource(value) -> bool:
    return type(value) is int and value > 0


def _alignment(obs: dict) -> dict:
    """Reuse the production canary helper, refusing malformed numeric input."""
    try:
        return _base().frame_alignment(obs)
    except (TypeError, ValueError, OverflowError, AttributeError):
        return {"render": {"sameFrozenFrame": False},
                "volumetric": {"sameFrozenFrame": False}}


def _rows_ok(rows: list[dict]) -> bool:
    if (len(rows) != FINAL_STAGE + 1 or any(not isinstance(row, dict) for row in rows)
            or [row.get("stage") for row in rows] != list(range(FINAL_STAGE + 1))):
        return False
    for row in rows:
        stage = row["stage"]
        expected = expected_stage(stage)
        if (row.get("checks") != expected_checks(stage) or row.get("errors") != 0
                or type(row.get("postfxRefusalCode")) is not int
                or row.get("postfxRefusalCode") != POSTFX_UNSUPPORTED_FEATURE
                or row.get("recordsInFile", 1) != 1
                or any(row.get(key) != expected[key] for key in
                       ("cycle", "weatherEnabled", "sunEnabled", "ordinaryEnabled", "played"))):
            return False
        if expected["alive"] is not (_integer(row.get("id")) > 0):
            return False
        seconds = _number(row.get("timeSeconds"))
        query = _number(row.get("querySeconds"))
        if not math.isfinite(seconds) or not math.isfinite(query) or seconds < 0 or query < 0:
            return False
        if expected["played"] and not (query < 0.08 and seconds < 0.08):
            return False
        if 4 <= stage <= 67 and stage % 2 and not (3.5 <= query < 6 and abs(seconds - query) < 0.08):
            return False
        if expected["rebuilt"]:
            if (row.get("oldRejected") is not True or _integer(row.get("oldId")) <= 0
                    or _integer(row.get("oldId")) != _integer(rows[stage - 1].get("id"))
                    or _integer(row.get("id")) <= _integer(row.get("oldId"))):
                return False
        elif (row.get("oldRejected") is not expected["staleWriteRefused"]
                or stage != 68 and _integer(row.get("oldId")) != 0):
            return False
        if stage and stage < 68 and not expected["rebuilt"] and row["id"] != rows[stage - 1]["id"]:
            return False
        if stage == 68 and row.get("oldId") != rows[67].get("id"):
            return False
        if stage >= 68 and (row.get("id") != 0 or seconds != 0 or query != 0):
            return False
    return True


def _settings_match(obs: dict, expected: dict) -> bool:
    settings = obs.get("authorSettings", {})
    return (settings.get("observationKind") == "owner-settings-live-state-not-render-input"
            and settings.get("sunEnabled") is expected["sunEnabled"]
            and settings.get("ordinaryVolumetricEnabled") is expected["ordinaryEnabled"]
            and settings.get("globalMediumEnabled") is True
            and abs(_number(settings.get("volumetricDensity")) - 0.12) < 1.0e-5
            and _integer(settings.get("volumetricBackend"), -1) == 2)


def _same_domain(value: dict, frozen: dict, exact_frame: bool) -> bool:
    observed = _integer(value.get("frameSerial"))
    frame = _integer(frozen.get("frameSerial"))
    return (frame > 0 and observed > 0 and (observed == frame if exact_frame else observed >= frame)
            and all(_integer(value.get(key)) > 0 and value.get(key) == frozen.get(key)
                    for key in ("mapEpoch", "deviceEpoch")))


def _owner_matches(obs: dict, stage: int, stages: dict) -> bool:
    start = stages.get(stage)
    if start is None or not 0 <= stage <= FINAL_STAGE:
        return False
    expected = expected_stage(stage)
    if not _settings_match(obs, expected):
        return False
    runtime = obs.get("runtime", {})
    if (runtime.get("observationKind") != "owner-control-live-state-not-render-input"
            or runtime.get("enabled") is not expected["weatherEnabled"]
            or runtime.get("paused") is not False
            or _integer(runtime.get("id")) != _integer(start.get("id"))
            or _integer(runtime.get("historyRevision")) <= 0):
        return False
    seconds = _number(runtime.get("timeSeconds"))
    receipt_seconds = _number(start.get("timeSeconds"))
    if not (receipt_seconds - 0.08 <= seconds <= receipt_seconds + expected["duration"] + 0.5):
        return False
    if not expected["weatherEnabled"] and abs(seconds - receipt_seconds) >= 0.08:
        return False
    if expected["alive"] and not _params_match(runtime.get("params", {})):
        return False
    return True


def phase_matches(obs: dict, stage: int, stages: dict) -> bool:
    if not _owner_matches(obs, stage, stages):
        return False
    start = stages[stage]
    expected = expected_stage(stage)
    runtime = obs["runtime"]
    seconds = _number(runtime.get("timeSeconds"))
    receipt_seconds = _number(start.get("timeSeconds"))
    if expected["weatherEnabled"]:
        frozen = obs.get("frozenInput", {})
        if (frozen.get("observationKind") != "render-consumed-value-snapshot"
                or frozen.get("enabled") is not True or frozen.get("paused") is not False
                or _integer(frozen.get("id")) != start.get("id")
                or frozen.get("historyRevision") != runtime.get("historyRevision")):
            return False
        frozen_seconds = _number(frozen.get("timeSeconds"))
        if not (receipt_seconds - 0.08 <= frozen_seconds <= seconds + 0.01):
            return False
        if not _params_match(frozen.get("params", {})):
            return False
    return True


def _ordinary_matches(obs: dict, expected: dict, frozen: dict | None = None) -> bool:
    ordinary = obs.get("ordinaryVolumetric", {})
    return (ordinary.get("compositeSubmitted") is expected["ordinaryEnabled"]
            and ordinary.get("stage") == ("composite-submitted" if expected["ordinaryEnabled"] else "disabled")
            and all(_integer(ordinary.get(key)) > 0
                    for key in ("frameSerial", "mapEpoch", "deviceEpoch"))
            and (not expected["ordinaryEnabled"] or
                 (_integer(ordinary.get("width")), _integer(ordinary.get("height"))) == (2560, 1440))
            and (frozen is None or _same_domain(ordinary, frozen, exact_frame=True)))


def capture_ready(observation: dict, stage: int, stages: dict) -> bool:
    if stage not in CAPTURES or not phase_matches(observation, stage, stages):
        return False
    expected = expected_stage(stage)
    if not _ordinary_matches(observation, expected):
        return False
    render = observation.get("render", {})
    volume = observation.get("volumetric", {})
    if not expected["weatherEnabled"]:
        if render.get("commandsRecorded") is not False or volume.get("compositeSubmitted") is not False:
            return False
        if stage >= 4 and (render.get("stage") != "disabled" or volume.get("stage") != "disabled"):
            return False
        if stage >= 4 and _integer(render.get("resourceBytes"), -1) != 0:
            return False
        return True
    alignment = _alignment(observation)
    frozen = observation["frozenInput"]
    return (alignment["render"]["sameFrozenFrame"] and alignment["volumetric"]["sameFrozenFrame"]
            and render.get("commandsRecorded") is True and render.get("stage") == "commands-recorded"
            and volume.get("compositeSubmitted") is True and volume.get("stage") == "composite-submitted"
            and _integer(render.get("capacity")) == 16384
            and abs(_integer(render.get("particleCount")) - int(16384 * PARAMS["snowfall"])) <= 1
            and _positive_resource(render.get("resourceBytes"))
            and _positive_resource(volume.get("resourceBytes"))
            and (_integer(volume.get("width")), _integer(volume.get("height"))) == (2560, 1440)
            and _ordinary_matches(observation, expected, frozen))


def classify(receipt: dict) -> dict:
    rows = receipt.get("stageReceipts", [])
    stages = {row["stage"]: row for row in rows
              if isinstance(row, dict) and type(row.get("stage")) is int}
    raw_samples = receipt.get("snapshots", [])
    grouped = {stage: [] for stage in range(FINAL_STAGE + 1)}
    for sample in raw_samples:
        if not isinstance(sample, dict):
            continue
        stage = sample.get("jassStage")
        if type(stage) is int and stage in grouped and sample.get("stageAtStatusCompletion") == stage:
            grouped[stage].append(sample)

    def active_sample(obs: dict, stage: int) -> bool:
        if not phase_matches(obs, stage, stages):
            return False
        expected = expected_stage(stage)
        if expected["weatherEnabled"]:
            # Reuse capture gate outside the small selected screenshot set.
            alignment = _alignment(obs)
            if not (alignment["render"]["sameFrozenFrame"] and alignment["volumetric"]["sameFrozenFrame"]):
                return False
            draw, volume, ordinary = (obs.get(name, {}) for name in
                                      ("render", "volumetric", "ordinaryVolumetric"))
            return (draw.get("commandsRecorded") is True and draw.get("stage") == "commands-recorded"
                    and _integer(draw.get("capacity")) == 16384
                    and abs(_integer(draw.get("particleCount")) - int(16384 * PARAMS["snowfall"])) <= 1
                    and _positive_resource(draw.get("resourceBytes"))
                    and volume.get("compositeSubmitted") is True and volume.get("stage") == "composite-submitted"
                    and _positive_resource(volume.get("resourceBytes"))
                    and (_integer(volume.get("width")), _integer(volume.get("height"))) == (2560, 1440)
                    and _ordinary_matches(obs, expected, obs["frozenInput"]))
        return _ordinary_matches(obs, expected)

    admitted = {stage: [row for row in samples if active_sample(row.get("observation", {}), stage)]
                for stage, samples in grouped.items()}
    # Count distinct actual frames, never duplicated or stored alignment booleans.
    coverage = True
    consumed_counts = {}
    for stage in range(FINAL_STAGE):
        expected = expected_stage(stage)
        required = 3 if expected["weatherEnabled"] or stage == 0 else 1
        frames = {_integer(row["observation"].get("ordinaryVolumetric", {}).get("frameSerial"))
                  for row in admitted[stage]}
        consumed_counts[stage] = len(frames - {0})
        coverage &= consumed_counts[stage] >= required

    drain = True
    for stage in list(range(5, 68, 2)) + [68]:
        # Destroy follows an already-disabled stage67. It must preserve the
        # proved stage67 stop, not fabricate an extra GPU consumption edge.
        previous = 66 if stage == 68 else stage - 1
        previous_frames = [_integer(row["observation"].get("render", {}).get("frameSerial"))
                           for row in admitted[previous]]
        lower_frame = max(previous_frames, default=0)
        found = False
        for sample in admitted[stage]:
            obs = sample["observation"]
            frozen, draw, volume = (obs.get(name, {}) for name in ("frozenInput", "render", "volumetric"))
            if (frozen.get("observationKind") == "render-consumed-value-snapshot"
                    and frozen.get("enabled") is False and _integer(frozen.get("id")) == 0
                    and _integer(frozen.get("frameSerial")) > lower_frame
                    and _alignment(obs)["render"]["sameFrozenFrame"]
                    and draw.get("commandsRecorded") is False and draw.get("stage") == "disabled"
                    and _integer(draw.get("resourceBytes"), -1) == 0
                    and volume.get("compositeSubmitted") is False and volume.get("stage") == "disabled"
                    and _same_domain(volume, frozen, exact_frame=False)):
                found = True
        drain &= found

    backing = {}
    stable = bool(raw_samples)
    histories = {}
    time_ok = True
    failure_stages_ok = True
    owner_consistent = bool(raw_samples)
    frozen_params_consistent = bool(raw_samples)
    for stage, samples in grouped.items():
        owner_consistent &= all(_owner_matches(sample.get("observation", {}), stage, stages)
                                for sample in samples)
        samples = [sample for sample in samples
                   if _owner_matches(sample.get("observation", {}), stage, stages)]
        if not samples:
            continue
        expected = expected_stage(stage)
        last_time = None
        for sample in samples:
            obs = sample["observation"]
            runtime = obs["runtime"]
            seconds = _number(runtime.get("timeSeconds"))
            if last_time is not None:
                time_ok &= seconds >= last_time - 1.0e-6 if expected["weatherEnabled"] else abs(seconds - last_time) < 0.08
            last_time = seconds
            history = _integer(runtime.get("historyRevision"))
            histories.setdefault(stage, set()).add(history)
            if expected["weatherEnabled"]:
                frozen = obs.get("frozenInput", {})
                if (frozen.get("observationKind") == "render-consumed-value-snapshot"
                        and frozen.get("enabled") is True
                        and frozen.get("id") == stages[stage].get("id")):
                    # This frozen value already names the current instance.
                    # A fourth bad value cannot vanish behind three good frames.
                    frozen_params_consistent &= _params_match(frozen.get("params", {}))
                volume, draw = obs.get("volumetric", {}), obs.get("render", {})
                if (draw.get("id") == stages[stage].get("id")
                        and draw.get("commandsRecorded") is True
                        and volume.get("compositeSubmitted") is True
                        and all(_integer(draw.get(name)) > 0
                                and draw.get(name) == volume.get(name)
                                for name in ("mapEpoch", "deviceEpoch"))
                        and (_integer(volume.get("width")), _integer(volume.get("height"))) == (2560, 1440)):
                    # Each diagnostic's current backing is meaningful for its
                    # declared owner/key even when its last-draw frame lags
                    # frozenInput. Do not filter it with active_sample().
                    key = tuple(draw.get(name) for name in ("mapEpoch", "deviceEpoch")) + (2560, 1440)
                    amount = (_resource_bytes(draw.get("resourceBytes")),
                              _resource_bytes(volume.get("resourceBytes")))
                    stable &= all(value is not None and value > 0 for value in amount)
                    backing.setdefault(key, set()).add(amount)
        time_ok &= len(histories[stage]) == 1
    for stage in [1] + list(range(4, 68, 2)):
        prior = stage - 1
        if histories.get(stage) and histories.get(prior):
            time_ok &= min(histories[stage]) > max(histories[prior])
    stable &= bool(backing) and all(len(amounts) == 1 for amounts in backing.values())
    # Gate invalid current-stage observations too; admission must not hide a
    # bad Query, time regression or resource/preparation failure behind >3 good frames.
    for stage, samples in grouped.items():
        for sample in samples:
            obs = sample.get("observation", {})
            if phase_matches(obs, stage, stages) and expected_stage(stage)["weatherEnabled"]:
                failure_stages_ok &= obs.get("render", {}).get("stage") == "commands-recorded"
                failure_stages_ok &= obs.get("volumetric", {}).get("stage") == "composite-submitted"

    captures = receipt.get("captures", [])
    capture_complete = (len(captures) == len(CAPTURES)
        and {row.get("stage") for row in captures} == set(CAPTURES)
        and len({row.get("stage") for row in captures}) == len(captures))
    for row in captures:
        stage = row.get("stage")
        capture_complete &= (stage in CAPTURES and row.get("label") == CAPTURES.get(stage)
            and row.get("ok") is True and row.get("dimensions") == [2560, 1440]
            and row.get("stageAtRequest") == stage and row.get("stageAtCompletion") == stage
            and capture_ready(row.get("beforeCaptureObservation", {}), stage, stages)
            and capture_ready(row.get("afterCaptureObservation", {}), stage, stages))
    return {"allStressJassStagesAndChecks": _rows_ok(rows), "allStagesHaveActualConsumption": coverage,
            "all32OffDrainsObserved": drain, "all32EnabledRecoveriesObserved": all(
                consumed_counts[stage] >= 3 for stage in range(4, 68, 2)),
            "sunOrdinaryWeatherCoexistence": all(consumed_counts[stage] >= 3 for stage in (0, 1, 2, 3)),
            "newInstanceParamsSurviveOldIdRefusals": _rows_ok(rows) and owner_consistent and frozen_params_consistent and all(
                consumed_counts[stage] >= 3 for stage in range(4, 68, 2) if expected_stage(stage)["rebuilt"]),
            "weatherClocksAndHistoriesConsistent": owner_consistent and time_ok and bool(histories),
            "sameKeyCurrentWeatherBackingStable": stable,
            "noUnexpectedWeatherPreparationFailures": failure_stages_ok and bool(raw_samples),
            "allSevenCapturesPhaseBracketed": capture_complete,
            "ordinaryCurrentResourceAccountingCertified": False,
            "gpuCompletionCertified": False, "pixelOrArtisticAcceptance": False,
            "processLeakCertified": False, "gpuResourceRecreationCertified": False}
