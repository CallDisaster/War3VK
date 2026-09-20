#pragma once
#include "recorder_event_wire.h"
#include <cstring>

// Deterministic CPU LABORATORY content, never a captured game Event.
namespace warvk::host::recorder::fixture {
inline constexpr uint64_t Session=0x1020304050607080ULL, Map=11, Device=17;
inline Event Make(uint64_t source) noexcept {
  Event e{};e.sequence=source;e.session=Session;
  e.parent=source;e.qpc=0x1234000000000000ULL+source;
  e.key={0x9876000000000000ULL+source,source/100,Map,Device};
  e.thread=uint32_t(source%6);e.kind=dxvk::war3::tools::evidence::Kind::PresentBegin;
  std::memcpy(e.label.data(),"recorder-e2",11);
  for(size_t i=0;i<e.data.size();++i)e.data[i]=e.qpc+i;
  for(size_t i=0;i<e.bits.size();++i)e.bits[i]=uint32_t(source*73+i);
  e.bits[0]=0x7f800000;e.bits[1]=0x7fc12345;e.bits[2]=0x80000000;
  return e;
}
inline bool ContentMatches(const Event& e) noexcept {
  const auto expected=Make(e.parent);
  return e.parent>0 && e.sequence==e.parent && e.session==expected.session && e.qpc==expected.qpc &&
    e.key.owner==expected.key.owner && e.key.frame==expected.key.frame && e.key.mapEpoch==Map && e.key.deviceEpoch==Device &&
    e.thread==expected.thread && e.kind==expected.kind && e.label==expected.label && e.data==expected.data && e.bits==expected.bits;
}
}
