#include "../war3_crash_capture_policy.h"

#include <cassert>

int main() {
  using namespace dxvk::war3dbg;

  War3CrashCaptureCoordinator coordinator;
  auto snapshot = coordinator.firstChanceSnapshot();
  assert(snapshot.count == 0u);

  coordinator.recordFirstChance(0xC0000005u, uintptr_t(0x12345678u), 17u);
  snapshot = coordinator.firstChanceSnapshot();
  assert(snapshot.count == 1u);
  assert(snapshot.exceptionCode == 0xC0000005u);
  assert(snapshot.exceptionAddress == uintptr_t(0x12345678u));
  assert(snapshot.threadId == 17u);

  // A handled first-chance exception must never consume or block the fatal gate.
  assert(coordinator.tryBeginFatalCapture());
  assert(!coordinator.tryBeginFatalCapture());

  // Observations after the fatal gate was claimed remain bounded scalar updates.
  coordinator.recordFirstChance(0xC000001Du, uintptr_t(0x87654321u), 23u);
  snapshot = coordinator.firstChanceSnapshot();
  assert(snapshot.count == 2u);
  assert(snapshot.exceptionCode == 0xC000001Du);
  assert(snapshot.exceptionAddress == uintptr_t(0x87654321u));
  assert(snapshot.threadId == 23u);
  return 0;
}
