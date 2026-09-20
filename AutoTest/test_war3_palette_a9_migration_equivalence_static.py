#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""A9 死代码裁定（迁移而非删除）的等价证据门禁（fail-closed，独立成文件）。

覆盖对象：dxvk::war3::semantic::War3SemanticBuildWorldPaletteIfNeeded
（pre-A9 d3d9_device.cpp :7161-7175 的 [[maybe_unused]] 死代码定义，逐字节迁入
src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}）。

裁定背景：主线程 2026-09-18 裁定 —— A10 War3SemanticVectorStorageReadable
（未实例化 function template）**删除**；A9 War3SemanticBuildWorldPaletteIfNeeded
（0 调用）**迁移**，因为它的正文是 A4 compose-policy 所服务的
model-local palette -> world-space 组合合同（删除会丢掉这份参考实现）。

本门禁自己复算：
  * legacy .inc 的 SHA/字节数与 provenance 文本（含 worktree-vs-git-blob 说明）；
  * 模块 .cpp 的 A9 BEGIN/END 标记之间正文 vs .inc 内 A9 定义正文（逐字节）；
  * 若 %TEMP%/device_pre_a9.cpp 在位，则从快照**独立重抽**定义（锚唯一 + 行号 +
    正文不含 device/registry token + 仍含 A4 调用链），并与 .inc/模块正文比对，
    同时用预算门禁**同一** DEF_RE（从其源码 eval）实测 baseline=0；
  * device.cpp 里 A9 定义文本必须为 0（迁移后的反方向断言）；
  * FROZEN_A9 键集合/baseline/budget 与原始 FROZEN 块的盲区；
  * 动态：4 组 env 下差分测试 exit 0、全过、A9 电池精确检查数 + 0 failures，
    以及 --probe-a9 在 2 组 env 下与门禁内**独立实现**的 A4/A9 语义推导逐行对照
    （module == legacy == 独立期望值）。

边界（不得据此宣称"A9 已生效"或"palette 侧职责已迁完"）：
  * A9 在生产代码里**仍然没有任何调用者**（迁移保持"死代码但可审计"）；
    本门禁证明的是"迁移逐字节等价 + 合同语义一致"，不是"它被使用了"；
  * 差分只证明"同一输入 + 同一替身世界下两个实现一致"；IsReadableRange 在宿主
    机是替身，不证明生产内存原语本身；
  * 不覆盖 GPU / Vulkan / 实机画面与性能；未部署、未启动游戏、不构成稳定版依据。
"""
import hashlib
import math
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

BUDGET_GATE = ROOT / "AutoTest/test_device_semantic_responsibility_budget_static.py"
GENERATOR = ROOT / "AutoTest/gen_war3_live_palette_selection_legacy_reference.py"
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp"
INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_a9_legacy_reference.inc"
)
TEST_CPP = (
    ROOT / "src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp"
)
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = ROOT / "docs/plan/2026-09-18-a9-a10-dead-code-ruling-record.md"
SNAPSHOT = Path(os.environ.get("TEMP", ".")) / "device_pre_a9.cpp"

# pre-A9 工作树身份（= A10 删除落地后的 device.cpp；与 A10 步骤的 post SHA 同值）。
PRE_A9_DEVICE_SHA256 = (
    "FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C"
)
PRE_A9_DEVICE_SIZE = 2_248_823
PRE_A9_CAPTURED_AT = "2026-09-18T04:49:48"

INC_SHA256 = (
    "D92048FCB8B293C2A738051043C918240BBCD5759459FA029744D8FADE3FD66F"
)
INC_SIZE = 3_026
# legacy .inc 内 A9 定义自身的行号（.inc 头部注释 + 空行之后的固定位置；改动 .inc
# 头部行数即红）。device.cpp 侧的行号另由 A9_LINES 与注释锚核对。
INC_DEF_LINE = 40

A9_KEY = "War3SemanticBuildWorldPaletteIfNeeded"
A9_DEV_SIG = "\n".join([
    "[[maybe_unused]] void War3SemanticBuildWorldPaletteIfNeeded(",
    "    const std::vector<Matrix4>& sourcePalette,",
    "    const Matrix4& worldTransform,",
    "    uint8_t objectKind,",
    "    std::vector<Matrix4>& outPalette) {",
])
A9_MOD_SIG = "\n".join([
    "void War3SemanticBuildWorldPaletteIfNeeded(",
    "    const std::vector<Matrix4>& sourcePalette,",
    "    const Matrix4& worldTransform,",
    "    uint8_t objectKind,",
    "    std::vector<Matrix4>& outPalette) {",
])
A9_LINES = (7161, 7175)
A9_BEGIN = ("    // --- BEGIN A9 body: %s (pre-A9 d3d9_device.cpp :%d-%d) ---"
            % (A9_KEY, A9_LINES[0], A9_LINES[1]))
A9_END = "    // --- END A9 body: %s ---" % A9_KEY

M24_INC_NAME = "war3_live_palette_selection_m2_4_legacy_reference.inc"
A9_INC_NAME = "war3_live_palette_selection_a9_legacy_reference.inc"

COVERED = (A9_KEY,)
SYMBOL = COVERED[0]

PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"
SKIN_CONTRACT = "DXVK_WAR3_SKIN_PALETTE_CONTRACT"

# 差分测试自身打印的 A9 电池检查数（精确值 + 下限双重咬合）。
# 9 palette variant × 2 可读性 × 8 world × 256 kind = 36,864 次系统网格调用 +
# 200,000 次定种子随机 = 236,864 次调用；每次 4 条（结构 memcmp + count + hash +
# 输出 vector 逐字节 memcmp）= 947,456，加 2 次段末累计 memcmp = 947,458。
EXPECTED_BATTERY_CHECKS = 947_458
MIN_BATTERY_CHECKS = 940_000
MIN_TOTAL_CHECKS = 34_000_000

BATTERY_RE = re.compile(r"A9 battery:\s*(\d+) checks,\s*(\d+) failures")
CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)
PROBE_RE = re.compile(
    r"probe-a9\s+(\S+)\s+module=(\d+),(\d+)\s+legacy=(\d+),(\d+)\s*$"
)

# --probe-a9 场景表（与测试 .cpp 的 A9ProbeSpec 表一一对应）：
# (name, world variant, palette variant, kind, registerReadable)
PROBE_SPECS = [
    ("a9-local-world-far", 3, 1, 1, True),
    ("a9-palette-too-far", 3, 4, 1, True),
    ("a9-palette-nan", 3, 3, 1, True),
    ("a9-palette-inf", 3, 5, 1, True),
    ("a9-world-zero", 0, 1, 1, True),
    ("a9-world-mag-16-0008", 1, 1, 1, True),
    ("a9-world-mag-exactly-16", 2, 1, 1, True),
    ("a9-world-nan", 5, 1, 1, True),
    ("a9-world-inf", 6, 1, 1, True),
    ("a9-world-5-palette-500", 7, 6, 1, True),
    ("a9-four-entries", 3, 2, 1, True),
    ("a9-kind-0-unknown", 3, 1, 0, True),
    ("a9-kind-2-building", 3, 1, 2, True),
    ("a9-empty-palette", 3, 0, 1, True),
    ("a9-count-256", 3, 7, 1, True),
    ("a9-count-257", 3, 8, 1, True),
    ("a9-unreadable", 3, 1, 1, False),
]

# 独立期望值表（先按源码语义手工推导，再与实跑对照后固定）。顺序与 PROBE_SPECS
# 一致：(count, hash)。hash = 门禁内独立实现的 FNV-1a 64（offset
# 0xcbf29ce484222325、prime 0x100000001b3）对输出 palette 每个 Matrix4 的
# 16 个 float32 位模式（行主序、按元素顺序）复算。
EXPECTED_PROBE = {
    "a9-local-world-far": (1, 6770109457156564069),
    "a9-palette-too-far": (0, 14695981039346656037),
    "a9-palette-nan": (0, 14695981039346656037),
    "a9-palette-inf": (0, 14695981039346656037),
    "a9-world-zero": (0, 14695981039346656037),
    "a9-world-mag-16-0008": (0, 14695981039346656037),
    "a9-world-mag-exactly-16": (0, 14695981039346656037),
    "a9-world-nan": (0, 14695981039346656037),
    "a9-world-inf": (0, 14695981039346656037),
    "a9-world-5-palette-500": (3, 12563832956526974693),
    "a9-four-entries": (4, 11761412259722496037),
    "a9-kind-0-unknown": (1, 6770109457156564069),
    "a9-kind-2-building": (0, 14695981039346656037),
    "a9-empty-palette": (0, 14695981039346656037),
    "a9-count-256": (256, 12645096708081967909),
    "a9-count-257": (0, 14695981039346656037),
    "a9-unreadable": (0, 14695981039346656037),
}

MASK64 = (1 << 64) - 1
FNV_OFFSET = 0xCBF29CE484222325
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


def extract_definition(text, signature):
    start = 0
    while True:
        index = text.find(signature, start)
        if index < 0:
            fail("找不到定义锚：%r" % signature[:60])
        brace = text.index("{", index)
        semi = text.find(";", index, brace)
        if semi < 0:
            end = text.index("\n}\n", brace)
            line = text.count("\n", 0, index) + 1
            return line, text[index:end + 3]
        start = index + 1


def inc_body(inc_text):
    line, definition = extract_definition(inc_text.replace("\r\n", "\n"), A9_DEV_SIG)
    return line, definition[len(A9_DEV_SIG):-2]


def module_body(module_text):
    if module_text.count(A9_BEGIN) != 1 or module_text.count(A9_END) != 1:
        fail("模块 .cpp 的 A9 BEGIN/END 标记不是恰好 1 对")
    begin_end = module_text.index("\n", module_text.index(A9_BEGIN)) + 1
    return module_text[begin_end:module_text.index(A9_END)]


def budget_def_re(text):
    """从预算门禁源码里 eval 出同一个 DEF_RE（不是抄写）。"""
    start = text.index("DEF_RE = re.compile(")
    end = text.index("SKIP_NAMES = {")
    namespace = {"re": re}
    exec(text[start:end], namespace)
    return namespace["DEF_RE"]


def parse_skip_names(text):
    match = re.search(r"SKIP_NAMES = \{(.*?)\}", text, re.DOTALL)
    if match is None:
        fail("无法从预算门禁解析 SKIP_NAMES")
    return eval("{" + match.group(1) + "}")


def parse_frozen_a9(text):
    start = text.index("FROZEN_A9 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def parse_frozen_base_block(text):
    """原始 FROZEN 块（到第一个行首 } 为止）——M1 等价门禁的唯一输入。"""
    start = text.index("FROZEN = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


# ---- 门禁内独立实现的 A4/A9 语义（不读 C++ 实现） ---------------------------
def float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def float_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def fnv1a_bits(values):
    hash_value = FNV_OFFSET
    for value in values:
        hash_value = ((hash_value ^ value) * FNV_PRIME) & MASK64
    return hash_value


def probe_palette(variant):
    """M24Palette(variant) 的平移部分（其余行是单位阵）。"""
    if variant == 0:
        return []
    if variant == 1:
        return [(64.0, 0.0, 0.0)]
    if variant == 2:
        return [(64.0, 0.0, 0.0), (96.0, 0.0, 0.0),
                (48.0, 0.0, 0.0), (80.0, 0.0, 0.0)]
    if variant == 3:
        return [(64.0, 0.0, 0.0), (96.0, 0.0, 0.0),
                (float("nan"), 0.0, 0.0), (80.0, 0.0, 0.0), (48.0, 0.0, 0.0)]
    if variant == 4:
        return [(1000000.0 + i * 4096.0, 0.0, 0.0) for i in range(4)]
    if variant == 5:
        return [(64.0, 0.0, 0.0), (float("inf"), 0.0, 0.0), (48.0, 0.0, 0.0)]
    if variant == 6:
        return [(500.0, 0.0, 0.0), (600.0, 0.0, 0.0), (700.0, 0.0, 0.0)]
    if variant == 7:
        return [(64.0 + i, 0.0, 0.0) for i in range(256)]
    if variant == 8:
        return [(64.0 + i, 0.0, 0.0) for i in range(257)]
    fail("未知 palette variant %d" % variant)


def probe_world(variant):
    """M24World(variant) 的平移部分。"""
    if variant == 0:
        return (0.0, 0.0, 0.0)
    if variant == 1:
        return (4.0001, 0.0, 0.0)
    if variant == 2:
        return (4.0, 0.0, 0.0)
    if variant == 3:
        return (1000.0, 0.0, 0.0)
    if variant == 4:
        return (4096.0, 0.0, 0.0)
    if variant == 5:
        return (float("nan"), 0.0, 0.0)
    if variant == 6:
        return (1000.0, 0.0, float("inf"))
    if variant == 7:
        return (5.0, 0.0, 0.0)
    fail("未知 world variant %d" % variant)


def object_kind_radius(kind):
    """War3SemanticBoundsRadiusForObjectKind 的独立复算。"""
    return {1: 260.0, 2: 900.0, 3: 750.0, 4: 220.0, 5: 900.0}.get(kind, 0.0)


def expected_a9(world_variant, palette_variant, kind, register_readable):
    """独立推导 A9 的输出 palette（元素 = world * local 的 16 个 float 位模式）。

    返回元素列表；每个元素是 4 行 × 4 列的元组。空列表 = outPalette.clear()
    后的早退（A3 storage 判定失败或 A4 compose-policy 判 false）。
    """
    palette = probe_palette(palette_variant)
    world = probe_world(world_variant)
    # A3 War3SemanticPaletteStorageReadable
    if not register_readable or len(palette) == 0 or len(palette) > 256:
        return []
    # A4 vector 重载 -> A3 -> pointer 重载（checkReadable = false）
    wx, wy, wz = world
    if not (math.isfinite(wx) and math.isfinite(wy) and math.isfinite(wz)):
        return []
    if not (float32(wx * wx + wy * wy + wz * wz) > 16.0):
        return []
    guard = object_kind_radius(kind)
    if not (guard > 0.0):
        guard = 260.0
    guard = max(384.0, guard * 1.5)
    threshold_sq = guard * guard
    closest_sq = float("inf")
    closest_palette_mag_sq = float("inf")
    for (lx, ly, lz) in palette[:4]:
        if not (math.isfinite(lx) and math.isfinite(ly) and math.isfinite(lz)):
            return []
        closest_palette_mag_sq = min(
            closest_palette_mag_sq, lx * lx + ly * ly + lz * lz)
        dx, dy, dz = lx - wx, ly - wy, lz - wz
        closest_sq = min(closest_sq, dx * dx + dy * dy + dz * dz)
    local_mag_limit = max(1024.0, guard * 2.0)
    if closest_palette_mag_sq > local_mag_limit * local_mag_limit:
        return []
    if not (closest_sq > threshold_sq):
        return []
    return [
        ((1.0, 0.0, 0.0, 0.0), (0.0, 1.0, 0.0, 0.0),
         (0.0, 0.0, 1.0, 0.0), (wx + lx, wy + ly, wz + lz, 1.0))
        for (lx, ly, lz) in palette
    ]


def expected_probe_row(spec):
    name, world_variant, palette_variant, kind, readable = spec
    elements = expected_a9(world_variant, palette_variant, kind, readable)
    bits = []
    for element in elements:
        for row in element:
            for value in row:
                bits.append(float_bits(value))
    return (len(elements), fnv1a_bits(bits))


def self_check_expectations():
    """期望值表自证：独立推导必须复算出登记值（改一处即红）。"""
    for spec in PROBE_SPECS:
        name = spec[0]
        derived = expected_probe_row(spec)
        if derived != EXPECTED_PROBE[name]:
            fail(
                "门禁内独立推导与登记期望值不一致：%s 推导 %s != 登记 %s"
                % (name, derived, EXPECTED_PROBE[name])
            )
    if set(EXPECTED_PROBE) != {spec[0] for spec in PROBE_SPECS}:
        fail("EXPECTED_PROBE 与 PROBE_SPECS 场景集合不一致")
    if EXPECTED_PROBE["a9-local-world-far"][1] == FNV_OFFSET:
        fail("空输出与非空输出的 FNV 值不能相同（自证失败）")


def measure_snapshot(def_re, skip_names, inc_text_body):
    raw = SNAPSHOT.read_bytes()
    actual = sha256_bytes(raw)
    if actual != PRE_A9_DEVICE_SHA256 or len(raw) != PRE_A9_DEVICE_SIZE:
        fail(
            "pre-A9 快照 %TEMP%/device_pre_a9.cpp 身份与登记值不一致：\n"
            "  SHA-256 %s != %s\n  size %d != %d"
            % (actual, PRE_A9_DEVICE_SHA256, len(raw), PRE_A9_DEVICE_SIZE)
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")
    if A9_DEV_SIG not in text:
        fail("pre-A9 快照里找不到 A9 定义（迁移前状态错误？）")
    line, definition = extract_definition(text, A9_DEV_SIG)
    span = (line, line + definition.count("\n") - 1)
    if span != A9_LINES:
        fail("pre-A9 快照里 A9 定义行号 %s != 登记 %s" % (span, A9_LINES))
    body = definition[len(A9_DEV_SIG):-2]
    if body != inc_text_body:
        fail("pre-A9 快照独立重抽的 A9 正文与 legacy .inc 正文逐字节不一致")
    sites = 0
    for raw_line in text.splitlines():
        if not raw_line or raw_line[:1] in (" ", "\t"):
            continue
        if raw_line.startswith(("//", "#", "/*", "*")):
            continue
        match = def_re.match(raw_line)
        if match and match.group(1) == SYMBOL and match.group(1) not in skip_names:
            sites += 1
    if sites != 0:
        fail("pre-A9 快照中 A9 的行首 DEF_RE 站点 = %d（实测 baseline 应为 0）"
             % sites)
    return "快照独立重抽一致 + DEF_RE baseline=0"


def check_static():
    budget_text = read(BUDGET_GATE)
    generator_text = read(GENERATOR)
    module_h = read(MODULE_H)
    module_cpp = read(MODULE_CPP)
    inc_text = read(INC)
    test_text = read(TEST_CPP)
    device_text = read(DEVICE_CPP)
    meson_text = read(MESON)
    read(RECORD_DOC)

    # --- 1. legacy 参考 provenance 与自身身份 ---
    if PRE_A9_DEVICE_SHA256 not in inc_text:
        fail("legacy 参考缺少 pre-A9 d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_A9_DEVICE_SIZE) not in inc_text:
        fail("legacy 参考缺少 pre-A9 字节数 provenance")
    if PRE_A9_CAPTURED_AT not in inc_text:
        fail("legacy 参考缺少捕获时间 provenance")
    if ("work-tree file hash," not in inc_text
            or "not a git blob hash" not in inc_text):
        fail("legacy 参考缺少 worktree-vs-git-blob 差异说明")
    if "gen_war3_live_palette_selection_legacy_reference.py" not in inc_text:
        fail("legacy 参考没有点名入库生成器")
    if SYMBOL not in inc_text or "namespace m2_legacy_reference" not in inc_text:
        fail("legacy 参考缺少符号名或 m2_legacy_reference 命名空间")
    if "War3SemanticPaletteLooksModelLocal(" not in inc_text:
        fail("legacy 参考正文丢失 A4 调用链合同")
    inc_bytes = INC.read_bytes()
    actual_inc_sha = sha256_bytes(inc_bytes)
    if actual_inc_sha != INC_SHA256:
        fail("legacy 参考 SHA-256 %s != 登记值 %s" % (actual_inc_sha, INC_SHA256))
    if len(inc_bytes) != INC_SIZE:
        fail("legacy 参考字节数 %d != 登记值 %d" % (len(inc_bytes), INC_SIZE))

    # --- 2. 正文逐字节：本门禁自己抽 .inc，与模块 BEGIN/END 之间比较 ---
    line, body = inc_body(inc_text)
    if line != INC_DEF_LINE:
        fail("legacy 参考里 A9 定义行号 %d != 登记 %d" % (line, INC_DEF_LINE))
    if ("d3d9_device.cpp line %d" % A9_LINES[0]) not in inc_text:
        fail("legacy 参考的抽取行号注释不是登记的 pre-A9 行号 %d" % A9_LINES[0])
    if (":%d-%d" % A9_LINES) not in inc_text:
        fail("legacy 参考头部缺少 pre-A9 行号范围 :%d-%d" % A9_LINES)
    if module_cpp.count(A9_MOD_SIG) != 1:
        fail("模块 .cpp 里的 A9 模块侧签名不是恰好 1 处")
    if module_body(module_cpp) != body:
        fail(
            "模块 .cpp 的 A9 BEGIN/END 之间正文与 legacy .inc 正文逐字节不一致"
            "（迁移文本已漂移）"
        )
    body_bytes = len(body.encode("utf-8"))

    # --- 3. 模块 .h 声明与模块侧语义 ---
    if A9_MOD_SIG.split("\n")[0] not in module_h:
        fail("模块 .h 缺少 A9 声明")
    if "std::vector<Matrix4>& outPalette);" not in module_h:
        fail("模块 .h 的 A9 声明缺少 outPalette 形参")
    if "[[maybe_unused]] void " + SYMBOL in module_h:
        fail("模块 .h 的 A9 声明不应带 [[maybe_unused]]（模块内非 static，无需该属性）")
    if "[[maybe_unused]] void " + SYMBOL in module_cpp:
        fail("模块 .cpp 的 A9 定义不应带 [[maybe_unused]]")

    # --- 4. device.cpp：A9 定义文本必须为 0（迁移后的反方向断言） ---
    if A9_DEV_SIG in device_text.replace("\r\n", "\n"):
        fail("device.cpp 仍含 A9 定义正文（迁移未落地/回流）")
    if "[[maybe_unused]] void " + SYMBOL in device_text:
        fail("device.cpp 仍含 A9 定义行（迁移未落地/回流）")
    def_re = budget_def_re(budget_text)
    skip_names = parse_skip_names(budget_text)
    sites = 0
    for raw_line in device_text.replace("\r\n", "\n").splitlines():
        if not raw_line or raw_line[:1] in (" ", "\t"):
            continue
        if raw_line.startswith(("//", "#", "/*", "*")):
            continue
        match = def_re.match(raw_line)
        if match and match.group(1) == SYMBOL and match.group(1) not in skip_names:
            sites += 1
    if sites != 0:
        fail("device.cpp 仍存在 A9 的行首 DEF_RE 站点 %d 个（回流/双实现）" % sites)

    # --- 5. FROZEN_A9 键集合 / baseline / budget / 原始块盲区 ---
    frozen = parse_frozen_a9(budget_text)
    if set(frozen) != set(COVERED):
        fail(
            "FROZEN_A9 与本门禁登记集合不一致：%s vs %s"
            % (sorted(frozen), sorted(COVERED))
        )
    if frozen[SYMBOL] != (0, 0):
        fail("FROZEN_A9[%s] != (0, 0)（baseline 必须是 pre-A9 快照上的 DEF_RE 实测值）"
             % SYMBOL)
    if "FROZEN.update(FROZEN_A9)" not in budget_text:
        fail("预算门禁没有把 FROZEN_A9 update 进 FROZEN（判定 1 不会生效）")
    if SYMBOL in parse_frozen_base_block(budget_text):
        fail(
            "原始 FROZEN 块里不得出现 A9（否则 M1 等价门禁会把它误判为 M1 迁出"
            "而无等价证据）"
        )

    # --- 6. 生成器 --a9 模式原语 ---
    for token in ("--a9", "main_a9", "A9_EXPECTED_SHA256", "A9_LINES",
                  "A9_DEV_SIG", "A9_MOD_SIG", "A9_END", "A9_OUT_PATH"):
        if token not in generator_text:
            fail("legacy 生成器缺少 A9 模式原语：" + token)

    # --- 7. 差分测试证据原语与 include 顺序（A4 legacy 解析依赖顺序） ---
    if A9_INC_NAME not in test_text:
        fail("差分测试没有 include A9 legacy 参考")
    if M24_INC_NAME not in test_text:
        fail("差分测试没有 include M2-4 legacy 参考")
    if test_text.index(A9_INC_NAME) < test_text.index(M24_INC_NAME):
        fail("A9 legacy 参考必须 include 在 M2-4 legacy 参考**之后**（A4 解析顺序）")
    for prefix in ("sem::", "legacy::"):
        if (prefix + SYMBOL + "(") not in test_text:
            fail("差分测试缺少调用点：" + prefix + SYMBOL + "(")
    for primitive in ("--probe-a9", "RunProbeA9", "DiffA9Case", "DiffA9Systematic",
                      "DiffA9Random", "DiffA9StructBytes", "A9HashPalette",
                      "A9 battery:"):
        if primitive not in test_text:
            fail("差分测试缺少 A9 必需原语：" + primitive)

    # --- 8. meson：差分测试目标仍链接真实模块 .cpp ---
    target = re.search(
        r"war3_live_palette_selection_test = executable\((.*?)\n\)",
        meson_text,
        re.DOTALL,
    )
    if target is None:
        fail("meson.build 里找不到 war3_live_palette_selection_test 目标")
    if "war3/semantic/war3_live_palette_selection.cpp" not in target.group(1):
        fail("差分测试目标没有链接真实模块 .cpp")
    if "war3_live_palette_selection_test.cpp" not in target.group(1):
        fail("差分测试目标没有链接差分测试 .cpp")

    # --- 9. 快照独立重抽（在位时 fail-closed） ---
    snapshot_note = "快照不在位（跳过独立重抽）"
    if SNAPSHOT.is_file():
        snapshot_note = measure_snapshot(def_re, skip_names, body)
    else:
        print("note: %TEMP%/device_pre_a9.cpp 不在位，" + snapshot_note)
    return body_bytes, actual_inc_sha, snapshot_note


def run_probe(scenario, overrides):
    proc = subprocess.run(
        [str(TEST_EXE), "--probe-a9"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=300,
    )
    if proc.returncode != 0:
        fail(
            "--probe-a9 场景 %s exit=%d\nstdout:\n%s\nstderr:\n%s"
            % (scenario, proc.returncode, proc.stdout[-4000:], proc.stderr[-4000:])
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = PROBE_RE.match(line)
        if not match:
            continue
        name, mc, mh, lc, lh = match.groups()
        observed[name] = ((int(mc), int(mh)), (int(lc), int(lh)))
    if set(observed) != {spec[0] for spec in PROBE_SPECS}:
        fail(
            "--probe-a9 场景集合与门禁登记不一致：\n  observed: %s\n  expected: %s"
            % (sorted(observed), sorted(spec[0] for spec in PROBE_SPECS))
        )
    for spec in PROBE_SPECS:
        name = spec[0]
        module_row, legacy_row = observed[name]
        want = EXPECTED_PROBE[name]
        if module_row != legacy_row:
            fail("--probe-a9 %s：module %s != legacy %s"
                 % (name, module_row, legacy_row))
        if module_row != want:
            fail("--probe-a9 %s：实测 %s != 门禁独立期望 %s"
                 % (name, module_row, want))
    return len(PROBE_SPECS)


def run_battery(scenario, overrides):
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
            % (scenario, proc.returncode, proc.stdout[-4000:], proc.stderr[-4000:])
        )
    passed, total = int(match.group(1)), int(match.group(2))
    checks, failures = int(battery.group(1)), int(battery.group(2))
    if passed != total:
        fail("差分测试 env 矩阵 %s: %d/%d（要求全过）" % (scenario, passed, total))
    if failures != 0:
        fail("差分测试 env 矩阵 %s: A9 battery failures=%d" % (scenario, failures))
    if checks != EXPECTED_BATTERY_CHECKS:
        fail("A9 电池检查数 %d != 登记精确值 %d（差分被删改？）"
             % (checks, EXPECTED_BATTERY_CHECKS))
    if checks < MIN_BATTERY_CHECKS:
        fail("A9 电池检查数 %d 低于下限 %d" % (checks, MIN_BATTERY_CHECKS))
    if total < MIN_TOTAL_CHECKS:
        fail("差分测试总检查数 %d 低于下限 %d" % (total, MIN_TOTAL_CHECKS))
    return total


def check_dynamic():
    if not TEST_EXE.is_file():
        fail(
            "fail-closed: 缺少差分测试可执行文件 "
            + str(TEST_EXE.relative_to(ROOT))
            + "（A9 动态等价证据不可缺省）"
        )
    matrices = [
        ("default", {}),
        ("palette-diag", {PALETTE_DIAG: "1"}),
        ("contract", {SKIN_CONTRACT: "1"}),
        ("contract-diag", {SKIN_CONTRACT: "1", PALETTE_DIAG: "1"}),
    ]
    totals = {}
    for name, overrides in matrices:
        totals[name] = run_battery(name, overrides)
    probe_rows = run_probe("default", {})
    probe_rows_diag = run_probe("palette-diag", {PALETTE_DIAG: "1"})
    return totals, probe_rows, probe_rows_diag


def main():
    self_check_expectations()
    body_bytes, actual_inc_sha, snapshot_note = check_static()
    totals, probe_rows, probe_rows_diag = check_dynamic()
    print(
        "war3 palette a9 migration equivalence gate static checks passed"
        f"（A9 已迁出符号 {len(COVERED)} 个；legacy 参考 {INC_SIZE} B / "
        f"SHA-256 {actual_inc_sha[:16]}…；模块正文与 legacy 正文逐字节相同 "
        f"{body_bytes} B；pre-A9 device.cpp {PRE_A9_DEVICE_SIZE} B / "
        f"{PRE_A9_DEVICE_SHA256[:16]}…；{snapshot_note}；"
        f"probe-a9 场景 {probe_rows}/{probe_rows_diag} × 2 组 env；"
        f"A9 电池 {EXPECTED_BATTERY_CHECKS} 断言（下限 {MIN_BATTERY_CHECKS}）；"
        f"差分总检查 default={totals['default']} "
        f"palette-diag={totals['palette-diag']} "
        f"contract={totals['contract']} "
        f"contract-diag={totals['contract-diag']}（下限 {MIN_TOTAL_CHECKS}））"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
