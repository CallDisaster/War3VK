#!/usr/bin/env python3
"""Positive and negative tests for release_v122_config.py."""
from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_v122_config as v  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MESON_OPTIONS = v.parse_options((ROOT / "meson_options.txt").read_text(encoding="utf-8"))


def entry(value, kind="boolean"):
    return {"value": value, "type": kind}


def base_intro():
    data = {
        "buildtype": entry("release", "combo"),
        "strip": entry(False),
        "debug": entry(False),
        "optimization": entry("3", "combo"),
        "build_id": entry(False),
        "b_ndebug": entry("false", "combo"),
        "enable_dxgi": entry(False),
        "enable_d3d8": entry(False),
        "enable_d3d9": entry(True),
        "enable_d3d10": entry(False),
        "enable_d3d11": entry(False),
        "native_glfw": entry("auto", "combo"),
        "native_sdl2": entry("auto", "combo"),
        "native_sdl3": entry("auto", "combo"),
    }
    for name, value in v.PRODUCT_OPTIONS.items():
        data[name] = entry(value)
    return data


def product_intro():
    data = base_intro()
    data["b_ndebug"] = entry("if-release", "combo")
    data["strip"] = entry(True)
    for name in ("native_glfw", "native_sdl2", "native_sdl3"):
        data[name] = entry("disabled", "combo")
    return data


def diagnostic_intro():
    data = product_intro()
    data["warvk_internal_frame_recorder"] = entry(True)
    data["b_ndebug"] = entry("false", "combo")
    data["strip"] = entry(False)
    for name in ("native_glfw", "native_sdl2", "native_sdl3"):
        data[name] = entry("auto", "combo")
    return data


def changed(data, name, value):
    result = copy.deepcopy(data)
    result[name] = entry(value, result[name].get("type", "boolean"))
    return result
class ReleaseV122ConfigTests(unittest.TestCase):
    def test_product_profile_accepts_intended_matrix(self):
        self.assertEqual(v.validate_profile("player-release", product_intro()), [])

    def test_diagnostic_profile_accepts_internal_matrix(self):
        self.assertEqual(v.validate_profile("internal-diagnostic", diagnostic_intro()), [])

    def test_product_rejects_recorder_leak(self):
        errors = v.validate_profile("player-release", changed(product_intro(), "warvk_internal_frame_recorder", True))
        self.assertTrue(any("warvk_internal_frame_recorder" in error for error in errors))

    def test_product_rejects_blind_skin_disable(self):
        errors = v.validate_profile("player-release", changed(product_intro(), "warvk_skin_palette_contract_candidate", False))
        self.assertTrue(any("warvk_skin_palette_contract_candidate" in error for error in errors))

    def test_product_rejects_b_ndebug_false(self):
        errors = v.validate_profile("player-release", changed(product_intro(), "b_ndebug", "false"))
        self.assertTrue(any("b_ndebug" in error for error in errors))

    def test_product_rejects_dev_option_true(self):
        errors = v.validate_profile("player-release", changed(product_intro(), "warvk_shadow_observers_dev", True))
        self.assertTrue(any("warvk_shadow_observers_dev" in error for error in errors))
    def test_diagnostic_profile_requires_internal_recorder(self):
        errors = v.validate_profile("internal-diagnostic", changed(diagnostic_intro(), "warvk_internal_frame_recorder", False))
        self.assertTrue(any("warvk_internal_frame_recorder" in error for error in errors))

    def test_product_rejects_boolean_type_mismatch(self):
        errors = v.validate_profile("player-release", changed(product_intro(), "warvk_internal_frame_recorder", "false"))
        self.assertTrue(any("warvk_internal_frame_recorder" in error for error in errors))

    def test_parser_rejects_duplicate(self):
        text = "option('a', type : 'boolean', value : false)\noption('a', type : 'boolean', value : true)\n"
        with self.assertRaises(ValueError):
            v.parse_options(text)

    def test_parser_rejects_unparsed_declaration(self):
        text = "option('a', type : 'boolean', value : false)\noption('bad', type : 'boolean', value : maybe)\n"
        with self.assertRaises(ValueError):
            v.parse_options(text)
    def test_recipe_accepts_complete_matrix(self):
        text = "meson setup " + " ".join(v.PLAYER_RECIPE) + " build\n"
        self.assertEqual(v.check_recipe(text), [])

    def test_recipe_rejects_missing_flag(self):
        text = "meson setup " + " ".join(v.PLAYER_RECIPE) + " build\n"
        text = text.replace("-Dwarvk_skin_palette_contract_candidate=true", "")
        self.assertTrue(v.check_recipe(text))

    def test_intro_rejects_duplicate_json_key(self):
        with self.assertRaises(ValueError):
            v._load_intro_text('[{"name":"x","type":"boolean","value":false,"value":true}]')

    def test_intro_rejects_nonfinite_json_number(self):
        with self.assertRaises(ValueError):
            v._load_intro_text('[{"name":"x","type":"combo","value":NaN}]')

    def test_product_rejects_wrong_intro_type(self):
        data = copy.deepcopy(product_intro())
        data["warvk_internal_frame_recorder"]["type"] = "combo"
        errors = v.validate_profile("player-release", data, MESON_OPTIONS)
        self.assertTrue(any("intro type" in error for error in errors))

    def test_product_rejects_unknown_enabled_warvk_option(self):
        data = copy.deepcopy(product_intro())
        data["warvk_unknown_enabled"] = {"type": "boolean", "value": True}
        errors = v.validate_profile("player-release", data, MESON_OPTIONS)
        self.assertTrue(any("unknown enabled warvk option" in error for error in errors))

    def test_recipe_rejects_tokens_only_in_comment(self):
        text = "meson setup\n# " + " ".join(v.PLAYER_RECIPE) + "\n"
        self.assertTrue(v.check_recipe(text))

    def test_recipe_rejects_contradiction(self):
        text = "meson setup " + " ".join(v.PLAYER_RECIPE) + " -Dwarvk_internal_frame_recorder=true\n"
        self.assertTrue(v.check_recipe(text))

    def test_actual_source_defaults_are_preserved(self):
        self.assertEqual(v.check_source(ROOT), [])

    def test_actual_version_sources_are_consistent(self):
        expected = (ROOT / "RELEASE").read_text(encoding="utf-8").strip()
        self.assertEqual(v.check_version(ROOT, expected), [])


if __name__ == "__main__":
    unittest.main()
