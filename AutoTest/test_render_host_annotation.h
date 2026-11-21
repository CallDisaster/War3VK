#pragma once
#include "../tools/render_host/d3d9_annotation_wire.h"
#include <array>
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
  for(uint32_t stage=0;stage<9;++stage)check(d9::ValidStage(stage)==(stage>=1&&stage<=6));
  check(uint32_t(d9::Stage::WorldSceneEnter)==5&&uint32_t(d9::Stage::WorldSceneExit)==6);
  // World scene markers pair strictly; the others never touch the scene.
  {d9::SceneTracker scene;const auto enter=uint32_t(d9::Stage::WorldSceneEnter),exit=uint32_t(d9::Stage::WorldSceneExit);
    check(!scene.mark(exit,0)&&!scene.active&&!scene.exits&&!scene.completedThisFrame);
    for(const auto other:{d9::Stage::WorldBegin,d9::Stage::BeforeUi,d9::Stage::UiBegin,d9::Stage::FrameEnd})
      check(scene.mark(uint32_t(other),0)&&!scene.active&&!scene.enters);
    check(scene.mark(enter,10)&&scene.active&&scene.enters==1&&!scene.completedThisFrame&&!scene.drawsAfterScene(15));
    check(!scene.mark(enter,11)&&scene.enters==1);
    // A session never changes inside a world scene.
    check(!scene.reset(1)&&!scene.generation&&!scene.resets);
    check(scene.mark(exit,25)&&!scene.active&&scene.exits==1&&scene.completedThisFrame&&scene.drawsAtExit==25);
    check(scene.drawsAfterScene(25)==0&&scene.drawsAfterScene(31)==6);
    // A second scene in one frame: the count restarts at its exit.
    check(scene.mark(enter,31)&&!scene.completedThisFrame&&!scene.drawsAfterScene(40));
    check(scene.mark(exit,44)&&scene.drawsAfterScene(47)==3&&scene.enters==2&&scene.exits==2);
    scene.frameEnd();check(!scene.completedThisFrame&&!scene.active&&!scene.drawsAfterScene(60));
    // An open scene stays open across a frame end, as the x86 flag does.
    check(scene.mark(enter,60));scene.frameEnd();check(scene.active&&!scene.mark(enter,61)&&scene.mark(exit,62));
    // Generations only ascend.
    check(!scene.reset(0)&&scene.reset(3)&&scene.generation==3&&scene.resets==1);
    check(!scene.reset(3)&&!scene.reset(2)&&scene.reset(uint64_t(1)<<32)&&scene.resets==2&&scene.generation==uint64_t(1)<<32);
    check(scene.completedThisFrame); // The frame that is ending keeps its scene.
  }
  // The recorder folds a reset as (4, reason, generation low, generation high).
  {d9::AnnotationRecorder a,b;a.reset(1,(uint64_t(7)<<32)|9);b.word(4);b.word(1);b.word(9);b.word(7);
    check(a.digest==b.digest&&a.sessionResets==1&&!a.stageMarkers);
    d9::AnnotationRecorder c;c.reset(2,(uint64_t(7)<<32)|9);check(c.digest!=a.digest);}
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
  // --- Scene packet of the effect executors (war3_effect_scene.h) ---
  namespace fx=d9::scene;
  check(fx::CameraBytes==160&&fx::SsaoBytes==24&&fx::SnowBytes==76&&fx::MaxRecordBytes==160);
  check(uint32_t(d9::AnnotationSchema::Ssao)==5&&uint32_t(d9::AnnotationSchema::WeatherSnow)==6);
  // One camera layout: the wire encoder's bytes decode to the same values and
  // re-encode to the same bytes.
  {fx::Camera decoded;check(fx::DecodeCamera(scene.data(),160,decoded)&&decoded.view[0]==1&&decoded.projection[5]==2
      &&!decoded.x&&!decoded.y&&decoded.width==64&&decoded.height==64&&decoded.minZ==0&&decoded.maxZ==1
      &&decoded.frame==0x7fc00000u&&decoded.gameTime==3.5f);
    check(fx::EncodeCamera(decoded)==scene);
    check(!fx::DecodeCamera(scene.data(),159,decoded)&&!fx::DecodeCamera(nullptr,160,decoded));
    // The transport carries any finite camera; an executor only takes whole
    // pixels within the bound and a viewport that is not empty.
    for(const float edge:{63.5f,0.0f,-1.0f,16385.0f}){auto c=camera;c.viewport={0,0,edge,64};const auto bytes=d9::EncodeFrameCamera(c);
      fx::Camera ignored;check(d9::ValidFrameScene(sceneSchema,bytes.data(),160)&&!fx::DecodeCamera(bytes.data(),160,ignored));}
    {auto c=camera;c.viewport={3,5,16384,1};const auto bytes=d9::EncodeFrameCamera(c);fx::Camera edge;
      check(fx::DecodeCamera(bytes.data(),160,edge)&&edge.x==3&&edge.y==5&&edge.width==16384&&edge.height==1);}}
  // SSAO: six floats, in this order, as written by hand.
  const std::array<uint8_t,24> ssaoBytes={0,0,0,0x41, 0,0,0,0x3f, 0,0,0x80,0x3e, 0,0,0,0x40, 0,0,0x80,0x3f, 0,0,0x80,0x40};
  {fx::Ssao s;s.radiusPx=8;s.strength=0.5f;s.bias=0.25f;s.power=2;s.fadeNear=1;s.fadeFar=4;
    check(fx::EncodeSsao(s)==ssaoBytes);
    fx::Ssao back;check(fx::DecodeSsao(ssaoBytes.data(),24,back)&&back.radiusPx==8&&back.strength==0.5f&&back.bias==0.25f
      &&back.power==2&&back.fadeNear==1&&back.fadeFar==4);
    check(!fx::DecodeSsao(ssaoBytes.data(),23,back)&&!fx::DecodeSsao(ssaoBytes.data(),28,back)&&!fx::DecodeSsao(nullptr,24,back));
    check(d9::ValidFrameScene(5,ssaoBytes.data(),24)&&!d9::ValidFrameScene(6,ssaoBytes.data(),24)
      &&!d9::ValidFrameScene(7,ssaoBytes.data(),24)&&!d9::ValidFrameScene(5,ssaoBytes.data(),20));
    for(uint32_t word=0;word<6;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){
      auto broken=ssaoBytes;d9::P32(broken.data()+word*4,bad);check(!d9::ValidFrameScene(5,broken.data(),24));}}
  // Snow: eleven floats, three doubles, the seed and the weather id.
  const std::array<uint8_t,76> snowBytes={
    0,0,0x80,0x3f, 0,0,0,0x3f, 0,0,0x80,0x3e, 0,0,0,0x40,           // snowfall 1, gust .5, haze .25, lowSnow 2
    0,0,0x80,0xbf, 0,0,0,0xc0, 0,0,0x80,0x40, 0,0,0,0x41,           // region -1,-2 .. 4,8
    0,0,0,0, 0,0,0x80,0x41, 0,0,0,0x3f,                             // bottomZ 0, topZ 16, edgeFeather .5
    0,0,0,0,0,0,0xf8,0x3f, 0,0,0,0,0,0,0,0xc0, 0,0,0,0,0,0,0xe0,0x3f, // time 1.5, wind displacement -2, .5
    0x44,0x33,0x22,0x11, 0xfe,0xff,0xff,0xff};                      // seed, id -2
  {fx::Snow s;s.snowfall=1;s.gust=0.5f;s.haze=0.25f;s.lowSnow=2;s.minX=-1;s.minY=-2;s.maxX=4;s.maxY=8;s.bottomZ=0;s.topZ=16;
    s.edgeFeather=0.5f;s.timeSeconds=1.5;s.windDisplacementX=-2.0;s.windDisplacementY=0.5;s.seed=0x11223344u;s.id=-2;
    check(fx::EncodeSnow(s)==snowBytes);
    fx::Snow back;check(fx::DecodeSnow(snowBytes.data(),76,back)&&back.snowfall==1&&back.gust==0.5f&&back.haze==0.25f&&back.lowSnow==2
      &&back.minX==-1&&back.minY==-2&&back.maxX==4&&back.maxY==8&&back.bottomZ==0&&back.topZ==16&&back.edgeFeather==0.5f
      &&back.timeSeconds==1.5&&back.windDisplacementX==-2.0&&back.windDisplacementY==0.5&&back.seed==0x11223344u&&back.id==-2);
    check(!fx::DecodeSnow(snowBytes.data(),72,back)&&!fx::DecodeSnow(snowBytes.data(),80,back)&&!fx::DecodeSnow(nullptr,76,back));
    check(d9::ValidFrameScene(6,snowBytes.data(),76)&&!d9::ValidFrameScene(5,snowBytes.data(),76)&&!d9::ValidFrameScene(6,snowBytes.data(),24));
    for(uint32_t word=0;word<11;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){
      auto broken=snowBytes;d9::P32(broken.data()+word*4,bad);check(!d9::ValidFrameScene(6,broken.data(),76));}
    // A double is not finite when its high word says so.
    for(const uint32_t high:{48u,56u,64u})for(const uint32_t bad:{0x7ff80000u,0x7ff00000u,0xfff00000u}){
      auto broken=snowBytes;d9::P32(broken.data()+high,bad);check(!d9::ValidFrameScene(6,broken.data(),76));}
    // The seed is any word; id 0 names no weather object.
    {auto any=snowBytes;d9::P32(any.data()+68,0x7fc00000u);check(d9::ValidFrameScene(6,any.data(),76));
      d9::P32(any.data()+72,0);check(!d9::ValidFrameScene(6,any.data(),76));d9::P32(any.data()+72,1);check(d9::ValidFrameScene(6,any.data(),76));}}
  // AA: the mode, four floats and two search step counts, as written by hand.
  const std::array<uint8_t,28> aaBytes={5,0,0,0, 0,0,0x40,0x3f, 0,0,0x80,0x3e, 0,0,0,0x3e, 0,0,0,0x3f,
    0x20,0,0,0, 0xff,0xff,0xff,0xff};
  {fx::Aa a;a.mode=5;a.fxaaQualitySubpix=0.75f;a.fxaaQualityEdgeThreshold=0.25f;a.fxaaQualityEdgeThresholdMin=0.125f;
    a.smaaThreshold=0.5f;a.smaaMaxSearchSteps=32;a.smaaMaxSearchStepsDiag=-1;
    check(fx::EncodeAa(a)==aaBytes);
    fx::Aa back;check(fx::DecodeAa(aaBytes.data(),28,back)&&back.mode==5&&back.fxaaQualitySubpix==0.75f
      &&back.fxaaQualityEdgeThreshold==0.25f&&back.fxaaQualityEdgeThresholdMin==0.125f&&back.smaaThreshold==0.5f
      &&back.smaaMaxSearchSteps==32&&back.smaaMaxSearchStepsDiag==-1);
    check(!fx::DecodeAa(aaBytes.data(),24,back)&&!fx::DecodeAa(aaBytes.data(),32,back)&&!fx::DecodeAa(nullptr,28,back));
    check(d9::ValidFrameScene(7,aaBytes.data(),28)&&!d9::ValidFrameScene(8,aaBytes.data(),28));
    // Modes 1 (FXAA) to 5 (SMAA Ultra); no record means no AA.
    for(const uint32_t mode:{0u,6u,0xffffffffu}){auto broken=aaBytes;d9::P32(broken.data(),mode);check(!d9::ValidFrameScene(7,broken.data(),28));}
    for(const uint32_t mode:{1u,2u,3u,4u}){auto good=aaBytes;d9::P32(good.data(),mode);check(d9::ValidFrameScene(7,good.data(),28));}
    for(uint32_t word=1;word<5;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){
      auto broken=aaBytes;d9::P32(broken.data()+word*4,bad);check(!d9::ValidFrameScene(7,broken.data(),28));}}
  // Bloom: four floats and two flags.
  const std::array<uint8_t,24> bloomBytes={0,0,0xc0,0x3f, 0,0,0,0x3f, 0,0,0x80,0x3e, 0,0,0,0x40, 1,0,0,0, 0,0,0,0};
  {fx::Bloom b;b.threshold=1.5f;b.softKnee=0.5f;b.intensity=0.25f;b.exposure=2;b.acesToneMap=1;b.useSrgb=0;
    check(fx::EncodeBloom(b)==bloomBytes);
    fx::Bloom back;check(fx::DecodeBloom(bloomBytes.data(),24,back)&&back.threshold==1.5f&&back.softKnee==0.5f
      &&back.intensity==0.25f&&back.exposure==2&&back.acesToneMap==1&&back.useSrgb==0);
    check(!fx::DecodeBloom(bloomBytes.data(),20,back)&&!fx::DecodeBloom(bloomBytes.data(),28,back)&&!fx::DecodeBloom(nullptr,24,back));
    check(d9::ValidFrameScene(8,bloomBytes.data(),24)&&!d9::ValidFrameScene(7,bloomBytes.data(),24));
    for(uint32_t word=0;word<4;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){
      auto broken=bloomBytes;d9::P32(broken.data()+word*4,bad);check(!d9::ValidFrameScene(8,broken.data(),24));}
    for(const uint32_t at:{16u,20u}){auto broken=bloomBytes;d9::P32(broken.data()+at,2);check(!d9::ValidFrameScene(8,broken.data(),24));}}
  // The frame packet: latest record per schema, refusals change nothing, an
  // unusable camera drops the previous one.
  {fx::FramePacket packet;check(!packet.effects()&&!packet.hasCamera);
    check(packet.accept(2,scene.data(),160)&&packet.hasCamera&&!packet.effects()&&packet.camera.width==64);
    check(packet.accept(5,ssaoBytes.data(),24)&&packet.effects()&&packet.hasSsao&&!packet.hasSnow&&packet.ssao.radiusPx==8);
    auto later=ssaoBytes;later[3]=0x40;check(packet.accept(5,later.data(),24)&&packet.ssao.radiusPx==2);
    check(!packet.accept(5,ssaoBytes.data(),23)&&packet.hasSsao&&packet.ssao.radiusPx==2);
    check(!packet.accept(9,ssaoBytes.data(),24)&&!packet.accept(1,ssaoBytes.data(),24)&&!packet.accept(0,nullptr,0));
    check(packet.accept(6,snowBytes.data(),76)&&packet.hasSnow&&packet.snow.id==-2&&packet.snow.seed==0x11223344u&&packet.hasCamera);
    {auto c=camera;c.viewport={0,0,63.5f,64};const auto bytes=d9::EncodeFrameCamera(c);
      check(!packet.accept(2,bytes.data(),160)&&!packet.hasCamera&&packet.effects());}
    check(packet.accept(7,aaBytes.data(),28)&&packet.hasAa&&packet.aa.mode==5&&packet.aa.smaaMaxSearchStepsDiag==-1);
    check(packet.accept(8,bloomBytes.data(),24)&&packet.hasBloom&&packet.bloom.acesToneMap==1&&packet.bloom.exposure==2);
    packet.clear();check(!packet.hasCamera&&!packet.hasSsao&&!packet.hasSnow&&!packet.hasAa&&!packet.hasBloom&&!packet.effects());}
  // AA or bloom alone is an effect to run.
  {fx::FramePacket packet;check(packet.accept(7,aaBytes.data(),28)&&packet.effects());packet.clear();
    check(packet.accept(8,bloomBytes.data(),24)&&packet.effects());}
  // BeforeUi effects: once per completed world scene, never inside one.
  {d9::SceneTracker t;const auto enter=uint32_t(d9::Stage::WorldSceneEnter),exit=uint32_t(d9::Stage::WorldSceneExit);
    check(uint32_t(d9::Stage::BeforeUi)==2&&uint32_t(d9::Stage::FrameEnd)==4);
    check(!t.canInsertBeforeUi());
    check(t.mark(enter,0)&&!t.canInsertBeforeUi());
    check(t.mark(exit,5)&&t.canInsertBeforeUi());
    // The marker itself changes nothing; the owner records the insertion.
    check(t.mark(uint32_t(d9::Stage::BeforeUi),5)&&t.canInsertBeforeUi()&&!t.insertions);
    t.insertedBeforeUi();check(!t.canInsertBeforeUi()&&t.insertions==1&&t.completedThisFrame);
    // A second scene in the frame has its own insertion.
    check(t.mark(enter,6)&&!t.canInsertBeforeUi()&&t.mark(exit,9)&&t.canInsertBeforeUi());
    t.insertedBeforeUi();check(t.insertions==2&&!t.canInsertBeforeUi());
    t.frameEnd();check(!t.canInsertBeforeUi());
    // A scene whose frame ended without an insertion does not carry over.
    check(t.mark(enter,10)&&t.mark(exit,12)&&t.canInsertBeforeUi());t.frameEnd();check(!t.canInsertBeforeUi()&&t.insertions==2);}
  // ---- shadow records (phase 4c) ----
  // ShadowFrame: fourteen words, as written by hand.
  const std::array<uint8_t,56> shadowFrameBytes={0,0,0x40,0x41, 5,0,0,0, 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11, 3,0,0,0,0,0,0,0,
    7,0,0,0, 2,0,0,0, 0x81,0,0,0, 4,0,0,0, 9,0,0,0, 0,0,0,0, 0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe};
  {fx::ShadowFrame f;f.timeOfDay=12;f.runtimeDisabledModules=5;f.tombstoneSerial=0x1122334455667788ull;f.stagePolicyRevision=3;
    f.casters=7;f.omittedCasters=2;f.omissionReasons=0x81;f.dynamicPoseCount=4;f.dynamicSkinnedOutputCount=9;
    f.dynamicPoseSignature=0xfedcba9876543210ull;
    check(fx::ShadowFrameSchema==10&&fx::ShadowFrameBytes==56&&fx::EncodeShadowFrame(f)==shadowFrameBytes);
    fx::ShadowFrame back;check(fx::DecodeShadowFrame(shadowFrameBytes.data(),56,back)&&back.timeOfDay==12&&back.runtimeDisabledModules==5
      &&back.tombstoneSerial==0x1122334455667788ull&&back.stagePolicyRevision==3&&back.casters==7&&back.omittedCasters==2
      &&back.omissionReasons==0x81&&back.dynamicPoseCount==4&&back.dynamicSkinnedOutputCount==9
      &&back.dynamicPoseSignature==0xfedcba9876543210ull);
    check(!fx::DecodeShadowFrame(shadowFrameBytes.data(),52,back)&&!fx::DecodeShadowFrame(shadowFrameBytes.data(),60,back)
      &&!fx::DecodeShadowFrame(nullptr,56,back));
    check(d9::ValidFrameScene(10,shadowFrameBytes.data(),56)&&!d9::ValidFrameScene(9,shadowFrameBytes.data(),56)
      &&!d9::ValidFrameScene(11,shadowFrameBytes.data(),56));
    // The time is finite (negative: not readable yet); the reserved word is zero.
    for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){auto broken=shadowFrameBytes;d9::P32(broken.data(),bad);
      check(!d9::ValidFrameScene(10,broken.data(),56));}
    {auto early=shadowFrameBytes;d9::P32(early.data(),0xbf800000u);check(d9::ValidFrameScene(10,early.data(),56));}
    {auto broken=shadowFrameBytes;d9::P32(broken.data()+44,1);check(!d9::ValidFrameScene(10,broken.data(),56));}
    // Omissions and their reasons come together; only known reason bits.
    {auto broken=shadowFrameBytes;d9::P32(broken.data()+28,0);check(!d9::ValidFrameScene(10,broken.data(),56));
      d9::P32(broken.data()+32,0);check(d9::ValidFrameScene(10,broken.data(),56));
      d9::P32(broken.data()+28,1);check(!d9::ValidFrameScene(10,broken.data(),56));
      d9::P32(broken.data()+32,0x100);check(!d9::ValidFrameScene(10,broken.data(),56));}}
  // Lighting: 123 words from one table. The defaults, at offsets counted by hand.
  {const fx::Lighting defaults;const auto bytes=fx::EncodeLighting(defaults);
    check(fx::LightingSchema==9&&fx::LightingBytes==492&&bytes.size()==492);
    check(d9::U32(bytes.data())==1);                                                     // sun.enabled
    check(d9::U32(bytes.data()+4)==0xbe99999au&&d9::U32(bytes.data()+8)==0xbf800000u
      &&d9::U32(bytes.data()+12)==0xbe4ccccdu&&d9::U32(bytes.data()+16)==0);              // sun.direction -0.3, -1, -0.2, 0
    check(d9::U32(bytes.data()+48)==4096);                                                // shadows.csm.shadowResolution
    check(d9::U32(bytes.data()+92)==0x3f400000u);                                         // shadows.strength 0.75
    check(d9::U32(bytes.data()+188)==0&&d9::U32(bytes.data()+192)==0);                    // shadows.shadowTaaSettingsRevision
    check(d9::U32(bytes.data()+360)==1&&d9::U32(bytes.data()+364)==1&&d9::U32(bytes.data()+368)==0); // celestial, grading, clock
    check(d9::U32(bytes.data()+380)==1&&d9::U32(bytes.data()+384)==0);                    // dayNight.clockRevision 1
    check(d9::U32(bytes.data()+468)==0&&d9::U32(bytes.data()+476)==0&&d9::U32(bytes.data()+488)==0); // ambient
    fx::Lighting l=defaults;l.sunDirection={0.5f,0.25f,-1,0};l.shadowStrength=0.5f;l.csmShadowResolution=1024;l.clockMode=1;
    l.shadowTaaSettingsRevision=0x0102030405060708ull;l.clockRevision=0x1112131415161718ull;l.ambientColor={1,2,4,8};
    const auto changed=fx::EncodeLighting(l);
    check(d9::U32(changed.data()+4)==0x3f000000u&&d9::U32(changed.data()+8)==0x3e800000u&&d9::U32(changed.data()+12)==0xbf800000u);
    check(d9::U32(changed.data()+48)==1024&&d9::U32(changed.data()+92)==0x3f000000u&&d9::U32(changed.data()+368)==1);
    check(d9::U32(changed.data()+188)==0x05060708u&&d9::U32(changed.data()+192)==0x01020304u);
    check(d9::U32(changed.data()+380)==0x15161718u&&d9::U32(changed.data()+384)==0x11121314u);
    check(d9::U32(changed.data()+476)==0x3f800000u&&d9::U32(changed.data()+488)==0x41000000u);
    fx::Lighting back;check(fx::DecodeLighting(changed.data(),492,back)&&fx::EncodeLighting(back)==changed
      &&back.shadowStrength==0.5f&&back.csmShadowResolution==1024&&back.clockMode==1&&back.ambientColor[3]==8
      &&back.shadowTaaSettingsRevision==0x0102030405060708ull);
    check(!fx::DecodeLighting(changed.data(),488,back)&&!fx::DecodeLighting(changed.data(),496,back)&&!fx::DecodeLighting(nullptr,492,back));
    check(d9::ValidFrameScene(9,changed.data(),492)&&!d9::ValidFrameScene(10,changed.data(),492)&&!d9::ValidFrameScene(9,changed.data(),24));
    // A switch is 0 or 1, an enum is within its range, a float is finite.
    {auto broken=changed;d9::P32(broken.data(),2);check(!d9::ValidFrameScene(9,broken.data(),492));}
    {auto broken=changed;d9::P32(broken.data()+368,3);check(!d9::ValidFrameScene(9,broken.data(),492));
      d9::P32(broken.data()+368,2);check(d9::ValidFrameScene(9,broken.data(),492));}
    for(const uint32_t at:{4u,92u,488u})for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u}){
      auto broken=changed;d9::P32(broken.data()+at,bad);check(!d9::ValidFrameScene(9,broken.data(),492));}
    // A count is any word.
    {auto any=changed;d9::P32(any.data()+48,0xffffffffu);check(d9::ValidFrameScene(9,any.data(),492));}}
  // ShadowCaster: a draw record of 34 words.
  const std::array<uint8_t,136> casterBytes={
    0,0,0x80,0x3f, 0,0,0,0, 0,0,0,0, 0,0,0,0,  0,0,0,0, 0,0,0x80,0x3f, 0,0,0,0, 0,0,0,0,                 // world rows 0, 1
    0,0,0,0, 0,0,0,0, 0,0,0x80,0x3f, 0,0,0,0,  0,0,0x80,0x40, 0,0,0,0x41, 0,0,0x80,0x41, 0,0,0x80,0x3f, // row 2, translation 4 8 16
    0,0,0x80,0x40, 0,0,0,0x41, 0,0,0x80,0x41, 0,0,0,0x40,                                               // bounds centre 4 8 16, radius 2
    1,2,3,4, 3,0,0,0, 0xff,0xff,0xff,0xff, 11,0,0,0, 0x86,0,0,0,                                        // kinds, category, tag -1, stage 11, flags
    0x10,0,0,0, 0x78,0x6f,0x62,0x68, 1,0,0x10,0, 0,0x20,0,0,                                            // batch, rawcode, object, part
    0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01, 6,0,0,0,0,0,0,0, 0,0,0,0};                                 // geometry key, bounds generation, age 0
  {fx::ShadowCaster c;c.world={1,0,0,0, 0,1,0,0, 0,0,1,0, 4,8,16,1};c.boundsCenter={4,8,16};c.boundsRadius=2;
    c.mode=1;c.objectKind=2;c.lifecycle=3;c.boundsProvenance=4;c.category=3;c.batchTag=-1;c.stage=11;
    c.flags=fx::ShadowCasterFlag::DepthWrite|fx::ShadowCasterFlag::DepthTest|fx::ShadowCasterFlag::BoundsIdentityProven;
    c.batchHandle=0x10;c.rawcode=0x68626f78u;c.objectHandle=0x100001u;c.part=0x2000u;c.geometryKey=0x0123456789abcdefull;
    c.boundsGeneration=6;c.boundsAge=0;
    check(fx::ShadowCasterSchema==11&&fx::ShadowCasterBytes==136&&fx::EncodeShadowCaster(c)==casterBytes);
    fx::ShadowCaster back;check(fx::DecodeShadowCaster(casterBytes.data(),136,back)&&back.world==c.world&&back.boundsCenter==c.boundsCenter
      &&back.boundsRadius==2&&back.mode==1&&back.objectKind==2&&back.lifecycle==3&&back.boundsProvenance==4&&back.category==3
      &&back.batchTag==-1&&back.stage==11&&back.flags==0x86&&back.batchHandle==0x10&&back.rawcode==0x68626f78u
      &&back.objectHandle==0x100001u&&back.part==0x2000u&&back.geometryKey==0x0123456789abcdefull&&back.boundsGeneration==6
      &&back.boundsAge==0);
    check(!fx::DecodeShadowCaster(casterBytes.data(),132,back)&&!fx::DecodeShadowCaster(casterBytes.data(),140,back)
      &&!fx::DecodeShadowCaster(nullptr,136,back));
    // A caster is a draw record, never a frame record.
    check(!d9::ValidFrameScene(11,casterBytes.data(),136));
    check(d9::ValidDrawRecord(11,136)&&!d9::ValidDrawRecord(11,132)&&d9::ValidDrawRecordData(11,casterBytes.data(),136));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=casterBytes;d9::P32(broken.data()+at,value);uint8_t out[160];
      return !d9::ValidDrawRecordData(11,broken.data(),136)&&!d9::AppendDrawRecord(out,sizeof(out),0,11,broken.data(),136);};
    for(uint32_t word=0;word<20;++word)for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xff800000u})check(refused(word*4,bad));
    check(refused(76,0xbf800000u));                             // a negative radius
    check(refused(80,0x04030200u)&&refused(80,0x04030202u));    // no mode, an unknown mode
    check(refused(80,0x04030601u)&&refused(80,0x04040201u)&&refused(80,0x05030201u)); // object kind, lifecycle, provenance
    check(refused(84,7)&&refused(88,13)&&refused(88,0xfffffffeu)&&refused(92,256)&&refused(92,0xfffffffeu));
    check(refused(96,0x886)&&!refused(96,0x7ff));               // only the eleven known flags
    // Identities and the bounds age are any word.
    check(!refused(100,0xffffffffu)&&!refused(112,0xffffffffu)&&!refused(132,0xffffffffu));
    // The record travels with its draw and arrives whole.
    uint8_t wire[160];const auto size=d9::AppendDrawRecord(wire,sizeof(wire),0,11,casterBytes.data(),136);
    check(size==144);
    d9::DrawAnnotations arrived;
    check(d9::DecodeDrawAnnotations(Annotate(1,1,d9::Op::Draw,size),wire,arrived)&&arrived.count==1);
    uint32_t seen=0;arrived.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes){
      seen+=index==0&&schema==11&&bytes==136&&std::memcmp(data,casterBytes.data(),136)==0;});
    check(seen==1);
    {std::vector<uint8_t> broken(wire,wire+size);d9::P32(broken.data()+d9::RecordHeaderBytes+80,0x04030200u);
      check(!d9::DecodeDrawAnnotations(Annotate(1,1,d9::Op::Draw,size),broken.data(),arrived));}}
  // ShadowCasterAlpha: three words after the caster's own record.
  {const std::array<uint8_t,12> alphaBytes={2,0,0,0, 0,0,0,0x3f, 1,0,0,0}; // stage 2, threshold 0.5, TEXCOORD1
    fx::ShadowCasterAlpha a;a.stage=2;a.alphaRef=0.5f;a.texcoord=1;
    check(fx::ShadowCasterAlphaSchema==12&&fx::ShadowCasterAlphaBytes==12&&fx::EncodeShadowCasterAlpha(a)==alphaBytes);
    fx::ShadowCasterAlpha back;check(fx::DecodeShadowCasterAlpha(alphaBytes.data(),12,back)&&back.stage==2&&back.alphaRef==0.5f
      &&back.texcoord==1);
    check(!fx::DecodeShadowCasterAlpha(alphaBytes.data(),8,back)&&!fx::DecodeShadowCasterAlpha(alphaBytes.data(),16,back)
      &&!fx::DecodeShadowCasterAlpha(nullptr,12,back));
    check(d9::ValidDrawRecord(12,12)&&!d9::ValidDrawRecord(12,16)&&!d9::ValidFrameScene(12,alphaBytes.data(),12));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=alphaBytes;d9::P32(broken.data()+at,value);uint8_t out[32];
      return !d9::ValidDrawRecordData(12,broken.data(),12)&&!d9::AppendDrawRecord(out,sizeof(out),0,12,broken.data(),12);};
    check(refused(0,8)&&!refused(0,7)&&refused(8,8)&&!refused(8,7));       // stages and coordinates 0..7
    for(const uint32_t bad:{0x7fc00000u,0x7f800000u,0xbf800000u,0x3f800001u})check(refused(4,bad)); // NaN, inf, -1, above 1
    check(!refused(4,0)&&!refused(4,0x3f800000u));                           // 0 and 1
    // On the wire it follows the caster of its draw, and only that.
    uint8_t wire[200];uint32_t size=d9::AppendDrawRecord(wire,sizeof(wire),0,11,casterBytes.data(),136);
    size+=d9::AppendDrawRecord(wire+size,sizeof(wire)-size,0,12,alphaBytes.data(),12);
    check(size==144+20);
    d9::DrawAnnotations arrived;
    check(d9::DecodeDrawAnnotations(Annotate(2,1,d9::Op::Draw,size),wire,arrived)&&arrived.count==2);
    uint32_t seen=0;arrived.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes){
      seen+=index==0&&((schema==11&&bytes==136)||(schema==12&&bytes==12&&std::memcmp(data,alphaBytes.data(),12)==0));});
    check(seen==2);
    uint8_t alone[32];const uint32_t aloneSize=d9::AppendDrawRecord(alone,sizeof(alone),0,12,alphaBytes.data(),12);
    check(aloneSize==20&&!d9::DecodeDrawAnnotations(Annotate(1,1,d9::Op::Draw,aloneSize),alone,arrived));
    // Two draws: a caster on draw 0, its companion on draw 1.
    uint8_t split[200];uint32_t splitSize=d9::AppendDrawRecord(split,sizeof(split),0,11,casterBytes.data(),136);
    splitSize+=d9::AppendDrawRecord(split+splitSize,sizeof(split)-splitSize,1,12,alphaBytes.data(),12);
    check(!d9::DecodeDrawAnnotations(Annotate(2,2,d9::Op::DrawBatch,splitSize),split,arrived));
    check(d9::CompanionFollowsCaster(12,11)&&!d9::CompanionFollowsCaster(12,0)&&!d9::CompanionFollowsCaster(12,4)
      &&d9::CompanionFollowsCaster(11,0)&&d9::CompanionFollowsCaster(4,0));}
  // PointLights: a count, then nine words a light.
  {fx::PointLights l;l.count=1;l.lights[0].position={1,2,3};l.lights[0].range=500;l.lights[0].color={1,0.5f,0.25f};
    l.lights[0].intensity=2;l.lights[0].shadowIntensity=1;
    std::array<uint8_t,fx::PointLightsMaxBytes> p;const uint32_t used=fx::EncodePointLights(l,p);
    const std::array<uint8_t,40> bytes={1,0,0,0, 0,0,0x80,0x3f, 0,0,0,0x40, 0,0,0x40,0x40, 0,0,0xfa,0x43,
      0,0,0x80,0x3f, 0,0,0,0x3f, 0,0,0x80,0x3e, 0,0,0,0x40, 0,0,0x80,0x3f};
    check(fx::PointLightsSchema==13&&fx::PointLightsMaxBytes==580&&used==40&&std::memcmp(p.data(),bytes.data(),40)==0);
    fx::PointLights back;check(fx::DecodePointLights(bytes.data(),40,back)&&back.count==1&&back.lights[0].range==500
      &&back.lights[0].color[2]==0.25f&&back.lights[0].shadowIntensity==1);
    const std::array<uint8_t,4> none={0,0,0,0};check(fx::DecodePointLights(none.data(),4,back)&&back.count==0);
    check(!fx::DecodePointLights(bytes.data(),36,back)&&!fx::DecodePointLights(bytes.data(),44,back)
      &&!fx::DecodePointLights(nullptr,40,back));
    check(d9::ValidFrameScene(13,bytes.data(),40)&&d9::ValidFrameScene(13,none.data(),4)&&!d9::ValidDrawRecord(13,40));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=bytes;d9::P32(broken.data()+at,value);return !d9::ValidFrameScene(13,broken.data(),40);};
    check(refused(0,2)&&refused(0,17));                                         // a count the bytes do not hold
    check(refused(4,0x7fc00000u)&&refused(16,0x3f000000u)&&refused(20,0xbf800000u)); // NaN, range below 1, negative colour
    check(refused(32,0xbf800000u)&&refused(36,0x3f800001u)&&!refused(36,0));      // negative intensity, shadow above 1, 0
    fx::PointLights full;full.count=16;for(auto& light:full.lights)light.intensity=1;
    std::array<uint8_t,fx::PointLightsMaxBytes> fullBytes;check(fx::EncodePointLights(full,fullBytes)==580
      &&d9::ValidFrameScene(13,fullBytes.data(),580));
    fx::ShadowPacket packet;check(packet.accept(13,bytes.data(),40)&&packet.hasPointLights&&packet.pointLights.count==1);}
  // Outline: eleven words of settings, a count, then the handles.
  {fx::Outline o;o.mode=0;o.useScreenSpace=0;o.widthPx=2;o.showVisible=1;o.color={0,1,0.5f,1};
    o.handleCount=2;o.handles[0]=0x101;o.handles[1]=0x102;
    std::array<uint8_t,fx::OutlineMaxBytes> p;const uint32_t used=fx::EncodeOutline(o,p);
    const std::array<uint8_t,52> bytes={1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0x40, 1,0,0,0, 1,0,0,0,
      0,0,0,0, 0,0,0x80,0x3f, 0,0,0,0x3f, 0,0,0x80,0x3f, 2,0,0,0, 1,1,0,0, 2,1,0,0};
    check(fx::OutlineSchema==14&&fx::OutlineMaxBytes==4140&&used==52&&std::memcmp(p.data(),bytes.data(),52)==0);
    fx::Outline back;check(fx::DecodeOutline(bytes.data(),52,back)&&back.handleCount==2&&back.handles[1]==0x102
      &&back.widthPx==2&&back.color[2]==0.5f&&back.mode==0&&back.useScreenSpace==0);
    fx::Outline defaults;std::array<uint8_t,fx::OutlineMaxBytes> d;check(fx::EncodeOutline(defaults,d)==44
      &&fx::DecodeOutline(d.data(),44,back)&&back.enabled==1&&back.mode==1&&back.widthPx==3&&back.color[3]==0.8f);
    check(!fx::DecodeOutline(bytes.data(),48,back)&&!fx::DecodeOutline(bytes.data(),56,back)
      &&!fx::DecodeOutline(nullptr,52,back));
    check(d9::ValidFrameScene(14,bytes.data(),52)&&d9::ValidFrameScene(14,d.data(),44)&&!d9::ValidDrawRecord(14,52));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=bytes;d9::P32(broken.data()+at,value);return !d9::ValidFrameScene(14,broken.data(),52);};
    check(refused(0,2)&&refused(4,2)&&refused(8,2)&&refused(16,2)&&refused(20,2));    // switches and the mode
    check(refused(12,0)&&refused(12,0x42820000u)&&refused(12,0x7fc00000u));          // width 0, 65, NaN
    check(refused(24,0xbf800000u)&&refused(36,0x3f800001u));                          // colour below 0, above 1
    check(refused(40,3)&&refused(40,1)&&refused(48,0));                               // a count the bytes do not hold, handle 0
    fx::Outline full;full.handleCount=1024;for(auto& h:full.handles)h=7;
    std::array<uint8_t,fx::OutlineMaxBytes> fullBytes;check(fx::EncodeOutline(full,fullBytes)==4140
      &&d9::ValidFrameScene(14,fullBytes.data(),4140));
    fx::ShadowPacket packet;check(packet.accept(14,bytes.data(),52)&&packet.hasOutline&&packet.outline.handleCount==2);}
  // VolumetricLight: 41 words, War3VolumetricLightSettings in order.
  {const auto d=fx::EncodeVolumetricLight(fx::VolumetricLight());
    const auto word=[&](uint32_t i){return d9::U32(d.data()+4*i);};
    check(fx::VolumetricLightSchema==15&&fx::VolumetricLightBytes==164&&d.size()==164);
    check(word(0)==0&&word(1)==2&&word(2)==1&&word(6)==0x3fc00000u&&word(14)==16&&word(15)==4   // off, FroxelHigh, temporal, 1.5, 16, /4
      &&word(16)==0x44af0000u&&word(19)==1&&word(20)==0&&word(29)==2&&word(37)==2048&&word(40)==1); // 1400, global, no height fog
    fx::VolumetricLight v;v.enabled=1;v.quality=0;v.sampleCount=-3;v.volumeSunResolution=512;v.heightFogBase=-20.0f;
    const auto e=fx::EncodeVolumetricLight(v);fx::VolumetricLight back;
    check(fx::DecodeVolumetricLight(e.data(),164,back)&&back.enabled==1&&back.quality==0&&back.sampleCount==-3
      &&back.volumeSunResolution==512&&back.heightFogBase==-20.0f&&d9::U32(e.data()+56)==0xfffffffdu);
    check(!fx::DecodeVolumetricLight(e.data(),160,back)&&!fx::DecodeVolumetricLight(nullptr,164,back));
    check(d9::ValidFrameScene(15,e.data(),164)&&!d9::ValidDrawRecord(15,164));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=e;d9::P32(broken.data()+at,value);return !d9::ValidFrameScene(15,broken.data(),164);};
    check(refused(0,2)&&refused(4,3)&&refused(8,2)&&refused(76,2)&&refused(160,2));  // switches and the quality
    check(refused(24,0x7fc00000u)&&refused(64,0x7f800000u));                        // intensity NaN, sun distance infinite
    fx::ShadowPacket packet;check(packet.accept(15,e.data(),164)&&packet.hasVolumetric&&packet.volumetric.volumeSunResolution==512);}
  // FogVolumes: a count, then 13 words a volume (shape, on, centre, half
  // extents, rotation, density, feather), at most 8.
  {fx::FogVolumes f;f.count=2;
    auto& b=f.volumes[0];b.shape=1;b.position={0,0,160};b.halfSize={520,520,170};b.rotationDegrees={0,0,30};
    b.density=.8f;b.edgeFeather=.25f;
    auto& s=f.volumes[1];s.shape=0;s.active=0;s.position={-300,0,130};s.halfSize={200,200,200};s.density=2;
    std::array<uint8_t,fx::FogVolumesMaxBytes> bytes;
    check(fx::FogVolumesSchema==16&&fx::FogVolumesMaxBytes==420&&fx::EncodeFogVolumes(f,bytes)==108);
    const auto word=[&](uint32_t i){return d9::U32(bytes.data()+4*i);};
    check(word(0)==2&&word(1)==1&&word(2)==1&&word(5)==0x43200000u&&word(6)==0x44020000u    // box, on, z 160, 520
      &&word(11)==0x41f00000u&&word(12)==0x3f4ccccdu&&word(13)==0x3e800000u                   // 30 degrees, .8, .25
      &&word(14)==0&&word(15)==0&&word(16)==0xc3960000u&&word(22)==0&&word(25)==0x40000000u); // sphere, off, x -300, 2
    fx::FogVolumes back;check(fx::DecodeFogVolumes(bytes.data(),108,back)&&back.count==2&&back.volumes[0].halfSize[2]==170
      &&back.volumes[1].active==0&&back.volumes[1].density==2);
    check(!fx::DecodeFogVolumes(bytes.data(),104,back)&&!fx::DecodeFogVolumes(nullptr,108,back));
    check(d9::ValidFrameScene(16,bytes.data(),108)&&!d9::ValidDrawRecord(16,108));
    auto refused=[&](uint32_t at,uint32_t value){auto broken=bytes;d9::P32(broken.data()+at,value);
      return !d9::ValidFrameScene(16,broken.data(),108);};
    check(refused(0,9)&&refused(0,1)&&refused(4,3)&&refused(8,2));                       // count, shape, switch
    check(refused(12,0x49800000u)&&refused(24,0x3e800000u)&&refused(48,0x40400000u));   // centre 1e6+, half .25, density 3
    check(refused(52,0x3fc00000u)&&refused(36,0x7fc00000u));                              // feather 1.5, rotation NaN
    check(refused(32+52,0x43000000u)&&refused(28+52,0x43000000u));                        // a sphere's three radii differ
    fx::FogVolumes empty;std::array<uint8_t,fx::FogVolumesMaxBytes> none;check(fx::EncodeFogVolumes(empty,none)==4
      &&d9::ValidFrameScene(16,none.data(),4));
    fx::ShadowPacket packet;check(packet.accept(16,bytes.data(),108)&&packet.hasFogVolumes&&packet.fogVolumes.count==2);}
  // The shadow packet: the latest record of each kind, beside the frame packet.
  {fx::ShadowPacket packet;check(!packet.work()&&!packet.hasFrame);
    check(packet.accept(10,shadowFrameBytes.data(),56)&&packet.hasFrame&&!packet.work()&&packet.frame.casters==7);
    const auto lighting=fx::EncodeLighting(fx::Lighting());
    check(packet.accept(9,lighting.data(),492)&&packet.work()&&packet.lighting.csmShadowResolution==4096);
    check(!packet.accept(9,lighting.data(),488)&&!packet.accept(11,casterBytes.data(),136)&&!packet.accept(5,ssaoBytes.data(),24)
      &&packet.work());
    packet.clear();check(!packet.work()&&!packet.hasFrame&&!packet.hasLighting);}
}
} // namespace warvk_annotation_test
