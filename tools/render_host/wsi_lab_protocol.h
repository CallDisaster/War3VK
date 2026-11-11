#pragma once

#include "win32_transport.h"
#include <array>
#include <cstring>

// RH3 is a bounded cross-bitness WSI experiment, not RH0 Echo and not a D3D9
// command stream. Only an HWND user-object token crosses processes; no pointer,
// HINSTANCE, Vk handle, image or native C++ layout is sent. The host independently
// verifies that the HWND belongs to its already authenticated exact parent.
namespace warvk::host::wsi {
inline constexpr size_t WireBytes = 96;
inline constexpr uint32_t MaxExtent = 1024;
inline constexpr uint32_t FramesPerBatch = 3;
enum class Op : uint16_t { Hello=1, Create=2, Frames=3, Resize=4, Close=5 };
struct Message {
  Op op=Op::Hello;
  bool reply=false;
  ipc::Nonce nonce;
  uint64_t sequence=0, windowEpoch=0;
  uint32_t hwnd=0, width=0, height=0, frames=0;
  uint32_t senderBits=32, peerBits=64;
  uint32_t presented=0, gpuCompleted=0, resized=0;
};
using Wire = std::array<uint8_t, WireBytes>;
inline void Require(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
inline Wire Encode(const Message& m) {
  Wire b{};
  std::memcpy(b.data(), "RHW3", 4);
  ipc::WriteLe(b.data()+4,1,2); ipc::WriteLe(b.data()+6,WireBytes,2);
  ipc::WriteLe(b.data()+8,uint16_t(m.op),2);
  ipc::WriteLe(b.data()+10,m.reply?1:0,2);
  ipc::WriteLe(b.data()+16,m.nonce.low,8); ipc::WriteLe(b.data()+24,m.nonce.high,8);
  ipc::WriteLe(b.data()+32,m.sequence,8); ipc::WriteLe(b.data()+40,m.windowEpoch,8);
  ipc::WriteLe(b.data()+48,m.hwnd,4); ipc::WriteLe(b.data()+52,m.width,4);
  ipc::WriteLe(b.data()+56,m.height,4); ipc::WriteLe(b.data()+60,m.frames,4);
  ipc::WriteLe(b.data()+64,m.senderBits,4); ipc::WriteLe(b.data()+68,m.peerBits,4);
  ipc::WriteLe(b.data()+72,m.presented,4); ipc::WriteLe(b.data()+76,m.gpuCompleted,4);
  ipc::WriteLe(b.data()+80,m.resized,4);
  return b;
}
inline Message Decode(const Wire& b) {
  Require(!std::memcmp(b.data(),"RHW3",4),"wire-magic");
  Require(ipc::ReadLe(b.data()+4,2)==1 && ipc::ReadLe(b.data()+6,2)==WireBytes,"wire-version-size");
  const auto op=ipc::ReadLe(b.data()+8,2), flags=ipc::ReadLe(b.data()+10,2);
  Require(op>=1 && op<=5 && flags<=1,"wire-op-flags");
  Require(ipc::ReadLe(b.data()+12,4)==0 && ipc::ReadLe(b.data()+84,4)==0 &&
          ipc::ReadLe(b.data()+88,8)==0,"wire-reserved");
  Message m; m.op=Op(op); m.reply=flags==1;
  m.nonce={ipc::ReadLe(b.data()+16,8),ipc::ReadLe(b.data()+24,8)};
  m.sequence=ipc::ReadLe(b.data()+32,8); m.windowEpoch=ipc::ReadLe(b.data()+40,8);
  m.hwnd=uint32_t(ipc::ReadLe(b.data()+48,4));
  m.width=uint32_t(ipc::ReadLe(b.data()+52,4)); m.height=uint32_t(ipc::ReadLe(b.data()+56,4));
  m.frames=uint32_t(ipc::ReadLe(b.data()+60,4));
  m.senderBits=uint32_t(ipc::ReadLe(b.data()+64,4)); m.peerBits=uint32_t(ipc::ReadLe(b.data()+68,4));
  m.presented=uint32_t(ipc::ReadLe(b.data()+72,4)); m.gpuCompleted=uint32_t(ipc::ReadLe(b.data()+76,4));
  m.resized=uint32_t(ipc::ReadLe(b.data()+80,4));
  Require(!m.nonce.empty() && m.sequence!=0,"wire-identity");
  Require(m.width<=MaxExtent && m.height<=MaxExtent && m.frames<=FramesPerBatch,"wire-limits");
  Require((m.senderBits==32 && m.peerBits==64) || (m.senderBits==64 && m.peerBits==32),"wire-bitness");
  Require(m.presented<=6 && m.gpuCompleted<=6 && m.resized<=1,"wire-counter-limit");
  return m;
}
inline Message Read(HANDLE pipe,HANDLE peer,lab::IoStats& stats) {
  Wire b{}; lab::ReadWindow deadline;
  lab::ReadExact(pipe,peer,b.data(),b.size(),deadline,lab::Stage::Header,stats);
  return Decode(b);
}
inline void Write(HANDLE pipe,HANDLE peer,const Message& m,lab::IoStats& stats) {
  const auto b=Encode(m); lab::WriteBytes(pipe,peer,b.data(),b.size(),stats);
}
inline HWND DecodeWindow(uint32_t token) noexcept {
  // Microsoft 32/64 IPC contract: user handles are sign-extended from 32 bits.
  return reinterpret_cast<HWND>(static_cast<intptr_t>(static_cast<int32_t>(token)));
}
inline uint32_t EncodeWindow(HWND hwnd) noexcept {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
}
inline uint64_t Number(const wchar_t* p) {
  Require(p && *p,"argument-number"); uint64_t value=0;
  for (;*p;++p) {
    Require(*p>=L'0' && *p<=L'9' && value<=(UINT64_MAX-uint64_t(*p-L'0'))/10,"argument-overflow");
    value=value*10+uint64_t(*p-L'0');
  }
  return value;
}
} // namespace warvk::host::wsi
