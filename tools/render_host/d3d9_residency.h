#pragma once
#include "../../src/util/thread.h"
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace warvk::host::d3d9proxy {
// x86 copies of evictable content: one buffer or one texture level.
// Once the host retains authoritative content (Initialize RetainContent), an
// unlocked copy is budgeted least-recently-released; a locked copy is pinned.
// An index buffer is also pinned while a draw validates its indices.
struct ContentOwner {
  virtual void contentEvicted() noexcept=0;
protected:
  ~ContentOwner()=default;
};
struct ResidentContent {
  ResidentContent* older=nullptr;ResidentContent* newer=nullptr;bool listed=false;
  std::vector<uint8_t>* bytes=nullptr;ContentOwner* owner=nullptr;
};
class Residency final {
public:
  struct Stats {
    uint64_t residentBytes=0,peakResidentBytes=0,listedBytes=0,peakPinnedBytes=0,maxListedAfterTrim=0;
    uint64_t allocations=0,evictions=0,evictedBytes=0,fetches=0,fetchCalls=0,fetchedBytes=0,discardMisses=0;
  };
  void configure(uint64_t budget) {std::lock_guard<dxvk::mutex> guard(mutex);enabledMode=true;budgetBytes=budget;}
  bool enabled() const {std::lock_guard<dxvk::mutex> guard(mutex);return enabledMode;}
  void allocated(uint64_t bytes) {
    std::lock_guard<dxvk::mutex> guard(mutex);stats.residentBytes+=bytes;++stats.allocations;
    stats.peakResidentBytes=std::max(stats.peakResidentBytes,stats.residentBytes);
  }
  void fetched(uint64_t bytes,uint64_t calls) {
    std::lock_guard<dxvk::mutex> guard(mutex);++stats.fetches;stats.fetchCalls+=calls;stats.fetchedBytes+=bytes;
  }
  void discardMiss() {std::lock_guard<dxvk::mutex> guard(mutex);++stats.discardMisses;}
  // Lock: the copy stays allocated until its Unlock or destruction.
  void pin(ResidentContent& content) {
    std::lock_guard<dxvk::mutex> guard(mutex);if(!enabledMode)return;unlink(content);
    stats.peakPinnedBytes=std::max(stats.peakPinnedBytes,stats.residentBytes-stats.listedBytes);
  }
  // Unlock: newest unpinned copy, then the oldest are evicted beyond budget.
  void unpin(ResidentContent& content) {
    std::lock_guard<dxvk::mutex> guard(mutex);if(!enabledMode||content.listed)return;
    content.older=newest;content.newer=nullptr;content.listed=true;
    (newest?newest->newer:oldest)=&content;newest=&content;stats.listedBytes+=content.bytes->size();
    while(stats.listedBytes>budgetBytes&&oldest) {
      auto& victim=*oldest;const auto bytes=victim.bytes->size();
      unlink(victim);std::vector<uint8_t>().swap(*victim.bytes);
      stats.residentBytes-=bytes;++stats.evictions;stats.evictedBytes+=bytes;victim.owner->contentEvicted();
    }
    stats.maxListedAfterTrim=std::max(stats.maxListedAfterTrim,stats.listedBytes);
  }
  // Destruction or a failed restore: forget the copy and free its bytes.
  void discard(ResidentContent& content) noexcept {
    std::lock_guard<dxvk::mutex> guard(mutex);unlink(content);
    stats.residentBytes-=content.bytes->size();std::vector<uint8_t>().swap(*content.bytes);
  }
  Stats snapshot(uint64_t& budget,bool& enabled) const {
    std::lock_guard<dxvk::mutex> guard(mutex);budget=enabledMode?budgetBytes:0;enabled=enabledMode;return stats;
  }
private:
  void unlink(ResidentContent& content) noexcept {
    if(!content.listed)return;
    (content.older?content.older->newer:oldest)=content.newer;
    (content.newer?content.newer->older:newest)=content.older;
    content.older=content.newer=nullptr;content.listed=false;stats.listedBytes-=content.bytes->size();
  }
  mutable dxvk::mutex mutex;
  bool enabledMode=false;uint64_t budgetBytes=0;
  ResidentContent* oldest=nullptr;ResidentContent* newest=nullptr;
  Stats stats;
};
} // namespace warvk::host::d3d9proxy
