#pragma once
#include "protocol.h"
#include "process_memory_probe.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace warvk::host::d9::memory {
// Development-only M0 observations, never an allocation policy or GPU receipt.
// These accounts cover explicit owned payload storage, not allocator overhead,
// compiler/driver internals, stack commitment, or physical GPU residency.
inline constexpr uint32_t Version = 1;
enum class Phase : uint32_t { DeviceReady=1, Uploaded, PrivateBound, Drained, FinalBound };
inline bool ValidPhase(uint32_t phase) noexcept { return phase>=1 && phase<=5; }
enum class Kind : uint32_t { VertexBuffer, IndexBuffer, Texture, SystemSurface,
  VertexShader, PixelShader, Declaration, PresentedReadback, Count };
inline constexpr size_t KindCount = size_t(Kind::Count);
inline constexpr std::array<const char*,KindCount> KindNames{{"vertexBuffer","indexBuffer","texture",
  "systemSurface","vertexShader","pixelShader","declaration","presentedReadback"}};
struct KindAccount {
  uint64_t objects=0, logicalBytes=0, cpuSizeBytes=0, cpuCapacityBytes=0;
};
struct CpuAccount {
  bool valid=true;
  std::array<KindAccount,KindCount> kinds{};
  // Observe storage independently of the logical-resource predicate. A dead
  // slot with retained vector capacity is NOT an empty account: retain its
  // actual bytes and invalidate certification, even when size() is zero.
  void observe(Kind kind,bool live,uint64_t logical,uint64_t size,uint64_t capacity) noexcept {
    if(size>capacity||size_t(kind)>=KindCount){valid=false;return;}
    auto& k=kinds[size_t(kind)];const auto max=std::numeric_limits<uint64_t>::max();
    const uint64_t objects=live?1:0;
    if(objects>max-k.objects||logical>max-k.logicalBytes||size>max-k.cpuSizeBytes
      ||capacity>max-k.cpuCapacityBytes){valid=false;return;}
    k.objects+=objects;k.logicalBytes+=logical;k.cpuSizeBytes+=size;k.cpuCapacityBytes+=capacity;
    if(!live&&(logical||size||capacity))valid=false;
  }
  void add(Kind kind,uint64_t logical,uint64_t size,uint64_t capacity) noexcept {
    observe(kind,true,logical,size,capacity);
  }
  void remove(Kind kind,uint64_t logical,uint64_t size,uint64_t capacity) noexcept {
    if(size_t(kind)>=KindCount){valid=false;return;}
    auto& k=kinds[size_t(kind)];
    if(!k.objects||logical>k.logicalBytes||size>k.cpuSizeBytes||capacity>k.cpuCapacityBytes){valid=false;return;}
    --k.objects;k.logicalBytes-=logical;k.cpuSizeBytes-=size;k.cpuCapacityBytes-=capacity;
  }
};
struct HostSample {
  uint32_t phase=0, ordinal=0, pid=0, thread=0, bits=64, logicalResources=0;
  uint64_t creation=0, nonceLow=0, nonceHigh=0, sequence=0, tickBegin=0, tickEnd=0;
  uint64_t logicalBytes=0, submitted=0, completed=0, retired=0, draws=0, uploads=0, readbacks=0;
  uint64_t fixedPacketPayloadBytes=0, ownerInlineBytes=0;
  lab::ProcessMemory process{};
  CpuAccount cpu{};
};
// Local x86 DLL ABI only. No native struct/pointer is copied to the x64 peer.
struct Checkpoint {
  uint32_t version=Version, phase=0, ordinal=0, pid=0, thread=0, bits=32;
  uint32_t logicalHandles=0, reservedHandles=0, privateBindings=0, publicZeroBindings=0;
  uint32_t expectedHostPid=0;
  uint64_t creation=0, tickBegin=0, beforeEnd=0, afterBegin=0, tickEnd=0;
  uint64_t expectedHostCreation=0, expectedNonceLow=0, expectedNonceHigh=0, expectedSequence=0;
  // fixedPacketPayloadBytes is contained in connectionInlineBytes. The worker's
  // separate local batch is stack payload, not another live heap allocation.
  uint64_t connectionInlineBytes=0, fixedPacketPayloadBytes=0, workerBatchPayloadBytes=0;
  lab::ProcessMemory before{},after{};
  CpuAccount cpu{};
  HostSample host{};
};
static_assert(std::is_standard_layout_v<Checkpoint> && std::is_trivially_copyable_v<Checkpoint>);

inline constexpr uint32_t HostWireBytes=512;
using HostWire=std::array<uint8_t,HostWireBytes>;
namespace detail {
struct Writer {
  uint8_t* p;size_t position=0;
  void u32(uint32_t v){ipc::WriteLe(p+position,v,4);position+=4;}
  void u64(uint64_t v){ipc::WriteLe(p+position,v,8);position+=8;}
};
struct Reader {
  const uint8_t* p;size_t position=0;
  uint32_t u32(){auto v=uint32_t(ipc::ReadLe(p+position,4));position+=4;return v;}
  uint64_t u64(){auto v=ipc::ReadLe(p+position,8);position+=8;return v;}
};
inline void WriteProcess(Writer& w,const lab::ProcessMemory& m){
  w.u32(m.valid?1:0);w.u32(m.win32);w.u64(m.privateBytes);w.u64(m.peakPrivateBytes);
  w.u64(m.workingSetBytes);w.u64(m.usedVirtualBytes);w.u64(m.privateVirtualBytes);
  w.u64(m.mappedVirtualBytes);w.u64(m.regions);
}
inline bool ReadProcess(Reader& r,lab::ProcessMemory& m){
  const auto valid=r.u32();if(valid>1)return false;m.valid=valid!=0;m.win32=r.u32();
  m.privateBytes=r.u64();m.peakPrivateBytes=r.u64();m.workingSetBytes=r.u64();
  m.usedVirtualBytes=r.u64();m.privateVirtualBytes=r.u64();m.mappedVirtualBytes=r.u64();m.regions=r.u64();
  return !m.valid||m.win32==0;
}
}
inline HostWire EncodeHost(const HostSample& h){
  HostWire bytes{};detail::Writer w{bytes.data()};
  w.u32(0x314d3944u);w.u32(Version); // D9M1, separate from the enclosing C2 identity.
  w.u32(h.phase);w.u32(h.ordinal);w.u32(h.pid);w.u32(h.thread);w.u32(h.bits);w.u32(h.logicalResources);
  w.u64(h.creation);w.u64(h.nonceLow);w.u64(h.nonceHigh);w.u64(h.sequence);w.u64(h.tickBegin);w.u64(h.tickEnd);
  w.u64(h.logicalBytes);w.u64(h.submitted);w.u64(h.completed);w.u64(h.retired);
  w.u64(h.draws);w.u64(h.uploads);w.u64(h.readbacks);w.u64(h.fixedPacketPayloadBytes);w.u64(h.ownerInlineBytes);
  detail::WriteProcess(w,h.process);w.u32(h.cpu.valid?1:0);w.u32(0);
  for(const auto& k:h.cpu.kinds){w.u64(k.objects);w.u64(k.logicalBytes);w.u64(k.cpuSizeBytes);w.u64(k.cpuCapacityBytes);}
  return bytes;
}
inline bool DecodeHost(const HostWire& bytes,HostSample& result){
  HostSample h;detail::Reader r{bytes.data()};
  if(r.u32()!=0x314d3944u||r.u32()!=Version)return false;
  h.phase=r.u32();h.ordinal=r.u32();h.pid=r.u32();h.thread=r.u32();h.bits=r.u32();h.logicalResources=r.u32();
  h.creation=r.u64();h.nonceLow=r.u64();h.nonceHigh=r.u64();h.sequence=r.u64();h.tickBegin=r.u64();h.tickEnd=r.u64();
  h.logicalBytes=r.u64();h.submitted=r.u64();h.completed=r.u64();h.retired=r.u64();
  h.draws=r.u64();h.uploads=r.u64();h.readbacks=r.u64();h.fixedPacketPayloadBytes=r.u64();h.ownerInlineBytes=r.u64();
  if(!detail::ReadProcess(r,h.process))return false;
  const auto valid=r.u32();if(valid>1||r.u32())return false;h.cpu.valid=valid!=0;
  for(auto& k:h.cpu.kinds){k.objects=r.u64();k.logicalBytes=r.u64();k.cpuSizeBytes=r.u64();k.cpuCapacityBytes=r.u64();
    if(k.cpuSizeBytes>k.cpuCapacityBytes)return false;}
  for(;r.position<bytes.size();++r.position)if(bytes[r.position])return false;
  if(!ValidPhase(h.phase)||h.bits!=64||!h.pid||!h.thread||!h.creation||!(h.nonceLow||h.nonceHigh)
    ||(h.phase==uint32_t(Phase::DeviceReady)?h.ordinal!=0:h.ordinal==0)
    ||!h.sequence||h.tickEnd<h.tickBegin||h.retired>h.completed||h.completed>h.submitted)return false;
  result=h;return true;
}
} // namespace warvk::host::d9::memory
