#pragma once
#include "../tools/render_host/d3d9_annotation_wire.h"
#include <limits>
#include <vector>
// Annotation records, their binding to the next draw message, and the
// recorder digest, independently of any connection, host or GPU.
namespace warvk_annotation_test {
using namespace warvk::host;
inline std::vector<uint8_t> Records(std::initializer_list<std::pair<uint16_t,d9::DrawTag>> entries) {
  std::vector<uint8_t> out(entries.size()*d9::RecordBytes(d9::DrawTagBytes));uint32_t at=0;
  for(const auto& [index,tag]:entries){const auto bytes=d9::EncodeDrawTag(tag);
    at+=d9::AppendDrawRecord(out.data()+at,uint32_t(out.size()-at),index,uint16_t(d9::AnnotationSchema::DrawTag),bytes.data(),d9::DrawTagBytes);}
  out.resize(at);return out;
}
inline d9::Header Annotate(uint32_t records,uint32_t draws,d9::Op op,uint32_t bytes) {
  d9::Header h;h.op=d9::Op::AnnotateDraws;h.args[0]=records;h.args[1]=draws;h.args[2]=uint32_t(op);h.payloadBytes=bytes;return h;
}
template<class Check> void Run(Check check) {
  const auto tagSchema=uint16_t(d9::AnnotationSchema::DrawTag);
  check(d9::RecordBytes(16)==24&&d9::RecordBytes(13)==24&&d9::RecordBytes(0)==8);
  check(d9::AnnotatedDrawCapacity(uint32_t(d9::Op::Draw))==1&&d9::AnnotatedDrawCapacity(uint32_t(d9::Op::DrawFixed))==1
    &&d9::AnnotatedDrawCapacity(uint32_t(d9::Op::DrawBatch))==d9::MaxDrawBatch
    &&d9::AnnotatedDrawCapacity(uint32_t(d9::Op::DrawFixedBatch))==d9::MaxFixedDrawBatch
    &&!d9::AnnotatedDrawCapacity(uint32_t(d9::Op::Clear))&&!d9::AnnotatedDrawCapacity(uint32_t(d9::Op::AnnotateDraws)));
  for(uint32_t stage=0;stage<7;++stage)check(d9::ValidStage(stage)==(stage>=1&&stage<=4));
  // FrameCamera: exact size and schema, every float finite; word 38 is the frame number.
  d9::FrameCamera camera;camera.view[0]=1;camera.projection[5]=2;camera.viewport={0,0,64,64};camera.depthRange={0,1};
  camera.frame=0x7fc00000u;camera.gameTime=3.5f;
  const auto scene=d9::EncodeFrameCamera(camera);const auto sceneSchema=uint32_t(d9::AnnotationSchema::FrameCamera);
  check(scene.size()==160&&d9::U32(scene.data()+38*4)==0x7fc00000u&&d9::Float(d9::U32(scene.data()+39*4))==3.5f);
  check(d9::ValidFrameScene(sceneSchema,scene.data(),160));
  check(!d9::ValidFrameScene(sceneSchema,scene.data(),159)&&!d9::ValidFrameScene(sceneSchema,nullptr,160)
    &&!d9::ValidFrameScene(tagSchema,scene.data(),160)&&!d9::ValidFrameScene(0,scene.data(),160));
  for(uint32_t word=0;word<40;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}) {
    auto broken=scene;d9::P32(broken.data()+word*4,bad);check(d9::ValidFrameScene(sceneSchema,broken.data(),160)==(word==38));
  }
  // Record encoding.
  const auto tag=d9::EncodeDrawTag({0x1122334455667788ull,7,9});
  check(d9::U64(tag.data())==0x1122334455667788ull&&d9::U32(tag.data()+8)==7&&d9::U32(tag.data()+12)==9);
  std::array<uint8_t,64> one{};one.fill(0xee);
  check(d9::AppendDrawRecord(one.data(),64,3,tagSchema,tag.data(),16)==24);
  check(one[0]==3&&one[1]==0&&one[2]==1&&one[3]==0&&one[4]==16&&one[5]==0&&one[6]==0&&one[7]==0
    &&std::equal(tag.begin(),tag.end(),one.begin()+8)&&one[24]==0xee);
  check(!d9::AppendDrawRecord(one.data(),23,0,tagSchema,tag.data(),16)&&!d9::AppendDrawRecord(one.data(),64,0,2,tag.data(),16)
    &&!d9::AppendDrawRecord(one.data(),64,0,tagSchema,tag.data(),12)&&!d9::AppendDrawRecord(one.data(),64,0,tagSchema,nullptr,16)
    &&!d9::AppendDrawRecord(nullptr,64,0,tagSchema,tag.data(),16));
  // A fixed batch of three draws with records for draws 0 and 2.
  const auto valid=Records({{0,{11,1,2}},{2,{13,3,4}}});
  d9::DrawAnnotations decoded,untouched;
  auto h=Annotate(2,3,d9::Op::DrawFixedBatch,uint32_t(valid.size()));
  check(d9::DecodeDrawAnnotations(h,valid.data(),decoded)&&decoded.draw==d9::Op::DrawFixedBatch
    &&decoded.draws==3&&decoded.count==2&&decoded.bytes==48);
  std::vector<std::pair<uint16_t,uint64_t>> seen;
  decoded.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes){
    check(schema==tagSchema&&bytes==16);seen.emplace_back(index,d9::U64(data));});
  check((seen==std::vector<std::pair<uint16_t,uint64_t>>{{0,11},{2,13}}));
  d9::Header next;next.op=d9::Op::DrawFixedBatch;next.args[0]=3;check(decoded.binds(next));
  next.args[0]=2;check(!decoded.binds(next));next.args[0]=3;next.op=d9::Op::DrawBatch;check(!decoded.binds(next));
  next.op=d9::Op::DrawFixed;check(!decoded.binds(next));next.op=d9::Op::StageMarker;check(!decoded.binds(next));
  // Single draws bind with no count argument.
  {const auto single=Records({{0,{5,0,0}}});d9::DrawAnnotations s;
    check(d9::DecodeDrawAnnotations(Annotate(1,1,d9::Op::DrawFixed,24),single.data(),s));
    d9::Header draw;draw.op=d9::Op::DrawFixed;check(s.binds(draw));draw.op=d9::Op::Draw;check(!s.binds(draw));
    check(d9::DecodeDrawAnnotations(Annotate(1,1,d9::Op::Draw,24),single.data(),s));draw.op=d9::Op::Draw;check(s.binds(draw));
    check(!d9::DecodeDrawAnnotations(Annotate(1,2,d9::Op::DrawFixed,24),single.data(),s));}
  // A full programmable batch: one record per draw.
  {std::vector<uint8_t> all;for(uint16_t i=0;i<d9::MaxDrawBatch;++i){const auto r=Records({{i,{i,0,0}}});all.insert(all.end(),r.begin(),r.end());}
    d9::DrawAnnotations full;check(d9::DecodeDrawAnnotations(Annotate(d9::MaxDrawBatch,d9::MaxDrawBatch,d9::Op::DrawBatch,uint32_t(all.size())),all.data(),full));
    check(!d9::DecodeDrawAnnotations(Annotate(d9::MaxDrawBatch,d9::MaxDrawBatch+1,d9::Op::DrawBatch,uint32_t(all.size())),all.data(),full));}
  // Every refusal leaves the destination untouched.
  auto refused=[&](d9::Header c,const std::vector<uint8_t>& payload){
    const auto before=untouched.data;check(!d9::DecodeDrawAnnotations(c,payload.data(),untouched)&&untouched.data==before&&!untouched.count);};
  for(uint32_t bytes=0;bytes<valid.size();++bytes)refused(Annotate(2,3,d9::Op::DrawFixedBatch,bytes),valid);
  {auto c=h;c.args[0]=1;refused(c,valid);c.args[0]=3;refused(c,valid);c.args[0]=0;refused(c,valid);}
  {auto c=h;c.args[1]=0;refused(c,valid);c.args[1]=2;refused(c,valid);c.args[1]=d9::MaxFixedDrawBatch+1;refused(c,valid);}
  for(const auto op:{d9::Op::Clear,d9::Op::AnnotateDraws,d9::Op::Readback,d9::Op::Initialize}){auto c=h;c.args[2]=uint32_t(op);refused(c,valid);}
  for(size_t i=3;i<h.args.size();++i){auto c=h;c.args[i]=1;refused(c,valid);}
  {auto c=h;c.id=1;c.generation=1;refused(c,valid);c=h;c.op=d9::Op::FrameScene;refused(c,valid);}
  {auto bad=valid;bad[6]=1;refused(h,bad);bad=valid;bad[24+7]=1;refused(h,bad);}           // reserved
  {auto bad=valid;bad[24]=3;refused(h,bad);}                                               // index beyond the batch
  {auto bad=valid;bad[2]=2;refused(h,bad);bad=valid;bad[2]=0;refused(h,bad);}             // schema
  {auto bad=valid;bad[4]=12;refused(h,bad);}                                               // DrawTag size
  {const auto swapped=Records({{2,{13,3,4}},{0,{11,1,2}}});refused(h,swapped);}             // order
  {const auto duplicate=Records({{0,{11,1,2}},{0,{12,1,2}}});refused(h,duplicate);}         // duplicate key
  {auto longer=valid;longer.insert(longer.end(),4,0);auto c=h;c.payloadBytes+=4;refused(c,longer);}
  {auto c=h;c.payloadBytes=d9::MaxDrawAnnotationBytes+4;std::vector<uint8_t> big(c.payloadBytes);refused(c,big);}
  // The digest depends on order, payload and draw shape.
  d9::AnnotationRecorder a,b,c2,d;
  a.draw(tagSchema,tag.data(),16,D3DPT_TRIANGLELIST,2,6,0);a.stage(2);
  b.draw(tagSchema,tag.data(),16,D3DPT_TRIANGLELIST,2,6,0);b.stage(2);
  c2.stage(2);c2.draw(tagSchema,tag.data(),16,D3DPT_TRIANGLELIST,2,6,0);
  d.draw(tagSchema,tag.data(),16,D3DPT_TRIANGLELIST,2,6,3);d.stage(2);
  check(a.digest==b.digest&&a.digest!=c2.digest&&a.digest!=d.digest&&a.drawRecords==1&&a.stageMarkers==1);
  a.scene(sceneSchema,scene.data(),160);check(a.frameScenes==1&&a.digest!=b.digest);
}
} // namespace warvk_annotation_test
