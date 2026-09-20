"""M2-5 palette taxonomy 发射块迁移的等价性证据门禁（fail-closed，独立成文件）。

背景：M2-5 把 d3d9_device.cpp :21202-21580 的 skinned palette taxonomy 发射块
（34 个 War3ShadowCaptureStats 字段 + 3 张跨帧 thread_local 探针表）抽成
    src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp}
的纯函数 War3EmitSemanticPaletteTaxonomy（输入全部显式传参，不访问 device 成员
或 registry/hook 全局）。

为什么需要**独立**门禁：该函数名不匹配预算门禁
（AutoTest/test_device_semantic_responsibility_budget_static.py）的
SEMANTIC_FAMILY_RE 语义选择命名族（名字里没有 Resolve/Select/Runtime/Key/...
任何后缀），因此那条门禁的判定 3（回流）对它盲。M2-5 把它登记进 FROZEN_M2_5
（baseline=0：迁移前 device.cpp 里根本不存在该符号、该块也不占任何行首站点；
budget=0）后，只有判定 1（预算）能咬住回流。本文件负责钉住**等价性证据本身**，
与 M2-1/M2-2/M2-3 的记录同型。

静态（fail-closed，任何一条不满足即失败）：
 1. 证据文件齐全：legacy 参考、入库生成器、预算门禁、模块 .h/.cpp、差分测试、
    meson 目标链接真实模块 .cpp、M2-5 等价记录文档。
 2. legacy 参考 provenance：必须记录迁移前**工作树** d3d9_device.cpp 的 SHA-256、
    字节数、捕获时间，并显式说明与 M1 git-blob provenance 的差异；文件自身
    SHA-256 与字节数与登记值一致（故意改参考文本 → 红）。
 3. 模块 .cpp 的 BEGIN/END 标记之间的正文与 legacy 参考的正文**逐字节相同**
    （防止"迁移文本漂移"或"参考与实现不一致"）。
 4. 34 个字段的单一实现：每个名字在 d3d9_war3_scene.h 恰好 1 处（定义）、在
    device.cpp 出现 0 次、在模块 .cpp 的出现次数等于登记表（4 个字段为 2：
    ProducerPartPacket / Unknown 的双累加点，以及两个 UniqueInWindowMax 的读+写）。
 5. 三张探针表仍是函数内 static thread_local 定长 std::array + 位掩码取槽
    （5 个数组：8192 项 keys+entries ×2 组 + lease 属性表 1 个）。
 6. device.cpp 只剩调用点（1 处），不再含诊断门字符串 / 块正文 / 任何字段名。
 7. meson：dll 源与差分测试目标都包含真实模块 .cpp。
 8. FROZEN_M2_5 的符号集合必须**恰好**等于本门禁登记的 COVERED，且每条为
    (0, 0)。

动态（可执行证据，构建产物存在时执行）：
 9. --probe-taxonomy 在两 env 下运行：诊断门 ON 时逐场景对照**独立期望值表**；
    诊断门 OFF（默认 env）时全部场景必须全 0（证明第一道门本身）。两侧
    module/legacy 必须逐字段相等。
10. 差分测试在 diagnostics-on env 下 exit 0、全过、检查数 >= MIN_CHECKS
    （防把差分删成空壳）。

边界（本门禁不覆盖，不得据此宣称"职责已迁完"或"已稳定"）：
    - device.cpp 侧 B1/B2 调用点编排（live palette refresh 块与来源状态装配）
      未迁移、也未被本门禁的差分覆盖；
    - 34 个字段从 bridge/hub/control-plane/perf 到报告的**出口链**由
      test_active_path_palette_instrumentation_export_static.py 等既有门禁覆盖；
    - 差分与 probe 只证明"同一输入 + 同一 stats 状态序列下两个实现一致"，且
      VisibleRenderableRegistry::computeShadowManifestPartKey 在宿主机是替身；
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
MODULE_H = ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.h"
MODULE_CPP = ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp"
TAXONOMY_INC = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_palette_taxonomy_emission_legacy_reference.inc"
)
TEST_CPP = (
    ROOT / "src/d3d9/war3/semantic/tests/war3_live_palette_selection_test.cpp"
)
DEVICE_CPP = ROOT / "src/d3d9/d3d9_device.cpp"
SCENE_H = ROOT / "src/d3d9/d3d9_war3_scene.h"
MESON = ROOT / "src/d3d9/meson.build"
TEST_EXE = ROOT / "build32/src/d3d9/war3_live_palette_selection_test.exe"
RECORD_DOC = ROOT / "docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md"

# 迁移前（M2-3 迁移已应用、M2-5 未应用）的**未提交工作树**
# src/d3d9/d3d9_device.cpp 身份（= M2-3 记录的"迁移后 device.cpp"，同一 SHA，
# 交叉验证闭合）。与 M1 不同：这不是 git blob——工作成果未提交。故意改动 → 红。
PRE_M2_5_DEVICE_SHA256 = (
    "D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE"
)
PRE_M2_5_DEVICE_SIZE = 2_273_048
PRE_M2_5_CAPTURED_AT = "2026-09-18T03:05:30"
# legacy 参考与模块 .cpp/.h 的身份（生成/落地后登记；改动即需重新审阅）。
TAXONOMY_INC_SHA256 = (
    "A5333AC7E7F5A080324F5D83668D394C53F26E7ADEEEC5DA2C76E9BFEE26105E"
)
TAXONOMY_INC_SIZE = 22_511
MODULE_CPP_SHA256 = (
    "E8C6F74A44F6F311ABA04759F358957C979FA4FE2A52DD09B378BD7405BFE436"
)
MODULE_H_SHA256 = (
    "5F8C3F29B78A9403017AF08CA88702C163A10F5C078785691A631D0B9B0205A8"
)

BEGIN_MARK = (
    "    // --- BEGIN M2-5 verbatim pre-migration emission block"
    " (d3d9_device.cpp :21202-21580, minus :21203) ---"
)
END_MARK = "    // --- END M2-5 verbatim pre-migration emission block ---"

# 本门禁覆盖的 M2-5 符号（= 预算门禁 FROZEN_M2_5 的键集合）。
COVERED = ("War3EmitSemanticPaletteTaxonomy",)

PALETTE_DIAG = "DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"

# diagnostics-on 矩阵的检查数下限（按实测 30,443,383 的 ~69% 登记）。
MIN_CHECKS = 21_000_000

K_FIELD_COUNT = 34
FIELD_NAMES = [
    "semanticSceneSubmittedSkinnedPaletteSourceNoneCount",
    "semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount",
    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount",
    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount",
    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount",
    "semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount",
    "semanticSceneSubmittedSkinnedPaletteSourceOwnedPartSnapshotCount",
    "semanticSceneSubmittedSkinnedPaletteSourceChurnCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceTrustedBlendedWriterCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceRawGlobalArenaCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceRangeCopyPoseRebuildCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceCModelFallbackCount",
    "semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount",
    "semanticSceneSubmittedSkinnedPaletteStablePartSampleCount",
    "semanticSceneSubmittedSkinnedPaletteHashChurnCount",
    "semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount",
    "semanticSceneSubmittedSkinnedPaletteCountChurnCount",
    "semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax",
    "semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax",
    "semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount",
    "semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount",
    "semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount",
    "semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount",
]

# 模块 .cpp 内每个字段名应有的出现次数（默认 1）。4 个例外来自发射块本身：
#   * ProducerPartPacket：DrawTimeCaptured 的 case 与 OwnedPartSnapshot 的成对
#     累加（清册 §2.1 明确"成对累加不得拆散"）；
#   * Unknown：switch 的 default 与 currentDrawSample == nullptr 的 else；
#   * Hash/SlotIndexUniqueInWindowMax：`if (unique... > stats.X)` 的读 + 写入。
FIELD_MODULE_COUNTS = {
    "semanticSceneSubmittedSkinnedPaletteProvenanceProducerPartPacketCount": 2,
    "semanticSceneSubmittedSkinnedPaletteProvenanceUnknownCount": 2,
    "semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax": 2,
    "semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax": 2,
}

CHECK_RE = re.compile(
    r"war3 live palette selection equivalence:\s*(\d+)/(\d+)\s*checks passed"
)
PROBE_RE = re.compile(
    r"probe-taxonomy\s+(\S+)\s+module=([0-9,]+)\s+legacy=([0-9,]+)\s*$"
)

# --probe-taxonomy 的独立期望值表（诊断门 ON；字段顺序 = FIELD_NAMES）。
# 每行的 key 都是该进程内首次出现，因此期望值是确定的绝对量。
PROBE_EXPECT = [
    ("none", [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("dtc-unknown-outofrange", [0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("dtc-trusted", [0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("dtc-null-sample", [0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("owned-part-snapshot", [0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("sources-slot-blended-published-cmodel", [0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("global-stable-pair", [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]),
    ("hash-churn-large-delta", [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1]),
    ("lease-multi-both", [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0]),
    ("stale-restore", [0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("window-wrap", [0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 0, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 4, 0, 0]),
    ("dtc-provenance-2-4-5", [0, 3, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("source-churn", [0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]),
    ("count-churn", [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0]),
    ("medium-delta", [0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0]),
    ("stale-then-live-large", [0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 2, 1, 1, 1, 0, 0, 2, 2, 0, 0, 0, 2]),
]


def fail(message):
    raise AssertionError(message)


def read(path):
    if not path.is_file():
        fail(f"fail-closed: 缺少必需文件 {path}")
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


def parse_frozen_m2_5(text):
    start = text.index("FROZEN_M2_5 = {")
    end = text.index("\n}", start)
    frozen = {}
    for match in re.finditer(
        r"\"([A-Za-z_][A-Za-z0-9_]*)\"\s*:\s*\((\d+),\s*(\d+)\)",
        text[start:end],
    ):
        frozen[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    return frozen


def run_probe_taxonomy(scenario, overrides, expected_rows, require_zero):
    proc = subprocess.run(
        [str(TEST_EXE), "--probe-taxonomy"],
        cwd=str(ROOT),
        env=sanitized_env(overrides),
        capture_output=True,
        text=True,
        timeout=120,
    )
    if proc.returncode != 0:
        fail(
            f"--probe-taxonomy 场景 {scenario} exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    observed = {}
    for line in proc.stdout.splitlines():
        match = PROBE_RE.match(line)
        if not match:
            continue
        name, module_fields, legacy_fields = match.groups()
        if module_fields != legacy_fields:
            fail(
                f"--probe-taxonomy 场景 {scenario}: {name} module={module_fields}"
                f" legacy={legacy_fields}（迁移前后不一致）"
            )
        observed[name] = [int(field) for field in module_fields.split(",")]
    if not observed:
        fail(f"--probe-taxonomy 场景 {scenario} 没有解析到任何场景输出")
    for name, expected in expected_rows:
        actual = observed.get(name)
        if actual is None:
            fail(f"--probe-taxonomy 场景 {scenario}: 缺少场景 {name} 的输出")
        if len(actual) != K_FIELD_COUNT:
            fail(
                f"--probe-taxonomy 场景 {scenario}: {name} 字段数 "
                f"{len(actual)} != {K_FIELD_COUNT}"
            )
        want = [0] * K_FIELD_COUNT if require_zero else expected
        for index, (got, expected_value) in enumerate(zip(actual, want)):
            if got != expected_value:
                fail(
                    f"--probe-taxonomy 场景 {scenario}: {name} 字段 "
                    f"#{index} ({FIELD_NAMES[index]}) 期望 {expected_value}，"
                    f"实测 {got}"
                )
    return observed


def check_dynamic():
    if not TEST_EXE.is_file():
        fail(
            "fail-closed: 缺少差分测试可执行文件 "
            f"{TEST_EXE.relative_to(ROOT)}（M2-5 动态等价证据不可缺省）"
        )
    run_probe_taxonomy("diagnostics-on", {PALETTE_DIAG: "1"}, PROBE_EXPECT, False)
    run_probe_taxonomy("default", {}, PROBE_EXPECT, True)
    proc = subprocess.run(
        [str(TEST_EXE)],
        cwd=str(ROOT),
        env=sanitized_env({PALETTE_DIAG: "1"}),
        capture_output=True,
        text=True,
        timeout=600,
    )
    match = CHECK_RE.search(proc.stdout)
    if proc.returncode != 0 or match is None:
        fail(
            f"差分测试 env 矩阵 diagnostics-on 失败 exit={proc.returncode}\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    passed, total = int(match.group(1)), int(match.group(2))
    if passed != total:
        fail(f"差分测试 env 矩阵 diagnostics-on: {passed}/{total}（要求全过）")
    if total < MIN_CHECKS:
        fail(
            f"差分测试 env 矩阵 diagnostics-on: {total} 低于 M2-5 登记下限 "
            f"{MIN_CHECKS}（差分被删成空壳？）"
        )
    return total


def main():
    module_text = read(MODULE_H) + "\n" + read(MODULE_CPP)
    inc_text = read(TAXONOMY_INC)
    test_text = read(TEST_CPP)
    device_text = read(DEVICE_CPP)
    scene_text = read(SCENE_H)
    meson_text = read(MESON)
    budget_text = read(BUDGET_GATE)
    read(GENERATOR)
    read(RECORD_DOC)

    # 1. legacy 参考 provenance 与自身身份。
    if PRE_M2_5_DEVICE_SHA256 not in inc_text:
        fail("legacy 参考缺少 pre-M2-5 d3d9_device.cpp 的 SHA-256 provenance")
    if str(PRE_M2_5_DEVICE_SIZE) not in inc_text:
        fail("legacy 参考缺少 pre-M2-5 d3d9_device.cpp 的字节数 provenance")
    if PRE_M2_5_CAPTURED_AT not in inc_text:
        fail("legacy 参考缺少 pre-M2-5 工作树文件的捕获时间 provenance")
    if "file hash, not a git blob hash" not in inc_text:
        fail("legacy 参考缺少 worktree-vs-git-blob 差异说明")
    if "gen_war3_live_palette_selection_legacy_reference.py" not in inc_text:
        fail("legacy 参考没有点名入库生成器")
    if "using namespace dxvk;" not in inc_text:
        fail("legacy 参考缺少共享契约类型的解析路径")
    if ":21202-21580" not in inc_text:
        fail("legacy 参考缺少迁移块行号 provenance")
    inc_bytes = TAXONOMY_INC.read_bytes()
    actual_inc_sha = sha256_bytes(inc_bytes)
    if actual_inc_sha != TAXONOMY_INC_SHA256:
        fail(
            "legacy 参考文件哈希与登记值不一致："
            f"{actual_inc_sha} != {TAXONOMY_INC_SHA256}"
        )
    if len(inc_bytes) != TAXONOMY_INC_SIZE:
        fail(
            "legacy 参考字节数与登记值不一致："
            f"{len(inc_bytes)} != {TAXONOMY_INC_SIZE}"
        )
    mod_cpp_bytes = MODULE_CPP.read_bytes()
    if sha256_bytes(mod_cpp_bytes) != MODULE_CPP_SHA256:
        fail("模块 .cpp 哈希与登记值不一致（迁移文本已漂移）")
    if sha256_bytes(MODULE_H.read_bytes()) != MODULE_H_SHA256:
        fail("模块 .h 哈希与登记值不一致")

    # 2. 模块正文与 legacy 正文逐字节相同（迁移文本一致性）。
    module_body = extract_body(module_text)
    inc_body = extract_body(inc_text)
    if module_body != inc_body:
        fail("模块 .cpp 与 legacy 参考的发射块正文不一致（逐字节比对失败）")
    module_body_bytes = len(module_body.encode("utf-8"))
    if module_body_bytes < 18_000:
        fail(f"发射块正文只有 {module_body_bytes} B，疑似被截断")

    # 3. 34 个字段的单一实现。
    for name in FIELD_NAMES:
        if scene_text.count(name) != 1:
            fail(f"scene.h 中字段定义不是恰好 1 处：{name}")
        if device_text.count(name) != 0:
            fail(f"device.cpp 仍出现已迁出字段名（双重计数风险）：{name}")
        want = FIELD_MODULE_COUNTS.get(name, 1)
        got = module_text.count(name)
        if got != want:
            fail(f"模块中字段出现次数 {got} != {want}：{name}")

    # 4. 三张探针表仍是函数内 static thread_local 定长 std::array。
    if module_text.count("static thread_local std::array<") != 5:
        fail("模块内 static thread_local std::array 不是 5 个（3 张表 5 个数组）")
    for table, entries in (
        ("kPaletteProbeEntries", "8192u"),
        ("kLeaseAttrEntries", "8192u"),
        ("kStrictProbeEntries", "8192u"),
    ):
        decl = f"static constexpr size_t {table} = {entries};"
        if decl not in module_text:
            fail(f"探针表容量声明丢失或改变：{decl}")
        if f"& ({table} - 1u)" not in module_text:
            fail(f"探针表不再用位掩码取槽：{table}")

    # 5. device.cpp 只剩调用点。
    if device_text.count("War3EmitSemanticPaletteTaxonomy(") != 1:
        fail("device.cpp 的 War3EmitSemanticPaletteTaxonomy 调用点不是恰好 1 处")
    if "if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {" in device_text:
        fail("device.cpp 仍含发射块的诊断门字符串（块未真正迁出）")
    for probe in ("s_paletteProbeKeys", "s_leaseAttrEntries", "s_strictProbeKeys"):
        if probe in device_text:
            fail(f"device.cpp 仍含探针表 {probe}")

    # 6. 差分测试证据原语。
    if "war3_palette_taxonomy_emission_legacy_reference.inc" not in test_text:
        fail("差分测试没有 include M2-5 legacy 参考")
    for name in COVERED:
        if f"sem::{name}(" not in test_text:
            fail(f"差分测试缺少模块侧调用点：sem::{name}(")
        if f"legacy::{name}(" not in test_text:
            fail(f"差分测试缺少 legacy 侧调用点：legacy::{name}(")
    for primitive in ("--probe-taxonomy", "RunProbeTaxonomy", "DiffTaxonomyStructBytes"):
        if primitive not in test_text:
            fail(f"差分测试缺少 M2-5 必需原语：{primitive}")

    # 7. meson：dll 源与差分测试目标都链接真实模块 .cpp。
    if "war3/semantic/war3_palette_taxonomy_emission.cpp" not in meson_text:
        fail("meson.build 没有把 M2-5 模块加进任何目标")
    target = re.search(
        r"war3_live_palette_selection_test = executable\((.*?)\n\)",
        meson_text,
        re.DOTALL,
    )
    if target is None:
        fail("meson.build 里找不到 war3_live_palette_selection_test 目标")
    if "war3/semantic/war3_palette_taxonomy_emission.cpp" not in target.group(1):
        fail("差分测试目标没有链接 M2-5 真实模块 .cpp")

    # 8. FROZEN_M2_5 恰好等于 COVERED，且每条 (0, 0)。
    frozen = parse_frozen_m2_5(budget_text)
    if set(frozen) != set(COVERED):
        fail(
            "预算门禁 FROZEN_M2_5 与本门禁登记集合不一致：\n"
            f"  FROZEN_M2_5: {sorted(frozen)}\n"
            f"  COVERED: {sorted(COVERED)}"
        )
    wrong = sorted(
        f"{name}={pair} (要求 (0, 0))"
        for name, pair in frozen.items()
        if pair != (0, 0)
    )
    if wrong:
        fail("FROZEN_M2_5 的 baseline/budget 与实测口径不符：\n  " + "\n  ".join(wrong))

    total = check_dynamic()

    print(
        "war3 palette taxonomy equivalence gate static checks passed"
        f"（M2-5 已迁出符号 {len(COVERED)} 个；34 个 taxonomy 字段单一实现；"
        f"legacy 参考 {TAXONOMY_INC_SIZE} B / SHA-256 {actual_inc_sha[:16]}…；"
        f"模块正文与 legacy 正文逐字节相同 {module_body_bytes} B；"
        f"pre-M2-5 device.cpp {PRE_M2_5_DEVICE_SIZE} B / "
        f"{PRE_M2_5_DEVICE_SHA256[:16]}…；probe-taxonomy 场景 "
        f"{len(PROBE_EXPECT)} × 2 组 env；diagnostics-on 差分 {total} 断言"
        f"（下限 {MIN_CHECKS}））"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
