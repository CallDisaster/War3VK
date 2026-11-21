#pragma once
#include "../tools/render_host/d3d9_upload_window.h"
// Write-only Lock windows: reuse, zero fill, the idle budget and its
// least-recently-used trim, independently of any COM object or IPC.
namespace warvk_upload_window_test {
template<class Check> void Run(Check check) {
  using namespace warvk::host::d3d9proxy;
  const uint32_t granule=UploadWindows::Granule;
  uint64_t budget=7;bool enabled=true;
  auto zero=[](const uint8_t* p,uint32_t bytes){for(uint32_t i=0;i<bytes;++i)if(p[i])return false;return true;};
  // The product profile: a 32 MiB residency cap and one idle 1 MiB window.
  check(DefaultBackingKiB==32768&&DefaultUploadWindowKiB==1024&&granule==65536);
  // Disabled: nothing is lent.
  {
    UploadWindows w;check(!w.enabled()&&!w.acquire(16));
    const auto s=w.snapshot(budget,enabled);check(!enabled&&!budget&&!s.acquires&&!s.allocations&&!s.outstanding);
  }
  {
    UploadWindows w;w.configure(2*granule);check(w.enabled()&&!w.acquire(0));
    // One idle window is reused and zero-filled again for its next borrower.
    auto* a=w.acquire(100);check(a&&zero(a,100));std::memset(a,0xee,100);w.release(a);
    auto* b=w.acquire(60000);check(b==a&&zero(b,60000));std::memset(b,0xdd,60000);w.release(b);
    auto s=w.snapshot(budget,enabled);
    check(enabled&&budget==2*granule&&s.acquires==2&&s.allocations==1&&s.reuses==1&&s.releases==2&&!s.trims
      &&s.idleBytes==granule&&s.requestedBytes==60100&&!s.outstanding&&s.peakOutstanding==1&&s.peakOutstandingBytes==granule);
    // No idle window holds the request: a new one, rounded up to the granule.
    auto* c=w.acquire(granule+1);check(c&&c!=a&&zero(c,granule+1));w.release(c);
    // 1 + 2 granules exceed the budget: the least recently released one goes.
    s=w.snapshot(budget,enabled);
    check(s.allocations==2&&s.trims==1&&s.trimmedBytes==granule&&s.idleBytes==2*granule&&s.peakIdleBytes==2*granule);
    // Two outstanding at once: the application's own concurrency is not bounded here.
    auto* d=w.acquire(10);auto* e=w.acquire(10);check(d==c&&e&&e!=d);
    s=w.snapshot(budget,enabled);
    check(s.outstanding==2&&s.outstandingBytes==3*granule&&s.peakOutstanding==2&&s.peakOutstandingBytes==3*granule&&!s.idleBytes);
    w.release(d);w.release(e);
    s=w.snapshot(budget,enabled);check(s.trims==2&&s.trimmedBytes==3*granule&&s.idleBytes==granule&&!s.outstanding);
    // 1 + 2 granules again: the older single granule leaves.
    auto* f=w.acquire(granule+7);check(f&&f!=e);w.release(f);
    s=w.snapshot(budget,enabled);check(s.trims==3&&s.idleBytes==2*granule);
    auto* g=w.acquire(8);check(g==f);auto* h=w.acquire(8);check(h&&h!=g);w.release(h);w.release(g);
    s=w.snapshot(budget,enabled);check(s.idleBytes==2*granule&&s.trims==4); // h's granule left: released first.
    auto* small=w.acquire(8);check(small==g);w.release(small);
    // A window the budget could never hold is freed alone; the idle one stays.
    auto* big=w.acquire(5*granule);check(big&&big!=g);w.release(big);
    s=w.snapshot(budget,enabled);
    check(s.idleBytes==2*granule&&s.trims==5&&s.peakIdleBytes==2*granule&&s.releases==s.acquires&&!s.outstanding&&!s.outstandingBytes);
    auto* again=w.acquire(8);check(again==g);w.release(again);
    // An unknown or already idle pointer changes nothing.
    uint8_t other=0;w.release(&other);w.release(again);
    const auto t=w.snapshot(budget,enabled);check(t.releases==s.releases+1&&t.idleBytes==s.idleBytes&&t.trims==s.trims);
  }
  // Best fit: the smallest idle window that holds the request.
  {
    UploadWindows w;w.configure(4*granule);
    auto* two=w.acquire(granule+1);auto* one=w.acquire(8);check(two&&one&&two!=one);w.release(two);w.release(one);
    auto* a=w.acquire(8);check(a==one);auto* b=w.acquire(8);check(b==two);w.release(a);w.release(b);
    auto* c=w.acquire(granule+1);check(c==two);w.release(c);
    const auto s=w.snapshot(budget,enabled);check(s.allocations==2&&s.reuses==3&&!s.trims&&s.idleBytes==3*granule);
  }
  // Zero budget: nothing is kept between Locks.
  {
    UploadWindows w;w.configure(0);
    auto* a=w.acquire(8);check(a&&zero(a,8));w.release(a);auto* b=w.acquire(8);check(b&&zero(b,8));w.release(b);
    const auto s=w.snapshot(budget,enabled);
    check(enabled&&!budget&&s.allocations==2&&!s.reuses&&s.trims==2&&!s.idleBytes&&!s.peakIdleBytes&&s.trimmedBytes==2*granule);
  }
}
}
