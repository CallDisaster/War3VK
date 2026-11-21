#pragma once

// war3_proxy_diag.h - the game's end of proxy mode's diagnostics block
// (war3_proxy_diag_block.h): frame evidence (Ctrl+Shift+C), the performance
// log and the numbers of the render statistics panel. The block is created
// when the proxy connects and stays for the life of the process; a host that
// does not open it costs nothing but these features.

#include "war3_proxy_diag_block.h"

#include <cstdint>
#include <string>

namespace dxvk::war3::proxy {

struct DiagStatus {
  bool available = false;     // the block exists
  bool hostAttached = false;  // the host opened it
  bool perfWanted = false;
  int32_t hostPid = 0;
  int32_t evidenceState = 0, evidenceFrames = 0, evidenceMillis = 0;
  int32_t evidenceSaved = 0, evidenceTotal = 0, evidencePackages = 0;
  int32_t perfState = 0, perfRows = 0;
  bool statsValid = false;    // `stats` is one complete copy
  warvk::diag::HostStats stats{};
  std::string outputDirectory, evidencePath, perfPath, error; // UTF-8
};

// When the proxy connects (idempotent).
void DiagConnect() noexcept;
// Any thread. False: no host is there to answer.
bool DiagRequestEvidence() noexcept;
void DiagSetPerfRecording(bool recording) noexcept;
DiagStatus DiagQuery();
// At each Present of the game, on the render thread.
void DiagGameFrame(const warvk::diag::GameFrame& frame) noexcept;

// This process's 32-bit address space. The game's own pointers must stay below
// 2 GiB even in a large-address process, so the low part is what can run out.
struct AddressSpaceSample {
  bool valid = false, largeAddress = false;
  uint64_t lowTotal = 0, lowUsed = 0, lowLargestFree = 0;
  uint64_t highTotal = 0, highUsed = 0, highLargestFree = 0;
  uint64_t privateBytes = 0;  // committed private memory of the process
};
AddressSpaceSample QueryAddressSpace() noexcept;

} // namespace dxvk::war3::proxy
