#pragma once

namespace dxvk::war3::memory {

// Exact 1.27a Storm #405 is NOT C realloc: size zero can return a live
// zero-byte allocation. Native small blocks shrink in place; external blocks
// reject NOMOVE or allocate a new zero-byte block before freeing the old one.
// A null return alone therefore proves no release. Share this fact between
// production accounting and the bounded MDLDATA observer.
inline bool NativeReallocReleasedOldPointer(const void* oldPtr,
                                          const void* result) noexcept {
  return oldPtr && result && result != oldPtr;
}

} // namespace dxvk::war3::memory
