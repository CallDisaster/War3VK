// Actual ingress -> wire -> HistoryStore handoff, NOT an IPC/game-memory test.
#include "../tools/render_host/recorder_ingress.h"
#include "../tools/render_host/recorder_history_store.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace warvk::host::recorder;
static uint64_t checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(#x); } while (false)

static Event sample(uint64_t n) {
  Event e{}; e.parent=n; e.qpc=0x1234567800000000ULL+n;
  e.thread=uint32_t(n%6); e.key={0x9876543200000000ULL+n,n/3,11,17};
  e.label[0]='e';
  for(size_t i=0;i<e.data.size();++i)e.data[i]=e.qpc+i;
  for(size_t i=0;i<e.bits.size();++i)e.bits[i]=uint32_t(n*73+i);
  e.bits[0]=0x7f800000; e.bits[1]=0x7fc12345; e.bits[2]=0x80000000;
  return e;
}

struct Worker {
  HistoryStore host;
  Header h{};
  std::array<uint8_t,MaxPacketBytes> bytes{};
  std::array<Event,MaxEvents> batch{};
  explicit Worker(uint64_t session) {
    h.session=session; h.capacity=16; h.ordinal=1; h.op=Op::Begin;
    CHECK(send(nullptr)==StoreError::None);
  }
  StoreError send(const Event* events) {
    size_t written=0;
    CHECK(Encode(h,events,bytes.data(),bytes.size(),written)==WireError::None);
    const auto result=host.accept(bytes.data(),written);
    ++h.ordinal;
    return result;
  }
  void drain(EventIngress<8>& q, uint64_t cut=NoTrigger) {
    uint32_t n=0;
    while(n<MaxEvents && q.tryPop(batch[n])==Pop::Item)++n;
    if(!n)return;
    h.op=Op::Data;h.count=n;h.trigger=cut;
    CHECK(send(batch.data())==StoreError::None);
  }
  StoreError seal(const IngressCounters& totals, uint64_t cut) {
    h.op=Op::Seal;h.count=0;h.trigger=cut;h.reason=2;
    h.accepted=totals.accepted;h.attempted=totals.attempted;h.lost=totals.lost;
    return send(nullptr);
  }
};

static void run(bool lateTrigger) {
  constexpr uint64_t Session=0x1020304050607080ULL;
  EventIngress<8> q(Session);
  Worker worker(Session);
  CHECK(q.tryPush(Session-1,sample(0)).outcome==Push::Stale);
  for(uint64_t i=1;i<=20;++i) {
    CHECK(q.tryPush(Session,sample(i)).sequence==i);
    if(i==8) {
      // The producer must return while the consumer has not been resumed.
      for(unsigned n=0;n<30;++n)CHECK(q.tryPush(Session,sample(999)).outcome==Push::Full);
    }
    if(i%8==0)worker.drain(q);
  }
  worker.drain(q);
  for(uint64_t i=21;i<=22;++i)CHECK(q.tryPush(Session,sample(i)).sequence==i);
  const auto cut=q.publicationCut(); CHECK(cut==22);
  for(uint64_t i=23;i<=26;++i)CHECK(q.tryPush(Session,sample(i)).sequence==i);
  q.close();CHECK(q.tryPush(Session,sample(27)).outcome==Push::Closed);
  // One real packet straddles the cut: records 21/22 pre, 23..26 post.
  worker.drain(q,lateTrigger?NoTrigger:cut);
  Event event{};CHECK(q.tryPop(event)==Pop::Finished);
  const auto totals=q.countersAfterJoin();
  CHECK(totals.attempted==56 && totals.accepted==26 && totals.lost==30 && !totals.queued);
  const auto result=worker.seal(totals,cut);
  if(lateTrigger) {
    CHECK(result==StoreError::TriggerRegressed);
    CHECK(worker.host.summary().state==StoreState::Fault);
    CHECK(!worker.host.visitFrozen([](const Event&){return true;}));
    return;
  }
  CHECK(result==StoreError::None);
  const auto s=worker.host.summary();
  CHECK(s.state==StoreState::Frozen && s.session==Session && s.trigger==22);
  CHECK(s.accepted==26 && s.attempted==56 && s.lost==30 && s.evicted==10 && s.retained==16);
  // Frozen is not complete: a caller MUST still reject lost=30 as evidence loss.
  CHECK(s.lost!=0);
  uint64_t next=11;
  CHECK(worker.host.visitFrozen([&](const Event& e) {
    const auto expected=sample(next);
    CHECK(e.sequence==next && e.session==Session && e.parent==next);
    CHECK(e.qpc==expected.qpc && e.thread==expected.thread);
    CHECK(e.key.owner==expected.key.owner && e.key.frame==expected.key.frame);
    CHECK(e.key.mapEpoch==11 && e.key.deviceEpoch==17 && e.label==expected.label);
    CHECK(e.data==expected.data && e.bits==expected.bits);
    ++next; return true;
  }));
  CHECK(next==27);
  CHECK(!worker.host.visitFrozen([](const Event&){return false;}));
  CHECK(worker.host.summary().state==StoreState::Frozen); // Failed visitor is not a successful export.
}

int main() {
  try {
    run(false);run(true);run(false); // Recovery is a new owned session object.
    std::cout<<"scope=CPU_IN_PROCESS_HANDOFF bits="<<sizeof(void*)*8<<" checks="<<checks<<" PASS\n";
    return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
