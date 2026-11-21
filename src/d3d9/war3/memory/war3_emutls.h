// war3_emutls.h
// The product DLL's own emulated thread-local storage runtime (war3_emutls.cpp).
// Only tests and diagnostics use this header; compiled code reaches the runtime
// through __emutls_get_address, which clang calls for every thread_local access
// when the DLL is built with -femulated-tls.
#pragma once

#include <cstdint>

namespace dxvk::war3::memory {

struct EmutlsStats {
  uint32_t tlsIndex = 0;        // TLS_OUT_OF_INDEXES before the first access
  uint32_t variables = 0;       // indices handed out (one per thread_local)
  uint32_t liveThreads = 0;     // threads that own storage now
  uint32_t liveObjects = 0;
  uint64_t liveBytes = 0;       // requested sizes, without alignment slack
  uint32_t threadsReleased = 0; // storage freed at thread exit
  uint32_t lateAccesses = 0;    // accesses on a thread after its storage was freed
  uint32_t unloadReleases = 0;  // FreeLibrary releases (process termination keeps all)
};

// Counters of this image's runtime; true when the runtime is built in.
bool EmutlsQueryStats(EmutlsStats& stats) noexcept;

// What the image's last static destructor does at DLL_PROCESS_DETACH. With
// processTerminating it releases nothing (code that runs after this DLL's
// detach keeps working); otherwise (FreeLibrary) it frees every thread's
// storage and the TLS index. Tests call it directly.
void EmutlsReleaseForUnload(bool processTerminating) noexcept;

} // namespace dxvk::war3::memory
