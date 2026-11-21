#pragma once
#include "d3d9_draw_wire.h"
#include "d3d9_ff_wire.h"
#include "../../src/d3d9/war3/effects/war3_shadow_scene.h"
#include <cmath>

// x86 data-layer facts for the host's enhancement executors, negotiated at
// Initialize (args[6] bit1). Records carry values, never game or COM pointers.
// - FrameScene: one schema record per message: the camera, one record per
//   effect executor (war3_effect_scene.h owns those layouts), and the shadow
//   pass's Lighting and ShadowFrame (war3_shadow_scene.h). The records of a
//   frame form its scene packet; nothing carries over to the next frame.
// - StageMarker: an ordered insertion point between draws. WorldSceneEnter and
//   WorldSceneExit bracket the game's world RenderScene and pair strictly.
//   BeforeUi is where the host runs the BeforeUi effect executors: once, after
//   the world scene completed and before anything else is drawn. FrameEnd ends
//   the frame's scene packet.
// - SessionReset (d3d9_wire.h): the map changed or the game ended; not a
//   position in the draw stream but a fact applied at the next Present.
// - AnnotateDraws: records for the draws of the message that immediately
//   follows it. Any other next message is refused and the records are dropped.
// - GeometryRef / BonesRef records name uploaded content (d3d9_content_wire.h)
//   by {key, revision} instead of carrying it.
// - ShadowCaster records (war3_shadow_scene.h) mark their draw as a shadow
//   caster; its geometry is the draw's own buffers. A ShadowCasterAlpha record
//   directly after it, on the same draw, makes the caster alpha-tested.
namespace warvk::host::d9 {
enum class AnnotationSchema : uint16_t { DrawTag=1, FrameCamera=2, GeometryRef=3, BonesRef=4, Ssao=5, WeatherSnow=6, Aa=7, Bloom=8,
  Lighting=9, ShadowFrame=10, ShadowCaster=11, ShadowCasterAlpha=12, PointLights=13, Outline=14,
  VolumetricLight=15, FogVolumes=16 };
namespace scene = dxvk::war3::effects::scene;
static_assert(uint32_t(AnnotationSchema::FrameCamera)==scene::CameraSchema&&uint32_t(AnnotationSchema::Ssao)==scene::SsaoSchema
  &&uint32_t(AnnotationSchema::WeatherSnow)==scene::SnowSchema&&uint32_t(AnnotationSchema::Aa)==scene::AaSchema
  &&uint32_t(AnnotationSchema::Bloom)==scene::BloomSchema&&uint32_t(AnnotationSchema::Lighting)==scene::LightingSchema
  &&uint32_t(AnnotationSchema::ShadowFrame)==scene::ShadowFrameSchema
  &&uint32_t(AnnotationSchema::ShadowCaster)==scene::ShadowCasterSchema
  &&uint32_t(AnnotationSchema::ShadowCasterAlpha)==scene::ShadowCasterAlphaSchema
  &&uint32_t(AnnotationSchema::PointLights)==scene::PointLightsSchema
  &&uint32_t(AnnotationSchema::Outline)==scene::OutlineSchema
  &&uint32_t(AnnotationSchema::VolumetricLight)==scene::VolumetricLightSchema
  &&uint32_t(AnnotationSchema::FogVolumes)==scene::FogVolumesSchema,"scene packet schema numbers");
// Provisional v1 vocabulary. A consumer that needs more adds a schema rather
// than reinterpreting these fields. object is a data-layer instance key.
struct DrawTag { uint64_t object=0; uint32_t kind=0,flags=0; };
struct FrameCamera {
  std::array<float,16> view{},projection{};
  std::array<float,4> viewport{};
  std::array<float,2> depthRange{};
  uint32_t frame=0;float gameTime=0;
};
constexpr uint32_t DrawTagBytes=16,FrameCameraBytes=(16+16+4+2)*4+8,ContentRefBytes=16;
static_assert(FrameCameraBytes==scene::CameraBytes,"one camera layout");
// Stage markers and the world-scene rule are shared with the in-process device.
using Stage=scene::Stage;using scene::ValidStage;using SceneTracker=scene::SceneTracker;
constexpr uint32_t RecordHeaderBytes=8,MaxAnnotationData=256,MaxRecordsPerDraw=4;
static_assert(scene::ShadowCasterBytes<=MaxAnnotationData,"a draw record fits the record bound");
constexpr uint32_t MaxDrawAnnotationBytes=MaxDrawBatch*MaxRecordsPerDraw*(RecordHeaderBytes+MaxAnnotationData);
static_assert(MaxDrawAnnotationBytes<=MaxPayload,"one batch's records must fit one bounded packet");
static_assert(MaxFixedDrawBatch<=MaxDrawBatch,"fixed batches reuse the programmable record bound");

inline uint32_t RecordBytes(uint32_t data) { return RecordHeaderBytes+((data+3)&~3u); }
// The size rule of a draw record. A ShadowCaster's values are checked where
// the record is made (AppendDrawRecord) and where it arrives (DecodeDrawAnnotations).
inline bool ValidDrawRecord(uint32_t schema,uint32_t bytes) {
  if(schema==uint32_t(AnnotationSchema::GeometryRef)||schema==uint32_t(AnnotationSchema::BonesRef))return bytes==ContentRefBytes;
  if(schema==uint32_t(AnnotationSchema::ShadowCaster))return bytes==scene::ShadowCasterBytes;
  if(schema==uint32_t(AnnotationSchema::ShadowCasterAlpha))return bytes==scene::ShadowCasterAlphaBytes;
  return schema==uint32_t(AnnotationSchema::DrawTag)&&bytes==DrawTagBytes;
}
inline bool ValidDrawRecordData(uint32_t schema,const uint8_t* data,uint32_t bytes) {
  if(schema==uint32_t(AnnotationSchema::ShadowCasterAlpha)) {
    scene::ShadowCasterAlpha ignored;return scene::DecodeShadowCasterAlpha(data,bytes,ignored);
  }
  if(schema!=uint32_t(AnnotationSchema::ShadowCaster))return true;
  scene::ShadowCaster ignored;return scene::DecodeShadowCaster(data,bytes,ignored);
}
// A caster's companion record directly follows the caster's own record on
// the same draw. previousSchema: the draw's record before it, 0 for none.
inline bool CompanionFollowsCaster(uint32_t schema,uint32_t previousSchema) {
  return schema!=uint32_t(AnnotationSchema::ShadowCasterAlpha)||previousSchema==uint32_t(AnnotationSchema::ShadowCaster);
}
// The draw message an annotation may bind to, and its draw-count bound.
inline uint32_t AnnotatedDrawCapacity(uint32_t op) {
  switch(Op(op)) {
    case Op::Draw: case Op::DrawFixed: return 1;
    case Op::DrawBatch: return MaxDrawBatch;
    case Op::DrawFixedBatch: case Op::DrawFixedDeltaBatch: return MaxFixedDrawBatch;
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
  if(schema==uint32_t(AnnotationSchema::Lighting)||schema==uint32_t(AnnotationSchema::ShadowFrame)
    ||schema==uint32_t(AnnotationSchema::PointLights)||schema==uint32_t(AnnotationSchema::Outline)
    ||schema==uint32_t(AnnotationSchema::VolumetricLight)||schema==uint32_t(AnnotationSchema::FogVolumes))
    return scene::ValidShadowRecord(schema,p,bytes);
  if(schema!=uint32_t(AnnotationSchema::FrameCamera))return scene::ValidEffectRecord(schema,p,bytes);
  if(bytes!=FrameCameraBytes||!p)return false;
  for(uint32_t i=0;i<FrameCameraBytes/4;++i)
    if(i!=38&&!std::isfinite(Float(U32(p+4*i))))return false; // word 38 is the frame number
  return true;
}
// Appends one record; the caller keeps records sorted by (index, schema).
inline uint32_t AppendDrawRecord(uint8_t* out,uint32_t capacity,uint16_t index,uint16_t schema,const void* data,uint16_t bytes) {
  const auto size=RecordBytes(bytes);
  if(!out||!data||size>capacity||!ValidDrawRecord(schema,bytes)
    ||!ValidDrawRecordData(schema,static_cast<const uint8_t*>(data),bytes))return 0;
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
    if(p[at+6]||p[at+7]||index>=h.args[1]||!ValidDrawRecord(schema,bytes)||size>h.payloadBytes-at
      ||!ValidDrawRecordData(schema,p+at+RecordHeaderBytes,bytes))return false;
    const uint32_t key=(index<<16)|schema;
    if(count&&key<=previous)return false;
    if(!CompanionFollowsCaster(schema,count&&(previous>>16)==index?(previous&0xffff):0))return false;
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
  uint64_t annotatedDraws=0,drawRecords=0,frameScenes=0,stageMarkers=0,sessionResets=0,digest=Fnv1a64Offset;
  uint64_t contentUploads=0,contentReleases=0;
  void fold(const uint8_t* p,size_t bytes) {digest=FoldFnv1a64(p,bytes,digest);}
  void word(uint32_t v) {uint8_t p[4];P32(p,v);fold(p,4);}
  void draw(uint16_t schema,const uint8_t* p,uint16_t bytes,uint32_t type,uint32_t primitives,uint32_t vertices,uint32_t start) {
    word(1);word(schema);word(bytes);fold(p,bytes);word(type);word(primitives);word(vertices);word(start);++drawRecords;
  }
  void scene(uint32_t schema,const uint8_t* p,uint32_t bytes) {word(2);word(schema);word(bytes);fold(p,bytes);++frameScenes;}
  void stage(uint32_t value) {word(3);word(value);++stageMarkers;}
  void reset(uint32_t reason,uint64_t generation) {
    word(4);word(reason);word(uint32_t(generation));word(uint32_t(generation>>32));++sessionResets;
  }
  // A committed blob: its identity, then every byte in order.
  void content(uint32_t kind,uint64_t key,uint64_t revision,const uint8_t* p,uint32_t bytes) {
    word(5);word(kind);word(uint32_t(key));word(uint32_t(key>>32));word(uint32_t(revision));word(uint32_t(revision>>32));
    word(bytes);fold(p,bytes);++contentUploads;
  }
  void release(uint64_t key,uint64_t revision) {
    word(6);word(uint32_t(key));word(uint32_t(key>>32));word(uint32_t(revision));word(uint32_t(revision>>32));++contentReleases;
  }
};
} // namespace warvk::host::d9
