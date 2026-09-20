#include "../src/d3d9/war3/tools/war3_async_screenshot_core.h"
#include "../src/d3d9/war3/hooks/war3_native_frame_sync_policy.h"
#include <cassert>
#include <fstream>
#include <thread>
#include <vector>
#include <iostream>

using namespace dxvk::war3::tools::screenshot;
int main(int argc, char** argv) {
  uint64_t bytes = 0;
  assert(Layout(2560, 1440, bytes) && bytes == 14745600);
  assert(Layout(3840, 2160, bytes) && bytes == 33177600);
  for (auto size : {std::array<uint32_t,2>{0,1}, {1,0}, {8193,1},
       {1,8193}, {8192,8192}, {UINT32_MAX,UINT32_MAX}})
    assert(!Layout(size[0], size[1], bytes));

  for (unsigned frame = 0; frame < 10000; ++frame)
    assert(!Readable(true, true, 41, 42));
  assert(Readable(true, true, 42, 42));
  assert(Readable(true, true, 43, 42));
  assert(!Readable(false, true, 42, 42));
  assert(!Readable(true, false, 42, 42));
  assert(!Readable(true, true, 42, 0));

  std::array<std::atomic<State>, SlotCount> slots;
  for (auto& s : slots) s.store(State::Free);
  for (auto& s : slots) assert(Claim(s, State::Free, State::Requested));
  for (auto& s : slots) assert(!Claim(s, State::Free, State::Requested));
  for (auto& s : slots) {
    assert(Claim(s, State::Requested, State::Preparing));
    s.store(State::Submitted);
    assert(!Claim(s, State::Requested, State::Retired)); // Reset cannot free GPU work.
    assert(!Claim(s, State::Free, State::Retired));
    s.store(State::Retired); // GPU + encoder completed, not yet requestable.
    assert(!Claim(s, State::Free, State::Requested));
    assert(Claim(s, State::Retired, State::Preparing)); // Rewarm for current size.
    s.store(State::Free);
  }
  // Exercise the actual release/acquire slot publication with concurrent owners.
  std::atomic<State> state{State::Free};
  unsigned payload = 0;
  std::thread consumer([&] {
    for (unsigned n = 1; n <= 100000; ++n) {
      while (state.load(std::memory_order_acquire) != State::Submitted) std::this_thread::yield();
      assert(payload == n);
      state.store(State::Retired, std::memory_order_release);
    }
  });
  for (unsigned n = 1; n <= 100000; ++n) {
    if (n > 1) {
      while (!Claim(state, State::Retired, State::Preparing)) std::this_thread::yield();
      state.store(State::Free, std::memory_order_release);
    }
    assert(Claim(state, State::Free, State::Requested));
    assert(Claim(state, State::Requested, State::Preparing));
    payload = n;
    state.store(State::Submitted, std::memory_order_release);
  }
  consumer.join();

  // Odd width + padded pitch, top row RGB, bottom row white/black/yellow.
  const uint8_t pixels[] = {0,0,255,0, 0,255,0,7, 255,0,0,1, 99,99,99,99,
      255,255,255,2, 0,0,0,3, 0,255,255,4, 88,88,88,88};
  std::vector<uint8_t> out;
  auto write = [&](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b+n); return true;
  };
  assert(EncodeTga(3, 2, pixels, 16, write, [] {return false;}));
  assert(out.size() == 18+24 && out[17] == 0x28);
  assert(out[18]==0 && out[20]==255 && out[21]==255);
  assert(out[30]==255 && out[31]==255 && out[32]==255);
  for (size_t i=21;i<out.size();i+=4) assert(out[i]==255);
  for (unsigned failAt=0; failAt<3; ++failAt) {
    unsigned writes=0;
    assert(!EncodeTga(3,2,pixels,16,[&](const void*,size_t) {return writes++ != failAt;},[] {return false;}));
    assert(writes == failAt+1);
  }
  unsigned calls=0;
  assert(!EncodeTga(3,2,pixels,16,write,[&] {return ++calls == 3;}));
  assert(!EncodeTga(3,2,pixels,11,write,[] {return false;}));
  assert(!EncodeTga(3,2,pixels,SIZE_MAX,write,[] {return false;}));
  assert(!EncodeTga(0,2,pixels,16,write,[] {return false;}));
  assert(!EncodeTga(3,2,nullptr,16,write,[] {return false;}));

  // Actual normalization routine, complete native callback, both ASLR signs.
  constexpr std::array<uint8_t,27> original = {0x8b,0x15,0x38,0x3a,0xbe,0x6f,
      0xb8,1,0,0,0,0x81,0x39,0x12,2,0,0,0x0f,0x44,0xd0,0x89,0x15,0x38,0x3a,0xbe,0x6f,0xc3};
  constexpr size_t offsets[] = {2,22};
  for (uint32_t base : {0x6f000000u,0x5d120000u,0x72000000u}) {
    auto code = original;
    const uint32_t delta = base-0x6f000000u;
    for (auto offset:offsets) {
      uint32_t v; std::memcpy(&v,code.data()+offset,4); v+=delta;
      std::memcpy(code.data()+offset,&v,4);
    }
    assert(dxvk::war3::hooks::NormalizeNativeFrameCode(code.data(),code.size(),delta,offsets,2));
    assert(code==original);
  }
  if (argc == 2) {
    out.clear(); assert(EncodeTga(3,2,pixels,16,write,[] {return false;}));
    std::ifstream existing(argv[1],std::ios::binary); assert(!existing.good());
    std::ofstream f(argv[1],std::ios::binary); f.write(reinterpret_cast<const char*>(out.data()),out.size());
    assert(f.good());
  }
  std::cout << "async screenshot CPU assertions and 100000 concurrent handoffs passed\n";
}
