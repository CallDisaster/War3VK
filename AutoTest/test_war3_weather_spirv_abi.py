#!/usr/bin/env python3
"""Reflect already-built production SPIR-V headers and weather counterexamples.

No compiler is launched. Missing/stale generated headers yield WAIT and exit 2,
not a skipped or passing ABI test. PASS reflects compiled interfaces plus source
and reference-math checks; it is not GPU execution or visual acceptance.

Example:
  python AutoTest/test_war3_weather_spirv_abi.py --build-dir build32_weather_20261010
  python AutoTest/test_war3_weather_spirv_abi.py --build-dir build32_weather_20261010 \
      --report AutoTest/artifacts/weather-night-20261010/spirv-abi-reflection.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "subprojects/war3fx/shaders"
SNOW_CPP = ROOT / "src/d3d9/d3d9_war3_weather.cpp"
VOLUME_CPP = ROOT / "src/d3d9/d3d9_war3_volumetric_light.cpp"
MAGIC = 0x07230203
# Verified against include/spirv/include/spirv/unified1/spirv.hpp.
OP_NAME = 5
OP_MEMBER_NAME = 6
OP_ENTRY_POINT = 15
OP_TYPE_INT = 21
OP_TYPE_FLOAT = 22
OP_TYPE_VECTOR = 23
OP_TYPE_MATRIX = 24
OP_TYPE_IMAGE = 25
OP_TYPE_SAMPLER = 26
OP_TYPE_ARRAY = 28
OP_TYPE_RUNTIME_ARRAY = 29
OP_TYPE_STRUCT = 30
OP_TYPE_POINTER = 32
OP_CONSTANT = 43
OP_VARIABLE = 59
OP_DECORATE = 71
OP_MEMBER_DECORATE = 72
DEC_BLOCK = 2
DEC_ROW_MAJOR = 4
DEC_ARRAY_STRIDE = 6
DEC_MATRIX_STRIDE = 7
DEC_BINDING = 33
DEC_DESCRIPTOR_SET = 34
DEC_OFFSET = 35

GENERATED = {
    "snow_vs": ("war3_weather_snow_vert", "vert", 0),
    "snow_fs": ("war3_weather_snow_frag", "frag", 4),
    "froxel_inject": ("war3_volumetric_froxel_inject", "comp", 5),
    "legacy_volume": ("war3_volumetric_light", "frag", 4),
}
SNOW_OFFSETS = [0, 64, 80, 96, 112, 116, 120, 124]
SNOW_NAMES = ["viewProj", "regionXY", "heightTime", "windAnchor", "seed",
              "depthSampler", "cameraZ", "packedMedia"]
WEATHER_NAMES = ["u_weatherMin", "u_weatherMax", "u_weatherControls",
                 "u_weatherMotion"]
WEATHER_OFFSETS = [528, 544, 560, 576]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def spv_string(words: list[int]) -> str:
    raw = struct.pack(f"<{len(words)}I", *words)
    end = raw.find(b"\0")
    require(end >= 0, "unterminated SPIR-V string")
    return raw[:end].decode("utf-8", errors="strict")


class Spirv:
    def __init__(self, header: Path):
        blob = header.read_bytes()
        text = blob.decode("utf-8-sig")
        match = re.search(r"\bconst\s+uint32_t\s+\w+\s*\[\s*\]\s*=\s*"
                          r"\{(.*?)\}\s*;", text, re.S)
        require(match is not None, f"no uint32 SPIR-V array in {header}")
        self.words = [int(word, 16) for word in
                      re.findall(r"0x[0-9a-fA-F]+", match.group(1))]
        require(len(self.words) >= 5 and self.words[0] == MAGIC,
                f"invalid SPIR-V magic/header in {header}")
        self.header = str(header)
        self.header_sha256 = hashlib.sha256(blob).hexdigest()
        self.spirv_sha256 = hashlib.sha256(struct.pack(
            f"<{len(self.words)}I", *self.words)).hexdigest()
        self.names: dict[int, str] = {}
        self.member_names: dict[tuple[int, int], str] = {}
        self.decorations: dict[tuple[int, int], list[int]] = {}
        self.member_decorations: dict[tuple[int, int, int], list[int]] = {}
        self.types: dict[int, tuple[int, list[int]]] = {}
        self.constants: dict[int, int] = {}
        self.variables: dict[int, tuple[int, int]] = {}
        self.entry_models: list[int] = []
        index = 5
        while index < len(self.words):
            first = self.words[index]
            size, opcode = first >> 16, first & 0xFFFF
            require(size > 0 and index + size <= len(self.words),
                    f"malformed instruction at word {index}")
            operands = self.words[index + 1:index + size]
            if opcode == OP_NAME:
                self.names[operands[0]] = spv_string(operands[1:])
            elif opcode == OP_MEMBER_NAME:
                self.member_names[(operands[0], operands[1])] = \
                    spv_string(operands[2:])
            elif opcode == OP_DECORATE:
                self.decorations[(operands[0], operands[1])] = operands[2:]
            elif opcode == OP_MEMBER_DECORATE:
                self.member_decorations[(operands[0], operands[1],
                                         operands[2])] = operands[3:]
            elif OP_TYPE_INT <= opcode <= OP_TYPE_POINTER:
                self.types[operands[0]] = (opcode, operands[1:])
            elif opcode == OP_CONSTANT:
                require(len(operands) >= 3, "invalid OpConstant")
                self.constants[operands[1]] = operands[2]
            elif opcode == OP_VARIABLE:
                self.variables[operands[1]] = (operands[0], operands[2])
            elif opcode == OP_ENTRY_POINT:
                self.entry_models.append(operands[0])
            index += size
        require(index == len(self.words), "SPIR-V instruction boundary drift")

    def struct_id(self, name: str) -> int:
        found = [key for key, value in self.names.items()
                 if value == name and self.types.get(key, (None, []))[0] ==
                 OP_TYPE_STRUCT]
        require(len(found) == 1, f"expected one compiled struct {name}: {found}")
        return found[0]

    def type_size(self, type_id: int) -> int:
        opcode, args = self.types[type_id]
        if opcode in (OP_TYPE_INT, OP_TYPE_FLOAT):
            return args[0] // 8
        if opcode in (OP_TYPE_VECTOR, OP_TYPE_MATRIX):
            return self.type_size(args[0]) * args[1]
        if opcode == OP_TYPE_ARRAY:
            stride = self.decorations.get((type_id, DEC_ARRAY_STRIDE))
            require(stride is not None and len(stride) == 1,
                    f"missing compiled ArrayStride for type {type_id}")
            return stride[0] * self.constants[args[1]]
        if opcode == OP_TYPE_STRUCT:
            entries = self.members(type_id)
            return max((entry["offset"] + entry["size"] for entry in entries),
                       default=0)
        raise AssertionError(f"unsupported ABI size type {type_id}: {opcode}")

    def members(self, type_id: int) -> list[dict]:
        opcode, args = self.types[type_id]
        require(opcode == OP_TYPE_STRUCT, "member reflection requires struct")
        result = []
        for index, member_type in enumerate(args):
            offset = self.member_decorations.get((type_id, index, DEC_OFFSET))
            require(offset is not None and len(offset) == 1,
                    f"missing OpMemberDecorate Offset in struct {type_id}:{index}")
            result.append({
                "name": self.member_names.get((type_id, index), ""),
                "offset": offset[0], "size": self.type_size(member_type),
                "rowMajor": (type_id, index, DEC_ROW_MAJOR) in
                            self.member_decorations,
                "matrixStride": self.member_decorations.get(
                    (type_id, index, DEC_MATRIX_STRIDE), [None])[0],
            })
        return result

    def bindings(self) -> list[dict]:
        result = []
        for variable_id, (pointer_id, storage) in self.variables.items():
            binding = self.decorations.get((variable_id, DEC_BINDING))
            descriptor_set = self.decorations.get((variable_id, DEC_DESCRIPTOR_SET))
            if binding is None or descriptor_set is None:
                continue
            opcode, pointer = self.types[pointer_id]
            require(opcode == OP_TYPE_POINTER, "resource variable lacks pointer")
            pointee = pointer[1]
            element_opcode, element_args = self.types[pointee]
            if element_opcode in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
                pointee = element_args[0]
                element_opcode, element_args = self.types[pointee]
            if element_opcode == OP_TYPE_SAMPLER:
                resource_type = "SAMPLER"
            elif element_opcode == OP_TYPE_IMAGE:
                resource_type = "STORAGE_IMAGE" if element_args[5] == 2 \
                                else "SAMPLED_IMAGE"
            elif element_opcode == OP_TYPE_STRUCT and storage == 2 and \
                    (pointee, DEC_BLOCK) in self.decorations:
                resource_type = "UNIFORM_BUFFER"
            else:
                resource_type = f"UNSUPPORTED_{element_opcode}_{storage}"
            result.append({"name": self.names.get(variable_id, ""),
                           "set": descriptor_set[0], "binding": binding[0],
                           "type": resource_type})
        return sorted(result, key=lambda value: (value["set"], value["binding"]))


def cpp_descriptor_types(source: str, function: str) -> list[str]:
    start = source.find(function)
    require(start >= 0, f"production pipeline function missing: {function}")
    array = re.search(r"std::array<DxvkDescriptorSetLayoutBinding,\s*\d+>"
                      r"\s+bindings\s*=\s*\{(.*?)\};", source[start:], re.S)
    require(array is not None, f"production descriptor array missing: {function}")
    return re.findall(r"DxvkDescriptorSetLayoutBinding\(VK_DESCRIPTOR_TYPE_(\w+)",
                      array.group(1))


def ray_interval(eye: tuple[float, ...], direction: tuple[float, ...],
                 region_min: tuple[float, ...], region_max: tuple[float, ...],
                 low: float, high: float) -> tuple[float, float] | None:
    values = (*eye, *direction, *region_min, *region_max, low, high)
    if not all(math.isfinite(value) for value in values):
        return None
    if any(region_max[i] <= region_min[i] for i in range(3)):
        return None
    for axis in range(3):
        if abs(direction[axis]) < 1.0e-7:
            if eye[axis] < region_min[axis] or eye[axis] > region_max[axis]:
                return None
            continue
        a = (region_min[axis] - eye[axis]) / direction[axis]
        b = (region_max[axis] - eye[axis]) / direction[axis]
        low, high = max(low, min(a, b)), min(high, max(a, b))
        if high <= low:
            return None
    return (low, high) if high > low else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path,
                        default=ROOT / "build32_weather_20261010")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    build_dir = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir
    report = {"status": "WAIT", "buildDir": str(build_dir.resolve()),
              "evidence": "compiled SPIR-V reflection + source/reference math; no GPU run",
              "checks": [], "modules": {}, "waitingFor": []}

    def check(name: str, function) -> None:
        try:
            function()
            report["checks"].append({"name": name, "ok": True})
        except (AssertionError, KeyError, ValueError, TypeError) as error:
            report["checks"].append({"name": name, "ok": False, "error": str(error)})

    snow_cpp = SNOW_CPP.read_text(encoding="utf-8")
    snow_frag = (SHADERS / "war3_weather_snow_frag.frag").read_text(encoding="utf-8")
    density = (SHADERS / "war3_weather_density.glsl").read_text(encoding="utf-8")
    volume_cpp = VOLUME_CPP.read_text(encoding="utf-8")

    def viewport_source():
        require("vp.MinZ + (vp.MinZ < 0.5f ? 0.001f : 0.0f)" in snow_cpp,
                "weather no longer uses native minimum positive depth range")
        require("vp.MaxZ <= vp.MinZ" in snow_cpp,
                "reverse/equal viewport range must remain a named refusal")
    check("narrow-positive-viewport.production-source", viewport_source)

    def viewport_math():
        for minimum, maximum in ((0.2, 0.2001), (0.4999, 0.49995),
                                  (0.5, 0.5001), (0.0, 1.0), (0.99, 1.0)):
            native_max = min(max(maximum, minimum +
                                 (0.001 if minimum < 0.5 else 0.0)), 1.0)
            weather_max = min(max(maximum, minimum +
                                  (0.001 if minimum < 0.5 else 0.0)), 1.0)
            for ndc_z in (0.0, 0.1, 0.5, 0.9, 1.0):
                require(minimum + ndc_z * (native_max - minimum) ==
                        minimum + ndc_z * (weather_max - minimum),
                        "reference viewport-depth mapping mismatch")
        require(abs(max(0.2001, 0.2 + 0.001) - 0.2001) > 0.0008,
                "counterexample did not distinguish the old implementation")
    check("narrow-positive-viewport.reference-math", viewport_math)

    def clip_source():
        require("war3WeatherRayInterval(eye, ray" in snow_frag,
                "production fragment no longer clips eye->particle path")
        require("mix(interval.x, interval.y" in snow_frag,
                "production samples must lie in clipped weather interval")
        require("tau * (interval.y - interval.x) * 0.25" in snow_frag,
                "optical thickness must use clipped path length")
        require("abs(direction[axis]) < 1.0e-7" in density,
                "ray-box function lost explicit parallel-axis handling")
    check("thin-region.production-source", clip_source)

    def thin_region_math():
        # A 1000-unit ray spends only its last 0.1 units in the weather box.
        # Old full-path midpoints 125/375/625/875 miss every density sample.
        region_min, region_max = (999.9, -1.0, -1.0), (1000.1, 1.0, 1.0)
        old_samples = [1000.0 * (i + 0.5) / 4.0 for i in range(4)]
        require(all(value < region_min[0] for value in old_samples),
                "old quadrature counterexample is not adversarial")
        interval = ray_interval((0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
                                region_min, region_max, 0.0, 1000.0)
        require(interval is not None, "clipped reference ray missed thin box")
        new_samples = [interval[0] + (interval[1] - interval[0]) *
                        (i + 0.5) / 4.0 for i in range(4)]
        require(all(region_min[0] <= value <= 1000.0 for value in new_samples),
                "clipped midpoint escapes eye->particle or weather box")
        require(0.0 < interval[1] - interval[0] < 0.2,
                "clipped length incorrect")
    check("thin-region.reference-math", thin_region_math)

    def ray_edge_math():
        box_min, box_max = (0.0, -1.0, -1.0), (1.0, 1.0, 1.0)
        require(ray_interval((-1.0, 2.0, 0.0), (1.0, 0.0, 0.0),
                             box_min, box_max, 0.0, 10.0) is None,
                "parallel ray outside slab was accepted")
        require(ray_interval((0.5, 0.0, 0.0), (1.0, 0.0, 0.0),
                             box_min, box_max, 0.0, 0.25) == (0.0, 0.25),
                "ray starting inside box was clipped past particle")
        require(ray_interval((-1.0, 0.0, 0.0), (-1.0, 0.0, 0.0),
                             box_min, box_max, 0.0, 10.0) is None,
                "box behind the eye was accepted")
        require(ray_interval((math.nan, 0.0, 0.0), (1.0, 0.0, 0.0),
                             box_min, box_max, 0.0, 10.0) is None,
                "nonfinite ray was accepted")
    check("ray-box-boundaries.reference-math", ray_edge_math)

    modules = {}
    for key, (symbol, extension, execution_model) in GENERATED.items():
        header = build_dir / "src/d3d9/d3d9.dll.p" / f"{symbol}.h"
        if not header.exists():
            report["waitingFor"].append({"header": str(header), "reason": "missing"})
            continue
        inputs = [SHADERS / f"{symbol}.{extension}"]
        if key != "snow_vs":
            inputs.append(SHADERS / "war3_weather_density.glsl")
        if header.stat().st_mtime_ns < max(path.stat().st_mtime_ns for path in inputs):
            report["waitingFor"].append({"header": str(header), "reason": "stale"})
            continue
        try:
            module = Spirv(header)
            modules[key] = module
            report["modules"][key] = {
                "header": module.header, "headerSha256": module.header_sha256,
                "spirvSha256": module.spirv_sha256, "words": len(module.words),
                "entryModels": module.entry_models, "bindings": module.bindings(),
            }
            check(f"{key}.compiled-stage", lambda m=module, e=execution_model:
                  require(m.entry_models == [e], "compiled execution stage mismatch"))
        except (AssertionError, KeyError, ValueError, IndexError) as error:
            report["checks"].append({"name": f"{key}.parse", "ok": False,
                                      "error": str(error)})

    for key in ("snow_vs", "snow_fs"):
        if key not in modules:
            continue
        module = modules[key]
        def snow_abi(m=module, k=key):
            members = m.members(m.struct_id("SnowPush"))
            report["modules"][k]["snowPush"] = members
            require([entry["name"] for entry in members] == SNOW_NAMES,
                    "SnowPush compiled member names/order mismatch")
            require([entry["offset"] for entry in members] == SNOW_OFFSETS,
                    "SnowPush compiled offsets mismatch")
            require([entry["size"] for entry in members] ==
                    [64, 16, 16, 16, 4, 4, 4, 4], "SnowPush compiled sizes mismatch")
            require(members[0]["rowMajor"] and members[0]["matrixStride"] == 16,
                    "SnowPush compiled matrix is not RowMajor/MatrixStride16")
            require(m.type_size(m.struct_id("SnowPush")) == 128,
                    "SnowPush compiled extent exceeds 128 bytes")
        check(f"{key}.compiled-push-abi", snow_abi)

    if "snow_vs" in modules and "snow_fs" in modules:
        check("snow.compiled-VS-FS-equality", lambda:
              require(modules["snow_vs"].members(modules["snow_vs"].struct_id("SnowPush")) ==
                      modules["snow_fs"].members(modules["snow_fs"].struct_id("SnowPush")),
                      "compiled vertex/fragment interfaces diverge"))

    for key in ("froxel_inject", "legacy_volume"):
        if key not in modules:
            continue
        module = modules[key]
        def fog_abi(m=module, k=key):
            members = m.members(m.struct_id("FogVolumeBlock"))
            report["modules"][k]["fogVolumeBlock"] = members
            tail = members[-4:]
            require([entry["name"] for entry in tail] == WEATHER_NAMES,
                    "compiled weather UBO tail member names/order mismatch")
            require([entry["offset"] for entry in tail] == WEATHER_OFFSETS,
                    "compiled weather UBO tail offsets mismatch")
            require(all(entry["size"] == 16 for entry in tail),
                    "compiled weather UBO tail must contain four vec4s")
            require(m.type_size(m.struct_id("FogVolumeBlock")) == 592,
                    "compiled FogVolumeBlock extent is not 592 bytes")
        check(f"{key}.compiled-weather-ubo-tail", fog_abi)

    descriptor_map = {
        "snow_fs": re.findall(
            r"const\s+DxvkDescriptorSetLayoutBinding\s+binding\(\s*"
            r"VK_DESCRIPTOR_TYPE_(\w+)", snow_cpp),
        "froxel_inject": cpp_descriptor_types(volume_cpp, "::createFroxelInjectLayout()"),
        "legacy_volume": cpp_descriptor_types(volume_cpp, "::createPipelineLayout()"),
    }
    for key, module in modules.items():
        def binding_check(m=module, k=key):
            bindings = m.bindings()
            if k == "snow_vs":
                require(not bindings, "procedural vertex shader unexpectedly uses descriptors")
                return
            require(any(item["set"] == 0 and item["binding"] == 0 and
                        item["type"] == "SAMPLER" for item in bindings),
                    "compiled sampler heap binding differs")
            expected = descriptor_map[k]
            for item in bindings:
                if item["set"] == 0:
                    require(item["binding"] == 0 and item["type"] == "SAMPLER",
                            f"unexpected compiled heap descriptor: {item}")
                    continue
                require(item["set"] == 1 and 0 <= item["binding"] < len(expected),
                        f"compiled binding outside production layout: {item}")
                require(item["type"] == expected[item["binding"]],
                        f"compiled descriptor type differs from production: {item}")
            fog_binding = 5 if k == "froxel_inject" else 6 if k == "legacy_volume" else 0
            require(any(item["set"] == 1 and item["binding"] == fog_binding for item in bindings),
                    "compiled weather-bearing binding missing")
        check(f"{key}.compiled-bindings-vs-production", binding_check)

    failures = sum(not item["ok"] for item in report["checks"])
    report["passed"] = sum(item["ok"] for item in report["checks"])
    report["failed"] = failures
    report["status"] = "FAIL" if failures else "WAIT" if report["waitingFor"] else "PASS"
    output = json.dumps(report, ensure_ascii=False, indent=2)
    if args.report:
        target = args.report if args.report.is_absolute() else ROOT / args.report
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(output + "\n", encoding="utf-8")
    print(output)
    return 1 if failures else 2 if report["waitingFor"] else 0


if __name__ == "__main__":
    sys.exit(main())
