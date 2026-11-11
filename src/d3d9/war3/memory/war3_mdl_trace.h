#pragma once
#include <cstdint>
namespace dxvk::war3::memory {
// Explicit developer opt-in only; compile-disabled builds have no caller.
// Does not install/reconfigure StormHook/TLSF or modify returned allocations.
void InstallMdlAllocationTrace(uintptr_t gameBase);
}
