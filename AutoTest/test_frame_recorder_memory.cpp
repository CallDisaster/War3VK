#include "../src/d3d9/war3/tools/war3_frame_recorder_memory.h"
#include "../src/d3d9/war3/tools/war3_frame_evidence.h"
#include "../src/d3d9/war3/tools/war3_frame_inputs_core.h"
#include <windows.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef WARVK_RECORDER_MEMORY_CONTROL_TEST
#include "../src/d3d9/war3/tools/war3_frame_evidence_control.h"
static bool failNextArray=false;
void* operator new[](std::size_t bytes) {
  if(failNextArray){failNextArray=false;throw std::bad_alloc();}
  if(void* p=std::malloc(bytes))return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
// Link-time test seam only. Production always links the Win32 sampler; no
// environment override or function pointer can forge memory in the DLL.
namespace dxvk::war3::tools::evidence {
RecorderMemory injectedMemory;
unsigned queryCount=0;
RecorderMemory QueryRecorderMemory(bool scan) noexcept {
  if(!scan)std::abort();++queryCount;return injectedMemory;
}
}
#endif
using namespace dxvk::war3::tools::evidence;
unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x))throw std::runtime_error(#x);} while(false)
RecorderMemory ample() {return {true,true,2ull<<30,1ull<<30,1ull<<30,512ull<<20};}
int main(){try{
  using R=RecorderMemoryReject;
  auto m=ample();
  CHECK(RecorderMemoryAdmission(m,1)==R::None);
  CHECK(RecorderMemoryAdmission(m,0)==R::None);
  m.valid=false;CHECK(RecorderMemoryAdmission(m,0)==R::Query);
  m=ample();m.totalVirtual=0;CHECK(RecorderMemoryAdmission(m,0)==R::Query);
  m=ample();m.availableVirtual=m.totalVirtual+1;CHECK(RecorderMemoryAdmission(m,0)==R::Query);
  for(uint64_t bytes:{0ull,1ull,4096ull,100ull<<20}){
    m=ample();m.availableVirtual=RecorderProcessHeadroom+bytes;
    CHECK(RecorderMemoryAdmission(m,bytes)==R::None);
    --m.availableVirtual;CHECK(RecorderMemoryAdmission(m,bytes)==R::AddressSpace);
    m=ample();m.availableCommit=RecorderProcessHeadroom+bytes;
    CHECK(RecorderMemoryAdmission(m,bytes)==R::None);
    --m.availableCommit;CHECK(RecorderMemoryAdmission(m,bytes)==R::Commit);
  }
  m=ample();CHECK(RecorderMemoryAdmission(m,UINT64_MAX)==R::AddressSpace);
  m.largestFreeRegion=4096;CHECK(RecorderMemoryAdmission(m,4096,4096)==R::None);
  CHECK(RecorderMemoryAdmission(m,4097,4097)==R::Contiguous);
  m.contiguousKnown=false;CHECK(RecorderMemoryAdmission(m,4096,4096)==R::Contiguous);
  CHECK(RecorderMemoryAdmission(m,4096,0)==R::None);
  CHECK(inputs::HostPayloadBudget==231211008ull); // 220.5 MiB, including metadata
  CHECK(inputs::HostPayloadBudgetForSlots(224)==89915392ull); // 85.75 MiB internal profile
  CHECK(Ring::storageBytes(262144)>=100ull*1024*1024);
  CHECK(Ring::storageBytes(256)==Ring::storageBytes(1)*256);
  // A small ring must not allocate the maximum; ownership survives reject and
  // legal capture/discard/re-arm still works (not a reject-everything test).
  Ring ring;CHECK(ring.arm(1,256));CHECK(ring.append(1,Event{})!=0);
  CHECK(ring.finish(1));CHECK(ring.frozenReady());CHECK(ring.discard(1));
  CHECK(ring.arm(2,256));CHECK(ring.finish(2));CHECK(ring.discard(2));
#ifdef WARVK_RECORDER_MEMORY_CONTROL_TEST
  CHECK(_putenv_s("DXVK_WAR3_FRAME_EVIDENCE","1")==0);
  CHECK(_putenv_s("DXVK_WAR3_FRAME_EVIDENCE_INPUTS","1")==0);
  const auto arm=[] {return Control({{"action","arm"},{"capacity",256}});};
  for(auto why:{R::Query,R::AddressSpace,R::Commit,R::Contiguous}){
    injectedMemory=ample();
    if(why==R::Query)injectedMemory.valid=false;
    if(why==R::AddressSpace)injectedMemory.availableVirtual=RecorderProcessHeadroom;
    if(why==R::Commit)injectedMemory.availableCommit=RecorderProcessHeadroom;
    if(why==R::Contiguous)injectedMemory.largestFreeRegion=1;
    const auto result=arm();CHECK(!result.at("ok").get<bool>());
    CHECK(result.at("memoryReject").get<uint32_t>()==uint32_t(why));
    CHECK(result.at("error").get<std::string>()==RecorderMemoryReason(why));
    CHECK(result.at("requiredPayloadBytes").get<std::string>()==
      std::to_string(Ring::storageBytes(256)+inputs::HostPayloadBudget));
    CHECK(!ActiveSession());
    const auto status=Control({{"action","status"}});
    CHECK(status.at("state").get<uint32_t>()==uint32_t(State::Idle));
    CHECK(status.at("session").get<std::string>()=="0");
  }
  CHECK(queryCount==4);injectedMemory=ample();
  failNextArray=true;const auto failedAllocation=arm();
  CHECK(!failedAllocation.at("ok").get<bool>());CHECK(!failNextArray);
  CHECK(failedAllocation.at("error").get<std::string>()=="recorder-cpu-ring-allocation-failed");
  CHECK(!ActiveSession());
  CHECK(Control({{"action","status"}}).at("session").get<std::string>()=="0");
  const auto first=arm();CHECK(first.at("ok").get<bool>());
  CHECK(first.at("session").get<std::string>()=="1");CHECK(ActiveSession()==1);
  CHECK(!arm().at("ok").get<bool>());CHECK(queryCount==6);
  CHECK(Control({{"action","freeze"},{"session","1"}}).at("ok").get<bool>());
  CHECK(Control({{"action","discard"},{"session","1"}}).at("ok").get<bool>());
  const auto second=arm();CHECK(second.at("ok").get<bool>());
  CHECK(second.at("session").get<std::string>()=="2");CHECK(ActiveSession()==2);
  CHECK(Control({{"action","freeze"},{"session","2"}}).at("ok").get<bool>());
  CHECK(Control({{"action","discard"},{"session","2"}}).at("ok").get<bool>());
  CHECK(queryCount==7);
#else
  SetLastError(0x654321);const auto fast=QueryRecorderMemory(false);
  CHECK(GetLastError()==0x654321);CHECK(fast.valid);CHECK(!fast.contiguousKnown);
  CHECK(fast.totalVirtual>0&&fast.availableVirtual<=fast.totalVirtual);
  SetLastError(0x654322);const auto full=QueryRecorderMemory(true);
  CHECK(GetLastError()==0x654322);CHECK(full.valid&&full.contiguousKnown);
  CHECK(full.largestFreeRegion>0&&full.largestFreeRegion<=full.totalVirtual);
  CHECK(RecorderMemoryAdmission(full,Ring::storageBytes(256),Ring::storageBytes(256)+65536)==R::None);
  std::cout<<"process VA="<<full.totalVirtual<<" free="<<full.availableVirtual
    <<" largest="<<full.largestFreeRegion<<" ringBytes="<<Ring::storageBytes(262144)<<'\n';
#endif
  std::cout<<"recorder memory checks="<<checks<<" PASS\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
