#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M2-5B 等价证据门禁（fail-closed）：M2-5 余项 B4 的宿主差分 + 单一实现钉死。

覆盖对象：dxvk::war3::semantic::War3AggregateSubmittedSkinnedPalette
（pre-M2-5B d3d9_device.cpp :22200-22244 的 45 行，逐字节迁入
src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp}）。

本门禁自己复算：
  * legacy .inc 的 SHA/字节数与 provenance 文本；
  * 模块 .cpp 的 BEGIN/END 标记之间正文 vs .inc 的 BEGIN/END 之间正文（逐字节）；
  * 若 %TEMP%/device_pre_m2_5b.cpp 在位，则再从快照独立抽取块并与 .inc 正文比对，
    同时用预算门禁**同一** DEF_RE（从其源码中 eval）实测 baseline；
  * 7 个 stats 字段在 device.cpp(0) / scene.h(1) / 模块(逐名次数) 的出现次数；
  * 动态：4 组 env 下差分测试的 "M2-5B battery: N checks, 0 failures" 精确值 +
    全文件 N/N，以及 --probe-m2-5b 逐行与门禁内**独立实现**的 FNV-1a 推导对照。
"""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BUDGET_GATE = ROOT / "AutoTest/test_device_semantic_responsibility_budget_static.py"
GENERATOR = ROOT / "AutoTest/gen_war3_live_palette_selection_legacy_reference.py"
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_palette_submitted_aggregation.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_palette_submitted_aggregation.cpp"
INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_palette_submitted_aggregation_legacy_reference.inc"
)
TEST_CPP = (
    ROOT / "src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp"
)
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
SCENE_H = ROOT / "src/d3d9/d3d9_war3_scene.h"
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = ROOT / "docs/plan/2026-09-18-m2-5-remainder-record.md"
SNAPSHOT = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_5b.cpp"

# pre-M2-5B 工作树身份（= M2-4 记录的"迁移后 device.cpp"，交叉验证闭合）。
PRE_M2_5B_DEVICE_SHA256 = (
    "D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811"
)
PRE_M2_5B_DEVICE_SIZE = 2_250_786
PRE_M2_5B_CAPTURED_AT = "2026-09-18T04:15:05"

INC_SHA256 = (
    "C6D32B4EF8DCCC27A50EE407037687F190CD3D72FC90FE053EB4A49D7E3EE46F"
)
INC_SIZE = 4_952
MODULE_CPP_SHA256 = (
    "83A241506B111B99CBA49E30E8228D48796E2004BFA745362EDCD3F1A993858D"
)
MODULE_H_SHA256 = (
    "3907893315D33A0D9DB0CB2F201D63BA76D804A92E6D27A005F793557B4E1E6A"
)

BLOCK_START_LINE = 22200
BLOCK_END_LINE = 22244
LOCATE = (
    "      // Phase 7.48：per-frame submitted skinned palette 聚合"
    "（只对 skinned 生效）。"
)
BEGIN_MARK = (
    "    // --- BEGIN M2-5B verbatim pre-migration aggregation block"
    " (d3d9_device.cpp :22200-22244) ---"
)
END_MARK = "    // --- END M2-5B verbatim pre-migration aggregation block ---"

COVERED = ("War3AggregateSubmittedSkinnedPalette",)
SYMBOL = COVERED[0]

FIELD_NAMES = [
    "semanticSceneSubmittedSkinnedPaletteZeroHashCount",
    "semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash",
    "semanticSceneSubmittedSkinnedPaletteCombinedHash",
    "semanticSceneSubmittedSkinnedPaletteDistinctSampleCount",
    "semanticSceneSubmittedSkinnedPaletteRunningLastHash",
    "semanticSceneSubmittedSkinnedPaletteRunningSameHashRun",
    "semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax",
]
# 模块 .cpp 内每个字段名的出现次数（逐名实测；改一处即红）。
FIELD_MODULE_COUNTS = {
    "semanticSceneSubmittedSkinnedPaletteZeroHashCount": 1,
    "semanticSceneSubmittedSkinnedPaletteFirstSubmittedHash": 2,
    "semanticSceneSubmittedSkinnedPaletteCombinedHash": 4,
    "semanticSceneSubmittedSkinnedPaletteDistinctSampleCount": 2,
    "semanticSceneSubmittedSkinnedPaletteRunningLastHash": 3,
    "semanticSceneSubmittedSkinnedPaletteRunningSameHashRun": 5,
    "semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax": 3,
}

PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"

# 差分测试自身打印的 M2-5B 电池检查数（精确值 + 下限双重咬合）。
EXPECTED_BATTERY_CHECKS = 1_347_808
MIN_BATTERY_CHECKS = 1_200_000
MIN_TOTAL_CHECKS = 30_000_000

BATTERY_RE = re.compile(r"M2-5B battery:\s*(\d+) checks,\s*(\d+) failures")
CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)
PROBE_RE = re.compile(
    r"probe-m2-5b\s+(\S+)\s+module=([0-9,]+)\s+legacy=([0-9,]+)\s*$"
)

# --probe-m2-5b 场景表（与测试 .cpp 的 kM25bProbes 一一对应）：
# (name, (lastSubmitted, first, combined, runLast, run, distinct, max, zero), skinned)
PROBE_SPECS = [
    ("probe-none", (0, 0, 0, 0, 0, 0, 0, 0), False),
    ("probe-fresh-zero", (0, 0, 0, 0, 0, 0, 0, 0), True),
    ("probe-fresh-nonzero", (0x1111, 0, 0, 0, 0, 0, 0, 0), True),
    ("probe-seeded-repeat", (0x1111, 0x1111, 0x9999, 0x1111, 1, 1, 1, 0), True),
    ("probe-seeded-change", (0x2222, 0x1111, 0x9999, 0x1111, 3, 5, 7, 2), True),
    ("probe-max-growth", (0x1111, 0x1111, 0x9999, 0x1111, 3, 3, 3, 0), True),
    ("probe-max-hold", (0x1111, 0x1111, 0x9999, 0x1111, 2, 2, 7, 0), True),
    ("probe-first-set", (0x1111, 0x3333, 0, 0x1111, 0, 0, 0, 0), True),
    ("probe-zero-seeded", (0, 0x1111, 0x9999, 0x1111, 3, 4, 6, 1), True),
    (
        "probe-fnv-lo-hi",
        (0x0000000100000000, 0x1111, 0xCBF29CE484222325, 1, 2, 5, 5, 0),
        True,
    ),
    (
        "probe-huge-hash",
        (0xFFFFFFFFFFFFFFFF, 0x1111, 0x9E3779B97F4A7C15, 0xFFFFFFFFFFFFFFFF, 4, 9, 12, 3),
        True,
    ),
    ("probe-skinned-off", (0x1111, 0x1111, 0x9999, 0x1111, 3, 5, 7, 2), False),
]

MASK64 = (1 << 64) - 1
FNV_PRIME = 0x100000001B3


def fail(message):
    raise AssertionError(message)


def read(path):
    if not path.is_file():
        fail("fail-closed: 缺少必需文件 " + str(path))
    return path.read_text(encoding="utf-8")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest().upper()


def sanitized_env(overrides):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("DXVK_WAR3_")
    }
    env.update(overrides)
    return env


def extract_body(text):
    begin = text.index(BEGIN_MARK)
    begin_end = text.index("\n", begin) + 1
    end = text.index(END_MARK)
    return text[begin_end:end]


def fnv1a_iter(hash_value, value):
    """门禁内独立实现的 FNV-1a 步进（util_bit.h 的 fnv1a_iter 语义）。"""
    return ((hash_value ^ (value & MASK64)) * FNV_PRIME) & MASK64


def expected_row(seed, skinned):
    """按源码语义独立推导 7 个字段的期望值（不与 C++ 实现互相印证）。"""
    last, first, combined, run_last, run, distinct, maximum, zero = seed
    if not skinned:
        return (zero, first, combined, distinct, run_last, run, maximum)
    if last == 0:
        zero += 1
    if first == 0:
        first = last
    if combined == 0:
        combined = last if last != 0 else 0x9E3779B97F4A7C15
        distinct = 1
        run_last = last
        run = 1
        maximum = 1
    else:
        lo = last & 0xFFFFFFFF
        hi = (last >> 32) & 0xFFFFFFFF
        combined = fnv1a_iter(fnv1a_iter(combined, lo), hi)
        if last != run_last:
            distinct += 1
            run_last = last
            run = 1
        else:
            run += 1
            if run > maximum:
                maximum = run
    return (zero, first, combined, distinct, run_last, run, maximum)


def parse_frozen_m2_5b(text):
    start = text.index("FROZEN_M2_5B = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def budget_def_re(text):
    """从预算门禁源码里 eval 出同一个 DEF_RE（不是抄写）。"""
    start = text.index("DEF_RE = re.compile(")
    end = text.index("SKIP_NAMES = {")
    namespace = {"re": re}
    exec(text[start:end], namespace)
    return namespace["DEF_RE"]


def measure_snapshot_baseline(def_re):
    raw = SNAPSHOT.read_bytes()
    actual = sha256_bytes(raw)
    if actual != PRE_M2_5B_DEVICE_SHA256 or len(raw) != PRE_M2_5B_DEVICE_SIZE:
        fail(
            "pre-M2-5B 快照 %TEMP%/device_pre_m2_5b.cpp 身份与登记值不一致：\n"
            "  SHA-256 %s != %s\n  size %d != %d"
            % (actual, PRE_M2_5B_DEVICE_SHA256, len(raw), PRE_M2_5B_DEVICE_SIZE)
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if text.count(SYMBOL) != 0:
        fail("pre-M2-5B 快照已出现 " + SYMBOL + "（baseline 不再是 0）")
    sites = 0
    for line in text.splitlines():
        if not line or line[:1] in (" ", "\t"):
            continue
        if line.startswith(("//", "#", "/*", "*")):
            continue
        match = def_re.match(line)
        if match and match.group(1) == SYMBOL:
            sites += 1
    if sites != 0:
        fail("pre-M2-5B 快照中 " + SYMBOL + " 的行首站点 = %d（要求 0）" % sites)
    # 独立从快照抽块：花括号配对 + 行号 + 与 .inc 正文逐字节比对。
    index = text.index(LOCATE)
    brace = text.index("{", index)
    depth = 0
    end = None
    for scan in range(brace, len(text)):
        char = text[scan]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                end = scan
                break
    if end is None:
        fail("fail-closed: 快照 B4 块花括号不配对")
    line_start = text.rfind("\n", 0, brace) + 1
    start_line = text.count("\n", 0, line_start) + 1
    end_line = text.count("\n", 0, end) + 1
    if (start_line, end_line) != (BLOCK_START_LINE, BLOCK_END_LINE):
        fail(
            "fail-closed: 快照 B4 块行号 %d-%d != %d-%d"
            % (start_line, end_line, BLOCK_START_LINE, BLOCK_END_LINE)
        )
    return text[line_start:end + 1]


def run_probe(scenario, overrides):
    proc = subprocess.run(
        [str(TEST_EXE), "--probe-m2-5b"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        fail(
            "--probe-m2-5b 场景 %s exit=%d\nstdout:\n%s\nstderr:\n%s"
            % (scenario, proc.returncode, proc.stdout, proc.stderr)
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = PROBE_RE.match(line)
        if not match:
            continue
        name, module_fields, legacy_fields = match.groups()
        observed[name] = (
            [int(part) for part in module_fields.split(",")],
            [int(part) for part in legacy_fields.split(",")],
        )
    if set(observed) != {spec[0] for spec in PROBE_SPECS}:
        fail(
            "--probe-m2-5b 场景集合与门禁登记不一致：\n  observed: %s\n  expected: %s"
            % (sorted(observed), sorted(spec[0] for spec in PROBE_SPECS))
        )
    for name, seed, skinned in PROBE_SPECS:
        module_fields, legacy_fields = observed[name]
        want = list(expected_row(seed, skinned))
        if module_fields != legacy_fields:
            fail(
                "--probe-m2-5b %s：module 与 legacy 不一致 module=%s legacy=%s"
                % (name, module_fields, legacy_fields)
            )
        if module_fields != want:
            fail(
                "--probe-m2-5b %s：与门禁独立推导不一致 observed=%s expected=%s"
                % (name, module_fields, want)
            )
    return len(PROBE_SPECS)


def run_battery(scenario, overrides, expect_battery):
    proc = subprocess.run(
        [str(TEST_EXE)],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=900,
    )
    match = CHECK_RE.search(proc.stdout)
    battery = BATTERY_RE.search(proc.stdout)
    if proc.returncode != 0 or match is None or battery is None:
        fail(
            "差分测试 env 矩阵 %s 失败 exit=%d\nstdout:\n%s\nstderr:\n%s"
            % (scenario, proc.returncode, proc.stdout, proc.stderr)
        )
    passed, total = int(match.group(1)), int(match.group(2))
    checks, failures = int(battery.group(1)), int(battery.group(2))
    if passed != total:
        fail("差分测试 env 矩阵 %s: %d/%d（要求全过）" % (scenario, passed, total))
    if failures != 0:
        fail("差分测试 env 矩阵 %s: M2-5B battery failures=%d" % (scenario, failures))
    if expect_battery:
        if checks != EXPECTED_BATTERY_CHECKS:
            fail(
                "M2-5B 电池检查数 %d != 登记精确值 %d（差分被删改？）"
                % (checks, EXPECTED_BATTERY_CHECKS)
            )
        if checks < MIN_BATTERY_CHECKS:
            fail("M2-5B 电池检查数 %d 低于下限 %d" % (checks, MIN_BATTERY_CHECKS))
    if total < MIN_TOTAL_CHECKS:
        fail("差分测试总检查数 %d 低于下限 %d" % (total, MIN_TOTAL_CHECKS))
    return total


def main():
    module_h_text = read(MODULE_H)
    module_cpp_text = read(MODULE_CPP)
    module_text = module_h_text + "\n" + module_cpp_text
    inc_text = read(INC)
    test_text = read(TEST_CPP)
    device_text = read(DEVICE_CPP)
    scene_text = read(SCENE_H)
    meson_text = read(MESON)
    budget_text = read(BUDGET_GATE)
    generator_text = read(GENERATOR)
    read(RECORD_DOC)

    # 1. legacy 参考 provenance 与身份。
    if PRE_M2_5B_DEVICE_SHA256 not in inc_text:
        fail("legacy 参考缺少 pre-M2-5B d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_M2_5B_DEVICE_SIZE) not in inc_text:
        fail("legacy 参考缺少 pre-M2-5B 字节数 provenance")
    if PRE_M2_5B_CAPTURED_AT not in inc_text:
        fail("legacy 参考缺少捕获时间 provenance")
    if SYMBOL not in inc_text:
        fail("legacy 参考缺少符号名 " + SYMBOL)
    for name in FIELD_NAMES:
        if name not in inc_text:
            fail("legacy 参考正文缺少字段名 " + name)
    inc_bytes = INC.read_bytes()
    actual_inc_sha = sha256_bytes(inc_bytes)
    if actual_inc_sha != INC_SHA256:
        fail("legacy 参考 SHA-256 %s != 登记值 %s" % (actual_inc_sha, INC_SHA256))
    if len(inc_bytes) != INC_SIZE:
        fail("legacy 参考字节数 %d != 登记值 %d" % (len(inc_bytes), INC_SIZE))

    # 2. 模块身份。
    if sha256_bytes(MODULE_CPP.read_bytes()) != MODULE_CPP_SHA256:
        fail("模块 .cpp SHA-256 与登记值不一致（迁移文本已漂移）")
    if sha256_bytes(MODULE_H.read_bytes()) != MODULE_H_SHA256:
        fail("模块 .h SHA-256 与登记值不一致（迁移文本已漂移）")

    # 3. 模块正文 vs legacy 正文逐字节（本门禁自己抽两侧）。
    module_body = extract_body(module_cpp_text)
    inc_body = extract_body(inc_text)
    if module_body != inc_body:
        fail(
            "模块 .cpp 的 BEGIN/END 之间正文与 legacy .inc 的正文逐字节不一致"
            "（迁移文本已漂移）"
        )
    module_body_bytes = len(module_body.encode("utf-8"))

    # 4. M2-5B 迁移前的块仍在 .inc 正文内逐字节（以模块正文为基准的双向校验）。
    for line in module_body.splitlines():
        if line.startswith("      if (skinned) {"):
            break
    else:
        fail("模块正文缺少 skinned 守卫行")

    # 5. device.cpp 只剩调用点 + 锚注释。
    if device_text.count(SYMBOL + "(") != 1:
        fail("device.cpp 的 " + SYMBOL + " 调用点不是恰好 1 处")
    if device_text.count(SYMBOL) != 2:
        fail(
            "device.cpp 中 " + SYMBOL + " 出现次数 != 2（锚注释 1 + 调用点 1）"
        )
    if "M2-5B" not in device_text:
        fail("device.cpp 缺少 M2-5B 锚注释")
    if "war3/semantic/war3_palette_submitted_aggregation.h" not in device_text:
        fail("device.cpp 没有 include M2-5B 模块头")
    device_lf = device_text.replace("\r\n", "\n")
    moved_head = "      if (skinned) {\n        const uint64_t curPaletteHash ="
    if moved_head in device_lf:
        fail("device.cpp 仍含 B4 聚合块正文（块未真正迁出）")
    for name in FIELD_NAMES:
        if device_text.count(name) != 0:
            fail("device.cpp 仍含已迁出字段名（双重计数风险）：" + name)

    # 6. scene.h 定义仍是单一来源。
    for name in FIELD_NAMES:
        if scene_text.count(name) != 1:
            fail("d3d9_war3_scene.h 中字段名出现次数 != 1：" + name)

    # 7. 模块 .cpp 的字段单一实现（逐名实测次数）。
    for name in FIELD_NAMES:
        want = FIELD_MODULE_COUNTS[name]
        got = module_cpp_text.count(name)
        if got != want:
            fail(
                "模块 .cpp 字段 %s 出现次数 %d != 登记值 %d（单一实现漂移）"
                % (name, got, want)
            )

    # 8. 预算门禁 FROZEN_M2_5B 与实测 baseline。
    frozen = parse_frozen_m2_5b(budget_text)
    if set(frozen) != set(COVERED):
        fail(
            "预算门禁 FROZEN_M2_5B 与本门禁登记集合不一致：\n"
            "  FROZEN_M2_5B: %s\n  COVERED: %s" % (sorted(frozen), sorted(COVERED))
        )
    if frozen[SYMBOL] != (0, 0):
        fail("FROZEN_M2_5B[%s] != (0, 0)（baseline/budget 与实测口径不符）" % SYMBOL)
    snapshot_note = "快照不在位（跳过独立重抽）"
    if SNAPSHOT.is_file():
        def_re = budget_def_re(budget_text)
        snapshot_body = measure_snapshot_baseline(def_re)
        if snapshot_body + "\n" != module_body:
            fail("pre-M2-5B 快照独立抽取的块正文与模块/legacy 正文逐字节不一致")
        snapshot_note = "快照独立重抽一致 + DEF_RE baseline=0"
    else:
        print("note: %TEMP%/device_pre_m2_5b.cpp 不在位，" + snapshot_note)

    # 9. 差分测试证据原语。
    if "war3_palette_submitted_aggregation_legacy_reference.inc" not in test_text:
        fail("差分测试没有 include M2-5B legacy 参考")
    if "war3_palette_submitted_aggregation.h" not in test_text:
        fail("差分测试没有 include M2-5B 模块头")
    for prefix in ("sem::", "legacy::"):
        if (prefix + SYMBOL + "(") not in test_text:
            fail("差分测试缺少调用点：" + prefix + SYMBOL + "(")
    for primitive in (
        "--probe-m2-5b",
        "RunProbeM25b",
        "DiffM25bStruct",
        "DiffM25bFields",
        "M2-5B battery",
        "DiffM25bCase",
    ):
        if primitive not in test_text:
            fail("差分测试缺少 M2-5B 必需原语：" + primitive)

    # 10. meson：dll 源与差分测试目标都链接真实模块 .cpp。
    if "war3/semantic/war3_palette_submitted_aggregation.cpp" not in meson_text:
        fail("meson.build 没有把 M2-5B 模块加进任何目标")
    target = re.search(
        r"war3_live_palette_selection_test = executable\((.*?)\n\)",
        meson_text,
        re.DOTALL,
    )
    if target is None:
        fail("meson.build 里找不到 war3_live_palette_selection_test 目标")
    if "war3/semantic/war3_palette_submitted_aggregation.cpp" not in target.group(1):
        fail("差分测试目标没有链接 M2-5B 真实模块 .cpp")

    # 11. 生成器 CLI 与 fail-closed 常量。
    for token in ("--m2-5b", "main_m2_5b", "M2_5B_EXPECTED_SHA256", "M2_5B_START_LINE"):
        if token not in generator_text:
            fail("legacy 生成器缺少 M2-5B 模式原语：" + token)

    # 12. 动态证据。
    if not TEST_EXE.is_file():
        fail(
            "fail-closed: 缺少差分测试可执行文件 "
            + str(TEST_EXE.relative_to(ROOT))
            + "（M2-5B 动态等价证据不可缺省）"
        )
    probe_rows = run_probe("default", {})
    probe_rows_diag = run_probe("diagnostics-on", {PALETTE_DIAG: "1"})
    total_default = run_battery("default", {}, True)
    total_diag = run_battery("diagnostics-on", {PALETTE_DIAG: "1"}, True)

    print(
        "war3 palette m2-5b equivalence gate static checks passed"
        f"（M2-5B 已迁出符号 {len(COVERED)} 个；7 个字段单一实现；"
        f"legacy 参考 {INC_SIZE} B / SHA-256 {actual_inc_sha[:16]}…；"
        f"模块正文与 legacy 正文逐字节相同 {module_body_bytes} B；"
        f"pre-M2-5B device.cpp {PRE_M2_5B_DEVICE_SIZE} B / "
        f"{PRE_M2_5B_DEVICE_SHA256[:16]}…；{snapshot_note}；"
        f"probe-m2-5b 场景 {probe_rows}/{probe_rows_diag} × 2 组 env；"
        f"M2-5B 电池 {EXPECTED_BATTERY_CHECKS} 断言（下限 {MIN_BATTERY_CHECKS}）；"
        f"差分总检查 default={total_default} diagnostics-on={total_diag}"
        f"（下限 {MIN_TOTAL_CHECKS}））"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
