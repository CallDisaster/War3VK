#pragma once
#include "../../src/util/thread.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace warvk::host::d3d9proxy {
// The x86 memory profile the product owner starts from. A resident copy exists
// only for content the application reads back or partly rewrites, so the
// residency budget is a cap, not a steady cost; one idle window covers the
// largest Lock the game takes (a 512x512 level, 1 MiB). The development proxy
// stays opt-in: unset variables keep the legacy full copies.
constexpr uint32_t DefaultBackingKiB=32768,DefaultUploadWindowKiB=1024;

// Write-only Lock storage (WARVK_D3D9_UPLOAD_WINDOW_KIB, with a residency
// budget). A Lock whose range holds no byte the application has defined needs
// neither the old content nor a copy afterwards: it borrows a zero-filled
// window, Unlock sends it and returns it. Idle windows are kept, least
// recently used first out, up to the budget; a locked window belongs to the
// application until its Unlock, so only the idle side is bounded here.
class UploadWindows final {
public:
  struct Stats {
    uint64_t acquires=0,allocations=0,reuses=0,releases=0,trims=0,trimmedBytes=0,requestedBytes=0;
    uint64_t idleBytes=0,peakIdleBytes=0,outstandingBytes=0,peakOutstandingBytes=0;
    uint32_t outstanding=0,peakOutstanding=0;
  };
  static constexpr uint32_t Granule=65536;
  void configure(uint64_t budget) {std::lock_guard<dxvk::mutex> guard(mutex);enabledMode=true;budgetBytes=budget;}
  bool enabled() const {std::lock_guard<dxvk::mutex> guard(mutex);return enabledMode;}
  // Zero-filled storage for `bytes`; nullptr when disabled, empty or out of memory.
  uint8_t* acquire(uint32_t bytes) noexcept {
    std::lock_guard<dxvk::mutex> guard(mutex);
    if(!enabledMode||!bytes||bytes>UINT32_MAX-Granule)return nullptr;
    Block* fit=nullptr;
    for(auto& block:blocks)
      if(!block.busy&&block.capacity>=bytes&&(!fit||block.capacity<fit->capacity))fit=&block;
    if(fit) {
      stats.idleBytes-=fit->capacity;++stats.reuses;
    } else {
      const uint32_t capacity=(bytes+Granule-1)/Granule*Granule;
      std::unique_ptr<uint8_t[]> data(new(std::nothrow) uint8_t[capacity]);
      if(!data)return nullptr;
      try{blocks.push_back({std::move(data),capacity,0,false});}catch(...){return nullptr;}
      fit=&blocks.back();++stats.allocations;
    }
    fit->busy=true;std::memset(fit->data.get(),0,bytes);
    ++stats.acquires;stats.requestedBytes+=bytes;stats.outstandingBytes+=fit->capacity;++stats.outstanding;
    stats.peakOutstandingBytes=std::max(stats.peakOutstandingBytes,stats.outstandingBytes);
    stats.peakOutstanding=std::max(stats.peakOutstanding,stats.outstanding);
    return fit->data.get();
  }
  // Back to the idle side. A window the budget could never hold is freed at
  // once; otherwise the least recently used go until the idle side fits.
  void release(uint8_t* data) noexcept {
    std::lock_guard<dxvk::mutex> guard(mutex);
    size_t at=0;
    while(at<blocks.size()&&(blocks[at].data.get()!=data||!blocks[at].busy))++at;
    if(at==blocks.size())return;
    const auto capacity=blocks[at].capacity;
    blocks[at].busy=false;blocks[at].stamp=++clock;++stats.releases;
    stats.outstandingBytes-=capacity;--stats.outstanding;stats.idleBytes+=capacity;
    auto trim=[&](size_t index) {
      stats.idleBytes-=blocks[index].capacity;stats.trimmedBytes+=blocks[index].capacity;++stats.trims;
      blocks.erase(blocks.begin()+ptrdiff_t(index));
    };
    if(capacity>budgetBytes)trim(at);
    while(stats.idleBytes>budgetBytes) {
      size_t oldest=blocks.size();
      for(size_t i=0;i<blocks.size();++i)
        if(!blocks[i].busy&&(oldest==blocks.size()||blocks[i].stamp<blocks[oldest].stamp))oldest=i;
      trim(oldest);
    }
    stats.peakIdleBytes=std::max(stats.peakIdleBytes,stats.idleBytes);
  }
  Stats snapshot(uint64_t& budget,bool& enabled) const {
    std::lock_guard<dxvk::mutex> guard(mutex);budget=enabledMode?budgetBytes:0;enabled=enabledMode;return stats;
  }
private:
  struct Block { std::unique_ptr<uint8_t[]> data;uint32_t capacity=0;uint64_t stamp=0;bool busy=false; };
  mutable dxvk::mutex mutex;
  bool enabledMode=false;uint64_t budgetBytes=0,clock=0;
  std::vector<Block> blocks;
  Stats stats;
};
} // namespace warvk::host::d3d9proxy
