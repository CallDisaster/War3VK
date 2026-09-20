"""M2-1/M2-2 live palette selection 迁移的 legacy 参考生成器（入库版）。

背景：M1 的等价记录指出"M1 生成器未入库"是已知缺口；M2-1 把生成器入库，
使 src/d3d9/war3/semantic/tests/war3_live_palette_selection_legacy_reference.inc
可被审计者从**迁移前**的 d3d9_device.cpp 快照确定性重建。
M2-2 以同一生成器加 `--m2-2` 模式产出**第二份** legacy 参考
src/d3d9/war3/semantic/tests/war3_live_palette_selection_chain_legacy_reference.inc
（选择链本体；M2-1 的 .inc 与其 SHA 钉死不动）。
M2-3 以 `--m2-3` 模式产出**第三份** legacy 参考
src/d3d9/war3/semantic/tests/war3_live_palette_selection_motion_legacy_reference.inc
（motion 诊断三函数 + 各自 Entry 结构；M2-1/M2-2 的 .inc 与其 SHA 钉死不动）。

用法：
    py AutoTest/gen_war3_live_palette_selection_legacy_reference.py [pre_m2_device_cpp]
    py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-2 [pre_m2_2_device_cpp]
    py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-3 [pre_m2_3_device_cpp]

    pre_m2_device_cpp 缺省为 %TEMP%\\device_pre_m2_1.cpp —— 即 M2-1 迁移**之前**
    对未提交工作树 src/d3d9/d3d9_device.cpp 的逐字节副本（捕获时刻与 SHA-256
    记录在生成文件的头部 provenance 中）。
    --m2-2 的输入缺省为 %TEMP%\\device_pre_m2_2.cpp —— M2-2 迁移**之前**的
    未提交工作树 device.cpp（= M2-1 迁移完成后的状态，SHA 与 M2-1 记录 §1
    "迁出后 d3d9_device.cpp" 一致，可交叉验证）。
    --m2-3 的输入缺省为 %TEMP%\\device_pre_m2_3.cpp —— M2-3 迁移**之前**的
    未提交工作树 device.cpp（= M2-2 迁移完成后的状态，SHA 与 M2-2 记录 §2
    "迁移后 device.cpp" 一致，可交叉验证）。

机械处理（与 M1 .inc 同型，此外不改任何 token）：
    1. 外层命名空间从 device.cpp 的匿名命名空间改为 m2_legacy_reference；
    2. 行尾归一化为 LF（device.cpp 为 CRLF；M1 的 git blob provenance 同样
       是 LF 归一后的文本）；
    3. War3GetEnvU32 作为 **supporting** 定义一并逐字节抽取：它在 M1 已迁往
       war3_device_semantic_predicates.cpp（不属于本轮 8 个迁移符号，因此
       pre-M2 的 device.cpp 里并不存在它），抽取源是 M1 模块当前文本——
       这也正是 pre-M2 device.cpp 里 4 个 env getter 当时的真实调用目标。
       头部注释明确标注这一点。
    --m2-2 模式的 supporting 定义改为：Gap A 计数器
       g_devicePaletteSlotCacheServedAfterConfirmCount /
       g_devicePaletteSlotCacheRejectedStaleCount（pre-M2-2 device.cpp
       :1060-1061 的定义行逐字节抽取；其 device 布局注释不随迁，见 .inc 头部），
       以及选择链引用的共享类型块（War3SemanticPaletteSource 枚举、
       War3LivePaletteBuildPhase 枚举、kWar3LivePaletteBuildPhaseCount、
       War3LivePaletteBuildTiming、War3LivePaletteBuildRawTiming，
       pre-M2-2 :5883-5967 逐字节抽取）。选择链本体对 M2-1 helper/env getter
       的调用为**无限定**形式，在 m2_legacy_reference 命名空间内解析到
       M2-1 .inc 的 legacy 副本（包含顺序要求见 .inc 头部注释）。

与 M1 provenance 的差异（必须显式说明）：M1 的迁移前文本是**已提交**的
git blob（ae890542d766:src/d3d9/d3d9_device.cpp）；M2-1/M2-2 的迁移前状态是
**未提交的工作树文件**（M1/M2-1 工作成果尚未提交），git 里不存在对应 blob，
因此本生成器以工作树文件副本为源，provenance 记录工作树文件 SHA-256、
字节数与捕获时间。
"""

from pathlib import Path
import hashlib
import os
import sys


ROOT = Path(__file__).resolve().parents[1]
OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_legacy_reference.inc"
)
CHAIN_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_chain_legacy_reference.inc"
)
M1_MODULE_CPP = (
    ROOT / "src/d3d9/war3/semantic/war3_device_semantic_predicates.cpp"
)

# 第 0 步捕获的 pre-M2 工作树身份（见 docs/plan/2026-09-18-m2-1-migration-
# equivalence-record.md）。CAPTURED_AT 是机器本地时间的记录常量，保持生成输出
# 确定性。
CAPTURED_AT = "2026-09-17T21:11:53 (machine local time)"
EXPECTED_SIZE = 2_314_463
EXPECTED_SHA256 = (
    "2736335B42FCF4947B728F2907A04FE9F63EC2E5C8683A887D0785432E499F1B"
)

# (marker 注释名, 定义锚签名)。顺序 = pre-M2 device.cpp 行号升序，与 M1 .inc 同型。
# War3GetEnvU32 不在此列：它在 M1 已迁出 device.cpp，单独从 M1 模块逐字节抽取
# （见 main()）。
SUPPORTING_SIGNATURE = (
    "uint32_t War3GetEnvU32(const char *name, uint32_t fallback) {"
)
SYMBOLS = [
    # 本轮 8 个迁移符号
    ("War3SemanticLivePaletteSafeCopyRuntime",
     "bool War3SemanticLivePaletteSafeCopyRuntime() {"),
    ("War3SemanticLivePaletteRefreshRuntime",
     "bool War3SemanticLivePaletteRefreshRuntime() {"),
    ("War3SemanticLivePaletteAllowCModelFallbackRuntime",
     "bool War3SemanticLivePaletteAllowCModelFallbackRuntime() {"),
    ("War3SemanticPaletteDiagnosticsRuntime",
     "inline bool War3SemanticPaletteDiagnosticsRuntime() {"),
    ("War3SemanticHashMatrixPalette",
     "uint64_t War3SemanticHashMatrixPalette(const Matrix4* matrices,"),
    ("War3DecodeRuntimePoseMatrix48",
     "Matrix4 War3DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {"),
    ("War3TryReadRuntimePoseArray",
     "bool War3TryReadRuntimePoseArray(void* runtimeModelPtr,"),
    ("War3ResolveLivePoseRuntimeAlias",
     "void* War3ResolveLivePoseRuntimeAlias(void* runtimeModelPtr,"),
]


# M2-2：pre-M2-2 工作树身份（= M2-1 迁移完成后的 device.cpp，SHA 与
# docs/plan/2026-09-18-m2-1-migration-equivalence-record.md §1 "迁出后
# d3d9_device.cpp" 一致，可交叉验证）。CAPTURED_AT 为机器本地时间记录常量。
M2_2_CAPTURED_AT = "2026-09-17T22:54:30 (machine local time)"
M2_2_EXPECTED_SIZE = 2_310_648
M2_2_EXPECTED_SHA256 = (
    "B70F3AF97CD3DB554DDB311E9B89F3C64B99996EF9ADEA4FBA4CA7D2CF6F98F1"
)


# M2-3：pre-M2-3 工作树身份（= M2-2 迁移完成后的 device.cpp，SHA 与
# docs/plan/2026-09-18-m2-2-migration-equivalence-record.md §2 "迁移后
# device.cpp" 一致，可交叉验证）。CAPTURED_AT 为机器本地时间记录常量。
M2_3_CAPTURED_AT = "2026-09-18T02:18:25 (machine local time)"
M2_3_EXPECTED_SIZE = 2_278_494
M2_3_EXPECTED_SHA256 = (
    "ECC4B828AF344DCF20368DAC9F6C8344CFCDE4B862779A0190C6390CB3F6F7B3"
)
MOTION_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_motion_legacy_reference.inc"
)


def extract_definition(text, signature):
    """按签名锚抽取定义：跳过同名的前置声明（签名块内先出现 ';' 者）。

    返回 (起始行号(1-based), 定义文本)。定义文本 = 从签名首行到列 0 的
    结束花括号行（含），逐字节保持原文。
    """
    start = 0
    while True:
        index = text.find(signature, start)
        if index < 0:
            raise AssertionError(f"找不到定义锚：{signature!r}")
        brace = text.index("{", index)
        semi = text.find(";", index, brace)
        if semi < 0:
            # 这是一个定义（签名块内没有 ';' 抢先出现）。
            end = text.index("\n}\n", brace)
            line = text.count("\n", 0, index) + 1
            return line, text[index:end + 3]
        start = index + 1


def extract_span(text, start_marker, end_marker, what):
    """抽取 [start_marker, 包含 end_marker 的行末] 的逐字节文本（含注释）。"""
    start = text.index(start_marker)
    end = text.index(end_marker, start) + len(end_marker)
    line = text.count("\n", 0, start) + 1
    return line, text[start:end]


def main():
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_1.cpp"
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else default_snapshot
    if not source.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 pre-M2 device.cpp 快照 {source}\n"
            "（M2-1 第 0 步应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%TEMP%\\\\device_pre_m2_1.cpp；或用参数显式给出路径）"
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != EXPECTED_SHA256 or len(raw) != EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2 快照身份与登记值不一致：\n"
            f"  SHA-256 {actual_sha} != {EXPECTED_SHA256}\n"
            f"  size {len(raw)} != {EXPECTED_SIZE}\n"
            "快照必须就是 M2-1 迁移前的那份工作树 device.cpp。"
        )
    # 行尾归一化为 LF（device.cpp 为 CRLF；见模块 docstring 第 2 条机械处理）。
    text = raw.decode("utf-8").replace("\r\n", "\n")

    chunks = []
    # supporting 定义：pre-M2 时这 4 个 env getter 的真实调用目标是 M1 模块里
    # 的 War3GetEnvU32（它不属于本轮 8 个迁移符号，pre-M2 device.cpp 里不存在）。
    m1_text = M1_MODULE_CPP.read_text(encoding="utf-8").replace("\r\n", "\n")
    m1_line, m1_body = extract_definition(m1_text, SUPPORTING_SIGNATURE)
    chunks.append(
        "// --- supporting verbatim definition (NOT one of the 8 migrated\n"
        "// symbols): the pre-M2 call target of the legacy env getters, from\n"
        f"// war3/semantic/war3_device_semantic_predicates.cpp line {m1_line} ---\n"
        f"{m1_body}"
    )
    for label, signature in SYMBOLS:
        line, body = extract_definition(text, signature)
        chunks.append(
            f"// --- pre-migration definition: d3d9_device.cpp line {line}"
            f" ({label}) ---\n{body}"
        )

    header = f"""// AUTO-GENERATED legacy reference for the M2-1 live-palette-selection migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of the
// 8 symbols that M2-1 moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{{h,cpp}}.
// Regenerate with: py AutoTest/gen_war3_live_palette_selection_legacy_reference.py
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\\\device_pre_m2_1.cpp)
//   SHA-256(pre-M2 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {CAPTURED_AT}
//   NOTE worktree-vs-git-blob difference: unlike M1 (whose pre-migration text was
//   the committed git blob ae890542d766:src/d3d9/d3d9_device.cpp), the pre-M2 state
//   is the UNCOMMITTED M1 work tree, so git has no blob for it. This provenance is
//   a work-tree file hash, not a git blob hash.
//
// Only two mechanical edits are applied: the body text is re-emitted inside this
// namespace, and the enclosing namespace changes from device.cpp's anonymous
// namespace (namespace dxvk {{ namespace {{ ... }} }}) to
// 'm2_legacy_reference'. Line endings are normalized to LF (device.cpp is CRLF;
// M1's git-blob provenance was likewise LF-normalized). No token of any function
// is rewritten here.
//
// War3GetEnvU32 below is a SUPPORTING verbatim definition: it is NOT one of the
// 8 migrated symbols. M1 already moved it out of device.cpp, so the pre-M2
// device.cpp does not contain it; it is extracted here from the M1 module
// (war3/semantic/war3_device_semantic_predicates.cpp), which is exactly the call
// target the pre-M2 env getters resolved to.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chr(10).join(chunks)}

}} // namespace m2_legacy_reference
"""

    OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = OUT_PATH.read_bytes()
    print(f"generated: {OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")


# ---------------------------------------------------------------------------
# 2026-09-18 外部独立复审批次 4 / 步骤③：**迁移后契约补丁**（具名、定点、可复算）
#
# 等价门禁的目的是「证明迁移本身未改变行为」。当迁移**之后**出现经裁定的行为修复时，
# 正确做法是让**参照实现也带上同一契约**，使门禁重新回到逐位等价；
# 而不是把差异放行（那等于实质关闭门禁）。
#
# 每条补丁必须自带：名称、日期、依据、以及**定点替换**（审计者用同一生成器即可复算）。
# 匹配必须**恰好一次**，否则抛错 —— 绝不静默产出错误的参考文件。
# ---------------------------------------------------------------------------
POST_MIGRATION_CONTRACT_PATCHES = [
    {
        "name": "R1-cold-cache-groupcount",
        "date": "2026-09-18",
        "rationale": (
            "冷缓存首次查询原先不校验 producer 记录的 groupCount，与热缓存路径的数量",
            "规则不一致：同一对象、同一数量条件会在冷缓存下被接受、热缓存下被拒绝。",
            "修复使其适用同一规则（war3_skin_palette_selection.h 的",
            "ProducerGroupCountCovers）；依据 selection.cpp:325-332 既有契约",
            "（拒绝后落到替代路径属合法设计）与批次 4 步骤①的行为测试。",
        ),
        "old": "      currentSlotIndex = producerSlotIndex;\n",
        "new": (
            "      // --- POST-MIGRATION CONTRACT PATCH [R1-cold-cache-groupcount] "
            "(2026-09-18) ---\n"
            "      // 冷缓存首次查询必须与热缓存适用同一条数量规则；拒绝时**不写负缓存**，\n"
            "      // 下帧绑定恢复后仍可重新命中（与本补丁依据的既有契约一致）。\n"
            "      if (!dxvk::war3::render::skin::ProducerGroupCountCovers(\n"
            "              requiredPaletteCount, producerGroupCount)) {\n"
            "        g_devicePaletteSlotCacheRejectedStaleCount.fetch_add(\n"
            "            1u, std::memory_order_relaxed);\n"
            "        return 0xFFFFFFFFu;\n"
            "      }\n"
            "      currentSlotIndex = producerSlotIndex;\n"
        ),
    },
]


def apply_post_migration_contract_patches(text):
    """对抽取出的迁移前文本应用**具名**迁移后契约补丁（定点替换，恰好一次）。"""
    applied = []
    for patch in POST_MIGRATION_CONTRACT_PATCHES:
        occurrences = text.count(patch["old"])
        if occurrences != 1:
            raise SystemExit(
                "post-migration contract patch %r: anchor found %d times (expected exactly 1); "
                "refusing to generate a reference that silently diverges"
                % (patch["name"], occurrences)
            )
        text = text.replace(patch["old"], patch["new"], 1)
        applied.append(patch)
    return text, applied


def main_m2_2(argv):
    """--m2-2 模式：选择链本体 legacy 参考（第二份 .inc，M2-1 .inc 不动）。"""
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_2.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 pre-M2-2 device.cpp 快照 {source}\n"
            "（M2-2 阶段 1 应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%TEMP%\\\\device_pre_m2_2.cpp；或用参数显式给出路径）"
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != M2_2_EXPECTED_SHA256 or len(raw) != M2_2_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2-2 快照身份与登记值不一致：\n"
            f"  SHA-256 {actual_sha} != {M2_2_EXPECTED_SHA256}\n"
            f"  size {len(raw)} != {M2_2_EXPECTED_SIZE}\n"
            "快照必须就是 M2-2 迁移前的那份工作树 device.cpp（= M2-1 完成态）。"
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")

    chunks = []
    # supporting 1：Gap A 计数器定义行（pre-M2-2 :1060-1061）。其前的 device
    # 布局注释（含"声明位置提前到本匿名命名空间"一行）不随迁——它描述的是
    # device.cpp 旧布局，模块侧与 legacy 侧均以本 .inc 头部注释说明为准；
    # 两条定义行本身逐字节。
    for counter in (
        "std::atomic<uint64_t> g_devicePaletteSlotCacheServedAfterConfirmCount{0};",
        "std::atomic<uint64_t> g_devicePaletteSlotCacheRejectedStaleCount{0};",
    ):
        line = text.count("\n", 0, text.index(counter)) + 1
        chunks.append(
            "// --- supporting verbatim definitions (NOT the migrated chain\n"
            "// itself): the two Gap A counters the chain increments, from\n"
            f"// d3d9_device.cpp line {line} (definition lines verbatim; the\n"
            "// device-layout comment above them is intentionally not carried,\n"
            "// see header) ---\n"
            f"{counter}\n"
        )
    # supporting 2：共享类型块（Phase 7.28 注释 + War3SemanticPaletteSource +
    # War3LivePaletteBuildPhase + kWar3LivePaletteBuildPhaseCount +
    # War3LivePaletteBuildTiming + War3LivePaletteBuildRawTiming），
    # pre-M2-2 :5883-5967 逐字节。legacy 侧独立副本；测试对
    # War3SemanticPaletteSource 按数值比较（两侧枚举底层值相同），对
    # War3LivePaletteBuildTiming.calls[] 逐相位比较增量（QPC ticks 不比数值）。
    _tstart = text.index(
        "// Phase 7.28：skinned palette content stability probe。"
    )
    _tclass = text.index("class War3LivePaletteBuildRawTiming final {", _tstart)
    _tend = text.index("\n};\n", _tclass) + len("\n};\n")
    type_line = text.count("\n", 0, _tstart) + 1
    type_body = text[_tstart:_tend]
    chunks.append(
        "// --- supporting verbatim types: War3SemanticPaletteSource /\n"
        "// War3LivePaletteBuildPhase / kWar3LivePaletteBuildPhaseCount /\n"
        "// War3LivePaletteBuildTiming / War3LivePaletteBuildRawTiming, from\n"
        f"// d3d9_device.cpp line {type_line} ---\n"
        f"{type_body}\n"
    )
    # supporting 3：链的前向声明（pre-M2-2 :5969-5984 逐字节）。原文件中它
    # 携带第 17 参 outSelection 的默认实参，定义再为第 8-16 参**追加**默认
    # 实参（C++ 允许后续声明追加默认实参）。缺了它，"8-16 有默认实参而 17
    # 没有"在单一声明下非法——首轮宿主编译即在此失败。
    _fstart = text.index("bool War3TryBuildLiveRuntimeGroupPalette(\n")
    _fend = text.index("outSelection = nullptr);\n", _fstart) + len(
        "outSelection = nullptr);\n"
    )
    fwd_line = text.count("\n", 0, _fstart) + 1
    fwd_body = text[_fstart:_fend]
    chunks.append(
        "// --- supporting verbatim forward declaration: carries the\n"
        "// outSelection (parameter 17) default argument; the definition below\n"
        "// legally ADDS the parameter 8-16 defaults, from d3d9_device.cpp\n"
        f"// line {fwd_line} ---\n"
        f"{fwd_body}\n"
    )
    # 迁移本体：选择链定义（含签名上第 8-16 参的默认实参——与上方前向声明
    # 组合后与原文件同为合法形式，且保持逐字节）。
    def_line, def_body = extract_definition(
        text, "bool War3TryBuildLiveRuntimeGroupPalette(\n"
    )
    chunks.append(
        "// --- pre-migration definition: d3d9_device.cpp line "
        f"{def_line} (War3TryBuildLiveRuntimeGroupPalette) ---\n{def_body}"
    )

    # 2026-09-18（步骤③ B1）：先应用**具名**迁移后契约补丁，再组装参考文本。
    body, applied_patches = apply_post_migration_contract_patches(chr(10).join(chunks))
    if applied_patches:
        patch_note = "".join(
            "//   + POST-MIGRATION CONTRACT PATCH [{name}] ({date})\n//     {rationale}\n".format(
                name=p["name"], date=p["date"], rationale=" ".join(p["rationale"]),
            )
            for p in applied_patches
        )
    else:
        patch_note = "//   (none)\n"

    header = f"""// AUTO-GENERATED legacy reference for the M2-2 selection-chain migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of
// the selection-chain body War3TryBuildLiveRuntimeGroupPalette (plus the
// supporting types/counters it uses) that M2-2 moved out of
// src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{{h,cpp}}.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-2
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-2 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\\\device_pre_m2_2.cpp)
//   SHA-256(pre-M2-2 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {M2_2_CAPTURED_AT}
//   The pre-M2-2 state equals the post-M2-1 work tree (same SHA as the M2-1
//   record's "post-migration d3d9_device.cpp"); like M2-1 this is a work-tree
//   file hash, not a git blob hash (M2-1's work is uncommitted).
//
// Post-migration contract patches applied (declared in the generator as
// POST_MIGRATION_CONTRACT_PATCHES; each is named, dated, justified and applied
// as an exactly-once fixed-point replacement, so this file stays reproducible):
{patch_note}//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization
// and the contract patches listed above):
//   * This file is included AFTER war3_live_palette_selection_legacy_reference.inc
//     and shares its 'm2_legacy_reference' namespace. The chain body's
//     UNQUALIFIED calls to the M2-1 helpers/env getters
//     (War3SemanticHashMatrixPalette, War3DecodeRuntimePoseMatrix48,
//     War3ResolveLivePoseRuntimeAlias, War3SemanticLivePaletteSafeCopyRuntime,
//     War3SemanticLivePaletteAllowCModelFallbackRuntime) therefore resolve to
//     the M2-1 legacy copies in the same namespace — exactly the call targets
//     the pre-M2-2 device.cpp text resolved through its using-directive.
//   * War3SemanticPaletteSource / War3LivePaletteBuildPhase /
//     kWar3LivePaletteBuildPhaseCount / War3LivePaletteBuildTiming /
//     War3LivePaletteBuildRawTiming are legacy-side INDEPENDENT copies
//     (verbatim). The differential test compares War3SemanticPaletteSource by
//     numeric value and War3LivePaletteBuildTiming.calls[] per-phase
//     increments; QPC tick values are never compared numerically.
//   * dxvk::war3::render::skin::Selection is the SHARED contract type — the
//     same real header type on both sides, deliberately not rewritten.
//   * Fully-qualified calls (dxvk::war3::model::*, dxvk::war3::SafeRead*,
//     dxvk::war3::SafeCopy, dxvk::war3::IsReadableRange,
//     dxvk::war3::GetGameDllBase, dxvk::high_resolution_clock) resolve to the
//     same host stubs / real inline primitives as the module side.
//   * g_devicePaletteSlotCacheServedAfterConfirmCount /
//     g_devicePaletteSlotCacheRejectedStaleCount are legacy-side INDEPENDENT
//     copies (definition lines verbatim from pre-M2-2 device.cpp; the stale
//     device-layout comment above them is intentionally not carried — it
//     described the old device.cpp layout). Per-call counter deltas are
//     compared against the module side's dxvk::war3::semantic externs.
//   * The verbatim FORWARD declaration (device.cpp :5969) is included so the
//     outSelection parameter-17 default argument exists exactly as in the
//     pre-M2-2 file; the definition (:7443) then legally adds the parameter
//     8-16 defaults. No text was edited to make this compile.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{body}

}} // namespace m2_legacy_reference
"""

    CHAIN_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = CHAIN_OUT_PATH.read_bytes()
    print(f"generated: {CHAIN_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")


def main_m2_3(argv):
    """--m2-3 模式：motion 诊断三函数 legacy 参考（第三份 .inc，前两份不动）。

    抽取范围 = pre-M2-3 device.cpp 的匿名命名空间内相邻文本块
    :7344-7520：两个 Entry 结构 + 三个 War3Note*Motion 定义。全部逐字节。
    device 侧的 War3SemanticPaletteMotionEntry / War3SemanticHashMotionEntry
    与三个函数一起搬入模块，因此它们同属本 .inc 的抽取范围（"各自 Entry
    结构"）。
    """
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_3.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 pre-M2-3 device.cpp 快照 {source}\n"
            "（M2-3 阶段 1 应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%TEMP%\\\\device_pre_m2_3.cpp；或用参数显式给出路径）"
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != M2_3_EXPECTED_SHA256 or len(raw) != M2_3_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2-3 快照身份与登记值不一致：\n"
            f"  SHA-256 {actual_sha} != {M2_3_EXPECTED_SHA256}\n"
            f"  size {len(raw)} != {M2_3_EXPECTED_SIZE}\n"
            "快照必须就是 M2-3 迁移前的那份工作树 device.cpp（= M2-2 完成态）。"
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")

    chunks = []

    def add_struct(signature, label):
        start = text.index(signature)
        end = text.index("\n};\n", start) + len("\n};\n")
        line = text.count("\n", 0, start) + 1
        chunks.append(
            f"// --- pre-migration entry struct: d3d9_device.cpp line {line}"
            f" ({label}) ---\n{text[start:end]}"
        )

    def add_definition(signature, label):
        line, body = extract_definition(text, signature)
        chunks.append(
            f"// --- pre-migration definition: d3d9_device.cpp line {line}"
            f" ({label}) ---\n{body}"
        )

    # 抽取顺序 = pre-M2-3 device.cpp 行号升序（:7344 结构、:7351 定义、
    # :7416 结构、:7422 定义、:7471 定义）。
    add_struct("struct War3SemanticPaletteMotionEntry {",
               "War3SemanticPaletteMotionEntry")
    add_definition("void War3NoteLivePaletteMotion(War3ShadowCaptureStats& stats,",
                   "War3NoteLivePaletteMotion")
    add_struct("struct War3SemanticHashMotionEntry {",
               "War3SemanticHashMotionEntry")
    add_definition("void War3NoteDrawTimePoseMotion(War3ShadowCaptureStats& stats,",
                   "War3NoteDrawTimePoseMotion")
    add_definition(
        "void War3NoteSubmittedPaletteMotion(War3ShadowCaptureStats& stats,",
        "War3NoteSubmittedPaletteMotion")

    header = f"""// AUTO-GENERATED legacy reference for the M2-3 motion-diagnostics migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of
// the three motion/churn diagnostic notes and their two entry structs that M2-3
// moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{{h,cpp}}:
//   War3SemanticPaletteMotionEntry, War3NoteLivePaletteMotion,
//   War3SemanticHashMotionEntry, War3NoteDrawTimePoseMotion,
//   War3NoteSubmittedPaletteMotion.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-3
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-3 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\\\device_pre_m2_3.cpp)
//   SHA-256(pre-M2-3 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {M2_3_CAPTURED_AT}
//   The pre-M2-3 state equals the post-M2-2 work tree (same SHA as the M2-2
//   record's "post-migration d3d9_device.cpp"); like M2-1/M2-2 this is a
//   work-tree file hash, not a git blob hash (M2-1/M2-2 work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER war3_live_palette_selection_legacy_reference.inc
//     and war3_live_palette_selection_chain_legacy_reference.inc and shares their
//     'm2_legacy_reference' namespace. The three bodies' UNQUALIFIED call to
//     War3SemanticPaletteDiagnosticsRuntime() therefore resolves to the M2-1
//     legacy copy in the same namespace — exactly the call target the pre-M2-3
//     device.cpp text resolved through its using-directive.
//   * War3ShadowCaptureStats is the SHARED real contract type (the same
//     d3d9_war3_scene.h struct on both sides, deliberately not rewritten); the
//     legacy text reaches it through the 'using namespace dxvk;' below.
//   * The two Entry structs are legacy-side copies (verbatim); the module side
//     has its own independently compiled copies with identical layout.
//   * No token of any function or struct is rewritten here.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chr(10).join(chunks)}

}} // namespace m2_legacy_reference
"""

    MOTION_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = MOTION_OUT_PATH.read_bytes()
    print(f"generated: {MOTION_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")


# M2-5（2026-09-18）：pre-M2-5 工作树身份（= M2-3 迁移完成后的 device.cpp，SHA 与
# docs/plan/2026-09-18-m2-3-migration-equivalence-record.md §2 "迁移后 device.cpp"
# 一致，可交叉验证）。CAPTURED_AT 为机器本地时间记录常量。
M2_5_CAPTURED_AT = "2026-09-18T03:05:30 (machine local time)"
M2_5_EXPECTED_SIZE = 2_273_048
M2_5_EXPECTED_SHA256 = (
    "D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE"
)
TAXONOMY_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_palette_taxonomy_emission_legacy_reference.inc"
)
TAXONOMY_MODULE_CPP = (
    ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp"
)
M2_5_START_LINE = 21202
M2_5_END_LINE = 21580
M2_5_GUARD = "    if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {"
M2_5_STATS_ALIAS = "      auto& stats = m_war3Scene.shadowStats;"
M2_5_SIGNATURE = "\n".join([
    "void War3EmitSemanticPaletteTaxonomy(",
    "    dxvk::War3ShadowCaptureStats& stats, bool skinned,",
    "    War3SemanticPaletteSource paletteSourceThisSubmit,",
    "    const dxvk::war3::render::CurrentDrawAuthoritativeSample* currentDrawSample,",
    "    dxvk::war3::render::PaletteProvenance drawTimeCapturedPaletteProvenance,",
    "    const std::vector<Matrix4>* effectiveCanonicalPalette,",
    "    uint32_t effectiveCanonicalPaletteCount,",
    "    uint32_t paletteSlotIndexThisSubmit, uint64_t submittedPaletteHash,",
    "    bool fromStalePoseRestore,",
    "    uint64_t m_war3ShadowPersistentFrameSerial) {",
])
M2_5_BEGIN = (
    "    // --- BEGIN M2-5 verbatim pre-migration emission block"
    " (d3d9_device.cpp :21202-21580, minus :21203) ---"
)
M2_5_END = "    // --- END M2-5 verbatim pre-migration emission block ---"


def extract_taxonomy_block(text):
    """从 LF 归一化后的 pre-M2-5 device.cpp 抽取 taxonomy 发射块正文。

    块 = 第 21202 行 `if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {`
    到第 21580 行其配对 `}`（花括号配对，逐字节），再删除第 21203 行的别名
    `auto& stats = m_war3Scene.shadowStats;`（该引用在两侧都是第一个参数）。
    """
    index = text.index(M2_5_GUARD)
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
        raise AssertionError("fail-closed: taxonomy 发射块花括号不配对")
    start_line = text.count("\n", 0, index) + 1
    end_line = text.count("\n", 0, end) + 1
    if (start_line, end_line) != (M2_5_START_LINE, M2_5_END_LINE):
        raise AssertionError(
            "fail-closed: taxonomy 发射块行号 "
            f"{start_line}-{end_line} != {M2_5_START_LINE}-{M2_5_END_LINE}"
        )
    block = text[index:end + 1]
    if block.count(M2_5_STATS_ALIAS) != 1:
        raise AssertionError(
            "fail-closed: 块内 stats 别名行不是恰好一行（迁移改写规则失效）"
        )
    body = block.replace(M2_5_STATS_ALIAS + "\n", "", 1)
    for forbidden in ("m_war3Scene", "m_war3Semantic", "m_state",
                      "VisibleRenderableRegistry::instance"):
        if forbidden in body:
            raise AssertionError(
                f"fail-closed: 抽取正文仍引用 device/registry 全局 {forbidden}"
            )
    return body


def module_taxonomy_body(module_text):
    """从模块 .cpp 的 BEGIN/END 标记之间取出正文（含首尾换行）。"""
    begin = module_text.index(M2_5_BEGIN)
    begin_end = module_text.index("\n", begin) + 1
    end = module_text.index(M2_5_END)
    return module_text[begin_end:end]


def main_m2_5(argv):
    """--m2-5 模式：taxonomy 发射块 legacy 参考（第四份 .inc，前三份不动）。

    抽取范围 = pre-M2-5 device.cpp :21202-21580（含 skinned 条件与诊断门本身的
    379 行），删除 :21203 的 stats 别名行。逐字节。

    fail-closed 双向校验：
      1. 快照 SHA-256/字节数必须等于登记值；
      2. 块行号必须等于 :21202-21580；
      3. 模块 .cpp 的 BEGIN/END 标记之间正文必须与快照抽取结果**逐字节相同**。
    """
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_5.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 pre-M2-5 device.cpp 快照 {source}\n"
            "（M2-5 阶段 1 应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%TEMP%\\\\device_pre_m2_5.cpp；或用参数显式给出路径）"
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != M2_5_EXPECTED_SHA256 or len(raw) != M2_5_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2-5 快照身份与登记值不一致：\n"
            f"  SHA-256 {actual_sha} != {M2_5_EXPECTED_SHA256}\n"
            f"  size {len(raw)} != {M2_5_EXPECTED_SIZE}\n"
            "快照必须就是 M2-5 迁移前的那份工作树 device.cpp（= M2-3 完成态）。"
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")
    body = extract_taxonomy_block(text)

    if not TAXONOMY_MODULE_CPP.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 M2-5 模块 {TAXONOMY_MODULE_CPP}（无法做正文逐字节"
            "一致性校验）"
        )
    module_text = TAXONOMY_MODULE_CPP.read_text(encoding="utf-8")
    module_body = module_taxonomy_body(module_text)
    if module_body != body + "\n":
        raise AssertionError(
            "fail-closed: 模块 .cpp BEGIN/END 之间的正文与 pre-M2-5 快照抽取结果"
            "不一致（逐字节比对失败）——迁移文本已漂移，必须人工复核。"
        )
    if M2_5_SIGNATURE not in module_text:
        raise AssertionError(
            "fail-closed: 模块 .cpp 里的函数签名与 .inc 登记的签名文本不一致"
        )

    chunk = (
        "// --- pre-migration emission block: d3d9_device.cpp :21202-21580"
        " (verbatim; line :21203 `auto& stats = m_war3Scene.shadowStats;` is"
        " deleted on both sides - `stats` is parameter 1) ---\n"
        f"{M2_5_SIGNATURE}\n"
        f"{M2_5_BEGIN}\n"
        f"{body}\n"
        f"{M2_5_END}\n"
        "}"
    )

    header = f"""// AUTO-GENERATED legacy reference for the M2-5 palette-taxonomy migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the skinned
// palette taxonomy emission block that M2-5 moved out of
// src/d3d9/d3d9_device.cpp (:21202-21580) into
// src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{{h,cpp}} as
// War3EmitSemanticPaletteTaxonomy.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-5
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-5 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\\\device_pre_m2_5.cpp)
//   SHA-256(pre-M2-5 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {M2_5_CAPTURED_AT}
//   The pre-M2-5 state equals the post-M2-3 work tree (same SHA as the M2-3
//   record's \"post-migration d3d9_device.cpp\"); like M1/M2-1/M2-2/M2-3 this is a
//   work-tree file hash, not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3 .inc files and shares their
//     'm2_legacy_reference' namespace. The block's UNQUALIFIED
//     War3SemanticPaletteDiagnosticsRuntime() therefore resolves to the M2-1
//     legacy copy in the same namespace, and UNQUALIFIED War3SemanticPaletteSource
//     resolves to the M2-2 legacy enum copy - exactly the call targets the
//     pre-M2-5 device.cpp text resolved through its using-directive.
//   * dxvk::war3::render::CurrentDrawAuthoritativeSample,
//     dxvk::war3::render::PaletteProvenance and dxvk::War3ShadowCaptureStats are
//     the SHARED real contract types (the same headers on both sides, deliberately
//     not rewritten).
//   * EXACTLY ONE line is deleted relative to the device.cpp text: the block's
//     first statement `auto& stats = m_war3Scene.shadowStats;`. On both sides
//     `stats` is the first parameter; every other byte (including the three
//     `static thread_local std::array<...>` probe tables) is verbatim. The
//     generator fail-closes unless the module .cpp text between the BEGIN/END
//     markers is byte-identical to this file's body.
//   * The block deliberately keeps its three probe tables as function-local
//     `static thread_local` fixed-size std::array with mask indexing, so the
//     storage domain, initialization order and per-thread lifetime are unchanged.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chunk}

}} // namespace m2_legacy_reference
"""

    TAXONOMY_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = TAXONOMY_OUT_PATH.read_bytes()
    print(f"generated: {TAXONOMY_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")



# ---------------------------------------------------------------------------
# M2-4（2026-09-18）：pre-M2-4 工作树身份（= M2-5 迁移完成后的 device.cpp，
# SHA 与 docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md §2 的
# "迁移后 device.cpp" 一致，可交叉验证闭合）。
# ---------------------------------------------------------------------------
M2_4_CAPTURED_AT = "2026-09-18T03:34:55 (machine local time)"
M2_4_EXPECTED_SIZE = 2_254_826
M2_4_EXPECTED_SHA256 = (
    "7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844"
)
M2_4_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_m2_4_legacy_reference.inc"
)
M2_4_MODULE_CPP = (
    ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp"
)

# (key, device-side signature, module-side signature, pre-M2-4 line range)
# 顺序 = .inc 内的发射顺序 = 依赖序（A4 依赖 A6/A7/A8；A4 vector 重载依赖 A4
# pointer 重载与 A3）。唯一签名差异：env getter 的 inline 落头文件一侧；A4
# pointer 重载的 checkReadable 默认实参集中在头文件声明（正文仍逐字节）。
M2_4_SYMBOLS = [
    ("War3SemanticPaletteInPlaceAppendRuntime",
     "inline bool War3SemanticPaletteInPlaceAppendRuntime() {",
     "bool War3SemanticPaletteInPlaceAppendRuntime() {",
     (1302, 1309)),
    ("War3SemanticDrawTimePoseRuntime",
     "bool War3SemanticDrawTimePoseRuntime() {",
     "bool War3SemanticDrawTimePoseRuntime() {",
     (2079, 2087)),
    ("War3SemanticBoundsRadiusForObjectKind",
     "float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {",
     "float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {",
     (7144, 7160)),
    ("War3SemanticTranslationDistanceSq",
     "float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {",
     "float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {",
     (7177, 7182)),
    ("War3SemanticTranslationFinite",
     "bool War3SemanticTranslationFinite(const Matrix4& m) {",
     "bool War3SemanticTranslationFinite(const Matrix4& m) {",
     (7184, 7187)),
    ("War3SemanticPaletteStorageReadable",
     "bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {",
     "bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {",
     (7189, 7196)),
    ("War3SemanticPaletteLooksModelLocal[pointer]",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const Matrix4* palette,\n"
     "    uint32_t paletteCount,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind,\n"
     "    bool checkReadable = true) {",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const Matrix4* palette,\n"
     "    uint32_t paletteCount,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind,\n"
     "    bool checkReadable) {",
     (7209, 7263)),
    ("War3SemanticPaletteLooksModelLocal[vector]",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const std::vector<Matrix4>& palette,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind) {",
     "bool War3SemanticPaletteLooksModelLocal(\n"
     "    const std::vector<Matrix4>& palette,\n"
     "    const Matrix4& worldTransform,\n"
     "    uint8_t objectKind) {",
     (7265, 7274)),
    ("War3SemanticHashMatrix4",
     "uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {",
     "uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {",
     (7292, 7301)),
]

M2_4_FORBIDDEN = (
    "m_war3Scene", "m_war3Semantic", "m_state",
    "VisibleRenderableRegistry::instance", "this->",
)


def m2_4_begin_marker(key, lines):
    return ("    // --- BEGIN M2-4 body: %s (pre-M2-4 d3d9_device.cpp :%d-%d) ---"
            % (key, lines[0], lines[1]))


def m2_4_end_marker(key):
    return "    // --- END M2-4 body: %s ---" % key


def main_m2_4(argv):
    """--m2-4 模式：调色板 compose-policy / env getter 余项的 legacy 参考。

    抽取范围 = pre-M2-4 device.cpp 的 9 个定义（8 个符号，A4 两个重载分列）。
    逐字节；行尾归一化 LF。

    fail-closed 双向校验：
      1. 快照 SHA-256/字节数必须等于登记值；
      2. 每个定义锚必须唯一命中且行号等于登记范围，正文不得引用
         device 成员 / registry 全局；
      3. 模块 .cpp 的每个 BEGIN/END 标记之间正文必须与快照抽取结果
         **逐字节相同**（任一侧漂移即失败），且模块签名必须逐字命中。
    """
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_4.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            "fail-closed: 缺少 pre-M2-4 device.cpp 快照 %s\n"
            "（M2-4 第 0 步应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%%TEMP%%\\device_pre_m2_4.cpp；或用参数显式给出路径）" % source
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != M2_4_EXPECTED_SHA256 or len(raw) != M2_4_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2-4 快照身份与登记值不一致：\n"
            "  SHA-256 %s != %s\n  size %d != %d\n"
            "快照必须就是 M2-4 迁移前的那份工作树 device.cpp（= M2-5 完成态）。"
            % (actual_sha, M2_4_EXPECTED_SHA256, len(raw), M2_4_EXPECTED_SIZE)
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")

    if not M2_4_MODULE_CPP.is_file():
        raise AssertionError(
            "fail-closed: 缺少 M2-4 模块 %s（无法做正文逐字节一致性校验）"
            % M2_4_MODULE_CPP
        )
    module_text = M2_4_MODULE_CPP.read_text(encoding="utf-8")

    chunks = []
    for key, dev_sig, mod_sig, want_lines in M2_4_SYMBOLS:
        if text.count(dev_sig) != 1:
            raise AssertionError(
                "fail-closed: pre-M2-4 快照里锚不唯一（%s）：%d 处"
                % (key, text.count(dev_sig))
            )
        line, definition = extract_definition(text, dev_sig)
        span = (line, line + definition.count("\n") - 1)
        if span != want_lines:
            raise AssertionError(
                "fail-closed: %s 行号 %s != 登记 %s" % (key, span, want_lines)
            )
        if not definition.startswith(dev_sig):
            raise AssertionError("fail-closed: %s 定义不以签名开头" % key)
        body = definition[len(dev_sig):-2]
        if not (body.startswith("\n") and body.endswith("\n")):
            raise AssertionError("fail-closed: %s 正文边界异常" % key)
        for forbidden in M2_4_FORBIDDEN:
            if forbidden in body:
                raise AssertionError(
                    "fail-closed: %s 正文引用 device/registry 全局 %s"
                    % (key, forbidden)
                )
        if module_text.count(mod_sig) != 1:
            raise AssertionError(
                "fail-closed: 模块 .cpp 里的签名不是恰好 1 处（%s）" % key
            )
        begin = m2_4_begin_marker(key, want_lines)
        end = m2_4_end_marker(key)
        if module_text.count(begin) != 1 or module_text.count(end) != 1:
            raise AssertionError(
                "fail-closed: 模块 .cpp 的 BEGIN/END 标记不是恰好 1 对（%s）" % key
            )
        begin_end = module_text.index("\n", module_text.index(begin)) + 1
        module_body = module_text[begin_end:module_text.index(end)]
        if module_body != body:
            raise AssertionError(
                "fail-closed: 模块 .cpp BEGIN/END 之间的正文与 pre-M2-4 快照抽取"
                "结果不一致（逐字节比对失败，%s）——迁移文本已漂移，必须人工复核。"
                % key
            )
        chunks.append(
            "// --- pre-migration definition: d3d9_device.cpp line %d (%s,"
            " verbatim; body byte-identical to the module) ---\n%s"
            % (line, key, definition)
        )

    header = f"""// AUTO-GENERATED legacy reference for the M2-4 palette compose-policy migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the eight
// palette-side symbols (nine definitions: War3SemanticPaletteLooksModelLocal
// has two overloads) that M2-4 moved out of src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{{h,cpp}}:
//   War3SemanticPaletteInPlaceAppendRuntime, War3SemanticDrawTimePoseRuntime,
//   War3SemanticBoundsRadiusForObjectKind, War3SemanticTranslationDistanceSq,
//   War3SemanticTranslationFinite, War3SemanticPaletteStorageReadable,
//   War3SemanticPaletteLooksModelLocal (pointer + vector overloads),
//   War3SemanticHashMatrix4.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-4
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-4 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_m2_4.cpp)
//   SHA-256(pre-M2-4 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {M2_4_CAPTURED_AT}
//   The pre-M2-4 state equals the post-M2-5 work tree (same SHA as the M2-5
//   record's "post-migration d3d9_device.cpp"); like M1/M2-1/M2-2/M2-3/M2-5 this
//   is a work-tree file hash, not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5 .inc files and shares
//     their 'm2_legacy_reference' namespace. The bodies' UNQUALIFIED calls
//     (War3SemanticTranslationFinite, War3SemanticTranslationDistanceSq,
//     War3SemanticBoundsRadiusForObjectKind, War3SemanticPaletteStorageReadable,
//     War3SemanticPaletteLooksModelLocal, War3GetEnvU32) therefore resolve to the
//     legacy copies in this same namespace - exactly the call targets the
//     pre-M2-4 device.cpp text resolved through its using-directive. The .inc
//     therefore emits the definitions in dependency order.
//   * dxvk::war3::render::ObjectKind and dxvk::war3::IsReadableRange are the
//     SHARED real contract primitives (same headers on both sides, deliberately
//     not rewritten).
//   * EXACTLY ZERO lines are deleted or rewritten relative to the device.cpp
//     text: only the enclosing namespace changes and line endings are LF-
//     normalized. The generator fail-closes unless every module .cpp body
//     between its BEGIN/END markers is byte-identical to this file's bodies.
//   * Signature-only differences that are NOT part of the compared body: the env
//     getters drop 'inline' (declared in the module header) and the pointer
//     overload of War3SemanticPaletteLooksModelLocal keeps its
//     'checkReadable = true' default only on the module header declaration
//     (M2-2 precedent). No token inside any function body was edited.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chr(10).join(chunks)}

}} // namespace m2_legacy_reference
"""

    M2_4_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = M2_4_OUT_PATH.read_bytes()
    print(f"generated: {M2_4_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")

# ---------------------------------------------------------------------------
# M2-5B（2026-09-18，M2-5 余项 B4）：pre-M2-5B 工作树身份（= M2-4 迁移完成后的
# device.cpp，SHA 与 docs/plan/2026-09-18-m2-4-migration-equivalence-record.md
# §9.1 "迁移后 device.cpp" 一致，可交叉验证）。CAPTURED_AT 为机器本地时间记录常量。
M2_5B_CAPTURED_AT = "2026-09-18T04:15:05 (machine local time)"
M2_5B_EXPECTED_SIZE = 2_250_786
M2_5B_EXPECTED_SHA256 = (
    "D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811"
)
SUBMITTED_AGG_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_palette_submitted_aggregation_legacy_reference.inc"
)
SUBMITTED_AGG_MODULE_CPP = (
    ROOT / "src/d3d9/war3/semantic/war3_palette_submitted_aggregation.cpp"
)
M2_5B_START_LINE = 22200
M2_5B_END_LINE = 22244
M2_5B_LOCATE = (
    "      // Phase 7.48：per-frame submitted skinned palette 聚合"
    "（只对 skinned 生效）。"
)
M2_5B_SIGNATURE = "\n".join([
    "void War3AggregateSubmittedSkinnedPalette(",
    "    dxvk::War3ShadowCaptureStats& st, bool skinned) {",
])
M2_5B_BEGIN = (
    "    // --- BEGIN M2-5B verbatim pre-migration aggregation block"
    " (d3d9_device.cpp :22200-22244) ---"
)
M2_5B_END = "    // --- END M2-5B verbatim pre-migration aggregation block ---"


def extract_submitted_agg_block(text):
    """从 LF 归一化后的 pre-M2-5B device.cpp 抽取 B4 聚合块正文（逐字节）。

    块 = 第 22200 行 '      if (skinned) {' 到第 22244 行其配对 '}'。
    无任何行被删除或改写（唯一的差异在包装层：块内的 st 引用在模块侧是
    第一个形参，名字逐字相同）。
    """
    index = text.index(M2_5B_LOCATE)
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
        raise AssertionError("fail-closed: B4 聚合块花括号不配对")
    line_start = text.rfind("\n", 0, brace) + 1
    start_line = text.count("\n", 0, line_start) + 1
    end_line = text.count("\n", 0, end) + 1
    if (start_line, end_line) != (M2_5B_START_LINE, M2_5B_END_LINE):
        raise AssertionError(
            "fail-closed: B4 聚合块行号 "
            f"{start_line}-{end_line} != {M2_5B_START_LINE}-{M2_5B_END_LINE}"
        )
    body = text[line_start:end + 1]
    if body.count("\n") != M2_5B_END_LINE - M2_5B_START_LINE:
        raise AssertionError("fail-closed: B4 聚合块行数不匹配")
    if not body.startswith("      if (skinned) {"):
        raise AssertionError("fail-closed: B4 聚合块起点不是 skinned 守卫")
    for forbidden in ("m_war3Scene", "m_war3Semantic", "m_state",
                      "VisibleRenderableRegistry::instance", "this->"):
        if forbidden in body:
            raise AssertionError(
                f"fail-closed: 抽取正文仍引用 device/registry 全局 {forbidden}"
            )
    return body


def module_submitted_agg_body(module_text):
    """从模块 .cpp 的 BEGIN/END 标记之间取出正文（含首尾换行）。"""
    begin = module_text.index(M2_5B_BEGIN)
    begin_end = module_text.index("\n", begin) + 1
    end = module_text.index(M2_5B_END)
    return module_text[begin_end:end]


def main_m2_5b(argv):
    """--m2-5b 模式：B4 聚合块 legacy 参考（第六份 .inc，前五份不动）。

    抽取范围 = pre-M2-5B device.cpp :22200-22244（45 行，逐字节，无行被删）。

    fail-closed 双向校验：
    1. 快照 SHA-256 / 字节数必须等于登记常量；
    2. 块必须花括号配对到 :22200-22244 且正文不含 device/registry token；
    3. 模块 .cpp 的 BEGIN/END 标记之间正文必须与快照抽取结果**逐字节相同**。
    """
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_m2_5b.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 pre-M2-5B device.cpp 快照 {source}\n"
            "（M2-5B 阶段 1 应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%TEMP%\\\\device_pre_m2_5b.cpp；或用参数显式给出路径）"
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != M2_5B_EXPECTED_SHA256 or len(raw) != M2_5B_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-M2-5B 快照身份与登记值不一致：\n"
            f"  SHA-256 {actual_sha} != {M2_5B_EXPECTED_SHA256}\n"
            f"  size {len(raw)} != {M2_5B_EXPECTED_SIZE}\n"
            "快照必须就是 M2-5B 迁移前的那份工作树 device.cpp（= M2-4 完成态）。"
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")
    body = extract_submitted_agg_block(text)

    if not SUBMITTED_AGG_MODULE_CPP.is_file():
        raise AssertionError(
            f"fail-closed: 缺少 M2-5B 模块 {SUBMITTED_AGG_MODULE_CPP}"
            "（无法做正文逐字节一致性校验）"
        )
    module_text = SUBMITTED_AGG_MODULE_CPP.read_text(encoding="utf-8")
    module_body = module_submitted_agg_body(module_text)
    if module_body != body + "\n":
        raise AssertionError(
            "fail-closed: 模块 .cpp BEGIN/END 之间的正文与 pre-M2-5B 快照抽取结果"
            "不一致（逐字节比对失败）——迁移文本已漂移，必须人工复核。"
        )
    if M2_5B_SIGNATURE not in module_text:
        raise AssertionError(
            "fail-closed: 模块 .cpp 里的函数签名与 .inc 登记的签名文本不一致"
        )

    chunk = (
        "// --- pre-migration aggregation block: d3d9_device.cpp :22200-22244"
        " (verbatim; not one byte deleted or rewritten) ---\n"
        f"{M2_5B_SIGNATURE}\n"
        f"{M2_5B_BEGIN}\n"
        f"{body}\n"
        f"{M2_5B_END}\n"
        "}"
    )

    header = f"""// AUTO-GENERATED legacy reference for the M2-5B submitted-skinned-palette
// aggregation migration (M2-5 remainder item B4).
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the per-frame
// submitted skinned palette aggregation block that M2-5B moved out of
// src/d3d9/d3d9_device.cpp (:22200-22244) into
// src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{{h,cpp}} as
// War3AggregateSubmittedSkinnedPalette.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-5b
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-5B work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\\\device_pre_m2_5b.cpp)
//   SHA-256(pre-M2-5B work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {M2_5B_CAPTURED_AT}
//   The pre-M2-5B state equals the post-M2-4 work tree (same SHA as the M2-4
//   record's "post-migration d3d9_device.cpp"); like M1/M2-1/M2-2/M2-3/M2-5/M2-4
//   this is a work-tree file hash, not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5/M2-4 .inc files and
//     shares their 'm2_legacy_reference' namespace. Nothing inside the block is
//     rewritten: the block only reads/writes the first parameter (named st, the
//     same identifier as in device.cpp) and calls the unqualified pure template
//     bit::fnv1a_iter, which resolves through 'using namespace dxvk;' exactly as
//     the pre-M2-5B device.cpp text did.
//   * dxvk::War3ShadowCaptureStats is the SHARED real contract type (the same
//     header on both sides, deliberately not rewritten).
//   * EXACTLY ZERO lines are deleted relative to the device.cpp text. The
//     generator fail-closes unless the module .cpp text between the BEGIN/END
//     markers is byte-identical to this file's body.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chunk}

}} // namespace m2_legacy_reference
"""

    SUBMITTED_AGG_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = SUBMITTED_AGG_OUT_PATH.read_bytes()
    print(f"generated: {SUBMITTED_AGG_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")




# ---------------------------------------------------------------------------
# A9（2026-09-18 死代码裁定）：pre-A9 工作树身份（= A10 删除落地后的 device.cpp，
# SHA 与 A10 步骤的 post device.cpp 同值，可交叉验证闭合）。CAPTURED_AT 为机器
# 本地时间记录常量。
# ---------------------------------------------------------------------------
A9_CAPTURED_AT = "2026-09-18T04:49:48 (machine local time)"
A9_EXPECTED_SIZE = 2_248_823
A9_EXPECTED_SHA256 = (
    "FB4D2FF61DC75EC1A8DAFEC2DCFD2A1CE51F101B16399BE1383F4B035555716C"
)
A9_OUT_PATH = (
    ROOT
    / "src/d3d9/war3/semantic/tests/"
    "war3_live_palette_selection_a9_legacy_reference.inc"
)
A9_MODULE_CPP = (
    ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp"
)
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
A9_FORBIDDEN = (
    "m_war3Scene", "m_war3Semantic", "m_state",
    "VisibleRenderableRegistry::instance", "this->",
)


def main_a9(argv):
    """--a9 模式：A9（死代码裁定 = 迁移而非删除）的 legacy 参考（第七份 .inc，
    前六份 .inc 与其 SHA 一字未动）。

    抽取范围 = pre-A9 device.cpp :7161-7175 的 1 个定义（逐字节；行尾 LF 归一）。

    fail-closed 三步（与 --m2-4 同型）：
      1. 快照 SHA-256 / 字节数必须等于登记常量；
      2. 定义锚必须唯一命中、行号必须等于登记范围、正文不得引用
         device 成员 / registry 全局，且必须仍含 A4 调用链合同；
      3. 模块 .cpp 的 A9 BEGIN/END 标记之间正文必须与快照抽取结果**逐字节
         相同**（任一侧漂移即失败），且模块签名必须逐字命中。
    """
    default_snapshot = Path(os.environ.get("TEMP", ".")) / "device_pre_a9.cpp"
    source = Path(argv[0]) if argv else default_snapshot
    if not source.is_file():
        raise AssertionError(
            "fail-closed: 缺少 pre-A9 device.cpp 快照 %s\n"
            "（A9 第 0 步应已把迁移前的 src/d3d9/d3d9_device.cpp 复制到 "
            "%%TEMP%%\\device_pre_a9.cpp；或用参数显式给出路径）" % source
        )
    raw = source.read_bytes()
    actual_sha = hashlib.sha256(raw).hexdigest().upper()
    if actual_sha != A9_EXPECTED_SHA256 or len(raw) != A9_EXPECTED_SIZE:
        raise AssertionError(
            "fail-closed: pre-A9 快照身份与登记值不一致：\n"
            "  SHA-256 %s != %s\n  size %d != %d\n"
            "快照必须就是 A9 迁移前的那份工作树 device.cpp（= A10 删除落地后的状态）。"
            % (actual_sha, A9_EXPECTED_SHA256, len(raw), A9_EXPECTED_SIZE)
        )
    text = raw.decode("utf-8").replace("\r\n", "\n")

    if not A9_MODULE_CPP.is_file():
        raise AssertionError(
            "fail-closed: 缺少 A9 模块 %s（无法做正文逐字节一致性校验）"
            % A9_MODULE_CPP
        )
    if text.count(A9_DEV_SIG) != 1:
        raise AssertionError(
            "fail-closed: pre-A9 快照里 A9 定义锚不唯一：%d 处"
            % text.count(A9_DEV_SIG)
        )
    line, definition = extract_definition(text, A9_DEV_SIG)
    span = (line, line + definition.count("\n") - 1)
    if span != A9_LINES:
        raise AssertionError(
            "fail-closed: A9 行号 %s != 登记 %s" % (span, A9_LINES)
        )
    if not definition.startswith(A9_DEV_SIG):
        raise AssertionError("fail-closed: A9 定义不以设备侧签名开头")
    body = definition[len(A9_DEV_SIG):-2]
    if not (body.startswith("\n") and body.endswith("\n")):
        raise AssertionError("fail-closed: A9 正文边界异常")
    for forbidden in A9_FORBIDDEN:
        if forbidden in body:
            raise AssertionError(
                "fail-closed: A9 正文引用 device/registry 全局 %s" % forbidden
            )
    if "War3SemanticPaletteLooksModelLocal(" not in body:
        raise AssertionError(
            "fail-closed: A9 正文未调用 A4 compose-policy（调用链合同丢失）"
        )

    module_text = A9_MODULE_CPP.read_text(encoding="utf-8")
    if module_text.count(A9_MOD_SIG) != 1:
        raise AssertionError(
            "fail-closed: 模块 .cpp 里的 A9 签名不是恰好 1 处"
        )
    if module_text.count(A9_BEGIN) != 1 or module_text.count(A9_END) != 1:
        raise AssertionError(
            "fail-closed: 模块 .cpp 的 A9 BEGIN/END 标记不是恰好 1 对"
        )
    begin_end = module_text.index("\n", module_text.index(A9_BEGIN)) + 1
    module_body = module_text[begin_end:module_text.index(A9_END)]
    if module_body != body:
        raise AssertionError(
            "fail-closed: 模块 .cpp A9 BEGIN/END 之间的正文与 pre-A9 快照抽取"
            "结果不一致（逐字节比对失败）——迁移文本已漂移，必须人工复核。"
        )

    chunk = (
        "// --- pre-migration definition: d3d9_device.cpp line %d (%s,"
        " verbatim; body byte-identical to the module) ---\n%s"
        % (line, A9_KEY, definition)
    )

    header = f"""// AUTO-GENERATED legacy reference for the A9 dead-code-ruling migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration text of the dead-code
// function War3SemanticBuildWorldPaletteIfNeeded that the 2026-09-18 dead-code
// ruling moved (NOT deleted) out of src/d3d9/d3d9_device.cpp (:7161-7175, where
// it was marked [[maybe_unused]] with zero callers) into
// src/d3d9/war3/semantic/war3_live_palette_selection.{{h,cpp}}.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --a9
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-A9 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_a9.cpp)
//   SHA-256(pre-A9 work-tree d3d9_device.cpp) = {actual_sha}
//   size = {len(raw)} bytes, captured {A9_CAPTURED_AT}
//   The pre-A9 state equals the post-A10 work tree (same SHA as the A10 step's
//   post-migration d3d9_device.cpp); like M1/M2-x this is a work-tree file hash,
//   not a git blob hash (the work is uncommitted).
//
// Namespace rewrite rules (the ONLY mechanical edits, besides LF normalization):
//   * This file is included AFTER the M2-1/M2-2/M2-3/M2-5/M2-4/M2-5B .inc files
//     and shares their 'm2_legacy_reference' namespace. The body's UNQUALIFIED
//     call to War3SemanticPaletteLooksModelLocal (the A4 compose-policy)
//     therefore resolves to the M2-4 legacy copy in this same namespace - exactly
//     the call target the pre-A9 device.cpp text resolved through its
//     using-directive. The A9 diff battery consequently covers A9's own logic AND
//     the A4 contract in its call chain (A4 -> A3/A6/A7/A8 legacy copies).
//   * dxvk::war3::render::ObjectKind and dxvk::war3::IsReadableRange are the
//     SHARED real contract primitives (same headers on both sides, deliberately
//     not rewritten).
//   * EXACTLY ZERO lines are deleted or rewritten relative to the device.cpp
//     text: only the enclosing namespace changes and line endings are LF-
//     normalized. The generator fail-closes unless the module .cpp body between
//     its A9 BEGIN/END markers is byte-identical to this file's body.
using namespace dxvk;
using namespace dxvk::war3;

namespace m2_legacy_reference {{

{chunk}

}} // namespace m2_legacy_reference
"""

    A9_OUT_PATH.write_text(header, encoding="utf-8", newline="\n")
    out_bytes = A9_OUT_PATH.read_bytes()
    print(f"generated: {A9_OUT_PATH.relative_to(ROOT)}")
    print(f"size: {len(out_bytes)}")
    print(f"SHA-256: {hashlib.sha256(out_bytes).hexdigest().upper()}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--m2-2":
        main_m2_2(args[1:])
    elif args and args[0] == "--m2-3":
        main_m2_3(args[1:])
    elif args and args[0] == "--m2-5":
        main_m2_5(args[1:])
    elif args and args[0] == "--m2-4":
        main_m2_4(args[1:])
    elif args and args[0] == "--m2-5b":
        main_m2_5b(args[1:])
    elif args and args[0] == "--a9":
        main_a9(args[1:])
    else:
        main()
