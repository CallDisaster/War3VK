"""Targeted wire-contract tests; no native memory, build, or GPU claims."""

import copy
import json
import struct
import unittest

try:
    from . import native_render_input_contract as contract
except ImportError:
    import native_render_input_contract as contract


def float_bits(value):
    return struct.unpack("!I", struct.pack("!f", value))[0]


def valid_block(available=True):
    native = {
        "schema": 1,
        "available": available,
        "frameDomain": "ShadowProducerSerial",
        "sourceFrame": "907" if available else "0",
        "mapEpoch": "2" if available else "0",
        "deviceEpoch": "3" if available else "0",
        "part": "4294967307" if available else "0",
        "layer": 4 if available else 0,
        "shaderMask": 3 if available else 0,
    }
    for name, length in contract.NATIVE_ARRAY_LENGTHS:
        native[name] = [0] * length
    if available:
        native["materialBits"][:4] = [float_bits(0.25), float_bits(0.5), float_bits(1.0), 0]
        for diagonal in (0, 5, 10, 15):
            native["textureTransform0Bits"][diagonal] = float_bits(1.0)
    return {
        "schema": 1,
        "captureStage": "PreparedShadowReplay",
        "finalShadowMaterial": {
            "alphaRefBits": float_bits(0.5),
            "alphaTest": True, "alphaBlend": False,
            "depthTest": True, "depthWrite": True, "additive": False,
            "uvBinding": 1, "uvStride": 8, "uvOffset": 0, "uvFormat": 103,
            "textureView": "4294967313", "sampler": "4294967314", "samplerIndex": 0,
        },
        "nativeDrawMaterialObservation": native,
        "requiredSharedFieldsMissing": list(contract.NATIVE_REQUIRED_SHARED_FIELDS_MISSING),
    }


class NativeRenderInputContractTests(unittest.TestCase):
    def validate(self, block=None, **kwargs):
        identity = dict(part="4294967307", layer=4, map_epoch="2", device_epoch="3")
        identity.update(kwargs)
        return contract.validate_render_input_evidence(
            valid_block() if block is None else block, **identity)

    def reject_native(self, name, value, available=True):
        block = valid_block(available)
        block["nativeDrawMaterialObservation"][name] = value
        with self.assertRaises(ValueError, msg=f"{name}={value!r}"):
            self.validate(block)

    def reject_final(self, name, value):
        block = valid_block()
        block["finalShadowMaterial"][name] = value
        with self.assertRaises(ValueError, msg=f"{name}={value!r}"):
            self.validate(block)

    def test_available_observation_summary(self):
        block = valid_block()
        result = self.validate(block)
        self.assertTrue(result["nativeDrawStateAvailable"])
        self.assertEqual(result["sourceFrame"], "907")
        self.assertEqual(result["sourceFrameDomain"], "ShadowProducerSerial")
        self.assertEqual(result["materialDiffuseBits"], block["nativeDrawMaterialObservation"]["materialBits"][:4])
        self.assertEqual(result["finalShadowMaterial"], block["finalShadowMaterial"])
        self.assertIs(result["sharedRendererReady"], False)
        self.assertEqual(result["requiredSharedFieldsMissing"], list(contract.NATIVE_REQUIRED_SHARED_FIELDS_MISSING))

    def test_unavailable_zero_payload_is_unknown(self):
        result = self.validate(valid_block(False))
        self.assertIs(result["nativeDrawStateAvailable"], False)
        self.assertIsNone(result["materialDiffuseBits"])
        self.assertEqual(result["sourceFrame"], "0")
        self.assertIs(result["sharedRendererReady"], False)

    def test_available_zero_opacity_is_known(self):
        block = valid_block()
        block["nativeDrawMaterialObservation"]["materialBits"][:4] = [0, 0, 0, 0]
        result = self.validate(block)
        self.assertTrue(result["nativeDrawStateAvailable"])
        self.assertEqual(result["materialDiffuseBits"], [0, 0, 0, 0])

    def test_available_all_zero_arrays_are_valid_observations(self):
        block = valid_block()
        native = block["nativeDrawMaterialObservation"]
        for name, length in contract.NATIVE_ARRAY_LENGTHS:
            native[name] = [0] * length
        self.assertTrue(self.validate(block)["nativeDrawStateAvailable"])

    def test_source_frame_is_independent_of_cpu_event_frame(self):
        # The surrounding schema-3 CPU event has a different frame domain.
        envelope = {"frame": "17", "eventFrame": "19", "renderInputEvidence": valid_block()}
        envelope["renderInputEvidence"]["nativeDrawMaterialObservation"]["sourceFrame"] = "987654321"
        result = self.validate(envelope["renderInputEvidence"])
        self.assertEqual(result["sourceFrame"], "987654321")

    def test_summary_is_detached_and_input_is_unchanged(self):
        block = valid_block()
        original = copy.deepcopy(block)
        result = self.validate(block)
        self.assertEqual(block, original)
        result["materialDiffuseBits"][0] = 1
        result["finalShadowMaterial"]["alphaRefBits"] = 0
        result["requiredSharedFieldsMissing"].clear()
        self.assertEqual(block, original)

    def test_json_roundtrip_preserves_raw_words(self):
        block = valid_block()
        block["nativeDrawMaterialObservation"]["materialBits"][0] = 0x80000000
        result = self.validate(json.loads(json.dumps(block)))
        self.assertEqual(result["materialDiffuseBits"][0], 0x80000000)

    def test_raw_negative_zero_material_and_transform_are_valid(self):
        block = valid_block()
        native = block["nativeDrawMaterialObservation"]
        native["materialBits"] = [0x80000000] * 17
        native["textureTransform0Bits"] = [0x80000000] * 16
        self.assertEqual(self.validate(block)["materialDiffuseBits"], [0x80000000] * 4)
        self.assertEqual(native["textureTransform0Bits"], [0x80000000] * 16)

    def test_final_alpha_negative_zero_bits_are_preserved(self):
        block = valid_block()
        block["finalShadowMaterial"]["alphaRefBits"] = 0x80000000
        self.assertEqual(self.validate(block)["finalShadowMaterial"]["alphaRefBits"], 0x80000000)

    def test_final_alpha_closed_interval_endpoints(self):
        for word in (0, float_bits(1.0)):
            with self.subTest(word=word):
                block = valid_block()
                block["finalShadowMaterial"]["alphaRefBits"] = word
                self.validate(block)

    def test_final_alpha_outside_interval_rejected(self):
        for word in (float_bits(-0.1), float_bits(1.1), 0x80000001, 0x3F800001):
            with self.subTest(word=word):
                self.reject_final("alphaRefBits", word)

    def test_final_alpha_nan_and_infinity_rejected(self):
        for word in (0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFC12345):
            with self.subTest(word=word):
                self.reject_final("alphaRefBits", word)

    def test_every_material_word_rejects_nonfinite_bits(self):
        for index in range(17):
            for word in (0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001):
                with self.subTest(index=index, word=word):
                    block = valid_block()
                    block["nativeDrawMaterialObservation"]["materialBits"][index] = word
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_every_transform_word_rejects_nonfinite_bits(self):
        for index in range(16):
            for word in (0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC12345):
                with self.subTest(index=index, word=word):
                    block = valid_block()
                    block["nativeDrawMaterialObservation"]["textureTransform0Bits"][index] = word
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_finite_float_extremes_and_subnormals_are_valid(self):
        for word in (0x7F7FFFFF, 0xFF7FFFFF, 1, 0x80000001):
            with self.subTest(word=word):
                block = valid_block()
                block["nativeDrawMaterialObservation"]["materialBits"] = [word] * 17
                block["nativeDrawMaterialObservation"]["textureTransform0Bits"] = [word] * 16
                self.assertEqual(self.validate(block)["materialDiffuseBits"], [word] * 4)

    def test_root_requires_dict(self):
        for value in (None, [], (), "", 1, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                contract.validate_render_input_evidence(value, part="1", layer=0, map_epoch="1", device_epoch="1")

    def test_nested_objects_require_dicts(self):
        for name in ("finalShadowMaterial", "nativeDrawMaterialObservation"):
            for value in (None, [], (), "", 1, True):
                with self.subTest(name=name, value=value):
                    block = valid_block()
                    block[name] = value
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_exact_keys_reject_missing_fields(self):
        for name in (None, "finalShadowMaterial", "nativeDrawMaterialObservation"):
            prototype = valid_block()
            obj = prototype if name is None else prototype[name]
            for key in obj:
                with self.subTest(object=name, key=key):
                    block = copy.deepcopy(prototype)
                    del (block if name is None else block[name])[key]
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_exact_keys_reject_extra_fields(self):
        for name in (None, "finalShadowMaterial", "nativeDrawMaterialObservation"):
            for key in ("sharedRendererReady", "sourceOwnerProof", "eventFrame", 7):
                with self.subTest(object=name, key=key):
                    block = valid_block()
                    (block if name is None else block[name])[key] = True
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_object_keys_require_exact_string_type(self):
        class StringSubclass(str):
            pass
        for name in (None, "finalShadowMaterial", "nativeDrawMaterialObservation"):
            with self.subTest(object=name):
                block = valid_block()
                obj = block if name is None else block[name]
                key = next(iter(obj))
                value = obj.pop(key)
                obj[StringSubclass(key)] = value
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_root_and_native_schema_are_exact_integer_one(self):
        for name in (None, "nativeDrawMaterialObservation"):
            for value in (0, 2, True, 1.0, "1", None):
                with self.subTest(object=name, value=value):
                    block = valid_block()
                    (block if name is None else block[name])["schema"] = value
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_capture_stage_is_exact(self):
        for value in ("", "NativeDraw", "preparedshadowreplay", "PreparedShadowReplay ", 1, True):
            with self.subTest(value=value):
                block = valid_block()
                block["captureStage"] = value
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_native_frame_domain_is_exact_even_when_unavailable(self):
        for available in (True, False):
            for value in ("", "CPUEventSerial", "FrameEvidenceSerial", 1, None):
                with self.subTest(available=available, value=value):
                    self.reject_native("frameDomain", value, available)

    def test_available_requires_real_boolean(self):
        for value in (0, 1, "true", "false", None, [], 1.0):
            with self.subTest(value=value):
                self.reject_native("available", value)

    def test_final_flags_require_real_booleans(self):
        for name in ("alphaTest", "alphaBlend", "depthTest", "depthWrite", "additive"):
            for value in (0, 1, None, "true", 0.0):
                with self.subTest(name=name, value=value):
                    self.reject_final(name, value)

    def test_native_identity_requires_canonical_u64_strings(self):
        invalid = (1, True, 1.0, None, "", "01", "+1", "-1", " 1", "1 ", "1\n", "1.0", "١", "１")
        for name in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
            for value in invalid:
                with self.subTest(name=name, value=value):
                    self.reject_native(name, value)

    def test_native_identity_rejects_u64_overflow_and_huge_values(self):
        for name in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
            for value in (str(1 << 64), "9" * 5000):
                with self.subTest(name=name, length=len(value)):
                    self.reject_native(name, value)

    def test_native_u64_maximum_is_valid(self):
        block = valid_block()
        native = block["nativeDrawMaterialObservation"]
        maximum = str((1 << 64) - 1)
        for name in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
            native[name] = maximum
        self.assertEqual(self.validate(block, part=maximum, map_epoch=maximum, device_epoch=maximum)["sourceFrame"], maximum)

    def test_available_identity_cannot_be_zero(self):
        for name in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
            with self.subTest(name=name):
                self.reject_native(name, "0")

    def test_available_map_epoch_must_match(self):
        self.reject_native("mapEpoch", "4")

    def test_available_device_epoch_must_match(self):
        self.reject_native("deviceEpoch", "4")

    def test_available_part_must_match(self):
        self.reject_native("part", "4294967308")

    def test_available_layer_must_match(self):
        self.reject_native("layer", 5)

    def test_layer_zero_and_uint32_max_are_valid(self):
        for layer in (0, (1 << 32) - 1):
            with self.subTest(layer=layer):
                block = valid_block()
                block["nativeDrawMaterialObservation"]["layer"] = layer
                self.validate(block, layer=layer)

    def test_native_layer_type_and_range(self):
        for value in (-1, 1 << 32, True, False, 4.0, "4", None):
            with self.subTest(value=value):
                self.reject_native("layer", value)

    def test_shader_masks_zero_through_three_are_valid(self):
        for mask in range(4):
            with self.subTest(mask=mask):
                block = valid_block()
                block["nativeDrawMaterialObservation"]["shaderMask"] = mask
                self.validate(block)

    def test_shader_mask_rejects_unknown_bits_types_and_ranges(self):
        for value in (4, 5, 8, (1 << 32) - 1, -1, 1 << 32, True, 3.0, "3", None):
            with self.subTest(value=value):
                self.reject_native("shaderMask", value)

    def test_all_native_arrays_have_exact_lengths(self):
        for name, length in contract.NATIVE_ARRAY_LENGTHS:
            for actual in (0, length - 1, length + 1):
                with self.subTest(name=name, length=actual):
                    self.reject_native(name, [0] * actual)

    def test_native_arrays_require_real_lists(self):
        for name, length in contract.NATIVE_ARRAY_LENGTHS:
            for value in (tuple([0] * length), {}, "0" * length, None):
                with self.subTest(name=name, type=type(value).__name__):
                    self.reject_native(name, value)

    def test_all_array_word_types_and_ranges(self):
        for name, length in contract.NATIVE_ARRAY_LENGTHS:
            for value in (-1, 1 << 32, True, False, 0.0, "0", None):
                for index in (0, length - 1):
                    with self.subTest(name=name, index=index, value=value):
                        block = valid_block()
                        block["nativeDrawMaterialObservation"][name][index] = value
                        with self.assertRaises(ValueError):
                            self.validate(block)

    def test_state_arrays_accept_arbitrary_uint32_values(self):
        block = valid_block()
        for name in ("renderStates", "textureStage0", "sampler0"):
            block["nativeDrawMaterialObservation"][name] = [(1 << 32) - 1] * len(block["nativeDrawMaterialObservation"][name])
        block["nativeDrawMaterialObservation"]["sampler0"][7] = 0xFF7FFFFF
        self.validate(block)

    def test_sampler_lod_bias_rejects_nonfinite_float_bits(self):
        for word in (0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFC12345):
            with self.subTest(word=word):
                block = valid_block()
                block["nativeDrawMaterialObservation"]["sampler0"][7] = word
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_sampler_lod_bias_retains_finite_signed_zero(self):
        block = valid_block()
        block["nativeDrawMaterialObservation"]["sampler0"][7] = 0x80000000
        self.validate(block)
        self.assertEqual(block["nativeDrawMaterialObservation"]["sampler0"][7], 0x80000000)

    def test_shader_presence_does_not_certify_shader_color_or_geoa(self):
        block = valid_block()
        native = block["nativeDrawMaterialObservation"]
        native["shaderMask"] = 3
        native["materialBits"][:4] = [float_bits(0.25)] * 4
        block["finalShadowMaterial"]["alphaRefBits"] = float_bits(0.75)
        result = self.validate(block)
        self.assertEqual(result["materialDiffuseBits"], [float_bits(0.25)] * 4)
        self.assertEqual(result["finalShadowMaterial"]["alphaRefBits"], float_bits(0.75))
        self.assertIs(result["sharedRendererReady"], False)
        self.assertIn("evaluatedGeoaAlphaRgb", result["requiredSharedFieldsMissing"])
        self.assertNotIn("finalShaderColor", result)
        self.assertNotIn("evaluatedGeoaAlphaRgb", result)

    def test_unavailable_cannot_backfill_native_identity(self):
        for name in ("sourceFrame", "mapEpoch", "deviceEpoch", "part"):
            with self.subTest(name=name):
                self.reject_native(name, "1", available=False)

    def test_unavailable_cannot_backfill_layer_or_shader(self):
        for name in ("layer", "shaderMask"):
            with self.subTest(name=name):
                self.reject_native(name, 1, available=False)

    def test_unavailable_requires_all_zero_arrays(self):
        for name, length in contract.NATIVE_ARRAY_LENGTHS:
            for index in range(length):
                with self.subTest(name=name, index=index):
                    block = valid_block(False)
                    block["nativeDrawMaterialObservation"][name][index] = 1
                    with self.assertRaises(ValueError):
                        self.validate(block)

    def test_unavailable_negative_zero_is_not_unknown_zero_payload(self):
        for name in ("materialBits", "textureTransform0Bits"):
            with self.subTest(name=name):
                block = valid_block(False)
                block["nativeDrawMaterialObservation"][name][0] = 0x80000000
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_final_integer_types_and_ranges(self):
        for name in ("alphaRefBits", "uvBinding", "uvStride", "uvOffset", "uvFormat", "samplerIndex"):
            for value in (-1, 1 << 32, True, False, "0", 0.0, None):
                with self.subTest(name=name, value=value):
                    self.reject_final(name, value)

    def test_final_uint32_maximum_is_valid_for_uint_fields(self):
        block = valid_block()
        for name in ("uvBinding", "uvStride", "uvOffset", "uvFormat", "samplerIndex"):
            block["finalShadowMaterial"][name] = (1 << 32) - 1
        self.validate(block)

    def test_final_handles_require_canonical_u64_strings(self):
        for name in ("textureView", "sampler"):
            for value in (1, True, None, "01", "-1", "+1", "1.0", " 1", "１", str(1 << 64)):
                with self.subTest(name=name, value=value):
                    self.reject_final(name, value)

    def test_final_handle_zero_and_uint64_max_are_valid(self):
        block = valid_block()
        block["finalShadowMaterial"]["textureView"] = "0"
        block["finalShadowMaterial"]["sampler"] = str((1 << 64) - 1)
        self.validate(block)

    def test_missing_list_cannot_self_certify_shared_renderer(self):
        original = list(contract.NATIVE_REQUIRED_SHARED_FIELDS_MISSING)
        variants = ([], original[:-1], original[1:], original[::-1], original + [original[0]],
                    original + ["newField"], tuple(original), None, [True] + original[1:])
        for missing in variants:
            with self.subTest(missing=missing):
                block = valid_block()
                block["requiredSharedFieldsMissing"] = missing
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_each_missing_list_entry_is_exact(self):
        for index in range(len(contract.NATIVE_REQUIRED_SHARED_FIELDS_MISSING)):
            with self.subTest(index=index):
                block = valid_block()
                block["requiredSharedFieldsMissing"][index] += " "
                with self.assertRaises(ValueError):
                    self.validate(block)

    def test_caller_identity_has_strict_types_and_ranges(self):
        for name in ("part", "map_epoch", "device_epoch"):
            for value in (1, True, None, "01", "-1", str(1 << 64)):
                with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                    self.validate(**{name: value})
        for value in (True, -1, 1 << 32, "4", 4.0):
            with self.subTest(layer=value), self.assertRaises(ValueError):
                self.validate(layer=value)

    def test_unavailable_does_not_require_matching_caller_zero_identity(self):
        self.validate(valid_block(False), part="0", layer=0, map_epoch="0", device_epoch="0")

    def test_wire_constant_orders_are_exact(self):
        self.assertEqual(contract.NATIVE_RENDER_STATES,
                         (15, 25, 24, 27, 19, 20, 171, 206, 207, 208, 209, 7, 14, 23, 22, 168, 60))
        self.assertEqual(contract.NATIVE_STAGE_STATES, (1, 2, 3, 4, 5, 6, 11, 24, 26, 27))
        self.assertEqual(contract.NATIVE_SAMPLER_STATES, tuple(range(1, 14)))
        self.assertEqual(contract.NATIVE_MATERIAL_FIELDS[:4], ("DiffuseR", "DiffuseG", "DiffuseB", "DiffuseA"))
        self.assertEqual(contract.NATIVE_ARRAY_LENGTHS,
                         (("renderStates", 17), ("textureStage0", 10), ("sampler0", 13),
                          ("materialBits", 17), ("textureTransform0Bits", 16)))


if __name__ == "__main__":
    unittest.main()
