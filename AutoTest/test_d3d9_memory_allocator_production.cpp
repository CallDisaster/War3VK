// Links the actual Win32 d3d9_mem.cpp. Only the log sink is substituted.
#include "../src/d3d9/d3d9_mem.h"
#include "../src/util/log/log.h"
#include <iostream>
#include <stdexcept>
#include <limits>
namespace dxvk { void Logger::err(const std::string& s) { std::cerr << s << '\n'; } }
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); ++checks; } while (0)
int main() {
  unsigned checks = 0;
  try {
#ifndef D3D9_ALLOW_UNMAPPING
    throw std::runtime_error("Requires the shipping 32-bit Win32 allocator branch");
#else
    using namespace dxvk;
    D3D9MemoryAllocator allocator;
    for (unsigned i = 0; i != 3; ++i) CHECK(!allocator.Alloc(0));
    for (uint32_t n = 0; n != 63; ++n) CHECK(!allocator.Alloc(UINT32_MAX - n));
    CHECK(allocator.CaptureDiagnosticSnapshot().chunks.empty());
    for (uint32_t tail : {64u, 128u, 4032u, 4096u, 4160u}) {
      auto large = allocator.Alloc(D3D9ChunkSize - tail);
      CHECK(bool(large)); CHECK(large.GetDiagnosticBinding().alignedSliceBytes == D3D9ChunkSize - tail);
      auto s = allocator.CaptureDiagnosticSnapshot();
      CHECK(s.accountingClosure && s.internalFragmentationBytes == 0 && s.freePayloadBytes == tail);
      auto small = allocator.Alloc(tail);
      CHECK(bool(small)); CHECK(small.GetDiagnosticBinding().chunkId == large.GetDiagnosticBinding().chunkId);
      small.Map(); CHECK(small.Ptr() != nullptr);
      static_cast<unsigned char*>(small.Ptr())[0] = 0x5a;
      CHECK(static_cast<unsigned char*>(small.Ptr())[0] == 0x5a);
      small.Unmap();
      large = {}; small = {};
      s = allocator.CaptureDiagnosticSnapshot();
      CHECK(s.accountingClosure && s.reserveBytes == 0 && s.chunks.empty() && s.mappedBytes == 0);
    }
    // Fragmentation/coalescing with different free orders and aligned payloads.
    for (unsigned turn = 0; turn != 16; ++turn) {
      std::vector<D3D9Memory> allocations;
      for (unsigned i = 1; i != 130; ++i) allocations.emplace_back(allocator.Alloc(i * 67));
      for (unsigned i = turn % 2; i < allocations.size(); i += 2) allocations[i] = {};
      CHECK(allocator.CaptureDiagnosticSnapshot().internalFragmentationBytes == 0);
      allocations.clear();
      CHECK(allocator.CaptureDiagnosticSnapshot().chunks.empty());
    }
#endif
    std::cout << "D3D9 production Win32 checks=" << checks << " failures=0\n"; return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
