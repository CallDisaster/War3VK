#!/usr/bin/env python3
"""Strict reader for cold D3D9 migration-census log records.

This describes observed public API attempts and retained local-build FF keys.
It cannot certify game-source attribution, successful draws, GPU completion,
full-scene capture, portable key ABI, performance, or game-memory savings.
Malformed records are never skipped to promote an earlier record as current.
Device ordinals are process-local: do not concatenate unrelated process logs.
v2 refusal observations are independent atomic reads taken after the coherent
snapshot copy. They are neither snapshot-consistent nor a complete call total.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys
from typing import Iterable


MARKER = "WARVK_D3D9_MIGRATION_CENSUS "
SCHEMA = "warvk-d3d9-migration-census/v1"
SCHEMA_V1 = SCHEMA  # Preserve the original fixture/API constant for old evidence.
SCHEMA_V2 = "warvk-d3d9-migration-census/v2"
SUPPORTED_SCHEMAS = (SCHEMA_V1, SCHEMA_V2)
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
CAPACITY = 32
MAX_RECORD_CHARS = 128 * 1024
MAX_RECORDS = 1024
API_NAMES = (
    "Reset", "Present", "Clear", "BeginScene", "EndScene",
    "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture",
    "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateOffscreenPlainSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "StretchRect",
    "SetRenderTarget", "SetDepthStencilSurface", "SetTransform", "SetViewport",
    "SetMaterial", "SetLight", "LightEnable", "SetRenderState", "SetTexture",
    "SetTextureStageState", "SetSamplerState", "SetScissorRect", "CreateStateBlock",
    "BeginStateBlock", "EndStateBlock", "CreateVertexDeclaration",
    "SetVertexDeclaration", "SetFVF", "CreateVertexShader", "CreatePixelShader",
    "SetVertexShader", "SetPixelShader", "SetVertexShaderConstantF",
    "SetPixelShaderConstantF", "SetStreamSource", "SetIndices", "DrawPrimitive",
    "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP",
    "ProcessVertices",
)
DRAW_APIS = ("DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP")
TEXTURE_APIS = ("CreateTexture", "CreateCubeTexture", "CreateVolumeTexture")
COUNTER_FIELDS = (
    "observedCalls", "unclassifiedCalls", "drawAttempts", "unknownSourceDrawAttempts",
    "primitiveTypeMissing", "primitiveTypeInvalid", "shaderPairMissing",
    "shaderPairInvalid", "invalidDrawKinds", "invalidSourceAttributions",
    "drawUnknownMaskBits", "drawSamplesDropped", "observedTextureRequests",
    "invalidTextureKinds", "textureUnknownMaskBits", "fullMipChainRequests",
    "textureSamplesDropped", "saturatedUpdates",
)
MAXIMUM_FIELDS = (
    "maxPrimitiveCount", "maxStreamStride", "maxVertexCount", "fvfBitUnion",
    "textureStageMaskUnion", "maxTextureWidth", "maxTextureHeight",
    "maxTextureDepth", "maxRequestedMipLevels",
)
HISTOGRAM_LENGTHS = {
    "drawKinds": 5, "primitiveTypes": 7, "shaderPairs": 4,
    "drawMissingFields": 8, "textureKinds": 4, "textureMissingFields": 7,
}
BOOL_FIELDS = (
    "diagnosticForcedDeviceLock", "includesInternalCalls", "gpuCompletionProven",
    "fullSceneCapture", "portableKeyAbi", "invariantPosition",
    "forceSampleRateShading", "ffUbershaderVS", "ffUbershaderFS",
)
IDENTITY_FIELDS = (
    "schema", "processBits", "behaviorFlags", "sourceAttribution",
    "textureStageMaskScope", "shaderPairMeaning", *BOOL_FIELDS,
)
ROOT_FIELDS = {
    "schema", "deviceOrdinal", "snapshotSequence", "reason", "presentAttempts",
    "processBits", "behaviorFlags", "sourceAttribution", "textureStageMaskScope",
    "shaderPairMeaning", "apiCalls", "drawSamples", "textureSamples", "vsKeys", "fsKeys",
    *BOOL_FIELDS, *COUNTER_FIELDS, *MAXIMUM_FIELDS, *HISTOGRAM_LENGTHS,
}
V2_POLICY_FIELDS = ("observationPolicy", "synchronizationPolicy")
V2_ROOT_FIELDS = ROOT_FIELDS | {*V2_POLICY_FIELDS, "observationRefusals"}
REFUSAL_FIELDS = {"recordBusy", "snapshotBusy", "counterSaturated"}
V2_DRAW_MASKS = {1: 0x03, 2: 0x43, 3: 0x13, 4: 0xD3}
DRAW_FACT_FIELDS_BY_BIT = (
    "primitiveType", "primitiveCount", "shaderMask", "fvf", "streamStride",
    "textureStageMask", "vertexCount", "indexFormat",
)
DRAW_SAMPLE_FIELDS = {
    "kind", "knownMask", "primitiveType", "primitiveCount", "shaderMask", "fvf",
    "streamStride", "textureStageMask", "vertexCount", "indexFormat",
}
TEXTURE_SAMPLE_FIELDS = {
    "kind", "knownMask", "width", "height", "depth", "levels", "format", "usage", "pool",
}
KEY_FIELDS = {"storedUnique", "observed", "duplicates", "droppedObservations", "missing", "keys"}


class EvidenceError(ValueError):
    """The supplied record cannot support the strict observation contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def uint(value: object, label: str, maximum: int = UINT64_MAX, minimum: int = 0) -> int:
    require(type(value) is int and minimum <= value <= maximum,
            f"{label}: expected integer in [{minimum}, {maximum}], not bool/string/float")
    return value


def exact_fields(value: object, fields: set[str], label: str) -> dict:
    require(type(value) is dict, f"{label}: expected object")
    require(all(type(key) is str for key in value), f"{label}: expected string field names")
    missing, extra = fields - value.keys(), value.keys() - fields
    require(not missing and not extra,
            f"{label}: fields differ; missing={sorted(missing)}, extra={sorted(extra)}")
    return value


def uint_array(value: object, length: int, label: str) -> list[int]:
    require(type(value) is list and len(value) == length, f"{label}: expected array length {length}")
    for index, item in enumerate(value):
        uint(item, f"{label}[{index}]")
    return value


def validate_keys(value: object, words: int, label: str) -> None:
    keys = exact_fields(value, KEY_FIELDS, label)
    for name in KEY_FIELDS - {"keys"}:
        uint(keys[name], f"{label}.{name}")
    count = keys["storedUnique"]
    require(count <= CAPACITY, f"{label}: storedUnique exceeds {CAPACITY}")
    require(type(keys["keys"]) is list and len(keys["keys"]) == count,
            f"{label}: storedUnique/key array length differ")
    for index, key in enumerate(keys["keys"]):
        require(type(key) is list and len(key) == words, f"{label}.keys[{index}]: expected {words} words")
        for word_index, word in enumerate(key):
            uint(word, f"{label}.keys[{index}][{word_index}]", UINT32_MAX)
    require(len({tuple(key) for key in keys["keys"]}) == count, f"{label}: duplicate retained keys")
    require(keys["observed"] == count + keys["duplicates"] + keys["droppedObservations"] + keys["missing"],
            f"{label}: observed != stored + duplicates + droppedObservations + missing")
    require(not keys["droppedObservations"] or count == CAPACITY,
            f"{label}: dropped observations before capacity")


def validate_samples(snapshot: dict, name: str, fields: set[str], total_name: str,
                     dropped_name: str, known_bits: int, kind_count: int) -> None:
    samples = snapshot[name]
    require(type(samples) is list and len(samples) <= CAPACITY, f"{name}: invalid fixed capacity")
    require(len(samples) == min(snapshot[total_name], CAPACITY), f"{name}: first-sample count inconsistent")
    require(snapshot[dropped_name] == snapshot[total_name] - len(samples), f"{dropped_name}: count inconsistent")
    for index, sample in enumerate(samples):
        exact_fields(sample, fields, f"{name}[{index}]")
        for field in fields:
            uint(sample[field], f"{name}[{index}].{field}", UINT32_MAX)
        require(1 <= sample["kind"] < kind_count, f"{name}[{index}]: impossible production kind")
        require(sample["knownMask"] & ~known_bits == 0, f"{name}[{index}]: unrecognized knownMask bits")


def validate_snapshot(snapshot: object) -> dict:
    require(type(snapshot) is dict, "snapshot: expected object")
    schema = snapshot.get("schema")
    require(schema in SUPPORTED_SCHEMAS, "unsupported census schema")
    s = exact_fields(snapshot, V2_ROOT_FIELDS if schema == SCHEMA_V2 else ROOT_FIELDS, "snapshot")
    uint(s["deviceOrdinal"], "deviceOrdinal", minimum=1)
    uint(s["snapshotSequence"], "snapshotSequence", minimum=1)
    uint(s["presentAttempts"], "presentAttempts")
    uint(s["processBits"], "processBits")
    require(s["processBits"] in (32, 64), "processBits: expected 32 or 64")
    uint(s["behaviorFlags"], "behaviorFlags", UINT32_MAX)
    require(s["reason"] in ("present-milestone", "normal-device-destruction"), "unknown snapshot reason")
    for field in BOOL_FIELDS:
        require(type(s[field]) is bool, f"{field}: expected JSON boolean")
    if schema == SCHEMA_V1:
        require(s["diagnosticForcedDeviceLock"] is True, "v1 diagnostic existing-device-lock contract absent")
    else:
        require(s["diagnosticForcedDeviceLock"] is False, "v2 must not force the device lock")
        require(s["observationPolicy"] == "argument-and-local-key-only", "unknown v2 observation policy")
        require(s["synchronizationPolicy"] == "bounded-try-cell", "unknown v2 synchronization policy")
        observation_refusals = exact_fields(s["observationRefusals"], REFUSAL_FIELDS, "observationRefusals")
        for field in ("recordBusy", "snapshotBusy"):
            uint(observation_refusals[field], f"observationRefusals.{field}", UINT32_MAX)
        uint(observation_refusals["counterSaturated"], "observationRefusals.counterSaturated", 1)
    require(s["sourceAttribution"] == "Unknown", "caller origin must remain Unknown")
    require(s["includesInternalCalls"] is True, "internal calls cannot be silently excluded")
    for field in ("gpuCompletionProven", "fullSceneCapture", "portableKeyAbi"):
        require(s[field] is False, f"{field}: unsupported certification claim")
    require(s["textureStageMaskScope"] == "pixel-fixed-function-0-through-7", "unknown texture-stage scope")
    require(s["shaderPairMeaning"] == "bound-handles-not-effective-shader", "unknown shader-pair meaning")
    for field in COUNTER_FIELDS:
        uint(s[field], field)
    for field in MAXIMUM_FIELDS:
        uint(s[field], field, UINT32_MAX)
    require(s["saturatedUpdates"] == 0, "counter saturation prevents arithmetic certification")
    api = exact_fields(s["apiCalls"], set(API_NAMES), "apiCalls")
    for name, count in api.items():
        uint(count, f"apiCalls.{name}")
    require(s["unclassifiedCalls"] == 0, "unclassified API calls prevent exact enumerated coverage")
    require(sum(api.values()) == s["observedCalls"], "sum(apiCalls) != observedCalls")
    require(api["Present"] == s["presentAttempts"], "Present calls != presentAttempts")
    for name, length in HISTOGRAM_LENGTHS.items():
        uint_array(s[name], length, name)
    draws = s["drawAttempts"]
    require(sum(api[name] for name in DRAW_APIS) == draws, "four public Draw calls != drawAttempts")
    require(sum(s["drawKinds"]) + s["invalidDrawKinds"] == draws, "draw-kind sum != drawAttempts")
    require(s["drawKinds"][1:] == [api[name] for name in DRAW_APIS], "draw kind/API correspondence differs")
    require(sum(s["shaderPairs"]) + s["shaderPairMissing"] + s["shaderPairInvalid"] == draws,
            "shader-pair/missing/invalid sum != drawAttempts")
    require(sum(s["primitiveTypes"][1:]) + s["primitiveTypeMissing"] + s["primitiveTypeInvalid"] == draws,
            "primitive/missing/invalid sum != drawAttempts")
    require(s["primitiveTypes"][0] <= s["primitiveTypeInvalid"], "known-zero primitive must also be invalid")
    require(s["drawMissingFields"][0] == s["primitiveTypeMissing"], "primitive missing mirrors differ")
    require(s["drawMissingFields"][2] == s["shaderPairMissing"], "shader missing mirrors differ")
    require(all(value <= draws for value in s["drawMissingFields"]), "draw missing field exceeds attempts")
    require(s["unknownSourceDrawAttempts"] == draws, "unknown source denominator differs")
    require(s["invalidSourceAttributions"] == 0, "unsupported source attribution present")
    require(s["drawUnknownMaskBits"] == 0, "unsupported draw knownMask bits present")
    require(s["textureStageMaskUnion"] <= 255, "texture-stage union outside declared 0..7 scope")
    textures = s["observedTextureRequests"]
    require(sum(api[name] for name in TEXTURE_APIS) == textures, "three texture API calls != texture requests")
    require(sum(s["textureKinds"]) + s["invalidTextureKinds"] == textures, "texture-kind sum != requests")
    require(s["textureKinds"][1:] == [api[name] for name in TEXTURE_APIS], "texture kind/API correspondence differs")
    require(all(value <= textures for value in s["textureMissingFields"]), "texture missing field exceeds requests")
    require(s["textureUnknownMaskBits"] == 0, "unsupported texture knownMask bits present")
    require(s["fullMipChainRequests"] <= textures - s["textureMissingFields"][3], "full mip chain count inconsistent")
    validate_samples(s, "drawSamples", DRAW_SAMPLE_FIELDS, "drawAttempts", "drawSamplesDropped", 255, 5)
    validate_samples(s, "textureSamples", TEXTURE_SAMPLE_FIELDS, "observedTextureRequests", "textureSamplesDropped", 127, 4)
    if schema == SCHEMA_V2:
        validate_v2_draw_contract(s)
    validate_retained_sample_aggregates(s)
    validate_keys(s["vsKeys"], 5, "vsKeys")
    validate_keys(s["fsKeys"], 16, "fsKeys")
    return s


def validate_v2_draw_contract(s: dict) -> None:
    """Validate the full argument-only contract, including facts beyond the prefix."""
    draws = s["drawAttempts"]
    kinds = s["drawKinds"]
    require(s["shaderPairs"] == [0, 0, 0, 0] and s["shaderPairMissing"] == draws
            and s["shaderPairInvalid"] == 0, "v2 shader state must remain unobserved")
    require(s["fvfBitUnion"] == 0 and s["textureStageMaskUnion"] == 0,
            "v2 state-derived unions must remain unobserved")
    expected_missing = [0, 0, draws, draws, kinds[1] + kinds[2], draws,
                        kinds[1] + kinds[3], draws - kinds[4]]
    require(s["drawMissingFields"] == expected_missing,
            "v2 argument-only cumulative missing-field histogram differs")
    if kinds[3] + kinds[4] == 0:
        require(s["maxStreamStride"] == 0, "v2 stride maximum without explicit stride arguments")
    if kinds[2] + kinds[4] == 0:
        require(s["maxVertexCount"] == 0, "v2 vertex maximum without explicit vertex arguments")
    for index, sample in enumerate(s["drawSamples"]):
        known = sample["knownMask"]
        require(known == V2_DRAW_MASKS[sample["kind"]], f"v2 drawSamples[{index}]: argument-only knownMask differs")
        for bit, field in enumerate(DRAW_FACT_FIELDS_BY_BIT):
            require(bool(known & (1 << bit)) or sample[field] == 0,
                    f"v2 drawSamples[{index}].{field}: unobserved value must be zero")


def validate_retained_sample_aggregates(s: dict) -> None:
    """Retained-prefix facts must not contradict cumulative histograms/maxima."""
    draw_kind = Counter()
    primitive = Counter()
    shader = Counter()
    draw_missing = [0] * 8
    invalid_primitive = invalid_shader = 0
    for sample in s["drawSamples"]:
        draw_kind[sample["kind"]] += 1
        known = sample["knownMask"]
        for bit in range(8):
            draw_missing[bit] += not bool(known & (1 << bit))
        if known & 1:
            value = sample["primitiveType"]
            if value <= 6:
                primitive[value] += 1
            invalid_primitive += not 1 <= value <= 6
        if known & 4:
            value = sample["shaderMask"]
            if value < 4:
                shader[value] += 1
            else:
                invalid_shader += 1
        for bit, fact, maximum in ((1, "primitiveCount", "maxPrimitiveCount"),
                                   (4, "streamStride", "maxStreamStride"),
                                   (6, "vertexCount", "maxVertexCount")):
            require(not known & (1 << bit) or sample[fact] <= s[maximum], f"retained {fact} exceeds {maximum}")
        for bit, fact, union in ((3, "fvf", "fvfBitUnion"), (5, "textureStageMask", "textureStageMaskUnion")):
            require(not known & (1 << bit) or sample[fact] & ~s[union] == 0, f"retained {fact} absent from {union}")
    for kind, count in draw_kind.items():
        require(count <= s["drawKinds"][kind], "retained draw-kind count exceeds aggregate")
    for kind, count in primitive.items():
        require(count <= s["primitiveTypes"][kind], "retained primitive count exceeds aggregate")
    for pair, count in shader.items():
        require(count <= s["shaderPairs"][pair], "retained shader count exceeds aggregate")
    require(invalid_primitive <= s["primitiveTypeInvalid"], "retained invalid primitive absent from aggregate")
    require(invalid_shader <= s["shaderPairInvalid"], "retained invalid shader absent from aggregate")
    require(all(a <= b for a, b in zip(draw_missing, s["drawMissingFields"])), "retained missing draw field absent from aggregate")
    if len(s["drawSamples"]) == s["drawAttempts"]:
        require([primitive[i] for i in range(7)] == s["primitiveTypes"], "fully retained primitive histogram differs")
        require([shader[i] for i in range(4)] == s["shaderPairs"], "fully retained shader histogram differs")
        require(draw_missing == s["drawMissingFields"], "fully retained draw missing fields differ")
        for bit, fact, maximum in ((1, "primitiveCount", "maxPrimitiveCount"),
                                   (4, "streamStride", "maxStreamStride"),
                                   (6, "vertexCount", "maxVertexCount")):
            expected = max((f[fact] for f in s["drawSamples"] if f["knownMask"] & (1 << bit)), default=0)
            require(expected == s[maximum], f"fully retained {maximum} differs")
        for bit, fact, union in ((3, "fvf", "fvfBitUnion"), (5, "textureStageMask", "textureStageMaskUnion")):
            expected = 0
            for sample in s["drawSamples"]:
                if sample["knownMask"] & (1 << bit):
                    expected |= sample[fact]
            require(expected == s[union], f"fully retained {union} differs")
    texture_kind = Counter()
    texture_missing = [0] * 7
    full_mip = 0
    for sample in s["textureSamples"]:
        texture_kind[sample["kind"]] += 1
        known = sample["knownMask"]
        for bit in range(7):
            texture_missing[bit] += not bool(known & (1 << bit))
        for bit, fact, maximum in ((0, "width", "maxTextureWidth"), (1, "height", "maxTextureHeight"),
                                   (2, "depth", "maxTextureDepth"), (3, "levels", "maxRequestedMipLevels")):
            require(not known & (1 << bit) or sample[fact] <= s[maximum], f"retained texture {fact} exceeds {maximum}")
        full_mip += bool(known & 8) and sample["levels"] == 0
    for kind, count in texture_kind.items():
        require(count <= s["textureKinds"][kind], "retained texture kind absent from aggregate")
    require(full_mip <= s["fullMipChainRequests"], "retained full mip chain absent from aggregate")
    require(all(a <= b for a, b in zip(texture_missing, s["textureMissingFields"])), "retained missing texture field absent from aggregate")
    if len(s["textureSamples"]) == s["observedTextureRequests"]:
        require(full_mip == s["fullMipChainRequests"], "fully retained full mip chain count differs")
        require(texture_missing == s["textureMissingFields"], "fully retained texture missing fields differ")
        for bit, fact, maximum in ((0, "width", "maxTextureWidth"), (1, "height", "maxTextureHeight"),
                                   (2, "depth", "maxTextureDepth"), (3, "levels", "maxRequestedMipLevels")):
            expected = max((f[fact] for f in s["textureSamples"] if f["knownMask"] & (1 << bit)), default=0)
            require(expected == s[maximum], f"fully retained {maximum} differs")


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> None:
    raise EvidenceError(f"non-JSON numeric constant: {value}")


def parse_lines(lines: Iterable[str]) -> list[dict]:
    records = []
    for line_number, line in enumerate(lines, 1):
        offset = line.find(MARKER)
        if offset < 0:
            require(MARKER.rstrip() not in line, f"line {line_number}: truncated or malformed census marker")
            continue
        require(line.find(MARKER, offset + len(MARKER)) < 0, f"line {line_number}: multiple census markers")
        raw = line[offset + len(MARKER):].strip()
        require(len(raw) <= MAX_RECORD_CHARS, f"line {line_number}: census record exceeds bound")
        require(len(records) < MAX_RECORDS, "too many census records")
        try:
            snapshot = json.loads(raw, object_pairs_hook=reject_duplicate_keys, parse_constant=reject_constant)
            validate_snapshot(snapshot)
        except (ValueError, TypeError, RecursionError) as error:
            raise EvidenceError(f"line {line_number}: {error}") from error
        records.append({"line": line_number, "snapshot": snapshot})
    require(bool(records), "no census records; no coverage evidence")
    validate_sequences(records)
    return records


def parse_log(text: str) -> list[dict]:
    return parse_lines(text.splitlines())


def validate_sequences(records: list[dict]) -> None:
    devices: dict[int, list[dict]] = {}
    for record in records:
        devices.setdefault(record["snapshot"]["deviceOrdinal"], []).append(record["snapshot"])
    for device, snapshots in devices.items():
        sequences = [s["snapshotSequence"] for s in snapshots]
        require(len(set(sequences)) == len(sequences), f"device {device}: duplicate snapshotSequence")
        snapshots.sort(key=lambda s: s["snapshotSequence"])
        for previous, current in zip(snapshots, snapshots[1:]):
            require(previous["reason"] != "normal-device-destruction", f"device {device}: snapshot after destruction")
            for field in IDENTITY_FIELDS:
                require(previous[field] == current[field], f"device {device}: identity/options changed: {field}")
            if current["schema"] == SCHEMA_V2:
                for field in V2_POLICY_FIELDS:
                    require(previous[field] == current[field], f"device {device}: identity/options changed: {field}")
            # observationRefusals is deliberately NOT a coherent sequence
            # counter: these independent atomics are read after releasing the
            # cell, so a lower snapshotSequence can observe a later refusal.
            for field in (*COUNTER_FIELDS, *MAXIMUM_FIELDS, "presentAttempts"):
                require(previous[field] <= current[field], f"device {device}: cumulative field regressed: {field}")
            for field in ("fvfBitUnion", "textureStageMaskUnion"):
                require(previous[field] & ~current[field] == 0, f"device {device}: union lost bits: {field}")
            for name in API_NAMES:
                require(previous["apiCalls"][name] <= current["apiCalls"][name], f"device {device}: API count regressed: {name}")
            for field in HISTOGRAM_LENGTHS:
                require(all(a <= b for a, b in zip(previous[field], current[field])), f"device {device}: histogram regressed: {field}")
            for field in ("drawSamples", "textureSamples"):
                require(current[field][:len(previous[field])] == previous[field], f"device {device}: retained first samples changed: {field}")
            for field in ("vsKeys", "fsKeys"):
                before, after = previous[field], current[field]
                for counter in KEY_FIELDS - {"keys"}:
                    require(before[counter] <= after[counter], f"device {device}: {field}.{counter} regressed")
                require(after["keys"][:len(before["keys"])] == before["keys"], f"device {device}: retained keys changed: {field}")


def select_snapshot(records: list[dict], device_ordinal: int | None = None) -> dict:
    devices = sorted({r["snapshot"]["deviceOrdinal"] for r in records})
    if device_ordinal is None:
        require(len(devices) == 1, f"multiple devices {devices}; explicitly select --device-ordinal")
        device_ordinal = devices[0]
    uint(device_ordinal, "selected deviceOrdinal", minimum=1)
    selected = [r for r in records if r["snapshot"]["deviceOrdinal"] == device_ordinal]
    require(bool(selected), f"device {device_ordinal}: not present in log")
    return max(selected, key=lambda r: r["snapshot"]["snapshotSequence"])


def report(records: list[dict], device_ordinal: int | None = None) -> dict:
    selected = select_snapshot(records, device_ordinal)
    s = selected["snapshot"]
    lines = [r for r in records if r["snapshot"]["deviceOrdinal"] == s["deviceOrdinal"]]
    arrival = [r["snapshot"]["snapshotSequence"] for r in lines]
    ordered = sorted(arrival)
    missing_sequences = ordered[-1] - ordered[0] + 1 - len(ordered)
    losses = {
        "drawSamplesDropped": s["drawSamplesDropped"], "textureSamplesDropped": s["textureSamplesDropped"],
        "vsKeyObservationsDropped": s["vsKeys"]["droppedObservations"],
        "fsKeyObservationsDropped": s["fsKeys"]["droppedObservations"],
        "vsKeyObservationsMissing": s["vsKeys"]["missing"], "fsKeyObservationsMissing": s["fsKeys"]["missing"],
    }
    refusals = ["source-attribution-unknown-includes-internal-calls", "attempts-not-GPU-execution",
                "no-full-scene-capture", "local-build-keys-not-portable-ABI", "enumerated-APIs-not-full-D3D9"]
    if not s["drawAttempts"]:
        refusals.append("no-draw-attempt-evidence")
    if not s["vsKeys"]["storedUnique"] and not s["fsKeys"]["storedUnique"]:
        refusals.append("no-fixed-function-key-samples")
    if any(losses.values()):
        refusals.append("retained-samples-incomplete")
    if missing_sequences or ordered[0] != 1:
        refusals.append("snapshot-log-sequence-incomplete")
    if s["schema"] == SCHEMA_V2:
        max_refusals = {
            field: max(record["snapshot"]["observationRefusals"][field] for record in lines)
            for field in sorted(REFUSAL_FIELDS)
        }
        refusal_evidence = {
            "available": True,
            "selected": dict(s["observationRefusals"]),
            "maxObserved": max_refusals,
            "coherentWithSnapshot": False,
            "completeCallTotal": False,
            "counterSaturatedIsStickyLatch": True,
            "provenance": "independent-atomic-reads-after-coherent-copy; not sequence-monotonic",
            "countingUnit": "recordBusy counts refused transactions, not public API calls; snapshotBusy counts refused copies",
        }
        if max_refusals["recordBusy"]:
            refusals.append("record-transactions-refused-busy")
        if max_refusals["snapshotBusy"]:
            refusals.append("snapshot-copies-refused-busy")
        if max_refusals["counterSaturated"]:
            refusals.append("observation-refusal-counters-saturated")
    else:
        refusal_evidence = {
            "available": False,
            "provenance": "v1-does-not-record-observation-refusals; absence-is-not-zero",
        }
    return {
        "schema": "warvk-d3d9-migration-census-analysis/v1",
        "selectedDeviceOrdinal": s["deviceOrdinal"], "selectedSnapshotSequence": s["snapshotSequence"],
        "selectedLogLine": selected["line"], "deviceOrdinals": sorted({r["snapshot"]["deviceOrdinal"] for r in records}),
        "validRecordCount": len(records), "selectedDeviceRecordCount": len(lines),
        "logWriteOrderDiffersFromSnapshotOrder": arrival != ordered,
        "missingInitialSnapshotCount": ordered[0] - 1,
        "missingIntermediateSnapshotCount": missing_sequences,
        "arithmeticValidated": True, "fullCoverageCertified": False,
        "gameSourceCertified": False, "gpuExecutionCertified": False,
        "fullSceneCapture": False, "portableKeyAbi": False,
        "scope": "Observed API attempts and retained local-build FF keys only; not game/GPU or memory-saving proof",
        "observedApiNames": [name for name in API_NAMES if s["apiCalls"][name]],
        "unobservedEnumeratedApiNames": [name for name in API_NAMES if not s["apiCalls"][name]],
        "sampleLosses": losses, "certificationRefusals": refusals,
        "observationRefusalEvidence": refusal_evidence,
        "fixtureAvailable": bool(s["vsKeys"]["keys"] or s["fsKeys"]["keys"]),
        "fixtureScope": "retained-local-build-key-samples-not-complete-workload",
        "selectedSnapshot": s,
    }


def analyze(text: str, device_ordinal: int | None = None) -> dict:
    return report(parse_log(text), device_ordinal)


def fixture_text(snapshot: dict) -> str:
    s = validate_snapshot(snapshot)
    vs, fs = s["vsKeys"]["keys"], s["fsKeys"]["keys"]
    require(bool(vs or fs), "no fixed-function keys; refusing an empty fixture")
    lines = ["WARVK_FF_KEYS_V1", f"invariantPosition {int(s['invariantPosition'])}",
             f"forceSampleRateShading {int(s['forceSampleRateShading'])}",
             f"vsCount {len(vs)}", f"fsCount {len(fs)}"]
    lines += ["VS " + " ".join(str(word) for word in key) for key in vs]
    lines += ["FS " + " ".join(str(word) for word in key) for key in fs]
    return "\n".join([*lines, "END", ""])


def write_fixture(snapshot: dict, output: Path) -> None:
    text = fixture_text(snapshot)
    # Exclusive creation, including a race against another writer. No overwrite.
    with output.open("x", encoding="ascii", newline="\n") as stream:
        stream.write(text)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="Raw DXVK log; optional logger prefix is accepted")
    parser.add_argument("--device-ordinal", type=int, help="Required when more than one device is present")
    parser.add_argument("--fixture-out", type=Path, help="Create a local-build FF key fixture; never overwrite")
    args = parser.parse_args(argv)
    try:
        with args.log.open("r", encoding="utf-8-sig", errors="strict") as stream:
            result = report(parse_lines(stream), args.device_ordinal)
        if args.fixture_out is not None:
            write_fixture(result["selectedSnapshot"], args.fixture_out)
            result["fixturePath"] = str(args.fixture_out.resolve())
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except (OSError, ValueError, UnicodeError) as error:
        print(f"CENSUS_REJECTED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
