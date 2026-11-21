#pragma once
#include "protocol.h"
#include "d3d9_resource_registry.h"
#include <algorithm>
#include <cstring>

// D9C2: explicitly limited D3D9 development profile, separate from CPU/RH4.
// Generic D3D9 resources are device scoped, not tied to a Warcraft map epoch.
// IDs may be reused only at the next generation after a replied or FIFO-published Destroy.
// A new device requires a fresh nonce. C1/R1 peers and old draw layouts fail.
namespace warvk::host::d9 {
constexpr uint32_t HeaderBytes = 128, ReplyBytes = 96, MaxBytes = 65536;
constexpr uint32_t MaxPayload = MaxBytes - HeaderBytes;
// Width/Height are the original lab extent (wire defaults and the 64x64 tests);
// the negotiated target extent is any 1..MaxTargetDimension per axis.
// MaxBuffer covers the game's largest buffer, a 576 KiB dynamic VB.
constexpr uint32_t MaxBuffer = 1048576, Width = 64, Height = 64, MaxTargetDimension = 4096;
constexpr uint32_t MaxReplyPayload = MaxBytes - ReplyBytes;
// CreateBuffer: args[0] bytes, [1] index16, [2] D3DUSAGE_DYNAMIC.
// UploadBuffer: args[0] offset, [1] how its Lock was taken. Ordered waits for
// every earlier use. A dynamic buffer may skip that: NoOverwrite writes bytes
// no earlier draw reads; Discard, on the first chunk of a Lock, leaves the old
// storage to earlier draws (its bytes carry over into the new storage).
enum class UploadMode : uint32_t { Ordered = 0, NoOverwrite = 1, Discard = 2 };
inline bool ValidUploadMode(uint32_t mode, bool dynamic) {
  return mode == uint32_t(UploadMode::Ordered) || (dynamic && mode <= uint32_t(UploadMode::Discard));
}
enum class Op : uint16_t {
  Initialize=1, CreateBuffer, UploadBuffer, CreateTexture, UploadTexture,
  CreateShader, CreateDeclaration, Clear, Draw, Readback, Issue, Poll,
  Destroy, Finish, Present, CreateQuery, DiagnosticCheckpoint, DrawBatch, WaitEvent, FlushDraws, ClearRegions, FactoryQuery, DrawFixed,
  CreateTexture2D, BeginTextureUpload, TextureUploadChunk, CommitTextureUpload, AbortTextureUpload, TextureLod, SetSamplerState, SetEffects, SetTextureStages,
  DrawFixedBatch, ReadContent, AnnotateDraws, FrameScene, StageMarker, Reset,
  AttachRing, SessionReset, ContentUpload, ContentRelease, ContentRead, DrawFixedDeltaBatch
};
inline bool IsFixedBatch(Op op) { return op==Op::DrawFixedBatch||op==Op::DrawFixedDeltaBatch; }
using Packet = std::array<uint8_t, MaxBytes>;
struct Header {
  Op op = Op::Initialize;
  ipc::Nonce nonce{};
  uint64_t sequence = 0;
  uint32_t id = 0, generation = 0, payloadBytes = 0;
  std::array<uint32_t,16> args{};
};
// Explicit profiles retain the initialization field layout, but old wire
// versions are rejected. WSI is negotiated, never inferred from an otherwise
// ordinary D3D Present call.
// args: width,height,profile,HWND-low32,HWND-owner-TID,interval,features,
// back buffer format. Peer PID is authenticated by the pipe/process handle, NOT supplied here.
// Interval (WSI profiles only): D3DPRESENT_INTERVAL_ONE or _IMMEDIATE; the x86
// owner folds DEFAULT into ONE. Format: 0 is the original A8R8G8B8 target,
// D3DFMT_X8R8G8B8 (22) an opaque one whose alpha reads as one when blending.
// RetainContent: the host keeps authoritative CPU copies of vertex-buffer and
// texture contents for ReadContent, so the x86 owner may drop its own copies.
// Annotations: the host accepts the data-layer records of d3d9_annotation_wire.h.
constexpr uint32_t InitializeRetainContent=1,InitializeAnnotations=2,InitializeFixedDelta=4;
constexpr uint32_t InitializeAsyncDestroy=8,InitializeAsyncCreateTexture=16;
constexpr uint32_t AsyncResourceFeatures=InitializeAsyncDestroy|InitializeAsyncCreateTexture;
inline uint32_t AsyncResourceFeature(Op op) {
  return op==Op::Destroy?InitializeAsyncDestroy:op==Op::CreateTexture2D?InitializeAsyncCreateTexture:0;
}
inline bool ValidResourceTransport(uint32_t features,bool ring,bool asyncBatches) {
  return !(features&AsyncResourceFeatures)||(ring&&asyncBatches);
}
constexpr uint32_t IntervalOne=1,IntervalImmediate=0x80000000u,OpaqueBackBuffer=22;
enum class PresentationProfile : uint32_t { Offscreen=0, IsolatedPresenter=1, IsolatedPresenterReadback=2 };
struct Initialization {
  PresentationProfile profile=PresentationProfile::Offscreen;
  uint32_t window=0,thread=0,interval=0,width=0,height=0;
  bool retainContent=false,annotations=false,opaque=false,fixedDelta=false;
  uint32_t asyncResources=0;
};
inline bool ValidTargetExtent(uint32_t width,uint32_t height) {
  return width&&height&&width<=MaxTargetDimension&&height<=MaxTargetDimension;
}
inline bool ValidInterval(bool presenter,uint32_t interval) {
  return presenter?interval==IntervalOne||interval==IntervalImmediate:!interval;
}
inline bool DecodeInitialization(const Header& h,Initialization& result) {
  if(h.op!=Op::Initialize||h.id||h.generation||h.payloadBytes||!ValidTargetExtent(h.args[0],h.args[1]))return false;
  if(h.args[6]&~(InitializeRetainContent|InitializeAnnotations|InitializeFixedDelta|AsyncResourceFeatures))return false;
  if(h.args[7]&&h.args[7]!=OpaqueBackBuffer)return false;
  for(size_t i=8;i<h.args.size();++i)if(h.args[i])return false;
  if(h.args[2]==uint32_t(PresentationProfile::Offscreen)) {
    if(h.args[3]||h.args[4]||!ValidInterval(false,h.args[5]))return false;
  } else if(h.args[2]==uint32_t(PresentationProfile::IsolatedPresenter)
      ||h.args[2]==uint32_t(PresentationProfile::IsolatedPresenterReadback)) {
    if(!h.args[3]||!h.args[4]||!ValidInterval(true,h.args[5]))return false;
  } else return false;
  result={PresentationProfile(h.args[2]),h.args[3],h.args[4],h.args[5],h.args[0],h.args[1],
    (h.args[6]&InitializeRetainContent)!=0,(h.args[6]&InitializeAnnotations)!=0,h.args[7]==OpaqueBackBuffer,(h.args[6]&InitializeFixedDelta)!=0,h.args[6]&AsyncResourceFeatures};return true;
}
// D3D9 Reset of the implicit swap chain: args width, height, then interval
// (args[5]) and back buffer format (args[7]) with Initialize's meanings; the
// profile and window are the session's. The x86 owner has already applied
// D3D9's rules (no DEFAULT-pool resource or state block alive); the host
// recreates the target, depth and swap chain and keeps every other resource.
// args[6] ResetKeepResources: the window's own resize, as WarVK's WM_SIZE hook
// resets the swap chain (src/d3d9/d3d9_window.cpp). DEFAULT-pool buffers stay
// alive too; only the target, depth and swap chain are new.
constexpr uint32_t ResetKeepResources=1;
// AttachRing: the one message after a successful Initialize, on the pipe. args
// ring capacity, reply capacity, ring version (d3d9_command_ring.h). After its
// D3D_OK reply every later message travels on the shared-memory command ring.
// SessionReset: the x86 data layer saw the map change or the game end; args
// reason, then its transition generation (low, high). Sent before the Present
// that applies it. Needs negotiated Annotations.
enum class SessionResetReason : uint32_t { MapTransition=1, GameExit=2 };
inline bool ValidSessionReset(const Header& h) {
  if(h.op!=Op::SessionReset||h.id||h.generation||h.payloadBytes)return false;
  if(h.args[0]!=uint32_t(SessionResetReason::MapTransition)&&h.args[0]!=uint32_t(SessionResetReason::GameExit))return false;
  if(!h.args[1]&&!h.args[2])return false;
  for(size_t i=3;i<h.args.size();++i)if(h.args[i])return false;
  return true;
}
struct Resize { uint32_t width=0,height=0,interval=0; bool opaque=false,keepResources=false; };
inline bool DecodeReset(const Header& h,bool presenter,Resize& result) {
  if(h.op!=Op::Reset||h.id||h.generation||h.payloadBytes||!ValidTargetExtent(h.args[0],h.args[1]))return false;
  if(h.args[2]||h.args[3]||h.args[4]||(h.args[6]&~ResetKeepResources)||!ValidInterval(presenter,h.args[5]))return false;
  if(h.args[7]&&h.args[7]!=OpaqueBackBuffer)return false;
  for(size_t i=8;i<h.args.size();++i)if(h.args[i])return false;
  result={h.args[0],h.args[1],h.args[5],h.args[7]==OpaqueBackBuffer,h.args[6]==ResetKeepResources};return true;
}
// Readback args: first row, row count, source, then the presented frame (low,
// high) for the Presented source. Rows (0,0) mean the whole image, accepted
// only when it fits one reply; otherwise the caller reads bands of whole rows.
enum class ReadbackSource : uint32_t { Target=0, Presented=1 };
inline uint32_t ReadbackBandRows(uint32_t width) { return MaxReplyPayload/(width*4); }
inline bool ResolveReadbackRows(uint32_t width,uint32_t height,uint32_t& first,uint32_t& rows) {
  if(!ValidTargetExtent(width,height))return false;
  if(!first&&!rows) {
    if(uint64_t(width)*height*4>MaxReplyPayload)return false;
    rows=height;return true;
  }
  return rows&&first<height&&rows<=height-first&&rows<=ReadbackBandRows(width);
}
// The presented frame number, then the image, iff both fit one Present reply.
inline bool PresentedInline(uint32_t width,uint32_t height) {
  return 8+uint64_t(width)*height*4<=MaxReplyPayload;
}
// Present refused only because the window's client no longer matches the back
// buffer (a resize awaiting Reset): nothing acquired or queued, the transport
// stays healthy and the x86 owner reports a successful, skipped frame.
constexpr uint32_t PresentSkipped=0x8876086Au; // D3DERR_NOTAVAILABLE
struct Reply {
  uint32_t result = 0;
  uint64_t submitted = 0, completed = 0, retired = 0, draws = 0;
  uint32_t payloadBytes = 0;
};
// Fixed-width little-endian helpers avoid an out-of-line variable-width loop
// for every draw word. Byte accesses keep unaligned buffers and host endian
// semantics explicit; the compiler can lower them to native scalar loads.
inline uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline uint64_t U64(const uint8_t* p) { return uint64_t(U32(p))|(uint64_t(U32(p+4))<<32); }
inline void P32(uint8_t* p,uint32_t v) {
  p[0]=uint8_t(v);p[1]=uint8_t(v>>8);p[2]=uint8_t(v>>16);p[3]=uint8_t(v>>24);
}
inline void P64(uint8_t* p,uint64_t v) { P32(p,uint32_t(v));P32(p+4,uint32_t(v>>32)); }
inline uint32_t FloatBits(float f) { uint32_t r; std::memcpy(&r,&f,4); return r; }
inline float Float(uint32_t u) { float r; std::memcpy(&r,&u,4); return r; }
constexpr uint64_t Fnv1a64Offset=0xcbf29ce484222325ull;
inline uint64_t FoldFnv1a64(const uint8_t* p,size_t bytes,uint64_t digest=Fnv1a64Offset) {
  for(size_t i=0;i<bytes;++i){digest^=p[i];digest*=0x100000001b3ull;}return digest;
}
inline bool ValidOp(uint32_t op) { return op>=uint32_t(Op::Initialize)&&op<=uint32_t(Op::DrawFixedDeltaBatch); }
inline bool DecodeHeader(const uint8_t* p, Header& h) {
  if(std::memcmp(p,"D9C2",4)||ipc::ReadLe(p+4,2)!=2||!ValidOp(ipc::ReadLe(p+6,2))
      ||U32(p+12)||U32(p+8)>MaxPayload) return false;
  for(uint32_t i=112;i<HeaderBytes;++i) if(p[i])return false;
  h.op=Op(ipc::ReadLe(p+6,2)); h.payloadBytes=U32(p+8);
  h.nonce={U64(p+16),U64(p+24)}; h.sequence=U64(p+32);
  h.id=U32(p+40); h.generation=U32(p+44);
  for(size_t i=0;i<h.args.size();++i)h.args[i]=U32(p+48+i*4);
  return !h.nonce.empty()&&h.sequence&&h.id<=MaxResources
    &&(h.id?h.generation!=0:h.generation==0);
}
// The 128 header bytes alone; the command ring copies the payload from its source.
inline bool EncodeHeader(const Header& h,uint8_t* p) {
  if(h.payloadBytes>MaxPayload)return false;
  std::fill_n(p,HeaderBytes,uint8_t(0));
  std::memcpy(p,"D9C2",4); ipc::WriteLe(p+4,2,2);
  ipc::WriteLe(p+6,uint16_t(h.op),2); P32(p+8,h.payloadBytes);
  P64(p+16,h.nonce.low);P64(p+24,h.nonce.high);P64(p+32,h.sequence);
  P32(p+40,h.id);P32(p+44,h.generation);
  for(size_t i=0;i<h.args.size();++i)P32(p+48+i*4,h.args[i]);
  Header check; return DecodeHeader(p,check);
}
inline bool Encode(const Header& h,const void* payload,Packet& p) {
  if((h.payloadBytes&&!payload)||!EncodeHeader(h,p.data()))return false;
  if(h.payloadBytes)std::memcpy(p.data()+HeaderBytes,payload,h.payloadBytes);
  return true;
}
inline void EncodeReply(const Header& h,const Reply& r,Packet& p) {
  std::fill_n(p.data(),ReplyBytes,uint8_t(0));
  std::memcpy(p.data(),"D9R2",4);P32(p.data()+4,2);P32(p.data()+8,r.payloadBytes);P32(p.data()+12,r.result);
  P64(p.data()+16,h.nonce.low);P64(p.data()+24,h.nonce.high);P64(p.data()+32,h.sequence);
  P32(p.data()+40,h.id);P32(p.data()+44,h.generation);
  P64(p.data()+48,r.submitted);P64(p.data()+56,r.completed);P64(p.data()+64,r.retired);P64(p.data()+72,r.draws);
}
inline bool DecodeReply(const Header& h,const uint8_t* p,Reply& r) {
  if(std::memcmp(p,"D9R2",4)||U32(p+4)!=2||U32(p+8)>MaxBytes-ReplyBytes
      ||U64(p+16)!=h.nonce.low||U64(p+24)!=h.nonce.high||U64(p+32)!=h.sequence
      ||U32(p+40)!=h.id||U32(p+44)!=h.generation)return false;
  for(uint32_t i=80;i<ReplyBytes;++i)if(p[i])return false;
  r.result=U32(p+12);r.payloadBytes=U32(p+8);r.submitted=U64(p+48);r.completed=U64(p+56);
  r.retired=U64(p+64);r.draws=U64(p+72);
  return r.retired<=r.completed&&r.completed<=r.submitted;
}
} // namespace warvk::host::d9
