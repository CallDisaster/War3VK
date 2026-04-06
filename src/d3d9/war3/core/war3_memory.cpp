// war3_memory.cpp - 内存安全读取工具实现
// 从 d3d9_war3_hook.cpp 提取

#include "war3_memory.h"
#include <algorithm>

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
