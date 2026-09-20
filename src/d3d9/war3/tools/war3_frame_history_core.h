#pragma once
#include <array>
#include <cstdint>

namespace dxvk::war3::tools::history {
// One state-number authority for the image owner, local worker and wire status.
// Explicit values preserve existing manifest/HUD/external analyzer contracts.
struct CaptureLifecycle {
  enum State : uint32_t {
    Idle=0, PendingArm=1, Armed=2, Triggered=3, Frozen=4,
    Exporting=5, Complete=6, Fault=7, Discard=8
  };
};
constexpr uint64_t MaxImageBytes=32ull*1024*1024;
constexpr uint64_t MaxRingBytes=4ull*1024*1024*1024;
constexpr uint32_t MaxPreFrames=256,MaxSlots=272;
inline bool IsShortcut(uint32_t message,uint64_t key,bool ctrl,bool shift,bool repeat) noexcept {
  return message==0x100 && key==0x43 && ctrl && shift && !repeat;
}
inline bool Budget(uint32_t width,uint32_t height,uint32_t pre,uint32_t post,uint64_t& bytes) noexcept {
  bytes=0;
  if(!width||!height||width>8192||height>8192||pre<4||pre>MaxPreFrames||post<1||post>16||pre+post>MaxSlots)return false;
  const uint64_t image=uint64_t(width)*height*4;
  if(image>MaxImageBytes||image*(pre+post)>MaxRingBytes)return false;
  bytes=image*(pre+post);return true;
}
inline uint32_t Gather(uint32_t pre,uint32_t filled,uint32_t head,uint32_t postWritten,
                       std::array<uint32_t,MaxSlots>& order) noexcept {
  if(!pre||pre>MaxPreFrames||filled>pre||head>=pre||postWritten>16||filled+postWritten>MaxSlots)return 0;
  uint32_t count=0;
  for(uint32_t i=0;i<filled;++i)order[count++]=(head+pre-filled+i)%pre;
  for(uint32_t i=0;i<postWritten;++i)order[count++]=pre+i;
  return count;
}
} // namespace dxvk::war3::tools::history
