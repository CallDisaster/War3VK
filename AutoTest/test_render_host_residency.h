#pragma once
#include "../tools/render_host/d3d9_residency.h"
#include <array>
// Budgeted least-recently-unlocked order, pinning and release accounting,
// independently of any COM object or IPC.
namespace warvk_residency_test {
struct Owner final : warvk::host::d3d9proxy::ContentOwner {
  unsigned evicted=0;
  void contentEvicted() noexcept override {++evicted;}
};
template<class Check> void Run(Check check) {
  using namespace warvk::host::d3d9proxy;
  uint64_t budget=0;bool enabled=false;
  // Legacy mode: counted, never listed, pinned or evicted.
  {
    Residency r;std::vector<uint8_t> bytes(100);Owner owner;ResidentContent item;item.bytes=&bytes;item.owner=&owner;
    r.allocated(100);r.pin(item);r.unpin(item);budget=7;enabled=true;auto s=r.snapshot(budget,enabled);
    check(!enabled&&!budget&&s.residentBytes==100&&!s.listedBytes&&!s.evictions&&!s.peakPinnedBytes&&bytes.size()==100);
    r.discard(item);s=r.snapshot(budget,enabled);
    check(!s.residentBytes&&bytes.empty()&&!bytes.capacity()&&s.peakResidentBytes==100&&!owner.evicted);
  }
  // 250-byte budget over 100-byte copies: the oldest unlocked copy leaves first.
  {
    Residency r;r.configure(250);
    std::array<std::vector<uint8_t>,4> bytes;std::array<Owner,4> owners;std::array<ResidentContent,4> items;
    for(size_t i=0;i<items.size();++i){items[i].bytes=&bytes[i];items[i].owner=&owners[i];}
    auto use=[&](size_t i,uint64_t size){if(bytes[i].empty()){bytes[i].resize(size);r.allocated(size);}r.pin(items[i]);r.unpin(items[i]);};
    use(0,100);use(1,100);check(!owners[0].evicted&&!owners[1].evicted);
    use(2,100);check(owners[0].evicted==1&&bytes[0].empty()&&!owners[1].evicted&&!owners[2].evicted);
    use(1,100); // Touch: order is now 2, 1.
    use(3,100);check(owners[2].evicted==1&&bytes[2].empty()&&!owners[1].evicted&&bytes[1].size()==100);
    // A pinned copy is never evicted; one newly unlocked copy larger than the
    // whole budget evicts everything else unpinned, then itself.
    r.pin(items[1]);bytes[0].resize(400);r.allocated(400);r.pin(items[0]);r.unpin(items[0]);
    check(owners[3].evicted==1&&owners[0].evicted==2&&bytes[0].empty()&&bytes[3].empty()&&bytes[1].size()==100);
    auto s=r.snapshot(budget,enabled);
    check(enabled&&budget==250&&s.residentBytes==100&&!s.listedBytes&&s.maxListedAfterTrim==200
      &&s.evictions==4&&s.evictedBytes==700&&s.peakPinnedBytes==500&&s.peakResidentBytes==600&&s.allocations==5);
    r.unpin(items[1]);r.unpin(items[1]);s=r.snapshot(budget,enabled);check(s.listedBytes==100&&owners[1].evicted==0);
    r.discard(items[1]);r.discard(items[1]);s=r.snapshot(budget,enabled);
    check(!s.residentBytes&&!s.listedBytes&&bytes[1].empty());
    for(auto& item:items)r.discard(item);
    s=r.snapshot(budget,enabled);check(!s.residentBytes&&!s.listedBytes&&s.evictions==4);
  }
  // Zero budget: every unlocked copy leaves at once; restore counters are separate.
  {
    Residency r;r.configure(0);std::vector<uint8_t> bytes(64);Owner owner;ResidentContent item;item.bytes=&bytes;item.owner=&owner;
    r.allocated(64);r.pin(item);r.unpin(item);check(bytes.empty()&&owner.evicted==1);
    r.fetched(64,1);r.discardMiss();budget=1;auto s=r.snapshot(budget,enabled);
    check(enabled&&!budget&&s.fetches==1&&s.fetchCalls==1&&s.fetchedBytes==64&&s.discardMisses==1
      &&!s.residentBytes&&!s.maxListedAfterTrim&&s.peakPinnedBytes==64);
  }
}
}
