#pragma once

// AlphaTest 专用：semantic/canonical shadow path 捕获漫反射纹理 + UV + alphaRef 的 payload。
//
// 背景：
//   - 旧的 legacy `War3TryCaptureShadowCaster` 在 draw-time 捕获
//     `D3DRS_ALPHATESTENABLE / D3DRS_ALPHAREF / TEXCOORD0 / textures[0]`，
//     填入 `War3ShadowCasterDraw` 的 `uv*` + `diffuseTexture*` 字段，
//     shadow caster shader 才能按 alphaRef 做像素 discard。
//   - semantic/canonical path 只通过 `ShadowMaterialSignature` 做材质分类
//     （Opaque/Cutout/AlphaBlend + 固定 0.5 alphaRef），并没有把 UV/diffuse
//     实际送到 replay 端，因此 cutout caster 在 semantic path 上要么被
//     `War3SemanticRejectUnsafeAlphaCasterRuntime` 整体踢掉，要么以全
//     opaque 方形卡片投射到 shadow map。
//
// 本 payload 负责补齐“materialClassification -> actual replay binding”的断层：
//   - 在 Stage11 semantic early-return 之前按完整 draw identity 捕获必要元数据；
//   - 只把 UV snapshot + diffuse view/sampler/descriptor/samplerIndex + 真
//     alphaRef 放入 exact-frame 三槽 metadata store；
//   - semantic `War3TryAppendSemanticShadowPacket` 仅在同 frame / identity /
//     material / texture generation 全部一致时应用它。
//
// 该通道不保存 position/index，不产生 draw，也没有跨帧 replay 能力。
//
// 仅在 AlphaTest lane 内使用。**不**掺入 pose/palette freshness 任何逻辑。

#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_descriptor.h"
#include "../../../dxvk/dxvk_image.h"
#include "../../../dxvk/dxvk_sampler.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dxvk {
namespace war3 {
namespace shadow {

/// @brief semantic shadow path alpha-test caster 所需的"活体"绑定 payload。
///
/// 字段与 legacy `War3ShadowCasterDraw::{alphaTestEnabled, alphaRef,
/// uvStride, uvOffset, uvFormat, uvStorage, uvInfo, diffuseTexture,
/// diffuseSampler, textureDescriptor, diffuseSamplerIndex}` 一一对应，
/// 确保在 semantic path 的 replay 端直接复用 legacy 已经验证过的 shader
/// 接线。
///
/// @note UV 数据必须在 stash 时快照到持久 buffer，避免 D3D9 VB 在 semantic
///       scene build 时已经 invalidate。
/// @note `Rc<...>` 保活 image view / sampler，避免上层纹理切换后出现悬空。
struct War3ShadowAlphaTestPayload {
  /// 是否启用 alpha-test（==1 时 shader p_flags bit2 会亮）。
  bool alphaTestEnabled = false;
  /// 从 D3DRS_ALPHAREF 取整后换算的 [0,1] 阈值；不再用硬编码 0.5。
  float alphaRef = 0.5f;

  /// UV 属性绑定格式（VK_FORMAT_R32G32_SFLOAT 等）。
  VkFormat uvFormat = VK_FORMAT_UNDEFINED;
  /// UV stride（通常 = D3D9 VB 完整顶点 stride）。
  uint32_t uvStride = 0;
  /// UV offset within vertex。
  uint32_t uvOffset = 0;
  /// 0 = 与 position 共享 binding 0；1 = 与 blend 共享 binding 1；
  /// 其他值由上层自行处理。
  uint32_t uvBinding = 0;

  /// UV VB 快照（device-local buffer）+ 切片元信息。
  Rc<DxvkBuffer> uvStorage;
  DxvkResourceBufferInfo uvInfo = {};

  /// 漫反射 sampled-image view（Stage0）。
  Rc<DxvkImageView> diffuseTexture;
  /// 回退 sampler（通常来自 shadow receiver pass fallback）。
  Rc<DxvkSampler> diffuseSampler;
  /// 完整 descriptor（view + sampler），供 bindless 路径直接 ctx->bindResources。
  DxvkDescriptor textureDescriptor = {};
  /// Bindless sampler slot。
  uint32_t diffuseSamplerIndex = 0;

  /// 本 payload 写入时的 shadow persistent frame serial；必须 exact-match。
  uint64_t frameSerial = 0;

  /// Metadata upload page generation. The safe metadata store accepts a UV
  /// slice only while this generation matches the current frame slot.
  uint64_t uvPageGeneration = 0;

  // Authoritative producer identity used only for tombstone retirement and
  // material-key verification. These fields do not participate in rendering.
  void* renderablePart = nullptr;
  void* sceneNode = nullptr;
  void* worldObjectEntry = nullptr;
  int16_t producerStage = -1;

  /// @brief payload 是否包含完整 alpha-test 必需资源。
  bool valid() const {
    return alphaTestEnabled && uvFormat != VK_FORMAT_UNDEFINED &&
           uvStride != 0 && uvStorage != nullptr &&
           uvInfo.buffer != VK_NULL_HANDLE && uvInfo.size != 0 &&
           diffuseTexture != nullptr;
  }
};

/// A complete draw identity for the metadata-only shadow channel.
///
/// This deliberately does not contain position/index storage and cannot be
/// converted into a caster. Texture identity and generation are part of the
/// key so a recycled D3D9 texture/view cannot inherit an older alpha payload.
struct War3ShadowDrawMetadataKey {
  void* instanceIdentity = nullptr;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  void* meshPayloadPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t layerIndex = 0u;
  int16_t producerStage = -1;
  uint32_t payloadWord108 = 0u;
  uint32_t payloadWord11C = 0u;
  uint64_t materialSignatureHash = 0u;
  void* textureIdentity = nullptr;
  uint64_t textureGeneration = 0u;

  bool operator==(const War3ShadowDrawMetadataKey& other) const noexcept;
  uint64_t stableHash() const noexcept;
};

/// Consumer-side key. Texture identity is intentionally not guessed by the
/// semantic consumer; lookup first matches this full logical slice, then
/// validates the stored texture identity/generation against its live Rc view.
struct War3ShadowDrawMetadataQuery {
  void* instanceIdentity = nullptr;
  void* sceneNode = nullptr;
  void* renderablePart = nullptr;
  void* meshPayloadPtr = nullptr;
  void* worldObjectEntry = nullptr;
  void* unitPtr = nullptr;
  uint32_t jHandle = 0u;
  uint32_t layerIndex = 0u;
  int16_t producerStage = -1;
  uint32_t payloadWord108 = 0u;
  uint32_t payloadWord11C = 0u;
  uint64_t materialSignatureHash = 0u;
};

enum class War3ShadowMetadataBlockerReason : uint8_t {
  None = 0u,
  KnownRawcode = 1u,
  WidgetIdentity = 2u,
  SmallFlat = 3u,
  BelowGround = 4u,
  Unreadable = 5u,
};

struct War3ShadowDrawMetadata {
  War3ShadowDrawMetadataKey key = {};
  War3ShadowAlphaTestPayload alpha = {};
  War3ShadowMetadataBlockerReason blockerReason =
      War3ShadowMetadataBlockerReason::None;
  uint64_t frameSerial = 0u;
  uint64_t uvPageGeneration = 0u;
  bool alphaPayloadComplete = false;

  bool hasBlocker() const {
    return blockerReason != War3ShadowMetadataBlockerReason::None &&
           blockerReason != War3ShadowMetadataBlockerReason::Unreadable;
  }
};

/// Three-slot, exact-frame metadata store. It owns only compact identity,
/// material/alpha state and an optional independently uploaded UV slice. It
/// never owns position/index data and exposes no draw/replay API.
class War3ShadowDrawMetadataFrameStore {
public:
  void publish(War3ShadowDrawMetadata metadata);
  bool lookupAlpha(const War3ShadowDrawMetadataQuery& query,
                   uint64_t frameSerial,
                   War3ShadowDrawMetadata& out,
                   bool noteRejectedLookup = true) const;
  bool lookupBlocker(const War3ShadowDrawMetadataQuery& query,
                     uint64_t frameSerial,
                     War3ShadowMetadataBlockerReason& outReason,
                     uint64_t* outKeyHash = nullptr) const;
  uint64_t retire(void* renderablePart, void* sceneNode,
                  void* worldObjectEntry, int16_t producerStage);
  void clear();
  uint64_t size() const;

private:
  struct FrameSlot {
    uint64_t frameSerial = 0u;
    uint64_t pageGeneration = 0u;
    std::vector<War3ShadowDrawMetadata> records;
  };

  mutable std::mutex m_mutex;
  std::array<FrameSlot, 3u> m_slots = {};
};

War3ShadowDrawMetadataFrameStore& War3ShadowDrawMetadataStore();
uint64_t War3ShadowTextureGeneration(const Rc<DxvkImageView>& view);

/// @brief AlphaTest payload plumbing 诊断计数器集合。
///
/// 目标：让 AutoTest / control plane 可以观察 alpha-test 路径是否真的
/// 被走通，而不是与 pose/palette 指标混线。
struct War3ShadowAlphaTestPayloadCounters {
  // ---- 消费端（semantic packet append 时查表） ----
  /// semantic cutout/alphablend packet 触发过 payload 查找的次数。
  std::atomic<uint64_t> attemptCount{0u};
  /// 查找命中且 payload valid 的次数。
  std::atomic<uint64_t> hitCount{0u};
  /// 命中但缺 UV snapshot 的次数。
  std::atomic<uint64_t> missNoUvCount{0u};
  /// 命中但缺 diffuse view 的次数。
  std::atomic<uint64_t> missNoDiffuseCount{0u};
  /// 查找时 identity key 无法构造（renderablePart/sceneNode 都为空）的次数。
  std::atomic<uint64_t> missStageInvalidCount{0u};
  /// semantic path 真正把 payload 应用到 draw/geometry 的次数。
  std::atomic<uint64_t> appliedCount{0u};
  /// payload 缺失 → 回落到"safety reject"（避免方形卡片）的次数。
  std::atomic<uint64_t> fallbackRejectCount{0u};

  // ---- 生产端（draw-time stash） ----
  /// 在 draw-time 成功 stash 一个 payload 的次数。
  std::atomic<uint64_t> stashCapturedCount{0u};
  /// draw-time 因 semantic key 不可用跳过 stash。
  std::atomic<uint64_t> stashSkipNoSemanticKeyCount{0u};
  /// draw-time 因没有 TEXCOORD0 或 VB 无效跳过 stash。
  std::atomic<uint64_t> stashSkipNoUvCount{0u};
  /// draw-time 因没有 Stage0 texture 跳过 stash。
  std::atomic<uint64_t> stashSkipNoDiffuseCount{0u};
  /// draw-time 因 freeze 上传失败跳过 stash。
  std::atomic<uint64_t> stashSkipNoUploadCount{0u};
  /// cache 按 TTL 淘汰的 entry 数（诊断用）。
  std::atomic<uint64_t> cacheEvictedCount{0u};
  /// 当前 cache 规模（最近一次读取后同步写入）。
  std::atomic<uint64_t> cacheSizeGauge{0u};

  // ---- metadata-only channel ----
  std::atomic<uint64_t> metadataClassifiedCount{0u};
  std::atomic<uint64_t> metadataCapturedCount{0u};
  std::atomic<uint64_t> metadataAppliedCount{0u};
  std::atomic<uint64_t> metadataRejectedFrameCount{0u};
  std::atomic<uint64_t> metadataRejectedGenerationCount{0u};
  std::atomic<uint64_t> metadataAmbiguousCount{0u};
  std::atomic<uint64_t> metadataRejectedNoMaterialCount{0u};
  std::atomic<uint64_t> metadataRejectedOpaqueCount{0u};
  std::atomic<uint64_t> metadataRejectedNoUvCount{0u};
  std::atomic<uint64_t> metadataRejectedNoDiffuseCount{0u};
  std::atomic<uint64_t> metadataRejectedUploadCount{0u};
  std::atomic<uint64_t> metadataRejectedDuplicateCount{0u};
  std::atomic<uint64_t> blockerKnownRawcodeCount{0u};
  std::atomic<uint64_t> blockerWidgetIdentityCount{0u};
  std::atomic<uint64_t> blockerSmallFlatCount{0u};
  std::atomic<uint64_t> blockerBelowGroundCount{0u};
  std::atomic<uint64_t> blockerUnreadableCount{0u};
  std::atomic<uint64_t> blockerFinalLeakCount{0u};
};

/// @brief 对外快照结构：用于 bridge summary / JSON 暴露。
struct War3ShadowAlphaTestPayloadCountersSnapshot {
  uint64_t attemptCount = 0;
  uint64_t hitCount = 0;
  uint64_t missNoUvCount = 0;
  uint64_t missNoDiffuseCount = 0;
  uint64_t missStageInvalidCount = 0;
  uint64_t appliedCount = 0;
  uint64_t fallbackRejectCount = 0;
  uint64_t stashCapturedCount = 0;
  uint64_t stashSkipNoSemanticKeyCount = 0;
  uint64_t stashSkipNoUvCount = 0;
  uint64_t stashSkipNoDiffuseCount = 0;
  uint64_t stashSkipNoUploadCount = 0;
  uint64_t stashSkipNoUploadCountBudget = 0;
  uint64_t cacheEvictedCount = 0;
  uint64_t cacheSizeGauge = 0;
  uint64_t metadataClassifiedCount = 0;
  uint64_t metadataCapturedCount = 0;
  uint64_t metadataAppliedCount = 0;
  uint64_t metadataRejectedFrameCount = 0;
  uint64_t metadataRejectedGenerationCount = 0;
  uint64_t metadataAmbiguousCount = 0;
  uint64_t metadataRejectedNoMaterialCount = 0;
  uint64_t metadataRejectedOpaqueCount = 0;
  uint64_t metadataRejectedNoUvCount = 0;
  uint64_t metadataRejectedNoDiffuseCount = 0;
  uint64_t metadataRejectedUploadCount = 0;
  uint64_t metadataRejectedDuplicateCount = 0;
  uint64_t blockerKnownRawcodeCount = 0;
  uint64_t blockerWidgetIdentityCount = 0;
  uint64_t blockerSmallFlatCount = 0;
  uint64_t blockerBelowGroundCount = 0;
  uint64_t blockerUnreadableCount = 0;
  uint64_t blockerFinalLeakCount = 0;
};

extern War3ShadowAlphaTestPayloadCounters g_war3ShadowAlphaTestPayloadCounters;

/// @brief 读取全局计数器快照（供 bridge 调用）。
War3ShadowAlphaTestPayloadCountersSnapshot
ReadWar3ShadowAlphaTestPayloadCountersSnapshot();

} // namespace shadow
} // namespace war3
} // namespace dxvk
