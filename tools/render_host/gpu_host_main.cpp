#include "gpu_lab_common.h"
#include "process_memory_probe.h"
#include "rh2_shaders.h"
#include <memory>
#include <string>
#include <vector>

using namespace warvk::host;
static_assert(sizeof(void*)==8,"Actual x64 renderer required");
int wmain(int argc,wchar_t** argv){
  lab::IoStats io;bool peerVerified=false;std::string failure,protocol="None";
  uint64_t accepted=0;int exitCode=2;
  std::vector<gpu::BackendStats> epochs;lab::ProcessMemory activeMemory;
  const auto beforeMemory=lab::ProbeOwnProcessMemory();
  try{
    gpulab::Require(argc==4,"host expects nonce parentPid parentCreation");
    auto nonce=lab::ParseNonce(argv[1]);auto parentId=gpulab::Number(argv[2]);
    gpulab::Require(parentId&&parentId<=MAXDWORD,"parent PID width");
    auto parent=lab::OpenPeer(DWORD(parentId),gpulab::Number(argv[3]));
    auto pipe=lab::CreateServer(nonce,lab::Channel::Gpu2);lab::Accept(pipe.get(),parent.get(),io);
    lab::VerifyPeer(pipe.get(),true,DWORD(parentId));peerVerified=true;
    gpuwire::Session session(nonce);gpuwire::Packet incoming{},outgoing{};
    std::unique_ptr<gpu::Renderer> renderer;
    for(;;){
      size_t count=0;gpuwire::View v;
      auto e=gpuwire::ReadPacket(pipe.get(),parent.get(),incoming,count,io);
      if(e==gpuwire::Error::None)e=session.receive(incoming.data(),count,v);
      accepted=session.accepted();
      if(e!=gpuwire::Error::None){protocol=gpuwire::ErrorName(e);session.fault();break;}
      auto reply=v.header;reply.flags=1;reply.payloadBytes=0;std::array<uint8_t,96> payload{};
      switch(v.header.op){
        case gpuwire::Op::Hello:{
          auto hello=ipc::EncodeHello({64,32,gpuwire::Capability,gpuwire::Capability});
          std::memcpy(payload.data(),hello.data(),hello.size());reply.payloadBytes=24;break;
        }
        case gpuwire::Op::Begin:{
          gpulab::Require(!renderer,"previous epoch renderer survived");
          renderer=std::make_unique<gpu::Renderer>(uint32_t(ipc::ReadLe(v.payload,4)),uint32_t(ipc::ReadLe(v.payload+4,4)),
            gpu::ShaderWords{rh2_mesh_vert,sizeof(rh2_mesh_vert)/4},gpu::ShaderWords{rh2_mesh_frag,sizeof(rh2_mesh_frag)/4});
          const auto descriptor=gpulab::EncodeImage(renderer->exportImage());
          std::memcpy(payload.data(),descriptor.data(),descriptor.size());reply.payloadBytes=uint32_t(descriptor.size());
          activeMemory=lab::ProbeOwnProcessMemory();break;
        }
        case gpuwire::Op::Render:
          gpulab::Require(bool(renderer),"no renderer");renderer->render(uint32_t(ipc::ReadLe(v.payload,4)));
          gpulab::Require(session.submitted(v.header.frame)==gpuwire::Error::None,"submit transition");break;
        case gpuwire::Op::Release:
          gpulab::Require(bool(renderer),"no renderer");renderer->settleLastConsumer();
          gpulab::Require(session.consumerFinished(v.header.frame)==gpuwire::Error::None,"GPU completion transition");break;
        case gpuwire::Op::End:
          gpulab::Require(bool(renderer),"no renderer");epochs.push_back(renderer->stats());renderer.reset();break;
        case gpuwire::Op::Close:break;
      }
      size_t bytes=0;gpulab::Require(gpuwire::Encode(reply,payload.data(),outgoing.data(),outgoing.size(),bytes)==gpuwire::Error::None,"reply encoding");
      lab::WriteBytes(pipe.get(),parent.get(),outgoing.data(),bytes,io);
      if(v.header.op==gpuwire::Op::Close){
        gpulab::Require(session.closeSent(v.header.sequence)==gpuwire::Error::None,"close I/O");exitCode=0;break;
      }
    }
    if(renderer)epochs.push_back(renderer->stats());
  }catch(const lab::Failure& e){failure=std::string(lab::FaultName(e.fault))+"."+lab::StageName(e.stage)+" Win32="+std::to_string(e.win32);}
  catch(const std::exception& e){failure=e.what();}
  std::printf("{\"bits\":64,\"peerVerified\":%s,\"closed\":%s,\"accepted\":%llu,\"protocol\":",
    peerVerified?"true":"false",exitCode==0?"true":"false",(unsigned long long)accepted);
  gpulab::JsonText(protocol.c_str());std::printf(",\"failure\":");gpulab::JsonText(failure.c_str());
  std::printf(",\"epochs\":[");for(size_t i=0;i<epochs.size();++i){if(i)std::putchar(',');gpulab::PrintStats(epochs[i]);}
  std::printf("],\"memoryBefore\":");lab::PrintProcessMemory(beforeMemory);
  std::printf(",\"memoryActive\":");lab::PrintProcessMemory(activeMemory);
  std::printf(",\"memoryAfter\":");lab::PrintProcessMemory(lab::ProbeOwnProcessMemory());
  std::printf(",\"validation\":");gpulab::PrintValidation();
  std::printf("}\n");return exitCode;
}
