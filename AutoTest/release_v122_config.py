#!/usr/bin/env python3
"""Offline strict verifier for the WarVK v1.22 product configuration."""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

DEFAULT_VERSION = "1.22.00"
OPTION_RE = re.compile(
    r"option\s*\(\s*'(?P<name>[^']+)'\s*,\s*type\s*:\s*'(?P<type>[^']+)'\s*,\s*"
    r"value\s*:\s*(?P<raw>true|false|'(?:[^']*)')",
    re.MULTILINE,
)
DIAGNOSTIC_OPTIONS = (
    "warvk_data_collection_tree_dev",
    "warvk_internal_frame_recorder",
    "warvk_skin_palette_contract_candidate",
    "warvk_shadow_observers_dev",
    "warvk_rts_shadow_candidate_dev",
    "warvk_coherent_up_index_trim_dev",
    "warvk_current_up_shadow_replay_dev",
    "warvk_coherent_real_index_trim_dev",
    "warvk_coherent_real_perf_candidate_dev",
    "warvk_device_address_binding_report_dev",
)
PRODUCT_OPTIONS = {
    "warvk_data_collection_tree_dev": False,
    "warvk_internal_frame_recorder": False,
    "warvk_skin_palette_contract_candidate": True,
    "warvk_shadow_observers_dev": False,
    "warvk_rts_shadow_candidate_dev": False,
    "warvk_coherent_up_index_trim_dev": False,
    "warvk_current_up_shadow_replay_dev": False,
    "warvk_coherent_real_index_trim_dev": False,
    "warvk_coherent_real_perf_candidate_dev": False,
    "warvk_device_address_binding_report_dev": False,
}
DIAGNOSTIC_PROFILE_OPTIONS = dict(PRODUCT_OPTIONS)
DIAGNOSTIC_PROFILE_OPTIONS["warvk_internal_frame_recorder"] = True
ENABLE = {
    "enable_dxgi": False,
    "enable_d3d8": False,
    "enable_d3d9": True,
    "enable_d3d10": False,
    "enable_d3d11": False,
}
BUILTIN_TYPES = {
    "buildtype": "combo",
    "b_ndebug": "combo",
    "strip": "boolean",
    "debug": "boolean",
    "optimization": "combo",
    "build_id": "boolean",
}
DECLARED_TO_INTRO = {
    "boolean": "boolean",
    "combo": "combo",
    "string": "string",
    "feature": "combo",
}
EXPECTED_INTRO_TYPES = {
    **BUILTIN_TYPES,
    **{name: "boolean" for name in ENABLE},
    "native_glfw": "combo",
    "native_sdl2": "combo",
    "native_sdl3": "combo",
    **{name: "boolean" for name in PRODUCT_OPTIONS},
}
PROFILES = {
    "player-release": {
        "buildtype": "release",
        "b_ndebug": ("if-release", "true"),
        "strip": True,
        "debug": False,
        "optimization": "3",
        "build_id": False,
        "enable": ENABLE,
        "features": {
            "native_glfw": "disabled",
            "native_sdl2": "disabled",
            "native_sdl3": "disabled",
        },
        "options": PRODUCT_OPTIONS,
    },
    "internal-diagnostic": {
        "buildtype": "release",
        "b_ndebug": ("false",),
        "strip": False,
        "debug": False,
        "optimization": "3",
        "build_id": False,
        "enable": ENABLE,
        "features": {"native_glfw": "auto", "native_sdl2": "auto", "native_sdl3": "auto"},
        "options": DIAGNOSTIC_PROFILE_OPTIONS,
    },
}
PLAYER_RECIPE = (
    "-Db_ndebug=if-release",
    "-Denable_dxgi=false",
    "-Denable_d3d8=false",
    "-Denable_d3d9=true",
    "-Denable_d3d10=false",
    "-Denable_d3d11=false",
    "-Dnative_glfw=disabled",
    "-Dnative_sdl2=disabled",
    "-Dnative_sdl3=disabled",
    "-Dwarvk_internal_frame_recorder=false",
    "-Dwarvk_skin_palette_contract_candidate=true",
    "-Dwarvk_data_collection_tree_dev=false",
    "-Dwarvk_shadow_observers_dev=false",
    "-Dwarvk_rts_shadow_candidate_dev=false",
    "-Dwarvk_coherent_up_index_trim_dev=false",
    "-Dwarvk_current_up_shadow_replay_dev=false",
    "-Dwarvk_coherent_real_index_trim_dev=false",
    "-Dwarvk_coherent_real_perf_candidate_dev=false",
    "-Dwarvk_device_address_binding_report_dev=false",
)
def read(root: Path, rel: str) -> str:
    return (root / rel).read_text(encoding="utf-8")


def parse_options(text: str) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for match in OPTION_RE.finditer(text):
        raw = match.group("raw")
        if raw == "true":
            value: object = True
        elif raw == "false":
            value = False
        elif raw.startswith("'"):
            value = raw[1:-1]
        else:
            raise ValueError(f"unsupported option value: {raw}")
        name = match.group("name")
        if name in result:
            raise ValueError(f"duplicate option {name}")
        result[name] = {"type": match.group("type"), "value": value}
    if len(result) != len(re.findall(r"\boption\s*\(", text)):
        raise ValueError("unparsed option declaration")
    return result


def _reject_constant(name: str) -> object:
    raise ValueError(f"non-finite JSON constant: {name}")


def _parse_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"non-finite JSON number: {text}")
    return value


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _load_intro_text(text: str) -> dict[str, dict[str, object]]:
    data = json.loads(
        text,
        object_pairs_hook=_unique_object,
        parse_constant=_reject_constant,
        parse_float=_parse_float,
    )
    if not isinstance(data, list):
        raise ValueError("intro-buildoptions.json is not an array")
    result: dict[str, dict[str, object]] = {}
    for item in data:
        if not isinstance(item, dict):
            raise ValueError("intro option is not an object")
        name = item.get("name")
        if not isinstance(name, str):
            raise ValueError("intro option without name")
        if ":" in name:
            continue
        if name in result:
            raise ValueError(f"duplicate intro option {name}")
        result[name] = item
    return result


def load_intro(path: Path) -> dict[str, dict[str, object]]:
    return _load_intro_text(path.read_text(encoding="utf-8"))


def _value_type_ok(value: object, intro_type: str) -> bool:
    if intro_type == "boolean":
        return isinstance(value, bool)
    if intro_type in ("combo", "string"):
        return isinstance(value, str)
    return False


def _expected_intro_type(name: str, meson_options) -> str | None:
    if name in BUILTIN_TYPES:
        return BUILTIN_TYPES[name]
    if meson_options is not None and name in meson_options:
        declared = meson_options[name]["type"]
        return DECLARED_TO_INTRO.get(declared, declared)
    return EXPECTED_INTRO_TYPES.get(name)


def validate_profile(profile: str, intro: dict[str, dict[str, object]], meson_options=None) -> list[str]:
    expected = PROFILES[profile]
    expected_names = {"buildtype", "b_ndebug", "strip", "debug", "optimization", "build_id"}
    expected_names.update(expected["options"])
    expected_names.update(expected["enable"])
    expected_names.update(expected["features"])
    wanted_values = {
        key: expected[key]
        for key in ("buildtype", "strip", "debug", "optimization", "build_id")
    }
    wanted_values.update(expected["options"])
    wanted_values.update(expected["enable"])
    wanted_values.update(expected["features"])
    errors: list[str] = []
    for name in sorted(expected_names):
        item = intro.get(name)
        if not isinstance(item, dict):
            errors.append(f"{name}: missing or non-dict intro item")
            continue
        actual_type = item.get("type")
        value = item.get("value")
        wanted_type = _expected_intro_type(name, meson_options)
        if wanted_type is None:
            errors.append(f"{name}: no declared Meson type")
            continue
        if actual_type != wanted_type:
            errors.append(f"{name}: intro type {actual_type!r}, expected {wanted_type!r}")
        if not _value_type_ok(value, wanted_type):
            errors.append(f"{name}: value {value!r} has wrong Python type")
        if name == "b_ndebug":
            if value not in expected["b_ndebug"]:
                errors.append(f"b_ndebug not in {expected['b_ndebug']!r}")
        elif name in wanted_values:
            wanted = wanted_values[name]
            if isinstance(wanted, bool):
                if value is not wanted:
                    errors.append(f"{name}: expected {wanted!r}, got {value!r}")
            elif value != wanted:
                errors.append(f"{name}: expected {wanted!r}, got {value!r}")
    for name, item in intro.items():
        if name.startswith("warvk_") and name not in expected_names:
            if isinstance(item, dict) and item.get("value") is True:
                errors.append(f"unknown enabled warvk option: {name}")
    return errors
def guard_stack_proof(block: str, macro: str, allowed: tuple[str, ...]) -> bool:
    stack: list[str] = []
    needle = f"-D{macro}=1"
    seen = False
    for raw in block.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("if "):
            stack.append(line)
        elif line.startswith("elif "):
            if stack:
                stack[-1] = line
        elif line.startswith("else"):
            if stack:
                stack[-1] = ""
        elif line.startswith("endif"):
            if stack:
                stack.pop()
        if needle in line:
            seen = True
            if any(f"get_option('{option}')" in condition for condition in stack for option in allowed):
                return True
    return False


def check_source(root: Path) -> list[str]:
    errors: list[str] = []
    try:
        opts = parse_options(read(root, "meson_options.txt"))
    except Exception as exc:
        return [f"meson_options.txt: {exc}"]
    for name in DIAGNOSTIC_OPTIONS:
        if opts.get(name, {}).get("value") is not False:
            errors.append(f"{name} must default false")
    skin = read(root, "src/d3d9/war3/render/war3_skin_palette_selection.h")
    if not re.search(r"WARVK_SKIN_PALETTE_CONTRACT_DEFAULT\s+0\b", skin):
        errors.append("skin contract source fallback must be 0")
    d3 = read(root, "src/d3d9/meson.build")
    dx = read(root, "src/dxvk/meson.build")
    def meson_block(text: str, start: str, end: str) -> str:
        begin = text.index(start)
        return text[begin:text.index(end, begin)]

    d3_block = meson_block(d3, "d3d9_cpp_args = []", "d3d9_dll = shared_library")
    dx_block = meson_block(dx, "dxvk_cpp_args = []", "dxvk_lib = static_library")
    for name, expected_type in EXPECTED_INTRO_TYPES.items():
        if name in opts:
            declared = opts[name]["type"]
            actual = DECLARED_TO_INTRO.get(declared, declared)
            if actual != expected_type:
                errors.append(f"{name}: declared {declared} maps to {actual}, expected {expected_type}")
    guards = {
        "WARVK_SKIN_PALETTE_CONTRACT_DEFAULT": ("warvk_skin_palette_contract_candidate",),
        "WARVK_ENABLE_SHADOW_OBSERVERS_DEV": ("warvk_shadow_observers_dev",),
        "WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV": ("warvk_rts_shadow_candidate_dev",),
        "WARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV": ("warvk_coherent_up_index_trim_dev",),
        "WARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV": ("warvk_current_up_shadow_replay_dev",),
        "WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV": ("warvk_coherent_real_perf_candidate_dev", "warvk_coherent_real_index_trim_dev"),
        "WARVK_ENABLE_COHERENT_REAL_PERF_CANDIDATE_DEV": ("warvk_coherent_real_perf_candidate_dev",),
        "WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV": ("warvk_device_address_binding_report_dev",),
        "WARVK_DATA_COLLECTION_TREE_DEV": ("warvk_data_collection_tree_dev",),
    }
    for macro, allowed in guards.items():
        if not guard_stack_proof(d3_block, macro, allowed):
            errors.append(f"{macro} lacks structural option guard {allowed}")
    if not guard_stack_proof(dx_block, "WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV", ("warvk_device_address_binding_report_dev",)):
        errors.append("WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV lacks dxvk structural guard")
    if not re.search(
        r"frame_recorder_build\.set\(\s*'WARVK_INTERNAL_FRAME_RECORDER_DEFAULT'\s*,\s*"
        r"get_option\('warvk_internal_frame_recorder'\)\s*\?\s*1\s*:\s*0\s*\)",
        d3,
        re.S,
    ):
        errors.append("recorder build default is not tied to warvk_internal_frame_recorder")
    sync = read(root, "src/d3d9/war3/hooks/war3_native_capture.cpp")
    for token in ("DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT", "DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE"):
        if token not in sync:
            errors.append(f"native sync lacks {token}")
    if "DXVK_WAR3_PERF_" in sync:
        errors.append("native sync must be independent of perf recording")
    shot = read(root, "src/d3d9/war3/tools/war3_async_screenshot.cpp")
    if 'DXVK_WAR3_ASYNC_SCREENSHOT' not in shot:
        errors.append("screenshot env missing")
    if 'return !v || std::strcmp(v, "1") == 0;' not in shot:
        errors.append("screenshot default must be enabled")
    api = read(root, "src/d3d9/war3/tools/war3_internal_test_api.cpp")
    cfg = read(root, "src/d3d9/war3/core/war3_internal_test_config.h")
    if "DXVK_WAR3_INTERNAL_TEST_API" not in api or "kNativeInternalTestApiEnabled = false" not in cfg:
        errors.append("internal test API must default disabled")
    light = read(root, "src/d3d9/war3/model/war3_native_light_bridge.cpp")
    consumer = read(root, "src/d3d9/d3d9_native_light.cpp")
    if 'DXVK_WAR3_NATIVE_MODEL_LIGHTS") != "1"' not in light:
        errors.append("native auto-light install must be explicit env opt-in")
    if 'DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER") == "1"' not in consumer:
        errors.append("native auto-light consumer must be explicit env opt-in")
    cross = read(root, "build-win32.txt")
    if "cpu_family = 'x86'" not in cross or "cpu = 'i686'" not in cross:
        errors.append("product cross file must be 32-bit x86")
    return errors
def check_version(root: Path, version: str) -> list[str]:
    errors: list[str] = []
    meson = read(root, "meson.build")
    release = read(root, "RELEASE").strip()
    version_h = read(root, "version.h")
    if not re.search(rf"version\s*:\s*'{re.escape(version)}'", meson):
        errors.append("meson.build version mismatch")
    if release != version or f'#define DXVK_VERSION "{version}"' not in version_h:
        errors.append("RELEASE/version.h mismatch")
    rc = read(root, "src/d3d9/version.rc")
    parts = [str(int(part)) for part in version.split(".")] + ["0", "0"]
    numeric = ",".join(parts[:4])
    if not re.search(rf"FILEVERSION\s+{re.escape(numeric)}\b", rc):
        errors.append("version.rc FILEVERSION mismatch")
    japi = read(root, "src/d3d9/war3/japi/war3_japi_v1.cpp")
    if f'kApiVersion = "WarVK JAPI {version}"' not in japi:
        errors.append("JAPI display version mismatch")
    if 'kCanonicalVersion = "v1"' not in japi:
        errors.append("JASS wire ABI changed")
    bridge = read(root, "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp")
    if f'result.publicVersionText.find("WarVK JAPI {version}")' not in bridge:
        errors.append("JASS runtime probe version mismatch")
    shader = read(root, "src/d3d9/war3_shader_api.h")
    if not all(x in shader for x in ("API_VERSION_MAJOR = 1", "API_VERSION_MINOR = 2", "API_VERSION_PATCH = 0")):
        errors.append("shader API version changed")
    return errors
def check_recipe(text: str) -> list[str]:
    start = text.find("meson setup")
    if start < 0:
        return ["recipe lacks meson setup"]
    block = text[start:start + 4000]
    active = "\n".join(
        line for line in block.splitlines() if not line.strip().startswith("#")
    )
    errors = [f"recipe lacks {token}" for token in PLAYER_RECIPE if token not in active]
    for forbidden in ("-Dwarvk_internal_frame_recorder=true",
                      "-Dwarvk_skin_palette_contract_candidate=false"):
        if forbidden in active:
            errors.append(f"recipe contains {forbidden}")
    return errors
def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--profile", choices=tuple(PROFILES), required=True)
    parser.add_argument("--intro", type=Path)
    parser.add_argument("--expected-version", default=DEFAULT_VERSION)
    parser.add_argument("--recipe", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--dump-profile", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.dump_profile:
        print(json.dumps({
            "profile": args.profile,
            "options": PROFILES[args.profile],
            "recipeFlags": list(PLAYER_RECIPE) if args.profile == "player-release" else [],
        }, ensure_ascii=False, indent=2))
        return 0
    if args.intro is None:
        parser.error("--intro is required unless --dump-profile is used")
    errors = check_source(root) + check_version(root, args.expected_version)
    try:
        errors += validate_profile(
            args.profile,
            load_intro(args.intro.resolve()),
            parse_options(read(root, "meson_options.txt")),
        )
    except Exception as exc:
        errors.append(str(exc))
    if args.recipe:
        errors += check_recipe(args.recipe.read_text(encoding="utf-8"))
    if args.json:
        print(json.dumps({"ok": not errors, "errors": errors}, ensure_ascii=False, indent=2))
    else:
        print(f"profile={args.profile} ok={not errors}")
        for error in errors:
            print(f"ERROR: {error}")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
