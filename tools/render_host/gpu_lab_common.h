#pragma once
#include "gpu_render_backend.h"
#include "gpu_channel.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace warvk::host::gpulab {
inline void Require(bool b,const char* reason){if(!b)throw std::runtime_error(reason);}
inline uint64_t Number(const wchar_t* p){
  Require(p&&*p,"empty number");uint64_t n=0;
  for(;*p;++p){Require(*p>=L'0'&&*p<=L'9'&&n<=(UINT64_MAX-uint64_t(*p-L'0'))/10,"invalid number");n=n*10+uint64_t(*p-L'0');}
  return n;
}
inline std::array<uint8_t,80> EncodeImage(const gpu::ExportedImage& x){
  std::array<uint8_t,80> p{};
  ipc::WriteLe(p.data(),x.width,4);ipc::WriteLe(p.data()+4,x.height,4);
  ipc::WriteLe(p.data()+8,x.format,4);ipc::WriteLe(p.data()+12,x.memoryTypeIndex,4);
  ipc::WriteLe(p.data()+16,x.allocationBytes,8);ipc::WriteLe(p.data()+24,x.imageMemoryHandle,8);
  ipc::WriteLe(p.data()+32,x.readySemaphoreHandle,8);ipc::WriteLe(p.data()+40,x.consumedSemaphoreHandle,8);
  std::memcpy(p.data()+48,x.deviceUUID.data(),16);std::memcpy(p.data()+64,x.driverUUID.data(),16);return p;
}
inline gpu::ExportedImage DecodeImage(const uint8_t* p,size_t n){
  Require(n==80,"image descriptor length");gpu::ExportedImage x;
  x.width=uint32_t(ipc::ReadLe(p,4));x.height=uint32_t(ipc::ReadLe(p+4,4));
  x.format=uint32_t(ipc::ReadLe(p+8,4));x.memoryTypeIndex=uint32_t(ipc::ReadLe(p+12,4));
  x.allocationBytes=ipc::ReadLe(p+16,8);x.imageMemoryHandle=ipc::ReadLe(p+24,8);
  x.readySemaphoreHandle=ipc::ReadLe(p+32,8);x.consumedSemaphoreHandle=ipc::ReadLe(p+40,8);
  std::memcpy(x.deviceUUID.data(),p+48,16);std::memcpy(x.driverUUID.data(),p+64,16);
  Require(x.width>=16&&x.width<=256&&x.height>=16&&x.height<=256&&x.memoryTypeIndex<32&&
    x.allocationBytes&&x.allocationBytes<=16*1024*1024,"descriptor limits");return x;
}
inline void JsonText(const char* value){
  std::putchar('"');for(;*value;++value){auto c=static_cast<unsigned char>(*value);
    if(c=='"'||c=='\\'){std::putchar('\\');std::putchar(c);}
    else if(c<32)std::printf("\\u%04x",c);else std::putchar(c);
  }std::putchar('"');
}
inline void PrintStats(const gpu::BackendStats& s){
  std::printf("{\"device\":");JsonText(s.deviceName.c_str());
  std::printf(",\"deviceUUID\":\"");for(auto x:s.deviceUUID)std::printf("%02x",x);
  std::printf("\",\"driverUUID\":\"");for(auto x:s.driverUUID)std::printf("%02x",x);
  std::printf("\",\"ownedDeviceBytes\":%llu,\"sharedImageBytes\":%llu,\"geometryBytes\":%llu,"
    "\"geometryPayloadBytes\":%llu,\"readbackBytes\":%llu,\"uploadStagingPeakBytes\":%llu,"
    "\"drawCount\":%llu,\"submitCount\":%llu,\"fenceWaitCount\":%llu,\"consumerWaitCount\":%llu,"
    "\"readbackCount\":%llu,\"imageExternalFeatures\":%u,\"semaphoreExternalFeatures\":%u,\"faulted\":%s}",
    (unsigned long long)s.ownedDeviceBytes,(unsigned long long)s.sharedImageBytes,
    (unsigned long long)s.geometryBytes,(unsigned long long)s.geometryPayloadBytes,
    (unsigned long long)s.readbackBytes,(unsigned long long)s.uploadStagingPeakBytes,
    (unsigned long long)s.drawCount,(unsigned long long)s.submitCount,(unsigned long long)s.fenceWaitCount,
    (unsigned long long)s.consumerWaitCount,(unsigned long long)s.readbackCount,
    s.imageExternalFeatures,s.semaphoreExternalFeatures,s.faulted?"true":"false");
}
inline void PrintValidation(){
  const auto v=gpu::processValidationTotals();
  std::printf("{\"enabled\":%s,\"instances\":%llu,\"errors\":%llu,\"warnings\":%llu}",
    v.instancesEnabled?"true":"false",(unsigned long long)v.instancesEnabled,
    (unsigned long long)v.errors,(unsigned long long)v.warnings);
}
inline lab::Handle DuplicateExport(HANDLE child,uint64_t value){
  // NT handles, not VkImage/VkMemory integers. Windows interop uses low 32 bits.
  Require(value&&value<=UINT32_MAX&&value!=UINT32_MAX,"invalid exported NT handle");
  HANDLE local=nullptr;
  Require(DuplicateHandle(child,reinterpret_cast<HANDLE>(uintptr_t(value)),GetCurrentProcess(),
    &local,0,FALSE,DUPLICATE_SAME_ACCESS)!=0,"DuplicateHandle from pinned child failed");
  return lab::Handle(local);
}
} // namespace warvk::host::gpulab
