"""M1 语义职责迁出的**等价性证据门禁**（fail-closed）。

背景：M1 把 d3d9_device.cpp 里 26 个纯判定/常量迁到
    src/d3d9/war3/semantic/war3_device_semantic_predicates.{h,cpp}。
    AutoTest/test_device_semantic_responsibility_budget_static.py 只防止**职责回流**；
    它不能证明"迁出后的实现与原实现逐点等价"。本门禁钉住等价性证据本身：

静态（fail-closed，任何一条不满足即失败）：
    1. 证据文件齐全：legacy 参考（迁移前 device.cpp 的 verbatim 文本）、差分测试、
       meson 目标链接真实模块 .cpp。
    2. legacy 参考的 provenance：必须记录迁移前 d3d9_device.cpp 的 SHA-256，且文件
       自身的 SHA-256 与登记值一致（故意改参考文本 → 红）。
    3. 覆盖：预算门禁里"budget < baseline"的每个已迁出符号，必须在
       - 模块（.h/.cpp）里出现，
       - legacy 参考里出现，
       - 差分测试里同时有 sem:: 与 legacy:: 调用点（只"提到名字"不算证据）。
    4. 已迁出集合不得超出登记集合（新增迁出但没补等价证据 → 红）。
    5. 差分测试里必须有 legacy 参考的 include 与差分断言原语；工作量大小时限
       （MIN_CHECKS）防止把差分删成空壳。

动态（可执行证据，构建产物存在时执行）：
    6. 以多组 env 矩阵运行差分测试可执行文件，要求 exit 0、检查数 >= MIN_CHECKS；
    7. --probe 模式逐 getter 对比 module vs legacy，并对照独立登记的期望值
       （覆盖 0 / 上界 / clamp / strtoul base-0 / 空值 / 溢出 / 负号 边界）。

边界（本门禁不覆盖，不得据此宣称"职责已迁完"或"已稳定"）：
    - 只覆盖 M1 的纯判定；M2/M3/M4 未做；
    - 动态部分只证明"同一输入 + 同一替身世界下两个实现一致"，不证明替身本身；
    - device.cpp 侧调用点编排不在覆盖范围内。
"""

from pathlib import Path
import hashlib
import os
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]

BUDGET_GATE = ROOT / "AutoTest/test_device_semantic_responsibility_budget_static.py"
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp"
LEGACY_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_device_semantic_predicates_legacy_reference.inc"
)
TEST_CPP = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_device_semantic_predicates_test.cpp"
)
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = (
    ROOT / "build32/src/d3d9/war3_device_semantic_predicates_test.exe"
)
RECORD_DOC = (
    ROOT / "docs/plan/2026-09-17-m1-migration-equivalence-record.md"
)

# 迁移前（git HEAD = ae890542d766 工作树，迁移尚未应用）的
# src/d3d9/d3d9_device.cpp blob 身份。故意改动 legacy 参考的 provenance → 红。
PRE_DEVICE_SHA256 = (
    "3541DF5FE02EB9045B4A8499A1E7F2CBD2AB30DFF07896AFE1AAE43F1C847454"
)
PRE_DEVICE_COMMIT = "ae890542d766"
# legacy 参考文件自身的身份（生成后登记；改动即需重新审阅并更新本文件）。
LEGACY_INC_SHA256 = (
    "2EA43F9782BE388E0FC3C98BDD8F60972354C0E8444EEF4A75E988C84E7B7968"
)

# 本记录覆盖的 M1 已迁出符号（= 预算门禁 FROZEN 中 budget < baseline 的集合）。
COVERED = {
    "War3IsEligibleSemanticDynamicUnit",
    "War3IsEligibleSemanticStaticWorldCaster",
    "War3PacketIsPathBlocker",
    "War3ResolveSemanticPacketObjectKind",
    "War3ResolveSemanticPacketObjectKindFast",
    "War3ScoreSemanticSceneFrame",
    "War3SemanticDirectRecordSelectionKey",
    "War3SemanticDirectSelectionKey",
    "War3SemanticObjectGroupedSelectionRuntime",
    "War3SemanticRejectAlphaBlendCasterRuntime",
    "War3SemanticRejectUnsafeAlphaCasterRuntime",
    "War3SemanticStickyPartSelectionMinRecordsRuntime",
    "War3SemanticStickyPartSelectionRuntime",
    "War3SemanticStickySelectionBroadLeasePreferenceRuntime",
    "War3SemanticStickySelectionFillMarginRuntime",
    "War3SemanticStickySelectionFillRuntime",
    "War3SemanticStickySelectionLeaseFramesRuntime",
    "War3SemanticStickySelectionLeaseRuntime",
    "War3SemanticValidateUnitCoreRuntime",
    "War3ShouldPreferSemanticSceneFrame",
    "War3ShouldSubmitSemanticPacket",
    "War3ShouldSubmitSemanticPacketFast",
    "War3ShadowIsLosBlockerByJHandleFallback",
    "War3ShadowIsLosBlockerByWidgetPtr",
    "War3WidgetNegativeFrameCacheRuntime",
    "War3WidgetProbeSafeCopyRuntime",
}

# 差分测试的下限断言数（当前实测 3,098,194；留足余量但仍能挡住空壳化）。
MIN_CHECKS = 3_000_000
CHECK_RE = re.compile(
    r"device semantic predicates equivalence:\s*(\d+)/(\d+)\s*checks passed"
)

# getter 的独立期望值表（按 env 场景）。键 = --probe 输出的 name。
PROBE_SCENARIOS = [
    (
        "default",
        {},
        {
            "War3SemanticValidateUnitCoreRuntime": 1,
            "War3SemanticObjectGroupedSelectionRuntime": 1,
            "War3SemanticStickySelectionLeaseRuntime": 1,
            "War3SemanticStickySelectionBroadLeasePreferenceRuntime": 0,
            "War3SemanticStickySelectionFillRuntime": 1,
            "War3SemanticStickyPartSelectionRuntime": 1,
            "War3SemanticRejectUnsafeAlphaCasterRuntime": 0,
            "War3SemanticRejectAlphaBlendCasterRuntime": 1,
            "War3WidgetProbeSafeCopyRuntime": 1,
            "War3WidgetNegativeFrameCacheRuntime": 1,
            "War3SemanticStickySelectionLeaseFramesRuntime": 120,
            "War3SemanticStickySelectionFillMarginRuntime": 8,
            "War3SemanticStickyPartSelectionMinRecordsRuntime": 16,
            "War3WidgetNegativeFrameCacheTtlFrames": 8,
            "War3GetEnvU32.dedicated": 7,
        },
    ),
    (
        "all-zero-rollback",
        {
            "DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE": "0",
            "DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION": "0",
            "DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER": "0",
            "DXVK_WAR3_WIDGET_PROBE_SAFE_COPY": "0",
            "DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL_MARGIN": "0",
            "DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION_MIN_RECORDS": "0",
            "DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES": "0",
            "DXVK_WAR3_M1_PROBE_VALUE": "abc",
        },
        {
            "War3SemanticValidateUnitCoreRuntime": 0,
            "War3SemanticObjectGroupedSelectionRuntime": 0,
            "War3SemanticStickySelectionLeaseRuntime": 0,
            "War3SemanticStickySelectionFillRuntime": 0,
            "War3SemanticStickyPartSelectionRuntime": 0,
            "War3SemanticRejectAlphaBlendCasterRuntime": 0,
            "War3WidgetProbeSafeCopyRuntime": 0,
            "War3WidgetNegativeFrameCacheRuntime": 0,
            # max(1, 0) / min(64, 0) / max(1, 0) / clamp(0,1,120)
            "War3SemanticStickySelectionLeaseFramesRuntime": 1,
            "War3SemanticStickySelectionFillMarginRuntime": 0,
            "War3SemanticStickyPartSelectionMinRecordsRuntime": 1,
            "War3WidgetNegativeFrameCacheTtlFrames": 1,
            # "abc" 无数字 → strtoul 无消费 → fallback
            "War3GetEnvU32.dedicated": 7,
        },
    ),
    (
        "upper-clamps",
        {
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES": "100000",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL_MARGIN": "100",
            "DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION_MIN_RECORDS": "1000000",
            "DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES": "1000",
            "DXVK_WAR3_M1_PROBE_VALUE": "0x1F",
        },
        {
            "War3SemanticStickySelectionLeaseFramesRuntime": 100000,
            "War3SemanticStickySelectionFillMarginRuntime": 64,
            "War3SemanticStickyPartSelectionMinRecordsRuntime": 1000000,
            "War3WidgetNegativeFrameCacheTtlFrames": 120,
            "War3GetEnvU32.dedicated": 31,
        },
    ),
    (
        "base0-and-sign",
        {
            # "08": base 0 视为八进制，"8" 非法 → 只消费 "0" → 0
            "DXVK_WAR3_M1_PROBE_VALUE": "08",
            # "-1": strtoul 取反 → 0xFFFFFFFF（32 位 unsigned long）
            "DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION": "2",
            "DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES": "0x10",
        },
        {
            "War3GetEnvU32.dedicated": 0,
            "War3SemanticObjectGroupedSelectionRuntime": 1,
            "War3WidgetNegativeFrameCacheTtlFrames": 16,
        },
    ),
    (
        "octal-and-empty",
        {
            "DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES": "010",
            "DXVK_WAR3_WIDGET_PROBE_SAFE_COPY": "",
            "DXVK_WAR3_M1_PROBE_VALUE": "12abc",
        },
        {
            "War3WidgetNegativeFrameCacheTtlFrames": 8,
            "War3WidgetProbeSafeCopyRuntime": 1,
            "War3GetEnvU32.dedicated": 12,
        },
    ),
    (
        "unsigned-negation",
        {
            "DXVK_WAR3_M1_PROBE_VALUE": "-1",
            "DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES": "-1",
        },
        {
            "War3GetEnvU32.dedicated": 0xFFFFFFFF,
            # max(1, 0xFFFFFFFF)
            "War3SemanticStickySelectionLeaseFramesRuntime": 0xFFFFFFFF,
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


def parse_frozen(text):
    """解析预算门禁的 FROZEN 表。

    不依赖行尾：该文件混用 CRLF/CR 行尾，按 "NAME": (baseline, budget) 直接抽取。
    """
    start = text.index("FROZEN = {")
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
            "rollback-branches",
            {
                "DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE": "0",
                "DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER": "1",
                "DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER": "0",
                "DXVK_WAR3_WIDGET_PROBE_SAFE_COPY": "0",
                "DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE": "0",
                "DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION": "0",
            },
        ),
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

    for scenario, overrides, expected in PROBE_SCENARIOS:
        observed = run_probe(scenario, overrides)
        for name, value in expected.items():
            if observed.get(name) != value:
                fail(
                    f"--probe 场景 {scenario}: {name} 期望 {value}，实测 "
                    f"{observed.get(name)}"
                )


def main():
    module_text = read(MODULE_H) + "\n" + read(MODULE_CPP)
    legacy_text = read(LEGACY_INC)
    test_text = read(TEST_CPP)
    meson_text = read(MESON)
    budget_text = read(BUDGET_GATE)
    read(RECORD_DOC)

    # 1. provenance：legacy 参考点名迁移前的 device.cpp 身份，且自身身份被登记。
    if PRE_DEVICE_SHA256 not in legacy_text:
        fail("legacy 参考缺少迁移前 d3d9_device.cpp 的 SHA-256 provenance")
    if PRE_DEVICE_COMMIT not in legacy_text:
        fail("legacy 参考缺少迁移前 commit provenance")
    actual_legacy_sha = sha256_bytes(LEGACY_INC.read_bytes())
    if actual_legacy_sha != LEGACY_INC_SHA256:
        fail(
            "legacy 参考文件哈希与登记值不一致："
            f"{actual_legacy_sha} != {LEGACY_INC_SHA256}"
        )

    # 2. 差分测试必须真的引用 legacy 参考并包含差分断言原语。
    if (
        "war3_device_semantic_predicates_legacy_reference.inc"
        not in test_text
    ):
        fail("差分测试没有 include 迁移前 legacy 参考")
    for primitive in ("legacy::", "DIFF_U64(", "RunProbe", "--probe"):
        if primitive not in test_text:
            fail(f"差分测试缺少必需原语：{primitive}")

    # 3. meson 目标必须链接真实模块 .cpp（否则差分测的是假实现）。
    target = re.search(
        r"war3_device_semantic_predicates_test = executable\((.*?)\n\)",
        meson_text,
        re.DOTALL,
    )
    if target is None:
        fail("meson.build 里找不到 war3_device_semantic_predicates_test 目标")
    if "war3/semantic/war3_device_semantic_predicates.cpp" not in target.group(1):
        fail("差分测试目标没有链接真实模块 .cpp")
    if "war3_device_semantic_predicates_test.cpp" not in target.group(1):
        fail("差分测试目标没有包含测试源文件")

    # 4. 覆盖：预算门禁里已迁出的每个符号都必须有 sem::/legacy:: 双侧调用点。
    frozen = parse_frozen(budget_text)
    if len(frozen) < 100:
        fail(f"fail-closed: 预算门禁 FROZEN 只解析出 {len(frozen)} 个符号")
    migrated = {
        name
        for name, (baseline, budget) in frozen.items()
        if budget < baseline
    }
    if not migrated:
        fail("fail-closed: 预算门禁里没有解析到已迁出符号")
    uncovered = sorted(migrated - COVERED)
    if uncovered:
        fail(
            "以下符号已迁出 device.cpp，但没有等价性证据（请补差分覆盖并"
            "更新本门禁的 COVERED）：\n  " + "\n  ".join(uncovered)
        )
    stale = sorted(COVERED - migrated)
    if stale:
        fail(
            "以下符号被列为本门禁的覆盖集合，但预算门禁显示它们并未迁出"
            "（迁移状态或证据已过期）：\n  " + "\n  ".join(stale)
        )

    missing_module = sorted(
        name for name in migrated if name not in module_text
    )
    if missing_module:
        fail("以下已迁出符号在语义模块里看不到：\n  " + "\n  ".join(missing_module))

    missing_legacy = sorted(name for name in migrated if name not in legacy_text)
    if missing_legacy:
        fail(
            "以下已迁出符号在 legacy 参考里看不到（迁移前实现缺失）：\n  "
            + "\n  ".join(missing_legacy)
        )

    not_compared = []
    for name in sorted(migrated):
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

    check_dynamic()

    print(
        "device semantic predicates equivalence gate static checks passed"
        f"（已迁出符号 {len(migrated)} 个全部有 legacy 差分覆盖；"
        f"legacy 参考 {LEGACY_INC.stat().st_size} B / SHA-256 "
        f"{actual_legacy_sha[:16]}…；差分下限 {MIN_CHECKS} 断言；"
        f"probe 场景 {len(PROBE_SCENARIOS)} 组）"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
