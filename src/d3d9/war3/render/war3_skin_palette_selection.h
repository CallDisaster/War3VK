#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifndef WARVK_SKIN_PALETTE_CONTRACT_DEFAULT
#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0
#endif

namespace dxvk::war3::render::skin {
enum class Source : uint32_t { Unknown, CapturedWriter, CapturedRawArena,
  OwnedPartSnapshot, LegacyGlobalSlot, LegacySlotCache, PoseGroups, CModelGroups };
enum class Space : uint32_t { Unknown, World, ModelLocal };
enum class Domain : uint32_t { Unknown, VertexGroups, Bones };
// This is a CPU publication contract, NOT an invented native slot lease. A
// publication ticket names immutable copied bytes; allocationGeneration stays
// zero until an actual native allocator witness exists. Zero never authorizes
// rereading the global arena.
struct Selection {
  Source source=Source::Unknown;
  Space space=Space::Unknown;
  Domain domain=Domain::Unknown;
  uintptr_t runtimeModel=0,part=0,meshPayload=0;
  uint64_t ownerEpoch=0,publicationTicket=0,captureSerial=0,hash=0;
  uint64_t slotAllocationGeneration=0;
  uint32_t slot=UINT32_MAX,actualGroupCount=0,frameTag=0;
};
inline bool ContractEnabled() noexcept {
  static const bool enabled=[] {const char* v=std::getenv("DXVK_WAR3_SKIN_PALETTE_CONTRACT");
    return v?std::strcmp(v,"1")==0:WARVK_SKIN_PALETTE_CONTRACT_DEFAULT!=0;}();
  return enabled;
}
inline bool GroupRange(uint32_t required,uint32_t actual) noexcept {
  return required&&actual&&actual<=256&&required<=actual;
}
// 2026-09-18 独立复审批次 4（步骤①）：**producer 绑定表的数量准入规则**。
// 冷缓存与热缓存必须适用同一条规则 —— 该规则原先在两处**各写一份**，
// 正是「冷缓存缺少检查」这一缺陷得以存在的土壤。抽为单一实现后：
//   · 两处调用同一函数（相同规则只维护一次）；
//   · 规则本身成为可独立单测的纯谓词（用于证明「新行为符合预期」）。
// 语义：本次所需矩阵数必须非零，且 producer 记录的 groupCount 必须覆盖它。
inline bool ProducerGroupCountCovers(uint32_t required,uint32_t producerGroupCount) noexcept {
  return required!=0u&&producerGroupCount>=required;
}
inline bool OwnedSnapshotMatches(const Selection& s,uintptr_t model,uintptr_t part,
    uint64_t epoch,uint32_t currentSlot,uint32_t currentFrame,uint32_t required) noexcept {
  return s.source==Source::OwnedPartSnapshot&&s.space==Space::World&&s.domain==Domain::VertexGroups&&
    model&&part&&epoch&&currentFrame&&s.runtimeModel==model&&s.part==part&&s.ownerEpoch==epoch&&
    s.publicationTicket&&s.hash&&s.slot==currentSlot&&currentSlot<0x3a98&&
    s.frameTag==currentFrame&&GroupRange(required,s.actualGroupCount);
}
inline bool IsCaptured(const Selection& s) noexcept {
  return s.source==Source::CapturedWriter||s.source==Source::CapturedRawArena;
}
inline bool Usable(const Selection& s,uintptr_t part,uint32_t required,uint64_t hash) noexcept {
  if(!part||s.part!=part||!hash||s.hash!=hash||!GroupRange(required,s.actualGroupCount)||
     s.domain!=Domain::VertexGroups||s.space!=Space::World)return false;
  if(s.source==Source::CapturedWriter)return s.captureSerial&&s.meshPayload;
  if(s.source==Source::OwnedPartSnapshot)return s.runtimeModel&&s.ownerEpoch&&s.publicationTicket&&s.frameTag;
  return false; // Raw arena / pose guessing cannot become authority by labeling.
}
inline bool CanReplace(const Selection& old,const Selection& next) noexcept {
  if(next.source!=Source::OwnedPartSnapshot||!next.part||!next.runtimeModel||!next.ownerEpoch||
     !next.publicationTicket||!next.frameTag||!next.hash||next.space!=Space::World||
     next.domain!=Domain::VertexGroups||!GroupRange(1,next.actualGroupCount))return false;
  if(old.part&&old.part!=next.part)return false;
  if(old.runtimeModel&&old.runtimeModel!=next.runtimeModel)return false;
  if(old.ownerEpoch&&old.ownerEpoch!=next.ownerEpoch)return false;
  return true;
}
inline uint64_t NextTicket(std::atomic<uint64_t>& value) noexcept {
  auto n=value.load(std::memory_order_relaxed);
  do {if(n==UINT64_MAX)return 0;} while(!value.compare_exchange_weak(n,n+1,std::memory_order_relaxed));
  return n+1;
}
class TryCell {
public:
  explicit TryCell(std::atomic_flag& flag) noexcept:f(flag),held(!f.test_and_set(std::memory_order_acquire)){}
  ~TryCell(){if(held)f.clear(std::memory_order_release);}
  explicit operator bool() const noexcept{return held;}
  TryCell(const TryCell&)=delete;
private:std::atomic_flag& f;bool held;
};
}
