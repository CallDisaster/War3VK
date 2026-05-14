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
//   - 在 draw-time（legacy 捕获同一位置）把必要资源按对象身份 stash 到
//     进程级 cache；
//   - 在 semantic `War3TryAppendSemanticShadowPacket` 消费时按同样的身份查表，
//     把 UV snapshot + diffuse view/sampler/descriptor/samplerIndex + 真
//     alphaRef 灌进 `War3ShadowPersistentGeometry` / `War3ShadowCasterDraw`。
//
// 仅在 AlphaTest lane 内使用。**不**掺入 pose/palette freshness 任何逻辑。

#include "../../../dxvk/dxvk_buffer.h"
#include "../../../dxvk/dxvk_descriptor.h"
#include "../../../dxvk/dxvk_image.h"
#include "../../../dxvk/dxvk_sampler.h"

#include <atomic>
#include <cstdint>

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

  /// 本 payload 写入时的 shadow persistent frame serial；用于 TTL 淘汰。
  uint64_t frameSerial = 0;

  /// @brief payload 是否包含完整 alpha-test 必需资源。
  bool valid() const {
    return alphaTestEnabled && uvFormat != VK_FORMAT_UNDEFINED &&
           uvStride != 0 && uvStorage != nullptr &&
           uvInfo.buffer != VK_NULL_HANDLE && uvInfo.size != 0 &&
           diffuseTexture != nullptr;
  }
};

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
};

extern War3ShadowAlphaTestPayloadCounters g_war3ShadowAlphaTestPayloadCounters;

/// @brief 读取全局计数器快照（供 bridge 调用）。
War3ShadowAlphaTestPayloadCountersSnapshot
ReadWar3ShadowAlphaTestPayloadCountersSnapshot();

} // namespace shadow
} // namespace war3
} // namespace dxvk
