#pragma once
#include "d3d9_draw_wire.h"
#include "d3d9_ff_wire.h"
#include <cmath>

// x86 data-layer facts for the host's enhancement executors, negotiated at
// Initialize (args[6] bit1). Records carry values, never game or COM pointers.
// - FrameScene: one schema record per message (camera now; lights/fog later).
// - StageMarker: an ordered insertion point between draws.
// - AnnotateDraws: records for the draws of the message that immediately
//   follows it. Any other next message is refused and the records are dropped.
namespace warvk::host::d9 {
enum class AnnotationSchema : uint16_t { DrawTag=1, FrameCamera=2 };
// Provisional v1 vocabulary. A consumer that needs more adds a schema rather
// than reinterpreting these fields. object is a data-layer instance key.
struct DrawTag { uint64_t object=0; uint32_t kind=0,flags=0; };
struct FrameCamera {
  std::array<float,16> view{},projection{};
  std::array<float,4> viewport{};
  std::array<float,2> depthRange{};
  uint32_t frame=0;float gameTime=0;
};
constexpr uint32_t DrawTagBytes=16,FrameCameraBytes=(16+16+4+2)*4+8;
enum class Stage : uint32_t { WorldBegin=1, BeforeUi, UiBegin, FrameEnd };
constexpr uint32_t RecordHeaderBytes=8,MaxAnnotationData=256,MaxRecordsPerDraw=4;
constexpr uint32_t MaxDrawAnnotationBytes=MaxDrawBatch*MaxRecordsPerDraw*(RecordHeaderBytes+MaxAnnotationData);
static_assert(MaxDrawAnnotationBytes<=MaxPayload,"one batch's records must fit one bounded packet");
static_assert(MaxFixedDrawBatch<=MaxDrawBatch,"fixed batches reuse the programmable record bound");

inline uint32_t RecordBytes(uint32_t data) { return RecordHeaderBytes+((data+3)&~3u); }
inline bool ValidStage(uint32_t stage) { return stage>=uint32_t(Stage::WorldBegin)&&stage<=uint32_t(Stage::FrameEnd); }
inline bool ValidDrawRecord(uint32_t schema,uint32_t bytes) {
  return schema==uint32_t(AnnotationSchema::DrawTag)&&bytes==DrawTagBytes;
}
// The draw message an annotation may bind to, and its draw-count bound.
inline uint32_t AnnotatedDrawCapacity(uint32_t op) {
  switch(Op(op)) {
    case Op::Draw: case Op::DrawFixed: return 1;
    case Op::DrawBatch: return MaxDrawBatch;
    case Op::DrawFixedBatch: return MaxFixedDrawBatch;
    default: return 0;
  }
}
inline std::array<uint8_t,DrawTagBytes> EncodeDrawTag(const DrawTag& tag) {
  std::array<uint8_t,DrawTagBytes> p{};P64(p.data(),tag.object);P32(p.data()+8,tag.kind);P32(p.data()+12,tag.flags);return p;
}
inline std::array<uint8_t,FrameCameraBytes> EncodeFrameCamera(const FrameCamera& c) {
  std::array<uint8_t,FrameCameraBytes> p{};size_t i=0;
  auto f=[&](float v){P32(p.data()+4*i++,FloatBits(v));};
  for(float v:c.view)f(v);for(float v:c.projection)f(v);for(float v:c.viewport)f(v);for(float v:c.depthRange)f(v);
  P32(p.data()+4*i++,c.frame);f(c.gameTime);return p;
}
inline bool ValidFrameScene(uint32_t schema,const uint8_t* p,uint32_t bytes) {
  if(schema!=uint32_t(AnnotationSchema::FrameCamera)||bytes!=FrameCameraBytes||!p)return false;
  for(uint32_t i=0;i<FrameCameraBytes/4;++i)
    if(i!=38&&!std::isfinite(Float(U32(p+4*i))))return false; // word 38 is the frame number
  return true;
}
// Appends one record; the caller keeps records sorted by (index, schema).
inline uint32_t AppendDrawRecord(uint8_t* out,uint32_t capacity,uint16_t index,uint16_t schema,const void* data,uint16_t bytes) {
  const auto size=RecordBytes(bytes);
  if(!out||!data||size>capacity||!ValidDrawRecord(schema,bytes))return 0;
  std::fill_n(out,size,uint8_t(0));
  out[0]=uint8_t(index);out[1]=uint8_t(index>>8);out[2]=uint8_t(schema);out[3]=uint8_t(schema>>8);
  out[4]=uint8_t(bytes);out[5]=uint8_t(bytes>>8);
  std::memcpy(out+RecordHeaderBytes,data,bytes);return size;
}
struct DrawAnnotations {
  Op draw=Op::Initialize;
  uint32_t draws=0,count=0,bytes=0;
  std::array<uint8_t,MaxDrawAnnotationBytes> data{};
  // The message these records bind to: its op and draw count.
  bool binds(const Header& h) const {
    return h.op==draw&&(AnnotatedDrawCapacity(uint32_t(draw))==1?draws==1:h.args[0]==draws);
  }
  template<class F> void forEach(F f) const {
    for(uint32_t at=0;at<bytes;) {
      const uint16_t index=uint16_t(data[at]|(data[at+1]<<8)),schema=uint16_t(data[at+2]|(data[at+3]<<8));
      const uint16_t size=uint16_t(data[at+4]|(data[at+5]<<8));
      f(index,schema,data.data()+at+RecordHeaderBytes,size);at+=RecordBytes(size);
    }
  }
};
// args: record count, draw count, bound draw op. Records ascend by (index,
// schema), padding and reserved bytes are zero, and the payload is exact.
inline bool DecodeDrawAnnotations(const Header& h,const uint8_t* p,DrawAnnotations& out) {
  const auto capacity=AnnotatedDrawCapacity(h.args[2]);
  if(h.op!=Op::AnnotateDraws||h.id||h.generation||!capacity||!h.args[1]||h.args[1]>capacity
    ||!h.args[0]||h.args[0]>h.args[1]*MaxRecordsPerDraw||!h.payloadBytes||h.payloadBytes>MaxDrawAnnotationBytes||!p)return false;
  for(size_t i=3;i<h.args.size();++i)if(h.args[i])return false;
  uint32_t at=0,count=0,previous=0;
  while(at<h.payloadBytes) {
    if(h.payloadBytes-at<RecordHeaderBytes)return false;
    const uint32_t index=p[at]|(p[at+1]<<8),schema=p[at+2]|(p[at+3]<<8),bytes=p[at+4]|(p[at+5]<<8);
    const auto size=RecordBytes(bytes);
    if(p[at+6]||p[at+7]||index>=h.args[1]||!ValidDrawRecord(schema,bytes)||size>h.payloadBytes-at)return false;
    const uint32_t key=(index<<16)|schema;
    if(count&&key<=previous)return false;
    for(uint32_t i=RecordHeaderBytes+bytes;i<size;++i)if(p[at+i])return false;
    previous=key;++count;at+=size;
  }
  if(count!=h.args[0])return false;
  out.draw=Op(h.args[2]);out.draws=h.args[1];out.count=count;out.bytes=h.payloadBytes;
  std::memcpy(out.data.data(),p,h.payloadBytes);return true;
}
// FNV-1a over the exact delivered sequence. The fixture folds the same fields
// independently, so equal digests mean equal order, payloads and draw binding.
struct AnnotationRecorder {
  uint64_t annotatedDraws=0,drawRecords=0,frameScenes=0,stageMarkers=0,digest=0xcbf29ce484222325ull;
  void fold(const uint8_t* p,size_t bytes) {for(size_t i=0;i<bytes;++i){digest^=p[i];digest*=0x100000001b3ull;}}
  void word(uint32_t v) {uint8_t p[4];P32(p,v);fold(p,4);}
  void draw(uint16_t schema,const uint8_t* p,uint16_t bytes,uint32_t type,uint32_t primitives,uint32_t vertices,uint32_t start) {
    word(1);word(schema);word(bytes);fold(p,bytes);word(type);word(primitives);word(vertices);word(start);++drawRecords;
  }
  void scene(uint32_t schema,const uint8_t* p,uint32_t bytes) {word(2);word(schema);word(bytes);fold(p,bytes);++frameScenes;}
  void stage(uint32_t value) {word(3);word(value);++stageMarkers;}
};
} // namespace warvk::host::d9
