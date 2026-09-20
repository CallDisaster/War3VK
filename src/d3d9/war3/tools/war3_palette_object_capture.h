#pragma once

#include <cstdint>

#include "war3_palette_object_evidence.h"

// 2026-09-17 上级裁定（Step 1③）——对象级 palette 证据的**生产侧采集支持**。
// 本头只依赖记录器头（+ <cstdint>），不含 d3d9 / nlohmann / Windows 头，采集点与宿主机测试共用。
//
// 纪律（逐条对应上级裁定）：
//  1) **子门短路由采集点自己做**：每个采集点必须先
//     `if (!PaletteObjectEvidenceEnabled()) skip;`，之后才允许取值 / 构造键 / 查表。
//     本头的所有函数都**不**做子门判定，关闭时不得调用。
//  2) R 的具名原因必须来自**实际执行的失败分支**；未执行的检查是 NotChecked，
//     不得用 R0 冒充。本头用同一个分类函数同时驱动"聚合细分计数分支"与"POD 里的原因"，
//     两者不可能漂移。
//  3) identityWeak / epochUnknown / 帧域口径集中在本头，避免三个采集点各写一份而漂移。

namespace dxvk::war3::tools::evidence {

// ---------------------------------------------------------------------------
// R：FindOrUpdatePaletteSlotCache 的"记忆槽位复核"（A0-A5）具名原因 POD。
// 只承载**当场已经读取 / 已经算出**的局部值：不新增查询、哈希、锁、分配。
// ---------------------------------------------------------------------------
struct PaletteSlotRecheckEvidence {
  // 复核链是否**执行**过。false ⇒ 原因必须是 NotChecked（无缓存条目 / 快路径命中 /
  // 新建条目 / 最终 miss / renderablePart == nullptr 都不执行这条链）。
  bool recheckPerformed = false;
  // true ⇒ 复核通过（继续服务记忆槽位），本次没有拒绝原因。
  bool producerConfirmed = false;
  PaletteObjectRejectReason rejectReason = PaletteObjectRejectReason::NotChecked;
  // ---- 当场局部布尔（A0-A5 判据；未参与判定的保持 false，不得事后补算）----
  bool bindingHit = false;
  bool slotDomainValid = false;
  bool boundSlotIndexMatchesRemembered = false;
  bool groupCountSuffices = false;
  bool bindingFrameFresh = false;
  bool slotRangeFrameFresh = false;
  // ---- 当场局部数值（不重新查询补证）----
  uint32_t requiredPaletteCount = 0u;
  uint32_t currentSlotIndexRaw = 0u;
  uint32_t rememberedSlotIndex = 0u;
  uint32_t boundSlotIndex = 0u;
  uint32_t boundGroupCount = 0u;
  uint32_t boundFrameTag = 0u;
  uint32_t currentPaletteFrameTag = 0u;
  uint32_t slotRangeMinFrameTag = 0u;
  uint32_t slotRangeMaxFrameTag = 0u;
  uint32_t slotRangeMissingCount = 0u;
};

// 生产具名原因分类：函数体 = war3_shadow_renderer_core.cpp 拒绝分支的条件与顺序
// （R0 → R1 → R2 → 其余 = R3），逐字保留。
inline PaletteObjectRejectReason ClassifyPaletteSlotRecheckReject(
    bool bindingHit, bool slotDomainValid,
    bool boundSlotIndexMatchesRemembered, bool groupCountSuffices,
    bool bindingFrameFresh) noexcept {
  if (!bindingHit || !slotDomainValid || !boundSlotIndexMatchesRemembered)
    return PaletteObjectRejectReason::R0;
  if (!groupCountSuffices)
    return PaletteObjectRejectReason::R1;
  if (!bindingFrameFresh)
    return PaletteObjectRejectReason::R2;
  return PaletteObjectRejectReason::R3;
}

// 生产填充：**仅当 out != nullptr** 时写入（子门关闭时调用方传 nullptr ⇒ 零填充、
// 不构造、不写任何字段）。
inline void PublishPaletteSlotRecheckEvidence(
    PaletteSlotRecheckEvidence* out,
    const PaletteSlotRecheckEvidence& value) noexcept {
  if (out != nullptr)
    *out = value;
}

// 生产门：只有"复核链执行过 + 确实被拒绝 + 原因具名"才允许发具名拒绝事件。
// 未执行的检查（recheckPerformed == false）与 NotChecked/Unknown **一律不发**，
// 因此不可能把"没检查"写成 R0。
inline bool ShouldNotifyPaletteSlotReject(
    const PaletteSlotRecheckEvidence& evidence,
    PaletteObjectRejectReason& outReason) noexcept {
  if (!evidence.recheckPerformed || evidence.producerConfirmed)
    return false;
  if (evidence.rejectReason == PaletteObjectRejectReason::NotChecked ||
      evidence.rejectReason == PaletteObjectRejectReason::Unknown)
    return false;
  outReason = evidence.rejectReason;
  return true;
}

// ---------------------------------------------------------------------------
// 实际来源与 hitKey 口径（集中一处，避免 S/D 两点各写一份而漂移）。
// 参数用 skin::Source 的**序号**，避免本头依赖 skin 头。
// 上级 03:58：S/D 必须记录**该次候选 / 该次 draw 实际携带的按值来源**，
// 不得从最近一次 Served 推测；来源可合法切换，但必须记录本次实际值与实际消费对象。
// ---------------------------------------------------------------------------
enum class PaletteObjectSourceOrdinal : uint32_t {
  Unknown = 0u, CapturedWriter = 1u, CapturedRawArena = 2u, OwnedPartSnapshot = 3u,
  LegacyGlobalSlot = 4u, LegacySlotCache = 5u, PoseGroups = 6u, CModelGroups = 7u,
};
inline PaletteObjectSource MapPaletteObjectSource(uint32_t ordinal) noexcept {
  switch (ordinal) {
  case uint32_t(PaletteObjectSourceOrdinal::CapturedWriter):
    return PaletteObjectSource::DrawTimeCaptured;
  case uint32_t(PaletteObjectSourceOrdinal::CapturedRawArena):
  case uint32_t(PaletteObjectSourceOrdinal::LegacyGlobalSlot):
  case uint32_t(PaletteObjectSourceOrdinal::LegacySlotCache):
    return PaletteObjectSource::ArenaSlot;
  case uint32_t(PaletteObjectSourceOrdinal::OwnedPartSnapshot):
    return PaletteObjectSource::OwnedPartSnapshot;
  case uint32_t(PaletteObjectSourceOrdinal::PoseGroups):
  case uint32_t(PaletteObjectSourceOrdinal::CModelGroups):
    return PaletteObjectSource::PoseKernel;
  default:
    return PaletteObjectSource::Unknown;
  }
}
// hitKey 只用**当场已有**的既有摘要/序号，不为取证新增哈希或查询。
inline uint64_t PaletteObjectHitKey(PaletteObjectSource source, uint32_t slot,
                                    uint64_t captureSerial,
                                    uint64_t publicationTicket) noexcept {
  switch (source) {
  case PaletteObjectSource::ArenaSlot:
    return slot == 0xFFFFFFFFu ? 0u : uint64_t(slot);
  case PaletteObjectSource::DrawTimeCaptured:
    return captureSerial;
  case PaletteObjectSource::OwnedPartSnapshot:
    return publicationTicket;
  default:
    return 0u;
  }
}

// ---------------------------------------------------------------------------
// 三个采集点共用的对象键口径（集中一处，避免三点漂移）。
// ---------------------------------------------------------------------------
// deviceEpoch：**三个采集点一律记 0 + epochUnknown = true**。
//  * R 点当场不可得：shadow-core 内没有 device epoch 来源（只有 mapEpoch）。
//  * S/D 点当场可得，但记录器的键判等（PaletteObjectEvidence::SameKey）是**逐字段相等**，
//    没有"未知即通配"的语义；只在一侧填真实值会让 R→S→D 三个键永不相等，
//    四步链（拒绝 / 接住 / 入队 / 绘制）也就无法闭合。
//    既有先例同样在这个域记 0：d3d9_device.cpp 的 `skin-selection/v1` 事件
//    `event.key = {…, mapEpoch, 0u}`（该处 m_war3GpuSkinDeviceEpoch 其实可访问）。
//  ⇒ 本轮**不把 deviceEpoch 当可用身份域**：100% 记录带 epochUnknown = true，
//     设备 / Reset 隔离不由此域承担（相应风险与会话隔离是否重置，在报告"未闭合点"单列，
//     不得据此宣称"跨设备不会合并"）。
//
// lifecycleIdentity：三个采集点**都没有可靠的对象实例生命周期证明**
//  （模型不可变代际是**模型资源**代际，不是对象实例生命周期证明；Selection 只用于门控）
//  ⇒ 一律 0 + identityWeak = true。字段多 ≠ 强身份，不得用可用字段冒充强身份。
inline PaletteObjectKey MakePaletteObjectKey(
    void* renderablePart, void* runtimeModelPtr, uint32_t jHandle,
    uint32_t rawcode, uint64_t sessionGeneration, uint64_t mapEpoch) noexcept {
  PaletteObjectKey key{};
  key.renderablePart = uint64_t(reinterpret_cast<uintptr_t>(renderablePart));
  key.runtimeModelPtr = uint64_t(reinterpret_cast<uintptr_t>(runtimeModelPtr));
  key.jHandle = uint64_t(jHandle);
  key.rawcode = uint64_t(rawcode);
  key.sessionGeneration = sessionGeneration;
  key.mapEpoch = mapEpoch;
  key.deviceEpoch = 0u;
  key.lifecycleIdentity = 0u;
  key.identityWeak = true;
  key.epochUnknown = true;
  return key;
}

// 帧域**四项分列**：renderFrame / manifest(帧 + 发布 revision) / record / native。
// manifest 帧与发布 revision 在三个采集点当场都不可得（War3ShadowCasterDraw 与
// ShadowRenderableRecord 都不携带 manifest 发布 revision；record.frameSerial 在
// pose-only 路径会被改写成 pose 帧而 records 保留旧值，故不得冒充 manifest 帧）
// ⇒ 记 0 且 manifestUnknown = true。
// nativeFrameTag 由调用方给出**当场已读到**的值（R 点 = QueryCurrentPaletteFrameTag 的
// 既有局部结果；S 点 = 最终 Selection 的 frameTag；D 点 = caster 上的 Selection frameTag），
// 读不到 / 已被 native override 清空时 nativeKnown = false ⇒ 记 0 + nativeUnknown = true。
// 2026-09-18 阶段 C（K3 **生产侧接线**）：`attemptSerial` **按值携带**。
//
// 语义 = **一条清单记录所关联的那一次尝试**（对象的清单记录更新 ⇒ 新尝试），
// **不是**帧级计数（每帧清零已被裁定禁止，且帧内同对象可能被多次尝试）。
// 生产**四**个站点各自已经有这个值（= 它们本来就在传的 recordFrameSerial），因此接线
// **不引入任何新的数据流**：
//   Served    : draw.shadowRecordFrameSerial（唯一写点 d3d9_device.cpp:21527）
//   FirstSight: manifestFrame
//   Reject    : renderable.frameSerial
//   Drawn(D)  : draw.shadowRecordFrameSerial —— 但**必须把 0 归一为未知哨兵**：
//               该字段只有 d3d9_device.cpp:21527 一条生产者路径会写，其余 6+ 条 append 路径
//               （含 draw-time producer）保持默认 **0**，而 0 是记录器**明令禁止**的冒充值。
// 默认哨兵 ⇒ 既有调用方（测试）行为**逐位不变**（见 Case 30 的哨兵契约锁）。
//
// 2026-09-18 P0-5：**未知尝试哨兵**的命名常量 —— 调用点拿不到真实记录级序号时必须**显式**传它，
// 绝不用 0 冒充（0 会与「第一次尝试」混淆，MarkStage 会据此错误重置尝试基线）。
constexpr uint64_t kPaletteObjectUnknownAttempt = ~0ull;
inline PaletteObjectFrames MakePaletteObjectFrames(
    uint64_t renderFrame, uint64_t recordFrameSerial, uint64_t nativeFrameTag,
    bool nativeKnown, uint64_t attemptSerial = ~0ull) noexcept {
  PaletteObjectFrames frames{};
  frames.renderFrame = renderFrame;
  frames.manifestFrameSerial = 0u;
  frames.manifestPublishRevision = 0u;
  frames.recordFrameSerial = recordFrameSerial;
  frames.attemptSerial = attemptSerial;
  frames.nativeFrameTag = nativeKnown ? nativeFrameTag : 0u;
  frames.manifestUnknown = true;
  frames.nativeUnknown = !nativeKnown;
  return frames;
}

} // namespace dxvk::war3::tools::evidence
