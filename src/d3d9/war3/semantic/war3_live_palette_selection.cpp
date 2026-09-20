#include "war3_live_palette_selection.h"
#include "war3_device_semantic_predicates.h"
#include "../core/war3_game_structs.h"
#include "../core/war3_memory.h"
#include "../shadow/war3_shadow_renderer_core.h"
#include "../model/war3_model_hook.h"
#include "../model/war3_model_registry.h"
#include "../model/war3_model_resource_cache.h"
#include "../war3.h"
#include "../../d3d9_war3_scene.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <windows.h>

namespace dxvk::war3::semantic {

// ---------------------------------------------------------------------------
// 运行时配置 getter（live palette 选择链）
// ---------------------------------------------------------------------------
bool War3SemanticLivePaletteSafeCopyRuntime() {
  // Game.dll 的全局 palette 通常每帧只读一次，帧内 region cache 无法摊薄
  // VirtualQuery。ReadProcessMemory 同时完成范围验证与快照复制，避免
  // VirtualQuery 后再直接解引用所留下的 TOCTOU 窗口。保留运行时回退用于
  // 同一 DLL 的 A/B/B/A 验证。
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_SAFE_COPY", 1u) != 0u;
  return s_enabled;
}

bool War3SemanticLivePaletteRefreshRuntime() {
  // 每帧从 Hook_RuntimeMatrixWrite 捕获的混合调色板缓存读取完整骨骼矩阵
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH", 1u) != 0u;
  return s_enabled;
}

bool War3SemanticLivePaletteAllowCModelFallbackRuntime() {
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK",
                    0u) != 0u;
  return s_enabled;
}

bool War3SemanticPaletteDiagnosticsRuntime() {
  // These historical motion/churn probes do not participate in palette
  // selection, replay validation, or publication. Keep their large lookup
  // tables out of the release hot path unless a targeted capture requests
  // them explicitly.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS", 0u) != 0u;
  return s_enabled;
}

// ---------------------------------------------------------------------------
// 纯计算 helper（FNV-1a 哈希 / 48 字节 pose 解码 / pose 数组读取 / alias 解析）
// ---------------------------------------------------------------------------
uint64_t War3SemanticHashMatrixPalette(const Matrix4* matrices,
                                        uint32_t matrixCount) {
  uint64_t hash = bit::fnv1a_init();
  hash = bit::fnv1a_iter(hash, matrixCount);
  if (matrices == nullptr || matrixCount == 0u)
    return hash;

  for (uint32_t i = 0u; i < matrixCount; ++i) {
    const Matrix4& matrix = matrices[i];
    for (uint32_t r = 0u; r < 4u; ++r) {
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
      hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
    }
  }
  return hash;
}

Matrix4 War3DecodeRuntimePoseMatrix48(const uint8_t* poseBytes) {
  float pose3x4[12] = {};
  std::memcpy(pose3x4, poseBytes, sizeof(pose3x4));
  return Matrix4(Vector4(pose3x4[0], pose3x4[1], pose3x4[2], 0.0f),
                 Vector4(pose3x4[3], pose3x4[4], pose3x4[5], 0.0f),
                 Vector4(pose3x4[6], pose3x4[7], pose3x4[8], 0.0f),
                 Vector4(pose3x4[9], pose3x4[10], pose3x4[11], 1.0f));
}

bool War3TryReadRuntimePoseArray(void* runtimeModelPtr,
                                 uint32_t& outPoseCount,
                                 void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return false;

  if (!dxvk::war3::SafeReadU32Fast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixCount,
          outPoseCount) ||
      outPoseCount == 0u || outPoseCount > 1024u ||
      !dxvk::war3::SafeReadPtrFast(
          runtimeModelPtr, dxvk::war3::CModelOffsets::FinalPoseMatrixArray,
          outPoseArrayPtr) ||
      outPoseArrayPtr == nullptr ||
      !dxvk::war3::IsReadableRange(
          outPoseArrayPtr, size_t(outPoseCount) * sizeof(float) * 12u)) {
    outPoseCount = 0u;
    outPoseArrayPtr = nullptr;
    return false;
  }
  return true;
}

void* War3ResolveLivePoseRuntimeAlias(void* runtimeModelPtr,
                                      uint32_t& outPoseCount,
                                      void*& outPoseArrayPtr) {
  outPoseCount = 0u;
  outPoseArrayPtr = nullptr;
  if (runtimeModelPtr == nullptr)
    return nullptr;

  constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
  const uintptr_t value = reinterpret_cast<uintptr_t>(runtimeModelPtr);
  if (value < 0x10000u)
    return nullptr;

  std::array<void*, 3> candidates = {
      reinterpret_cast<void*>(value + kCModelComplexExtensionOffset),
      runtimeModelPtr,
      value > kCModelComplexExtensionOffset
          ? reinterpret_cast<void*>(value - kCModelComplexExtensionOffset)
          : nullptr};

  for (void* candidate : candidates) {
    uint32_t poseCount = 0u;
    void* poseArrayPtr = nullptr;
    if (!War3TryReadRuntimePoseArray(candidate, poseCount, poseArrayPtr))
      continue;
    outPoseCount = poseCount;
    outPoseArrayPtr = poseArrayPtr;
    return candidate;
  }

  return nullptr;
}


// ---------------------------------------------------------------------------
// M2-2：Gap A slot 缓存复核计数器（从 d3d9_device.cpp 逐字节迁入；M1 D 类
// 先例——模块内唯一定义 + 头文件 extern；device.cpp 的 war3_diag Query
// 访问器与 bridge 消费侧零改动）。
// ---------------------------------------------------------------------------
// 2026-09-16 P0 Gap A：device 侧 palette 记忆槽位复核计数，与 shadow-core
// Gap B 的 g_paletteSlotCacheServedAfterConfirm/RejectedStale 同风格。
// ServedAfterConfirm 证明"经 producer 复核后合法快路径仍命中"（未误杀）；
// RejectedStale 记录"producer miss / 槽位不一致 / groupCount 不兼容"导致的
// 记忆槽位拒绝——被拒对象必须落到 PoseFallback 等替代来源而非 SourceNone，
// 且不得形成永久负缓存（下帧 +0x08/producer 恢复后即可重新命中）。
// （M2-2 搬动痕迹：原注释尾行"声明位置提前到本匿名命名空间，供下方 war3_diag
// Query 访问器导出"描述的是 device.cpp 旧布局，迁出后不再成立，按 B 类先例
// 删除；两条计数器定义本身逐字节未变。）
std::atomic<uint64_t> g_devicePaletteSlotCacheServedAfterConfirmCount{0};
std::atomic<uint64_t> g_devicePaletteSlotCacheRejectedStaleCount{0};

// ---------------------------------------------------------------------------
// M2-2：调色板来源选择链本体（从 d3d9_device.cpp 逐字节迁入；函数体、
// 注释、函数内 static/thread_local 语义一字未改；默认实参集中在模块头
// 声明。含 resolvePaletteSlotIndex lambda 的 4096 项 thread_local slot
// 缓存 + 8192 项 lookup 加速器与 2026-09-16 P0 Gap A producer 复核）。
// ---------------------------------------------------------------------------
bool War3TryBuildLiveRuntimeGroupPalette(
    const dxvk::war3::shadow::ShadowPacketResource& resource,
    void* runtimeModelPtr,
    void* renderablePart,
    uint64_t frameSerial,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    uint64_t& outHash,
    uint64_t* outRawPoseHash,
    void** outPoseRuntimeModelPtr,
    bool allowCModelFallbackForCall,
    War3SemanticPaletteSource* outPaletteSource,
    uint32_t* outPaletteSlotIndex,
    uint32_t* outPaletteMinFrameTag,
    uint32_t* outPaletteMaxFrameTag,
    War3LivePaletteBuildTiming* outTiming,
    uint32_t provenMaxVertexGroupSlot,
    dxvk::war3::render::skin::Selection* outSelection) {
  War3LivePaletteBuildRawTiming buildTiming(outTiming);
  if (outSelection != nullptr)
    *outSelection = {};
  auto markSource = [&](War3SemanticPaletteSource source) {
    if (outPaletteSource != nullptr)
      *outPaletteSource = source;
  };
  markSource(War3SemanticPaletteSource::None);
  outPalette.clear();
  outMaxVertexGroupSlot = 0u;
  outHash = 0u;
  if (outRawPoseHash != nullptr)
    *outRawPoseHash = 0u;
  if (outPoseRuntimeModelPtr != nullptr)
    *outPoseRuntimeModelPtr = nullptr;
  if (outPaletteSlotIndex != nullptr)
    *outPaletteSlotIndex = 0xFFFFFFFFu;
  if (outPaletteMinFrameTag != nullptr)
    *outPaletteMinFrameTag = 0u;
  if (outPaletteMaxFrameTag != nullptr)
    *outPaletteMaxFrameTag = 0u;
  if (runtimeModelPtr == nullptr && renderablePart == nullptr)
    return false;
  const auto& vertexGroups = resource.vertexGroupIndexVec();
  const auto& matrixGroupSizes = resource.matrixGroupSizeVec();
  const auto& matrixIndices = resource.matrixIndexVec();
  if (vertexGroups.empty())
    return false;

  if (provenMaxVertexGroupSlot < 256u) {
    // The caller may reuse the immutable group-domain maximum that was sealed
    // with this packet. Palette bytes can change every frame, but the vertex
    // group stream and its maximum do not. Unknown callers retain the exact
    // byte scan below.
    outMaxVertexGroupSlot = provenMaxVertexGroupSlot;
  } else {
    buildTiming.enter(War3LivePaletteBuildPhase::GroupScan);
    for (const uint8_t groupSlot : vertexGroups)
      outMaxVertexGroupSlot =
          std::max(outMaxVertexGroupSlot, uint32_t(groupSlot));
  }
  const uint32_t requiredPaletteCount = outMaxVertexGroupSlot + 1u;
  if (requiredPaletteCount == 0u || requiredPaletteCount > 256u)
    return false;

  if (dxvk::war3::render::skin::ContractEnabled()) {
    // Never reinterpret a remembered slot as ownership of today's arena bytes.
    // A miss terminates this attempt; no legacy/raw/Pose fallback is admissible.
    buildTiming.enter(War3LivePaletteBuildPhase::PartSnapshot);
    dxvk::war3::render::skin::Selection selected;
    if (!dxvk::war3::model::QueryOwnedRenderablePartPaletteSnapshot(
            runtimeModelPtr, renderablePart, requiredPaletteCount,
            &outPalette, selected))
      return false;
    outHash = selected.hash;
    if (outRawPoseHash) *outRawPoseHash = selected.hash;
    if (outPoseRuntimeModelPtr) *outPoseRuntimeModelPtr = runtimeModelPtr;
    if (outPaletteSlotIndex) *outPaletteSlotIndex = selected.slot;
    if (outPaletteMinFrameTag) *outPaletteMinFrameTag = selected.frameTag;
    if (outPaletteMaxFrameTag) *outPaletteMaxFrameTag = selected.frameTag;
    if (outSelection) *outSelection = selected;
    markSource(War3SemanticPaletteSource::OwnedPartSnapshot);
    return true;
  }

  auto resolvePaletteSlotIndex = [&](void* partPtr) -> uint32_t {
    if (partPtr == nullptr)
      return 0xFFFFFFFFu;

    struct PaletteSlotCacheEntry {
      void* renderablePart = nullptr;
      uint64_t mapEpoch = 0u;
      uint32_t paletteSlotIndex = 0xFFFFFFFFu;
      // 2026-09-16 P0 Gap A：producer 绑定查询一并取回的 groupCount/frameTag，
      // 仅在 producer 复核通过时刷新；供出记忆槽位前用于兼容性复核。
      uint32_t paletteGroupCount = 0u;
      uint32_t paletteFrameTag = 0u;
      uint64_t lastSeenFrameSerial = 0u;
    };

    struct PaletteSlotCacheLookupEntry {
      void* renderablePart = nullptr;
      uint64_t mapEpoch = 0u;
      uint32_t cacheIndex = 0xFFFFFFFFu;
    };

    static constexpr size_t kMaxPaletteSlotCacheEntries = 4096u;
    static constexpr size_t kPaletteSlotCacheLookupEntries = 8192u;
    uint32_t currentSlotIndex = 0xFFFFFFFFu;
    dxvk::war3::SafeReadU32Fast(
        partPtr, dxvk::war3::RenderablePartFieldOffsets::StagePresetSpanBaseIndex,
        currentSlotIndex);

    // 2026-09-16 P0 Gap A：除槽位外同时取回 producer 记录的
    // groupCount/frameTag（war3_model_hook.h:499 签名本就支持），
    // 供 useCachedEntry 复核记忆槽位时做兼容性判断与 entry 刷新。
    auto queryProducerBindingSlot = [&](uint32_t* outGroupCount,
                                        uint32_t* outFrameTag) -> uint32_t {
      uint32_t boundSlotIndex = 0xFFFFFFFFu;
      uint32_t boundGroupCount = 0u;
      uint32_t boundFrameTag = 0u;
      if (dxvk::war3::model::QueryRenderablePartPaletteSlot(
              partPtr, boundSlotIndex, &boundGroupCount, &boundFrameTag) &&
          boundSlotIndex != 0xFFFFFFFFu && boundSlotIndex < 0x3A98u) {
        if (outGroupCount != nullptr)
          *outGroupCount = boundGroupCount;
        if (outFrameTag != nullptr)
          *outFrameTag = boundFrameTag;
        return boundSlotIndex;
      }
      return 0xFFFFFFFFu;
    };

    static thread_local std::array<PaletteSlotCacheEntry,
                                   kMaxPaletteSlotCacheEntries>
        s_paletteSlotCache = {};
    // The authoritative cache historically performed a 4096-entry linear
    // walk for every skinned caster. Keep that walk as the exact collision
    // fallback, but remember the most recent index in a fixed direct-mapped
    // accelerator. A collision can only cost a fallback scan; identity and
    // epoch are revalidated against the original entry before use.
    static thread_local std::array<PaletteSlotCacheLookupEntry,
                                   kPaletteSlotCacheLookupEntries>
        s_paletteSlotCacheLookup = {};
    static thread_local size_t s_paletteSlotCacheCursor = 0u;
    const uint64_t mapEpoch =
        dxvk::war3::model::ShadowModelResourceCache::instance().mapEpoch();

    auto useCachedEntry = [&](PaletteSlotCacheEntry& entry) -> uint32_t {
      entry.lastSeenFrameSerial = frameSerial;
      if (currentSlotIndex != 0xFFFFFFFFu && currentSlotIndex < 0x3A98u) {
        entry.paletteSlotIndex = currentSlotIndex;
        return currentSlotIndex;
      }
      // 2026-09-16 P0 Gap A：+0x08 本帧未更新时，绝不把记忆槽位直接当作
      // 今天 arena 字节的所有权供出（路由式修复，不是只改成返回失败）。
      // 必须由 producer 绑定表确认同一 part 仍绑定同一槽位、且 producer
      // 记录的 groupCount 覆盖本次所需矩阵数，才允许供出记忆槽位并刷新
      // entry；任一不满足即返回 0xFFFFFFFFu，调用方随即跳过下方按 slot 键
      // 的 Game.dll 全局 arena / QueryBlendedPaletteBySlotIndex 读者，落到
      // PoseFallback（PoseRegistry 已发布姿态 + producer-owner 反查）这条
      // 合法对象的替代路径。拒绝不写入负缓存，下帧绑定恢复后即可重新命中。
      uint32_t producerGroupCount = 0u;
      uint32_t producerFrameTag = 0u;
      const uint32_t producerSlotIndex =
          queryProducerBindingSlot(&producerGroupCount, &producerFrameTag);
      if (producerSlotIndex != 0xFFFFFFFFu &&
          producerSlotIndex == entry.paletteSlotIndex &&
          dxvk::war3::render::skin::ProducerGroupCountCovers(
              requiredPaletteCount, producerGroupCount)) {
        entry.paletteGroupCount = producerGroupCount;
        entry.paletteFrameTag = producerFrameTag;
        g_devicePaletteSlotCacheServedAfterConfirmCount.fetch_add(
            1u, std::memory_order_relaxed);
        return entry.paletteSlotIndex;
      }
      g_devicePaletteSlotCacheRejectedStaleCount.fetch_add(
          1u, std::memory_order_relaxed);
      return 0xFFFFFFFFu;
    };

    const uintptr_t partValue = reinterpret_cast<uintptr_t>(partPtr);
    const size_t lookupSlot =
        ((partValue >> 4u) ^ size_t(mapEpoch)) &
        (kPaletteSlotCacheLookupEntries - 1u);
    auto& lookup = s_paletteSlotCacheLookup[lookupSlot];
    if (lookup.renderablePart == partPtr && lookup.mapEpoch == mapEpoch &&
        lookup.cacheIndex < s_paletteSlotCache.size()) {
      auto& cached = s_paletteSlotCache[lookup.cacheIndex];
      if (cached.renderablePart == partPtr && cached.mapEpoch == mapEpoch)
        return useCachedEntry(cached);
    }

    for (size_t cacheIndex = 0u; cacheIndex < s_paletteSlotCache.size();
         ++cacheIndex) {
      auto& entry = s_paletteSlotCache[cacheIndex];
      if (entry.renderablePart != partPtr || entry.mapEpoch != mapEpoch)
        continue;
      lookup = {partPtr, mapEpoch, uint32_t(cacheIndex)};
      return useCachedEntry(entry);
    }

    uint32_t producerGroupCount = 0u;
    uint32_t producerFrameTag = 0u;
    if (currentSlotIndex == 0xFFFFFFFFu || currentSlotIndex >= 0x3A98u) {
      const uint32_t producerSlotIndex =
          queryProducerBindingSlot(&producerGroupCount, &producerFrameTag);
      if (producerSlotIndex == 0xFFFFFFFFu)
        return currentSlotIndex;
      // 2026-09-18 独立复审 R1：冷缓存首次查询必须与缓存命中路径（见上方
      // useCachedEntry）适用**同一条**数量规则——producer 记录的 groupCount
      // 必须覆盖本次所需矩阵数，否则不得供出该槽位。此前本分支在取到 producer
      // slot 后直接采用并落缓存，导致同一对象、同一数量条件在冷缓存下被接受、
      // 热缓存下被拒绝。拒绝时**不写负缓存/不落条目**（本分支尚未落条目）。
      //
      // 2026-09-18 等价门禁实测（**已定论**）：该拒绝会改变路由 —— 冷缓存被拒后
      // 调用方落到后续合法替代路径（part snapshot / published pose 等），因此
      // 与 pre-M2 legacy 参考**行为不同**（差分：module slotIndex=8 vs legacy=0xFFFFFFFF）。
      // 这是**有意的行为修复**，按复审原则「修错误时证明新行为符合预期，不要求与
      // 旧错误等价」，应更新等价门禁的期望，而**不是**回退本修复。
      if (!dxvk::war3::render::skin::ProducerGroupCountCovers(
              requiredPaletteCount, producerGroupCount)) {
        g_devicePaletteSlotCacheRejectedStaleCount.fetch_add(
            1u, std::memory_order_relaxed);
        return 0xFFFFFFFFu;
      }
      currentSlotIndex = producerSlotIndex;
    }

    const size_t cacheIndex =
        s_paletteSlotCacheCursor++ % kMaxPaletteSlotCacheEntries;
    auto& entry = s_paletteSlotCache[cacheIndex];
    entry.renderablePart = partPtr;
    entry.mapEpoch = mapEpoch;
    entry.paletteSlotIndex = currentSlotIndex;
    entry.paletteGroupCount = producerGroupCount;
    entry.paletteFrameTag = producerFrameTag;
    entry.lastSeenFrameSerial = frameSerial;
    lookup = {partPtr, mapEpoch, uint32_t(cacheIndex)};
    return currentSlotIndex;
  };

  // 首选：从 Hook_RuntimeMatrixWrite 当场捕获的混合调色板读取
  if (renderablePart != nullptr) {
    buildTiming.enter(War3LivePaletteBuildPhase::SlotResolve);
    const uint32_t slotIndex = resolvePaletteSlotIndex(renderablePart);
    if (outPaletteSlotIndex != nullptr && slotIndex != 0xFFFFFFFFu)
      *outPaletteSlotIndex = slotIndex;
    if (slotIndex != 0xFFFFFFFFu && slotIndex < 0x3A98u) {
      uint32_t slotMinFrameTag = 0u;
      uint32_t slotMaxFrameTag = 0u;
      uint32_t slotMissingCount = 0u;
      bool slotFrameTagQueried = false;
      bool slotFrameTagReady = false;
      auto ensureSlotFrameTags = [&]() {
        if (slotFrameTagQueried)
          return;
        slotFrameTagQueried = true;
        buildTiming.enter(War3LivePaletteBuildPhase::FrameTagQuery);
        slotFrameTagReady =
            dxvk::war3::model::QueryBlendedPaletteFrameTagRange(
                slotIndex, requiredPaletteCount, slotMinFrameTag,
                slotMaxFrameTag, slotMissingCount);
      };
      auto publishSlotFrameTags = [&]() {
        ensureSlotFrameTags();
        if (!slotFrameTagReady)
          return;
        if (outPaletteMinFrameTag != nullptr)
          *outPaletteMinFrameTag = slotMinFrameTag;
        if (outPaletteMaxFrameTag != nullptr)
          *outPaletteMaxFrameTag = slotMaxFrameTag;
      };
      // Phase 7.46：优先消费 0x12FED0/0x12FF90 producer hook 按
      // renderablePart 记录的完整 palette snapshot。相比重新按 slot 读
      // Game.dll 全局 palette，这条路径和主模型 renderablePart 的 writer
      // 绑定更紧，能规避 slot 复用/相位差造成的 stale bytes。
      uint64_t producerPartPaletteHash = 0u;
      uint32_t producerPartFrameTag = 0u;
      buildTiming.enter(War3LivePaletteBuildPhase::PartSnapshot);
      if (dxvk::war3::model::QueryRenderablePartPaletteSnapshot(
              renderablePart, requiredPaletteCount, &outPalette,
              &producerPartPaletteHash, &producerPartFrameTag) &&
          !outPalette.empty()) {
        outHash = producerPartPaletteHash != 0u
                      ? producerPartPaletteHash
                      : War3SemanticHashMatrixPalette(
                            outPalette.data(), uint32_t(outPalette.size()));
        if (outRawPoseHash != nullptr)
          *outRawPoseHash = outHash;
        if (outPoseRuntimeModelPtr != nullptr)
          *outPoseRuntimeModelPtr = runtimeModelPtr;
        if (producerPartFrameTag != 0u) {
          if (outPaletteMinFrameTag != nullptr)
            *outPaletteMinFrameTag = producerPartFrameTag;
          if (outPaletteMaxFrameTag != nullptr)
            *outPaletteMaxFrameTag = producerPartFrameTag;
        } else {
          publishSlotFrameTags();
        }
        markSource(War3SemanticPaletteSource::SubmitTimeBlendedPaletteCache);
        return true;
      }
      // 与 2026-05-03 的动态阴影修复路线保持一致：优先直接读取引擎的
      // group-blended 调色板缓冲区，而不是继续依赖较晚阶段的 pose 重建。
      buildTiming.enter(War3LivePaletteBuildPhase::GlobalModuleLookup);
      uintptr_t gameDllBase = dxvk::war3::GetGameDllBase();
      if (gameDllBase == 0u) {
        gameDllBase =
            reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
      }
      if (gameDllBase != 0u) {
        void* globalPaletteBufferPtr = nullptr;
        buildTiming.enter(War3LivePaletteBuildPhase::GlobalPointerRead);
        if (dxvk::war3::SafeReadPtrFast(
                reinterpret_cast<const void*>(gameDllBase + 0xBC6BD0u), 0u,
                globalPaletteBufferPtr) &&
            globalPaletteBufferPtr != nullptr) {
          const auto* enginePalette = reinterpret_cast<const uint8_t*>(
              reinterpret_cast<uintptr_t>(globalPaletteBufferPtr) +
              size_t(slotIndex) * 48u);
          const size_t requiredBytes = size_t(requiredPaletteCount) * 48u;
          const uint8_t* decodePalette = enginePalette;
          bool paletteReadable = false;
          if (War3SemanticLivePaletteSafeCopyRuntime()) {
            static thread_local std::array<uint8_t, 256u * 48u>
                globalPaletteSnapshot;
            buildTiming.enter(War3LivePaletteBuildPhase::GlobalSafeCopy);
            paletteReadable = dxvk::war3::SafeCopy(
                globalPaletteSnapshot.data(), enginePalette, requiredBytes);
            decodePalette = globalPaletteSnapshot.data();
          } else {
            buildTiming.enter(War3LivePaletteBuildPhase::GlobalRangeCheck);
            paletteReadable =
                dxvk::war3::IsReadableRange(enginePalette, requiredBytes);
          }
          if (paletteReadable) {
            buildTiming.enter(War3LivePaletteBuildPhase::GlobalDecode);
            outPalette.resize(requiredPaletteCount);
            for (uint32_t i = 0u; i < requiredPaletteCount; ++i) {
              outPalette[i] = War3DecodeRuntimePoseMatrix48(
                  decodePalette + size_t(i) * 48u);
            }
            buildTiming.enter(War3LivePaletteBuildPhase::GlobalHash);
            outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                                    requiredPaletteCount);
            if (outRawPoseHash != nullptr)
              *outRawPoseHash = outHash;
            if (outPoseRuntimeModelPtr != nullptr)
              *outPoseRuntimeModelPtr = runtimeModelPtr;
            publishSlotFrameTags();
            markSource(War3SemanticPaletteSource::SubmitTimeGlobalSlot);
            return true;
          }
        }
      }

      uint32_t capturedCount = 0u;
      buildTiming.enter(War3LivePaletteBuildPhase::BlendedSlot);
      if (dxvk::war3::model::QueryBlendedPaletteBySlotIndex(
              slotIndex, &outPalette, capturedCount) &&
          capturedCount >= requiredPaletteCount && capturedCount <= 256u &&
          outPalette.size() >= requiredPaletteCount) {
        if (outPalette.size() > requiredPaletteCount)
          outPalette.resize(requiredPaletteCount);
        outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                                requiredPaletteCount);
        if (outRawPoseHash) *outRawPoseHash = outHash;
        if (outPoseRuntimeModelPtr) *outPoseRuntimeModelPtr = runtimeModelPtr;
        publishSlotFrameTags();
        markSource(War3SemanticPaletteSource::SubmitTimeBlendedPaletteCache);
        return true;
      }
    }
  }

  buildTiming.enter(War3LivePaletteBuildPhase::PoseFallback);
  uint32_t poseCount = 0u;
  void* poseArrayPtr = nullptr;
  void* poseRuntimeModelPtr = nullptr;
  const Matrix4* publishedPoseMatrices = nullptr;
  uint64_t publishedPoseHash = 0u;
  dxvk::war3::model::PoseRecord publishedPose = {};
  auto tryUsePublishedPose = [&](void* candidateRuntimeModelPtr) -> bool {
    if (candidateRuntimeModelPtr == nullptr)
      return false;
    dxvk::war3::model::PoseRecord candidate = {};
    if (!dxvk::war3::model::PoseRegistry::instance().findByRuntimeModel(
            candidateRuntimeModelPtr, candidate) ||
        candidate.matrixCount == 0u || candidate.matrixPalette.empty()) {
      return false;
    }

    publishedPose = std::move(candidate);
    poseRuntimeModelPtr = publishedPose.runtimeModelPtr != nullptr
                              ? publishedPose.runtimeModelPtr
                              : candidateRuntimeModelPtr;
    poseCount = std::min<uint32_t>(
        publishedPose.matrixCount,
        uint32_t(std::min<size_t>(publishedPose.matrixPalette.size(),
                                  size_t(1024u))));
    if (poseCount == 0u)
      return false;
    publishedPoseMatrices = publishedPose.matrixPalette.data();
    publishedPoseHash =
        publishedPose.matrixHash != 0u
            ? publishedPose.matrixHash
            : War3SemanticHashMatrixPalette(publishedPoseMatrices, poseCount);
    return true;
  };

  bool usingPublishedPose = tryUsePublishedPose(runtimeModelPtr);
  if (!usingPublishedPose) {
    constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
    const uintptr_t runtimeValue = reinterpret_cast<uintptr_t>(runtimeModelPtr);
    if (runtimeValue >= 0x10000u) {
      if (runtimeValue <= (~uintptr_t(0u)) - kCModelComplexExtensionOffset)
        usingPublishedPose = tryUsePublishedPose(
            reinterpret_cast<void*>(runtimeValue + kCModelComplexExtensionOffset));
      if (!usingPublishedPose && runtimeValue > kCModelComplexExtensionOffset)
        usingPublishedPose = tryUsePublishedPose(
            reinterpret_cast<void*>(runtimeValue - kCModelComplexExtensionOffset));
    }
  }
  // Phase 7.51：前 3 次 tryUsePublishedPose 用的是 caller 传进来的 runtimeModelPtr
  // 及其 +/-0xA0 偏移；但 PoseRegistry 的真实 key 是 producer hook (0x12FED0) 的
  // this 参数。两者在 1.27a 上经常不一致（alias、bucket 偏移）。这里直接用
  // renderablePart 反查 producer 侧的 runtimeModel，这才是 PoseRegistry 的原始 key。
  if (!usingPublishedPose && renderablePart != nullptr) {
    void* producerOwnerRuntimeModel = nullptr;
    if (dxvk::war3::model::QueryRenderablePartOwnerRuntimeModel(
            renderablePart, &producerOwnerRuntimeModel) &&
        producerOwnerRuntimeModel != nullptr &&
        producerOwnerRuntimeModel != runtimeModelPtr) {
      usingPublishedPose = tryUsePublishedPose(producerOwnerRuntimeModel);
    }
  }
  if (!usingPublishedPose) {
    poseRuntimeModelPtr = War3ResolveLivePoseRuntimeAlias(
        runtimeModelPtr, poseCount, poseArrayPtr);
    if (poseRuntimeModelPtr == nullptr) {
      return false;
    }
    usingPublishedPose = tryUsePublishedPose(poseRuntimeModelPtr);
    if (!usingPublishedPose) {
      constexpr uintptr_t kCModelComplexExtensionOffset = 0xA0u;
      const uintptr_t poseRuntimeValue =
          reinterpret_cast<uintptr_t>(poseRuntimeModelPtr);
      if (poseRuntimeValue >= 0x10000u) {
        if (poseRuntimeValue <=
            (~uintptr_t(0u)) - kCModelComplexExtensionOffset)
          usingPublishedPose = tryUsePublishedPose(reinterpret_cast<void*>(
              poseRuntimeValue + kCModelComplexExtensionOffset));
        if (!usingPublishedPose &&
            poseRuntimeValue > kCModelComplexExtensionOffset)
          usingPublishedPose = tryUsePublishedPose(reinterpret_cast<void*>(
              poseRuntimeValue - kCModelComplexExtensionOffset));
      }
    }
  }
  if (poseRuntimeModelPtr == nullptr)
    return false;
  if (!usingPublishedPose && !allowCModelFallbackForCall &&
      !War3SemanticLivePaletteAllowCModelFallbackRuntime())
    return false;
  if (outPoseRuntimeModelPtr != nullptr)
    *outPoseRuntimeModelPtr = poseRuntimeModelPtr;

  const auto* poseBytes = reinterpret_cast<const uint8_t*>(poseArrayPtr);
  // Most Warcraft III runtime models expose a modest final-pose array here.
  // Decoding it once per visible packet is cheaper and more deterministic than
  // repeated matrix-index probes through an on-demand cache.
  (void)frameSerial;
  thread_local std::array<Matrix4, 1024> s_posePalette = {};
  if (usingPublishedPose && publishedPoseMatrices != nullptr) {
    for (uint32_t i = 0u; i < poseCount; ++i)
      s_posePalette[i] = publishedPoseMatrices[i];
  } else {
    if (poseBytes == nullptr)
      return false;
    poseCount = std::min<uint32_t>(poseCount, uint32_t(s_posePalette.size()));
    for (uint32_t i = 0u; i < poseCount; ++i) {
      s_posePalette[i] = War3DecodeRuntimePoseMatrix48(
          poseBytes + size_t(i) * sizeof(float) * 12u);
    }
  }
  if (outRawPoseHash != nullptr) {
    *outRawPoseHash = usingPublishedPose && publishedPoseHash != 0u
                          ? publishedPoseHash
                          : War3SemanticHashMatrixPalette(s_posePalette.data(),
                                                          poseCount);
  }
  auto decodePoseMatrix = [&](uint32_t index, Matrix4& outMatrix) -> bool {
    if (index >= poseCount)
      return false;
    outMatrix = s_posePalette[index];
    return true;
  };

  std::array<uint16_t, 256> uniqueGroupSlots = {};
  uint32_t uniqueGroupSlotCount = 0u;
  std::array<bool, 256> seenGroupSlots = {};
  for (const uint8_t groupSlot : vertexGroups) {
    if (!seenGroupSlots[groupSlot]) {
      seenGroupSlots[groupSlot] = true;
      uniqueGroupSlots[uniqueGroupSlotCount++] = groupSlot;
    }
  }

  auto buildDirectMatrixRemap = [&]() -> bool {
    if (matrixIndices.empty() ||
        outMaxVertexGroupSlot >= matrixIndices.size())
      return false;
    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      const uint32_t matrixIndex = matrixIndices[group];
      if (!decodePoseMatrix(matrixIndex, outPalette[group]))
        return false;
    }
    return true;
  };

  auto buildSparseMatrixRemap = [&]() -> bool {
    if (matrixIndices.empty() || uniqueGroupSlotCount == 0u ||
        uniqueGroupSlotCount > matrixIndices.size())
      return false;
    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (uint32_t i = 0u; i < uniqueGroupSlotCount; ++i) {
      const uint32_t matrixIndex = matrixIndices[i];
      const uint32_t groupSlot = uniqueGroupSlots[i];
      if (!decodePoseMatrix(matrixIndex, outPalette[groupSlot]))
        return false;
    }
    return true;
  };

  auto buildDirectPosePalette = [&]() -> bool {
    if (outMaxVertexGroupSlot >= poseCount)
      return false;
    outPalette.resize(outMaxVertexGroupSlot + 1u);
    for (uint32_t group = 0u; group <= outMaxVertexGroupSlot; ++group) {
      if (!decodePoseMatrix(group, outPalette[group]))
        return false;
    }
    return true;
  };

  auto buildSparsePosePalette = [&]() -> bool {
    if (uniqueGroupSlotCount == 0u || uniqueGroupSlotCount > poseCount)
      return false;
    outPalette.assign(outMaxVertexGroupSlot + 1u, Matrix4(0.0f));
    for (uint32_t i = 0u; i < uniqueGroupSlotCount; ++i) {
      if (!decodePoseMatrix(i, outPalette[uniqueGroupSlots[i]]))
        return false;
    }
    return true;
  };

  auto buildUniformPosePalette = [&]() -> bool {
    Matrix4 firstPose;
    if (!decodePoseMatrix(0u, firstPose))
      return false;
    const uint32_t paletteCount =
        std::max(outMaxVertexGroupSlot + 1u,
                 uint32_t(matrixGroupSizes.size()));
    if (paletteCount == 0u)
      return false;
    outPalette.assign(paletteCount, firstPose);
    return true;
  };

  const uint32_t groupCount = uint32_t(matrixGroupSizes.size());
  if (groupCount != 0u) {
    std::array<uint32_t, 256> prefix = {};
    if (groupCount <= prefix.size()) {
    uint32_t running = 0u;
    for (uint32_t i = 0u; i < groupCount; ++i) {
      prefix[i] = running;
      running += matrixGroupSizes[i];
    }
    if (running <= matrixIndices.size()) {
      outPalette.resize(groupCount);
      bool valid = true;
      for (uint32_t group = 0u; group < groupCount && valid; ++group) {
        const uint32_t groupSize = matrixGroupSizes[group];
        const uint32_t groupBase = prefix[group];
        if (groupSize == 0u ||
            (groupBase + groupSize) > matrixIndices.size()) {
          valid = false;
          break;
        }
        Matrix4 accum(0.0f);
        for (uint32_t i = 0u; i < groupSize; ++i) {
          const uint32_t matrixIndex = matrixIndices[groupBase + i];
          Matrix4 poseMatrix;
          if (!decodePoseMatrix(matrixIndex, poseMatrix)) {
            valid = false;
            break;
          }
          accum += poseMatrix;
        }
        if (valid)
          outPalette[group] =
              groupSize == 1u ? accum : (accum / float(groupSize));
      }
      if (valid) {
        for (const uint8_t groupSlot : vertexGroups) {
          if (uint32_t(groupSlot) >= groupCount) {
            valid = false;
            break;
          }
        }
      }
      if (valid && !outPalette.empty()) {
        outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                                uint32_t(outPalette.size()));
        // Phase 7.28：标注 palette 来源。walk-through 分支同样依赖
        // decodePoseMatrix 返回的 s_posePalette 内容，实际来源取决于上方
        // tryUsePublishedPose 的结果。
        markSource(usingPublishedPose
                       ? War3SemanticPaletteSource::SubmitTimePublishedPoseRegistry
                       : War3SemanticPaletteSource::SubmitTimeCModelFallback);
        return true;
      }
    }
    }
  }

  const bool fallbackOk = buildDirectMatrixRemap() || buildSparseMatrixRemap() ||
                          buildDirectPosePalette() || buildSparsePosePalette() ||
                          buildUniformPosePalette();
  if (!fallbackOk || outPalette.empty())
    return false;
  outHash = War3SemanticHashMatrixPalette(outPalette.data(),
                                          uint32_t(outPalette.size()));
  // Phase 7.28：fallback 分支（matrix remap / pose palette / uniform pose）同样
  // 消费的是 s_posePalette。来源判定沿用上方 tryUsePublishedPose 结果。
  markSource(usingPublishedPose
                 ? War3SemanticPaletteSource::SubmitTimePublishedPoseRegistry
                 : War3SemanticPaletteSource::SubmitTimeCModelFallback);
  return true;
}

// ---------------------------------------------------------------------------
// M2-3：motion / churn 诊断三函数本体 + 各自 Entry 结构（逐字节迁自
// d3d9_device.cpp :7344-7520 的匿名命名空间文本块，抽取与校验由
// AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-3 与
// 迁移脚本共同保证）。device.cpp 的 5 个调用点经同一条 using-directive
// 机械解析；调用点编排不在本轮范围（M2-5）。
// War3ShadowCaptureStats 是共享真实类型（d3d9_war3_scene.h）；该重量头只在
// 本 .cpp include，模块 .h 仅前向声明，故头文件 include 面不膨胀。
// Entry 结构是本翻译单元的实现细节，不导出。
// ---------------------------------------------------------------------------

struct War3SemanticPaletteMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t rawHash = 0u;
  uint64_t groupHash = 0u;
  uint64_t frameSerial = 0u;
};

void War3NoteLivePaletteMotion(War3ShadowCaptureStats& stats,
                               void* runtimeModelPtr,
                               uint64_t frameSerial,
                               uint64_t rawHash,
                               uint64_t groupHash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || rawHash == 0u || groupHash == 0u)
    return;

  stats.semanticSceneLivePaletteMotionSampleCount++;
  static std::array<War3SemanticPaletteMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticPaletteMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }

  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }

  if (entry == nullptr) {
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];
  }

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneLivePaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneLivePaletteMotionLastPrevRawHash =
      isNewRuntime ? 0u : entry->rawHash;
  stats.semanticSceneLivePaletteMotionLastRawHash = rawHash;
  stats.semanticSceneLivePaletteMotionLastPrevGroupHash =
      isNewRuntime ? 0u : entry->groupHash;
  stats.semanticSceneLivePaletteMotionLastGroupHash = groupHash;

  if (isNewRuntime) {
    stats.semanticSceneLivePaletteMotionNewRuntimeCount++;
  } else {
    if (entry->rawHash != rawHash)
      stats.semanticSceneLivePaletteMotionRawChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionRawStableCount++;

    if (entry->groupHash != groupHash)
      stats.semanticSceneLivePaletteMotionGroupChangedCount++;
    else
      stats.semanticSceneLivePaletteMotionGroupStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->rawHash = rawHash;
  entry->groupHash = groupHash;
  entry->frameSerial = frameSerial;
}

struct War3SemanticHashMotionEntry {
  void* runtimeModelPtr = nullptr;
  uint64_t hash = 0u;
  uint64_t frameSerial = 0u;
};

void War3NoteDrawTimePoseMotion(War3ShadowCaptureStats& stats,
                                void* runtimeModelPtr,
                                uint64_t frameSerial,
                                uint64_t hash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneDrawTimePoseLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneDrawTimePoseLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneDrawTimePoseLastHash = hash;

  if (!isNewRuntime) {
    if (entry->hash != hash)
      stats.semanticSceneDrawTimePoseChangedCount++;
    else
      stats.semanticSceneDrawTimePoseStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}

void War3NoteSubmittedPaletteMotion(War3ShadowCaptureStats& stats,
                                    void* runtimeModelPtr,
                                    uint64_t frameSerial,
                                    uint64_t hash) {
  if (!War3SemanticPaletteDiagnosticsRuntime())
    return;
  if (runtimeModelPtr == nullptr || hash == 0u)
    return;

  stats.semanticSceneSubmittedPaletteMotionSampleCount++;
  static std::array<War3SemanticHashMotionEntry, 512> s_entries = {};
  static uint32_t s_replaceCursor = 0u;

  War3SemanticHashMotionEntry* entry = nullptr;
  for (auto& candidate : s_entries) {
    if (candidate.runtimeModelPtr == runtimeModelPtr) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    for (auto& candidate : s_entries) {
      if (candidate.runtimeModelPtr == nullptr) {
        entry = &candidate;
        break;
      }
    }
  }
  if (entry == nullptr)
    entry = &s_entries[s_replaceCursor++ % s_entries.size()];

  const bool isNewRuntime = entry->runtimeModelPtr != runtimeModelPtr;
  stats.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr =
      reinterpret_cast<uintptr_t>(runtimeModelPtr);
  stats.semanticSceneSubmittedPaletteMotionLastPrevHash =
      isNewRuntime ? 0u : entry->hash;
  stats.semanticSceneSubmittedPaletteMotionLastHash = hash;

  if (isNewRuntime) {
    stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount++;
  } else if (entry->hash != hash) {
    stats.semanticSceneSubmittedPaletteMotionChangedCount++;
  } else {
    stats.semanticSceneSubmittedPaletteMotionStableCount++;
  }

  entry->runtimeModelPtr = runtimeModelPtr;
  entry->hash = hash;
  entry->frameSerial = frameSerial;
}

// ---------------------------------------------------------------------------
// M2-4 (2026-09-18): palette compose-policy predicates / env getters.
// The body between each BEGIN/END marker below must be the LF-normalized
// byte-for-byte copy of the pre-M2-4 d3d9_device.cpp extraction (enforced
// bidirectionally by generator --m2-4). The only mechanical signature
// differences: the env getters drop inline (declared in the header) and the
// A4 pointer overload does not repeat the checkReadable default argument
// (M2-2 precedent: defaults live on the header declaration).
// ---------------------------------------------------------------------------

bool War3SemanticPaletteInPlaceAppendRuntime() {
    // --- BEGIN M2-4 body: War3SemanticPaletteInPlaceAppendRuntime (pre-M2-4 d3d9_device.cpp :1302-1309) ---

  // Construct the compact live palette directly in its scene slot. Keep a
  // runtime A/B gate so isolated tests can compare the two byte-equivalent
  // paths without changing the fixed 256-matrix shader upload layout.
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND", 1u) != 0u;
  return s_enabled;
    // --- END M2-4 body: War3SemanticPaletteInPlaceAppendRuntime ---
}

bool War3SemanticDrawTimePoseRuntime() {
    // --- BEGIN M2-4 body: War3SemanticDrawTimePoseRuntime (pre-M2-4 d3d9_device.cpp :2079-2087) ---

  // 1.27a 默认关闭：实机报告证实 D3DRS_VERTEXBLEND 恒为 DISABLE，
  // draw-time D3D palette 路径 100% NoVertexBlend 拒绝（Published=0）。
  // 默认开时每帧数百次空跑会直接抬高 capture 热路径与 Untracked。
  // 诊断时再用 DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE=1。
  static const bool s_enabled =
      War3GetEnvU32("DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE", 0u) != 0u;
  return s_enabled;
    // --- END M2-4 body: War3SemanticDrawTimePoseRuntime ---
}

float War3SemanticBoundsRadiusForObjectKind(uint8_t objectKind) {
    // --- BEGIN M2-4 body: War3SemanticBoundsRadiusForObjectKind (pre-M2-4 d3d9_device.cpp :7144-7160) ---

  using dxvk::war3::render::ObjectKind;
  switch (static_cast<ObjectKind>(objectKind)) {
  case ObjectKind::Unit:
    return 260.0f;
  case ObjectKind::Building:
    return 900.0f;
  case ObjectKind::Destructible:
    return 750.0f;
  case ObjectKind::Item:
    return 220.0f;
  case ObjectKind::Effect:
    return 900.0f;
  default:
    return 0.0f;
  }
    // --- END M2-4 body: War3SemanticBoundsRadiusForObjectKind ---
}

float War3SemanticTranslationDistanceSq(const Matrix4& a, const Matrix4& b) {
    // --- BEGIN M2-4 body: War3SemanticTranslationDistanceSq (pre-M2-4 d3d9_device.cpp :7177-7182) ---

  const float dx = a[3].x - b[3].x;
  const float dy = a[3].y - b[3].y;
  const float dz = a[3].z - b[3].z;
  return dx * dx + dy * dy + dz * dz;
    // --- END M2-4 body: War3SemanticTranslationDistanceSq ---
}

bool War3SemanticTranslationFinite(const Matrix4& m) {
    // --- BEGIN M2-4 body: War3SemanticTranslationFinite (pre-M2-4 d3d9_device.cpp :7184-7187) ---

  return std::isfinite(m[3].x) && std::isfinite(m[3].y) &&
         std::isfinite(m[3].z);
    // --- END M2-4 body: War3SemanticTranslationFinite ---
}

bool War3SemanticPaletteStorageReadable(const std::vector<Matrix4>& palette) {
    // --- BEGIN M2-4 body: War3SemanticPaletteStorageReadable (pre-M2-4 d3d9_device.cpp :7189-7196) ---

  if (palette.empty())
    return false;
  if (palette.size() > 256u)
    return false;
  return dxvk::war3::IsReadableRange(
      palette.data(), palette.size() * sizeof(Matrix4));
    // --- END M2-4 body: War3SemanticPaletteStorageReadable ---
}

bool War3SemanticPaletteLooksModelLocal(
    const Matrix4* palette,
    uint32_t paletteCount,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    bool checkReadable) {
    // --- BEGIN M2-4 body: War3SemanticPaletteLooksModelLocal[pointer] (pre-M2-4 d3d9_device.cpp :7209-7263) ---

  if (palette == nullptr || paletteCount == 0u ||
      (checkReadable &&
       !dxvk::war3::IsReadableRange(palette,
                                    size_t(paletteCount) * sizeof(Matrix4))) ||
      !War3SemanticTranslationFinite(worldTransform))
    return false;

  const float worldMagSq = worldTransform[3].x * worldTransform[3].x +
                           worldTransform[3].y * worldTransform[3].y +
                           worldTransform[3].z * worldTransform[3].z;
  if (!(worldMagSq > 16.0f))
    return false;

  float guardRadius = War3SemanticBoundsRadiusForObjectKind(objectKind);
  if (!(guardRadius > 0.0f))
    guardRadius = 260.0f;
  guardRadius = std::max(384.0f, guardRadius * 1.5f);
  const float thresholdSq = guardRadius * guardRadius;

  float closestSq = std::numeric_limits<float>::max();
  float closestPaletteMagSq = std::numeric_limits<float>::max();
  const uint32_t sampleCount =
      std::min<uint32_t>(paletteCount, 4u);
  for (uint32_t i = 0u; i < sampleCount; ++i) {
    if (!War3SemanticTranslationFinite(palette[i]))
      return false;
    const float px = palette[i][3].x;
    const float py = palette[i][3].y;
    const float pz = palette[i][3].z;
    closestPaletteMagSq =
        std::min(closestPaletteMagSq, px * px + py * py + pz * pz);
    closestSq = std::min(
        closestSq,
        War3SemanticTranslationDistanceSq(palette[i], worldTransform));
  }

  // The shadow caster shader expects world-space fixed-function matrices
  // because it evaluates `in_pos * paletteMatrix` directly. CModel's live
  // final-pose array can be model-local on the semantic direct-read path, while
  // CModel+0x64 carries the runtime world transform. If sampled palette
  // translations are far from the runtime world origin, treat the palette as
  // model-local and compose it to the same world-space contract the old D3D
  // fixed-function path provided.
  const float localMagLimit = std::max(1024.0f, guardRadius * 2.0f);
  if (closestPaletteMagSq > localMagLimit * localMagLimit)
    return false;

  return closestSq > thresholdSq;
    // --- END M2-4 body: War3SemanticPaletteLooksModelLocal[pointer] ---
}

bool War3SemanticPaletteLooksModelLocal(
    const std::vector<Matrix4>& palette,
    const Matrix4& worldTransform,
    uint8_t objectKind) {
    // --- BEGIN M2-4 body: War3SemanticPaletteLooksModelLocal[vector] (pre-M2-4 d3d9_device.cpp :7265-7274) ---

  if (!War3SemanticPaletteStorageReadable(palette))
    return false;
  return War3SemanticPaletteLooksModelLocal(
      palette.data(), uint32_t(palette.size()), worldTransform, objectKind,
      false);
    // --- END M2-4 body: War3SemanticPaletteLooksModelLocal[vector] ---
}

uint64_t War3SemanticHashMatrix4(const Matrix4& matrix) {
    // --- BEGIN M2-4 body: War3SemanticHashMatrix4 (pre-M2-4 d3d9_device.cpp :7292-7301) ---

  uint64_t hash = bit::fnv1a_init();
  for (uint32_t r = 0u; r < 4u; ++r) {
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].x));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].y));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].z));
    hash = bit::fnv1a_iter(hash, bit::cast<uint32_t>(matrix[r].w));
  }
  return hash;
    // --- END M2-4 body: War3SemanticHashMatrix4 ---
}

// ---------------------------------------------------------------------------
// A9 (2026-09-18 dead-code ruling): War3SemanticBuildWorldPaletteIfNeeded.
// The ruling on dead-code inventory item A9 is MIGRATE, not delete: the
// function had zero callers, but its body is the complete model-local ->
// world-space palette compose contract served by A4
// (War3SemanticPaletteLooksModelLocal). It is dead by callers only, so it
// carries no [[maybe_unused]] here (external linkage in this module does
// not warn when unused). The body between the BEGIN/END markers below is
// the LF-normalized byte-for-byte copy of the pre-A9 d3d9_device.cpp
// extraction, enforced bidirectionally by generator --a9 and by
// AutoTest/test_war3_palette_a9_migration_equivalence_static.py.
// ---------------------------------------------------------------------------

void War3SemanticBuildWorldPaletteIfNeeded(
    const std::vector<Matrix4>& sourcePalette,
    const Matrix4& worldTransform,
    uint8_t objectKind,
    std::vector<Matrix4>& outPalette) {
    // --- BEGIN A9 body: War3SemanticBuildWorldPaletteIfNeeded (pre-A9 d3d9_device.cpp :7161-7175) ---

  outPalette.clear();
  if (!War3SemanticPaletteLooksModelLocal(sourcePalette, worldTransform,
                                          objectKind)) {
    return;
  }

  outPalette.reserve(sourcePalette.size());
  for (const Matrix4& localMatrix : sourcePalette)
    outPalette.push_back(worldTransform * localMatrix);
    // --- END A9 body: War3SemanticBuildWorldPaletteIfNeeded ---
}

} // namespace dxvk::war3::semantic
