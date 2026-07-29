#include "war3_hook_render_identity.h"

#include "war3_hook_address_book.h"
#include "war3_hook_install_util.h"
#include "war3_hook_perf.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_memory.h"
#include "../core/war3_internal_test_config.h"
#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_renderer.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dxvk {

extern std::atomic<bool> g_renderQueueGlobalsValid;
extern uint32_t *g_numOfElementsPtr;
extern uint32_t *g_numOfTransparentPtr;
extern void **g_batchArrayPtr;

} // namespace dxvk

namespace dxvk::war3::hooks {

using WorldObjectListEntryWriteFn = int(__fastcall *)(void *, void *, void *,
                                                      int);
using WorldObjectEntryRenderFn = int(__fastcall *)(void *, int);
using RenderQueueAddBatchFn = void(__fastcall *)(void *, int);
using RenderBatchSubmitFn = void(__fastcall *)(void *, int);
using AucTransparentAddEntryFn =
    int(__fastcall *)(void *, uint32_t, const float *, uint32_t);

static WorldObjectListEntryWriteFn g_originalWorldObjectListEntryWrite = nullptr;
static WorldObjectListEntryWriteFn g_trampolineWorldObjectListEntryWrite = nullptr;

static WorldObjectEntryRenderFn g_originalWorldObjectEntryRender = nullptr;
static WorldObjectEntryRenderFn g_trampolineWorldObjectEntryRender = nullptr;

static RenderQueueAddBatchFn g_originalRenderQueueAddBatch = nullptr;
static RenderQueueAddBatchFn g_trampolineRenderQueueAddBatch = nullptr;

static RenderBatchSubmitFn g_originalRenderBatchSubmit = nullptr;
static RenderBatchSubmitFn g_trampolineRenderBatchSubmit = nullptr;

static AucTransparentAddEntryFn g_originalAucTransparentAddEntry = nullptr;
static AucTransparentAddEntryFn g_trampolineAucTransparentAddEntry = nullptr;

namespace {

void **g_transparentArrayBasePtr = nullptr;
constexpr size_t kTransparentEntryStride = 24;
constexpr size_t kTransparentEntryDistSqOffset = 8;
constexpr size_t kRecentWorldObjectOwnerHintSlots = 256;
std::atomic<uint64_t> g_worldObjectEntryRenderCallCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderSceneNodeReadyBeforeCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderSceneNodeReadyAfterCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderSceneNodeFilledByCallCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderSceneNodeChangedCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderKnownListOwnerHintZeroCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderKnownListOwnerHintNonzeroCount{0u};
std::atomic<uint64_t> g_worldObjectEntryRenderUnknownListOwnerHintCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteCallCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteOwnerHintZeroCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteOwnerHintNonzeroCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteOwnerHintHandleCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteOwnerHintUnitPtrCount{0u};
std::atomic<uint64_t> g_worldObjectListEntryWriteSerial{0u};
std::atomic<uint64_t> g_lastWorldObjectEntryRenderEntryPtr{0u};
std::atomic<uint64_t> g_lastWorldObjectEntryRenderResolvedListOwnerHintValue{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntryWriteListPtr{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntryWriteWorldObjectEntryPtr{0u};
std::atomic<uint64_t> g_lastWorldObjectListEntryWriteOwnerHintValue{0u};
std::atomic<uint64_t> g_lastWorldObjectEntryRenderSceneNodeBeforePtr{0u};
std::atomic<uint64_t> g_lastWorldObjectEntryRenderSceneNodeAfterPtr{0u};
std::atomic<bool> g_renderIdentityFullDiagnostics{false};
std::atomic<bool> g_worldObjectListEntryWriteProbeHookInstalled{false};
std::atomic<bool> g_worldObjectEntryRenderContextHookInstalled{false};
std::atomic<bool> g_renderQueueIdentityPrimingHookInstalled{false};
std::array<std::atomic<uint64_t>, kRecentWorldObjectOwnerHintSlots>
    g_recentWorldObjectEntryPtrs = {};
std::array<std::atomic<uint64_t>, kRecentWorldObjectOwnerHintSlots>
    g_recentWorldObjectEntryOwnerHints = {};
std::array<std::atomic<uint64_t>, kRecentWorldObjectOwnerHintSlots>
    g_recentWorldObjectEntryGenerations = {};

struct ScopedWorldObjectContext {
  void *prevEntry = nullptr;
  void *prevScene = nullptr;

  ScopedWorldObjectContext(void *worldObjectEntry, void *sceneNode) {
    auto &renderer = dxvk::war3::render::War3Renderer::instance();
    prevEntry = renderer.GetCurrentWorldObjectEntry();
    prevScene = renderer.GetCurrentSceneNode();
    renderer.SetCurrentWorldObjectContext(worldObjectEntry, sceneNode);
  }

  ~ScopedWorldObjectContext() {
    auto &renderer = dxvk::war3::render::War3Renderer::instance();
    if (prevEntry != nullptr || prevScene != nullptr) {
      renderer.SetCurrentWorldObjectContext(prevEntry, prevScene);
    } else {
      renderer.ClearCurrentWorldObjectContext();
    }
  }
};

void RecordRecentWorldObjectOwnerHint(void *worldObjectEntry,
                                      uint32_t ownerHintRaw) {
  if (worldObjectEntry == nullptr)
    return;

  const uint64_t serial =
      g_worldObjectListEntryWriteSerial.fetch_add(1u,
                                                  std::memory_order_relaxed) +
      1u;
  const size_t slot = size_t(serial % kRecentWorldObjectOwnerHintSlots);
  g_recentWorldObjectEntryPtrs[slot].store(
      uint64_t(reinterpret_cast<uintptr_t>(worldObjectEntry)),
      std::memory_order_relaxed);
  g_recentWorldObjectEntryOwnerHints[slot].store(uint64_t(ownerHintRaw),
                                                 std::memory_order_relaxed);
  g_recentWorldObjectEntryGenerations[slot].store(serial,
                                                  std::memory_order_relaxed);
}

bool TryResolveRecentWorldObjectOwnerHint(void *worldObjectEntry,
                                          uint32_t &ownerHintRaw) {
  if (worldObjectEntry == nullptr)
    return false;

  const uint64_t target = uint64_t(reinterpret_cast<uintptr_t>(worldObjectEntry));
  uint64_t bestGeneration = 0u;
  uint32_t bestOwnerHint = 0u;

  for (size_t i = 0; i < kRecentWorldObjectOwnerHintSlots; ++i) {
    const uint64_t generation =
        g_recentWorldObjectEntryGenerations[i].load(std::memory_order_relaxed);
    if (generation == 0u)
      continue;

    const uint64_t entryPtr =
        g_recentWorldObjectEntryPtrs[i].load(std::memory_order_relaxed);
    if (entryPtr != target)
      continue;

    if (generation >= bestGeneration) {
      bestGeneration = generation;
      bestOwnerHint = static_cast<uint32_t>(
          g_recentWorldObjectEntryOwnerHints[i].load(std::memory_order_relaxed));
    }
  }

  if (bestGeneration == 0u)
    return false;

  ownerHintRaw = bestOwnerHint;
  return true;
}

} // namespace

static float TryReadTransparentDistanceSq(uint32_t queueSlot) {
  if (g_transparentArrayBasePtr == nullptr)
    return 0.0f;

  void *base = *g_transparentArrayBasePtr;
  if (base == nullptr)
    return 0.0f;

  float distanceSq = 0.0f;
  dxvk::war3::SafeReadFast(
      reinterpret_cast<const std::uint8_t *>(base) +
          size_t(queueSlot) * kTransparentEntryStride +
          kTransparentEntryDistSqOffset,
      0u, distanceSq);
  return distanceSq;
}

static int __fastcall Hook_WorldObjectListEntry_Write(void *listPtr,
                                                      void * /*edx*/,
                                                      void *worldObjectEntry,
                                                      int ownerHint) {
  const uint32_t ownerHintRaw = static_cast<uint32_t>(ownerHint);
  g_worldObjectListEntryWriteCallCount.fetch_add(1u,
                                                 std::memory_order_relaxed);
  if (ownerHintRaw == 0u) {
    g_worldObjectListEntryWriteOwnerHintZeroCount.fetch_add(
        1u, std::memory_order_relaxed);
  } else {
    g_worldObjectListEntryWriteOwnerHintNonzeroCount.fetch_add(
        1u, std::memory_order_relaxed);
    if (ownerHintRaw < 0x01000000u) {
      g_worldObjectListEntryWriteOwnerHintHandleCount.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      g_worldObjectListEntryWriteOwnerHintUnitPtrCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  }
  g_lastWorldObjectListEntryWriteListPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(listPtr)),
      std::memory_order_relaxed);
  g_lastWorldObjectListEntryWriteWorldObjectEntryPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(worldObjectEntry)),
      std::memory_order_relaxed);
  g_lastWorldObjectListEntryWriteOwnerHintValue.store(
      uint64_t(ownerHintRaw), std::memory_order_relaxed);
  RecordRecentWorldObjectOwnerHint(worldObjectEntry, ownerHintRaw);

  if (g_trampolineWorldObjectListEntryWrite)
    return g_trampolineWorldObjectListEntryWrite(listPtr, nullptr,
                                                 worldObjectEntry, ownerHint);
  if (g_originalWorldObjectListEntryWrite)
    return g_originalWorldObjectListEntryWrite(listPtr, nullptr,
                                               worldObjectEntry, ownerHint);
  return ownerHint;
}

static int CallOriginalWorldObjectEntryRender(void *entry, int reserved) {
  if (g_trampolineWorldObjectEntryRender)
    return g_trampolineWorldObjectEntryRender(entry, reserved);
  if (g_originalWorldObjectEntryRender)
    return g_originalWorldObjectEntryRender(entry, reserved);
  return 0;
}

// Production keeps this hook because ScopedWorldObjectContext is consumed by
// RenderQueue identity priming and the visible/transparent registries.  The
// pre/post scene reads and the 256-slot owner-hint probe are diagnostics only.
static int __fastcall Hook_WorldObjectEntry_Render_Production(
    void *entry, int reserved) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldObjectEntryRender, 4u);
  const auto callNativeOriginal = [&]() {
    if (!g_trampolineWorldObjectEntryRender &&
        !g_originalWorldObjectEntryRender)
      return 0;
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallOriginalWorldObjectEntryRender(entry, reserved);
  };
  if (!dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled || !entry) {
    return callNativeOriginal();
  }

  ScopedWorldObjectContext scope(entry, nullptr);
  return callNativeOriginal();
}

static int __fastcall Hook_WorldObjectEntry_Render_Diagnostics(
    void *entry, int reserved) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::WorldObjectEntryRender, 4u);
  const auto callNativeOriginal = [&]() {
    if (!g_trampolineWorldObjectEntryRender &&
        !g_originalWorldObjectEntryRender)
      return 0;
    War3HotHookNativeScope nativeTiming(hookTiming);
    return CallOriginalWorldObjectEntryRender(entry, reserved);
  };
  if (!dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled || !entry) {
    return callNativeOriginal();
  }

  void *sceneNodeBefore = nullptr;
  dxvk::war3::SafeReadPtrFast(entry, 0x20, sceneNodeBefore);
  g_worldObjectEntryRenderCallCount.fetch_add(1u, std::memory_order_relaxed);
  if (sceneNodeBefore != nullptr) {
    g_worldObjectEntryRenderSceneNodeReadyBeforeCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_lastWorldObjectEntryRenderEntryPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(entry)), std::memory_order_relaxed);
  g_lastWorldObjectEntryRenderSceneNodeBeforePtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sceneNodeBefore)),
      std::memory_order_relaxed);
  uint32_t resolvedListOwnerHintRaw = 0u;
  if (TryResolveRecentWorldObjectOwnerHint(entry, resolvedListOwnerHintRaw)) {
    if (resolvedListOwnerHintRaw == 0u) {
      g_worldObjectEntryRenderKnownListOwnerHintZeroCount.fetch_add(
          1u, std::memory_order_relaxed);
    } else {
      g_worldObjectEntryRenderKnownListOwnerHintNonzeroCount.fetch_add(
          1u, std::memory_order_relaxed);
    }
  } else {
    g_worldObjectEntryRenderUnknownListOwnerHintCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_lastWorldObjectEntryRenderResolvedListOwnerHintValue.store(
      uint64_t(resolvedListOwnerHintRaw), std::memory_order_relaxed);

  ScopedWorldObjectContext scope(entry, nullptr);

  const int result = callNativeOriginal();

  void *sceneNodeAfter = nullptr;
  dxvk::war3::SafeReadPtrFast(entry, 0x20, sceneNodeAfter);
  if (sceneNodeAfter != nullptr) {
    g_worldObjectEntryRenderSceneNodeReadyAfterCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (sceneNodeBefore == nullptr && sceneNodeAfter != nullptr) {
    g_worldObjectEntryRenderSceneNodeFilledByCallCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  if (sceneNodeBefore != sceneNodeAfter) {
    g_worldObjectEntryRenderSceneNodeChangedCount.fetch_add(
        1u, std::memory_order_relaxed);
  }
  g_lastWorldObjectEntryRenderSceneNodeAfterPtr.store(
      uint64_t(reinterpret_cast<uintptr_t>(sceneNodeAfter)),
      std::memory_order_relaxed);
  return result;
}

static void __fastcall Hook_RenderQueue_AddBatch(void *sceneNode, int reserved) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::RenderQueueAddBatch, 4u);
  dxvk::war3::render::RenderObjectIdentitySnapshot identity = {};
  void *batchArrayBefore = nullptr;
  uint32_t before = 0;
  bool canPrime = false;

  if (dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled && sceneNode &&
      dxvk::g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
      dxvk::g_numOfElementsPtr != nullptr && dxvk::g_batchArrayPtr != nullptr &&
      dxvk::war3::render::TryResolveCurrentRenderObjectIdentity(sceneNode,
                                                                identity)) {
    before = *dxvk::g_numOfElementsPtr;
    batchArrayBefore = *dxvk::g_batchArrayPtr;
    canPrime = identity.HasStableIdentity();

    if (identity.worldObjectEntry != nullptr) {
      dxvk::war3::render::War3Renderer::instance().OnWorldObjectEntry(
          identity.worldObjectEntry, sceneNode);
    }
  }

  if (g_trampolineRenderQueueAddBatch) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineRenderQueueAddBatch(sceneNode, reserved);
  } else if (g_originalRenderQueueAddBatch) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_originalRenderQueueAddBatch(sceneNode, reserved);
  }

  if (!canPrime || dxvk::g_numOfElementsPtr == nullptr ||
      dxvk::g_batchArrayPtr == nullptr)
    return;

  identity.sceneNode = sceneNode;
  const uint32_t after = *dxvk::g_numOfElementsPtr;
  void *batchArrayAfter = *dxvk::g_batchArrayPtr;
  void *batchArray = batchArrayAfter != nullptr ? batchArrayAfter : batchArrayBefore;
  if (batchArray == nullptr || after <= before)
    return;

  dxvk::war3::render::RenderQueueTracker::instance()
      .PrimeCachedObjectIdentities(batchArray, before, after, identity);
}

static void __fastcall Hook_RenderBatch_Submit(void *sceneNode, int reserved) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::RenderBatchSubmit, 4u);
  dxvk::war3::render::RenderObjectIdentitySnapshot identity = {};
  void *batchArrayBefore = nullptr;
  uint32_t before = 0;
  bool canCapture = false;

  if (dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled &&
      sceneNode != nullptr &&
      dxvk::g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
      dxvk::g_numOfElementsPtr != nullptr && dxvk::g_batchArrayPtr != nullptr) {
    before = *dxvk::g_numOfElementsPtr;
    batchArrayBefore = *dxvk::g_batchArrayPtr;
    canCapture = true;
    dxvk::war3::render::TryResolveCurrentRenderObjectIdentity(sceneNode, identity);
    identity.sceneNode = sceneNode;
  }

  if (g_trampolineRenderBatchSubmit) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_trampolineRenderBatchSubmit(sceneNode, reserved);
  } else if (g_originalRenderBatchSubmit) {
    War3HotHookNativeScope nativeTiming(hookTiming);
    g_originalRenderBatchSubmit(sceneNode, reserved);
  }

  if (!canCapture || dxvk::g_numOfElementsPtr == nullptr ||
      dxvk::g_batchArrayPtr == nullptr)
    return;

  const uint32_t after = *dxvk::g_numOfElementsPtr;
  void *batchArrayAfter = *dxvk::g_batchArrayPtr;
  void *batchArray =
      batchArrayAfter != nullptr ? batchArrayAfter : batchArrayBefore;
  if (batchArray == nullptr || after <= before)
    return;

  dxvk::war3::render::War3Renderer::instance().OnVisibleRenderables(
      batchArray, before, after, identity);
}

static int __fastcall Hook_AUCTransparent_AddEntry(void *payload,
                                                   uint32_t transparentType,
                                                   const float *worldPos,
                                                   uint32_t sortKey) {
  (void)worldPos;
  dxvk::war3::render::RenderObjectIdentitySnapshot identity = {};
  uint32_t before = 0;
  bool canCapture = false;
  int result = static_cast<int>(sortKey);

  if (dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled &&
      dxvk::g_renderQueueGlobalsValid.load(std::memory_order_relaxed) &&
      dxvk::g_numOfTransparentPtr != nullptr) {
    before = *dxvk::g_numOfTransparentPtr;
    canCapture = true;
    dxvk::war3::render::TryResolveCurrentRenderObjectIdentity(nullptr, identity);
  }

  if (g_trampolineAucTransparentAddEntry) {
    result = g_trampolineAucTransparentAddEntry(payload, transparentType,
                                                worldPos, sortKey);
  } else if (g_originalAucTransparentAddEntry) {
    result = g_originalAucTransparentAddEntry(payload, transparentType,
                                              worldPos, sortKey);
  }

  if (!canCapture || dxvk::g_numOfTransparentPtr == nullptr)
    return result;

  const uint32_t after = *dxvk::g_numOfTransparentPtr;
  if (after <= before)
    return result;

  const float distanceSq = TryReadTransparentDistanceSq(before);
  dxvk::war3::render::War3Renderer::instance().OnTransparentRenderable(
      payload, transparentType, before, sortKey, distanceSq, identity);
  return result;
}

void War3HookRenderIdentity::Install(uintptr_t gameBase) {
  const bool wantsIdentityBridge =
      dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled;
  const bool wantsVisibleManifest =
      dxvk::war3::internal::kNativeVisibleRenderableRegistryEnabled;
  const bool fullDiagnostics =
      dxvk::war3::gpu_skin::NativeBridgeFullDiagnosticsEnabled();

  g_renderIdentityFullDiagnostics.store(fullDiagnostics,
                                        std::memory_order_relaxed);
  g_worldObjectListEntryWriteProbeHookInstalled.store(
      false, std::memory_order_relaxed);
  g_worldObjectEntryRenderContextHookInstalled.store(
      false, std::memory_order_relaxed);
  g_renderQueueIdentityPrimingHookInstalled.store(
      false, std::memory_order_relaxed);

  if (!wantsIdentityBridge && !wantsVisibleManifest)
    return;

  const auto &book = GetWar3HookAddressBook127a();
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };
  auto resolveData = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID worldObjectListEntryWriteAddr =
      resolveCode(book.worldObjectListEntryWrite);
  LPVOID worldObjectEntryRenderAddr = resolveCode(book.worldObjectEntryRender);
  LPVOID renderQueueAddBatchAddr = resolveCode(book.renderQueueAddBatch);
  LPVOID renderBatchSubmitAddr = resolveCode(book.renderBatchSubmit);
  LPVOID aucTransparentAddEntryAddr = resolveCode(book.aucTransparentAddEntry);
  LPVOID transparentArrayBaseAddr = resolveData(book.rqTransparentArrayBasePtr);

  g_originalWorldObjectListEntryWrite =
      reinterpret_cast<WorldObjectListEntryWriteFn>(
          worldObjectListEntryWriteAddr);
  g_originalWorldObjectEntryRender =
      reinterpret_cast<WorldObjectEntryRenderFn>(worldObjectEntryRenderAddr);
  g_originalRenderQueueAddBatch =
      reinterpret_cast<RenderQueueAddBatchFn>(renderQueueAddBatchAddr);
  g_originalRenderBatchSubmit =
      reinterpret_cast<RenderBatchSubmitFn>(renderBatchSubmitAddr);
  g_originalAucTransparentAddEntry =
      reinterpret_cast<AucTransparentAddEntryFn>(aucTransparentAddEntryAddr);
  g_transparentArrayBasePtr =
      reinterpret_cast<void **>(transparentArrayBaseAddr);

  if (wantsIdentityBridge) {
    // The writer feeds only the diagnostic owner-hint ring.  Production-light
    // leaves Game.dll's function completely unhooked.
    if (fullDiagnostics) {
      const bool installed = InstallMinHook(
          worldObjectListEntryWriteAddr,
          reinterpret_cast<LPVOID>(&Hook_WorldObjectListEntry_Write),
          reinterpret_cast<LPVOID *>(&g_trampolineWorldObjectListEntryWrite),
          "Render", "WorldObjectListEntry_Write", false, true);
      g_worldObjectListEntryWriteProbeHookInstalled.store(
          installed, std::memory_order_relaxed);
    } else {
      g_trampolineWorldObjectListEntryWrite = nullptr;
    }

    LPVOID entryRenderDetour =
        fullDiagnostics
            ? reinterpret_cast<LPVOID>(
                  &Hook_WorldObjectEntry_Render_Diagnostics)
            : reinterpret_cast<LPVOID>(
                  &Hook_WorldObjectEntry_Render_Production);
    const bool entryRenderInstalled = InstallMinHook(
        worldObjectEntryRenderAddr, entryRenderDetour,
        reinterpret_cast<LPVOID *>(&g_trampolineWorldObjectEntryRender), "Render",
        "WorldObjectEntry_Render", false, true);
    g_worldObjectEntryRenderContextHookInstalled.store(
        entryRenderInstalled, std::memory_order_relaxed);

    const bool primingInstalled = InstallMinHook(
        renderQueueAddBatchAddr,
        reinterpret_cast<LPVOID>(&Hook_RenderQueue_AddBatch),
        reinterpret_cast<LPVOID *>(&g_trampolineRenderQueueAddBatch),
        "Render", "RenderQueue_AddBatch", false, true);
    g_renderQueueIdentityPrimingHookInstalled.store(
        primingInstalled, std::memory_order_relaxed);
  }

  if (wantsVisibleManifest) {
    InstallMinHook(renderBatchSubmitAddr,
                   reinterpret_cast<LPVOID>(&Hook_RenderBatch_Submit),
                   reinterpret_cast<LPVOID *>(&g_trampolineRenderBatchSubmit),
                   "Render", "RenderBatch_Submit", false, true);

    if constexpr (dxvk::war3::internal::
                      kNativeVisibleRenderableTransparentHookEnabled) {
      InstallMinHook(
          aucTransparentAddEntryAddr,
          reinterpret_cast<LPVOID>(&Hook_AUCTransparent_AddEntry),
          reinterpret_cast<LPVOID *>(&g_trampolineAucTransparentAddEntry),
          "Render", "AUCTransparent_AddEntry", false, true);
    } else {
      g_trampolineAucTransparentAddEntry = nullptr;
    }
  }
}

RenderIdentityLifecycleProbeSummary QueryRenderIdentityLifecycleProbeSummary() {
  RenderIdentityLifecycleProbeSummary summary = {};
  summary.fullDiagnostics =
      g_renderIdentityFullDiagnostics.load(std::memory_order_relaxed);
  summary.worldObjectListEntryWriteProbeHookInstalled =
      g_worldObjectListEntryWriteProbeHookInstalled.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderContextHookInstalled =
      g_worldObjectEntryRenderContextHookInstalled.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderPrePostProbeEnabled =
      summary.fullDiagnostics &&
      summary.worldObjectEntryRenderContextHookInstalled;
  summary.renderQueueIdentityPrimingHookInstalled =
      g_renderQueueIdentityPrimingHookInstalled.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderCallCount =
      g_worldObjectEntryRenderCallCount.load(std::memory_order_relaxed);
  summary.worldObjectEntryRenderSceneNodeReadyBeforeCount =
      g_worldObjectEntryRenderSceneNodeReadyBeforeCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderSceneNodeReadyAfterCount =
      g_worldObjectEntryRenderSceneNodeReadyAfterCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderSceneNodeFilledByCallCount =
      g_worldObjectEntryRenderSceneNodeFilledByCallCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderSceneNodeChangedCount =
      g_worldObjectEntryRenderSceneNodeChangedCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderKnownListOwnerHintZeroCount =
      g_worldObjectEntryRenderKnownListOwnerHintZeroCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderKnownListOwnerHintNonzeroCount =
      g_worldObjectEntryRenderKnownListOwnerHintNonzeroCount.load(
          std::memory_order_relaxed);
  summary.worldObjectEntryRenderUnknownListOwnerHintCount =
      g_worldObjectEntryRenderUnknownListOwnerHintCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListEntryWriteCallCount =
      g_worldObjectListEntryWriteCallCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListEntryWriteOwnerHintZeroCount =
      g_worldObjectListEntryWriteOwnerHintZeroCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListEntryWriteOwnerHintNonzeroCount =
      g_worldObjectListEntryWriteOwnerHintNonzeroCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListEntryWriteOwnerHintHandleCount =
      g_worldObjectListEntryWriteOwnerHintHandleCount.load(
          std::memory_order_relaxed);
  summary.worldObjectListEntryWriteOwnerHintUnitPtrCount =
      g_worldObjectListEntryWriteOwnerHintUnitPtrCount.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectEntryRenderEntryPtr =
      g_lastWorldObjectEntryRenderEntryPtr.load(std::memory_order_relaxed);
  summary.lastWorldObjectEntryRenderResolvedListOwnerHintValue =
      g_lastWorldObjectEntryRenderResolvedListOwnerHintValue.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectListEntryWriteListPtr =
      g_lastWorldObjectListEntryWriteListPtr.load(std::memory_order_relaxed);
  summary.lastWorldObjectListEntryWriteWorldObjectEntryPtr =
      g_lastWorldObjectListEntryWriteWorldObjectEntryPtr.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectListEntryWriteOwnerHintValue =
      g_lastWorldObjectListEntryWriteOwnerHintValue.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectEntryRenderSceneNodeBeforePtr =
      g_lastWorldObjectEntryRenderSceneNodeBeforePtr.load(
          std::memory_order_relaxed);
  summary.lastWorldObjectEntryRenderSceneNodeAfterPtr =
      g_lastWorldObjectEntryRenderSceneNodeAfterPtr.load(
          std::memory_order_relaxed);
  return summary;
}

} // namespace dxvk::war3::hooks
