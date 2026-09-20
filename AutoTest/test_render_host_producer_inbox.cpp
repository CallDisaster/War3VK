#include "../tools/render_host/producer_inbox.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <malloc.h>
#include <thread>
#include <type_traits>

using namespace warvk::host::inbox;
static std::atomic<unsigned> checks{0};
static thread_local bool forbidAllocation = false;
void* operator new(std::size_t size) {
  if (forbidAllocation) std::abort();
  if (auto* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  if(forbidAllocation)std::abort();
#ifdef _WIN32
  if(auto* p=_aligned_malloc(size?size:1,static_cast<size_t>(alignment)))return p;
#else
  const auto a=static_cast<size_t>(alignment);
  if(auto* p=std::aligned_alloc(a,((size?size:1)+a-1)/a*a))return p;
#endif
  throw std::bad_alloc();
}
void operator delete(void* p,std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete[](void* p,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
static void Check(bool condition, unsigned line) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "inbox check failed line=%u\n", line); std::exit(1); }
}
#define CHECK(x) Check(bool(x), __LINE__)
static const Scope ScopeA{{0x1122334455667788ull,0x8877665544332211ull},3,7};
static_assert(!std::is_copy_constructible_v<ProducerInbox> && !std::is_move_constructible_v<ProducerInbox>);
static uint8_t Pattern(uint64_t frame, uint32_t offset) { return uint8_t(frame * 17 + offset * 29 + (offset >> 8)); }
static void Fill(std::array<uint8_t,MaxBytes>& data, uint64_t frame, uint32_t bytes) {
  for (uint32_t i=0;i<bytes;++i) data[i]=Pattern(frame,i);
}
static void Verify(const Sample& sample, const Scope& expected=ScopeA) {
  CHECK(sample.scope==expected);CHECK(sample.bytes>0&&sample.bytes<=MaxBytes);
  bool same=true;for(uint32_t i=0;i<sample.bytes;++i) same &= sample.payload[i]==Pattern(sample.frame,i);
  CHECK(same);
}
static Counters Balanced(const ProducerInbox& queue) {
  const auto c=queue.countersAfterJoin();
  CHECK(uint64_t(c.attempted)==uint64_t(c.published)+c.full+c.invalid);
  CHECK(uint64_t(c.published)==uint64_t(c.popped)+c.queued);
  CHECK(c.queued<=Capacity);return c;
}

static void Core(Scope scope=ScopeA) {
  auto queue=std::make_unique<ProducerInbox>(scope);
  std::array<uint8_t,MaxBytes> source{};Sample sample;
  forbidAllocation=true;
  sample.ordinal=999;CHECK(queue->tryPop(sample)==Pop::Empty);CHECK(sample.ordinal==999);
  for(uint32_t i=1;i<=4;++i){Fill(source,i,MaxBytes);const auto r=queue->tryPush(i,source.data(),MaxBytes);
    CHECK(r.outcome==Push::Published&&r.ordinal==i);}
  for(uint32_t i=5;i<=14;++i){const auto r=queue->tryPush(i,source.data(),1);CHECK(r.outcome==Push::Full&&r.ordinal==i);}
  for(uint32_t i=1;i<=2;++i){CHECK(queue->tryPop(sample)==Pop::Item);CHECK(sample.ordinal==i);Verify(sample,scope);}
  for(uint32_t i=15;i<=16;++i){Fill(source,i,97);const auto r=queue->tryPush(i,source.data(),97);
    CHECK(r.outcome==Push::Published&&r.ordinal==i);}
  queue->closeProducer();CHECK(queue->tryPush(17,source.data(),1).outcome==Push::Closed);
  for(uint32_t i:{3u,4u,15u,16u}){CHECK(queue->tryPop(sample)==Pop::Item);CHECK(sample.ordinal==i);Verify(sample,scope);}
  CHECK(queue->tryPop(sample)==Pop::Finished);CHECK(sample.ordinal==16);
  const auto c=Balanced(*queue);CHECK(c.attempted==16&&c.published==6&&c.full==10&&c.closed&&c.queued==0);
  forbidAllocation=false;

  auto invalid=std::make_unique<ProducerInbox>(scope);forbidAllocation=true;
  CHECK(invalid->tryPush(1,nullptr,1).outcome==Push::Invalid);
  CHECK(invalid->tryPush(0,source.data(),1).outcome==Push::Invalid);
  CHECK(invalid->tryPush(1,source.data(),0).outcome==Push::Invalid);
  CHECK(invalid->tryPush(1,source.data(),MaxBytes+1).outcome==Push::Invalid);
  Fill(source,5,1);CHECK(invalid->tryPush(5,source.data(),1).outcome==Push::Published);
  CHECK(invalid->tryPush(4,source.data(),1).outcome==Push::Invalid);
  CHECK(invalid->tryPop(sample)==Pop::Item);CHECK(sample.ordinal==5);Verify(sample,scope);
  Fill(source,5,MaxBytes);CHECK(invalid->tryPush(5,source.data(),MaxBytes).outcome==Push::Published);
  CHECK(invalid->tryPop(sample)==Pop::Item);CHECK(sample.ordinal==7);Verify(sample,scope);
  invalid->requestStop();CHECK(invalid->tryPush(6,source.data(),1).outcome==Push::Stopped);
  CHECK(invalid->tryPop(sample)==Pop::Empty);invalid->closeProducer();CHECK(invalid->tryPop(sample)==Pop::Finished);
  const auto bad=Balanced(*invalid);CHECK(bad.attempted==7&&bad.invalid==5&&bad.published==2&&bad.stopped);
  forbidAllocation=false;
  for(const auto scope:{Scope{},Scope{{1,0},0,1},Scope{{1,0},1,0}}){
    auto q=std::make_unique<ProducerInbox>(scope);CHECK(q->tryPush(1,source.data(),1).outcome==Push::Unconfigured);
    q->closeProducer();CHECK(q->tryPop(sample)==Pop::Finished);CHECK(Balanced(*q).attempted==0);}
  for(uint32_t limit:{0u,2u}){
    auto q=std::make_unique<ProducerInbox>(scope,Limits{limit,MaxBytes});forbidAllocation=true;
    for(uint32_t i=1;i<=limit;++i){Fill(source,i,4096);CHECK(q->tryPush(i,source.data(),4096).ordinal==i);}
    CHECK(q->tryPush(3,source.data(),1).outcome==Push::Exhausted);
    CHECK(q->tryPush(3,source.data(),1).outcome==Push::Exhausted);
    q->closeProducer();for(uint32_t i=1;i<=limit;++i){CHECK(q->tryPop(sample)==Pop::Item);CHECK(sample.ordinal==i);Verify(sample,scope);}
    CHECK(q->tryPop(sample)==Pop::Finished);const auto c2=Balanced(*q);CHECK(c2.attempted==limit&&c2.exhausted);
    forbidAllocation=false;
  }
  for(uint32_t capacity:{1u,97u,65456u,65536u}){
    auto q=std::make_unique<ProducerInbox>(scope,Limits{UINT32_MAX,capacity});forbidAllocation=true;
    Fill(source,1,capacity);CHECK(q->tryPush(1,source.data(),capacity+1).outcome==Push::Invalid);
    CHECK(q->tryPush(1,source.data(),capacity).outcome==Push::Published);
    q->closeProducer();CHECK(q->tryPop(sample)==Pop::Item);Verify(sample,scope);
    CHECK(sample.bytes==capacity&&sample.ordinal==2);CHECK(q->tryPop(sample)==Pop::Finished);
    const auto c3=Balanced(*q);CHECK(c3.attempted==2&&c3.invalid==1&&c3.published==1);forbidAllocation=false;
  }
  for(uint32_t badLimit:{0u,MaxBytes+1}){
    auto q=std::make_unique<ProducerInbox>(scope,Limits{UINT32_MAX,badLimit});
    CHECK(q->tryPush(1,source.data(),1).outcome==Push::Unconfigured);q->closeProducer();
    CHECK(q->tryPop(sample)==Pop::Finished);CHECK(Balanced(*q).attempted==0);
  }
}

// Producer must complete/join while consumer is deliberately prevented from
// popping. A blocking queue implementation would fail the external test timeout.
static Counters PausedConsumer() {
  auto queue=std::make_unique<ProducerInbox>(ScopeA);
  std::atomic<bool> resume{false};unsigned read=0;
  std::thread consumer([&]{Sample sample;
    while(!resume.load(std::memory_order_acquire))std::this_thread::yield();
    forbidAllocation=true;
    for(;;){auto r=queue->tryPop(sample);if(r==Pop::Finished)break;
      CHECK(r==Pop::Item);Verify(sample);CHECK(sample.ordinal==++read);}
    forbidAllocation=false;
  });
  std::thread producer([&]{std::array<uint8_t,MaxBytes> bytes{};forbidAllocation=true;
    for(unsigned i=1;i<=2000;++i){Fill(bytes,i,32);CHECK(queue->tryPush(i,bytes.data(),32).outcome==(i<=4?Push::Published:Push::Full));}
    queue->closeProducer();forbidAllocation=false;
  });
  producer.join();CHECK(!resume.load());resume.store(true,std::memory_order_release);consumer.join();
  const auto c=Balanced(*queue);CHECK(c.attempted==2000&&c.published==4&&c.full==1996&&c.popped==4&&c.queued==0);
  return c;
}

static Counters Concurrent(uint64_t& byteCount) {
  auto queue=std::make_unique<ProducerInbox>(ScopeA);
  uint64_t receivedBytes=0,sentBytes=0;uint32_t last=0;unsigned received=0;
  std::thread consumer([&]{Sample sample;forbidAllocation=true;
    for(;;){auto r=queue->tryPop(sample);if(r==Pop::Finished)break;
      if(r==Pop::Empty){std::this_thread::yield();continue;} // test consumer, not producer core
      CHECK(sample.ordinal>last&&sample.frame==sample.ordinal);last=sample.ordinal;Verify(sample);
      receivedBytes+=sample.bytes;++received;
    }
    forbidAllocation=false;
  });
  std::thread producer([&]{std::array<uint8_t,MaxBytes> bytes{};forbidAllocation=true;
    for(uint32_t i=1;i<=50000;++i){const auto size=i%32==0?MaxBytes:(i%3==0?1u:97u);Fill(bytes,i,size);
      const auto r=queue->tryPush(i,bytes.data(),size);CHECK(r.ordinal==i);
      CHECK(r.outcome==Push::Published||r.outcome==Push::Full);
      if(r.outcome==Push::Published)sentBytes+=size;
      if(i%16==0)std::this_thread::yield(); // explicit test scheduling, outside tryPush
    }
    queue->closeProducer();forbidAllocation=false;
  });
  producer.join();consumer.join();
  const auto c=Balanced(*queue);CHECK(c.attempted==50000&&c.invalid==0&&c.closed&&c.queued==0);
  CHECK(c.published==received&&received>100);CHECK(sentBytes==receivedBytes);
  CHECK(uint64_t(last)-received+(50000-last)==c.full);byteCount=receivedBytes;return c;
}

static void ConsumerStop() {
  auto queue=std::make_unique<ProducerInbox>(ScopeA);std::atomic<bool> ready{false},resume{false};
  std::thread producer([&]{std::array<uint8_t,MaxBytes> bytes{};forbidAllocation=true;
    for(uint32_t i=1;i<=4;++i){Fill(bytes,i,97);CHECK(queue->tryPush(i,bytes.data(),97).outcome==Push::Published);}
    ready.store(true,std::memory_order_release);
    while(!resume.load(std::memory_order_acquire))std::this_thread::yield(); // test barrier only
    CHECK(queue->tryPush(5,bytes.data(),97).outcome==Push::Stopped);
    queue->closeProducer();forbidAllocation=false;
  });
  std::thread consumer([&]{Sample sample;
    while(!ready.load(std::memory_order_acquire))std::this_thread::yield();
    forbidAllocation=true;CHECK(queue->tryPop(sample)==Pop::Item);Verify(sample);CHECK(sample.ordinal==1);
    queue->requestStop();resume.store(true,std::memory_order_release);uint32_t read=1;
    for(;;){const auto r=queue->tryPop(sample);if(r==Pop::Finished)break;
      if(r==Pop::Empty){std::this_thread::yield();continue;}
      Verify(sample);CHECK(sample.ordinal==++read);}
    CHECK(read==4);forbidAllocation=false;
  });
  producer.join();consumer.join();const auto c=Balanced(*queue);
  CHECK(c.closed&&c.stopped&&c.attempted==4&&c.popped==4&&c.queued==0);
}

int main(){
  Core();const auto pause=PausedConsumer();uint64_t bytes=0;const auto concurrent=Concurrent(bytes);
  ConsumerStop();
  // A fresh independently scoped queue is still valid after terminal failures.
  Core(Scope{{0xabcdef1234ull,0x99887766ull},4,8});
  std::printf("{\"ok\":true,\"bits\":%zu,\"checks\":%u,\"queueBytes\":%zu,\"sampleBytes\":%zu,"
    "\"atomicBytes\":%zu,\"lockFree\":true,\"pausedAttempts\":%u,\"pausedPublished\":%u,\"pausedFull\":%u,"
    "\"concurrentAttempts\":%u,\"concurrentPublished\":%u,\"concurrentFull\":%u,\"concurrentBytes\":%llu,"
    "\"cppAllocationForbiddenInQueueCalls\":true,\"consumerStopDrained\":true,\"freshScopeRecovery\":true,\"gpuTested\":false,\"ipcTested\":false}\n",
    sizeof(void*)*8,checks.load(),sizeof(ProducerInbox),sizeof(Sample),sizeof(std::atomic<uint32_t>),
    pause.attempted,pause.published,pause.full,concurrent.attempted,concurrent.published,concurrent.full,
    static_cast<unsigned long long>(bytes));
}
