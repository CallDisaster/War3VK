#include "../tools/render_host/gpu_lab_common.h"
#include "../tools/render_host/owned_child.h"
#include "../tools/render_host/process_memory_probe.h"
#include <cwchar>
#include <vector>

using namespace warvk::host;
static_assert(sizeof(void*)==4,"Actual x86 client required");
namespace {
struct Client {
  HANDLE pipe,peer;ipc::Nonce nonce;lab::IoStats io;
  gpuwire::Header h;gpuwire::Packet response{};unsigned& exchanges;
  Client(HANDLE p,HANDLE c,ipc::Nonce n,unsigned& completed):pipe(p),peer(c),nonce(n),exchanges(completed){h.nonce=n;}
  void send(const uint8_t* payload){gpuwire::Packet packet{};size_t count=0;
    gpulab::Require(gpuwire::Encode(h,payload,packet.data(),packet.size(),count)==gpuwire::Error::None,"encode request");
    lab::WriteBytes(pipe,peer,packet.data(),count,io);
  }
  gpuwire::View exchange(const uint8_t* payload=nullptr){
    send(payload);size_t count=0;gpuwire::View v;
    gpulab::Require(gpuwire::ReadPacket(pipe,peer,response,count,io)==gpuwire::Error::None&&
      gpuwire::Decode(response.data(),count,v)==gpuwire::Error::None,"decode reply");
    const auto& r=v.header;
    gpulab::Require(r.flags==1&&r.op==h.op&&r.nonce==nonce&&r.sequence==h.sequence&&
      r.mapEpoch==h.mapEpoch&&r.deviceEpoch==h.deviceEpoch&&r.resourceId==h.resourceId&&
      r.generation==h.generation&&r.frame==h.frame,"reply identity");
    if(h.op!=gpuwire::Op::Hello&&h.op!=gpuwire::Op::Begin)gpulab::Require(r.payloadBytes==0,"unexpected reply body");
    ++h.sequence;++exchanges;return v;
  }
  void hello(){h.op=gpuwire::Op::Hello;h.payloadBytes=24;
    auto p=ipc::EncodeHello({32,64,gpuwire::Capability,gpuwire::Capability});auto v=exchange(p.data());
    auto expected=ipc::EncodeHello({64,32,gpuwire::Capability,gpuwire::Capability});
    gpulab::Require(v.header.payloadBytes==expected.size()&&!std::memcmp(v.payload,expected.data(),expected.size()),"hello response capability");
  }
};
struct FrameProof {unsigned epoch,frame,variant;uint64_t hash;std::array<uint8_t,4> center,corner;};
// Observe the exact child BEFORE the OwnedChild exception-unwind closes its
// job. A default STILL_ACTIVE or the cleanup exit code is not a crash diagnosis.
struct ChildExitProbe {
  HANDLE process;DWORD& exit;bool& observed;
  ~ChildExitProbe() noexcept { DWORD value=0;observed=GetExitCodeProcess(process,&value)!=FALSE;if(observed)exit=value; }
};
FrameProof VerifyPixels(const std::vector<uint8_t>& bytes,unsigned width,unsigned height,unsigned epoch,unsigned frame,unsigned variant){
  gpulab::Require(bytes.size()==size_t(width)*height*4,"readback length");
  FrameProof p{epoch,frame,variant,14695981039346656037ull,{},{}};
  for(auto b:bytes){p.hash^=b;p.hash*=1099511628211ull;}
  std::memcpy(p.center.data(),bytes.data()+((height/2)*width+width/2)*4,4);
  std::memcpy(p.corner.data(),bytes.data(),4);
  constexpr int centers[4][4]={{255,32,16,255},{16,255,32,255},{32,16,255,255},{255,255,16,255}};
  constexpr int corner[4]={4,8,16,255};
  for(unsigned c=0;c<4;++c){
    gpulab::Require(std::abs(int(p.center[c])-centers[variant][c])<=1,"indexed geometry center pixel mismatch");
    gpulab::Require(std::abs(int(p.corner[c])-corner[c])<=1,"clear corner pixel mismatch");
  }
  return p;
}
}
int wmain(int argc,wchar_t** argv){
  std::string failure;DWORD pid=0,childExit=STILL_ACTIVE;uint64_t creation=0;bool verified=false;
  std::vector<FrameProof> frames;std::vector<gpu::BackendStats> imports;lab::ProcessMemory activeMemory;
  const auto beforeMemory=lab::ProbeOwnProcessMemory();std::string mode;
  unsigned exchanges=0;int code=1;bool adapterMismatchRejected=false,childExitObservedBeforeCleanup=false;
  try{
    gpulab::Require(argc==4,"client expects host case create-new-log");
    const std::wstring wideMode=argv[2];
    for(const char* p:{"normal","bad-generation","duplicate-frame","bad-nonce","wrong-adapter"})
      if(wideMode==std::wstring(p,p+std::strlen(p)))mode=p;
    gpulab::Require(!mode.empty(),"unknown case");
    const auto nonce=lab::RandomNonce();
    auto args=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(lab::CreationTime(GetCurrentProcess()));
    lab::OwnedChild child(argv[1],args,argv[3]);pid=child.pid();creation=child.creation();
    ChildExitProbe childProbe{child.process(),childExit,childExitObservedBeforeCleanup};
    try {
    auto pipe=lab::Connect(nonce,child.process(),child.pid(),lab::Channel::Gpu2);verified=true;
    Client c(pipe.get(),child.process(),nonce,exchanges);c.hello();
    for(unsigned epoch=1;epoch<=2;++epoch){
      unsigned width=epoch==1?64:96,height=epoch==1?64:80;
      c.h.op=gpuwire::Op::Begin;c.h.mapEpoch=epoch;c.h.deviceEpoch=epoch;c.h.resourceId=1;c.h.generation=epoch;c.h.frame=0;c.h.payloadBytes=8;
      uint8_t dimensions[8];ipc::WriteLe(dimensions,width,4);ipc::WriteLe(dimensions+4,height,4);
      auto descriptor=c.exchange(dimensions);auto image=gpulab::DecodeImage(descriptor.payload,descriptor.header.payloadBytes);
      gpulab::Require(image.width==width&&image.height==height,"returned dimensions");
      auto memory=gpulab::DuplicateExport(child.process(),image.imageMemoryHandle);
      auto ready=gpulab::DuplicateExport(child.process(),image.readySemaphoreHandle);
      auto consumed=gpulab::DuplicateExport(child.process(),image.consumedSemaphoreHandle);
      image.imageMemoryHandle=uint64_t(uintptr_t(memory.get()));image.readySemaphoreHandle=uint64_t(uintptr_t(ready.get()));
      image.consumedSemaphoreHandle=uint64_t(uintptr_t(consumed.get()));
      if(mode=="wrong-adapter"){
        image.deviceUUID[0]^=0xff;bool rejected=false;
        try{gpu::Importer incompatible(image);}catch(const gpu::AdapterMismatch&){rejected=true;}
        gpulab::Require(rejected,"wrong adapter accepted");
        adapterMismatchRejected=true;
      }else{
        gpu::Importer importer(image);memory.reset();ready.reset();consumed.reset();activeMemory=lab::ProbeOwnProcessMemory();
        for(unsigned frame=1;frame<=8;++frame){
          unsigned variant=(frame-1)%4;uint8_t payload[4];ipc::WriteLe(payload,variant,4);
          c.h.op=gpuwire::Op::Render;c.h.frame=frame;c.h.payloadBytes=4;
          if(frame==2&&mode!="normal"){
            if(mode=="bad-generation")++c.h.generation;
            if(mode=="duplicate-frame")c.h.frame=1;
            if(mode=="bad-nonce")c.h.nonce.high^=1;
            c.send(payload);childExit=child.wait(7000);gpulab::Require(childExit==2,"bad command did not terminate host");
            break;
          }
          c.exchange(payload);frames.push_back(VerifyPixels(importer.readback(),width,height,epoch,frame,variant));
          c.h.op=gpuwire::Op::Release;c.h.payloadBytes=0;c.exchange();
        }
        imports.push_back(importer.stats());
      }
      if(childExit==2)break;
      // Importer destroyed before the exporting owner may retire this epoch.
      memory.reset();ready.reset();consumed.reset();
      c.h.op=gpuwire::Op::End;c.h.frame=0;c.h.payloadBytes=0;c.exchange();
      if(mode=="wrong-adapter")break;
    }
    if(childExit!=2){
      c.h.op=gpuwire::Op::Close;c.h.payloadBytes=0;c.h.mapEpoch=c.h.deviceEpoch=c.h.resourceId=c.h.generation=c.h.frame=0;
      c.exchange();childExit=child.wait(7000);gpulab::Require(childExit==0,"normal child exit");
    }
    exchanges=c.exchanges;code=0;
    } catch (...) {
      // The peer can close its pipe during exception unwinding, BEFORE its
      // final diagnostic JSON/exit is published. Give this exact lab child a
      // bounded chance to report; immediate Job destruction would erase the
      // original failure. A still-running child is then contained by its owner.
      // This is fault-only lab settlement, never a game/render-thread wait.
      (void)WaitForSingleObject(child.process(),5000);
      throw;
    }
  }catch(const lab::Failure& e){failure=std::string(lab::FaultName(e.fault))+"."+lab::StageName(e.stage)+" Win32="+std::to_string(e.win32);}
  catch(const std::exception& e){failure=e.what();}
  std::printf("{\"bits\":32,\"case\":");gpulab::JsonText(mode.c_str());
  std::printf(",\"peerVerified\":%s,\"childPid\":%lu,\"childCreation\":%llu,\"childExit\":%lu,\"exchanges\":%u,\"failure\":",
    verified?"true":"false",(unsigned long)pid,(unsigned long long)creation,(unsigned long)childExit,exchanges);
  gpulab::JsonText(failure.c_str());std::printf(",\"frames\":[");
  for(size_t i=0;i<frames.size();++i){const auto& p=frames[i];
    std::printf("%s{\"epoch\":%u,\"frame\":%u,\"variant\":%u,\"fnv64\":\"%016llx\",\"center\":[%u,%u,%u,%u],\"corner\":[%u,%u,%u,%u]}",
      i?",":"",p.epoch,p.frame,p.variant,(unsigned long long)p.hash,p.center[0],p.center[1],p.center[2],p.center[3],p.corner[0],p.corner[1],p.corner[2],p.corner[3]);
  }
  std::printf("],\"imports\":[");for(size_t i=0;i<imports.size();++i){if(i)std::putchar(',');gpulab::PrintStats(imports[i]);}
  std::printf("],\"memoryBefore\":");lab::PrintProcessMemory(beforeMemory);
  std::printf(",\"memoryActive\":");lab::PrintProcessMemory(activeMemory);
  std::printf(",\"memoryAfter\":");lab::PrintProcessMemory(lab::ProbeOwnProcessMemory());
  std::printf(",\"childExitObservedBeforeCleanup\":%s,\"adapterMismatchRejected\":%s,\"validation\":",
    childExitObservedBeforeCleanup?"true":"false",adapterMismatchRejected?"true":"false");
  gpulab::PrintValidation();std::printf("}\n");return code;
}
