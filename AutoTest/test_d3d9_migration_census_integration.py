#!/usr/bin/env python3
"""Parser adversaries and production-wiring static checks, not runtime proof.

No native build/execution, game, GPU, deployment, or foreground input is used.
Synthetic records exercise the strict reader; source tests check the actual
device integration instead of pretending the records came from a game run.
"""

from __future__ import annotations

import contextlib
from collections import Counter
import copy
import io
import json
from pathlib import Path
import re
import tempfile
import unittest

import analyze_d3d9_migration_census as a


ROOT = Path(__file__).resolve().parents[1]


def sample_snapshot(*, draws: int = 1, textures: int = 1, presents: int = 1,
                    sequence: int = 1, device: int = 1) -> dict:
    s = {field: 0 for field in (*a.COUNTER_FIELDS, *a.MAXIMUM_FIELDS)}
    s.update({field: [0] * length for field, length in a.HISTOGRAM_LENGTHS.items()})
    s.update({
        "schema": a.SCHEMA, "deviceOrdinal": device, "snapshotSequence": sequence,
        "reason": "present-milestone", "presentAttempts": presents, "processBits": 32,
        "behaviorFlags": 64, "diagnosticForcedDeviceLock": True,
        "sourceAttribution": "Unknown", "includesInternalCalls": True,
        "gpuCompletionProven": False, "fullSceneCapture": False, "portableKeyAbi": False,
        "textureStageMaskScope": "pixel-fixed-function-0-through-7",
        "shaderPairMeaning": "bound-handles-not-effective-shader",
        "invariantPosition": False, "forceSampleRateShading": True,
        "ffUbershaderVS": True, "ffUbershaderFS": True,
        "apiCalls": {name: 0 for name in a.API_NAMES}, "drawSamples": [], "textureSamples": [],
    })
    s["apiCalls"].update(Present=presents, DrawIndexedPrimitive=draws, CreateTexture=textures)
    s["observedCalls"] = sum(s["apiCalls"].values())
    s["drawAttempts"] = draws
    s["drawKinds"][2] = draws
    s["primitiveTypes"][4] = draws
    s["shaderPairs"][0] = draws
    s["unknownSourceDrawAttempts"] = draws
    if draws:
        s.update(maxPrimitiveCount=1, maxStreamStride=40, maxVertexCount=3,
                 fvfBitUnion=274, textureStageMaskUnion=1)
    draw = {"kind": 2, "knownMask": 255, "primitiveType": 4, "primitiveCount": 1,
            "shaderMask": 0, "fvf": 274, "streamStride": 40, "textureStageMask": 1,
            "vertexCount": 3, "indexFormat": 101}
    s["drawSamples"] = [dict(draw) for _ in range(min(draws, 32))]
    s["drawSamplesDropped"] = max(0, draws - 32)
    s["observedTextureRequests"] = textures
    s["textureKinds"][1] = textures
    s["fullMipChainRequests"] = textures
    if textures:
        s.update(maxTextureWidth=1024, maxTextureHeight=512, maxTextureDepth=1)
    texture = {"kind": 1, "knownMask": 127, "width": 1024, "height": 512, "depth": 1,
               "levels": 0, "format": 21, "usage": 0, "pool": 1}
    s["textureSamples"] = [dict(texture) for _ in range(min(textures, 32))]
    s["textureSamplesDropped"] = max(0, textures - 32)
    for name, words in (("vsKeys", 5), ("fsKeys", 16)):
        keys = [[0] * words] if draws else []
        s[name] = {"storedUnique": len(keys), "observed": len(keys), "duplicates": 0,
                   "droppedObservations": 0, "missing": 0, "keys": keys}
    return s


def sample_snapshot_v2(*, kinds: tuple[int, int, int, int] = (0, 1, 0, 0),
                       textures: int = 1, presents: int = 1, sequence: int = 1,
                       device: int = 1) -> dict:
    """Arguments-only synthetic observations; never represented as native proof."""
    draws = sum(kinds)
    s = sample_snapshot(draws=draws, textures=textures, presents=presents,
                        sequence=sequence, device=device)
    s.update(schema=a.SCHEMA_V2, diagnosticForcedDeviceLock=False,
             observationPolicy="argument-and-local-key-only",
             synchronizationPolicy="bounded-try-cell",
             observationRefusals={"recordBusy": 0, "snapshotBusy": 0, "counterSaturated": 0},
             shaderPairs=[0, 0, 0, 0], shaderPairMissing=draws,
             fvfBitUnion=0, textureStageMaskUnion=0,
             maxStreamStride=40 if kinds[2] + kinds[3] else 0,
             maxVertexCount=3 if kinds[1] + kinds[3] else 0,
             drawKinds=[0, *kinds], drawSamples=[])
    s["drawMissingFields"] = [0, 0, draws, draws, kinds[0] + kinds[1], draws,
                              kinds[0] + kinds[2], draws - kinds[3]]
    for index, (api, count) in enumerate(zip(a.DRAW_APIS, kinds), 1):
        s["apiCalls"][api] = count
        for _ in range(count):
            if len(s["drawSamples"]) == a.CAPACITY:
                break
            s["drawSamples"].append({
                "kind": index, "knownMask": a.V2_DRAW_MASKS[index],
                "primitiveType": 4, "primitiveCount": 1, "shaderMask": 0,
                "fvf": 0, "streamStride": 40 if index in (3, 4) else 0,
                "textureStageMask": 0, "vertexCount": 3 if index in (2, 4) else 0,
                "indexFormat": 101 if index == 4 else 0,
            })
    s["observedCalls"] = sum(s["apiCalls"].values())
    return s


def line(snapshot: dict, prefix: str = "info: ") -> str:
    return prefix + a.MARKER + json.dumps(snapshot, separators=(",", ":")) + "\n"


def code_mask(source: str) -> str:
    # Keep indices stable while masking comments and ordinary C++ literals.
    pattern = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27'
    return re.sub(pattern, lambda match: " " * len(match.group()), source, flags=re.S)


def function_body(source: str, name: str) -> str:
    marker = f"D3D9DeviceEx::{name}("
    start = source.index(marker)
    masked = code_mask(source[start:])
    begin = masked.index("{")
    depth = 0
    for index in range(begin, len(masked)):
        depth += masked[index] == "{"
        depth -= masked[index] == "}"
        if depth == 0:
            return source[start + begin:start + index + 1]
    raise AssertionError(f"unclosed function: {name}")


def nesting_depth(body: str, offset: int) -> int:
    masked = code_mask(body[:offset])
    return masked.count("{") - masked.count("}")


class StrictParser(unittest.TestCase):
    def reject(self, snapshot: dict, pattern: str = ".") -> None:
        with self.assertRaisesRegex(a.EvidenceError, pattern):
            a.parse_log(line(snapshot))

    def test_valid_prefix_and_zero_known_values(self):
        s = sample_snapshot()
        result = a.analyze("ordinary logger message\n" + line(s, "[12:00] info: "))
        self.assertTrue(result["arithmeticValidated"])
        self.assertFalse(result["fullCoverageCertified"])
        self.assertFalse(result["gameSourceCertified"])
        self.assertFalse(result["gpuExecutionCertified"])
        self.assertFalse(result["portableKeyAbi"])
        self.assertEqual(result["selectedSnapshot"]["shaderPairs"], [1, 0, 0, 0])
        self.assertEqual(result["selectedSnapshot"]["fullMipChainRequests"], 1)
        self.assertIn("no-full-scene-capture", result["certificationRefusals"])

    def test_empty_log_and_zero_observations_are_not_coverage(self):
        with self.assertRaisesRegex(a.EvidenceError, "no census records"):
            a.analyze("ordinary log only\n")
        zero = sample_snapshot(draws=0, textures=0, presents=0)
        result = a.analyze(line(zero))
        self.assertFalse(result["fullCoverageCertified"])
        self.assertFalse(result["fixtureAvailable"])
        self.assertIn("no-draw-attempt-evidence", result["certificationRefusals"])
        self.assertIn("no-fixed-function-key-samples", result["certificationRefusals"])
        with self.assertRaisesRegex(a.EvidenceError, "empty fixture"):
            a.fixture_text(zero)

    def test_exact_fields_and_schema(self):
        for target, mutation in (("root missing", lambda s: s.pop("sourceAttribution")),
                                 ("root extra", lambda s: s.update(unsupported=0)),
                                 ("api missing", lambda s: s["apiCalls"].pop("SetLight")),
                                 ("api extra", lambda s: s["apiCalls"].update(UnsupportedApi=0)),
                                 ("schema", lambda s: s.update(schema="v2"))):
            with self.subTest(target=target):
                s = sample_snapshot()
                mutation(s)
                self.reject(s)

    def test_integer_ranges_and_bool_rejection(self):
        for value in (True, False, "1", 1.0, -1, a.UINT64_MAX + 1, None):
            with self.subTest(value=value):
                s = sample_snapshot()
                s["observedCalls"] = value
                self.reject(s, "expected integer")
        for path in ("behaviorFlags", "maxTextureWidth"):
            s = sample_snapshot()
            s[path] = a.UINT32_MAX + 1
            self.reject(s, "expected integer")

    def test_booleans_and_proof_boundaries(self):
        for name in a.BOOL_FIELDS:
            with self.subTest(name=name):
                s = sample_snapshot()
                s[name] = int(s[name])
                self.reject(s, "JSON boolean")
        for name, value in (("sourceAttribution", "Game"), ("includesInternalCalls", False),
                            ("gpuCompletionProven", True), ("fullSceneCapture", True),
                            ("portableKeyAbi", True), ("diagnosticForcedDeviceLock", False),
                            ("shaderPairMeaning", "effective-programmable-shader")):
            with self.subTest(name=name):
                s = sample_snapshot()
                s[name] = value
                self.reject(s)

    def test_json_duplicate_trailing_truncated_and_nonfinite_rejected(self):
        valid = line(sample_snapshot())
        corrupt = (
            valid.replace('"deviceOrdinal":1', '"deviceOrdinal":1,"deviceOrdinal":1'),
            valid.replace('"SetLight":0', '"SetLight":0,"SetLight":0'),
            valid.rstrip() + " trailing\n", valid[:len(valid) // 2],
            valid.replace('"observedCalls":3', '"observedCalls":NaN'),
            a.MARKER + a.MARKER + "{}\n",
            a.MARKER.rstrip() + "\n",
        )
        for raw in corrupt:
            with self.subTest(raw=raw[:100]):
                with self.assertRaises(a.EvidenceError):
                    a.parse_log(raw)

    def test_invalid_newest_record_never_falls_back_to_old(self):
        with self.assertRaises(a.EvidenceError):
            a.analyze(line(sample_snapshot()) + a.MARKER + "{broken\n")

    def test_api_draw_shader_primitive_texture_equalities(self):
        mutations = (
            lambda s: s.update(observedCalls=4),
            lambda s: s.update(unclassifiedCalls=1),
            lambda s: s.update(presentAttempts=2),
            lambda s: s.update(drawAttempts=2),
            lambda s: s["drawKinds"].__setitem__(2, 0),
            lambda s: s["shaderPairs"].__setitem__(0, 0),
            lambda s: s["primitiveTypes"].__setitem__(4, 0),
            lambda s: s.update(unknownSourceDrawAttempts=0),
            lambda s: s.update(observedTextureRequests=2),
            lambda s: s["textureKinds"].__setitem__(1, 0),
            lambda s: s.update(fullMipChainRequests=2),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                s = sample_snapshot()
                mutate(s)
                self.reject(s)

    def test_missing_and_invalid_fields_remain_distinct(self):
        s = sample_snapshot()
        s["drawSamples"][0]["knownMask"] &= ~4
        s["drawSamples"][0]["shaderMask"] = 0
        s["shaderPairs"][0] = 0
        s["shaderPairMissing"] = 1
        s["drawMissingFields"][2] = 1
        self.assertEqual(a.validate_snapshot(s)["shaderPairMissing"], 1)
        s["shaderPairMissing"] = 0
        s["shaderPairInvalid"] = 1
        self.reject(s, "missing mirrors")

    def test_sample_capacity_loss_counts_and_maxima(self):
        s = sample_snapshot(draws=100, textures=80)
        result = a.analyze(line(s))
        self.assertEqual(result["sampleLosses"]["drawSamplesDropped"], 68)
        self.assertEqual(result["sampleLosses"]["textureSamplesDropped"], 48)
        self.assertFalse(result["fullCoverageCertified"])
        self.assertIn("retained-samples-incomplete", result["certificationRefusals"])
        for mutation in (lambda x: x["drawSamples"].append(dict(x["drawSamples"][0])),
                         lambda x: x.update(drawSamplesDropped=67),
                         lambda x: x.update(maxStreamStride=39),
                         lambda x: x.update(maxTextureWidth=1023),
                         lambda x: x["textureSamples"][0].update(width=True)):
            bad = copy.deepcopy(s)
            mutation(bad)
            self.reject(bad)

    def test_key_shape_unique_words_and_accounting(self):
        mutations = (
            lambda s: s["vsKeys"].update(storedUnique=2),
            lambda s: s["vsKeys"]["keys"][0].append(0),
            lambda s: s["fsKeys"]["keys"][0].pop(),
            lambda s: s["fsKeys"]["keys"][0].__setitem__(15, a.UINT32_MAX + 1),
            lambda s: s["vsKeys"]["keys"][0].__setitem__(4, True),
            lambda s: s["vsKeys"].update(observed=2),
            lambda s: s["vsKeys"].update(droppedObservations=1, observed=2),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                s = sample_snapshot()
                mutate(s)
                self.reject(s)
        duplicate = sample_snapshot()
        duplicate["vsKeys"].update(keys=[[0] * 5, [0] * 5], storedUnique=2, observed=2)
        self.reject(duplicate, "duplicate retained")

    def test_fully_retained_facts_cannot_invent_high_maxima_or_zero_primitives(self):
        for field in ("maxPrimitiveCount", "maxStreamStride", "maxVertexCount", "maxTextureWidth", "maxTextureDepth", "maxRequestedMipLevels"):
            with self.subTest(field=field):
                s = sample_snapshot()
                s[field] += 1
                self.reject(s, "fully retained")
        s = sample_snapshot()
        s["drawSamples"][0]["primitiveType"] = 99
        s["primitiveTypes"][4] = 0
        s["primitiveTypeInvalid"] = 1
        s["primitiveTypes"][0] = 1  # Invented known-zero subtype of invalid.
        self.reject(s, "fully retained primitive")

    def test_key_overflow_keeps_retained_fixture_but_never_full_coverage(self):
        s = sample_snapshot(draws=100)
        for name, words in (("vsKeys", 5), ("fsKeys", 16)):
            s[name].update(keys=[[0] * (words - 1) + [i] for i in range(32)],
                           storedUnique=32, observed=35, duplicates=1, missing=1, droppedObservations=1)
        result = a.analyze(line(s))
        self.assertFalse(result["fullCoverageCertified"])
        self.assertTrue(result["fixtureAvailable"])
        self.assertIn("vsCount 32\nfsCount 32\n", a.fixture_text(s))
        self.assertIn("retained-samples-incomplete", result["certificationRefusals"])

    def test_saturation_refuses_arithmetic_certification(self):
        s = sample_snapshot()
        s["saturatedUpdates"] = 1
        self.reject(s, "saturation prevents arithmetic certification")
        with self.assertRaises(a.EvidenceError):
            a.fixture_text(s)

    def test_multiple_devices_need_selection(self):
        text = line(sample_snapshot(device=2)) + line(sample_snapshot(device=1))
        with self.assertRaisesRegex(a.EvidenceError, "multiple devices"):
            a.analyze(text)
        result = a.analyze(text, 2)
        self.assertEqual(result["selectedDeviceOrdinal"], 2)
        self.assertEqual(result["deviceOrdinals"], [1, 2])
        with self.assertRaisesRegex(a.EvidenceError, "not present"):
            a.analyze(text, 3)

    def test_sequence_write_reordering_preserves_every_record(self):
        first = sample_snapshot(sequence=1)
        second = sample_snapshot(sequence=2, draws=2, textures=2, presents=64)
        latest = sample_snapshot(sequence=3, draws=3, textures=3, presents=64)
        latest["reason"] = "normal-device-destruction"
        result = a.analyze(line(latest) + line(first) + line(second))
        self.assertEqual(result["selectedSnapshotSequence"], 3)
        self.assertEqual(result["selectedLogLine"], 1)
        self.assertEqual(result["validRecordCount"], 3)
        self.assertTrue(result["logWriteOrderDiffersFromSnapshotOrder"])
        self.assertEqual(result["missingIntermediateSnapshotCount"], 0)

    def test_duplicate_sequence_rejected_even_if_identical(self):
        s = sample_snapshot()
        with self.assertRaisesRegex(a.EvidenceError, "duplicate snapshotSequence"):
            a.analyze(line(s) + line(s))

    def test_sequence_gap_visible_without_fabricating_records(self):
        first = sample_snapshot(sequence=1)
        last = sample_snapshot(sequence=4, presents=64)
        result = a.analyze(line(first) + line(last))
        self.assertEqual(result["missingIntermediateSnapshotCount"], 2)
        self.assertIn("snapshot-log-sequence-incomplete", result["certificationRefusals"])

    def test_cumulative_regression_options_change_and_terminal_order(self):
        earlier = sample_snapshot(sequence=1, draws=3, textures=3, presents=64)
        regression = sample_snapshot(sequence=2)
        with self.assertRaisesRegex(a.EvidenceError, "regressed"):
            a.analyze(line(earlier) + line(regression))
        later = copy.deepcopy(earlier)
        later["snapshotSequence"] = 2
        later["invariantPosition"] = True
        with self.assertRaisesRegex(a.EvidenceError, "identity/options changed"):
            a.analyze(line(earlier) + line(later))
        later["invariantPosition"] = earlier["invariantPosition"]
        earlier["reason"] = "normal-device-destruction"
        with self.assertRaisesRegex(a.EvidenceError, "after destruction"):
            a.analyze(line(earlier) + line(later))

    def test_retained_prefix_cannot_be_rewritten(self):
        first = sample_snapshot()
        later = sample_snapshot(sequence=2, draws=33, presents=64)
        later["drawSamples"][0]["primitiveCount"] = 0
        with self.assertRaisesRegex(a.EvidenceError, "retained first samples changed"):
            a.analyze(line(first) + line(later))
        later = sample_snapshot(sequence=2, draws=33, presents=64)
        later["vsKeys"]["keys"][0][4] = 1
        with self.assertRaisesRegex(a.EvidenceError, "retained keys changed"):
            a.analyze(line(first) + line(later))

    def test_fixture_exact_text_and_exclusive_creation(self):
        s = sample_snapshot()
        expected = "WARVK_FF_KEYS_V1\ninvariantPosition 0\nforceSampleRateShading 1\nvsCount 1\nfsCount 1\nVS 0 0 0 0 0\nFS " + " ".join(["0"] * 16) + "\nEND\n"
        self.assertEqual(a.fixture_text(s), expected)
        with tempfile.TemporaryDirectory(prefix="warvk-census-reader-") as directory:
            output = Path(directory) / "keys.txt"
            a.write_fixture(s, output)
            self.assertEqual(output.read_bytes(), expected.encode("ascii"))
            with self.assertRaises(FileExistsError):
                a.write_fixture(s, output)
            self.assertEqual(output.read_bytes(), expected.encode("ascii"))

    def test_cli_exit_and_invalid_latest_do_not_create_fixture(self):
        with tempfile.TemporaryDirectory(prefix="warvk-census-reader-") as directory:
            root = Path(directory)
            raw, output = root / "raw.log", root / "keys.txt"
            raw.write_text(line(sample_snapshot()), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                self.assertEqual(a.main([str(raw), "--fixture-out", str(output)]), 0)
            self.assertEqual(json.loads(stdout.getvalue())["selectedDeviceOrdinal"], 1)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(a.main([str(raw), "--fixture-out", str(output)]), 2)
            bad_output = root / "bad.txt"
            raw.write_text(line(sample_snapshot()) + a.MARKER + "{broken", encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(a.main([str(raw), "--fixture-out", str(bad_output)]), 2)
            self.assertFalse(bad_output.exists())


class StrictParserV2(unittest.TestCase):
    def reject(self, snapshot: dict, pattern: str = ".") -> None:
        with self.assertRaisesRegex(a.EvidenceError, pattern):
            a.parse_log(line(snapshot))

    def test_v1_evidence_keeps_original_contract_and_no_fabricated_refusal_zeros(self):
        old = sample_snapshot()
        result = a.analyze(line(old))
        self.assertEqual(result["selectedSnapshot"], old)
        self.assertEqual(result["selectedSnapshot"]["schema"], a.SCHEMA_V1)
        evidence = result["observationRefusalEvidence"]
        self.assertFalse(evidence["available"])
        self.assertNotIn("selected", evidence)
        self.assertNotIn("maxObserved", evidence)
        self.assertNotIn("observationRefusals", result["selectedSnapshot"])
        old["observationRefusals"] = {"recordBusy": 0, "snapshotBusy": 0, "counterSaturated": 0}
        self.reject(old, "fields differ")

    def test_argument_only_four_draw_forms_and_zero_known_values(self):
        s = sample_snapshot_v2(kinds=(1, 1, 1, 1))
        result = a.analyze(line(s))
        self.assertTrue(result["arithmeticValidated"])
        self.assertFalse(result["fullCoverageCertified"])
        self.assertEqual(s["drawMissingFields"], [0, 0, 4, 4, 2, 4, 2, 3])
        self.assertEqual([f["knownMask"] for f in s["drawSamples"]], [3, 67, 19, 211])
        zero = sample_snapshot_v2(kinds=(0, 0, 0, 1))
        zero["drawSamples"][0].update(streamStride=0, vertexCount=0, indexFormat=0)
        zero.update(maxStreamStride=0, maxVertexCount=0)
        self.assertEqual(a.validate_snapshot(zero)["drawMissingFields"][7], 0)

    def test_v2_exact_shape_policy_and_refusal_range(self):
        mutations = (
            lambda s: s.pop("observationPolicy"),
            lambda s: s.update(observationPolicy="state-and-argument"),
            lambda s: s.update(synchronizationPolicy="spin-until-ready"),
            lambda s: s.update(diagnosticForcedDeviceLock=True),
            lambda s: s["observationRefusals"].pop("snapshotBusy"),
            lambda s: s["observationRefusals"].update(unknown=0),
            lambda s: s["observationRefusals"].update(recordBusy=a.UINT32_MAX + 1),
            lambda s: s["observationRefusals"].update(snapshotBusy=-1),
            lambda s: s["observationRefusals"].update(counterSaturated=2),
            lambda s: s["observationRefusals"].update(recordBusy=True),
            lambda s: s["observationRefusals"].update(snapshotBusy="1"),
            lambda s: s["observationRefusals"].update(counterSaturated=False),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                s = sample_snapshot_v2()
                mutation(s)
                self.reject(s)

    def test_busy_does_not_relax_accepted_transaction_equalities(self):
        mutations = (
            lambda s: s.update(observedCalls=s["observedCalls"] + 1),
            lambda s: s.update(drawAttempts=s["drawAttempts"] + 1),
            lambda s: s.update(observedTextureRequests=s["observedTextureRequests"] + 1),
            lambda s: s.update(presentAttempts=s["presentAttempts"] + 1),
            lambda s: s["apiCalls"].update(DrawPrimitive=1),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                s = sample_snapshot_v2()
                s["observationRefusals"].update(recordBusy=100, snapshotBusy=100, counterSaturated=1)
                mutation(s)
                self.reject(s)

    def test_unobserved_fact_values_must_stay_zero(self):
        for kind in range(1, 5):
            for bit, field in enumerate(a.DRAW_FACT_FIELDS_BY_BIT):
                if a.V2_DRAW_MASKS[kind] & (1 << bit):
                    continue
                with self.subTest(kind=kind, field=field):
                    s = sample_snapshot_v2(kinds=tuple(int(i == kind) for i in range(1, 5)))
                    s["drawSamples"][0][field] = 1
                    self.reject(s, "unobserved value must be zero")

    def test_exact_known_masks_cannot_gain_or_lose_proofs(self):
        for kind in range(1, 5):
            for mask in (0, 255, a.V2_DRAW_MASKS[kind] ^ 4):
                with self.subTest(kind=kind, mask=mask):
                    s = sample_snapshot_v2(kinds=tuple(int(i == kind) for i in range(1, 5)))
                    s["drawSamples"][0]["knownMask"] = mask
                    self.reject(s, "argument-only knownMask")

    def test_cumulative_contract_checks_unretained_tail_beyond_32(self):
        s = sample_snapshot_v2(kinds=(32, 4, 3, 2), textures=80)
        self.assertTrue(all(f["kind"] == 1 for f in s["drawSamples"]))
        result = a.analyze(line(s))
        self.assertEqual(result["sampleLosses"]["drawSamplesDropped"], 9)
        self.assertEqual(result["sampleLosses"]["textureSamplesDropped"], 48)
        # These values can be consistent with the retained prefix but violate
        # the argument-only cumulative policy in the unretained tail.
        mutations = (
            lambda x: x["drawMissingFields"].__setitem__(4, x["drawMissingFields"][4] + 1),
            lambda x: x["drawMissingFields"].__setitem__(6, x["drawMissingFields"][6] + 1),
            lambda x: x["drawMissingFields"].__setitem__(7, x["drawMissingFields"][7] + 1),
            lambda x: x.update(fvfBitUnion=1),
            lambda x: x.update(textureStageMaskUnion=1),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                bad = copy.deepcopy(s)
                mutation(bad)
                self.reject(bad, "v2")
        bad = copy.deepcopy(s)
        bad["shaderPairs"][0] = 1
        bad["shaderPairMissing"] -= 1
        bad["drawMissingFields"][2] -= 1
        self.reject(bad, "v2 shader state")

    def test_busy_observations_are_noncoherent_and_maxima_not_complete_totals(self):
        first = sample_snapshot_v2(sequence=1)
        second = sample_snapshot_v2(sequence=2, kinds=(0, 2, 0, 0), presents=64)
        # Sequence1 may release its copy then read atomics after sequence2 has
        # already done so. All three observations are therefore independent.
        first["observationRefusals"] = {"recordBusy": 17, "snapshotBusy": 4, "counterSaturated": 1}
        second["observationRefusals"] = {"recordBusy": 9, "snapshotBusy": 2, "counterSaturated": 0}
        result = a.analyze(line(second) + line(first))
        evidence = result["observationRefusalEvidence"]
        self.assertTrue(result["arithmeticValidated"])
        self.assertEqual(result["selectedSnapshotSequence"], 2)
        self.assertEqual(evidence["selected"], second["observationRefusals"])
        self.assertEqual(evidence["maxObserved"], first["observationRefusals"])
        self.assertFalse(evidence["coherentWithSnapshot"])
        self.assertFalse(evidence["completeCallTotal"])
        self.assertTrue(evidence["counterSaturatedIsStickyLatch"])
        self.assertIn("record-transactions-refused-busy", result["certificationRefusals"])
        self.assertIn("snapshot-copies-refused-busy", result["certificationRefusals"])
        self.assertIn("observation-refusal-counters-saturated", result["certificationRefusals"])
        self.assertNotIn("totalCallAttempts", result)

    def test_v1_v2_mixed_same_device_rejected_but_distinct_devices_allowed(self):
        old = sample_snapshot(sequence=1)
        new = sample_snapshot_v2(sequence=2)
        with self.assertRaisesRegex(a.EvidenceError, "identity/options changed: schema"):
            a.analyze(line(old) + line(new))
        new["deviceOrdinal"] = 2
        result = a.analyze(line(old) + line(new), 2)
        self.assertEqual(result["selectedSnapshot"]["schema"], a.SCHEMA_V2)

    def test_v2_invalid_latest_and_coherent_regression_are_not_hidden_by_refusals(self):
        earlier = sample_snapshot_v2(sequence=1, kinds=(0, 2, 0, 0))
        later = sample_snapshot_v2(sequence=2)
        later["observationRefusals"].update(recordBusy=100)
        with self.assertRaisesRegex(a.EvidenceError, "regressed"):
            a.analyze(line(earlier) + line(later))
        for last in (a.MARKER + "{broken", line({**later, "synchronizationPolicy": "unchecked"})):
            with self.subTest(last=last[:40]):
                with self.assertRaises(a.EvidenceError):
                    a.analyze(line(earlier) + last)

    def test_v2_saturation_distinguishes_readable_refusal_latch_from_bad_arithmetic(self):
        s = sample_snapshot_v2()
        s["observationRefusals"].update(recordBusy=a.UINT32_MAX, snapshotBusy=a.UINT32_MAX,
                                       counterSaturated=1)
        self.assertTrue(a.analyze(line(s))["arithmeticValidated"])
        s["saturatedUpdates"] = 1
        self.reject(s, "counter saturation prevents arithmetic certification")

    def test_v2_fixture_preserves_same_build_key_format_not_complete_workload(self):
        old, new = sample_snapshot(), sample_snapshot_v2()
        self.assertEqual(a.fixture_text(new), a.fixture_text(old))
        result = a.analyze(line(new))
        self.assertEqual(result["fixtureScope"], "retained-local-build-key-samples-not-complete-workload")


class ProductionWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.device = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
        cls.header = (ROOT / "src/d3d9/d3d9_device.h").read_text(encoding="utf-8")
        cls.sink = (ROOT / "src/d3d9/d3d9_migration_census.cpp").read_text(encoding="utf-8")
        cls.census = (ROOT / "src/d3d9/d3d9_migration_census.h").read_text(encoding="utf-8")
        cls.writer = (ROOT / "src/d3d9/d3d9_migration_census_serialize.h").read_text(encoding="utf-8")

    def test_default_off_and_device_lock_policy_unchanged_in_dev_build(self):
        options = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
        meson = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")
        self.assertRegex(options, r"option\('warvk_d3d9_migration_census_dev',\s*type\s*:\s*'boolean',\s*value\s*:\s*false")
        self.assertRegex(meson, r"if get_option\('warvk_d3d9_migration_census_dev'\)\s+d3d9_cpp_args \+= \[ '-DWARVK_D3D9_MIGRATION_CENSUS_DEV=1' \]\s+endif")
        constructor = self.device[self.device.index("D3D9DeviceEx::D3D9DeviceEx("):self.device.index("D3D9DeviceEx::~D3D9DeviceEx(")]
        self.assertRegex(constructor, r"m_d3d9Options\(dxvkDevice, pParent->GetInstance\(\)->config\(\)\),\s*m_multithread\(BehaviorFlags & D3DCREATE_MULTITHREADED\),")
        self.assertNotIn("m_multithread(TRUE)", constructor)
        self.assertTrue(self.sink.startswith("#if defined(WARVK_D3D9_MIGRATION_CENSUS_DEV)"))
        self.assertNotIn("getenv", self.sink)
        multithread = (ROOT / "src/d3d9/d3d9_multithread.h").read_text(encoding="utf-8")
        self.assertIn("sync::RecursiveSpinlock m_mutex", multithread)
        self.assertIn("d3d9::migration::ConcurrentCensus m_migrationCensus;", self.header)
        self.assertIn("const uint64_t m_migrationDeviceOrdinal = MigrationAllocateDeviceOrdinal();", self.header)
        self.assertNotIn("m_migrationPresents", self.header)
        self.assertNotIn("m_migrationSnapshotSequence", self.header)

    def test_all_48_api_calls_have_matching_production_entry(self):
        labels = re.findall(r'"([A-Za-z0-9]+)"', self.census.split("ApiNames =", 1)[1].split("inline constexpr const char* ApiName", 1)[0])
        self.assertEqual(tuple(labels), a.API_NAMES)
        seen = re.findall(r"MigrationNoteApi\(d3d9::migration::Api::(\w+)\)", self.device)
        grouped = {"Present", *a.DRAW_APIS, *a.TEXTURE_APIS}
        self.assertEqual(Counter(seen), Counter(name for name in a.API_NAMES if name not in grouped))
        for name in a.API_NAMES:
            with self.subTest(api=name):
                if name == "Present":
                    self.assertIn("MigrationPresent();", function_body(self.device, "PresentEx"))
                    self.assertIn("m_migrationCensus.notePresent(emit);", function_body(self.sink, "MigrationPresent"))
                    self.assertNotIn("noteCall", function_body(self.sink, "MigrationPresent"))
                elif name in a.DRAW_APIS:
                    self.assertIn("MigrationNoteDraw(", function_body(self.device, name))
                    self.assertNotIn("MigrationNoteApi(", function_body(self.device, name))
                elif name in a.TEXTURE_APIS:
                    self.assertIn("MigrationNoteTexture(", function_body(self.device, name))
                    self.assertNotIn("MigrationNoteApi(", function_body(self.device, name))
                else:
                    self.assertIn(f"MigrationNoteApi(d3d9::migration::Api::{name});", function_body(self.device, name))

    def test_all_integration_calls_compile_guarded(self):
        blocks = re.findall(r"#if defined\(WARVK_D3D9_MIGRATION_CENSUS_DEV\)(.*?)#endif", self.device, re.S)
        guarded = "\n".join(blocks)
        calls = re.findall(r"\bMigration(?:NoteApi|NoteTexture|NoteDraw|NoteVsKey|NoteFsKey|Present|Log)\([^;]*;", self.device, re.S)
        self.assertTrue(calls)
        for call in calls:
            self.assertIn(call, guarded)

    def test_four_draw_entries_keep_original_lock_and_one_grouped_transaction(self):
        arguments = {
            "Primitive": "PrimitiveType, PrimitiveCount, 0u, false, 0u, false, D3DFMT_UNKNOWN, false",
            "IndexedPrimitive": "PrimitiveType, PrimitiveCount, 0u, false, NumVertices, true, D3DFMT_UNKNOWN, false",
            "PrimitiveUP": "PrimitiveType, PrimitiveCount, VertexStreamZeroStride, true, 0u, false, D3DFMT_UNKNOWN, false",
            "IndexedPrimitiveUP": "PrimitiveType, PrimitiveCount, VertexStreamZeroStride, true, NumVertices, true, IndexDataFormat, true",
        }
        for api, kind in zip(a.DRAW_APIS, ("Primitive", "IndexedPrimitive", "PrimitiveUP", "IndexedPrimitiveUP")):
            with self.subTest(api=api):
                body = function_body(self.device, api)
                lock = body.index("D3D9DeviceLock lock = LockDevice();")
                facts = body.index(f"MigrationNoteDraw(d3d9::migration::DrawKind::{kind},")
                self.assertLess(lock, facts)
                self.assertNotIn("MigrationNoteApi(", body)
                self.assertEqual(nesting_depth(body, lock), nesting_depth(body, facts))
                self.assertNotIn("return ", code_mask(body[lock:facts]))
                flattened = re.sub(r"\s+", " ", body)
                self.assertIn(f"MigrationNoteDraw(d3d9::migration::DrawKind::{kind}, {arguments[kind]});", flattened)
        helper = function_body(self.sink, "MigrationNoteDraw")
        self.assertIn("facts.knownMask = PrimitiveTypeKnown | PrimitiveCountKnown;", helper)
        self.assertIn("facts.knownMask |= StreamStrideKnown;", helper)
        self.assertIn("facts.knownMask |= VertexCountKnown;", helper)
        self.assertIn("facts.knownMask |= IndexFormatKnown;", helper)
        self.assertNotIn("FvfKnown", helper)
        self.assertNotIn("ShaderPairKnown", helper)
        self.assertIn("m_migrationCensus.noteDraw(facts);", helper)
        self.assertNotIn("noteCall", helper)
        self.assertIn("vertexCountKnown", helper)
        self.assertIn("indexFormatKnown", helper)
        self.assertNotIn("GetShaderModule", helper)

    def test_texture_calls_and_known_zero_requests_use_one_grouped_transaction(self):
        for api, kind in zip(a.TEXTURE_APIS, ("Texture2D", "Cube", "Volume")):
            with self.subTest(api=api):
                body = function_body(self.device, api)
                facts = body.index(f"MigrationNoteTexture({{d3d9::migration::TextureKind::{kind},")
                self.assertNotIn("migrationLock", body)
                self.assertNotIn("MigrationNoteApi(", body)
                self.assertIn("AllTextureFieldsKnown", body[facts:body.index("#endif", facts)])
                self.assertIn("Levels", body[facts:body.index("#endif", facts)])
                self.assertNotIn("?", body[facts:body.index("#endif", facts)])
        helper = function_body(self.sink, "MigrationNoteTexture")
        self.assertIn("m_migrationCensus.noteTexture(facts);", helper)
        self.assertNotIn("noteCall", helper)

    def test_sink_never_reacquires_device_or_queries_live_device_state(self):
        masked = code_mask(self.sink)
        self.assertNotRegex(masked, r"\b(LockDevice|m_state|GetCommonBuffer|GetShaderModule|EmitCs)\b")
        self.assertNotRegex(masked, r"\b(m_migrationPresents|m_migrationSnapshotSequence)\b")
        self.assertNotIn("m_migrationCensus.snapshot()", masked)

    def test_ff_specialized_and_uber_capture_on_calling_side(self):
        vs = function_body(self.device, "UpdateFixedFunctionVS")
        fs = function_body(self.device, "UpdateFixedFunctionPS")
        self.assertIn("MigrationNoteVsKey(key.Data);", vs)
        self.assertIn("MigrationNoteVsKey(key);", vs)
        self.assertNotIn("MigrationNoteVsKey(data->Key)", vs)
        self.assertLess(vs.index("MigrationNoteVsKey(key.Data)"), vs.index("EmitCs("))
        self.assertRegex(vs, r"const auto key = BuildFFKeyVS\(vertexBlendMode, indexedVertexBlend\)\.Data;\s*data->Key = key;\s*#if defined\(WARVK_D3D9_MIGRATION_CENSUS_DEV\)\s*MigrationNoteVsKey\(key\);")
        self.assertNotIn("data->Key = BuildFFKeyVS", vs)
        self.assertLess(fs.index("BuildFFKeyFS()"), fs.index("MigrationNoteFsKey(key)"))
        self.assertLess(fs.index("MigrationNoteFsKey(key)"), fs.index("if (useUbershader"))
        self.assertNotIn("MigrationNote", (ROOT / "src/d3d9/d3d9_fixed_function.cpp").read_text(encoding="utf-8"))

    def test_normal_destructor_only_after_module_detachment_guard(self):
        body = function_body(self.device, "~D3D9DeviceEx")
        guard = body.index("this_thread::isInModuleDetachment()")
        exit_early = body.index("return;", guard)
        log = body.index('MigrationLog("normal-device-destruction")')
        self.assertLess(guard, exit_early)
        self.assertLess(exit_early, log)
        self.assertEqual(nesting_depth(body, log), 1)

    def test_snapshot_sequence_copied_by_value_refusals_and_logger_after_cell(self):
        body = function_body(self.sink, "MigrationLog")
        copy = body.index("d3d9::migration::CensusCopy copy;")
        snapshot = body.index("if (!m_migrationCensus.trySnapshot(copy)) return;")
        refusals = body.index("m_migrationCensus.observationRefusals();")
        formatter = body.index("std::ostringstream out;")
        serialize = body.index("d3d9::migration::WriteCensusSnapshot(out, copy, refusals, info);")
        logger = body.index("Logger::info(out.str());")
        self.assertLess(copy, snapshot)
        self.assertLess(snapshot, refusals)
        self.assertLess(refusals, formatter)
        self.assertLess(formatter, serialize)
        self.assertLess(serialize, logger)
        self.assertEqual(body.count(".trySnapshot("), 1)
        self.assertEqual(nesting_depth(body, snapshot), 1)
        self.assertEqual(nesting_depth(body, logger), 1)
        self.assertEqual(nesting_depth(body, formatter), 1)
        self.assertIn("const auto& s = copy.values;", self.writer)
        self.assertIn("<< copy.sequence", self.writer)
        self.assertIn("s.apiCalls[std::size_t(Api::Present)]", self.writer)
        self.assertIn('\\"schema\\":\\"warvk-d3d9-migration-census/v2\\"', self.writer)
        self.assertIn('\\"diagnosticForcedDeviceLock\\":false', self.writer)
        self.assertIn('\\"observationPolicy\\":\\"argument-and-local-key-only\\"', self.writer)
        self.assertIn('\\"synchronizationPolicy\\":\\"bounded-try-cell\\"', self.writer)
        self.assertIn('\\"shaderPairMeaning\\":\\"bound-handles-not-effective-shader\\"', self.writer)

    def test_cold_production_writer_only_consumes_owned_snapshot_and_export_metadata(self):
        self.assertIn('#include "d3d9_migration_census_serialize.h"', self.sink)
        self.assertRegex(self.writer, r"WriteCensusSnapshot\(std::ostream& out, const CensusCopy& copy,\s*const ObservationRefusals& refusals, const CensusExportInfo& info\)")
        masked = code_mask(self.writer)
        self.assertNotRegex(masked, r"\b(LockDevice|m_state|GetCommonBuffer|GetShaderModule|EmitCs|Logger|ConcurrentCensus)\b")
        self.assertNotRegex(masked, r"\.(trySnapshot|observationRefusals|noteCall|noteDraw|noteTexture)\(")


if __name__ == "__main__":
    unittest.main(verbosity=2)
