#include "../tools/render_host/recorder_ingress.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <malloc.h>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace warvk::host::recorder;
static uint64_t checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (false)
static thread_local bool forbidNew = false;
[[gnu::noinline]] void* operator new(std::size_t n) {
  if (forbidNew) std::abort();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return ::operator new(n); }
[[gnu::noinline]] void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t) noexcept { std::free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t a) {
  if (forbidNew) std::abort();
  if (void* p = _aligned_malloc(n ? n : 1, size_t(a))) return p;
  throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n,a); }
[[gnu::noinline]] void operator delete(void* p, std::align_val_t) noexcept { _aligned_free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::align_val_t) noexcept { _aligned_free(p); }
[[gnu::noinline]] void operator delete(void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
[[gnu::noinline]] void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { _aligned_free(p); }
Event sample(unsigned thread, unsigned ordinal) {
  Event e{}; e.thread=thread; e.key={17,ordinal,33,44}; e.parent=ordinal;
  e.qpc=uint64_t(thread)*1000000+ordinal;
  for (size_t i=0;i<e.data.size();++i) e.data[i]=e.qpc+i;
  for (size_t i=0;i<e.bits.size();++i) e.bits[i]=uint32_t(e.qpc)^uint32_t(i*71);
  return e;
}
int main() {
  try {
    static_assert(sizeof(EventIngress<>) < 4*1024*1024, "x86 ingress must remain small");
    EventIngress<8> small(9);
    CHECK(small.tryPush(8,sample(1,1)).outcome==Push::Stale);
    Event out{};out.sequence=999;
    CHECK(small.tryPop(out)==Pop::Empty && out.sequence==999);
    for (unsigned i=1;i<=8;++i) CHECK(small.tryPush(9,sample(1,i)).sequence==i);
    // Consumer is not running: every call must return, no sleep releases it.
    for (unsigned i=0;i<2000;++i) {
      forbidNew=true; auto r=small.tryPush(9,sample(2,i)); forbidNew=false;
      CHECK(r.outcome==Push::Full && !r.sequence);
    }
    CHECK(small.publicationCut()==8);
    for (unsigned i=1;i<=8;++i) CHECK(small.tryPop(out)==Pop::Item && out.sequence==i && out.parent==i);
    CHECK(small.tryPush(9,sample(3,17)).sequence==9);
    small.close(); CHECK(small.tryPush(9,sample(1,1)).outcome==Push::Closed);
    CHECK(small.tryPop(out)==Pop::Item && out.sequence==9);
    CHECK(small.tryPop(out)==Pop::Finished);
    auto c=small.countersAfterJoin();
    CHECK(c.attempted==2009 && c.accepted==9 && c.lost==2000 && c.popped==9 && !c.queued && c.closed);

    EventIngress<8> limited(10,4);
    for (unsigned i=0;i<4;++i) CHECK(limited.tryPush(10,sample(0,i)).outcome==Push::Published);
    CHECK(limited.tryPush(10,sample(0,4)).outcome==Push::Exhausted);
    CHECK(limited.tryPush(10,sample(0,5)).outcome==Push::Closed);
    while(limited.tryPop(out)==Pop::Item) {}
    c=limited.countersAfterJoin(); CHECK(c.attempted==5 && c.accepted==4 && c.lost==1 && c.closed);

    auto queue=std::make_unique<EventIngress<>>(11);
    constexpr unsigned Producers=6, Attempts=30000;
    std::vector<std::thread> threads;
    std::vector<Event> received; received.reserve(Producers*Attempts);
    std::atomic<bool> start{false}, bad{false};
    std::thread consumer([&] {
      while(!start.load()) std::this_thread::yield();
      Event e;
      for (;;) {
        forbidNew=true; const auto r=queue->tryPop(e); forbidNew=false;
        if(r==Pop::Finished) break;
        if(r==Pop::Item) received.push_back(e);
        else std::this_thread::yield();
      }
    });
    for (unsigned t=0;t<Producers;++t) threads.emplace_back([&,t] {
      while(!start.load()) std::this_thread::yield();
      for(unsigned i=0;i<Attempts;++i) {
        forbidNew=true; const auto r=queue->tryPush(11,sample(t,i)); forbidNew=false;
        if(r.outcome!=Push::Published && r.outcome!=Push::Full && r.outcome!=Push::Contended) bad.store(true);
      }
    });
    start.store(true); for(auto& t:threads)t.join(); queue->close(); consumer.join();
    CHECK(!bad.load()); c=queue->countersAfterJoin();
    CHECK(c.attempted==uint64_t(Producers)*Attempts && c.attempted==c.accepted+c.lost);
    CHECK(c.accepted==received.size() && c.popped==c.accepted && !c.queued && c.accepted>0);
    std::array<unsigned,Producers> last{}; std::array<bool,Producers> seen{};
    for(size_t i=0;i<received.size();++i) {
      const auto& e=received[i]; CHECK(e.session==11 && e.sequence==i+1 && e.thread<Producers);
      CHECK(e.key.owner==17 && e.key.mapEpoch==33 && e.key.deviceEpoch==44 && e.key.frame==e.parent);
      CHECK(!seen[e.thread] || e.parent>last[e.thread]); seen[e.thread]=true; last[e.thread]=unsigned(e.parent);
      const auto expected=sample(e.thread,unsigned(e.parent));
      CHECK(e.qpc==expected.qpc && e.data==expected.data && e.bits==expected.bits);
    }
    // New scope is a new object, never reuses old queue/counters/storage state.
    EventIngress<8> next(12); CHECK(next.tryPush(11,sample(0,0)).outcome==Push::Stale);
    CHECK(next.tryPush(12,sample(0,1)).sequence==1);next.close();
    CHECK(next.tryPop(out)==Pop::Item && out.session==12 && out.sequence==1);
    CHECK(next.tryPop(out)==Pop::Finished);

    // Close while actual producer calls are racing admission, then join them
    // before destruction. Closed calls do not masquerade as accepted/lost.
    EventIngress<256> closing(13);
    std::atomic<bool> raceStart{false}, raceBad{false}, finishRace{false};
    std::atomic<uint64_t> closedCalls{0};
    std::vector<std::thread> racers;
    for(unsigned t=0;t<4;++t)racers.emplace_back([&,t] {
      while(!raceStart.load())std::this_thread::yield();
      for(unsigned i=0;i<20000;++i) {
        // Test harness barrier, outside tryPush: the coordinator cannot miss
        // all producer lifetimes if it is temporarily descheduled.
        if(i==1000)while(!finishRace.load())std::this_thread::yield();
        forbidNew=true;auto r=closing.tryPush(13,sample(t,i));forbidNew=false;
        if(r.outcome==Push::Closed)closedCalls.fetch_add(1);
        else if(r.outcome!=Push::Published && r.outcome!=Push::Full && r.outcome!=Push::Contended)raceBad.store(true);
      }
    });
    raceStart.store(true);
    while(closing.publicationCut()<8)std::this_thread::yield();
    closing.close();
    finishRace.store(true);
    for(auto& t:racers)t.join();
    uint64_t drained=0;
    while(closing.tryPop(out)==Pop::Item)CHECK(out.sequence==++drained && out.session==13);
    CHECK(closing.tryPop(out)==Pop::Finished && !raceBad.load());
    const auto closed=closing.countersAfterJoin();
    CHECK(closed.attempted+closedCalls.load()==80000);
    CHECK(closed.accepted==drained && closed.attempted==closed.accepted+closed.lost);
    CHECK(closed.closed && closedCalls.load()>0 && !closed.queued);
    std::cout<<"bits="<<sizeof(void*)*8<<" ingressBytes="<<sizeof(EventIngress<>)
      <<" attempted="<<c.attempted<<" accepted="<<c.accepted<<" lost="<<c.lost<<" checks="<<checks<<" PASS\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
