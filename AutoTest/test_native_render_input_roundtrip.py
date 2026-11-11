"""Actual CaptureMaterialFacts -> production JSON encoder -> strict reader.

Usage: py AutoTest/test_native_render_input_roundtrip.py <fixture.exe>
The fixture is AutoTest/test_native_render_input_export.cpp built with include/.
This verifies a CPU observation/export contract, not GPU or shared-core readiness.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import unittest
from pathlib import Path

from native_render_input_contract import (
    NATIVE_ARRAY_LENGTHS,
    NATIVE_DRAW_MATERIAL_KEYS,
    NATIVE_FINAL_SHADOW_MATERIAL_KEYS,
    NATIVE_RENDER_INPUT_KEYS,
    NATIVE_REQUIRED_SHARED_FIELDS_MISSING,
    validate_render_input_evidence,
)

FIXTURE: Path


class NativeRenderInputRoundtrip(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(
            [str(FIXTURE), "--fixture"], check=True, capture_output=True,
            text=True, timeout=30,
        )
        records = json.loads(result.stdout)
        if type(records) is not list:
            raise AssertionError("fixture must emit a JSON case array")
        cls.cases = {case["name"]: case for case in records}
        expected = {
            "normal", "unknown", "nan", "retained", "zero_alpha", "dirty_unknown",
            "zero_source_frame", "zero_map_epoch", "zero_device_epoch",
        }
        if len(records) != len(expected) or set(cls.cases) != expected:
            raise AssertionError("fixture must emit every declared case exactly once")

    def validate(self, name):
        case = self.cases[name]
        return validate_render_input_evidence(
            case["renderInputEvidence"], part=case["part"], layer=case["layer"],
            map_epoch=case["mapEpoch"], device_epoch=case["deviceEpoch"],
        )

    def test_exact_wire_shapes(self):
        for name, case in self.cases.items():
            with self.subTest(name=name):
                block = case["renderInputEvidence"]
                self.assertEqual(set(block), set(NATIVE_RENDER_INPUT_KEYS))
                self.assertEqual(set(block["finalShadowMaterial"]), set(NATIVE_FINAL_SHADOW_MATERIAL_KEYS))
                native = block["nativeDrawMaterialObservation"]
                self.assertEqual(set(native), set(NATIVE_DRAW_MATERIAL_KEYS))
                for field, count in NATIVE_ARRAY_LENGTHS:
                    self.assertEqual(len(native[field]), count)
                self.assertEqual(block["requiredSharedFieldsMissing"], list(NATIVE_REQUIRED_SHARED_FIELDS_MISSING))

    def test_normal_raw_bits_and_tokens(self):
        result = self.validate("normal")
        self.assertTrue(result["nativeDrawStateAvailable"])
        self.assertEqual(result["materialDiffuseBits"], [0x80000000, 0x3F800000, 0x40000000, 0])
        native = self.cases["normal"]["renderInputEvidence"]["nativeDrawMaterialObservation"]
        self.assertEqual(native["textureTransform0Bits"][2], 0x80000000)
        self.assertEqual(native["sampler0"][7], 0x80000000)
        final = result["finalShadowMaterial"]
        self.assertEqual(final["alphaRefBits"], 0x80000000)
        self.assertEqual(final["textureView"], "18446744073709551615")
        self.assertEqual(final["sampler"], "9007199254740993")
        self.assertFalse(result["sharedRendererReady"])

    def test_unknown_has_zero_payload_and_no_white_fallback(self):
        for name in ("unknown", "dirty_unknown"):
            with self.subTest(name=name):
                result = self.validate(name)
                self.assertFalse(result["nativeDrawStateAvailable"])
                self.assertIsNone(result["materialDiffuseBits"])
                native = self.cases[name]["renderInputEvidence"]["nativeDrawMaterialObservation"]
                for field in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
                    self.assertEqual(native[field], "0")
                for field, count in NATIVE_ARRAY_LENGTHS:
                    self.assertEqual(native[field], [0] * count)

    def test_known_zero_alpha_remains_known(self):
        result = self.validate("zero_alpha")
        self.assertTrue(result["nativeDrawStateAvailable"])
        self.assertEqual(result["materialDiffuseBits"], [0, 0, 0, 0])

    def test_nan_bits_survive_encoder_but_reader_rejects(self):
        native = self.cases["nan"]["renderInputEvidence"]["nativeDrawMaterialObservation"]
        self.assertIs(native["available"], True)
        self.assertEqual(native["materialBits"][0], 0x7FC12345)
        with self.assertRaisesRegex(ValueError, r"materialBits\[0\].*finite"):
            self.validate("nan")

    def test_retained_producer_frame_is_not_batch_or_event_frame(self):
        case = self.cases["retained"]
        result = self.validate("retained")
        self.assertEqual(result["sourceFrame"], "987654321")
        self.assertEqual(result["sourceFrameDomain"], "ShadowProducerSerial")
        self.assertNotEqual(result["sourceFrame"], case["frame"])
        self.assertNotEqual(result["sourceFrame"], case["eventFrame"])

    def test_available_zero_identity_is_never_fabricated(self):
        for name, field in (
            ("zero_source_frame", "sourceFrame"),
            ("zero_map_epoch", "mapEpoch"),
            ("zero_device_epoch", "deviceEpoch"),
        ):
            with self.subTest(name=name):
                native = self.cases[name]["renderInputEvidence"]["nativeDrawMaterialObservation"]
                self.assertIs(native["available"], True)
                self.assertEqual(native[field], "0")
                with self.assertRaisesRegex(ValueError, field + r".*nonzero"):
                    self.validate(name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    FIXTURE = parser.parse_args().fixture.resolve(strict=True)
    unittest.main(argv=[__file__], verbosity=2)
