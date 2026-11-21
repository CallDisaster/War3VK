#include "../tools/render_host/d3d9_draw_wire.h"
#include "../tools/render_host/d3d9_factory_wire.h"
#include "test_render_host_fvf.h"
#include "test_render_host_texture.h"
#include "test_render_host_residency.h"
#include "test_render_host_sampler.h"
#include "test_render_host_effects.h"
#include "test_render_host_texture_stages.h"
#include "test_render_host_annotation.h"
#include "test_render_host_content.h"
#include "test_render_host_upload_window.h"
#include "../tools/render_host/d3d9_ff_delta_wire.h"
#include <random>
#include "../tools/render_host/dxvk_core/d3d9_batch_state.h"
#include <cstdio>
#include <cstring>
using namespace warvk::host;
template<class Check> static void TestFixedDelta(Check check) {
  using Result=d9::FixedDeltaResult;
  std::mt19937 rng(0x50440001u);
  const auto scalar=[&]{return float(int(rng()%65536)-32768)/128.0f;};
  d9::DrawCommand draw;draw.resources[0]={1,1};draw.resources[5]={2,1};
  d9::FixedFunctionState state;
  const auto first=d9::EncodeFixedDraw(draw,state);
  d9::FixedDrawPacket previous{};
  std::array<uint8_t,d9::MaxPayload> payload{};
  std::array<d9::FixedDrawPacket,d9::MaxFixedDrawBatch> expected;
  d9::Header h;h.op=d9::Op::DrawFixedDeltaBatch;
  for(bool verify:{false,true})for(uint32_t count=1;count<=d9::MaxFixedDrawBatch;++count) {
    h.args[0]=count;h.args[1]=verify;h.payloadBytes=0;previous.fill(0);
    for(uint32_t i=0;i<count;++i) {
      switch(i%7) {
        case 0: // A reset/resize changes the next complete input, even mid-sequence.
          state={};draw.state.viewport.Width=1+rng()%4096;break;
        case 1: // UP stream offset and resource generation.
          draw.state.streamOffset=rng();++draw.resources[0].generation;break;
        case 2: // Restored state block and post-world-lighting values.
          for(auto& m:state.transforms)for(auto& row:m.m)for(float& v:row)v=scalar();
          state.render[0]=rng()%2;state.render[9]=rng();state.material.Diffuse.r=scalar();break;
        case 3:
          for(auto& f:draw.state.vertexFloatConstants)f=scalar();
          for(auto& f:draw.state.pixelFloatConstants)f=scalar();
          state.lightCount=8;
          for(auto& l:state.lights){l.Type=D3DLIGHT_DIRECTIONAL;l.Diffuse.r=scalar();l.Direction.z=1;}
          break;
        case 4:state.lights[7].Phi+=1;break; // Just word 794.
        case 5:state.lightCount=rng()%8;break; // Removed slots must become zero.
        case 6:break; // An identical input still describes a draw.
      }
      expected[i]=d9::EncodeFixedDraw(draw,state);
      const auto at=h.payloadBytes;
      const auto size=d9::EncodeFixedDeltaRecord(expected[i],previous,payload.data()+at,uint32_t(payload.size())-at,verify);
      check(size&&expected[i]==previous);h.payloadBytes+=size;
      if(i%7==4)check(size==12u+verify*8&&d9::U32(payload.data()+at)==(1u<<24)&&d9::U32(payload.data()+at+4)==(1u<<26));
      if(i%7==6)check(size==4u+verify*8&&d9::U32(payload.data()+at)==0);
      if(verify)check(d9::U64(payload.data()+at+size-8)==d9::FoldFnv1a64(expected[i].data(),d9::FixedDrawBytes));
    }
    uint32_t seen=0;
    const auto status=d9::StageFixedDeltaBatch(h,payload.data(),[&](uint32_t i,const uint8_t* bytes) {
      ++seen;check(!std::memcmp(bytes,expected[i].data(),d9::FixedDrawBytes));
      d9::DrawCommand out;d9::FixedFunctionState ff;
      return d9::DecodeFixedDraw(bytes,d9::FixedDrawBytes,out,ff)&&d9::EncodeFixedDraw(out,ff)==expected[i];
    });
    check(status==Result::Valid&&seen==count);
  }
  for(bool verify:{false,true}) {
    previous.fill(0);h.args[0]=d9::MaxFixedDrawBatch;h.args[1]=verify;h.payloadBytes=0;
    for(uint32_t i=0;i<h.args[0];++i) {
      const auto at=h.payloadBytes;
      const auto bytes=d9::EncodeFixedDeltaRecord(first,previous,payload.data()+at,uint32_t(payload.size())-at,verify);
      check(i?bytes==4u+8u*verify:bytes>4u);h.payloadBytes+=bytes;
    }
    check(d9::StageFixedDeltaBatch(h,payload.data(),[&](uint32_t,const uint8_t* bytes) {
      return !std::memcmp(bytes,first.data(),first.size());
    })==Result::Valid);
  }
  // A literal vector pins little-endian values and ascending block/word order
  // independently of the decoder (including a changed word becoming zero).
  {
    previous.fill(0);previous[31*4]=0xff;d9::FixedDrawPacket full{};
    d9::P32(full.data()+2*4,0x12345678);d9::P32(full.data()+32*4,0xabcdef89);
    d9::P32(full.data()+794*4,0x01020304);
    const uint32_t wire[]={0x1000003,0x80000004,0x12345678,0,1,0xabcdef89,0x4000000,0x01020304};
    check(d9::EncodeFixedDeltaRecord(full,previous,payload.data(),uint32_t(payload.size()),false)==sizeof(wire));
    for(unsigned i=0;i<sizeof(wire)/4;++i)check(d9::U32(payload.data()+4*i)==wire[i]);
  }
  // Literal all-word changes exercise bit 31 in every full block. Such
  // arbitrary packets are a codec test; valid fixed states have constant words.
  for(bool verify:{false,true}) {
    previous.fill(0);uint32_t used=0,records=0,batches=0,total=0;
    const auto flush=[&] {
      h.args[0]=records;h.args[1]=verify;h.payloadBytes=used;
      uint32_t staged=0;
      check(d9::StageFixedDeltaBatch(h,payload.data(),[&](uint32_t i,const uint8_t* p) {
        const uint8_t value=(total+i)%2?0x11:0xee;++staged;
        return std::all_of(p,p+d9::FixedDrawBytes,[&](uint8_t b){return b==value;});
      })==Result::Valid&&staged==records);
      total+=records;++batches;records=used=0;previous.fill(0);
    };
    for(unsigned i=0;i<20;++i) {
      if(!d9::FixedDeltaHasRoom(used,verify))flush();
      d9::FixedDrawPacket full;full.fill(i%2?0x11:0xee);
      const auto size=d9::EncodeFixedDeltaRecord(full,previous,payload.data()+used,uint32_t(payload.size())-used,verify);
      check(size==d9::FixedDeltaRecordBound(verify)&&d9::U32(payload.data()+used)==0x1ffffffu);
      used+=size;++records;
    }
    flush();check(batches==2&&total==20);
  }
  // The encoder finds dirty words four at a time with a scalar tail. Pin its
  // bytes against the plain word-by-word form: every word position set and
  // cleared on its own, then sparse and dense random changes, at each output
  // alignment.
  {
    const auto reference=[](const d9::FixedDrawPacket& full,d9::FixedDrawPacket& base,uint8_t* out) {
      uint32_t blocks=0,at=4;
      for(uint32_t b=0;b<d9::FixedDeltaBlocks;++b) {
        uint32_t mask=0;const auto maskAt=at;
        for(uint32_t w=b*32;w<std::min(b*32+32,d9::FixedDrawWords);++w) {
          const auto value=d9::U32(full.data()+w*4);
          if(value==d9::U32(base.data()+w*4))continue;
          if(!mask)at+=4;
          mask|=1u<<(w%32);d9::P32(out+at,value);at+=4;d9::P32(base.data()+w*4,value);
        }
        if(mask){blocks|=1u<<b;d9::P32(out+maskAt,mask);}
      }
      d9::P32(out,blocks);return at;
    };
    d9::FixedDrawPacket full{},mine{},theirs{};
    std::array<uint8_t,d9::FixedDrawBytes+4*(1+d9::FixedDeltaBlocks)+8> a{},b{};
    const auto same=[&](uint32_t shift) {
      const auto x=d9::EncodeFixedDeltaRecord(full,mine,a.data()+shift,uint32_t(a.size())-shift,false);
      const auto y=reference(full,theirs,b.data()+shift);
      check(x==y&&!std::memcmp(a.data()+shift,b.data()+shift,x)&&mine==theirs&&mine==full);
    };
    for(uint32_t w=0;w<d9::FixedDrawWords;++w){d9::P32(full.data()+w*4,0x80000001u+w);same(w%4);}
    for(uint32_t w=0;w<d9::FixedDrawWords;++w){d9::P32(full.data()+w*4,0);same((w+1)%4);}
    for(uint32_t round=0;round<4000;++round) {
      const uint32_t changes=round%5?rng()%24:rng()%d9::FixedDrawWords;
      for(uint32_t i=0;i<changes;++i)
        d9::P32(full.data()+(rng()%d9::FixedDrawWords)*4,round%3?uint32_t(rng()):uint32_t(rng()%2));
      same(round%4);
    }
  }
  // A complete valid prefix, then damage the last record. The observable
  // output changes only when the entire staged batch was accepted.
  h.args[0]=2;h.args[1]=1;previous.fill(0);
  const auto second=d9::EncodeFixedDeltaRecord(first,previous,payload.data(),uint32_t(payload.size()),true);
  h.payloadBytes=second+d9::EncodeFixedDeltaRecord(first,previous,payload.data()+second,uint32_t(payload.size())-second,true);
  const auto good=h;const auto goodPayload=payload;
  std::array<d9::FixedDrawPacket,2> output{};
  const auto receive=[&](const d9::Header& msg,const uint8_t* p) {
    decltype(output) scratch;
    const auto status=d9::StageFixedDeltaBatch(msg,p,[&](uint32_t i,const uint8_t* full) {
      d9::DrawCommand out;d9::FixedFunctionState ff;
      if(i>=scratch.size()||!d9::DecodeFixedDraw(full,d9::FixedDrawBytes,out,ff))return false;
      scratch[i]=d9::EncodeFixedDraw(out,ff);return true;
    });
    if(status==Result::Valid)output=scratch;
    return status;
  };
  const auto reject=[&](const d9::Header& bad,Result reason=Result::Invalid) {
    output[0].fill(0xad);output[1].fill(0xde);const auto before=output;
    check(receive(bad,payload.data())==reason&&output==before);
    check(receive(good,goodPayload.data())==Result::Valid&&output[0]==first&&output[1]==first);
    payload=goodPayload;
  };
  for(unsigned bit=25;bit<32;++bit){d9::P32(payload.data()+second,1u<<bit);reject(good);}
  d9::P32(payload.data()+second,1);d9::P32(payload.data()+second+4,0);reject(good);
  for(unsigned bit=27;bit<32;++bit){d9::P32(payload.data()+second,1u<<24);d9::P32(payload.data()+second+4,1u<<bit);reject(good);}
  for(unsigned count:{0u,21u}){auto bad=good;bad.args[0]=count;reject(bad);}
  {auto bad=good;bad.args[1]=2;reject(bad);}
  for(size_t i=2;i<good.args.size();++i){auto bad=good;bad.args[i]=1;reject(bad);}
  {auto bad=good;bad.payloadBytes+=4;reject(bad);}
  for(unsigned bytes=0;bytes<good.payloadBytes;++bytes){auto bad=good;bad.payloadBytes=bytes;reject(bad);}
  payload[good.payloadBytes-1]^=1;reject(good,Result::HashMismatch);
  // Supply the correct hash for each invalid complete state, to reach the
  // existing fixed draw validator after a valid first record.
  for(unsigned invalid=0;invalid<3;++invalid) {
    auto badFull=first;
    if(invalid==0)d9::P32(badFull.data()+d9::DrawBytes,0x7fc00000);
    if(invalid==1)d9::P32(badFull.data()+d9::FixedDrawBytes-8*26*4-4,9);
    if(invalid==2)badFull.back()=1;
    previous=first;auto bad=good;bad.payloadBytes=second+d9::EncodeFixedDeltaRecord(badFull,previous,
      payload.data()+second,uint32_t(payload.size())-second,true);reject(bad);
  }
  // Independent FNV-1a vector and an annotation on the last legal draw.
  check(d9::FoldFnv1a64(reinterpret_cast<const uint8_t*>("hello"),5)==0xa430d84680aabd0bull);
  d9::Header a;a.op=d9::Op::AnnotateDraws;a.args[0]=1;a.args[1]=20;a.args[2]=uint32_t(h.op);
  auto tag=d9::EncodeDrawTag({1,2,3});a.payloadBytes=d9::AppendDrawRecord(payload.data(),uint32_t(payload.size()),19,
    uint16_t(d9::AnnotationSchema::DrawTag),tag.data(),uint16_t(tag.size()));
  d9::DrawAnnotations annotations;h.args[0]=20;
  check(d9::DecodeDrawAnnotations(a,payload.data(),annotations)&&annotations.binds(h));
  h.args[0]=19;check(!annotations.binds(h));h.op=d9::Op::DrawFixedBatch;h.args[0]=20;check(!annotations.binds(h));
}

int main(){
  unsigned checks=0,failures=0;
  auto check=[&](bool value){++checks;if(!value)++failures;};
  // Preserve the legacy receipt's check count; new contracts fail this run directly.
  auto require=[](bool ok){if(!ok){std::fputs("async-resource wire contract failed\n",stderr);std::abort();}};
  for(uint32_t flags=0;flags<32;++flags) {
    d9::Header h;h.args[0]=h.args[1]=64;h.args[6]=flags;h.sequence=1;h.nonce={1,2};
    d9::Packet packet{};d9::Header decoded;d9::Initialization init;
    require(d9::EncodeHeader(h,packet.data())&&d9::DecodeHeader(packet.data(),decoded)&&d9::DecodeInitialization(decoded,init));
    require(init.asyncResources==(flags&24)&&init.fixedDelta==bool(flags&4)&&init.annotations==bool(flags&2)
      &&init.retainContent==bool(flags&1));
    d9::Reply reply;d9::EncodeReply(h,{},packet);require(d9::DecodeReply(h,packet.data(),reply)&&!reply.result&&!reply.payloadBytes);
    for(bool ring:{false,true})for(bool batches:{false,true})
      require(d9::ValidResourceTransport(flags,ring,batches)==(!(flags&24)||(ring&&batches)));
  }
  uint32_t bits=0;
  for(const auto format:{D3DFMT_A8R8G8B8,D3DFMT_X8R8G8B8,D3DFMT_A8B8G8R8,D3DFMT_X8B8G8R8,D3DFMT_R5G6B5,
      D3DFMT_A1R5G5B5,D3DFMT_X1R5G5B5,D3DFMT_A4R4G4B4,D3DFMT_A8L8,D3DFMT_A8,D3DFMT_L8,
      D3DFMT_DXT1,D3DFMT_DXT2,D3DFMT_DXT3,D3DFMT_DXT4,D3DFMT_DXT5}) {
    const auto bit=d9::TextureFormatBit(format);require(bit&&!(bit&(bit-1))&&!(bits&bit));bits|=bit;
    d9::TextureLayout layout;require(d9::MakeTextureLayout(13,7,0,layout,format)&&layout.levels==4);
    const bool bc=format>=D3DFMT_DXT1&&format<=D3DFMT_DXT5;
    const uint32_t unit=bc?(format==D3DFMT_DXT1?8:16):(format==D3DFMT_A8||format==D3DFMT_L8)?1:
      (format==D3DFMT_A8R8G8B8||format==D3DFMT_X8R8G8B8||format==D3DFMT_A8B8G8R8||format==D3DFMT_X8B8G8R8)?4:2;
    uint32_t offset=0;
    for(unsigned i=0;i<4;++i) {
      const auto w=13u>>i,h=std::max(1u,7u>>i),pitch=(bc?(w+3)/4:w)*unit,bytes=pitch*(bc?(h+3)/4:h);
      const auto& l=layout.level[i];require(l.width==w&&l.height==h&&l.pitch==pitch&&l.offset==offset&&l.bytes==bytes);
      offset+=bytes;
    }
    require(layout.bytes==offset);
  }
  require(bits==0xffff&&!d9::TextureFormatBit(D3DFMT_UNKNOWN));
  TestFixedDelta(check);
  warvk_fvf_test::Run(check);
  warvk_texture_test::Run(check);
  warvk_residency_test::Run(check);
  warvk_sampler_test::Run(check);
  warvk_effects_test::Run(check);
  warvk_texture_stages_test::Run(check);
  warvk_annotation_test::Run(check);
  warvk_content_test::Run(check);
  warvk_upload_window_test::Run(check);
  {
    d9::FixedFunctionState state,decoded;d9::DrawCommand draw,copy;
    draw.resources[0]={1,2};draw.resources[5]={6,2};
    state.transforms[0]._41=.25f;state.material.Diffuse={.5f,.25f,1,1};
    state.lightCount=1;state.lights[0].Type=D3DLIGHT_DIRECTIONAL;
    state.lights[0].Direction={0,0,1};state.lights[0].Diffuse={1,.5f,.25f,1};
    const auto bytes=d9::EncodeFixedDraw(draw,state);
    check(d9::DecodeFixedDraw(bytes.data(),uint32_t(bytes.size()),copy,decoded));
    check(d9::EncodeFixedDraw(copy,decoded)==bytes);
    for(uint32_t i=0;i<bytes.size();++i) {
      const auto before=d9::EncodeFixedDraw(copy,decoded);
      check(!d9::DecodeFixedDraw(bytes.data(),i,copy,decoded));
      check(d9::EncodeFixedDraw(copy,decoded)==before);
    }
    for(uint32_t i=d9::DrawBytes;i<d9::DrawBytes+11*64;i+=4) {
      auto bad=bytes;d9::P32(bad.data()+i,0x7fc00000u);
      check(!d9::DecodeFixedDraw(bad.data(),uint32_t(bad.size()),copy,decoded));
      check(d9::EncodeFixedDraw(copy,decoded)==bytes);
    }
    for(uint32_t i=0;i<8;++i) {
      auto broken=state;broken.stages[i][0]=0;
      const auto bad=d9::EncodeFixedDraw(draw,broken);
      check(!d9::DecodeFixedDraw(bad.data(),uint32_t(bad.size()),copy,decoded));
    }
    auto bad=bytes;bad.back()=1;
    check(!d9::DecodeFixedDraw(bad.data(),uint32_t(bad.size()),copy,decoded));
    auto programmable=draw;programmable.resources[3]={3,1};programmable.resources[4]={4,1};
    bad=d9::EncodeFixedDraw(programmable,state);check(!d9::DecodeFixedDraw(bad.data(),uint32_t(bad.size()),copy,decoded));
    for(unsigned i:{2u,3u,4u}) {
      bad=bytes;d9::P32(bad.data()+i*8+4,1);
      check(!d9::DecodeFixedDraw(bad.data(),uint32_t(bad.size()),copy,decoded));
    }
  }
  {
    static_assert(sizeof(D3DCAPS9)==304&&d9::FactoryCapsBytes==304);
    D3DCAPS9 original{};uint32_t words[76]{};
    for(unsigned i=0;i<76;++i)words[i]=0x3f000000+i; // finite floats and distinct scalar words
    std::memcpy(&original,words,sizeof(original));original.DeviceType=D3DDEVTYPE_HAL;original.AdapterOrdinal=0;original.Reserved5=0;
    std::array<uint8_t,d9::FactoryCapsBytes> encoded{};d9::EncodeCaps(original,encoded.data());D3DCAPS9 decoded{};
    check(d9::DecodeCaps(encoded.data(),uint32_t(encoded.size()),decoded));check(!std::memcmp(&original,&decoded,sizeof(original)));
    for(unsigned i=0;i<encoded.size();++i){const auto before=decoded;check(!d9::DecodeCaps(encoded.data(),i,decoded));check(!std::memcmp(&before,&decoded,sizeof(decoded)));}
    for(unsigned i=0;i<76;++i) {
      // One distinct source field at a time ensures no omitted/duplicated field
      // can survive the round trip by copying a single constant into two slots.
      auto altered=original;uint32_t fields[76];std::memcpy(fields,&altered,sizeof(fields));
      if(i==0||i==1||offsetof(D3DCAPS9,Reserved5)/4==i)continue;
      fields[i]^=0x00001000;std::memcpy(&altered,fields,sizeof(fields));d9::EncodeCaps(altered,encoded.data());
      check(d9::DecodeCaps(encoded.data(),uint32_t(encoded.size()),decoded));check(!std::memcmp(&altered,&decoded,sizeof(altered)));
    }
    D3DADAPTER_IDENTIFIER9 id{},copy{};std::strcpy(id.Driver,"adapter-driver");std::strcpy(id.Description,"physical GPU");std::strcpy(id.DeviceName,"DISPLAY1");
    id.DriverVersion.QuadPart=0x123456789abcdef;id.VendorId=0x10de;id.DeviceId=19;id.SubSysId=31;id.Revision=7;
    id.DeviceIdentifier={123,456,789,{1,2,3,4,5,6,7,8}};id.WHQLLevel=42;
    std::array<uint8_t,d9::FactoryIdentifierBytes> bytes{};d9::EncodeIdentifier(id,bytes.data());
    check(d9::DecodeIdentifier(bytes.data(),uint32_t(bytes.size()),copy));check(!std::memcmp(&id,&copy,sizeof(id)));
    bytes.back()=1;check(!d9::DecodeIdentifier(bytes.data(),uint32_t(bytes.size()),copy));bytes.back()=0;
    std::memset(bytes.data(),1,512);check(!d9::DecodeIdentifier(bytes.data(),uint32_t(bytes.size()),copy));
    for(unsigned q=1;q<=10;++q){d9::Header h;h.op=d9::Op::FactoryQuery;h.args[0]=q;check(d9::ValidFactoryQuery(h));
      for(unsigned i=d9::FactoryArguments(d9::FactoryQuery(q));i<16;++i){auto bad=h;bad.args[i]=1;check(!d9::ValidFactoryQuery(bad));}}
  }
  {
    d9::Header h;h.op=d9::Op::ClearRegions;h.args[0]=0xff123456;h.args[1]=d9::FloatBits(.5f);h.args[2]=D3DCLEAR_TARGET;h.args[3]=64;h.payloadBytes=1024;
    std::array<uint8_t,1024> bytes{};for(unsigned i=0;i<64;++i)d9::EncodeClearRegion(bytes.data()+16*i,{i,0,1,64});
    d9::ClearRegions result;check(d9::DecodeClearRegions(h,bytes.data(),result,64,64));
    for(unsigned i=0;i<64;++i)for(unsigned field=0;field<4;++field){auto bad=bytes;d9::P32(bad.data()+16*i+4*field,UINT32_MAX);
      const auto before=result;check(!d9::DecodeClearRegions(h,bad.data(),result,64,64));check(!std::memcmp(&before,&result,sizeof(result)));}
    for(unsigned i=0;i<1024;++i){auto bad=h;bad.payloadBytes=i;check(!d9::DecodeClearRegions(bad,bytes.data(),result,64,64));}
    for(unsigned i=4;i<16;++i){auto bad=h;bad.args[i]=1;check(!d9::DecodeClearRegions(bad,bytes.data(),result,64,64));}
    // Regions are bounded by the negotiated extent, not by the 64x64 lab one.
    {auto one=h;one.args[3]=1;one.payloadBytes=16;std::array<uint8_t,16> edge{};d9::EncodeClearRegion(edge.data(),{1800,900,102,63});
      check(d9::DecodeClearRegions(one,edge.data(),result,1902,963)&&result.regions[0].x==1800&&result.regions[0].height==63);
      check(!d9::DecodeClearRegions(one,edge.data(),result,64,64)&&!d9::DecodeClearRegions(one,edge.data(),result,1901,963)
        &&!d9::DecodeClearRegions(one,edge.data(),result,1902,962));}
    const D3DVIEWPORT9 viewport{8,10,40,36,0,1};
    for(int left=-16;left<80;left+=7)for(int top=-16;top<80;top+=9){
      const D3DRECT box{left,top,left+13,top+19};const auto region=d9::IntersectClear(viewport,box);
      for(int y=0;y<64;++y)for(int x=0;x<64;++x){
        const bool expected=x>=8&&x<48&&y>=10&&y<46&&x>=box.x1&&x<box.x2&&y>=box.y1&&y<box.y2;
        const bool actual=uint32_t(x)>=region.x&&uint32_t(x)-region.x<region.width&&uint32_t(y)>=region.y&&uint32_t(y)-region.y<region.height;
        check(actual==expected);
      }
    }
  }
  {
    using State=dxvkcore::D3D9Backend::DrawState;
    const auto mask=[](dxvkcore::BatchStateUpdates u){return unsigned(u.targets)|(unsigned(u.layout)<<1)
      |(unsigned(u.assembly)<<2)|(unsigned(u.raster)<<3)|(unsigned(u.multisample)<<4)
      |(unsigned(u.depth)<<5)|(unsigned(u.blend)<<6);};
    State old{};check(mask(dxvkcore::BatchStateChanges(old,nullptr))==127);
    check(mask(dxvkcore::BatchStateChanges(old,&old))==0);
    for(unsigned field=0;field<16;++field){
      State next=old;unsigned expected=0;
      switch(field){
        case 0:++next.vertexShader;expected=2;break;
        case 1:++next.declaration;expected=2;break;
        case 2:next.primitiveType=D3DPT_TRIANGLESTRIP;expected=4;break;
        case 3:++next.render.cullMode;expected=8;break;
        case 4:++next.render.zEnable;expected=32;break;
        case 5:++next.render.zWriteEnable;expected=32;break;
        case 6:++next.render.zFunc;expected=32;break;
        case 7:++next.render.alphaBlendEnable;expected=64;break;
        case 8:++next.render.srcBlend;expected=64;break;
        case 9:++next.render.destBlend;expected=64;break;
        case 10:++next.render.blendOp;expected=64;break;
        case 11:++next.render.colorWriteMask;expected=64;break;
        case 12:++next.vertexFloatConstants[0];break;
        case 13:++next.pixelFloatConstants[0];break;
        case 14:++next.viewport.Width;break;
        case 15:++next.indexBuffer;++next.vertexBuffer;++next.texture;++next.streamOffset;++next.stride;break;
      }
      check(mask(dxvkcore::BatchStateChanges(next,&old))==expected);
      check(mask(dxvkcore::BatchStateChanges(old,&next))==expected);
      check(mask(dxvkcore::BatchStateChanges(next,nullptr))==127);
    }
  }
  // Independent old variable-width byte codec, including unaligned buffers,
  // all bit positions and deterministic full-width inputs. Guard bytes prove
  // the fixed-width optimization never writes outside the requested field.
  uint64_t seed=0xa7d8c9325b0146efull;
  for(unsigned n=0;n<4096;++n){
    seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
    const uint64_t value=n<64?uint64_t(1)<<n:seed;
    for(unsigned offset=1;offset<=7;++offset){
      std::array<uint8_t,24> expected{},actual{};expected.fill(0xcd);actual=expected;
      ipc::WriteLe(expected.data()+offset,value,8);d9::P64(actual.data()+offset,value);
      check(actual==expected);check(d9::U64(actual.data()+offset)==ipc::ReadLe(expected.data()+offset,8));
      expected.fill(0xab);actual=expected;
      ipc::WriteLe(expected.data()+offset,uint32_t(value),4);d9::P32(actual.data()+offset,uint32_t(value));
      check(actual==expected);check(d9::U32(actual.data()+offset)==ipc::ReadLe(expected.data()+offset,4));
    }
  }
  d9::Header h;h.nonce={7,19};h.sequence=1;h.op=d9::Op::UploadBuffer;h.id=3;h.generation=1;h.args[0]=17;h.payloadBytes=3;
  const uint8_t data[]={5,8,9};d9::Packet p;check(d9::Encode(h,data,p));
  d9::Header back;check(d9::DecodeHeader(p.data(),back));check(back.nonce==h.nonce&&back.id==3&&back.args[0]==17&&back.payloadBytes==3);
  check(!std::memcmp(p.data()+128,data,3));
  // Buffers up to 1 MiB (the game's 576 KiB VB); dynamic Lock modes only on a
  // dynamic buffer, Ordered on any.
  check(d9::MaxBuffer==1048576&&d9::MaxBuffer>=589824);
  check(d9::ValidUploadMode(0,false)&&d9::ValidUploadMode(0,true));
  check(!d9::ValidUploadMode(1,false)&&!d9::ValidUploadMode(2,false));
  check(d9::ValidUploadMode(1,true)&&d9::ValidUploadMode(2,true));
  for(const uint32_t mode:{3u,UINT32_MAX})check(!d9::ValidUploadMode(mode,true));
  {auto wait=h;wait.op=d9::Op::WaitEvent;check(d9::Encode(wait,data,p));
    check(d9::DecodeHeader(p.data(),back)&&back.op==d9::Op::WaitEvent);
    wait.op=d9::Op::FlushDraws;check(d9::Encode(wait,data,p));
    check(d9::DecodeHeader(p.data(),back)&&back.op==d9::Op::FlushDraws);
    wait.op=d9::Op::DrawFixedBatch;check(d9::Encode(wait,data,p));
    check(d9::DecodeHeader(p.data(),back)&&back.op==d9::Op::DrawFixedBatch);
    wait.op=d9::Op::ReadContent;check(d9::Encode(wait,data,p));
    check(d9::DecodeHeader(p.data(),back)&&back.op==d9::Op::ReadContent);
    for(const auto op:{d9::Op::AnnotateDraws,d9::Op::FrameScene,d9::Op::StageMarker,d9::Op::Reset,d9::Op::AttachRing,
        d9::Op::SessionReset,d9::Op::ContentUpload,d9::Op::ContentRelease,d9::Op::ContentRead,d9::Op::DrawFixedDeltaBatch}) {
      wait.op=op;check(d9::Encode(wait,data,p));check(d9::DecodeHeader(p.data(),back)&&back.op==op);
    }
    ipc::WriteLe(p.data()+6,uint16_t(d9::Op::DrawFixedDeltaBatch)+1,2);check(!d9::DecodeHeader(p.data(),back));
    // EncodeHeader writes exactly the bytes Encode puts ahead of a payload.
    {std::array<uint8_t,d9::HeaderBytes> only{};wait.op=d9::Op::Present;wait.payloadBytes=0;
      check(d9::EncodeHeader(wait,only.data())&&d9::Encode(wait,nullptr,p)&&std::equal(only.begin(),only.end(),p.begin()));}
    // SessionReset: a reason, a nonzero generation, nothing else.
    {d9::Header reset;reset.op=d9::Op::SessionReset;reset.args[0]=uint32_t(d9::SessionResetReason::MapTransition);reset.args[1]=1;
      check(d9::ValidSessionReset(reset));
      auto c=reset;c.args[0]=uint32_t(d9::SessionResetReason::GameExit);c.args[1]=0;c.args[2]=1;check(d9::ValidSessionReset(c));
      c=reset;c.args[0]=0;check(!d9::ValidSessionReset(c));c.args[0]=3;check(!d9::ValidSessionReset(c));
      c=reset;c.args[1]=0;check(!d9::ValidSessionReset(c));
      c=reset;c.id=1;check(!d9::ValidSessionReset(c));c=reset;c.generation=1;check(!d9::ValidSessionReset(c));
      c=reset;c.payloadBytes=4;check(!d9::ValidSessionReset(c));
      for(size_t i=3;i<reset.args.size();++i){c=reset;c.args[i]=1;check(!d9::ValidSessionReset(c));}
      c=reset;c.op=d9::Op::StageMarker;check(!d9::ValidSessionReset(c));}
    check(d9::Encode(h,data,p));}
  for(size_t i=112;i<128;++i){auto broken=p;broken[i]=1;check(!d9::DecodeHeader(broken.data(),back));}
  for(const uint32_t value:{0u,65536u,UINT32_MAX}){auto broken=p;d9::P32(broken.data()+8,value);check(d9::DecodeHeader(broken.data(),back)==(value<=d9::MaxPayload));}
  {auto broken=p;d9::P64(broken.data()+32,0);check(!d9::DecodeHeader(broken.data(),back));}
  {auto reused=p;d9::P32(reused.data()+44,2);check(d9::DecodeHeader(reused.data(),back)&&back.generation==2);}
  {auto broken=p;d9::P32(broken.data()+44,0);check(!d9::DecodeHeader(broken.data(),back));}
  {auto old=p;old[3]='1';ipc::WriteLe(old.data()+4,1,2);check(!d9::DecodeHeader(old.data(),back));}
  {auto broken=p;d9::P32(broken.data()+40,d9::MaxResources+1);check(!d9::DecodeHeader(broken.data(),back));}
  {auto broken=p;d9::P32(broken.data()+12,1);check(!d9::DecodeHeader(broken.data(),back));}
  d9::Reply r{0,3,2,1,1,4},decoded;d9::Packet rp{};d9::EncodeReply(h,r,rp);
  check(d9::DecodeReply(h,rp.data(),decoded));check(decoded.submitted==3&&decoded.completed==2&&decoded.retired==1);
  for(size_t i=16;i<48;++i){auto broken=rp;broken[i]^=1;check(!d9::DecodeReply(h,broken.data(),decoded));}
  for(size_t i=80;i<96;++i){auto broken=rp;broken[i]=1;check(!d9::DecodeReply(h,broken.data(),decoded));}
  {auto broken=rp;d9::P64(broken.data()+56,4);check(!d9::DecodeReply(h,broken.data(),decoded));}
  {auto broken=rp;d9::P64(broken.data()+64,3);check(!d9::DecodeReply(h,broken.data(),decoded));}
  {auto old=rp;old[3]='1';d9::P32(old.data()+4,1);check(!d9::DecodeReply(h,old.data(),decoded));}
  d9::DrawCommand command{},copy{};auto& state=command.state;
  d9::Header init;init.args[0]=64;init.args[1]=64;d9::Initialization mode;
  check(d9::DecodeInitialization(init,mode));check(mode.profile==d9::PresentationProfile::Offscreen&&!mode.retainContent&&!mode.annotations);
  for(size_t i=2;i<16;++i){if(i==6)continue;auto invalid=init;invalid.args[i]=1;check(!d9::DecodeInitialization(invalid,mode));}
  for(uint32_t features=0;features<8;++features) {
    auto h=init;h.args[6]=features;check(d9::DecodeInitialization(h,mode)&&mode.retainContent==bool(features&1)
      &&mode.annotations==bool(features&2)&&mode.fixedDelta==bool(features&4));
  }
  for(const uint32_t value:{32u,33u,0x80000000u,UINT32_MAX}){auto invalid=init;invalid.args[6]=value;check(!d9::DecodeInitialization(invalid,mode));}
  init.args[2]=1;init.args[3]=0x80401234;init.args[4]=17;init.args[5]=1;
  check(d9::DecodeInitialization(init,mode));check(mode.window==0x80401234&&mode.thread==17&&mode.interval==1);
  {auto diagnostic=init;diagnostic.args[2]=2;check(d9::DecodeInitialization(diagnostic,mode));
    check(mode.profile==d9::PresentationProfile::IsolatedPresenterReadback);}
  {auto invalid=init;invalid.args[2]=3;check(!d9::DecodeInitialization(invalid,mode));}
  for(size_t i:{size_t(0),size_t(1),size_t(3),size_t(4),size_t(5)}){auto invalid=init;invalid.args[i]=0;check(!d9::DecodeInitialization(invalid,mode));}
  // Windowed intervals: ONE (DEFAULT folds into it on the x86 side) or IMMEDIATE.
  {auto immediate=init;immediate.args[5]=d9::IntervalImmediate;check(d9::DecodeInitialization(immediate,mode)&&mode.interval==0x80000000u);}
  for(const uint32_t value:{2u,0x80000001u,UINT32_MAX}){auto invalid=init;invalid.args[5]=value;check(!d9::DecodeInitialization(invalid,mode));}
  for(size_t i=7;i<16;++i){auto invalid=init;invalid.args[i]=1;check(!d9::DecodeInitialization(invalid,mode));}
  {auto retain=init;retain.args[6]=1;check(d9::DecodeInitialization(retain,mode)&&mode.retainContent&&mode.window==0x80401234);}
  {auto annotate=init;annotate.args[6]=2;check(d9::DecodeInitialization(annotate,mode)&&mode.annotations&&mode.window==0x80401234);}
  {auto delta=init;delta.args[6]=4;check(d9::DecodeInitialization(delta,mode)&&mode.fixedDelta&&mode.window==0x80401234);}
  {auto invalid=init;invalid.id=1;check(!d9::DecodeInitialization(invalid,mode));}
  {auto invalid=init;invalid.payloadBytes=1;check(!d9::DecodeInitialization(invalid,mode));}
  // Back buffer format: 0 keeps A8R8G8B8, D3DFMT_X8R8G8B8 is the opaque target.
  check(d9::DecodeInitialization(init,mode)&&!mode.opaque);
  {auto opaque=init;opaque.args[7]=D3DFMT_X8R8G8B8;check(d9::DecodeInitialization(opaque,mode)&&mode.opaque);}
  for(const uint32_t value:{uint32_t(D3DFMT_A8R8G8B8),23u,UINT32_MAX}){auto invalid=init;invalid.args[7]=value;check(!d9::DecodeInitialization(invalid,mode));}
  {auto offscreen=init;offscreen.args[2]=offscreen.args[3]=offscreen.args[4]=offscreen.args[5]=0;offscreen.args[7]=d9::OpaqueBackBuffer;
    check(d9::DecodeInitialization(offscreen,mode)&&mode.opaque&&!mode.interval);
    offscreen.args[5]=d9::IntervalImmediate;check(!d9::DecodeInitialization(offscreen,mode));}
  check(d9::DecodeInitialization(init,mode));
  // Target extent: any 1..4096 per axis, reported back; nothing else.
  check(mode.width==64&&mode.height==64);
  for(const auto& extent:{std::array<uint32_t,2>{1,1},{1902,963},{4096,4096},{63,65},{4096,1}}){
    auto sized=init;sized.args[0]=extent[0];sized.args[1]=extent[1];
    check(d9::DecodeInitialization(sized,mode)&&mode.width==extent[0]&&mode.height==extent[1]);}
  for(const uint32_t value:{0u,4097u,65536u,UINT32_MAX})for(size_t axis:{size_t(0),size_t(1)}){
    auto invalid=init;invalid.args[axis]=value;check(!d9::DecodeInitialization(invalid,mode));}
  // Reset: extent, interval (args[5]) and format (args[7]) with Initialize's
  // meanings; the profile and window stay the session's.
  {d9::Header reset;reset.op=d9::Op::Reset;reset.args[0]=1280;reset.args[1]=720;d9::Resize resize;
    check(d9::DecodeReset(reset,false,resize)&&resize.width==1280&&resize.height==720&&!resize.interval&&!resize.opaque
      &&!resize.keepResources);
    check(!d9::DecodeReset(reset,true,resize));
    {auto wsi=reset;wsi.args[5]=d9::IntervalOne;wsi.args[7]=d9::OpaqueBackBuffer;
      check(d9::DecodeReset(wsi,true,resize)&&resize.interval==1&&resize.opaque&&!d9::DecodeReset(wsi,false,resize));
      wsi.args[5]=d9::IntervalImmediate;check(d9::DecodeReset(wsi,true,resize)&&resize.interval==0x80000000u);
      for(const uint32_t value:{0u,2u,0x80000001u}){auto invalid=wsi;invalid.args[5]=value;check(!d9::DecodeReset(invalid,true,resize));}}
    // The window's own resize keeps every resource; args[6] has no other flag.
    {auto window=reset;window.args[6]=d9::ResetKeepResources;
      check(d9::DecodeReset(window,false,resize)&&resize.keepResources&&resize.width==1280&&resize.height==720);
      for(const uint32_t value:{2u,3u,0x80000000u}){auto invalid=window;invalid.args[6]=value;check(!d9::DecodeReset(invalid,false,resize));}}
    for(size_t i:{size_t(2),size_t(3),size_t(4),size_t(8),size_t(15)}){auto invalid=reset;invalid.args[i]=1;check(!d9::DecodeReset(invalid,false,resize));}
    for(const uint32_t value:{0u,4097u})for(size_t axis:{size_t(0),size_t(1)}){auto invalid=reset;invalid.args[axis]=value;check(!d9::DecodeReset(invalid,false,resize));}
    {auto invalid=reset;invalid.args[7]=D3DFMT_A8R8G8B8;check(!d9::DecodeReset(invalid,false,resize));}
    {auto invalid=reset;invalid.id=1;invalid.generation=1;check(!d9::DecodeReset(invalid,false,resize));}
    {auto invalid=reset;invalid.payloadBytes=1;check(!d9::DecodeReset(invalid,false,resize));}
    {auto invalid=reset;invalid.op=d9::Op::Initialize;check(!d9::DecodeReset(invalid,false,resize));}
    {auto invalid=init;check(!d9::DecodeReset(invalid,true,resize));}
    check(d9::PresentSkipped==uint32_t(D3DERR_NOTAVAILABLE));}
  // Readback rows: (0,0) is the whole image only when it fits one reply;
  // otherwise whole-row bands of at most one reply.
  {uint32_t first=0,rows=0;check(d9::ResolveReadbackRows(64,64,first,rows)&&first==0&&rows==64);
    first=rows=0;check(!d9::ResolveReadbackRows(1902,963,first,rows));
    check(d9::ReadbackBandRows(1902)==8&&d9::ReadbackBandRows(4096)==3&&d9::ReadbackBandRows(1)==16360);
    first=960;rows=3;check(d9::ResolveReadbackRows(1902,963,first,rows));
    first=960;rows=4;check(!d9::ResolveReadbackRows(1902,963,first,rows));
    first=0;rows=9;check(!d9::ResolveReadbackRows(1902,963,first,rows));
    first=963;rows=1;check(!d9::ResolveReadbackRows(1902,963,first,rows));
    first=5;rows=0;check(!d9::ResolveReadbackRows(1902,963,first,rows));
    first=0;rows=1;check(!d9::ResolveReadbackRows(0,963,first,rows)&&!d9::ResolveReadbackRows(4097,1,first,rows));
    check(d9::PresentedInline(64,64)&&d9::PresentedInline(127,128)&&!d9::PresentedInline(128,128)&&!d9::PresentedInline(1902,963));}
  for(uint32_t i=0;i<6;++i)command.resources[i]={i+1,i+9};
  state.streamOffset=65560;state.stride=40;state.viewport={0,0,64,64,0.125f,0.875f};
  state.render.alphaBlendEnable=TRUE;state.baseVertexIndex=4;state.minVertexIndex=1;state.numVertices=4;state.startIndex=0;state.primitiveCount=2;
  for(size_t i=0;i<state.vertexFloatConstants.size();++i){state.vertexFloatConstants[i]=float(i)*0.5f;state.pixelFloatConstants[i]=-float(i)*0.25f;}
  const auto draw=d9::EncodeDraw(command);check(d9::DecodeDraw(draw.data(),draw.size(),copy));check(d9::EncodeDraw(copy)==draw);
  check(copy.state.vertexBuffer==0&&copy.state.indexBuffer==0&&copy.state.texture==0
    &&copy.state.vertexShader==0&&copy.state.pixelShader==0&&copy.state.declaration==0);
  for(uint32_t i=0;i<6;++i){
    check(copy.resources[i]==command.resources[i]);
    auto broken=draw;d9::P32(broken.data()+8*i+4,0);
    const auto saved=d9::EncodeDraw(copy);check(!d9::DecodeDraw(broken.data(),broken.size(),copy));
    check(d9::EncodeDraw(copy)==saved);
    broken=draw;d9::P32(broken.data()+8*i,d9::MaxResources+1);check(!d9::DecodeDraw(broken.data(),broken.size(),copy));
  }
  for(uint32_t i=0;i<d9::DrawBytes;++i)check(!d9::DecodeDraw(draw.data(),i,copy));
  check(d9::U32(draw.data()+48)==65560&&d9::U32(draw.data()+52)==40);
  {auto nonindexed=command;nonindexed.resources[1]={};
    const auto packet=d9::EncodeDraw(nonindexed);check(d9::DecodeDraw(packet.data(),packet.size(),copy));
    check(copy.resources[1]==d9::ResourceRef{}&&d9::EncodeDraw(copy)==packet);
    for(unsigned slot=0;slot<6;++slot)for(unsigned kind=0;kind<3;++kind){
      auto broken=packet;d9::P32(broken.data()+slot*8,kind==1?1:0);d9::P32(broken.data()+slot*8+4,kind==2?1:0);
      check(d9::DecodeDraw(broken.data(),broken.size(),copy)==((slot==1||slot==2)&&kind==0));
    }}
  // Exhaust the simultaneous empty-resource subsets, independently of the
  // decoder's per-slot predicates. VB and declaration are always required.
  for(unsigned empty=0;empty<64;++empty)for(bool fixed:{false,true}) {
    auto value=command;for(unsigned i=0;i<6;++i)if(empty&(1u<<i))value.resources[i]={};
    const auto bytes=d9::EncodeDraw(value);const unsigned permitted=fixed?0x1e:0x06;
    const auto before=d9::EncodeDraw(copy);const bool accepted=d9::DecodeDraw(bytes.data(),bytes.size(),copy,fixed);
    check(accepted==((empty&~permitted)==0));
    check(accepted?d9::EncodeDraw(copy)==bytes:d9::EncodeDraw(copy)==before);
  }
  std::printf("d3d9 wire checks=%u failures=%u bits=%zu drawBytes=%u\n",checks,failures,sizeof(void*)*8,d9::DrawBytes);
  return failures?1:0;
}
