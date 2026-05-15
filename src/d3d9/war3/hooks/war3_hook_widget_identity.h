// war3_hook_widget_identity.h - CWidget identity sync hook (Phase 7.98)
//
// Phase 7.98: fix the destructible / path-blocker rawcode-missing problem
// at its root by hooking CWidget_RegisterFootprintAndShadowMask @ 0x6F65A140.
//
// Background: destructibles (path blockers like YTab/YTac/YTpb, doodads,
// gates) do NOT pass through Hook_WorldObjects_RenderGroup, which means
// `RenderObjectRegistry` only contains units/buildings each frame. Anything
// that depends on `findByHandle(jHandle)` for destructibles will miss:
//   - path-blocker shadow rejection -> path blocker shadows leak through
//   - outline matching jHandle->rawcode lookup -> destructible outline broken
//   - bloom matching jHandle->rawcode lookup -> destructible bloom broken
//
// The single central sync entry that ALL CWidget-derived lifecycle events
// pass through is CWidget_RegisterFootprintAndShadowMask (30+ callers,
// covering CDestructable_create / CUnit_create / setHidden / setKilled /
// setPosition / morphTo / etc). Hooking that one entry gets us:
//   widget+0x0C  magic 0x2B5DB42C  (CWidget signature)
//   widget+0x30  rawcode (4-byte fourcc)
//   handle resolved via HandleResolver::findHandleByUnitPtr
//
// Cache lifetime: process-wide, additive. War3 does not reuse widget pointers
// across different rawcodes (verified by paper chapter 6 + 24). Even if it
// happened, the next lifecycle hook fire would update the entry.
//
// Hot-path safety: cache reads use std::shared_mutex (multi-reader). Lookups
// only fire when rawcode is missing on the candidate object, so hot-path cost
// is bounded.
//
// Env switch: DXVK_WAR3_WIDGET_IDENTITY_HOOK=0 disables the hook entirely.

#pragma once

#include "../render/war3_render_objects.h"
#include <cstdint>

namespace dxvk::war3::hooks {

// Install the CWidget identity hook on Game.dll+0x65A140.
// Returns true if installed; false if address is null or env-disabled.
bool InstallWidgetIdentityHook(void* widgetRegisterAddr);

// Lookup cached identity by widget pointer (CWidget*/CUnit*/CDestructable*).
bool QueryWidgetIdentityByPtr(
    void* widgetPtr,
    dxvk::war3::render::RenderObjectInfo& out);

// Lookup cached identity by jHandle (with 0x100000 prefix).
bool QueryWidgetIdentityByHandle(
    uint32_t jHandle,
    dxvk::war3::render::RenderObjectInfo& out);

// Fast path: just return the cached rawcode (0 if not cached).
uint32_t QueryWidgetRawcodeByPtr(void* widgetPtr);
uint32_t QueryWidgetRawcodeByHandle(uint32_t jHandle);

// Diagnostics.
uint64_t GetWidgetIdentityCacheSize();

struct WidgetIdentityHookStats {
  uint64_t enterCount = 0;
  uint64_t magicMatchedCount = 0;
  uint64_t magicMismatchCount = 0;
  uint64_t cacheInsertCount = 0;
  uint64_t cacheUpdateCount = 0;
  uint64_t handleResolvedCount = 0;
  uint64_t handleMissingCount = 0;
  // Phase 7.99：install 状态诊断字段（让外部直接知道 hook 装没装上）。
  uint64_t installAttempted = 0;
  uint64_t installSucceeded = 0;
  uint64_t installFailedAddrNull = 0;
  uint64_t installFailedEnvDisabled = 0;
  uint64_t installFailedMinHook = 0;
};

WidgetIdentityHookStats GetWidgetIdentityHookStats();

}  // namespace dxvk::war3::hooks
