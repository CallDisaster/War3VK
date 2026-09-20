"""2026-09-17 S2（收窄版）共享纯计算内核的防回退门禁。

目的：把"分组调色板重映射 / group 表校验 / 分组等权平均"钉死为**单一来源**，
并保证薄适配层没有被顺手扩张成"合并来源链"。本测试只读源码文本，不运行产品。

边界声明：本测试证明的是**结构/接线**，不证明画面或性能；S2 候选未实机。
"""
import re
from pathlib import Path


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) for line in text.split("\n"))


ROOT = Path(__file__).resolve().parents[1]
KERNEL_PATH = ROOT / "src/d3d9/war3/render/war3_runtime_group_palette_kernel.h"
TEST_PATH = (
    ROOT / "src/d3d9/war3/render/tests/war3_runtime_group_palette_kernel_test.cpp"
)
CORE_PATH = ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp"
UPPER_PATH = ROOT / "src/d3d9/war3/render/war3_upper_layer_shadow.cpp"
MESON_PATH = ROOT / "src/d3d9/meson.build"

assert KERNEL_PATH.exists(), "S2 内核头缺失"
assert TEST_PATH.exists(), "S2 生产函数测试缺失"

KERNEL = KERNEL_PATH.read_text(encoding="utf-8")
KERNEL_CODE = strip_comments(KERNEL)
TEST = TEST_PATH.read_text(encoding="utf-8")
CORE = CORE_PATH.read_text(encoding="utf-8")
UPPER = UPPER_PATH.read_text(encoding="utf-8")
MESON = MESON_PATH.read_text(encoding="utf-8")

# ---- 1. 内核是唯一来源，且两个 fallback 集合都在 ----
assert "inline bool TryBuildRuntimeGroupPaletteKernel(" in KERNEL_CODE
assert "enum class RuntimeGroupPaletteFallbackSet" in KERNEL_CODE
assert "MatrixAndPoseRemap" in KERNEL_CODE
assert "MatrixPoseAndUniformRoot" in KERNEL_CODE
assert "ScanRuntimeGroupPaletteSlots" in KERNEL_CODE
assert "ScanRuntimeGroupPaletteSlotsFixed" in KERNEL_CODE
assert "RuntimeGroupPaletteSlotScanFixed" in KERNEL_CODE
assert "ClassifyRuntimeGroupPaletteOutputPoseAlias" in KERNEL_CODE
assert "ClassifyRuntimeGroupPaletteByteRangeOverlap" in KERNEL_CODE
assert "RuntimeGroupPaletteTryByteLength" in KERNEL_CODE
assert "RuntimeGroupPaletteAliasRelation" in KERNEL_CODE
assert "aliasedInputOutput" in KERNEL_CODE

# ---- 2. 内核纯度：零内存读取、零来源概念、零上界收窄 ----
for forbidden in (
    "SafeRead",
    "GetModuleHandleA",
    "IsReadableRange",
    "DecodeRuntimePoseMatrix48",
    "renderablePart",
    "paletteSlotIndex",
    "kGlobalPaletteBufferRva",
    "Producer",
    "Selection",
    "arena",
    "std::min",
):
    assert forbidden not in KERNEL_CODE, "内核出现禁止概念: " + forbidden

# 内核只 include 纯数学 util_matrix.h（无 war3 记录类型头）
# 2026-09-17 对抗性复核 B-1：groupCount > 0 却给空 group 表视图必须被守卫（不得解引用）
assert "in.groupCount != 0u && in.matrixGroupSizes == nullptr" in KERNEL_CODE
assert "in.matrixIndexCount != 0u && in.matrixIndices == nullptr" in KERNEL_CODE
assert "RuntimeGroupPaletteKernelMiss::InvalidGroupTable" in KERNEL_CODE
assert '#include "../../../util/util_matrix.h"' in KERNEL_CODE
assert "precomputedMaxVertexGroupSlot" not in KERNEL_CODE, (
    "内核不得保留 caller-provided trusted max 参数"
)
assert "CollectRuntimeGroupPaletteUniqueSlots" not in KERNEL_CODE, (
    "旧 unique vector 扫描实现必须被单趟固定数组替换"
)
for banned_include in ("war3_model_hook.h", "d3d9_device.h", "war3_shadow_renderer_core.h"):
    assert banned_include not in KERNEL_CODE, banned_include

# ---- 3. 分组等权平均与行结构只在内核里存在（被改造的两个函数体内无重复实现） ----
assert "Matrix4 accum(0.0f);" in KERNEL_CODE
assert "groupSize == 1u ? accum : (accum / float(groupSize))" in KERNEL_CODE
assert "if (groupSize > 1u)" in KERNEL_CODE

# 注意：core.cpp 另有一处 tupleCount 变体（附件/tuple 平均），**不在 S2 范围内**，
# 因此断言必须收敛到被改造的两个函数体，而不是整文件。
CORE_FN_START = "bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,"
CORE_FN_END = "bool ShouldBuildAttachmentSupplementalForChunk("
UPPER_FN_START = "bool TryBuildRuntimeGroupPalette(const model::ShadowGeosetResourceRecord &geoset,"
UPPER_FN_END = "} // namespace"

core_fn = CORE[CORE.index(CORE_FN_START):CORE.index(CORE_FN_END, CORE.index(CORE_FN_START))]
upper_fn = UPPER[UPPER.index(UPPER_FN_START):UPPER.index(UPPER_FN_END, UPPER.index(UPPER_FN_START))]
for name, body in (("core", core_fn), ("upper", upper_fn)):
    assert "Matrix4 accum(0.0f);" not in body, name + " 适配层仍有重复平均实现"
    assert "accum / float(groupSize)" not in body, name + " 适配层仍有重复平均实现"
    assert "accum += " not in body, name + " 适配层仍有重复累加实现"
    assert "matrixGroupSizes[" not in body, name + " 适配层仍在自行遍历 group 表"
    # 必须通过内核取得结果
    assert "TryBuildRuntimeGroupPaletteKernel(" in body, name + " 未调用共享内核"

# ---- 4. 两侧适配层各用各自的 fallback 集合（D2 差异不得被合并） ----
# core 在 dxvk::war3::shadow 命名空间（需 render:: 限定）；upper 在 dxvk::war3::render（无限定）
assert "render::RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot" in CORE
assert "RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap" in UPPER
assert "RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap" not in CORE
assert "RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot" not in UPPER
# 每侧只能出现一次 fallback 集合选择（防止一侧被改成另一侧语义）
assert CORE.count("RuntimeGroupPaletteFallbackSet::") == 1
assert UPPER.count("RuntimeGroupPaletteFallbackSet::") == 1

# ---- 5. 来源链与前置校验必须留在适配层（不得搬进内核） ----
for anchor in (
    "auto tryEngineDirectPosePalette = [&]() -> bool {",
    "QueryRenderablePartPaletteSnapshot(",
    "FindOrUpdatePaletteSlotCache(",
    "NoSkinningData",
    "uniqueGroupSlots",
    "logFailure",
):
    assert anchor in CORE, "core 适配层丢失: " + anchor
assert "geoset.vertexGroupCount" in UPPER and "geoset.matrixGroupCount" in UPPER

# ---- 6. 两个 TryBuildRuntimeGroupPalette 的签名与唯一调用点形态未变 ----
assert "bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource," in CORE
assert "bool TryBuildRuntimeGroupPalette(const model::ShadowGeosetResourceRecord &geoset," in UPPER
assert "out.hasRuntimeGroupPalette = TryBuildRuntimeGroupPalette(" in UPPER
assert "TryBuildRuntimeGroupPaletteKernel(" in CORE and "render::TryBuildRuntimeGroupPaletteKernel(" in CORE

# ---- 7. 生产函数测试与 meson 目标必须在位 ----
assert "war3_runtime_group_palette_kernel" in MESON
assert MESON.count("war3_runtime_group_palette_kernel_test") >= 2
assert "TryBuildRuntimeGroupPaletteKernel(" in TEST
for case in ("T1", "T5", "T6", "T7", "T10", "T13"):
    assert case in TEST, "生产函数测试缺少用例标记: " + case
# 测试必须真的调用生产函数，而不是只做字符串断言
assert TEST.count("TryBuildRuntimeGroupPaletteKernel(") >= 1

# ---------------------------------------------------------------------------
# 2026-09-17 S2 补充（可复现随机差分 + 适配层派生/成本口径）
# 上级要求：随机差分程序、种子、旧实现身份及输出都必须保留，且覆盖适配层。
# ---------------------------------------------------------------------------
import hashlib

DIFF_PATH = (
    ROOT
    / "src/d3d9/war3/render/tests/war3_runtime_group_palette_kernel_diff_test.cpp"
)
assert DIFF_PATH.exists(), "S2 随机差分目标缺失（旧实现身份与 30 万次零差异不可复现）"
DIFF = DIFF_PATH.read_text(encoding="utf-8")

# 8.0 差分目标与测试必须在 meson 里登记
assert DIFF_PATH.name in MESON, "meson 未登记差分目标源文件"
assert "war3_runtime_group_palette_kernel_diff_test = executable(" in MESON
assert MESON.count("war3_runtime_group_palette_kernel_diff_test") >= 2, (
    "差分目标缺少 executable()/test() 成对登记"
)
assert "'war3_runtime_group_palette_kernel_diff'," in MESON

# 8.1 固定种子常量与生成算法必须写进程序（结果可复现）
assert "kSeedCoreGroup = 0x5332464600000001ull" in DIFF, "core 组种子常量缺失"
assert "kSeedUpperGroup = 0x5332464600000002ull" in DIFF, "upper 组种子常量缺失"
assert "kGroupIterations = 300000ull" in DIFF, "每组 30 万次迭代常量缺失"
assert "SplitMix64" in DIFF, "PRNG 生成算法未写进程序"
for prng_constant in ("0x9E3779B97F4A7C15ull", "0xBF58476D1CE4E5B9ull",
                      "0x94D049BB133111EBull"):
    assert prng_constant in DIFF, "PRNG 常量缺失: " + prng_constant

# 8.2 摘要输出字段：种子 / 两组输入数 / 各自 mismatch 数 / 旧实现副本 SHA-256
for token in (
    "SUMMARY",
    "seedCore=",
    "seedUpper=",
    "iterationsPerGroup=",
    "coreInputs=",
    "coreMismatches=",
    "upperInputs=",
    "upperMismatches=",
    "legacyCoreSha256=",
    "legacyUpperSha256=",
):
    assert token in DIFF, "差分摘要缺少字段: " + token
assert "不据此宣称热路径成本等价" in DIFF, "差分测试必须显式声明不宣称成本等价"

# 8.3 成本口径：测试 TU 内重载全局 operator new/delete（产品代码零改动）
assert "void* operator new(std::size_t n)" in DIFF
assert "void operator delete(void* p) noexcept" in DIFF
assert "peakLiveBytes" in DIFF and "maxSingleAllocBytes" in DIFF
assert "pointerChanged" in DIFF

# 8.4 旧实现副本（逐语句抽取）必须在位：5 步 core + 4 步 upper
assert "namespace legacy {" in DIFF, "旧实现副本缺少独立 legacy 命名空间"
assert "bool TryBuildRuntimeGroupPaletteFiveStep(" in DIFF, "5 步旧实现副本缺失"
assert "bool TryBuildRuntimeGroupPaletteFourStep(" in DIFF, "4 步旧实现副本缺失"
for anchor in (
    "Matrix4 accum(0.0f);",
    "groupSize == 1u ? accum : (accum / float(groupSize))",
    "outPalette.assign(paletteCount, pose.matrixPalette.front());",
    "uniqueGroupSlots.reserve(outMaxVertexGroupSlot + 1u);",
    "constexpr std::size_t kMaxSemanticVertexGroupSlots = 128u * 1024u;",
    "seenGroupSlots[groupSlot] = true;",
):
    assert anchor in DIFF, "旧实现副本丢失逐语句锚点: " + anchor

# 8.5 旧实现身份（pre-S2 副本 SHA-256）必须被记录；副本可读时逐个复核
LEGACY_CORE_SHA = "0F7B720E1FAF5B7065CE40DFCE4D7462A5AC14D3217DDA6716A395228030D8F9"
LEGACY_UPPER_SHA = "61EB0C8FFDF24F36016B1651A7C677BE0C47E805680C5278F0CE18BD950517A9"
assert LEGACY_CORE_SHA in DIFF, "旧实现副本 core SHA-256 未被记录"
assert LEGACY_UPPER_SHA in DIFF, "旧实现副本 upper SHA-256 未被记录"
assert "warvk_s2_backup" in DIFF, "旧实现副本来源路径未被记录"
LEGACY_BACKUPS = (
    (Path(r"C:\Windows\Temp\warvk_s2_backup\war3_shadow_renderer_core.cpp"),
     LEGACY_CORE_SHA),
    (Path(r"C:\Windows\Temp\warvk_s2_backup\war3_upper_layer_shadow.cpp"),
     LEGACY_UPPER_SHA),
)
for backup, expected in LEGACY_BACKUPS:
    if backup.exists():
        actual = hashlib.sha256(backup.read_bytes()).hexdigest().upper()
        assert actual == expected, (
            "旧实现副本身份不符: %s 实际 %s != 记录 %s" % (backup, actual, expected)
        )
    else:
        print("note: pre-S2 backup 不在本机，跳过身份复核: " + str(backup))

# 8.6 适配层覆盖说明：不可链接 -> 派生建模 + 内核差分；派生规则必须与源码同步
assert "DeriveCoreAdapterInputs" in DIFF and "DeriveUpperAdapterInputs" in DIFF
assert "无法在宿主机链接" in DIFF, "差分测试必须说明适配层不可链接的原因"
assert "RuntimeVertexGroupSlotCount" in DIFF
assert "kMaxSemanticVertexGroupSlots = 128u * 1024u" in CORE, (
    "core 的 128Ki clamp 文本改动，差分测试的派生建模需同步"
)
assert "vertexGroupCount == 0u" in UPPER and "matrixGroupCount" in UPPER
assert "三条前置校验" in DIFF

# 8.7 内核 miss 枚举顺序必须与 core 的 RuntimeGroupPaletteMissReason 逐值一致
# （差分测试用 legacy 局部枚举 + 8 条 static_assert 把该映射钉成编译期事实）
def _enum_members(text, declaration):
    start = text.index(declaration)
    body = text[text.index("{", start) + 1:text.index("};", start)]
    return re.findall(r"[A-Za-z_][A-Za-z0-9_]*", body)


CORE_MISS = _enum_members(CORE, "enum class RuntimeGroupPaletteMissReason")
KERNEL_MISS = _enum_members(KERNEL_CODE, "enum class RuntimeGroupPaletteKernelMiss")
assert CORE_MISS == KERNEL_MISS, (
    "内核 miss 枚举与 core 的 miss reason 顺序不一致: %s vs %s"
    % (KERNEL_MISS, CORE_MISS)
)
assert CORE_MISS[0] == "None" and CORE_MISS[-1] == "FallbacksFailed"
for member in CORE_MISS:
    assert "uint32_t(legacy::MissReason::%s)" % member in DIFF or (
        "legacy::MissReason::%s)" % member
    ) in DIFF, "差分测试缺少 miss 枚举映射断言: " + member

# 8.8 槽位扫描/安全结构（single-source；内核单趟有界扫描）
SCAN_LOOP = "for (size_t i = 0u; i < vertexGroupSlotCount; ++i)"
# b02：不得保留 caller-provided trusted max；内核用固定 256 项数组单趟求 max+unique。
for name, body in (("kernel", KERNEL_CODE), ("core", core_fn), ("upper", upper_fn)):
    assert "precomputedMaxVertexGroupSlot" not in body, (
        name + " 仍引用 caller-provided trusted max 参数"
    )
assert "ScanRuntimeGroupPaletteSlotsFixed(" in KERNEL_CODE, (
    "内核缺少固定 256 项单趟扫描原语"
)
assert "std::array<bool, 256> seenGroupSlots" in KERNEL_CODE, (
    "内核单趟扫描缺少固定 seen 数组"
)
assert "ScanRuntimeGroupPaletteSlots(" in KERNEL_CODE, (
    "公开扫描测试接口已被删除"
)
# core 适配层只在 logFailure（FallbacksFailed 诊断）内部按需重建 unique 槽位；
# 来源链前的 maxSlot 走共享 FindRuntimeGroupPaletteMaxSlot，不重复实现扫描。
_logfailure = core_fn.index("auto logFailure = [&]()")
_pre_logfailure = core_fn[:_logfailure]
assert "render::FindRuntimeGroupPaletteMaxSlot(" in _pre_logfailure, (
    "core 适配层未复用共享 maxSlot 扫描原语"
)
assert _pre_logfailure.count(SCAN_LOOP) == 0, (
    "core 适配层在 logFailure 之前仍有内联槽位扫描"
)
assert "uniqueGroupSlots" not in _pre_logfailure, (
    "core 适配层在 logFailure 之前仍无条件收集 unique 槽位"
)
assert core_fn.count(SCAN_LOOP) == 1, (
    "core 适配层的槽位遍历只应留在 logFailure 内部"
)
_dedup_push = core_fn.index("uniqueGroupSlots.push_back")
assert _dedup_push > _logfailure, (
    "unique 槽位重建必须位于 logFailure 内部（仅在诊断时执行）"
)
assert "uniqueGroupSlots" in core_fn[_logfailure:], (
    "logFailure 必须仍消费 uniqueGroupSlots"
)
assert core_fn.count("uniqueGroupSlots") == core_fn[_logfailure:].count(
    "uniqueGroupSlots"
), "适配层 unique 槽位只能在 logFailure 内部出现/消费"
assert core_fn.count("logFailure();") == 1
assert "RuntimeGroupPaletteKernelMiss::FallbacksFailed" in core_fn
# 内核必须拒绝 input posePalette 与 output storage 别名，而不是 clear/resize 后越界读。
assert "ClassifyRuntimeGroupPaletteOutputPoseAlias" in KERNEL_CODE
assert "ClassifyRuntimeGroupPaletteByteRangeOverlap" in KERNEL_CODE
assert "RuntimeGroupPaletteTryByteLength" in KERNEL_CODE
assert "RuntimeGroupPaletteAliasRelation" in KERNEL_CODE
assert "aliasedInputOutput" in KERNEL_CODE
assert "outPalette.data() + outPalette.capacity()" not in KERNEL_CODE, (
    "alias helper 不得构造 capacity 端点指针"
)
assert "poseBegin + in.posePaletteSize" not in KERNEL_CODE, (
    "alias helper 不得构造 input 端点指针"
)
assert "std::less<const Matrix4*>" not in KERNEL_CODE, (
    "alias helper 应使用整数半开区间分类"
)
# 输出容量复用：生产适配层必须把调用方 vector 直接交给内核，不得再出现
# 局部 RuntimeGroupPaletteOutput + std::move。
for name, body in (("core", core_fn), ("upper", upper_fn)):
    assert "RuntimeGroupPaletteOutput kernelOutput" not in body, (
        name + " 仍有局部内核输出对象"
    )
    assert "std::move(kernelOutput.palette)" not in body, (
        name + " 仍通过 move 局部输出丢失调用方容量"
    )
    assert "TryBuildRuntimeGroupPaletteKernel(" in body, (
        name + " 未调用共享内核"
    )
assert "&outMaxVertexGroupSlot" not in core_fn, (
    "core 仍在传 caller-provided trusted max 指针"
)
assert upper_fn.count(SCAN_LOOP) == 0, "upper 适配层不应再有内联槽位扫描"

# 8.9 差分测试必须真的调用生产内核与扫描函数（不是纯字符串断言）
assert DIFF.count("TryBuildRuntimeGroupPaletteKernel(") >= 1
assert "ScanRuntimeGroupPaletteSlots(" in DIFF
assert DIFF.count("kGroupIterations") >= 3
assert "逐位" in DIFF or "bitwise" in DIFF

print("runtime group palette kernel single-source static checks passed")
