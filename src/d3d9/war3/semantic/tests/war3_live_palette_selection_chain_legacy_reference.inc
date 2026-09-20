// AUTO-GENERATED legacy reference for the M2-2 selection-chain migration.
//
// DO NOT EDIT. This file is the verbatim pre-migration implementation text of
// the selection-chain body War3TryBuildLiveRuntimeGroupPalette (plus the
// supporting types/counters it uses) that M2-2 moved out of
// src/d3d9/d3d9_device.cpp into
// src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}.
// Regenerate with:
//   py AutoTest/gen_war3_live_palette_selection_legacy_reference.py --m2-2
//
// Provenance (recorded 2026-09-18, uncommitted work tree, migration applied on top):
//   pre-M2-2 work-tree src/d3d9/d3d9_device.cpp (snapshot %TEMP%\\device_pre_m2_2.cpp)
//   SHA-256(pre-M2-2 work-tree d3d9_device.cpp) = B70F3AF97CD3DB554DDB311E9B89F3C64B99996EF9ADEA4FBA4CA7D2CF6F98F1
//   size = 2310648 bytes, captured 2026-09-17T22:54:30 (machine local time)
//   The pre-M2-2 state equals the post-M2-1 work tree (same SHA as the M2-1
//   record's "post-migration d3d9_device.cpp"); like M2-1 this is a work-tree
//   file hash, not a git blob hash (M2-1's work is uncommitted).
//
// Post-migration contract patches applied (declared in the generator as
// POST_MIGRATION_CONTRACT_PATCHES; each is named, dated, justified and applied
// as an exactly-once fixed-point replacement, so this file stays reproducible):
//   + POST-MIGRATION CONTRACT PATCH [R1-cold-cache-groupcount] (2026-09-18)
//     冷缓存首次查询原先不校验 producer 记录的 groupCount，与热缓存路径的数量 规则不一致：同一对象、同一数量条件会在冷缓存下被接受、热缓存下被拒绝。 修复使其适用同一规则（war3_skin_palette_selection.h 的 ProducerGroupCountCovers）；依据 selection.cpp:325-332 既有契约 （拒绝后落到替代路径属合法设计）与批次 4 步骤①的行为测试。
//
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

namespace m2_legacy_reference {

// --- supporting verbatim definitions (NOT the migrated chain
// itself): the two Gap A counters the chain increments, from
// d3d9_device.cpp line 1060 (definition lines verbatim; the
// device-layout comment above them is intentionally not carried,
// see header) ---
std::atomic<uint64_t> g_devicePaletteSlotCacheServedAfterConfirmCount{0};

// --- supporting verbatim definitions (NOT the migrated chain
// itself): the two Gap A counters the chain increments, from
// d3d9_device.cpp line 1061 (definition lines verbatim; the
// device-layout comment above them is intentionally not carried,
// see header) ---
std::atomic<uint64_t> g_devicePaletteSlotCacheRejectedStaleCount{0};

// --- supporting verbatim types: War3SemanticPaletteSource /
// War3LivePaletteBuildPhase / kWar3LivePaletteBuildPhaseCount /
// War3LivePaletteBuildTiming / War3LivePaletteBuildRawTiming, from
// d3d9_device.cpp line 5883 ---
// Phase 7.28：skinned palette content stability probe。
// 当 skinned packet 的 palette 在 submit 阶段完成选择后，我们需要知道这份
// palette 具体是从哪条来源读回的。不同来源在稳定性、读取时机和时效性上差距
// 极大，也就是用户观察到的"储物桶(rigid)不闪、skinned 单位闪"的真正分水岭。
// 该枚举刻意保持 "submit 端视角"：DrawTimeCaptured 才是和 current-draw 同
// 帧同事件捕获到的真源；其它分支都涉及一次性重读或跨帧缓存。
enum class War3SemanticPaletteSource : uint32_t {
  None = 0,                           // 没有 live palette，fallback 到 packet 原始 palette
  DrawTimeCaptured = 1,               // current-draw 同步捕获的 palette（最稳定）
  SubmitTimeGlobalSlot = 2,           // Game.dll + 0xBC6BD0 全局 palette 通路，slotIndex 通过 renderablePart+0x08 读取
  SubmitTimeBlendedPaletteCache = 3,  // QueryBlendedPaletteBySlotIndex（Hook_RuntimeMatrixWrite 同帧捕获）
  SubmitTimePublishedPoseRegistry = 4, // 发布过的 PoseRegistry record
  SubmitTimeCModelFallback = 5,       // CModel +0x5C/+0x60 的 FinalPoseMatrixArray（当前 1.27a 偏移不可信）
  OwnedPartSnapshot = 6,             // CPU-owned producer publication, not a native allocation lease
};

enum class War3LivePaletteBuildPhase : uint8_t {
  GroupScan = 0u,
  SlotResolve,
  FrameTagQuery,
  PartSnapshot,
  GlobalModuleLookup,
  GlobalPointerRead,
  GlobalRangeCheck,
  GlobalSafeCopy,
  GlobalDecode,
  GlobalHash,
  BlendedSlot,
  PoseFallback,
  Count,
};

constexpr size_t kWar3LivePaletteBuildPhaseCount =
    static_cast<size_t>(War3LivePaletteBuildPhase::Count);

struct War3LivePaletteBuildTiming {
  std::array<uint64_t, kWar3LivePaletteBuildPhaseCount> ticks = {};
  std::array<uint32_t, kWar3LivePaletteBuildPhaseCount> calls = {};
  uint64_t qpcOverheadTicks = 0u;
};

class War3LivePaletteBuildRawTiming final {
public:
  explicit War3LivePaletteBuildRawTiming(
      War3LivePaletteBuildTiming* timing)
      : m_timing(timing) {
  }

  ~War3LivePaletteBuildRawTiming() {
    if (m_timing != nullptr)
      closeCurrent(dxvk::high_resolution_clock::get_counter());
  }

  War3LivePaletteBuildRawTiming(
      const War3LivePaletteBuildRawTiming&) = delete;
  War3LivePaletteBuildRawTiming& operator=(
      const War3LivePaletteBuildRawTiming&) = delete;

  inline void enter(War3LivePaletteBuildPhase phase) {
    if (m_timing == nullptr)
      return;
    const int64_t now = dxvk::high_resolution_clock::get_counter();
    closeCurrent(now);
    m_phase = phase;
    m_begin = now;
    ++m_timing->calls[static_cast<size_t>(phase)];
  }

private:
  inline void closeCurrent(int64_t now) {
    if (m_begin == 0 || m_phase == War3LivePaletteBuildPhase::Count)
      return;
    if (now > m_begin) {
      const size_t index = static_cast<size_t>(m_phase);
      const uint64_t elapsed = uint64_t(now - m_begin);
      const uint64_t corrected = elapsed > m_timing->qpcOverheadTicks
          ? elapsed - m_timing->qpcOverheadTicks : 0u;
      m_timing->ticks[index] += corrected;
    }
  }

  War3LivePaletteBuildTiming* m_timing = nullptr;
  War3LivePaletteBuildPhase m_phase = War3LivePaletteBuildPhase::Count;
  int64_t m_begin = 0;
};


// --- supporting verbatim forward declaration: carries the
// outSelection (parameter 17) default argument; the definition below
// legally ADDS the parameter 8-16 defaults, from d3d9_device.cpp
// line 5969 ---
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
    dxvk::war3::render::skin::Selection* outSelection = nullptr);


// --- pre-migration definition: d3d9_device.cpp line 7443 (War3TryBuildLiveRuntimeGroupPalette) ---
bool War3TryBuildLiveRuntimeGroupPalette(
    const dxvk::war3::shadow::ShadowPacketResource& resource,
    void* runtimeModelPtr,
    void* renderablePart,
    uint64_t frameSerial,
    std::vector<Matrix4>& outPalette,
    uint32_t& outMaxVertexGroupSlot,
    uint64_t& outHash,
    uint64_t* outRawPoseHash = nullptr,
    void** outPoseRuntimeModelPtr = nullptr,
    bool allowCModelFallbackForCall = false,
    War3SemanticPaletteSource* outPaletteSource = nullptr,
    uint32_t* outPaletteSlotIndex = nullptr,
    uint32_t* outPaletteMinFrameTag = nullptr,
    uint32_t* outPaletteMaxFrameTag = nullptr,
    War3LivePaletteBuildTiming* outTiming = nullptr,
    uint32_t provenMaxVertexGroupSlot = 0xFFFFFFFFu,
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
          producerGroupCount >= requiredPaletteCount) {
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
      // --- POST-MIGRATION CONTRACT PATCH [R1-cold-cache-groupcount] (2026-09-18) ---
      // 冷缓存首次查询必须与热缓存适用同一条数量规则；拒绝时**不写负缓存**，
      // 下帧绑定恢复后仍可重新命中（与本补丁依据的既有契约一致）。
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


} // namespace m2_legacy_reference
