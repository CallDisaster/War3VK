#define wmain ExistingLifetimeFixtureMain
#include "test_render_host_d3d9_resource_lifetime.cpp"
#undef wmain
#include <limits>
#include "../tools/render_host/d3d9_memory_probe.h"

int wmain(int argc,wchar_t** argv) {
  try {
    const auto options=Parse(argc,argv);Desktop(options);Shaders shaders;Evidence evidence;
    Raw r(shaders,evidence);auto h=Raw::Header(d9::Op::Initialize);h.args[0]=h.args[1]=64;r.call(h);
    r.createScene(1,0xff11bb66);
    unsigned rejected=0,recovered=0;uint64_t blackChecks=0;
    for(unsigned test=0;test<29;++test) {
      h=Raw::Header(d9::Op::Clear);h.args[0]=ClearColor;h.args[1]=d9::FloatBits(1);h.args[2]=D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER;r.call(h);
      d9::DrawBatchPacket bytes{};
      for(unsigned i=0;i<32;++i){const auto packet=d9::EncodeDraw(r.draw());std::memcpy(bytes.data()+i*d9::DrawBytes,packet.data(),packet.size());}
      auto bad=bytes;h=Raw::Header(d9::Op::DrawBatch);h.args[0]=32;h.payloadBytes=UINT(bytes.size());
      if(test<24) {
        const unsigned position=std::array<unsigned,3>{0,15,31}[test%3];auto command=r.draw();
        switch(test/3) {
          case 0:command.resources[0].generation=2;break;
          case 1:command.resources[2]=command.resources[0];break;
          case 2:command.state.primitiveCount=0;break;
          case 3:command.state.startIndex=65535;break;
          case 4:command.state.vertexFloatConstants[31]=std::numeric_limits<float>::quiet_NaN();break;
          case 5:command.state.pixelFloatConstants[127]=std::numeric_limits<float>::infinity();break;
          case 6:command.state.viewport.Width=0;break;
          case 7:command.state.streamOffset=UINT32_MAX;break;
        }
        const auto packet=d9::EncodeDraw(command);std::memcpy(bad.data()+position*d9::DrawBytes,packet.data(),packet.size());
      } else if(test==24)h.args[0]=0;
      else if(test==25)h.args[0]=33;
      else if(test==26)--h.payloadBytes;
      else if(test==27)h.args[1]=1;
      else {h.id=1;h.generation=1;}
      Require(r.connection->call(h,bad.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"bad batch not recoverably refused");++rejected;
      std::array<uint8_t,64*64*4> image{};r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),UINT(image.size()));
      for(unsigned i=0;i<64*64;++i){DWORD pixel=0;std::memcpy(&pixel,image.data()+i*4,4);Require(pixel==ClearColor,"invalid batch rendered a prefix");++blackChecks;}
      h=Raw::Header(d9::Op::DrawBatch);h.args[0]=32;h.payloadBytes=UINT(bytes.size());r.call(h,bytes.data());
      r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),UINT(image.size()));Pixels(evidence,image.data(),64*4,r.color);++recovered;
    }
    // Fill and wrap the flight ring, then destroy while its final batches may
    // still be in flight. No query/readback settles it before Destroy. Reuse
    // every typed ID with a new generation, reject the old generation in each
    // resource position, and prove that neither reuse nor refusal redraws A.
    d9::DrawBatchPacket live{};
    const auto oldRefs=r.refs;
    const auto oldColor=r.color;
    const auto encodeBatch=[&](const d9::DrawCommand& command) {
      const auto packet=d9::EncodeDraw(command);
      for(unsigned i=0;i<32;++i)std::memcpy(live.data()+i*d9::DrawBytes,packet.data(),packet.size());
    };
    encodeBatch(r.draw());
    h=Raw::Header(d9::Op::DrawBatch);h.args[0]=32;h.payloadBytes=UINT(live.size());
    for(unsigned batch=0;batch<5;++batch)r.call(h,live.data());
    unsigned pendingDiagnosticRefused=0;
    wchar_t groupMode[8]{},streamMode[8]{};
    // Both host profiles hold recorded batches unsubmitted until a sync point.
    if(GetEnvironmentVariableW(L"WARVK_D3D9_GPU_GROUP",groupMode,8)
      ||GetEnvironmentVariableW(L"WARVK_D3D9_STREAM",streamMode,8)) {
      d9::memory::HostWire guard{};guard.fill(0xa7);const auto before=guard;
      auto diagnostic=Raw::Header(d9::Op::DiagnosticCheckpoint);
      diagnostic.args[0]=uint32_t(d9::memory::Phase::Uploaded);diagnostic.args[1]=1;
      Require(r.connection->call(diagnostic,nullptr,guard.data(),uint32_t(guard.size()))==D3DERR_INVALIDCALL
        &&guard==before&&r.connection->healthy(),"read-only diagnostic submitted deferred GPU work");
      ++pendingDiagnosticRefused;
    }
    for(unsigned i=0;i<6;++i)r.destroy(r.refs[i],Types[i]);
    r.createScene(2,0xffdd4422);
    unsigned staleRejected=0;
    for(unsigned slot=0;slot<6;++slot) {
      auto stale=r.draw();stale.resources[slot]=oldRefs[slot];encodeBatch(stale);
      Require(r.connection->call(h,live.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),
        "batch accepted destroyed resource generation");++staleRejected;
      std::array<uint8_t,64*64*4> image{};
      r.call(Raw::Header(d9::Op::Readback),nullptr,image.data(),UINT(image.size()));
      Pixels(evidence,image.data(),64*4,oldColor);
    }
    encodeBatch(r.draw());r.call(h,live.data());
    std::array<uint8_t,64*64*4> newImage{};
    r.call(Raw::Header(d9::Op::Readback),nullptr,newImage.data(),UINT(newImage.size()));
    Pixels(evidence,newImage.data(),64*4,r.color);
    unsigned nonIndexedRejected=0;
    for(unsigned mode=0;mode<4;++mode)for(unsigned position:{0u,15u,31u}) {
      encodeBatch(r.draw());auto invalid=r.draw();invalid.resources[1]={};
      invalid.state.baseVertexIndex=0;invalid.state.minVertexIndex=0;invalid.state.numVertices=3;
      invalid.state.primitiveCount=1;invalid.state.startIndex=0;
      if(mode==0)invalid.state.baseVertexIndex=1;
      if(mode==1)invalid.state.minVertexIndex=1;
      if(mode==2)invalid.state.numVertices=4;
      if(mode==3)invalid.state.startIndex=UINT32_MAX;
      const auto packet=d9::EncodeDraw(invalid);std::memcpy(live.data()+position*d9::DrawBytes,packet.data(),packet.size());
      Require(r.connection->call(h,live.data())==D3DERR_INVALIDCALL&&r.connection->healthy(),"bad nonindexed batch prefix");
      ++nonIndexedRejected;r.call(Raw::Header(d9::Op::Readback),nullptr,newImage.data(),UINT(newImage.size()));
      Pixels(evidence,newImage.data(),64*4,r.color);
    }
    unsigned eventRejected=0,eventRecovered=0;
    const d9::ResourceRef query{20,1};
    r.call(Raw::Header(d9::Op::CreateQuery,query));
    const auto waitHeader=[&](d9::ResourceRef ref,uint64_t revision,uint64_t serial) {
      auto command=Raw::pollHeader(ref,revision,serial);command.op=d9::Op::WaitEvent;return command;
    };
    const auto refuse=[&](d9::Header command) {
      const uint8_t byte=1;
      Require(r.connection->call(command,command.payloadBytes?&byte:nullptr)==D3DERR_INVALIDCALL
        &&r.connection->healthy(),"invalid event wait was not recoverably rejected");++eventRejected;
    };
    refuse(waitHeader(query,1,1)); // No successful Issue exists.
    const auto issued=r.issue(query,1);
    r.call(waitHeader(query,issued.first,issued.second));++eventRecovered;
    const auto repeated=r.issue(query,2);
    for(unsigned mode=0;mode<9;++mode) {
      auto command=waitHeader(query,repeated.first,repeated.second);
      if(mode==0)command.args[0]=1;
      if(mode==1)command.args[0]=0;
      if(mode==2)command.args[2]=command.args[3]=0;
      if(mode==3)++command.args[2];
      if(mode==4){command.id=r.refs[0].id;command.generation=r.refs[0].generation;}
      if(mode==5)command.id=command.generation=0;
      if(mode==6)command.generation=2;
      if(mode==7)command.args[4]=1;
      if(mode==8)command.payloadBytes=1;
      refuse(command);
    }
    r.call(waitHeader(query,repeated.first,repeated.second));++eventRecovered;
    r.destroy(query,d9::Type::EventQuery);refuse(waitHeader(query,2,repeated.second));
    const d9::ResourceRef reused{20,2};r.call(Raw::Header(d9::Op::CreateQuery,reused));
    refuse(waitHeader(query,2,repeated.second));
    const auto replacement=r.issue(reused,1);
    r.call(waitHeader(reused,replacement.first,replacement.second));++eventRecovered;
    r.destroy(reused,d9::Type::EventQuery);
    for(unsigned i=0;i<6;++i)r.destroy(r.refs[i],Types[i]);
    r.call(Raw::Header(d9::Op::Finish));r.connection.reset();Desktop(options);
    std::printf("{\"schema\":\"warvk-d3d9-batch-rejection/v1\",\"ok\":true,\"rejected\":%u,\"recoveries\":%u,"
      "\"noPrefixPixelChecks\":%llu,\"recoveryPixelChecks\":%u,\"pixelFailures\":%u,"
      "\"destroyPendingBatches\":5,\"reusedResourceTypes\":6,\"staleAfterReuseRejected\":%u,"
      "\"eventWaitRejected\":%u,\"eventWaitRecovered\":%u,\"nonIndexedRejected\":%u,"
      "\"pendingDiagnosticRefused\":%u,\"gameTouched\":false}\n",
      rejected,recovered,blackChecks,evidence.pixels,evidence.failures,staleRejected,eventRejected,eventRecovered,
      nonIndexedRejected,pendingDiagnosticRefused);return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"batch rejection: %s\n",e.what());return 1;}
}
