// war3_memory.cpp - 内存安全读取工具实现
// 从 d3d9_war3_hook.cpp 提取

#include "war3_memory.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace dxvk::war3 {

bool IsReadableProtect(DWORD protect) {
  switch (protect & 0xFF) {
  case PAGE_READONLY:
  case PAGE_READWRITE:
  case PAGE_WRITECOPY:
  case PAGE_EXECUTE_READ:
  case PAGE_EXECUTE_READWRITE:
  case PAGE_EXECUTE_WRITECOPY:
    return true;
  default:
    return false;
  }
}

bool IsExecutableProtect(DWORD protect) {
  switch (protect & 0xFF) {
  case PAGE_EXECUTE:
  case PAGE_EXECUTE_READ:
  case PAGE_EXECUTE_READWRITE:
  case PAGE_EXECUTE_WRITECOPY:
    return true;
  default:
    return false;
  }
}

bool IsReadableRange(const void *p, size_t size) {
  if (!p || size == 0)
    return false;

  const uintptr_t start = reinterpret_cast<uintptr_t>(p);
  const uintptr_t end = start + size;
  if (end < start) // overflow
    return false;

  uintptr_t addr = start;
  size_t remaining = size;

  while (remaining > 0) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
      return false;

    if (mbi.State != MEM_COMMIT)
      return false;

    if (!IsReadableProtect(mbi.Protect))
      return false;

    const uintptr_t regionEnd =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (regionEnd <= addr)
      return false;

    const size_t chunk =
        std::min(remaining, static_cast<size_t>(regionEnd - addr));
    addr += chunk;
    remaining -= chunk;
  }

  return true;
}

bool IsExecutableRange(const void *p, size_t size) {
  if (!p || size == 0)
    return false;

  const uintptr_t start = reinterpret_cast<uintptr_t>(p);
  const uintptr_t end = start + size;
  if (end < start)
    return false;

  uintptr_t addr = start;
  size_t remaining = size;

  while (remaining > 0) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0)
      return false;

    if (mbi.State != MEM_COMMIT)
      return false;

    if (!IsExecutableProtect(mbi.Protect))
      return false;

    const uintptr_t regionEnd =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (regionEnd <= addr)
      return false;

    const size_t chunk =
        std::min(remaining, static_cast<size_t>(regionEnd - addr));
    addr += chunk;
    remaining -= chunk;
  }

  return true;
}

bool SafeCopy(void* destination, const void* source, size_t size) noexcept {
  if (destination == nullptr || source == nullptr || size == 0u)
    return false;

  const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(source);
  const uintptr_t destinationAddress = reinterpret_cast<uintptr_t>(destination);
  if (size > std::numeric_limits<uintptr_t>::max() - sourceAddress ||
      size > std::numeric_limits<uintptr_t>::max() - destinationAddress) {
    return false;
  }

  SIZE_T copied = 0u;
  return ::ReadProcessMemory(::GetCurrentProcess(), source, destination, size,
                             &copied) != FALSE &&
      copied == size;
}

bool SafeEqual(const void* lhs, const void* rhs, size_t size) noexcept {
  if (lhs == nullptr || rhs == nullptr || size == 0u)
    return false;

  const uintptr_t lhsAddress = reinterpret_cast<uintptr_t>(lhs);
  const uintptr_t rhsAddress = reinterpret_cast<uintptr_t>(rhs);
  if (size > std::numeric_limits<uintptr_t>::max() - lhsAddress ||
      size > std::numeric_limits<uintptr_t>::max() - rhsAddress) {
    return false;
  }

  constexpr size_t kChunkSize = 256u;
  std::array<uint8_t, kChunkSize> lhsBytes = {};
  std::array<uint8_t, kChunkSize> rhsBytes = {};
  size_t offset = 0u;
  while (offset < size) {
    const size_t chunk = std::min(kChunkSize, size - offset);
    if (!SafeCopy(lhsBytes.data(),
                  reinterpret_cast<const void*>(lhsAddress + offset), chunk) ||
        !SafeCopy(rhsBytes.data(),
                  reinterpret_cast<const void*>(rhsAddress + offset), chunk) ||
        std::memcmp(lhsBytes.data(), rhsBytes.data(), chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool SafeEqualUntrustedToTrusted(
    const void* untrusted, const void* trustedBytes, size_t size) noexcept {
  if (untrusted == nullptr || trustedBytes == nullptr || size == 0u)
    return false;

  const uintptr_t untrustedAddress =
      reinterpret_cast<uintptr_t>(untrusted);
  const uintptr_t trustedAddress =
      reinterpret_cast<uintptr_t>(trustedBytes);
  if (size > std::numeric_limits<uintptr_t>::max() - untrustedAddress ||
      size > std::numeric_limits<uintptr_t>::max() - trustedAddress) {
    return false;
  }

  // ReadProcessMemory validates this untrusted range. A 2 KiB chunk keeps the
  // exact comparison fail-closed, avoids the 32-bit 4 KiB stack-probe edge,
  // and removes most syscalls; the trusted side is manager-owned storage.
  constexpr size_t kChunkSize = 2048u;
  std::array<uint8_t, kChunkSize> nativeBytes;
  size_t offset = 0u;
  while (offset < size) {
    const size_t chunk = std::min(kChunkSize, size - offset);
    if (!SafeCopy(
            nativeBytes.data(),
            reinterpret_cast<const void*>(untrustedAddress + offset),
            chunk) ||
        std::memcmp(
            nativeBytes.data(),
            reinterpret_cast<const void*>(trustedAddress + offset),
            chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool IsReadableRangeFast(const void *p, size_t size) {
  // [DIAG] 2026-01-22: 临时禁用缓存以诊断崩溃问题
  // 如果禁用缓存后崩溃消失，说明缓存机制导致了误判
  if (!p || size == 0)
    return false;

  const uintptr_t start = reinterpret_cast<uintptr_t>(p);
  const uintptr_t end = start + size;
  if (end < start)
    return false;

  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(reinterpret_cast<LPCVOID>(start), &mbi, sizeof(mbi)) == 0)
    return false;

  const bool readable = (mbi.State == MEM_COMMIT) &&
                        IsReadableProtect(mbi.Protect) &&
                        ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0);

  if (!readable)
    return false;

  const uintptr_t regionEnd =
      reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return end <= regionEnd;
}

} // namespace dxvk::war3
