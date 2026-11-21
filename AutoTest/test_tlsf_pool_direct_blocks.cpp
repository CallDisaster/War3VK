// CPU test of the TLSF pool's direct blocks (src/d3d9/war3/memory/war3_tlsf_pool.cpp).
//
// Requests above 8 MB that the pools cannot serve get their own VirtualAlloc
// range; nothing else may land in it, and freeing the block must release the
// range. This runs the production pool in a 32-bit test process: no game, no
// Storm, no product DLL. It checks the allocator contract the Storm hook relies
// on (exact-block inspection, ownership, block size, in-place resize, moving
// realloc, free) and that the address space really comes back.
//
// Build (32-bit LLVM, the product's toolchain), from the repository root:
//   i686-w64-mingw32-clang++ -std=c++17 -O2 -DNOMINMAX -Isrc -Isrc/d3d9
//     AutoTest/test_tlsf_pool_direct_blocks.cpp src/d3d9/war3/memory/war3_tlsf_pool.cpp
//     -x c src/d3d9/war3/memory/tlsf.c -o tlsf_direct_test.exe
#include "d3d9/war3/memory/war3_tlsf_pool.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dxvk::war3::memory;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("FAIL %s\n", what);
  }
}

// State of the allocation the address belongs to.
DWORD StateOf(const void *address) {
  MEMORY_BASIC_INFORMATION info = {};
  if (!VirtualQuery(address, &info, sizeof(info)))
    return 0;
  return info.State;
}

bool Inspect(void *block, void *context) {
  *static_cast<void **>(context) = block;
  return true;
}

constexpr size_t kMB = 1024u * 1024u;

} // namespace

int main() {
  Check(TlsfPool_Init(), "init");

  // Fill the 64 MB main pool so the next big request cannot be served there.
  std::vector<void *> filler;
  for (int i = 0; i < 64; ++i) {
    void *p = TlsfPool_AllocAligned(900 * 1024, 16);
    if (!p)
      break;
    filler.push_back(p);
  }
  Check(filler.size() >= 60, "main pool filled with sub-MB blocks");
  const TlsfPoolStats before = TlsfPool_GetStats();

  // 1. A 19 MB request becomes a direct block: 64 KiB aligned, owned, exact.
  const size_t bigSize = 19 * kMB + 12345;
  void *big = TlsfPool_AllocAligned(bigSize, 16);
  Check(big != nullptr, "direct alloc");
  Check((reinterpret_cast<uintptr_t>(big) & 0xFFFFu) == 0u, "direct block 64 KiB aligned");
  Check(reinterpret_cast<uintptr_t>(big) + bigSize <= 0x80000000u, "direct block below 2 GiB");
  Check(TlsfPool_IsFromPool(big), "direct block owned");
  Check(TlsfPool_IsFromPool(static_cast<uint8_t *>(big) + bigSize - 1), "direct block tail owned");
  const size_t bigBlock = TlsfPool_BlockSize(big);
  Check(bigBlock >= bigSize && bigBlock < bigSize + 4096, "direct block size is page-rounded request");
  Check(TlsfPool_BlockSize(static_cast<uint8_t *>(big) + 16) == 0u, "interior pointer is not a block");
  void *seen = nullptr;
  Check(TlsfPool_InspectExactBlock(big, Inspect, &seen) && seen == big, "inspect exact direct block");
  Check(!TlsfPool_InspectExactBlock(static_cast<uint8_t *>(big) + 64, Inspect, &seen),
        "inspect rejects interior pointer");
  std::memset(big, 0x5A, bigSize);
  TlsfPoolStats stats = TlsfPool_GetStats();
  Check(stats.directBlocks == before.directBlocks + 1, "one live direct block");
  Check(stats.directBytes == before.directBytes + bigBlock, "direct bytes counted");
  Check(stats.extendCount == before.extendCount, "no growth pool for the big request");
  Check(stats.totalSize == before.totalSize + bigBlock, "total includes the direct block");
  Check(stats.usedSize == before.usedSize + bigBlock, "used includes the direct block");

  // 2. A small request never lands in the direct block's range.
  void *small = TlsfPool_AllocAligned(70 * 1024, 16);
  Check(small != nullptr, "small alloc after direct");
  const uintptr_t s = reinterpret_cast<uintptr_t>(small);
  const uintptr_t b = reinterpret_cast<uintptr_t>(big);
  Check(s + 70 * 1024 <= b || s >= b + bigBlock, "small block outside the direct range");

  // 3. In-place resize: within the committed range only, data kept.
  Check(TlsfPool_ReallocInPlace(big, bigSize - kMB) == big, "shrink in place");
  Check(TlsfPool_ReallocInPlace(big, bigBlock) == big, "grow in place up to the block size");
  Check(TlsfPool_ReallocInPlace(big, bigBlock + 1) == nullptr, "no in-place growth past the block");
  Check(static_cast<uint8_t *>(big)[bigSize - 1] == 0x5A, "data kept after in-place resize");
  Check(TlsfPool_BlockSize(big) == bigBlock, "block size unchanged by in-place resize");

  // 4. Moving realloc to a larger direct block releases the old range.
  void *moved = TlsfPool_Realloc(big, 22 * kMB);
  Check(moved != nullptr && moved != big, "moving realloc");
  Check(static_cast<uint8_t *>(moved)[0] == 0x5A && static_cast<uint8_t *>(moved)[bigSize - 1] == 0x5A,
        "data moved");
  Check(StateOf(big) == MEM_FREE, "old direct range released after move");
  Check(!TlsfPool_IsFromPool(big), "old direct range no longer owned");
  Check(TlsfPool_BlockSize(big) == 0u, "old pointer no longer a block");
  stats = TlsfPool_GetStats();
  Check(stats.directBlocks == before.directBlocks + 1, "still one live direct block after move");

  // 5. Free releases the range and the accounting.
  Check(TlsfPool_Free(moved), "free direct block");
  Check(StateOf(moved) == MEM_FREE, "direct range released after free");
  Check(!TlsfPool_IsFromPool(moved), "freed direct range not owned");
  Check(!TlsfPool_Free(moved), "double free refused");
  stats = TlsfPool_GetStats();
  Check(stats.directBlocks == before.directBlocks, "no live direct block");
  Check(stats.directBytes == before.directBytes, "direct bytes back");
  Check(stats.totalSize == before.totalSize, "total back");

  // 6. The old failure: a big block's pool kept alive by a small neighbour.
  //    Repeat a growing realloc chain with small allocations in between; every
  //    freed big block must give its range back.
  void *chain = TlsfPool_AllocAligned(19 * kMB, 16);
  std::vector<void *> neighbours;
  size_t chainSize = 19 * kMB;
  for (int step = 0; step < 5 && chain; ++step) {
    neighbours.push_back(TlsfPool_AllocAligned(100 * 1024 + step * 4096, 16));
    chainSize += 2 * kMB;
    void *next = TlsfPool_AllocAligned(chainSize, 16);
    Check(next != nullptr, "chain step alloc");
    if (!next)
      break;
    std::memcpy(next, chain, 4096);
    void *old = chain;
    Check(TlsfPool_Free(old), "chain step free");
    Check(StateOf(old) == MEM_FREE, "chain step range released");
    chain = next;
  }
  stats = TlsfPool_GetStats();
  Check(stats.directBlocks == before.directBlocks + 1, "only the live chain block remains");
  Check(TlsfPool_Free(chain), "free chain");
  for (void *p : neighbours)
    Check(p == nullptr || TlsfPool_Free(p), "free neighbour");
  Check(TlsfPool_Free(small), "free small");

  // 7. A request at or below the threshold still grows a pool, as before.
  void *mid = TlsfPool_AllocAligned(7 * kMB, 16);
  stats = TlsfPool_GetStats();
  Check(mid != nullptr && stats.directBlocks == before.directBlocks, "7 MB request is not direct");
  Check(TlsfPool_Free(mid), "free 7 MB");

  for (void *p : filler)
    TlsfPool_Free(p);
  TlsfPool_Shutdown();
  std::printf("tlsf direct blocks: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
