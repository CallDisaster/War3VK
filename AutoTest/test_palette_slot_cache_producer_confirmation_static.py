from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp").read_text(
    encoding="utf-8"
)

# 2026-09-16 P0：shadow-core palette 槽位缓存必须经 producer 绑定复核，
# 但**不是**"只改成返回失败"——合法对象仍要有经过证明的替代来源。
assert '#include "../model/war3_model_hook.h"' in CORE

# ---- 1. 记忆槽位不可能在未经 producer 确认下被供出 ----
# 口径边界（2026-09-17 对抗性复核，必须随结论声明）：
#   A0-A5 只覆盖"命中条目后 +0x08 本帧无值 ⇒ 用记忆槽位"这一条供出路径。
#   另外两条路径**有意保留为未证兜底**（与 docs/agent-history/2026-09-16-architecture-review-followup.md
#   的"直读有效的快速路径行为不变"一致）：
#     (a) 命中条目 + currentSlotIndex 合法 ⇒ 覆写记忆并直接供出；
#     (b) 未命中条目 + currentSlotIndex 合法 ⇒ 首见插入并直接供出。
#   因此不得宣称"未经帧证明的 arena 读在代码路径上不可达"。
fn_start = CORE.index("static uint32_t FindOrUpdatePaletteSlotCache(")
fn_end = CORE.index("uint64_t HashMatrixPalette(", fn_start)
fn = CORE[fn_start:fn_end]

scan = fn.index("for (size_t i = 0; i < kMaxPaletteSlotCacheEntries; ++i)")
branch = fn.index("} else {", scan)
confirm = fn.index("if (producerConfirmed) {", branch)
confirm_ret = fn.index("return s_paletteSlotCache[i].paletteSlotIndex;", confirm)
rej_count = fn.index("g_paletteSlotCacheRejectedStaleCount.fetch_add(", confirm_ret)
rej_ret = fn.index("return 0xFFFFFFFFu;", rej_count)
assert branch < confirm < confirm_ret < rej_count < rej_ret

# 复核本身必须问 producer，并且要求槽位与缓存一致
assert "QueryRenderablePartPaletteSlot(" in fn[branch:confirm]
assert "boundSlotIndex == s_paletteSlotCache[i].paletteSlotIndex" in fn[branch:confirm]
# 命中与拒绝必须分开计数，便于度量"未误杀"与"拒绝后走替代路径"
assert "g_paletteSlotCacheServedAfterConfirmCount.fetch_add(" in fn[confirm:confirm_ret]
assert "g_paletteSlotCacheRejectedStaleCount.fetch_add(" in fn[rej_count:rej_ret]

# ---- 1b. 2026-09-17 Gap B 补强：A3/A4/A5 三要素与细分拒绝 ----
# A3 groupCount 必须参与判定（对齐 device 侧 8688 口径）
assert "boundGroupCount >= requiredPaletteCount" in fn[branch:confirm]
# A4 绑定帧新鲜度：当前帧可读且差值不超过常量容差
assert "QueryCurrentPaletteFrameTag(" in fn[branch:confirm]
assert "kPaletteSlotCacheMaxFrameTagDelta" in fn[branch:confirm]
# A5 槽位区间帧同源：missing==0 且 min==max==绑定帧
assert "QueryBlendedPaletteFrameTagRange(" in fn[branch:confirm]
assert "slotRangeMissingCount == 0u" in fn[branch:confirm]
assert "slotRangeMinFrameTag == slotRangeMaxFrameTag" in fn[branch:confirm]
assert "slotRangeMaxFrameTag == boundFrameTag" in fn[branch:confirm]
# 槽位域必须保证 [slot, slot+requiredCount) 整段合法，且判定式必须**溢出安全**
# （2026-09-17 对抗性复核 A-4：不得写成 boundSlotIndex + requiredPaletteCount <= 上界，
#  requiredPaletteCount 极大时 uint32 回绕会绕过上界）
assert "boundSlotIndex + requiredPaletteCount <= 0x3A98u" not in fn[branch:confirm]
assert "requiredPaletteCount <= 0x3A98u" in fn[branch:confirm]
assert "boundSlotIndex <= 0x3A98u - requiredPaletteCount" in fn[branch:confirm]
# 直读两条路径必须仍然存在（有意保留的未证兜底；移除它们时须同时更新上面的口径注释）
assert "s_paletteSlotCache[cacheSlot].renderablePart = renderablePart;" in fn
assert "// 没有找到缓存条目，添加新的" in fn
# 细分拒绝计数器存在且和聚合同分支（sum(细分) == RejectedStale 可对账）
for name in ("g_paletteSlotCacheBindingMissRejectCount",
             "g_paletteSlotCacheGroupShortRejectCount",
             "g_paletteSlotCacheBindingFrameStaleRejectCount",
             "g_paletteSlotCacheSlotRangeStaleRejectCount"):
    assert name + ".fetch_add(" in fn[rej_count:rej_ret], name
assert "g_paletteSlotCacheFrameProofServedCount.fetch_add(" in fn[confirm:confirm_ret]
# 容差常量默认必须为 0（严格同帧；放宽需实机证据驱动的下一候选）
assert "kPaletteSlotCacheMaxFrameTagDelta = 0u" in CORE

# 跨地图会话隔离语义必须存活（issue6 合同）
assert "g_paletteSlotCacheSessionGeneration" in fn
assert "for (auto& entry : s_paletteSlotCache)" in fn

# ---- 2. producer 快照优先于按 slot 读 Game.dll arena ----
lam_start = CORE.index("auto tryEngineDirectPosePalette = [&]() -> bool {")
lam_end = CORE.index("// 优先使用引擎的全局调色板缓冲区", lam_start)
lam = CORE[lam_start:lam_end]
required = lam.index("const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;")
snapshot = lam.index("QueryRenderablePartPaletteSnapshot(")
slot_resolve = lam.index("FindOrUpdatePaletteSlotCache(")
arena = lam.index("kGlobalPaletteBufferRva")
assert required < snapshot < slot_resolve < arena
# 补强：调用点必须把 requiredCount 转发给复核链，并判 [slot, slot+requiredCount) 上界
assert "renderable.renderablePart, paletteSlotIndex, requiredCount" in lam[slot_resolve:arena]
assert "paletteSlotIndex + requiredCount > 0x3A98u" in lam[slot_resolve:arena]
# 快照必须按需要的矩阵数**精确**校验（2026-09-17 Gap B 补强：原 >= 不足以排除
# "多带了别人槽位"的调色板；同时加 producer 上限与快照帧新鲜度）
assert "size() == size_t(requiredCount)" in lam[snapshot:slot_resolve]
assert "kProducerSnapshotMaxCount = 64u" in lam[:snapshot]
assert "requiredCount <= kProducerSnapshotMaxCount" in lam[:snapshot]
assert "&snapshotFrameTag" in lam[snapshot:slot_resolve]
assert "QueryCurrentPaletteFrameTag(" in lam[snapshot:slot_resolve]
# 快照陈旧必须被计数（不得静默回退），但**只统计确实超差**的情形：
# 2026-09-17 对抗性复核 A-3 —— 当前帧不可读 / snapshotFrameTag == 0 属不可证明，
# 必须排除在该计数之外，否则会污染"是否放宽 kDelta"的判据。
_stale = lam[snapshot:slot_resolve]
assert "g_paletteSlotCacheSnapshotFrameStaleRejectCount.fetch_add(" in _stale
assert "if (currentReadable && snapshotFrameTag != 0u) {" in _stale
# 调用点必须先把 requiredCount 限制在 [1,256]（内核/适配层的硬前置）
assert "requiredCount == 0u || requiredCount > 256u" in lam
# producer 快照上限必须与 war3_model_hook.cpp 的单一来源常量一致（2026-09-17 A-5）
import re as _re
HOOK_SRC = (ROOT / "src/d3d9/war3/model/war3_model_hook.cpp").read_text(
    encoding="utf-8")
_hook_max = _re.search(r"kRenderablePartPaletteSnapshotMaxCount\s*=\s*(\d+)u", HOOK_SRC)
_core_max = _re.search(r"kProducerSnapshotMaxCount\s*=\s*(\d+)u", lam)
assert _hook_max is not None, "找不到 producer 快照上限常量"
assert _core_max is not None, "找不到 core 侧快照上限常量"
assert _hook_max.group(1) == _core_max.group(1), "两侧快照上限常量已漂移"
assert lam.count("const uint32_t requiredCount = outMaxVertexGroupSlot + 1u;") == 1
assert "if (requiredCount == 0u || requiredCount > 256u)" in lam

# ---- 3. 经过验证的 CPU 替代路径仍保留（否则就是"只拒绝"） ----
tail = CORE[lam_end:]
assert "if (!resource.hasSkinningData())" in tail
assert "uniqueGroupSlots" in tail

# ---- 4. 合同 ON 的严格路径未被本次加固改动（该分支原在 device 侧） ----
# M2-2：该严格分支已随选择链本体迁往 war3/semantic/war3_live_palette_selection.cpp
#（逐字节）；断言跟随符号改锚到模块，断言内容一个字未改。
MODULE = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(
    encoding="utf-8"
)
assert "if (dxvk::war3::render::skin::ContractEnabled()) {" in MODULE
contract = MODULE.index("if (dxvk::war3::render::skin::ContractEnabled()) {")
contract_block = MODULE[contract:contract + 3000]
assert "QueryOwnedRenderablePartPaletteSnapshot(" in contract_block
assert "no legacy/raw/Pose fallback is admissible" in contract_block

print("palette slot cache producer-confirmation static checks passed")
