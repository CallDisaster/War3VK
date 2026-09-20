from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
# M2-2：resolvePaletteSlotIndex lambda 与选择链本体已迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（逐字节）。
# 本门禁的函数体断言跟随符号改锚到模块；断言内容一个字未改。
MODULE = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(
    encoding="utf-8"
)

# 2026-09-16 P0 Gap A：device 侧 palette 记忆槽位必须经 producer 绑定复核
# 后才供出（路由式修复，不是只改成返回失败）；被拒对象必须仍能落到
# PoseFallback 替代路径，且正向快路径（复核通过供出）必须保留。

start = MODULE.index("auto resolvePaletteSlotIndex =")
end = MODULE.index("// 首选：从 Hook_RuntimeMatrixWrite", start)
body = MODULE[start:end]

# ---- 0. 复核计数器存在（区分"经复核仍命中"与"陈旧被拒"，与 Gap B 同风格） ----
assert "g_devicePaletteSlotCacheServedAfterConfirmCount" in DEVICE
assert "g_devicePaletteSlotCacheRejectedStaleCount" in DEVICE

# ---- 1. queryProducerBindingSlot 已扩展为同时取回 groupCount/frameTag ----
q_start = body.index("auto queryProducerBindingSlot =")
q_end = body.index("auto useCachedEntry =", q_start)
query = body[q_start:q_end]
assert "uint32_t* outGroupCount" in query
assert "uint32_t* outFrameTag" in query
assert "QueryRenderablePartPaletteSlot(" in query
assert "&boundGroupCount, &boundFrameTag" in query

# ---- 2. entry 结构携带 producer 复核所需的 groupCount/frameTag ----
e_start = body.index("struct PaletteSlotCacheEntry {")
e_end = body.index("};", e_start)
entry_struct = body[e_start:e_end]
assert "uint32_t paletteSlotIndex" in entry_struct
assert "uint32_t paletteGroupCount" in entry_struct
assert "uint32_t paletteFrameTag" in entry_struct
# 身份/epoch 复核字段不得被取消
assert "void* renderablePart" in entry_struct
assert "uint64_t mapEpoch" in entry_struct

# ---- 3. useCachedEntry：供出记忆槽位前必须经 producer 复核 ----
u_start = body.index("auto useCachedEntry =")
fast = body.index("if (lookup.renderablePart == partPtr")
use = body[u_start:fast]

# +0x08 当前槽位合法的写入路径保留不变
cur = use.index("if (currentSlotIndex != 0xFFFFFFFFu && currentSlotIndex < 0x3A98u)")
assert "entry.paletteSlotIndex = currentSlotIndex;" in use[cur:]
assert "return currentSlotIndex;" in use[cur:]

# 复核条件的三个要素：producer 命中、槽位与记忆一致、groupCount 兼容
confirm = use.index("producerSlotIndex == entry.paletteSlotIndex")
assert "producerSlotIndex != 0xFFFFFFFFu" in use[:confirm]
# 2026-09-18 批次 4（步骤①）：数量规则已抽为**单一实现**
# `dxvk::war3::render::skin::ProducerGroupCountCovers`，热缓存与冷缓存都调用它。
# 因此这里断言「调用共享规则」而非原先各写一份的字面比较。
assert "dxvk::war3::render::skin::ProducerGroupCountCovers(" in use[confirm:]
assert "producerGroupCount >= requiredPaletteCount" not in use, \
    "不得再出现各写一份的字面比较（规则必须单一实现）"

# 复核通过才供出记忆槽位（既有加速器测试的 return entry.paletteSlotIndex 语义）
serve = use.index("return entry.paletteSlotIndex;", confirm)
assert confirm < serve
assert "g_devicePaletteSlotCacheServedAfterConfirmCount.fetch_add(" in use[confirm:serve]

# 复核不通过 -> 返回 0xFFFFFFFFu，不再无条件供出记忆槽位
rej_count = use.index("g_devicePaletteSlotCacheRejectedStaleCount.fetch_add(", serve)
rej_ret = use.index("return 0xFFFFFFFFu;", rej_count)
assert serve < rej_count < rej_ret
# 拒绝路径之后不得再出现任何供出记忆槽位的语句
assert "return entry.paletteSlotIndex" not in use[rej_ret:]
# fail-open 旧形态（producer miss 兜底供出记忆槽位）必须已消除：
# 供出语句只允许出现在复核条件之后
assert use.index("return entry.paletteSlotIndex;") > confirm

# ---- 4. 直映射加速器与环形分配合同保留 ----
assert "s_paletteSlotCacheLookup" in body
assert "s_paletteSlotCacheCursor++" in body
assert "cached.renderablePart == partPtr && cached.mapEpoch == mapEpoch" in body
assert "entry.renderablePart != partPtr || entry.mapEpoch != mapEpoch" in body

# ---- 5. 正向侧：slotIndex 非法时不进 slot 块，PoseFallback 仍可达 ----
caller = MODULE[end:]
slot_guard = caller.index("if (slotIndex != 0xFFFFFFFFu && slotIndex < 0x3A98u) {")
arena = caller.index("0xBC6BD0", slot_guard)
blended = caller.index("QueryBlendedPaletteBySlotIndex(", slot_guard)
pose_fallback = caller.index("War3LivePaletteBuildPhase::PoseFallback")
assert slot_guard < arena < pose_fallback
assert slot_guard < blended < pose_fallback
# PoseFallback 替代路径的关键构件仍在（已发布姿态 + producer-owner 反查）
pose_block = caller[pose_fallback:]
assert "PoseRegistry::instance().findByRuntimeModel(" in pose_block
assert "QueryRenderablePartOwnerRuntimeModel(" in pose_block

# ---- 6. 合同 ON 严格分支未受本修复影响 ----
contract = MODULE.index("if (dxvk::war3::render::skin::ContractEnabled()) {")
contract_block = MODULE[contract:contract + 3000]
assert "QueryOwnedRenderablePartPaletteSnapshot(" in contract_block
assert "no legacy/raw/Pose fallback is admissible" in contract_block
assert contract < start  # 合同分支在 resolvePaletteSlotIndex 之前，先行返回

print("device palette slot cache producer-confirmation static checks passed")
