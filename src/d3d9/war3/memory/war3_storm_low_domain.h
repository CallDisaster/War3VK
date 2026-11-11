#pragma once

#include "war3_storm_native_small_repair.h"

namespace dxvk::war3::memory {
struct StormLowDomainStats {
  uint32_t enabled, scopes, reserves, highRejected, searchQueries, failures;
  uint32_t preexistingArenas, preexistingHigh, createCalls, createdLow;
  uint32_t preexistingExternalUnproven;
};

// Called only after the existing installer has verified the exact Storm file.
// Prepare does not change hooks/allocator ownership; verifies the native hash.
bool StormLowDomain_Prepare(HMODULE storm, StormNativeSmallRepairHookDesc* createHook);
bool StormLowDomain_PublishImport();
void StormLowDomain_ActivateAfterBatch();
bool StormLowDomain_RollbackImport();
bool StormLowDomain_IsRequested();
// Give up only the optional low-address domain (prepare or import publication
// failed and the import slot is proven restored). The base Storm hooks still
// install; ActivateAfterBatch then leaves the domain disabled.
void StormLowDomain_Disable();
// Preserve the native arena as owner even for target allocations large enough
// for TLSF. Otherwise a later realloc with a different source could lose the
// domain when downgrading/falling back from a source-less managed header.
bool StormLowDomain_RequiresNativeAllocation(const char* source, int32_t line) noexcept;
StormLowDomainStats StormLowDomain_GetStats();

// A native arena owns the domain, not realloc's current source argument. This
// covers copied source names, native realloc, and protect-memory external VA.
class StormLowDomainScope {
public:
  explicit StormLowDomainScope(const StormNativeArena* arena) noexcept;
  StormLowDomainScope(const char* source, int32_t line, uint32_t heapId) noexcept;
  ~StormLowDomainScope();
  StormLowDomainScope(const StormLowDomainScope&) = delete;
  StormLowDomainScope& operator=(const StormLowDomainScope&) = delete;
private:
  bool previous_;
};
} // namespace dxvk::war3::memory
