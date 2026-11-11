// Real authenticated IPC -> production decoder/backend -> GPU unchanged and
// then recovery. No process-memory diagnostics or game launch.
#define wmain ExistingLifetimeFixtureMain
#include "test_render_host_d3d9_resource_lifetime.cpp"
#undef wmain
#include "../tools/render_host/d3d9_ff_wire.h"
#include "../tools/render_host/d3d9_texture_wire.h"
#include "../tools/render_host/d3d9_sampler_wire.h"
#include "../tools/render_host/d3d9_effects_wire.h"
#include "../tools/render_host/d3d9_texture_stages_wire.h"
#include "../tools/render_host/d3d9_annotation_wire.h"
#include <limits>
int wmain(int argc,wchar_t** argv) {
  try {
    const auto options=Parse(argc,argv);Desktop(options);Shaders shaders;Evidence evidence;
    // Optional negotiated host content authority (ReadContent positive cases).
    wchar_t retainMode[4]{};
    const bool retain=GetEnvironmentVariableW(L"WARVK_TEST_RAW_RETAIN",retainMode,4)==1&&retainMode[0]==L'1';
    // Optional negotiated data-layer annotations (positive and binding cases).
    wchar_t annotateMode[4]{};
    const bool annotate=GetEnvironmentVariableW(L"WARVK_TEST_RAW_ANNOTATE",annotateMode,4)==1&&annotateMode[0]==L'1';
    Raw r(shaders,evidence);auto h=Raw::Header(d9::Op::Initialize);h.args[0]=h.args[1]=64;
    h.args[6]=(retain?d9::InitializeRetainContent:0)|(annotate?d9::InitializeAnnotations:0);r.call(h);
    r.createScene(1,0xff11bb66);
    auto draw=r.draw();draw.resources[3]=draw.resources[4]={};
    d9::FixedFunctionState state;state.render[d9::FFIndex(d9::FFRenderStates,D3DRS_LIGHTING)]=FALSE;
    const auto valid=d9::EncodeFixedDraw(draw,state);
    unsigned rejected=0,recovered=0;uint64_t unchanged=0;
    // Exercise sustained normal draws before any invalid request. This catches
    // quick-pipeline/optimized-pipeline divergence independently of rejection.
    for(unsigned i=0;i<36;++i) {
      h=Raw::Header(d9::Op::Clear);h.args[0]=ClearColor;h.args[1]=d9::FloatBits(1);h.args[2]=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;r.call(h);
      h=Raw::Header(d9::Op::DrawFixed);h.payloadBytes=uint32_t(valid.size());r.call(h,valid.data());
      std::array<uint8_t,64*64*4> image{};r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));
      Pixels(evidence,image.data(),64*4,r.color);
    }
    for(unsigned test=0;test<24;++test) {
      h=Raw::Header(d9::Op::Clear);h.args[0]=ClearColor;h.args[1]=d9::FloatBits(1);h.args[2]=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;r.call(h);
      std::fprintf(stderr,"fixed negative case=%u\n",test);
      auto bad=valid;auto changedDraw=draw;auto changedState=state;
      h=Raw::Header(d9::Op::DrawFixed);h.payloadBytes=uint32_t(bad.size());
      switch(test) {
        case 0:--h.payloadBytes;break;
        case 1:h.args[0]=1;break;
        case 2:h.id=h.generation=1;break;
        case 3:changedState.transforms[10]._44=std::numeric_limits<float>::quiet_NaN();break;
        case 4:changedState.stages[7][0]=D3DTOP_BUMPENVMAP;break;
        case 5:changedState.stages[0][2]=D3DTA_CONSTANT;break;
        case 6:changedState.render[d9::FFIndex(d9::FFRenderStates,D3DRS_VERTEXBLEND)]=1;break;
        case 7:changedState.lightCount=9;break;
        case 8:changedState.lightCount=1;changedState.lights[0].Type=D3DLIGHTTYPE(99);break;
        case 9:changedState.material.Power=std::numeric_limits<float>::infinity();break;
        case 10:changedDraw.resources[5].generation=2;break;
        case 11:changedDraw.resources[0]=r.refs[2];break;
        case 12:changedDraw.resources[3]={0,1};break;
        case 13:changedDraw.resources[2]={0,1};break;
        case 14:changedDraw.resources[3]=r.refs[3];changedDraw.resources[4]=r.refs[4];break;
        case 15:changedDraw.state.viewport.Width=0;break;
        case 16:changedDraw.state.stride=1;break;
        case 17:changedDraw.state.startIndex=UINT32_MAX;break;
        case 18:changedDraw.state.primitiveCount=0;break;
        case 19:break;
        // Topologies: points stay refused, as do undefined types, more than
        // 65535 primitives, and a strip needing 7 of the 6 initialized indices.
        case 20:changedDraw.state.primitiveType=D3DPT_POINTLIST;break;
        case 21:changedDraw.state.primitiveType=D3DPRIMITIVETYPE(7);break;
        case 22:changedDraw.state.primitiveCount=65536;break;
        case 23:changedDraw.state.primitiveType=D3DPT_TRIANGLESTRIP;changedDraw.state.primitiveCount=5;break;
      }
      if(test>=3)bad=d9::EncodeFixedDraw(changedDraw,changedState);
      if(test==19)bad.back()=1; // Noncanonical unused light, at the end of the packet.
      Require(r.connection->call(h,bad.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"fixed input was not recoverably refused");
      ++rejected;std::array<uint8_t,64*64*4> image{};
      r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));
      for(unsigned i=0;i<64*64;++i){DWORD color=0;std::memcpy(&color,image.data()+4*i,4);
        Require(color==ClearColor,"rejected fixed draw changed pixels");++unchanged;}
      h=Raw::Header(d9::Op::DrawFixed);h.payloadBytes=uint32_t(valid.size());r.call(h,valid.data());
      r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));
      Pixels(evidence,image.data(),64*4,r.color);++recovered;
    }
    // Batched fixed draws: every entry is validated before the first records,
    // so a bad last entry leaves no rendered prefix. An asynchronous refusal is
    // terminal by contract; that profile runs only the accepted batches.
    const bool asyncBatches=GetEnvironmentVariableW(L"WARVK_D3D9_ASYNC_BATCH",nullptr,0)!=0;
    unsigned batchRejected=0,batchRecovered=0,batchDraws=0;uint64_t batchUnchanged=0;
    std::vector<uint8_t> batch(d9::MaxFixedDrawBatch*d9::FixedDrawBytes);
    auto fillBatch=[&](uint32_t count){for(uint32_t i=0;i<count;++i)std::copy(valid.begin(),valid.end(),batch.begin()+i*d9::FixedDrawBytes);};
    auto batchHeader=[&](uint32_t count){auto c=Raw::Header(d9::Op::DrawFixedBatch);c.args[0]=count;c.payloadBytes=count*d9::FixedDrawBytes;return c;};
    auto clearTarget=[&](){h=Raw::Header(d9::Op::Clear);h.args[0]=ClearColor;h.args[1]=d9::FloatBits(1);h.args[2]=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;r.call(h);};
    auto acceptBatch=[&](uint32_t count){
      fillBatch(count);r.call(batchHeader(count),batch.data());batchDraws+=count;
      std::array<uint8_t,64*64*4> image{};r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));
      Pixels(evidence,image.data(),64*4,r.color);
    };
    clearTarget();acceptBatch(d9::MaxFixedDrawBatch);
    for(unsigned test=0;!asyncBatches&&test<12;++test) {
      clearTarget();std::fprintf(stderr,"fixed batch negative case=%u\n",test);
      fillBatch(d9::MaxFixedDrawBatch);auto c=batchHeader(d9::MaxFixedDrawBatch);
      auto changedDraw=draw;auto changedState=state;uint32_t entry=d9::MaxFixedDrawBatch-1;bool reencode=true;
      switch(test) {
        case 0:c.args[0]=0;c.payloadBytes=0;reencode=false;break;
        case 1:c.args[0]=d9::MaxFixedDrawBatch+1;reencode=false;break;
        case 2:c.payloadBytes-=d9::FixedDrawBytes;reencode=false;break;
        case 3:c.args[1]=1;reencode=false;break;
        case 4:c.id=c.generation=1;reencode=false;break;
        case 5:changedState.transforms[0]._11=std::numeric_limits<float>::quiet_NaN();break;
        case 6:entry=d9::MaxFixedDrawBatch/2;changedState.lightCount=9;break;
        case 7:changedDraw.resources[5].generation=2;break;
        case 8:entry=0;changedDraw.resources[3]=r.refs[3];changedDraw.resources[4]=r.refs[4];break;
        // Decoded and resolved, refused only by the backend's all-entry prepare.
        case 9:changedDraw.state.primitiveCount=0;break;
        case 10:changedDraw.state.viewport.Width=0;break;
        case 11:batch.back()=1;reencode=false;break; // Last entry's unused light is noncanonical.
      }
      if(reencode){const auto bad=d9::EncodeFixedDraw(changedDraw,changedState);std::copy(bad.begin(),bad.end(),batch.begin()+entry*d9::FixedDrawBytes);}
      Require(r.connection->call(c,batch.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"fixed batch was not atomically refused");
      ++batchRejected;std::array<uint8_t,64*64*4> image{};
      r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));
      for(unsigned i=0;i<64*64;++i){DWORD color=0;std::memcpy(&color,image.data()+4*i,4);
        Require(color==ClearColor,"rejected fixed batch rendered a prefix");++batchUnchanged;}
      acceptBatch(3);++batchRecovered;
    }
    // Cross-packet texture upload must not publish a prefix, mix generations,
    // or retain an abandoned transaction after resource destruction.
    unsigned textureRejected=0,textureRecoveries=0;
    r.destroy(r.refs[2],d9::Type::Texture);r.refs[2].generation=2;
    auto createTexture=[&](){auto c=Raw::Header(d9::Op::CreateTexture2D,r.refs[2]);c.args[0]=257;c.args[1]=129;c.args[2]=1;c.args[3]=D3DFMT_A8R8G8B8;r.call(c);};
    createTexture();draw.resources[2]=r.refs[2];
    const auto textureDraw=d9::EncodeFixedDraw(draw,state);
    auto rejectedTexture=[&](d9::Header c,const void* data=nullptr){Require(r.connection->call(c,data)==D3DERR_INVALIDCALL&&r.connection->healthy(),"texture transaction rejection");++textureRejected;};
    auto begin=[&](uint32_t revision){auto c=Raw::Header(d9::Op::BeginTextureUpload,r.refs[2]);c.args[0]=revision;c.args[4]=257;c.args[5]=129;return c;};
    auto end=[&](d9::Op op,uint32_t revision){auto c=Raw::Header(op,r.refs[2]);c.args[0]=revision;return c;};
    std::vector<DWORD> texels(257*129,0xff11bb66);const uint32_t total=uint32_t(texels.size()*4);
    auto chunk=[&](uint32_t revision,uint32_t offset,uint32_t count){auto c=Raw::Header(d9::Op::TextureUploadChunk,r.refs[2]);c.args[0]=revision;c.args[1]=offset;c.payloadBytes=count;return c;};
    auto upload=[&](uint32_t revision){for(uint32_t offset=0;offset<total;){const auto count=std::min(d9::MaxPayload,total-offset);r.call(chunk(revision,offset,count),reinterpret_cast<uint8_t*>(texels.data())+offset);offset+=count;}};
    auto observe=[&](bool render,const std::array<uint8_t,d9::FixedDrawBytes>& packet){
      if(render){auto c=Raw::Header(d9::Op::Clear);c.args[0]=ClearColor;c.args[1]=d9::FloatBits(1);c.args[2]=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;r.call(c);
        c=Raw::Header(d9::Op::DrawFixed);c.payloadBytes=uint32_t(packet.size());r.call(c,packet.data());++textureRecoveries;}
      std::array<uint8_t,64*64*4> image{};r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),uint32_t(image.size()));Pixels(evidence,image.data(),64*4,r.color);
    };
    auto bad=begin(1);bad.args[4]=258;rejectedTexture(bad);
    r.call(begin(1));rejectedTexture(begin(2));
    rejectedTexture(chunk(2,0,4),texels.data());rejectedTexture(chunk(1,4,4),texels.data());
    rejectedTexture(end(d9::Op::CommitTextureUpload,1));upload(1);r.call(end(d9::Op::CommitTextureUpload,1));observe(true,textureDraw);
    std::fill(texels.begin(),texels.end(),0xffcc5511);
    r.call(begin(2));r.call(chunk(2,0,d9::MaxPayload),texels.data());
    rejectedTexture(end(d9::Op::CommitTextureUpload,2));bad=chunk(2,d9::MaxPayload,4);bad.generation=1;rejectedTexture(bad,texels.data());
    rejectedTexture(end(d9::Op::AbortTextureUpload,1));observe(false,textureDraw);
    r.call(end(d9::Op::AbortTextureUpload,2));rejectedTexture(chunk(2,d9::MaxPayload,4),texels.data());observe(true,textureDraw);
    r.call(begin(3));upload(3);r.call(end(d9::Op::CommitTextureUpload,3));r.color=0xffcc5511;observe(true,textureDraw);
    r.call(begin(4));r.call(chunk(4,0,4),texels.data());const auto oldCommit=end(d9::Op::CommitTextureUpload,4);
    r.destroy(r.refs[2],d9::Type::Texture);r.refs[2].generation=3;createTexture();rejectedTexture(oldCommit);
    r.call(begin(1));upload(1);r.call(end(d9::Op::CommitTextureUpload,1));draw.resources[2]=r.refs[2];observe(true,d9::EncodeFixedDraw(draw,state));
    // Block-compressed traffic uses the same authenticated transaction owner.
    // Invalid format/block domains must not consume a resource generation or
    // publish an incomplete block prefix to the GPU.
    r.destroy(r.refs[2],d9::Type::Texture);r.refs[2].generation=4;
    auto compressed=Raw::Header(d9::Op::CreateTexture2D,r.refs[2]);
    compressed.args[0]=128;compressed.args[1]=64;compressed.args[2]=1;compressed.args[3]=D3DFMT_D24S8;
    rejectedTexture(compressed);compressed.args[3]=D3DFMT_DXT1;r.call(compressed);
    auto compressedBegin=Raw::Header(d9::Op::BeginTextureUpload,r.refs[2]);
    compressedBegin.args[0]=1;compressedBegin.args[4]=128;compressedBegin.args[5]=64;
    bad=compressedBegin;bad.args[2]=1;bad.args[4]=4;rejectedTexture(bad);
    bad=compressedBegin;bad.args[4]=4;bad.args[5]=4;rejectedTexture(bad); // first commit must cover whole level
    bad=compressedBegin;bad.args[4]=3;rejectedTexture(bad);
    bad=compressedBegin;bad.args[5]=3;rejectedTexture(bad);
    bad=compressedBegin;bad.args[1]=1;rejectedTexture(bad);
    r.call(compressedBegin);
    std::array<uint8_t,32*16*8> blocks{};
    for(size_t i=0;i<blocks.size();i+=8){blocks[i]=0xe0;blocks[i+1]=7;} // RGB565 green, selector 0
    r.call(chunk(1,0,8),blocks.data());rejectedTexture(end(d9::Op::CommitTextureUpload,1));
    observe(false,textureDraw);
    r.call(chunk(1,8,uint32_t(blocks.size()-8)),blocks.data()+8);r.call(end(d9::Op::CommitTextureUpload,1));
    draw.resources[2]=r.refs[2];r.color=0xff00ff00;observe(true,d9::EncodeFixedDraw(draw,state));
    unsigned samplerRejected=0,samplerRecoveries=0;
    d9::SamplerState sampler(true);const auto goodSampler=d9::EncodeSamplerState(sampler);
    for(unsigned test=0;test<11;++test) {
      auto packet=goodSampler;h=Raw::Header(d9::Op::SetSamplerState);h.payloadBytes=uint32_t(packet.size());
      switch(test) {
        case 0:--h.payloadBytes;break;
        case 1:d9::P32(packet.data(),1);break;
        case 2:d9::P32(packet.data()+D3DSAMP_ADDRESSU*4,0);break;
        case 3:d9::P32(packet.data()+D3DSAMP_MAGFILTER*4,D3DTEXF_NONE);break;
        case 4:d9::P32(packet.data()+D3DSAMP_MIPFILTER*4,D3DTEXF_ANISOTROPIC);break;
        case 5:d9::P32(packet.data()+D3DSAMP_MIPMAPLODBIAS*4,0x7fc00000u);break;
        case 6:d9::P32(packet.data()+D3DSAMP_SRGBTEXTURE*4,2);break;
        case 7:d9::P32(packet.data()+D3DSAMP_ELEMENTINDEX*4,1);break;
        case 8:d9::P32(packet.data()+D3DSAMP_DMAPOFFSET*4,1);break;
        case 9:h.id=h.generation=1;break;
        case 10:h.args[1]=1;break;
      }
      Require(r.connection->call(h,packet.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"sampler recoverable rejection");++samplerRejected;
      observe(false,textureDraw);
      h=Raw::Header(d9::Op::SetSamplerState);h.payloadBytes=uint32_t(goodSampler.size());r.call(h,goodSampler.data());
      observe(true,d9::EncodeFixedDraw(draw,state));--textureRecoveries;++samplerRecoveries;
    }
    unsigned effectsRejected=0,effectsRecoveries=0;
    const d9::EffectsState effects;const auto goodEffects=d9::EncodeEffects(effects);
    for(unsigned test=0;test<12;++test){
      auto packet=goodEffects;h=Raw::Header(d9::Op::SetEffects);h.payloadBytes=uint32_t(packet.size());
      // A valid destructive prefix must not leak through a later bad field.
      d9::P32(packet.data(),TRUE);d9::P32(packet.data()+4,D3DCMP_NEVER);
      switch(test){
        case 0:--h.payloadBytes;break;
        case 1:d9::P32(packet.data(),2);break;
        case 2:d9::P32(packet.data()+4,0);break;
        case 3:d9::P32(packet.data()+4,9);break;
        case 4:d9::P32(packet.data()+5*4,4);break;
        case 5:d9::P32(packet.data()+9*4,0x7fc00000);break;
        case 6:d9::P32(packet.data()+10*4,2);break;
        case 7:d9::P32(packet.data()+11*4,2);break;
        case 8:d9::P32(packet.data()+12*4,0);break;
        case 9:d9::P32(packet.data()+14*4,0);break;
        case 10:h.id=h.generation=1;break;
        case 11:h.args[0]=1;break;
      }
      Require(r.connection->call(h,packet.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"effects recoverable rejection");++effectsRejected;
      observe(true,d9::EncodeFixedDraw(draw,state));--textureRecoveries;++effectsRecoveries;
      h=Raw::Header(d9::Op::SetEffects);h.payloadBytes=uint32_t(goodEffects.size());r.call(h,goodEffects.data());
    }
    unsigned stagesRejected=0,stagesDraws=0;
    d9::ResourceRef extra{10,1};DWORD extraColor=0xffff0000;
    auto createExtra=[&](){auto c=Raw::Header(d9::Op::CreateTexture,extra);c.args[0]=c.args[1]=1;r.call(c);
      c=Raw::Header(d9::Op::UploadTexture,extra);c.payloadBytes=4;r.call(c,&extraColor);};createExtra();
    d9::TextureStages stages;stages.textures.fill(r.refs[2]);
    auto bindStages=[&](){const auto bytes=d9::EncodeTextureStages(stages);auto c=Raw::Header(d9::Op::SetTextureStages);c.payloadBytes=bytes.size();r.call(c,bytes.data());};bindStages();
    state.stages[1][d9::FFIndex(d9::FFStageStates,D3DTSS_COLOROP)]=D3DTOP_SELECTARG1;
    state.stages[1][d9::FFIndex(d9::FFStageStates,D3DTSS_ALPHAOP)]=D3DTOP_SELECTARG1;
    auto stageImage=[&](){observe(true,d9::EncodeFixedDraw(draw,state));--textureRecoveries;++stagesDraws;};stageImage();
    for(unsigned test=0;test<10;++test){
      auto poison=stages;poison.textures[0]=extra;auto bytes=d9::EncodeTextureStages(poison);
      h=Raw::Header(d9::Op::SetTextureStages);h.payloadBytes=bytes.size();
      auto* tail=bytes.data()+6*d9::TextureStageBytes;
      switch(test){
        case 0:--h.payloadBytes;break;
        case 1:h.args[0]=1;break;
        case 2:h.id=h.generation=1;break;
        case 3:d9::P32(tail,0);break;
        case 4:d9::P32(tail+4,0);break;
        case 5:d9::P32(tail,65);break;
        case 6:d9::P32(tail+4,3);break;
        case 7:d9::P32(tail,r.refs[0].id);d9::P32(tail+4,r.refs[0].generation);break;
        case 8:d9::P32(tail+8+D3DSAMP_ADDRESSU*4,0);break;
        case 9:d9::P32(tail+8+D3DSAMP_MIPMAPLODBIAS*4,0x7fc00000);break;
      }
      Require(r.connection->call(h,bytes.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"texture stages partial transaction accepted");
      ++stagesRejected;stageImage(); // Must remain green, not the valid red prefix.
    }
    stages.textures[0]=extra;bindStages();r.color=extraColor;stageImage();
    r.destroy(extra,d9::Type::Texture);
    auto rejectStaleDraw=[&](){const auto bytes=d9::EncodeFixedDraw(draw,state);auto c=Raw::Header(d9::Op::DrawFixed);c.payloadBytes=bytes.size();
      Require(r.connection->call(c,bytes.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"destroyed/reused stage identity accepted");
      ++stagesRejected;observe(false,bytes);};rejectStaleDraw();
    ++extra.generation;extraColor=0xff0000ff;createExtra();rejectStaleDraw();
    stages.textures[0]=extra;bindStages();r.color=extraColor;stageImage();
    stages.textures.fill({});bindStages();r.destroy(extra,d9::Type::Texture);
    // ReadContent reads only the host's retained CPU copies. Without the
    // negotiated RetainContent feature every request is a recoverable refusal.
    unsigned contentRejected=0,contentReads=0;uint64_t contentReadBytes=0;
    auto read=[&](d9::ResourceRef ref,uint32_t level,uint32_t offset,uint32_t count){
      auto c=Raw::Header(d9::Op::ReadContent,ref);c.args[0]=level;c.args[1]=offset;c.args[2]=count;return c;};
    auto rejectedRead=[&](d9::Header c,const void* data=nullptr){
      Require(r.connection->call(c,data)==D3DERR_INVALIDCALL&&r.connection->healthy(),"content read was not recoverably refused");
      ++contentRejected;};
    if(!retain)rejectedRead(read(r.refs[2],0,0,8));
    else {
      std::vector<uint8_t> level(blocks.size());
      r.call(read(r.refs[2],0,0,uint32_t(level.size())),nullptr,level.data(),uint32_t(level.size()));
      Require(std::equal(level.begin(),level.end(),blocks.begin()),"retained texture level differs from its upload");
      std::array<uint8_t,8> part{};r.call(read(r.refs[2],0,8,8),nullptr,part.data(),8);
      Require(std::equal(part.begin(),part.end(),blocks.begin()+8),"retained texture range differs from its upload");
      contentReads+=2;contentReadBytes+=level.size()+part.size();
      const auto vertexBytes=uint32_t(sizeof(Vertices));
      std::vector<uint8_t> pattern(vertexBytes),vertex(vertexBytes);
      for(size_t i=0;i<pattern.size();++i)pattern[i]=uint8_t(i*29+3);
      auto upload=Raw::Header(d9::Op::UploadBuffer,r.refs[0]);upload.payloadBytes=vertexBytes;r.call(upload,pattern.data());
      r.call(read(r.refs[0],0,0,vertexBytes),nullptr,vertex.data(),vertexBytes);
      Require(vertex==pattern,"retained vertex buffer differs from its upload");++contentReads;contentReadBytes+=vertexBytes;
      rejectedRead(read(r.refs[2],0,0,0));
      rejectedRead(read(r.refs[2],0,0,d9::MaxContentRead+1));
      rejectedRead(read(r.refs[2],0,uint32_t(blocks.size())-4,8));
      rejectedRead(read(r.refs[2],1,0,8));
      {auto c=read(r.refs[2],0,0,8);c.args[3]=1;rejectedRead(c);}
      {auto c=read(r.refs[2],0,0,8);c.payloadBytes=4;rejectedRead(c,blocks.data());}
      auto indexUpload=Raw::Header(d9::Op::UploadBuffer,r.refs[1]);indexUpload.payloadBytes=sizeof(Indices);
      r.call(indexUpload,Indices.data());std::array<uint16_t,6> index{};
      r.call(read(r.refs[1],0,0,sizeof(index)),nullptr,index.data(),sizeof(index));
      Require(index==Indices,"retained index buffer differs from its upload");++contentReads;contentReadBytes+=sizeof(index);
      rejectedRead(read(r.refs[0],1,0,8)); // A buffer has no level.
      rejectedRead(read(r.refs[1],1,0,2));
      rejectedRead(read(r.refs[0],0,vertexBytes,1)); // Beyond the initialized prefix.
      rejectedRead(read({r.refs[2].id,r.refs[2].generation-1},0,0,8));
      // An open upload transaction owns the level until commit or abort.
      auto open=compressedBegin;open.args[0]=2;r.call(open);
      rejectedRead(read(r.refs[2],0,0,8));r.call(end(d9::Op::AbortTextureUpload,2));
      const d9::ResourceRef blank{extra.id,extra.generation+1};
      auto create=Raw::Header(d9::Op::CreateTexture2D,blank);create.args[0]=create.args[1]=4;create.args[2]=1;
      create.args[3]=D3DFMT_A8R8G8B8;r.call(create);
      rejectedRead(read(blank,0,0,8)); // Never initialized: nothing retained to read.
      r.destroy(blank,d9::Type::Texture);
    }
    // Data-layer annotations. Without the negotiated feature each op is a
    // recoverable refusal. With it, records bind only to the next, declared
    // draw message; refused traffic leaves nothing pending, which the plain
    // draw after each refusal and the host's final digest both observe.
    // AnnotateDraws and batch refusals are terminal under async batches, so
    // that profile keeps only the accepted traffic and synchronous refusals.
    unsigned annotationRejected=0,annotationDraws=0;d9::AnnotationRecorder expected;
    const auto tagSchema=uint16_t(d9::AnnotationSchema::DrawTag);
    auto refusedAnnotation=[&](d9::Header c,const void* data=nullptr){
      Require(r.connection->call(c,data)==D3DERR_INVALIDCALL&&r.connection->healthy(),"annotation was not recoverably refused");
      ++annotationRejected;};
    auto records=[&](std::initializer_list<std::pair<uint16_t,uint64_t>> entries){
      std::vector<uint8_t> out(entries.size()*d9::RecordBytes(d9::DrawTagBytes));uint32_t at=0;
      for(const auto& [index,object]:entries){const auto tag=d9::EncodeDrawTag({object,2,1});
        at+=d9::AppendDrawRecord(out.data()+at,uint32_t(out.size()-at),index,tagSchema,tag.data(),d9::DrawTagBytes);}
      Require(at==out.size(),"annotation record encoding");return out;};
    auto annotateHeader=[&](uint32_t count,uint32_t draws,d9::Op op,size_t bytes){
      auto c=Raw::Header(d9::Op::AnnotateDraws);c.args[0]=count;c.args[1]=draws;c.args[2]=uint32_t(op);c.payloadBytes=uint32_t(bytes);return c;};
    d9::FrameCamera camera;camera.view={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};camera.projection=camera.view;
    camera.viewport={0,0,64,64};camera.depthRange={0,1};camera.frame=7;camera.gameTime=1.25f;
    const auto scene=d9::EncodeFrameCamera(camera);const auto sceneSchema=uint32_t(d9::AnnotationSchema::FrameCamera);
    auto sceneHeader=[&](){auto c=Raw::Header(d9::Op::FrameScene);c.args[0]=sceneSchema;c.payloadBytes=d9::FrameCameraBytes;return c;};
    auto stageHeader=[&](uint32_t stage){auto c=Raw::Header(d9::Op::StageMarker);c.args[0]=stage;return c;};
    if(!annotate) {
      const auto one=records({{0,1}});
      if(!asyncBatches)refusedAnnotation(annotateHeader(1,1,d9::Op::DrawFixed,one.size()),one.data());
      refusedAnnotation(sceneHeader(),scene.data());refusedAnnotation(stageHeader(1));
    } else {
      state.stages[1][d9::FFIndex(d9::FFStageStates,D3DTSS_COLOROP)]=D3DTOP_DISABLE;
      state.stages[1][d9::FFIndex(d9::FFStageStates,D3DTSS_ALPHAOP)]=D3DTOP_DISABLE;
      // Distinct draw shapes inside one batch make a misplaced record visible.
      auto shaped=[&](uint32_t start,uint32_t primitives){auto d=draw;d.state.startIndex=start;d.state.primitiveCount=primitives;return d;};
      const auto plain=d9::EncodeFixedDraw(draw,state);
      auto drawFixed=[&](){auto c=Raw::Header(d9::Op::DrawFixed);c.payloadBytes=uint32_t(plain.size());r.call(c,plain.data());++annotationDraws;};
      const std::array<d9::DrawCommand,3> shapes{shaped(0,2),shaped(3,1),shaped(0,1)};
      std::vector<uint8_t> batched(shapes.size()*d9::FixedDrawBytes);
      for(size_t i=0;i<shapes.size();++i){const auto p=d9::EncodeFixedDraw(shapes[i],state);std::copy(p.begin(),p.end(),batched.begin()+i*d9::FixedDrawBytes);}
      auto batchHeader=[&](uint32_t count){auto c=Raw::Header(d9::Op::DrawFixedBatch);c.args[0]=count;c.payloadBytes=count*d9::FixedDrawBytes;return c;};
      auto expectDraw=[&](uint64_t object,const d9::DrawCommand& d){const auto tag=d9::EncodeDrawTag({object,2,1});
        expected.draw(tagSchema,tag.data(),d9::DrawTagBytes,uint32_t(d.state.primitiveType),d.state.primitiveCount,d.state.numVertices,d.state.startIndex);
        ++expected.annotatedDraws;};
      r.call(sceneHeader(),scene.data());expected.scene(sceneSchema,scene.data(),d9::FrameCameraBytes);
      r.call(stageHeader(uint32_t(d9::Stage::WorldBegin)));expected.stage(uint32_t(d9::Stage::WorldBegin));
      const auto one=records({{0,1}});
      r.call(annotateHeader(1,1,d9::Op::DrawFixed,one.size()),one.data());drawFixed();expectDraw(1,draw);
      const auto pair=records({{0,2},{2,3}});
      r.call(annotateHeader(2,3,d9::Op::DrawFixedBatch,pair.size()),pair.data());r.call(batchHeader(3),batched.data());
      annotationDraws+=3;expectDraw(2,shapes[0]);expectDraw(3,shapes[2]);
      r.call(stageHeader(uint32_t(d9::Stage::BeforeUi)));expected.stage(uint32_t(d9::Stage::BeforeUi));
      // Scene and stage refusals.
      {auto c=sceneHeader();c.payloadBytes=159;refusedAnnotation(c,scene.data());}
      {auto c=sceneHeader();c.args[0]=tagSchema;refusedAnnotation(c,scene.data());}
      {auto bad=scene;d9::P32(bad.data()+4,0x7fc00000u);refusedAnnotation(sceneHeader(),bad.data());}
      {auto c=sceneHeader();c.args[1]=1;refusedAnnotation(c,scene.data());}
      {auto c=sceneHeader();c.id=c.generation=1;refusedAnnotation(c,scene.data());}
      refusedAnnotation(stageHeader(0));refusedAnnotation(stageHeader(5));
      {auto c=stageHeader(1);c.payloadBytes=4;refusedAnnotation(c,scene.data());}
      {auto c=stageHeader(1);c.args[1]=1;refusedAnnotation(c);}
      // Malformed records leave nothing pending for the plain draw after them.
      auto malformed=[&](d9::Header c,const std::vector<uint8_t>& payload){refusedAnnotation(c,payload.data());drawFixed();};
      if(!asyncBatches) {
        malformed(annotateHeader(0,1,d9::Op::DrawFixed,one.size()),one);
        malformed(annotateHeader(1,0,d9::Op::DrawFixed,one.size()),one);
        malformed(annotateHeader(1,1,d9::Op::Clear,one.size()),one);
        malformed(annotateHeader(1,1,d9::Op::DrawFixed,24),records({{1,9}}));
        {auto bad=one;bad[6]=1;malformed(annotateHeader(1,1,d9::Op::DrawFixed,bad.size()),bad);}
        {auto bad=one;bad[2]=2;malformed(annotateHeader(1,1,d9::Op::DrawFixed,bad.size()),bad);}
        {auto bad=one;bad[4]=12;malformed(annotateHeader(1,1,d9::Op::DrawFixed,bad.size()),bad);}
        malformed(annotateHeader(2,3,d9::Op::DrawFixedBatch,48),records({{2,5},{0,6}}));
        malformed(annotateHeader(2,3,d9::Op::DrawFixedBatch,48),records({{0,5},{0,6}}));
        {auto longer=one;longer.resize(one.size()+4);malformed(annotateHeader(1,1,d9::Op::DrawFixed,longer.size()),longer);}
        {auto c=annotateHeader(1,1,d9::Op::DrawFixed,one.size());c.args[3]=1;malformed(c,one);}
        {auto c=annotateHeader(1,1,d9::Op::DrawFixed,one.size());c.id=c.generation=1;malformed(c,one);}
        // A valid record whose next message is not its declared draw: that
        // message is refused and the record dropped. A refused Clear is
        // terminal (asynchronous), so the non-draw probes are a stage marker
        // and a synchronous Readback.
        const auto twoOfTwo=records({{0,2},{1,3}});
        auto pending=[&](uint32_t count,uint32_t draws,d9::Op op,const std::vector<uint8_t>& p){r.call(annotateHeader(count,draws,op,p.size()),p.data());};
        pending(1,1,d9::Op::DrawFixed,one);refusedAnnotation(stageHeader(1));drawFixed();
        pending(2,3,d9::Op::DrawFixedBatch,pair);{auto c=Raw::Header(d9::Op::DrawFixed);c.payloadBytes=uint32_t(plain.size());refusedAnnotation(c,plain.data());}drawFixed();
        pending(2,2,d9::Op::DrawFixedBatch,twoOfTwo);refusedAnnotation(batchHeader(3),batched.data());drawFixed();
        pending(1,1,d9::Op::DrawFixed,one);refusedAnnotation(annotateHeader(1,1,d9::Op::DrawFixed,one.size()),one.data());drawFixed();
        pending(1,1,d9::Op::DrawFixed,one);{auto c=Raw::Header(d9::Op::DrawFixed);c.payloadBytes=uint32_t(plain.size())-1;refusedAnnotation(c,plain.data());}drawFixed();
        pending(1,1,d9::Op::DrawFixed,one);refusedAnnotation(Raw::Header(d9::Op::Readback));drawFixed();
      }
      // Recovery: the next valid record binds normally.
      const auto last=records({{0,4}});
      r.call(annotateHeader(1,1,d9::Op::DrawFixed,last.size()),last.data());drawFixed();expectDraw(4,draw);
      r.call(stageHeader(uint32_t(d9::Stage::FrameEnd)));expected.stage(uint32_t(d9::Stage::FrameEnd));
    }
    for(unsigned i=0;i<6;++i)r.destroy(r.refs[i],Types[i]);
    r.call(Raw::Header(d9::Op::Finish));r.connection.reset();Desktop(options);
    std::printf("{\"schema\":\"warvk-fixed-rejection/v1\",\"ok\":true,\"normalBeforeRejection\":36,\"rejected\":%u,\"recoveries\":%u,"
      "\"unchangedPixels\":%llu,\"recoveryPixelChecks\":%u,\"pixelFailures\":%u,"
      "\"textureRejected\":%u,\"textureRecoveries\":%u,\"samplerRejected\":%u,\"samplerRecoveries\":%u,\"effectsRejected\":%u,\"effectsRecoveries\":%u,\"stagesRejected\":%u,\"stagesDraws\":%u,"
      "\"fixedBatchAsync\":%s,\"fixedBatchRejected\":%u,\"fixedBatchRecoveries\":%u,\"fixedBatchDraws\":%u,\"fixedBatchUnchangedPixels\":%llu,"
      "\"contentRetain\":%s,\"contentRejected\":%u,\"contentReads\":%u,\"contentReadBytes\":%llu,"
      "\"annotate\":%s,\"annotationRejected\":%u,\"annotationDraws\":%u,\"annotatedDraws\":%llu,\"annotationRecords\":%llu,"
      "\"frameScenes\":%llu,\"stageMarkers\":%llu,\"annotationDigest\":\"%016llx\","
      "\"memoryMeasured\":false,\"gameTouched\":false}\n",rejected,recovered,unchanged,evidence.pixels,evidence.failures,textureRejected,textureRecoveries,samplerRejected,samplerRecoveries,effectsRejected,effectsRecoveries,stagesRejected,stagesDraws,
      asyncBatches?"true":"false",batchRejected,batchRecovered,batchDraws,batchUnchanged,
      retain?"true":"false",contentRejected,contentReads,contentReadBytes,
      annotate?"true":"false",annotationRejected,annotationDraws,(unsigned long long)expected.annotatedDraws,(unsigned long long)expected.drawRecords,
      (unsigned long long)expected.frameScenes,(unsigned long long)expected.stageMarkers,(unsigned long long)expected.digest);
    return 0;
  }catch(const std::exception& error){std::fprintf(stderr,"fixed rejection: %s\n",error.what());return 1;}
}
