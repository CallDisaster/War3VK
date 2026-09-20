#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace dxvk::war3::tools::evidence {

enum class State : uint32_t { Idle, Armed, Triggered, Frozen };
enum class Reason : uint32_t { None, Requested, PostWindow, Capacity, SequenceWrap };
enum class Kind : uint32_t { PresentBegin=1, PresentEnd, PipelineBegin, PipelineEnd,
  PassBegin, PassEnd, Camera, Trigger, CasterInput, CasterBinding, CasterOmitted,
  ShadowState, ScreenshotPrepared, ScreenshotSaved, ScreenshotCopyRecorded,HistoryCopyRecorded,CsmState,DirectionalDraw };
struct Key {
  uint64_t owner=0, frame=0, mapEpoch=0, deviceEpoch=0;
};
struct Event {
  uint64_t sequence=0, session=0, parent=0, qpc=0;
  Key key;
  uint32_t thread=0;
  Kind kind=Kind::Camera;
  std::array<char,32> label{};
  std::array<uint64_t,12> data{};
  std::array<uint32_t,48> bits{};
};
struct Snapshot {
  State state=State::Idle;
  Reason reason=Reason::None;
  uint64_t session=0, accepted=0, evicted=0, triggerSequence=0;
  uint32_t capacity=0, postRemaining=0;
  std::vector<Event> events;
  uint64_t reserved=0;
};

// Control methods are externally serialized. Producers claim DIFFERENT cells,
// not one shared mutex. A preempted writer's cell is never overwritten: a lap
// collision is explicitly rejected. Freeze + writer drain protects export.
class Ring {
public:
  static constexpr uint64_t storageBytes(uint32_t capacity) noexcept {
    return uint64_t(capacity) * sizeof(Cell);
  }
  bool arm(uint64_t session, uint32_t capacity) {
    if (m_state.load()!=State::Idle || !session || !capacity || capacity>262144 || m_writers.load()) return false;
    auto storage=std::make_unique<Cell[]>(capacity);
    m_storage.swap(storage);m_capacity=capacity;m_pre=capacity-capacity/4;
    m_session=session;m_accepted=0;m_evicted=0;m_reserved=0;m_trigger=UINT64_MAX;
    m_remaining=0;m_triggerClaim=false;m_reason=Reason::None;m_state.store(State::Armed);
    return true;
  }
  uint64_t append(uint64_t token, Event event) noexcept {
    // The writer gate and state transitions share the seq_cst order: a writer
    // arriving after a zero-writer frozen snapshot cannot see old Armed state.
    m_writers.fetch_add(1);
    struct Exit{std::atomic<uint32_t>& n;~Exit(){n.fetch_sub(1);}} exit{m_writers};
    const auto state=m_state.load();
    if(token!=m_session.load() || (state!=State::Armed&&state!=State::Triggered))return 0;
    auto previous=m_reserved.load(std::memory_order_relaxed);
    do {if(previous==UINT64_MAX){freeze(Reason::SequenceWrap);return 0;}}
    while(!m_reserved.compare_exchange_weak(previous,previous+1,std::memory_order_relaxed));
    const uint64_t sequence=previous+1;
    const uint64_t trigger=m_trigger.load(std::memory_order_acquire);
    const bool post=trigger!=UINT64_MAX&&sequence>trigger;
    const uint64_t index=post?m_pre+(sequence-trigger-1):(sequence-1)%m_pre;
    if(index>=m_capacity){freeze(Reason::Capacity);return 0;}
    auto& cell=m_storage[size_t(index)];
    if(cell.busy.test_and_set(std::memory_order_acquire))return 0;
    // An older preempted ticket must not replace a newer lap already published.
    if(cell.event.sequence>=sequence){cell.busy.clear(std::memory_order_release);return 0;}
    if(cell.event.sequence)m_evicted.fetch_add(1,std::memory_order_relaxed);
    event.sequence=sequence;event.session=token;cell.event=event;
    m_accepted.fetch_add(1,std::memory_order_release);
    cell.busy.clear(std::memory_order_release);
    if(post&&event.kind==Kind::PresentEnd){
      auto remaining=m_remaining.load();
      while(remaining&&!m_remaining.compare_exchange_weak(remaining,remaining-1)){}
      if(remaining==1)freeze(Reason::PostWindow);
    }
    return sequence;
  }
  bool trigger(uint64_t token, uint32_t postPresents) noexcept {
    m_writers.fetch_add(1);
    struct Exit{std::atomic<uint32_t>& n;~Exit(){n.fetch_sub(1);}} exit{m_writers};
    if(m_state.load()!=State::Armed||token!=m_session.load()||postPresents>120)return false;
    bool unclaimed=false;
    if(!m_triggerClaim.compare_exchange_strong(unclaimed,true))return false;
    m_remaining=postPresents;
    m_trigger.store(m_reserved.load(),std::memory_order_release);
    State expected=State::Armed;
    if(!m_state.compare_exchange_strong(expected,State::Triggered))return expected==State::Frozen;
    if(!postPresents)freeze(Reason::Requested);
    return true;
  }
  bool finish(uint64_t token) noexcept {
    auto state=m_state.load();
    if(token!=m_session.load()||(state!=State::Armed&&state!=State::Triggered))return false;
    freeze(Reason::Requested); return true;
  }
  bool discard(uint64_t token) {
    if(token!=m_session.load()||m_state.load()!=State::Frozen||m_writers.load())return false;
    m_storage.reset();m_capacity=m_pre=0;m_state=State::Idle;return true;
  }
  Snapshot snapshot(bool includeEvents) const {
    if(includeEvents&&m_writers.load())throw std::runtime_error("evidence writers not drained");
    Snapshot result{m_state.load(),m_reason.load(),m_session.load(),m_accepted.load(),m_evicted.load(),
      m_trigger.load()==UINT64_MAX?0:m_trigger.load(),m_capacity,m_remaining.load(),{},m_reserved.load()};
    // includeEvents requires frozen or externally quiescent (CPU unit tests).
    if(includeEvents){
      result.events.reserve(m_capacity);
      for(uint32_t i=0;i<m_capacity;++i)if(m_storage[i].event.sequence)result.events.push_back(m_storage[i].event);
      std::sort(result.events.begin(),result.events.end(),[](const Event& a,const Event& b){return a.sequence<b.sequence;});
    }
    return result;
  }
  // 预冻结钩子（2026-09-17 上级裁定 3a）：证据环**自动**冻结（post-window / 容量 / 序列回绕 / 请求）时，
  // 观察侧的记录器必须**在此之前**结算终态，否则终态永远进不了环（append 在 Frozen 下返回 0）。
  // 钩子在状态仍为 Armed/Triggered 时运行 ⇒ 钩子内经 Record() 的 append 仍被接受；带重入保护。
  using PreFreezeHook = void (*)() noexcept;
  void setPreFreezeHook(PreFreezeHook hook) noexcept {
    m_preFreeze.store(hook, std::memory_order_release);
  }
  State state() const noexcept { return m_state.load(); }
  uint64_t session() const noexcept { return m_session.load(); }
  bool frozenReady() const noexcept { return state()==State::Frozen && !m_writers.load(); }
  // Control owner retains the ring and excludes discard/rearm for this call.
  // Sort bounded pointers (~1 MiB on Win32), not a second ~100 MiB event copy.
  template<typename Visitor> bool visitFrozen(Visitor&& visitor) const {
    if(!frozenReady())return false;
    std::vector<const Event*> order;order.reserve(m_capacity);
    for(uint32_t i=0;i<m_capacity;++i)if(m_storage[i].event.sequence)order.push_back(&m_storage[i].event);
    std::sort(order.begin(),order.end(),[](const Event* a,const Event* b){return a->sequence<b->sequence;});
    for(const auto* event:order)if(!visitor(*event))return false;
    return true;
  }
private:
  void freeze(Reason why) noexcept {
    if (m_state.load() != State::Frozen) {
      const auto hook = m_preFreeze.load(std::memory_order_acquire);
      bool expected = false;
      if (hook != nullptr &&
          m_preFreezeRunning.compare_exchange_strong(expected, true)) {
        hook();
        m_preFreezeRunning.store(false, std::memory_order_release);
      }
    }
    m_reason.store(why);m_state.store(State::Frozen);
}
  struct Cell{std::atomic_flag busy=ATOMIC_FLAG_INIT;Event event{};};
  std::atomic<PreFreezeHook> m_preFreeze{nullptr};
  std::atomic<bool> m_preFreezeRunning{false};
  std::atomic<State> m_state{State::Idle};
  std::atomic<Reason> m_reason{Reason::None};
  std::atomic<uint64_t> m_session{0},m_accepted{0},m_evicted{0},m_trigger{UINT64_MAX},m_reserved{0};
  std::atomic<uint32_t> m_remaining{0},m_writers{0};
  std::atomic<bool> m_triggerClaim{false};
  uint32_t m_capacity=0,m_pre=0;
  std::unique_ptr<Cell[]> m_storage;
};
} // namespace dxvk::war3::tools::evidence
