"""M2-1 live palette selection 迁移的**等价性证据门禁**（fail-closed）。

背景：M2-1 把 d3d9_device.cpp 里 8 个符号（4 个 live palette 纯计算 helper +
4 个运行时配置 getter）迁到
    src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}。
    AutoTest/test_device_semantic_responsibility_budget_static.py 只防止**职责回流**；
    它不能证明"迁出后的实现与原实现逐点等价"。本门禁钉住等价性证据本身，
    结构同型于 M1 的 AutoTest/test_device_semantic_predicates_equivalence_static.py
    （该脚本本轮一个字节不改）。

静态（fail-closed，任何一条不满足即失败）：
    1. 证据文件齐全：legacy 参考（迁移前 device.cpp 的 verbatim 文本）、
       入库生成器、差分测试、meson 目标链接真实模块 .cpp、等价记录文档。
    2. legacy 参考的 provenance：必须记录迁移前**工作树** d3d9_device.cpp 的
       SHA-256、捕获时间，并显式说明与 M1 git-blob provenance 的差异；文件
       自身的 SHA-256 与登记值一致（故意改参考文本 → 红）。
    3. 覆盖：预算门禁 FROZEN_M2_1 表里的每个符号（budget==0 < baseline）必须在
       - 模块（.h/.cpp）里出现，
       - legacy 参考里出现，
       - 差分测试里同时有 sem:: 与 legacy:: 调用点（只"提到名字"不算证据）。
       FROZEN_M2_1 的符号集合必须恰好等于本门禁登记的 COVERED（已迁出集合
       不得超出登记集合；新增迁出但没补等价证据 → 红）。
    4. 差分测试里必须有 legacy 参考的 include 与差分断言原语；检查数量下限
       （MIN_CHECKS）防止把差分删成空壳。

动态（可执行证据，构建产物存在时执行）：
    5. 以 2 组 env 矩阵运行差分测试可执行文件，要求 exit 0、检查数 >= MIN_CHECKS；
    6. --probe 模式逐 getter 对比 module vs legacy，并对照独立登记的期望值
       （覆盖默认 / 翻转 / 非数字 / 空值 / 十六进制 base-0 / 非零多值）。

边界（本门禁不覆盖，不得据此宣称"职责已迁完"或"已稳定"）：
    - 只覆盖 M2-1 的 8 个符号、M2-2 的选择链本体
      War3TryBuildLiveRuntimeGroupPalette（含其枚举/类型/slot 缓存/
      计数器的双侧一致性）与 M2-3 的 motion 诊断三函数（见下方加法式扩展）；
      M2-4/M2-5（调用点编排、taxonomy 发射）与 M3/M4 未做；
    - 动态部分只证明"同一输入 + 同一替身世界下两个实现一致"，不证明替身本身
      （sem::War3GetEnvU32 是与 M1 模块逐字节相同的替身，见测试文件头部；
      M2-2 的 12 个外部符号替身同理，见测试文件 stub 世界注释）；
    - device.cpp 侧调用点编排与 taxonomy 发射不在覆盖范围内；
    - 不覆盖 GPU、Vulkan、实机画面与性能。

M2-2 加法式扩展（2026-09-18）：本门禁在不改动任何 M2-1 断言的前提下
追加选择链本体的等价证据——
    7. 链 legacy 参考（--m2-2 生成）的 provenance 与自身 SHA-256 登记；
    8. 预算门禁 FROZEN_M2_2 的迁出集合必须恰好等于 CHAIN_COVERED；
    9. 链符号在模块/链 legacy 参考/差分测试（sem::+legacy:: 双侧调用点）
       三处同时出现；
    10. 电池 env 矩阵追加 contract-on（DXVK_WAR3_SKIN_PALETTE_CONTRACT=1），
        并按矩阵分别钉住检查数下限（CHAIN_MIN_CHECKS）；
    11. --probe-chain 10 个固定场景 × 3 组 env 的双侧逐字段一致 +
        独立期望值表（合同 ON 时 owned-hit 命中 source=6、其余场景
        fail-closed 不得 fallthrough）。

M2-3 加法式扩展（2026-09-18）：仍不改动任何 M2-1/M2-2 断言，追加 motion /
churn 诊断三函数（War3NoteLivePaletteMotion / War3NoteDrawTimePoseMotion /
War3NoteSubmittedPaletteMotion，连同各自 Entry 结构）的等价证据——
    12. motion legacy 参考（--m2-3 生成）的 provenance 与自身 SHA-256/字节数
        登记；
    13. 预算门禁 FROZEN_M2_3 的迁出集合必须恰好等于 MOTION_COVERED，且每条
        baseline=1 / budget=0（这三个符号不匹配语义选择命名族，判定 3 对它们
        是盲的，只能在预算判定上显式冻结）；
    14. 三个 motion 符号在模块 / motion legacy 参考 / 差分测试（sem::+legacy::
        双侧调用点）三处同时出现；两个 Entry 结构在模块与 legacy 参考里都在；
    15. 电池 env 矩阵追加 motion-on（DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1）
        并按矩阵登记检查数下限 MOTION_MIN_CHECKS；
    16. M2-3 等价记录文档存在。
    更细的 motion 证据（--probe-motion 15 场景 × 2 组 env 的独立期望值表）
    由独立门禁 AutoTest/test_war3_live_palette_motion_equivalence_static.py
    钉住；本门禁只钉住"文件/登记/矩阵下限"这一层，避免两处期望值漂移。
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
LEGACY_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_legacy_reference.inc"
)
TEST_CPP = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = (
    ROOT / "docs/plan/2026-09-18-m2-1-migration-equivalence-record.md"
)

# 迁移前（M1 迁移已应用、M2-1 未应用）的**未提交工作树** src/d3d9/d3d9_device.cpp
# 身份。与 M1 不同：这不是 git blob——M1 的工作成果尚未提交，git 里没有对应
# blob，因此本 provenance 是工作树文件哈希 + 捕获时间。故意改动 → 红。
PRE_M2_DEVICE_SHA256 = (
    "2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B"
)
PRE_M2_CAPTURED_AT = "2026-09-17T21:11:53"
# legacy 参考文件自身的身份（生成后登记；改动即需重新审阅并更新本文件）。
LEGACY_INC_SHA256 = (
    "D458C1AE6FF08B4962395CF77DC19D03AA6631FDDB05FC364002442A2A4F67D4"
)

# ---------------------------------------------------------------------------
# M2-2（加法式扩展）：选择链本体 War3TryBuildLiveRuntimeGroupPalette。
# ---------------------------------------------------------------------------
CHAIN_LEGACY_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_chain_legacy_reference.inc"
)
# 迁移前（M2-1 迁移已应用、M2-2 未应用）的**未提交工作树**
# src/d3d9/d3d9_device.cpp 身份（= M2-1 记录的"迁出后"工作树，同一 SHA，
# 交叉验证闭合）。同样是工作树文件哈希 + 捕获时间，不是 git blob。
PRE_M2_2_DEVICE_SHA256 = (
    "B70F3AF97CD3DB554DDB311E9B89F3C64B99996EF9ADEA4FBA4CA7D2CF6F98F1"
)
PRE_M2_2_CAPTURED_AT = "2026-09-17T22:54:30"
# 链 legacy 参考文件自身的身份（含前向声明修正后的再生成果；改动即需
# 重新审阅并更新本文件）。
CHAIN_LEGACY_INC_SHA256 = (
    "3D258F48355353B948ADDAB5266524DD07EEBF849B186C329DB2C54528274C1B"
)
M2_2_RECORD_DOC = (
    ROOT / "docs/plan/2026-09-18-m2-2-migration-equivalence-record.md"
)

# 本记录覆盖的 M2-2 已迁出符号（= 预算门禁 FROZEN_M2_2 的键集合）。
CHAIN_COVERED = {
    "War3TryBuildLiveRuntimeGroupPalette",
}

# ---------------------------------------------------------------------------
# M2-3（加法式扩展）：motion / churn 诊断三函数。
# ---------------------------------------------------------------------------
MOTION_LEGACY_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_motion_legacy_reference.inc"
)
# 迁移前（M2-2 迁移已应用、M2-3 未应用）的**未提交工作树**
# src/d3d9/d3d9_device.cpp 身份（= M2-2 记录的"迁移后"工作树，同一 SHA，
# 交叉验证闭合）。同样是工作树文件哈希 + 捕获时间，不是 git blob。
PRE_M2_3_DEVICE_SHA256 = (
    "ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3"
)
PRE_M2_3_DEVICE_SIZE = 2_278_494
PRE_M2_3_CAPTURED_AT = "2026-09-18T02:18:25"
MOTION_LEGACY_INC_SHA256 = (
    "7DF61688219BAF83BCA1836FBEB97DC2D1269B103A703917718CA9C9177AC727"
)
MOTION_LEGACY_INC_SIZE = 8_406
M2_3_RECORD_DOC = (
    ROOT / "docs/plan/2026-09-18-m2-3-migration-equivalence-record.md"
)

# 本记录覆盖的 M2-3 已迁出符号（= 预算门禁 FROZEN_M2_3 的键集合）。
MOTION_COVERED = {
    "War3NoteLivePaletteMotion",
    "War3NoteDrawTimePoseMotion",
    "War3NoteSubmittedPaletteMotion",
}

# motion 电池的检查数下限（2026-09-18 实测 default 17,314,411 /
# flipped-branches 18,123,899 / contract-on 13,826,299 / motion-on 17,314,411；
# motion 电池本身恒为 6,956,814 断言）。按下限约 70% 登记，留余量但仍能挡住
# 空壳化。
MOTION_MIN_CHECKS = {
    "default": 12_000_000,
    "flipped-branches": 12_500_000,
    "contract-on": 9_500_000,
    "motion-on": 12_000_000,
}

SKIN_CONTRACT = "DXVK_WAR3_SKIN_PALETTE_CONTRACT"

# 链电池各 env 矩阵的检查数下限（2026-09-18 实测 default 10,357,597 /
# flipped-branches 11,167,085 / contract-on 6,869,485；按下限约 70% 登记，
# 留余量但仍能挡住空壳化）。M2-1 的 MIN_CHECKS 继续适用于全部矩阵。
CHAIN_MIN_CHECKS = {
    "default": 7_200_000,
    "flipped-branches": 7_800_000,
    "contract-on": 4_800_000,
}

# --probe-chain 输出的解析：module/legacy 各 10 个逗号分隔字段
# (ok,source,slot,maxSlot,paletteSize,hash,minTag,maxTag,served,rejected)。
CHAIN_PROBE_RE = re.compile(
    r"probe-chain\s+(\S+)\s+module=([0-9,]+)\s+legacy=([0-9,]+)\s*$"
)
# 期望值表的字段子集（下标）：ok/source/slot/paletteSize/minTag/maxTag/
# served/rejected（hash 与 maxSlot 由 module==legacy 全字段相等覆盖）。
CHAIN_EXPECT = [
    # (name, ok, source, slot, paletteSize, minTag, maxTag, served, rejected)
    ("slot-direct", 1, 3, 7, 3, 55, 55, 0, 0),
    ("caches-confirm", 1, 3, 9, 3, 0, 0, 1, 0),
    ("reject-then-published", 1, 4, 4294967295, 3, 0, 0, 0, 1),
    ("global", 1, 2, 3, 3, 20, 21, 0, 0),
    ("blended", 1, 3, 4, 3, 30, 30, 0, 0),
    ("cmodel-deny", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("cmodel-allow", 1, 5, 4294967295, 3, 0, 0, 0, 0),
    ("pose-none", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    # 合同 OFF：owned 表被忽略，其余来源全 miss → false（slotIndex 已发布
    # +0x08 直读值 6）。
    ("owned-hit", 0, 0, 6, 0, 0, 0, 0, 0),
    ("owned-miss", 1, 3, 8, 3, 3, 3, 0, 0),
]
# flipped-branches：ALLOW_CMODEL=1 使 cmodel-deny 场景翻转成功（source=5）。
CHAIN_EXPECT_FLIPPED = [
    row if row[0] != "cmodel-deny"
    else ("cmodel-deny", 1, 5, 4294967295, 3, 0, 0, 0, 0)
    for row in CHAIN_EXPECT
]
# contract-on：合同路径短路一切非 owned 来源（fail-closed）；owned-hit
# 命中 source=6/slot=42；owned-miss fail-closed（不得 fallthrough）。
CHAIN_EXPECT_CONTRACT = [
    ("slot-direct", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("caches-confirm", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("reject-then-published", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("global", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("blended", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("cmodel-deny", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("cmodel-allow", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("pose-none", 0, 0, 4294967295, 0, 0, 0, 0, 0),
    ("owned-hit", 1, 6, 42, 3, 77, 77, 0, 0),
    ("owned-miss", 0, 0, 4294967295, 0, 0, 0, 0, 0),
]

# 本记录覆盖的 M2-1 已迁出符号（= 预算门禁 FROZEN_M2_1 的键集合）。
COVERED = {
    "War3SemanticHashMatrixPalette",
    "War3DecodeRuntimePoseMatrix48",
    "War3TryReadRuntimePoseArray",
    "War3ResolveLivePoseRuntimeAlias",
    "War3SemanticLivePaletteSafeCopyRuntime",
    "War3SemanticLivePaletteRefreshRuntime",
    "War3SemanticLivePaletteAllowCModelFallbackRuntime",
    "War3SemanticPaletteDiagnosticsRuntime",
}

# 差分测试的下限断言数（当前实测约 2.4M；留足余量但仍能挡住空壳化）。
MIN_CHECKS = 2_000_000
CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)

SAFE_COPY = "DXVK_WAR3_SEMANTIC_LIVE_PALETTE_SAFE_COPY"
REFRESH = "DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH"
ALLOW_CMODEL = "DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK"
PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"

# M2-2：--probe-chain 的 env 场景表（引用上方 env 常量，故定义于此）。
CHAIN_PROBE_SCENARIOS = [
    ("default", {}, CHAIN_EXPECT),
    (
        "flipped-branches",
        {SAFE_COPY: "0", ALLOW_CMODEL: "1"},
        CHAIN_EXPECT_FLIPPED,
    ),
    ("contract-on", {SKIN_CONTRACT: "1"}, CHAIN_EXPECT_CONTRACT),
]

GETTER_SAFE_COPY = "War3SemanticLivePaletteSafeCopyRuntime"
GETTER_REFRESH = "War3SemanticLivePaletteRefreshRuntime"
GETTER_ALLOW_CMODEL = "War3SemanticLivePaletteAllowCModelFallbackRuntime"
GETTER_PALETTE_DIAG = "War3SemanticPaletteDiagnosticsRuntime"

# getter 的独立期望值表（按 env 场景）。键 = --probe 输出的 name。
PROBE_SCENARIOS = [
    (
        "default",
        {},
        {
            GETTER_SAFE_COPY: 1,
            GETTER_REFRESH: 1,
            GETTER_ALLOW_CMODEL: 0,
            GETTER_PALETTE_DIAG: 0,
        },
    ),
    (
        "flipped",
        {
            SAFE_COPY: "0",
            REFRESH: "0",
            ALLOW_CMODEL: "1",
            PALETTE_DIAG: "1",
        },
        {
            GETTER_SAFE_COPY: 0,
            GETTER_REFRESH: 0,
            GETTER_ALLOW_CMODEL: 1,
            GETTER_PALETTE_DIAG: 1,
        },
    ),
    (
        "non-numeric",
        {
            # "abc"/"zzz" 无数字 → strtoul 无消费 → 回退默认值
            SAFE_COPY: "abc",
            PALETTE_DIAG: "zzz",
        },
        {
            GETTER_SAFE_COPY: 1,
            GETTER_PALETTE_DIAG: 0,
        },
    ),
    (
        "empty-string",
        {
            # 空字符串 → getEnvVar 为空 → 回退默认值
            REFRESH: "",
            ALLOW_CMODEL: "",
        },
        {
            GETTER_REFRESH: 1,
            GETTER_ALLOW_CMODEL: 0,
        },
    ),
    (
        "hex-base0",
        {
            # strtoul base 0："0x1" → 1（真）、"0x0" → 0（假）
            PALETTE_DIAG: "0x1",
            SAFE_COPY: "0x0",
        },
        {
            GETTER_PALETTE_DIAG: 1,
            GETTER_SAFE_COPY: 0,
        },
    ),
    (
        "nonzero-variants",
        {
            # "2" → 非零（真）；"07" → base 0 八进制 7 → 非零（真）
            ALLOW_CMODEL: "2",
            REFRESH: "07",
        },
        {
            GETTER_ALLOW_CMODEL: 1,
            GETTER_REFRESH: 1,
        },
    ),
]


def fail(message):
    raise AssertionError(message)


def read(path):
    if not path.is_file():
        fail(f"fail-closed: 缺少必需文件 {path}")
    return path.read_text(encoding="utf-8")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest().upper()


def parse_frozen_m2_1(text):
    """解析预算门禁的 FROZEN_M2_1 表（M2-1 的已迁出登记）。

    只读 FROZEN_M2_1 块：M1 等价门禁按文本解析原始 FROZEN 块，M2-1 的 8 个
    符号刻意不进入该块（见预算门禁内注释），本门禁的对照表因此必须锚在
    FROZEN_M2_1 上。
    """
    start = text.index("FROZEN_M2_1 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r'"([A-Za-z_][A-Za-z0-9_]*)":\s*\((\d+),\s*(\d+)\)',
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def parse_frozen_m2_2(text):
    """解析预算门禁的 FROZEN_M2_2 表（M2-2 的已迁出登记；同 M2-1 口径）。"""
    start = text.index("FROZEN_M2_2 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r'"([A-Za-z_][A-Za-z0-9_]*)":\s*\((\d+),\s*(\d+)\)',
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def parse_frozen_m2_3(text):
    """解析预算门禁的 FROZEN_M2_3 表（M2-3 的已迁出登记；同 M2-1 口径）。"""
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


def run_probe(scenario, overrides):
    proc = subprocess.run(
        [str(TEST_EXE), "--probe"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        fail(
            f"--probe 场景 {scenario} exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = re.match(
            r"probe\s+(\S+)\s+module=(\d+)\s+legacy=(\d+)\s*$", line
        )
        if not match:
            continue
        name, module_value, legacy_value = match.groups()
        if module_value != legacy_value:
            fail(
                f"--probe 场景 {scenario}: {name} module={module_value} "
                f"legacy={legacy_value}（迁移前后不一致）"
            )
        observed[name] = int(module_value)
    if not observed:
        fail(f"--probe 场景 {scenario} 没有解析到任何 getter 输出")
    return observed


def run_chain_probe(scenario, overrides):
    """运行 --probe-chain：逐场景要求 module/legacy 全字段相等，返回字段表。"""
    proc = subprocess.run(
        [str(TEST_EXE), "--probe-chain"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        fail(
            f"--probe-chain 场景 {scenario} exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = CHAIN_PROBE_RE.match(line)
        if not match:
            continue
        name, module_fields, legacy_fields = match.groups()
        if module_fields != legacy_fields:
            fail(
                f"--probe-chain 场景 {scenario}: {name} module={module_fields} "
                f"legacy={legacy_fields}（迁移前后不一致）"
            )
        observed[name] = [int(field) for field in module_fields.split(",")]
    if not observed:
        fail(f"--probe-chain 场景 {scenario} 没有解析到任何场景输出")
    return observed


def check_dynamic():
    if not TEST_EXE.is_file():
        print(
            "NOTE 差分测试可执行文件不存在，跳过动态等价部分："
            f"{TEST_EXE.relative_to(ROOT)}（静态部分仍然 fail-closed）"
        )
        return

    matrices = [
        ("default", {}),
        (
            "flipped-branches",
            {
                SAFE_COPY: "0",
                REFRESH: "0",
                ALLOW_CMODEL: "1",
                PALETTE_DIAG: "1",
            },
        ),
        # M2-2：合同 ON 矩阵——选择链只许走 owned snapshot，任何 miss 都
        # fail-closed；两侧在同一替身世界下仍必须逐点一致。
        ("contract-on", {SKIN_CONTRACT: "1"}),
        # M2-3：motion 诊断门 ON 矩阵——三个 War3Note*Motion 的**函数体**只在
        # DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1 时才被走到（默认 env 两侧
        # 都早退）；本矩阵专为 M2-3 的函数体差分登记。
        ("motion-on", {PALETTE_DIAG: "1"}),
    ]
    for name, overrides in matrices:
        proc = subprocess.run(
            [str(TEST_EXE)],
            cwd=str(ROOT),
            env=sanitized_env(overrides),
            capture_output=True,
            text=True,
            timeout=300,
        )
        match = CHECK_RE.search(proc.stdout)
        if proc.returncode != 0 or match is None:
            fail(
                f"差分测试 env 矩阵 {name} 失败 exit={proc.returncode}\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        passed, total = int(match.group(1)), int(match.group(2))
        if passed != total or total < MIN_CHECKS:
            fail(
                f"差分测试 env 矩阵 {name}: {passed}/{total}（要求全过且"
                f" >= {MIN_CHECKS}）"
            )
        chain_floor = CHAIN_MIN_CHECKS.get(name)
        if chain_floor is not None and total < chain_floor:
            fail(
                f"差分测试 env 矩阵 {name}: {total} 低于 M2-2 链电池登记下限"
                f" {chain_floor}（差分被删成空壳？）"
            )
        motion_floor = MOTION_MIN_CHECKS.get(name)
        if motion_floor is not None and total < motion_floor:
            fail(
                f"差分测试 env 矩阵 {name}: {total} 低于 M2-3 motion 电池登记"
                f"下限 {motion_floor}（差分被删成空壳？）"
            )

    for scenario, overrides, expected in PROBE_SCENARIOS:
        observed = run_probe(scenario, overrides)
        for name, value in expected.items():
            if observed.get(name) != value:
                fail(
                    f"--probe 场景 {scenario}: {name} 期望 {value}，实测 "
                    f"{observed.get(name)}"
                )

    # M2-2：--probe-chain 10 个固定场景 × 3 组 env。期望值子集字段下标：
    # (name, ok[0], source[1], slot[2], paletteSize[4], minTag[6], maxTag[7],
    #  served[8], rejected[9])。
    field_index = {
        "ok": 0,
        "source": 1,
        "slot": 2,
        "paletteSize": 4,
        "minTag": 6,
        "maxTag": 7,
        "served": 8,
        "rejected": 9,
    }
    for scenario, overrides, expected_rows in CHAIN_PROBE_SCENARIOS:
        observed = run_chain_probe(scenario, overrides)
        for row in expected_rows:
            name = row[0]
            fields = observed.get(name)
            if fields is None:
                fail(f"--probe-chain 场景 {scenario}: 缺少场景 {name} 的输出")
            for label, expected_value in zip(
                ("name",) + tuple(field_index.keys()), row
            ):
                if label == "name":
                    continue
                actual_value = fields[field_index[label]]
                if actual_value != expected_value:
                    fail(
                        f"--probe-chain 场景 {scenario}: {name}.{label} 期望"
                        f" {expected_value}，实测 {actual_value}"
                    )


def main():
    module_text = read(MODULE_H) + "\n" + read(MODULE_CPP)
    legacy_text = read(LEGACY_INC)
    test_text = read(TEST_CPP)
    meson_text = read(MESON)
    budget_text = read(BUDGET_GATE)
    read(GENERATOR)
    read(RECORD_DOC)

    # 1. provenance：legacy 参考点名迁移前工作树 device.cpp 身份、捕获时间，
    #    并显式说明与 M1 git-blob provenance 的差异；自身身份被登记。
    if PRE_M2_DEVICE_SHA256 not in legacy_text:
        fail("legacy 参考缺少迁移前 d3d9_device.cpp 的 SHA-256 provenance")
    if PRE_M2_CAPTURED_AT not in legacy_text:
        fail("legacy 参考缺少迁移前工作树文件的捕获时间 provenance")
    if "work-tree file hash, not a git blob hash" not in legacy_text:
        fail(
            "legacy 参考缺少 worktree-vs-git-blob 差异说明"
            "（M2-1 provenance 是工作树文件哈希，不是 M1 那样的 git blob）"
        )
    actual_legacy_sha = sha256_bytes(LEGACY_INC.read_bytes())
    if actual_legacy_sha != LEGACY_INC_SHA256:
        fail(
            "legacy 参考文件哈希与登记值不一致："
            f"{actual_legacy_sha} != {LEGACY_INC_SHA256}"
        )

    # 2. 差分测试必须真的引用 legacy 参考、点名入库生成器并包含差分断言原语。
    if "war3_live_palette_selection_legacy_reference.inc" not in test_text:
        fail("差分测试没有 include 迁移前 legacy 参考")
    if (
        "gen_war3_live_palette_selection_legacy_reference.py"
        not in legacy_text
    ):
        fail("legacy 参考没有点名入库生成器（生成器入库是 M2-1 的补齐项）")
    for primitive in ("legacy::", "DIFF_U64(", "RunProbe", "--probe"):
        if primitive not in test_text:
            fail(f"差分测试缺少必需原语：{primitive}")

    # 3. meson 目标必须链接真实模块 .cpp（否则差分测的是假实现）。
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

    # 4. 覆盖：预算门禁 FROZEN_M2_1 登记的符号集合必须恰好等于 COVERED，
    #    且每个符号 budget==0 < baseline（真迁出）。
    frozen = parse_frozen_m2_1(budget_text)
    if not frozen:
        fail("fail-closed: 预算门禁里没有解析到 FROZEN_M2_1 表")
    migrated = {
        name
        for name, (baseline, budget) in frozen.items()
        if budget == 0 and budget < baseline
    }
    if migrated != COVERED:
        fail(
            "预算门禁 FROZEN_M2_1 的迁出集合与本门禁登记集合不一致：\n"
            f"  仅在 FROZEN_M2_1: {sorted(migrated - COVERED)}\n"
            f"  仅在 COVERED:     {sorted(COVERED - migrated)}\n"
            "（已迁出集合不得超出登记集合；新增迁出必须同步补等价证据）"
        )

    missing_module = sorted(
        name for name in COVERED if name not in module_text
    )
    if missing_module:
        fail("以下已迁出符号在新模块里看不到：\n  " + "\n  ".join(missing_module))

    missing_legacy = sorted(name for name in COVERED if name not in legacy_text)
    if missing_legacy:
        fail(
            "以下已迁出符号在 legacy 参考里看不到（迁移前实现缺失）：\n  "
            + "\n  ".join(missing_legacy)
        )

    not_compared = []
    for name in sorted(COVERED):
        module_call = (
            f"sem::{name}(" in test_text or f"&sem::{name}" in test_text
        )
        legacy_call = (
            f"legacy::{name}(" in test_text or f"&legacy::{name}" in test_text
        )
        if not (module_call and legacy_call):
            not_compared.append(
                f"{name} (sem={module_call}, legacy={legacy_call})"
            )
    if not_compared:
        fail(
            "以下已迁出符号没有在差分测试里同时与 legacy 实现对比：\n  "
            + "\n  ".join(not_compared)
        )

    # ------------------------------------------------------------------
    # M2-2（加法式扩展）：选择链本体的等价证据。
    # ------------------------------------------------------------------
    chain_legacy_text = read(CHAIN_LEGACY_INC)
    # 7. 链 legacy provenance + 自身身份登记。
    if PRE_M2_2_DEVICE_SHA256 not in chain_legacy_text:
        fail("链 legacy 参考缺少 pre-M2-2 d3d9_device.cpp 的 SHA-256 provenance")
    if PRE_M2_2_CAPTURED_AT not in chain_legacy_text:
        fail("链 legacy 参考缺少 pre-M2-2 工作树文件的捕获时间 provenance")
    if "file hash, not a git blob hash" not in chain_legacy_text:
        fail("链 legacy 参考缺少 worktree-vs-git-blob 差异说明")
    actual_chain_sha = sha256_bytes(CHAIN_LEGACY_INC.read_bytes())
    if actual_chain_sha != CHAIN_LEGACY_INC_SHA256:
        fail(
            "链 legacy 参考文件哈希与登记值不一致："
            f"{actual_chain_sha} != {CHAIN_LEGACY_INC_SHA256}"
        )
    if (
        "gen_war3_live_palette_selection_legacy_reference.py"
        not in chain_legacy_text
    ):
        fail("链 legacy 参考没有点名入库生成器")
    if (
        "war3_live_palette_selection_chain_legacy_reference.inc"
        not in test_text
    ):
        fail("差分测试没有 include 链 legacy 参考")
    for primitive in ("--probe-chain", "RunProbeChain"):
        if primitive not in test_text:
            fail(f"差分测试缺少链必需原语：{primitive}")

    # 8. 预算门禁 FROZEN_M2_2 的迁出集合必须恰好等于 CHAIN_COVERED。
    frozen_m2_2 = parse_frozen_m2_2(budget_text)
    if not frozen_m2_2:
        fail("fail-closed: 预算门禁里没有解析到 FROZEN_M2_2 表")
    migrated_m2_2 = {
        name
        for name, (baseline, budget) in frozen_m2_2.items()
        if budget == 0 and budget < baseline
    }
    if migrated_m2_2 != CHAIN_COVERED:
        fail(
            "预算门禁 FROZEN_M2_2 的迁出集合与本门禁登记集合不一致：\n"
            f"  仅在 FROZEN_M2_2: {sorted(migrated_m2_2 - CHAIN_COVERED)}\n"
            f"  仅在 CHAIN_COVERED:     {sorted(CHAIN_COVERED - migrated_m2_2)}"
        )

    # 9. 链符号三处出现：模块 / 链 legacy 参考 / 差分测试双侧调用点。
    for name in sorted(CHAIN_COVERED):
        if name not in module_text:
            fail(f"已迁出链符号在新模块里看不到：{name}")
        if name not in chain_legacy_text:
            fail(f"已迁出链符号在链 legacy 参考里看不到：{name}")
        if f"sem::{name}(" not in test_text:
            fail(f"差分测试缺少链符号的模块侧调用点：sem::{name}(")
        if f"legacy::{name}(" not in test_text:
            fail(f"差分测试缺少链符号的 legacy 侧调用点：legacy::{name}(")

    # M2-2 等价记录文档（M2-2 阶段 7 交付；缺失即红）。
    read(M2_2_RECORD_DOC)

    # ------------------------------------------------------------------
    # M2-3（加法式扩展）：motion / churn 诊断三函数的等价证据。
    # ------------------------------------------------------------------
    motion_legacy_text = read(MOTION_LEGACY_INC)
    # 12. motion legacy provenance + 自身身份登记。
    if PRE_M2_3_DEVICE_SHA256 not in motion_legacy_text:
        fail("motion legacy 参考缺少 pre-M2-3 d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_M2_3_DEVICE_SIZE) not in motion_legacy_text:
        fail("motion legacy 参考缺少 pre-M2-3 d3d9_device.cpp 的字节数 provenance")
    if PRE_M2_3_CAPTURED_AT not in motion_legacy_text:
        fail("motion legacy 参考缺少 pre-M2-3 工作树文件的捕获时间 provenance")
    if "file hash, not a git blob hash" not in motion_legacy_text:
        fail("motion legacy 参考缺少 worktree-vs-git-blob 差异说明")
    if (
        "gen_war3_live_palette_selection_legacy_reference.py"
        not in motion_legacy_text
    ):
        fail("motion legacy 参考没有点名入库生成器")
    actual_motion_sha = sha256_bytes(MOTION_LEGACY_INC.read_bytes())
    if actual_motion_sha != MOTION_LEGACY_INC_SHA256:
        fail(
            "motion legacy 参考文件哈希与登记值不一致："
            f"{actual_motion_sha} != {MOTION_LEGACY_INC_SHA256}"
        )
    if MOTION_LEGACY_INC.stat().st_size != MOTION_LEGACY_INC_SIZE:
        fail(
            "motion legacy 参考字节数与登记值不一致："
            f"{MOTION_LEGACY_INC.stat().st_size} != {MOTION_LEGACY_INC_SIZE}"
        )
    if (
        "war3_live_palette_selection_motion_legacy_reference.inc"
        not in test_text
    ):
        fail("差分测试没有 include motion legacy 参考")
    for primitive in ("--probe-motion", "RunProbeMotion"):
        if primitive not in test_text:
            fail(f"差分测试缺少 M2-3 必需原语：{primitive}")

    # 13. 预算门禁 FROZEN_M2_3 的迁出集合必须恰好等于 MOTION_COVERED。
    frozen_m2_3 = parse_frozen_m2_3(budget_text)
    if not frozen_m2_3:
        fail("fail-closed: 预算门禁里没有解析到 FROZEN_M2_3 表")
    migrated_m2_3 = {
        name
        for name, (baseline, budget) in frozen_m2_3.items()
        if budget == 0 and budget < baseline
    }
    if migrated_m2_3 != MOTION_COVERED:
        fail(
            "预算门禁 FROZEN_M2_3 的迁出集合与本门禁登记集合不一致：\n"
            f"  仅在 FROZEN_M2_3: {sorted(migrated_m2_3 - MOTION_COVERED)}\n"
            f"  仅在 MOTION_COVERED: {sorted(MOTION_COVERED - migrated_m2_3)}"
        )
    wrong_baseline = sorted(
        f"{name}={pair} (要求 (1, 0))"
        for name, pair in frozen_m2_3.items()
        if pair != (1, 0)
    )
    if wrong_baseline:
        fail(
            "FROZEN_M2_3 的 baseline/budget 与实测口径不符：\n  "
            + "\n  ".join(wrong_baseline)
        )

    # 14. 三处出现：模块 / motion legacy 参考 / 差分测试双侧调用点。
    for name in sorted(MOTION_COVERED):
        if name not in module_text:
            fail(f"已迁出 motion 符号在新模块里看不到：{name}")
        if name not in motion_legacy_text:
            fail(f"已迁出 motion 符号在 motion legacy 参考里看不到：{name}")
        if f"sem::{name}(" not in test_text:
            fail(f"差分测试缺少 motion 符号的模块侧调用点：sem::{name}(")
        if f"legacy::{name}(" not in test_text:
            fail(f"差分测试缺少 motion 符号的 legacy 侧调用点：legacy::{name}(")
    for entry in (
        "War3SemanticPaletteMotionEntry",
        "War3SemanticHashMotionEntry",
    ):
        if entry not in module_text:
            fail(f"模块里看不到随迁的 Entry 结构：{entry}")
        if entry not in motion_legacy_text:
            fail(f"motion legacy 参考里看不到随迁的 Entry 结构：{entry}")

    # 16. M2-3 等价记录文档（缺失即红）。
    read(M2_3_RECORD_DOC)

    check_dynamic()

    print(
        "war3 live palette selection equivalence gate static checks passed"
        f"（M2-1 已迁出符号 {len(COVERED)} 个 + M2-2 链符号"
        f" {len(CHAIN_COVERED)} 个全部有 legacy 差分覆盖；"
        f"M2-1 legacy 参考 {LEGACY_INC.stat().st_size} B / SHA-256 "
        f"{actual_legacy_sha[:16]}…；链 legacy 参考"
        f" {CHAIN_LEGACY_INC.stat().st_size} B / SHA-256 "
        f"{actual_chain_sha[:16]}…；差分下限 {MIN_CHECKS} 断言（各矩阵"
        f"另有 M2-2 链电池下限）；probe 场景 {len(PROBE_SCENARIOS)} 组 +"
        f" probe-chain 场景 {len(CHAIN_PROBE_SCENARIOS)} 组 × 10 场景；"
        f"M2-3 motion 符号 {len(MOTION_COVERED)} 个 + 2 个 Entry 结构有 legacy"
        f" 差分覆盖，motion legacy 参考 {MOTION_LEGACY_INC.stat().st_size} B /"
        f" SHA-256 {actual_motion_sha[:16]}…，motion-on 矩阵下限 "
        f"{MOTION_MIN_CHECKS['motion-on']}）"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
