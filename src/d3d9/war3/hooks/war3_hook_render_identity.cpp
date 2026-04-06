#include "war3_hook_render_identity.h"

#include "war3_hook_address_book.h"
#include "war3_hook_install_util.h"
#include "../../d3d9_war3_debug.h"
#include "../core/war3_internal_test_config.h"
#include "../render/war3_render_identity_bridge.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_renderer.h"

#include <atomic>

namespace dxvk {

extern std::atomic<bool> g_renderQueueGlobalsValid;
extern uint32_t *g_numOfElementsPtr;
extern void **g_batchArrayPtr;

} // namespace dxvk

namespace dxvk::war3::hooks {

using WorldObjectEntryRenderFn = int(__fastcall *)(void *, int);
using RenderQueueAddBatchFn = void(__fastcall *)(void *, int);

static WorldObjectEntryRenderFn g_originalWorldObjectEntryRender = nullptr;
static WorldObjectEntryRenderFn g_trampolineWorldObjectEntryRender = nullptr;

static RenderQueueAddBatchFn g_originalRenderQueueAddBatch = nullptr;
static RenderQueueAddBatchFn g_trampolineRenderQueueAddBatch = nullptr;

namespace {

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

} // namespace

static int __fastcall Hook_WorldObjectEntry_Render(void *entry, int reserved) {
  if (!dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled || !entry) {
    if (g_trampolineWorldObjectEntryRender)
      return g_trampolineWorldObjectEntryRender(entry, reserved);
    if (g_originalWorldObjectEntryRender)
      return g_originalWorldObjectEntryRender(entry, reserved);
    return 0;
  }

  ScopedWorldObjectContext scope(entry, nullptr);

  if (g_trampolineWorldObjectEntryRender)
    return g_trampolineWorldObjectEntryRender(entry, reserved);
  if (g_originalWorldObjectEntryRender)
    return g_originalWorldObjectEntryRender(entry, reserved);
  return 0;
}

static void __fastcall Hook_RenderQueue_AddBatch(void *sceneNode, int reserved) {
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
    g_trampolineRenderQueueAddBatch(sceneNode, reserved);
  } else if (g_originalRenderQueueAddBatch) {
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

void War3HookRenderIdentity::Install(uintptr_t gameBase) {
  if (!dxvk::war3::internal::kNativeRenderIdentityBridgeEnabled)
    return;

  const auto &book = GetWar3HookAddressBook127a();
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID worldObjectEntryRenderAddr = resolveCode(book.worldObjectEntryRender);
  LPVOID renderQueueAddBatchAddr = resolveCode(book.renderQueueAddBatch);

  g_originalWorldObjectEntryRender =
      reinterpret_cast<WorldObjectEntryRenderFn>(worldObjectEntryRenderAddr);
  g_originalRenderQueueAddBatch =
      reinterpret_cast<RenderQueueAddBatchFn>(renderQueueAddBatchAddr);

  InstallMinHook(
      worldObjectEntryRenderAddr,
      reinterpret_cast<LPVOID>(&Hook_WorldObjectEntry_Render),
      reinterpret_cast<LPVOID *>(&g_trampolineWorldObjectEntryRender), "Render",
      "WorldObjectEntry_Render", false, true);

  InstallMinHook(renderQueueAddBatchAddr,
                 reinterpret_cast<LPVOID>(&Hook_RenderQueue_AddBatch),
                 reinterpret_cast<LPVOID *>(&g_trampolineRenderQueueAddBatch),
                 "Render", "RenderQueue_AddBatch", false, true);
}

} // namespace dxvk::war3::hooks
