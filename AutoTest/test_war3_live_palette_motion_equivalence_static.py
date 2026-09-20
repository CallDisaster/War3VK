"""M2-3 motion 诊断三函数迁移的**等价性证据门禁**（fail-closed，独立成文件）。

背景：M2-3 把 d3d9_device.cpp 里的三个 motion / churn 诊断函数连同各自 Entry
结构迁到
    src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}：
        War3NoteLivePaletteMotion / War3NoteDrawTimePoseMotion /
        War3NoteSubmittedPaletteMotion。

为什么需要**独立**门禁（而不是仅靠 M2 等价门禁的加法式扩展）：
    这三个符号**不匹配** AutoTest/test_device_semantic_responsibility_budget_static.py
    的 SEMANTIC_FAMILY_RE 语义选择命名族（名字里没有
    Resolve/Select/Runtime/Key/... 任何后缀），因此那条门禁的判定 3（回流）
    对它们是盲的。M2-3 把三者单独登记进 FROZEN_M2_3 后，只有"预算"这一条
    判定能咬住回流；本文件负责钉住**等价性证据本身**，与 M2-1/M2-2 的记录
    同型。

静态（fail-closed，任何一条不满足即失败）：
    1. 证据文件齐全：motion legacy 参考、入库生成器、预算门禁、模块 .h/.cpp、
       差分测试、meson 目标链接真实模块 .cpp、M2-3 等价记录文档。
    2. legacy 参考 provenance：必须记录迁移前**工作树** d3d9_device.cpp 的
       SHA-256、字节数、捕获时间，并显式说明与 M1 git-blob provenance 的差异；
       文件自身 SHA-256 与登记值一致（故意改参考文本 → 红）。
    3. FROZEN_M2_3 的迁出集合必须**恰好**等于本门禁登记的 MOTION_COVERED，
       且每条 baseline=1 / budget=0（真迁出；baseline 由门禁同一 regex 的
       行首站点口径实测，见 M2-3 等价记录 §2）。
    4. 三个符号必须在模块（.h/.cpp）、motion legacy 参考、差分测试
       （sem:: + legacy:: 双侧调用点）三处同时出现；只"提到名字"不算证据。
    5. 差分测试必须 include motion legacy 参考、点名入库生成器，且带
       --probe-motion 原语与 RunProbeMotion。

动态（可执行证据，构建产物存在时执行）：
    6. --probe-motion 在两组 env 下运行：诊断门 ON
       （DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1）时逐场景对照**独立期望值
       表**（new-runtime / stable / raw-change / group-change / 三条早退 /
       pose 三态 / submitted 三态）；诊断门 OFF（默认 env）时全部场景必须
       全 0（证明早退门本身）。两侧 module/legacy 必须逐字段相等。
    7. 差分测试在 motion-on env 矩阵下 exit 0、全过、检查数 >=
       MOTION_MIN_CHECKS（防把差分删成空壳）。

边界（本门禁不覆盖，不得据此宣称"职责已迁完"或"已稳定"）：
    - 只覆盖 M2-3 的三个 motion 符号与两个 Entry 结构；M2-4/M2-5（调用点
      编排、taxonomy 发射）未做；
    - 动态部分只证明"同一输入 + 同一 stats 状态序列下两个实现一致"；三个
      函数不解引用 runtimeModelPtr，故本电池不含任何替身，也不证明
      device.cpp 调用点编排、env 原语或 GPU 行为；
    - 不覆盖 GPU、Vulkan、实机画面与性能。
"""

from pathlib import Path
import hashlib
import os
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]

BUDGET_GATE = ROOT / "AutoTest/test_device_semantic_responsibility_budget_static.py"
GENERATOR = ROOT / "AutoTest/gen_war3_live_palette_selection_legacy_reference.py"
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp"
MOTION_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_motion_legacy_reference.inc"
)
TEST_CPP = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = ROOT / "docs/plan/2026-09-18-m2-3-migration-equivalence-record.md"

# 迁移前（M2-2 迁移已应用、M2-3 未应用）的**未提交工作树**
# src/d3d9/d3d9_device.cpp 身份（= M2-2 记录的"迁移后 device.cpp"，同一
# SHA，交叉验证闭合）。与 M1 不同：这不是 git blob——工作成果未提交。故意
# 改动 → 红。
PRE_M2_3_DEVICE_SHA256 = (
    "ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3"
)
PRE_M2_3_DEVICE_SIZE = 2_278_494
PRE_M2_3_CAPTURED_AT = "2026-09-18T02:18:25"
# motion legacy 参考文件自身的身份（生成后登记；改动即需重新审阅并更新本文件）。
MOTION_INC_SHA256 = (
    "7DF61688219BAF83BCA1836FBEB97DC2D1269B103A703917718CA9C9177AC727"
)
MOTION_INC_SIZE = 8_406

# 本门禁覆盖的 M2-3 已迁出符号（= 预算门禁 FROZEN_M2_3 的键集合）。
MOTION_COVERED = (
    "War3NoteLivePaletteMotion",
    "War3NoteDrawTimePoseMotion",
    "War3NoteSubmittedPaletteMotion",
)

PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"

# motion-on 矩阵的检查数下限（按实测约 70% 登记；见 M2-3 等价记录 §5）。
MOTION_MIN_CHECKS = 12_000_000

CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)
PROBE_MOTION_RE = re.compile(
    r"probe-motion\s+(\S+)\s+module=([0-9,]+)\s+legacy=([0-9,]+)\s*$"
)

K_MOTION_FIELD_COUNT = 23

# 期望值字段下标（与测试 kMotionFieldNames 同序）：
#   0 liveSample, 1 liveNewRuntime, 2 liveRawChanged, 3 liveRawStable,
#   4 liveGroupChanged, 5 liveGroupStable, 6 liveLastRuntimeModelPtr,
#   7 liveLastPrevRawHash, 8 liveLastRawHash, 9 liveLastPrevGroupHash,
#   10 liveLastGroupHash, 11 poseChanged, 12 poseStable,
#   13 poseLastRuntimeModelPtr, 14 poseLastPrevHash, 15 poseLastHash,
#   16 submittedSample, 17 submittedNewRuntime, 18 submittedChanged,
#   19 submittedStable, 20 submittedLastRuntimeModelPtr,
#   21 submittedLastPrevHash, 22 submittedLastHash


def motion_ptr(index):
    """与测试 MotionPtr() 同一公式（合成指针，函数只比较指针值）。"""
    return 0x100000 + index * 0x100


def motion_row(live=None, pose=None, submitted=None):
    row = [0] * K_MOTION_FIELD_COUNT
    if live is not None:
        row[0:11] = live
    if pose is not None:
        row[11:16] = pose
    if submitted is not None:
        row[16:23] = submitted
    return row


def zero_row():
    return [0] * K_MOTION_FIELD_COUNT


ZERO = zero_row()

# --probe-motion 场景的独立期望值表（诊断门 ON）。顺序 = 测试输出顺序。
MOTION_PROBE_EXPECT = [
    ("live-new", motion_row(
        live=[1, 1, 0, 0, 0, 0, motion_ptr(2000), 0, 0x11, 0, 0x22])),
    ("live-stable", motion_row(
        live=[2, 1, 0, 1, 0, 1, motion_ptr(2001), 0x11, 0x11, 0x22, 0x22])),
    ("live-raw-change", motion_row(
        live=[2, 1, 1, 0, 0, 1, motion_ptr(2002), 0x11, 0x33, 0x22, 0x22])),
    ("live-group-change", motion_row(
        live=[2, 1, 0, 1, 1, 0, motion_ptr(2003), 0x11, 0x11, 0x22, 0x44])),
    ("live-null-ptr", ZERO),
    ("live-zero-raw", ZERO),
    ("live-zero-group", ZERO),
    ("pose-new", motion_row(
        pose=[0, 0, motion_ptr(2006), 0, 0xAA])),
    ("pose-stable", motion_row(
        pose=[0, 1, motion_ptr(2007), 0xAA, 0xAA])),
    ("pose-change", motion_row(
        pose=[1, 0, motion_ptr(2008), 0xAA, 0xBB])),
    ("pose-zero", ZERO),
    ("submitted-new", motion_row(
        submitted=[1, 1, 0, 0, motion_ptr(2010), 0, 0xCC])),
    ("submitted-stable", motion_row(
        submitted=[2, 1, 0, 1, motion_ptr(2011), 0xCC, 0xCC])),
    ("submitted-change", motion_row(
        submitted=[2, 1, 1, 0, motion_ptr(2012), 0xCC, 0xDD])),
    ("submitted-zero", ZERO),
]
# 诊断门 OFF（默认 env）：三个函数的第一个 if 即早退，全部场景必须全 0。
MOTION_PROBE_EXPECT_OFF = [
    (name, ZERO) for name, _row in MOTION_PROBE_EXPECT
]

MOTION_PROBE_SCENARIOS = [
    ("diagnostics-on", {PALETTE_DIAG: "1"}, MOTION_PROBE_EXPECT),
    ("default", {}, MOTION_PROBE_EXPECT_OFF),
]


def fail(message):
    raise AssertionError(message)


def read(path):
    if not path.is_file():
        fail(f"fail-closed: 缺少必需文件 {path}")
    return path.read_text(encoding="utf-8")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest().upper()


def parse_frozen_m2_3(text):
    """只解析预算门禁的 FROZEN_M2_3 块（M1 等价门禁按文本解析原始 FROZEN
    块；M2-3 的符号刻意不进入该块，见预算门禁内注释）。"""
    start = text.index("FROZEN_M2_3 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r'"([A-Za-z_][A-Za-z0-9_]*)":\s*\((\d+),\s*(\d+)\)',
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def sanitized_env(overrides):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith("DXVK_WAR3_")
    }
    env.update(overrides)
    return env


def run_motion_probe(scenario, overrides):
    proc = subprocess.run(
        [str(TEST_EXE), "--probe-motion"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        fail(
            f"--probe-motion 场景 {scenario} exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = PROBE_MOTION_RE.match(line)
        if not match:
            continue
        name, module_fields, legacy_fields = match.groups()
        if module_fields != legacy_fields:
            fail(
                f"--probe-motion 场景 {scenario}: {name} module={module_fields}"
                f" legacy={legacy_fields}（迁移前后不一致）"
            )
        observed[name] = [int(field) for field in module_fields.split(",")]
    if not observed:
        fail(f"--probe-motion 场景 {scenario} 没有解析到任何场景输出")
    return observed


def check_dynamic():
    if not TEST_EXE.is_file():
        fail(
            "fail-closed: 缺少差分测试可执行文件 "
            f"{TEST_EXE.relative_to(ROOT)}（M2-3 动态等价证据不可缺省）"
        )

    for scenario, overrides, expected_rows in MOTION_PROBE_SCENARIOS:
        observed = run_motion_probe(scenario, overrides)
        for name, expected in expected_rows:
            actual = observed.get(name)
            if actual is None:
                fail(f"--probe-motion 场景 {scenario}: 缺少场景 {name} 的输出")
            if len(actual) != K_MOTION_FIELD_COUNT:
                fail(
                    f"--probe-motion 场景 {scenario}: {name} 字段数 "
                    f"{len(actual)} != {K_MOTION_FIELD_COUNT}"
                )
            for index, (got, want) in enumerate(zip(actual, expected)):
                if got != want:
                    fail(
                        f"--probe-motion 场景 {scenario}: {name} 字段 "
                        f"#{index} 期望 {want}，实测 {got}"
                    )

    proc = subprocess.run(
        [str(TEST_EXE)],
        cwd=str(ROOT),
        env=sanitized_env({PALETTE_DIAG: "1"}),
        capture_output=True,
        text=True,
        timeout=300,
    )
    match = CHECK_RE.search(proc.stdout)
    if proc.returncode != 0 or match is None:
        fail(
            f"差分测试 env 矩阵 motion-on 失败 exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    passed, total = int(match.group(1)), int(match.group(2))
    if passed != total:
        fail(f"差分测试 env 矩阵 motion-on: {passed}/{total}（要求全过）")
    if total < MOTION_MIN_CHECKS:
        fail(
            f"差分测试 env 矩阵 motion-on: {total} 低于 M2-3 登记下限 "
            f"{MOTION_MIN_CHECKS}（差分被删成空壳？）"
        )
    return total


def main():
    module_text = read(MODULE_H) + "\n" + read(MODULE_CPP)
    motion_text = read(MOTION_INC)
    test_text = read(TEST_CPP)
    meson_text = read(MESON)
    budget_text = read(BUDGET_GATE)
    read(GENERATOR)
    read(RECORD_DOC)

    # 1. legacy 参考 provenance 与自身身份。
    if PRE_M2_3_DEVICE_SHA256 not in motion_text:
        fail("motion legacy 参考缺少 pre-M2-3 d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_M2_3_DEVICE_SIZE) not in motion_text:
        fail("motion legacy 参考缺少 pre-M2-3 d3d9_device.cpp 的字节数 provenance")
    if PRE_M2_3_CAPTURED_AT not in motion_text:
        fail("motion legacy 参考缺少 pre-M2-3 工作树文件的捕获时间 provenance")
    if "file hash, not a git blob hash" not in motion_text:
        fail("motion legacy 参考缺少 worktree-vs-git-blob 差异说明")
    if (
        "gen_war3_live_palette_selection_legacy_reference.py"
        not in motion_text
    ):
        fail("motion legacy 参考没有点名入库生成器")
    if "using namespace dxvk;" not in motion_text:
        fail("motion legacy 参考缺少共享契约类型 War3ShadowCaptureStats 的解析路径")
    actual_inc_sha = sha256_bytes(MOTION_INC.read_bytes())
    if actual_inc_sha != MOTION_INC_SHA256:
        fail(
            "motion legacy 参考文件哈希与登记值不一致："
            f"{actual_inc_sha} != {MOTION_INC_SHA256}"
        )
    if MOTION_INC.stat().st_size != MOTION_INC_SIZE:
        fail(
            "motion legacy 参考字节数与登记值不一致："
            f"{MOTION_INC.stat().st_size} != {MOTION_INC_SIZE}"
        )

    # 2. 差分测试必须真的引用 motion legacy 参考、点名生成器并含差分原语。
    if "war3_live_palette_selection_motion_legacy_reference.inc" not in test_text:
        fail("差分测试没有 include motion legacy 参考")
    for primitive in ("--probe-motion", "RunProbeMotion", "DIFF_U64("):
        if primitive not in test_text:
            fail(f"差分测试缺少 M2-3 必需原语：{primitive}")

    # 3. 差分测试必须链接**真实**模块 .cpp（否则差分测的是假实现）。
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
        fail("差分测试目标没有包含测试源文件")

    # 4. 预算门禁 FROZEN_M2_3 的迁出集合必须恰好等于 MOTION_COVERED，
    #    且每条 baseline=1 / budget=0（迁移前实测行首站点数，见记录 §2）。
    frozen = parse_frozen_m2_3(budget_text)
    if not frozen:
        fail("fail-closed: 预算门禁里没有解析到 FROZEN_M2_3 表")
    migrated = {
        name
        for name, (baseline, budget) in frozen.items()
        if budget == 0 and budget < baseline
    }
    if migrated != set(MOTION_COVERED):
        fail(
            "预算门禁 FROZEN_M2_3 的迁出集合与本门禁登记集合不一致：\n"
            f"  仅在 FROZEN_M2_3: {sorted(migrated - set(MOTION_COVERED))}\n"
            f"  仅在 MOTION_COVERED: "
            f"{sorted(set(MOTION_COVERED) - migrated)}"
        )
    wrong_baseline = sorted(
        f"{name}={pair} (要求 (1, 0))"
        for name, pair in frozen.items()
        if pair != (1, 0)
    )
    if wrong_baseline:
        fail(
            "FROZEN_M2_3 的 baseline/budget 与实测口径不符（迁移前 device.cpp"
            " 行首定义站点各 1 个、无前置声明；budget 必须为 0）：\n  "
            + "\n  ".join(wrong_baseline)
        )

    # 5. 三处出现：模块 / motion legacy 参考 / 差分测试双侧调用点。
    for name in MOTION_COVERED:
        if name not in module_text:
            fail(f"已迁出 motion 符号在新模块里看不到：{name}")
        if name not in motion_text:
            fail(f"已迁出 motion 符号在 legacy 参考里看不到：{name}")
        if f"sem::{name}(" not in test_text:
            fail(f"差分测试缺少 motion 符号的模块侧调用点：sem::{name}(")
        if f"legacy::{name}(" not in test_text:
            fail(f"差分测试缺少 motion 符号的 legacy 侧调用点：legacy::{name}(")

    # 6. Entry 结构随迁：两侧都必须有（模块 .cpp 内是唯一实现细节）。
    for entry in (
        "War3SemanticPaletteMotionEntry",
        "War3SemanticHashMotionEntry",
    ):
        if entry not in module_text:
            fail(f"模块里看不到随迁的 Entry 结构：{entry}")
        if entry not in motion_text:
            fail(f"legacy 参考里看不到随迁的 Entry 结构：{entry}")

    total = check_dynamic()

    print(
        "war3 live palette motion equivalence gate static checks passed"
        f"（M2-3 已迁出符号 {len(MOTION_COVERED)} 个 + 2 个 Entry 结构全部"
        f"有 legacy 差分覆盖；motion legacy 参考 {MOTION_INC.stat().st_size} B"
        f" / SHA-256 {actual_inc_sha[:16]}…；pre-M2-3 device.cpp "
        f"{PRE_M2_3_DEVICE_SIZE} B / {PRE_M2_3_DEVICE_SHA256[:16]}…；"
        f"probe-motion 场景 {len(MOTION_PROBE_SCENARIOS)} 组 × "
        f"{len(MOTION_PROBE_EXPECT)} 场景；motion-on 差分 {total} 断言"
        f"（下限 {MOTION_MIN_CHECKS}））"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
