// M0 uses the SAME real COM quad/resource/Query workload as R1, with eight
// explicit cold observations. It never substitutes byte counters for pixels,
// remote resource retirement, or the child's actual bounded process exit.
#define wmain WarVKUnusedLifetimeMain
#include "test_render_host_d3d9_resource_lifetime.cpp"
#undef wmain
#include "../tools/render_host/d3d9_memory_probe.h"
#include <limits>

namespace mem = warvk::host::d9::memory;
namespace lab = warvk::host::lab;
namespace {
unsigned long long U64(uint64_t value) { return static_cast<unsigned long long>(value); }
struct ShaderShape { uint32_t bytes=0, floats=0, integers=0, booleans=0; };
ShaderShape CountOriginalDefinitions(const std::vector<DWORD>& words) {
  // Independent fixture walk of actual compiler bytes, not the host's scanner
  // or returned allocation account. SM3 length excludes the opcode word.
  Require(words.size()>2 && (words[0]&0xffffu)==0x0300u, "original SM3 version");
  ShaderShape shape;shape.bytes=uint32_t(words.size()*sizeof(DWORD));
  size_t at=1;
  while(at<words.size()) {
    const DWORD instruction=words[at++];const auto opcode=instruction&0xffffu;
    if(opcode==D3DSIO_END) {
      Require(instruction==DWORD(D3DSIO_END)&&at==words.size(), "original shader exact END");
      return shape;
    }
    const size_t count=opcode==D3DSIO_COMMENT?(instruction>>16)&0x7fffu:(instruction>>24)&15u;
    Require(count<=words.size()-at, "original shader instruction bound");
    if(opcode==D3DSIO_DEF){Require(count==5, "float DEF shape");++shape.floats;}
    if(opcode==D3DSIO_DEFI){Require(count==5, "integer DEF shape");++shape.integers;}
    if(opcode==D3DSIO_DEFB){Require(count==2, "boolean DEF shape");++shape.booleans;}
    at+=count;
  }
  throw std::runtime_error("original shader missing END");
}
uint32_t CpuHelpers() {
  uint32_t checks=0;
  const auto expect=[&](bool value){++checks;Require(value,"M0 CPU account/codec adversarial assertion");};
  const auto kind=mem::Kind::IndexBuffer;const auto index=size_t(kind);
  mem::CpuAccount account;account.add(kind,12,12,16);
  expect(account.valid&&account.kinds[index].objects==1&&account.kinds[index].logicalBytes==12
    &&account.kinds[index].cpuSizeBytes==12&&account.kinds[index].cpuCapacityBytes==16);
  account.remove(kind,12,12,16);
  expect(account.valid&&!account.kinds[index].objects&&!account.kinds[index].logicalBytes
    &&!account.kinds[index].cpuSizeBytes&&!account.kinds[index].cpuCapacityBytes);
  account={};account.add(kind,12,17,16);expect(!account.valid&&!account.kinds[index].objects);
  account={};account.add(mem::Kind::Count,0,0,0);expect(!account.valid);
  account={};account.kinds[index].logicalBytes=std::numeric_limits<uint64_t>::max();
  account.add(kind,1,0,0);expect(!account.valid&&!account.kinds[index].objects);
  account={};account.kinds[index].cpuCapacityBytes=std::numeric_limits<uint64_t>::max();
  account.add(kind,0,0,1);expect(!account.valid&&!account.kinds[index].objects);
  account={};account.kinds[index].objects=std::numeric_limits<uint64_t>::max();
  account.add(kind,0,0,0);expect(!account.valid);
  account={};account.remove(kind,0,0,0);expect(!account.valid);
  account={};account.add(kind,2,1,1);account.remove(kind,3,1,1);expect(!account.valid&&account.kinds[index].objects==1);
  account={};account.add(kind,2,1,2);account.remove(kind,2,2,2);expect(!account.valid&&account.kinds[index].objects==1);
  account={};account.add(kind,2,1,1);account.remove(kind,2,1,2);expect(!account.valid&&account.kinds[index].objects==1);
  mem::HostSample original;original.phase=1;original.pid=42;original.thread=7;original.creation=123;
  original.nonceLow=4;original.sequence=3;original.tickBegin=100;original.tickEnd=101;
  original.submitted=original.completed=original.retired=1;
  original.process.win32=ERROR_PARTIAL_COPY; // Deliberately INVALID synthetic OS sample.
  original.cpu.add(kind,12,12,16);
  const auto encoded=mem::EncodeHost(original);mem::HostSample decoded;
  expect(mem::DecodeHost(encoded,decoded)&&mem::EncodeHost(decoded)==encoded);
  expect(!decoded.process.valid&&decoded.process.win32==ERROR_PARTIAL_COPY);
  const auto bad32=[&](size_t offset,uint32_t value){auto bad=encoded;d9::P32(bad.data()+offset,value);
    expect(!mem::DecodeHost(bad,decoded));};
  const auto bad64=[&](size_t offset,uint64_t value){auto bad=encoded;d9::P64(bad.data()+offset,value);
    expect(!mem::DecodeHost(bad,decoded));};
  bad32(0,0);bad32(4,2);bad32(480,1);bad32(220,1);bad32(216,2);bad32(152,2);
  bad64(224+32+16,17); // IB CPU size > its independently retained capacity (16).
  bad32(16,0);bad32(24,32);bad64(104,2);bad64(72,99);bad32(8,0);bad64(56,0);bad64(40,0);
  bad32(152,1); // A valid OS bit cannot coexist with ERROR_PARTIAL_COPY.
  bad32(12,1);bad32(8,uint32_t(mem::Phase::Uploaded));
  // Actual vector storage, through the same helper used for EVERY host slot.
  // clear() is the faulty retirement: size zero does not release capacity.
  // This is CPU production-helper coverage, not an executed GPU destroy fault.
  const std::array<mem::Kind,4> retainedKinds{{mem::Kind::IndexBuffer,mem::Kind::VertexShader,
    mem::Kind::PixelShader,mem::Kind::PresentedReadback}};
  const std::array<size_t,4> retainedSizes{{12,20,20,Size*Size*4}};
  for(size_t n=0;n<retainedKinds.size();++n) {
    const auto observedKind=retainedKinds[n];const auto observedIndex=size_t(observedKind);
    std::vector<uint8_t> payload;
    const auto observe=[&](mem::CpuAccount& target,bool live) {
      target.observe(observedKind,live,live?payload.size():0,payload.size(),payload.capacity());
    };
    mem::CpuAccount empty;observe(empty,false);
    expect(empty.valid&&!empty.kinds[observedIndex].objects&&!empty.kinds[observedIndex].cpuCapacityBytes);
    payload.resize(retainedSizes[n]);const uint64_t retained=payload.capacity();
    mem::CpuAccount live;observe(live,true);
    expect(live.valid&&live.kinds[observedIndex].objects==1
      &&live.kinds[observedIndex].cpuSizeBytes==retainedSizes[n]&&live.kinds[observedIndex].cpuCapacityBytes==retained);
    payload.clear();mem::CpuAccount clearOnly;observe(clearOnly,false);
    expect(!clearOnly.valid&&!clearOnly.kinds[observedIndex].objects&&!clearOnly.kinds[observedIndex].logicalBytes
      &&!clearOnly.kinds[observedIndex].cpuSizeBytes&&clearOnly.kinds[observedIndex].cpuCapacityBytes==retained&&retained>0);
    payload.resize(retainedSizes[n]);mem::CpuAccount stillSized;observe(stillSized,false);
    expect(!stillSized.valid&&!stillSized.kinds[observedIndex].objects
      &&stillSized.kinds[observedIndex].cpuSizeBytes==retainedSizes[n]&&stillSized.kinds[observedIndex].cpuCapacityBytes==retained);
    mem::CpuAccount mixed;observe(mixed,true);
    std::vector<uint8_t> deadPeer(payload);const uint64_t peerCapacity=deadPeer.capacity();deadPeer.clear();
    mixed.observe(observedKind,false,0,deadPeer.size(),deadPeer.capacity());
    expect(!mixed.valid&&mixed.kinds[observedIndex].objects==1&&mixed.kinds[observedIndex].cpuSizeBytes==retainedSizes[n]
      &&mixed.kinds[observedIndex].cpuCapacityBytes==retained+peerCapacity);
    payload=std::vector<uint8_t>{};mem::CpuAccount reset;observe(reset,false);
    expect(reset.valid&&!reset.kinds[observedIndex].objects&&!reset.kinds[observedIndex].cpuSizeBytes
      &&!reset.kinds[observedIndex].cpuCapacityBytes);
  }
  return checks;
}
void ProcessJson(const lab::ProcessMemory& value) {
  // PeakPagefileUsage is peak process commit, NOT an observed private-vector
  // maximum. Do not reuse the historical peakPrivateBytes JSON spelling.
  std::printf("{\"valid\":%s,\"win32\":%lu,\"privateBytes\":%llu,\"peakCommitBytes\":%llu,"
    "\"workingSetBytes\":%llu,\"usedVirtualBytes\":%llu,\"privateVirtualBytes\":%llu,"
    "\"mappedVirtualBytes\":%llu,\"regions\":%llu}",Bool(value.valid),DWORD(value.win32),
    U64(value.privateBytes),U64(value.peakPrivateBytes),U64(value.workingSetBytes),U64(value.usedVirtualBytes),
    U64(value.privateVirtualBytes),U64(value.mappedVirtualBytes),U64(value.regions));
}
void CpuJson(const mem::CpuAccount& value) {
  std::printf("{\"valid\":%s,\"kinds\":{",Bool(value.valid));
  for(size_t i=0;i<mem::KindCount;++i) {
    const auto& kind=value.kinds[i];
    std::printf("%s\"%s\":{\"objects\":%llu,\"logicalBytes\":%llu,\"cpuSizeBytes\":%llu,\"cpuCapacityBytes\":%llu}",
      i?",":"",mem::KindNames[i],U64(kind.objects),U64(kind.logicalBytes),U64(kind.cpuSizeBytes),U64(kind.cpuCapacityBytes));
  }
  std::printf("}}");
}
void PrintCheckpoint(const mem::Checkpoint& s) {
  std::printf("{\"schema\":\"warvk-d3d9-memory-checkpoint/v1\",\"phase\":%u,\"ordinal\":%u,\"parent\":{"
    "\"version\":%u,\"phase\":%u,\"ordinal\":%u,\"pid\":%u,\"thread\":%u,\"bits\":%u,"
    "\"logicalHandles\":%u,\"reservedHandles\":%u,\"privateBindings\":%u,\"publicZeroBindings\":%u,"
    "\"expectedHostPid\":%u,\"creation\":%llu,\"tickBegin\":%llu,\"beforeEnd\":%llu,\"afterBegin\":%llu,\"tickEnd\":%llu,"
    "\"expectedHostCreation\":%llu,\"expectedNonceLow\":%llu,\"expectedNonceHigh\":%llu,\"expectedSequence\":%llu,"
    "\"connectionInlineBytes\":%llu,\"fixedPacketPayloadBytes\":%llu,\"workerBatchPayloadBytes\":%llu,\"before\":",
    s.phase,s.ordinal,s.version,s.phase,s.ordinal,s.pid,s.thread,s.bits,s.logicalHandles,s.reservedHandles,
    s.privateBindings,s.publicZeroBindings,s.expectedHostPid,U64(s.creation),U64(s.tickBegin),U64(s.beforeEnd),
    U64(s.afterBegin),U64(s.tickEnd),U64(s.expectedHostCreation),U64(s.expectedNonceLow),U64(s.expectedNonceHigh),
    U64(s.expectedSequence),U64(s.connectionInlineBytes),U64(s.fixedPacketPayloadBytes),U64(s.workerBatchPayloadBytes));
  ProcessJson(s.before);std::printf(",\"after\":");ProcessJson(s.after);std::printf(",\"cpu\":");CpuJson(s.cpu);
  std::printf("},\"hostWireHex\":\"");const auto wire=mem::EncodeHost(s.host);
  for(const auto byte:wire)std::printf("%02x",unsigned(byte));
  std::printf("\",\"synchronizedInstant\":false,\"gpuPhysicalMeasured\":false,\"opaqueCpuMeasured\":false,"
    "\"gameTouched\":false,\"productIntegrated\":false,\"memorySavingsProven\":false}\n");std::fflush(stdout);
}
bool SameAccount(const mem::CpuAccount& a,const mem::CpuAccount& b) {
  if(a.valid!=b.valid)return false;
  for(size_t i=0;i<mem::KindCount;++i) {
    const auto& x=a.kinds[i];const auto& y=b.kinds[i];
    if(x.objects!=y.objects||x.logicalBytes!=y.logicalBytes||x.cpuSizeBytes!=y.cpuSizeBytes
      ||x.cpuCapacityBytes!=y.cpuCapacityBytes)return false;
  }
  return true;
}
void ValidProcess(const lab::ProcessMemory& s) {
  Require(s.valid&&s.win32==0&&s.privateBytes&&s.workingSetBytes&&s.usedVirtualBytes&&s.regions,
    "required actual process OS sample is missing/partial");
}
using SampleFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,mem::Checkpoint*,UINT);
struct MemoryEvidence {
  std::array<mem::Checkpoint,8> samples{};uint32_t count=0;
  ShaderShape vertex,pixel;
  void capture(SampleFn sample,IDirect3DDevice9* device,mem::Phase phase,uint32_t ordinal) {
    Require(count<samples.size(),"bounded checkpoint count");auto& s=samples[count];
    Check(sample(device,uint32_t(phase),ordinal,&s,sizeof(s)),"actual memory checkpoint");
    const auto& h=s.host;
    constexpr std::array<uint32_t,8> phases{{1,2,3,4,4,4,4,5}},ordinals{{0,1,1,1,16,48,95,96}};
    // 20 calls per unbound round: 6 creates, 2 buffer uploads, a 3-call texture
    // Begin/Chunk/Commit transaction, clear/draw/readback, 6 deferred destroys;
    // the first draw also publishes the unchanged sampler state once.
    constexpr std::array<uint64_t,8> sequences{{3,15,20,27,328,969,1910,2437}};
    constexpr std::array<uint64_t,8> serials{{1,5,8,8,113,337,666,673}};
    constexpr std::array<uint64_t,8> draws{{0,0,1,1,16,48,95,96}},uploads{{0,3,3,3,48,144,285,288}};
    const bool active=count==1||count==2||count==7;const bool privateBound=count==2||count==7;
    Require(s.version==1&&s.bits==32&&s.pid==GetCurrentProcessId()&&s.thread==GetCurrentThreadId()
      &&s.creation==lab::CreationTime(GetCurrentProcess()),"actual parent identity");
    Require(s.phase==phases[count]&&s.ordinal==ordinals[count]&&h.phase==s.phase&&h.ordinal==s.ordinal
      &&h.bits==64&&h.pid!=s.pid&&h.pid==s.expectedHostPid&&h.creation==s.expectedHostCreation
      &&h.nonceLow==s.expectedNonceLow&&h.nonceHigh==s.expectedNonceHigh
      &&h.sequence==s.expectedSequence&&h.sequence==sequences[count],"checkpoint phase and independently bound peer identity");
    Require(s.tickBegin<=s.beforeEnd&&s.beforeEnd<=h.tickBegin&&h.tickBegin<=h.tickEnd
      &&h.tickEnd<=s.afterBegin&&s.afterBegin<=s.tickEnd,"ordered paired process windows");
    ValidProcess(s.before);ValidProcess(h.process);ValidProcess(s.after);
    Require(s.logicalHandles==(active?7u:1u)&&s.reservedHandles==0
      &&s.privateBindings==(privateBound?6u:0u)&&s.publicZeroBindings==s.privateBindings,
      "COM registry/private-public lifetime population");
    Require(h.logicalResources==(active?6u:0u)
      &&h.logicalBytes==(active?uint64_t(160+12+4+24)+vertex.bytes+pixel.bytes:0)
      &&h.submitted==serials[count]&&h.completed==serials[count]&&h.retired==serials[count]
      &&h.draws==draws[count]&&h.readbacks==draws[count]&&h.uploads==uploads[count],"actual GPU/resource milestones unchanged");
    Require(s.connectionInlineBytes==sizeof(proxy::Connection)
      &&s.fixedPacketPayloadBytes==uint64_t(warvk::host::commands::Capacity)*warvk::host::commands::MaxBatchBytes+2ull*sizeof(d9::Packet)
      &&s.fixedPacketPayloadBytes<=s.connectionInlineBytes
      &&s.workerBatchPayloadBytes==warvk::host::commands::MaxBatchBytes
      &&h.fixedPacketPayloadBytes==2ull*sizeof(d9::Packet)&&h.ownerInlineBytes>0,"declared fixed transport storage");
    Require(s.cpu.valid&&h.cpu.valid,"owned CPU account valid");
    const std::array<uint64_t,mem::KindCount> logical{{160,12,4,Size*Size*4,vertex.bytes,pixel.bytes,32,0}};
    for(size_t i=0;i<mem::KindCount;++i) {
      const auto kind=mem::Kind(i);const bool author=i<3||kind==mem::Kind::VertexShader||kind==mem::Kind::PixelShader||kind==mem::Kind::Declaration;
      const bool localLive=kind==mem::Kind::SystemSurface||(active&&author);
      const auto& local=s.cpu.kinds[i];const auto& remote=h.cpu.kinds[i];
      Require(local.objects==(localLive?1u:0u)&&local.logicalBytes==(localLive?logical[i]:0)
        &&local.cpuSizeBytes==local.logicalBytes&&local.cpuCapacityBytes>=local.cpuSizeBytes,"proxy actual vector size/logical population");
      if(!localLive)Require(local.cpuCapacityBytes==0,"inactive proxy payload released");
      if(active&&(kind==mem::Kind::VertexShader||kind==mem::Kind::PixelShader))
        Require(local.cpuCapacityBytes>=16384&&local.cpuCapacityBytes%4==0,"actual bounded original-token capacity");
      const bool remoteLive=active&&author;
      const uint64_t remoteLogical=remoteLive?(kind==mem::Kind::Declaration?24:logical[i]):0;
      const uint64_t remoteSize=!remoteLive?0:kind==mem::Kind::IndexBuffer?12
        :kind==mem::Kind::VertexShader?uint64_t(vertex.floats)*20
        :kind==mem::Kind::PixelShader?uint64_t(pixel.floats)*20:0;
      Require(remote.objects==(remoteLive?1u:0u)&&remote.logicalBytes==remoteLogical
        &&remote.cpuSizeBytes==remoteSize&&remote.cpuCapacityBytes>=remoteSize,"host actual owned payload vs independent original bytes");
      if(!remoteLive||kind==mem::Kind::VertexBuffer||kind==mem::Kind::Texture||kind==mem::Kind::Declaration)
        Require(remote.cpuCapacityBytes==0,"host has no attributed payload for this kind");
      if(kind==mem::Kind::VertexShader||kind==mem::Kind::PixelShader)
        Require(remote.cpuCapacityBytes%20==0,"host float constant vector byte granularity");
    }
    if(count) {
      const auto& first=samples[0];
      Require(s.expectedHostPid==first.expectedHostPid&&s.expectedHostCreation==first.expectedHostCreation
        &&s.expectedNonceLow==first.expectedNonceLow&&s.expectedNonceHigh==first.expectedNonceHigh
        &&h.thread==first.host.thread&&h.ownerInlineBytes==first.host.ownerInlineBytes
        &&s.tickBegin>=samples[count-1].tickEnd,"same actual owner/session across phases");
      const auto& baseline=active?samples[1]:samples[0];
      if(count!=1)Require(SameAccount(s.cpu,baseline.cpu)&&SameAccount(h.cpu,baseline.host.cpu),
        "same-shaped explicit payloads do not accumulate across resource rounds");
    }
    ++count;PrintCheckpoint(s);
  }
};
void MemoryComMode(const Options& o,const Shaders& shaders,Evidence& e,MemoryEvidence& memory) {
  ComOwner owner;
  owner.window=CreateWindowExW(0,L"STATIC",L"M0 isolated offscreen account",WS_POPUP,
    0,0,Size,Size,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  Require(owner.window!=nullptr,"own offscreen HWND creation");
  owner.module=LoadLibraryW(o.dll.c_str());Require(owner.module!=nullptr,"explicit proxy DLL load");
  const auto create=reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(owner.module,"Direct3DCreate9"));
  const auto sample=reinterpret_cast<SampleFn>(GetProcAddress(owner.module,"WarVKD3D9DiagnosticCheckpoint"));
  Require(create&&sample,"explicit development proxy exports");*owner.factory.out()=create(D3D_SDK_VERSION);
  Require(owner.factory.p!=nullptr,"factory creation");
  D3DPRESENT_PARAMETERS p{};p.BackBufferWidth=Size;p.BackBufferHeight=Size;p.BackBufferFormat=D3DFMT_A8R8G8B8;
  p.BackBufferCount=1;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.hDeviceWindow=owner.window;p.Windowed=TRUE;
  p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D32F_LOCKABLE;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
  Check(owner.factory->CreateDevice(0,D3DDEVTYPE_HAL,owner.window,
    D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&p,owner.device.out()),"CreateDevice");
  auto* d=owner.device.p;
  Check(d->GetRenderTarget(0,owner.target.out()),"GetRenderTarget");
  Check(d->CreateOffscreenPlainSurface(Size,Size,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,owner.readback.out(),nullptr),"readback surface");
  Check(d->CreateQuery(D3DQUERYTYPE_EVENT,owner.query.out()),"CreateQuery");
  memory.capture(sample,d,mem::Phase::DeviceReady,0);
  const D3DVIEWPORT9 viewport{0,0,Size,Size,0,1};Check(d->SetViewport(&viewport),"SetViewport");
  Check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"cull none");
  Check(d->SetRenderState(D3DRS_ZENABLE,TRUE),"depth enable");Check(d->SetRenderState(D3DRS_ZWRITEENABLE,TRUE),"depth write");
  Check(d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL),"depth compare");
  Check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"blend disabled");Check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15),"color mask");
  for(UINT round=0;round<Rounds;++round) {
    Com<IDirect3DVertexBuffer9> vb;Com<IDirect3DIndexBuffer9> ib;Com<IDirect3DTexture9> texture;
    Com<IDirect3DVertexShader9> vs;Com<IDirect3DPixelShader9> ps;Com<IDirect3DVertexDeclaration9> declaration;
    Check(d->CreateVertexBuffer(sizeof(Vertices),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,vb.out(),nullptr),"create VB");
    Check(d->CreateIndexBuffer(sizeof(Indices),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,ib.out(),nullptr),"create IB");
    Check(d->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,texture.out(),nullptr),"create texture");
    Check(d->CreateVertexShader(shaders.vs.data(),vs.out()),"create VS");Check(d->CreatePixelShader(shaders.ps.data(),ps.out()),"create PS");
    Check(d->CreateVertexDeclaration(Elements,declaration.out()),"create declaration");e.resourcesCreated+=6;
    void* mapped=nullptr;Check(vb->Lock(0,sizeof(Vertices),&mapped,0),"lock VB");std::memcpy(mapped,Vertices.data(),sizeof(Vertices));
    Check(vb->Unlock(),"upload VB");Check(ib->Lock(0,sizeof(Indices),&mapped,0),"lock IB");std::memcpy(mapped,Indices.data(),sizeof(Indices));
    Check(ib->Unlock(),"upload IB");const DWORD color=0xff2060a0u+round;
    D3DLOCKED_RECT locked{};Check(texture->LockRect(0,&locked,nullptr,0),"lock texture");
    Require(locked.pBits&&locked.Pitch>=4,"texture storage");std::memcpy(locked.pBits,&color,4);Check(texture->UnlockRect(0),"upload texture");
    if(round==0)memory.capture(sample,d,mem::Phase::Uploaded,1);
    Check(d->SetStreamSource(0,vb.p,0,sizeof(Vertex)),"bind VB");Check(d->SetIndices(ib.p),"bind IB");
    Check(d->SetTexture(0,texture.p),"bind texture");Check(d->SetVertexShader(vs.p),"bind VS");
    Check(d->SetPixelShader(ps.p),"bind PS");Check(d->SetVertexDeclaration(declaration.p),"bind declaration");
    Require(vb.close()==0&&ib.close()==0&&texture.close()==0&&vs.close()==0&&ps.close()==0&&declaration.close()==0,"public resource references zero");
    e.resourcesReleased+=6;
    Check(d->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,ClearColor,1,0),"clear");Check(d->BeginScene(),"BeginScene");
    Check(d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"draw");Check(d->EndScene(),"EndScene");++e.draws;++e.privateBoundDraws;
    Check(d->GetRenderTargetData(owner.target.p,owner.readback.p),"readback");
    Check(owner.readback->LockRect(&locked,nullptr,D3DLOCK_READONLY),"readback lock");Pixels(e,locked.pBits,UINT(locked.Pitch),color);
    Check(owner.readback->UnlockRect(),"readback unlock");
    if(round==0)memory.capture(sample,d,mem::Phase::PrivateBound,1);
    if(round+1<Rounds) {
      Check(d->SetStreamSource(0,nullptr,0,0),"unbind VB");Check(d->SetIndices(nullptr),"unbind IB");
      Check(d->SetTexture(0,nullptr),"unbind texture");Check(d->SetVertexShader(nullptr),"unbind VS");
      Check(d->SetPixelShader(nullptr),"unbind PS");Check(d->SetVertexDeclaration(nullptr),"unbind declaration");
      if(round==0||round==15||round==47||round==94)memory.capture(sample,d,mem::Phase::Drained,round+1);
    }else e.finalPrivateBoundResources=6;
    ++e.rounds;
  }
  for(UINT n=0;n<QueryRepeats;++n) {
    Check(owner.query->Issue(D3DISSUE_END),"repeat query Issue");++e.queryIssues;
    struct {DWORD before;BOOL complete;DWORD after;} value{0x18395a7bu,FALSE,0xc2e40628u};
    Check(owner.query->GetData(&value.complete,sizeof(BOOL),0),"repeat query GetData");++e.queryPolls;
    Require(value.complete==TRUE&&value.before==0x18395a7bu&&value.after==0xc2e40628u,"query BOOL guards");
  }
  memory.capture(sample,d,mem::Phase::FinalBound,Rounds);
  owner.close();Desktop(o);e.cleanup=true;
}
void PrintMemory(const Options& o,const Evidence& e,const MemoryEvidence& m,uint32_t helpers,
    const lab::ProcessMemory& initial,const lab::ProcessMemory& final) {
  std::printf("{\"schema\":\"warvk-d3d9-memory/v1\",\"ok\":true,\"bits\":32,\"mode\":\"com\",\"publicD3D9Api\":true,"
    "\"rounds\":%u,\"draws\":%u,\"readbacks\":%u,\"pixelsChecked\":%u,\"pixelFailures\":%u,\"maxChannelError\":%u,"
    "\"resourcesCreated\":%u,\"resourcesReleased\":%u,\"privateBoundDraws\":%u,\"finalPrivateBoundResources\":%u,"
    "\"queryIssues\":%u,\"queryPolls\":%u,\"checkpoints\":%u,\"cpuHelperChecks\":%u,"
    "\"vertexShaderBytes\":%u,\"pixelShaderBytes\":%u,\"vertexFloatConstants\":%u,\"pixelFloatConstants\":%u,"
    "\"vertexIntConstants\":%u,\"pixelIntConstants\":%u,\"vertexBoolConstants\":%u,\"pixelBoolConstants\":%u,"
    "\"hostFloatConstantBytes\":20,\"parentPid\":%lu,\"parentThread\":%lu,\"parentCreation\":%llu,\"initialProcess\":",
    e.rounds,e.draws,e.readbacks,e.pixels,e.failures,e.maxError,e.resourcesCreated,e.resourcesReleased,e.privateBoundDraws,
    e.finalPrivateBoundResources,e.queryIssues,e.queryPolls,m.count,helpers,m.vertex.bytes,m.pixel.bytes,
    m.vertex.floats,m.pixel.floats,m.vertex.integers,m.pixel.integers,m.vertex.booleans,m.pixel.booleans,
    GetCurrentProcessId(),GetCurrentThreadId(),U64(lab::CreationTime(GetCurrentProcess())));
  ProcessJson(initial);std::printf(",\"finalProcess\":");ProcessJson(final);
  std::printf(",\"fixtureCleanupComplete\":%s,\"parentVulkanAbsent\":true,\"desktop\":%s,\"inputBefore\":%s,\"inputAfter\":%s,"
    "\"globalInputUsed\":false,\"desktopSwitched\":false,\"gameTouched\":false,\"productIntegrated\":false,"
    "\"memorySavingsProven\":false,\"gpuPhysicalMeasured\":false,\"opaqueCpuMeasured\":false,\"synchronizedInstant\":false,"
    "\"windowPixelsVerified\":false,\"playerVisualAccepted\":false}\n",Bool(e.cleanup),d9::NameJson(d9::CurrentDesktop()).c_str(),
    d9::NameJson(o.input).c_str(),d9::NameJson(d9::InputDesktop()).c_str());
}
} // namespace
int wmain(int argc,wchar_t** argv) {
  try {
    const auto options=Parse(argc,argv);Require(options.mode==L"com","M0 requires the actual public COM peer");Desktop(options);
    wchar_t profile[64]{};Require(!GetEnvironmentVariableW(L"WARVK_D3D9_PROFILE",profile,64),"M0 offscreen profile only");
    const uint32_t helperChecks=CpuHelpers();Require(helperChecks==54,"exact M0 CPU adversarial population");
    Shaders shaders;Evidence evidence;MemoryEvidence memory;
    memory.vertex=CountOriginalDefinitions(shaders.vs);memory.pixel=CountOriginalDefinitions(shaders.ps);
    Require(!memory.vertex.integers&&!memory.vertex.booleans&&!memory.pixel.integers&&!memory.pixel.booleans,
      "source shaders respect host integer/bool constant profile");
    const auto initial=lab::ProbeOwnProcessMemory();ValidProcess(initial);
    MemoryComMode(options,shaders,evidence,memory);
    const auto final=lab::ProbeOwnProcessMemory();ValidProcess(final);
    Require(memory.count==8&&evidence.rounds==96&&evidence.draws==96&&evidence.readbacks==96
      &&evidence.pixels==319488&&!evidence.failures&&evidence.resourcesCreated==576&&evidence.resourcesReleased==576
      &&evidence.privateBoundDraws==96&&evidence.finalPrivateBoundResources==6
      &&evidence.queryIssues==256&&evidence.queryPolls==256&&evidence.cleanup,"full native workload and cleanup population");
    PrintMemory(options,evidence,memory,helperChecks,initial,final);return 0;
  }catch(const std::exception& error) {
    std::printf("{\"schema\":\"warvk-d3d9-memory-error/v1\",\"ok\":false,\"error\":%s}\n",Json(error.what()).c_str());return 1;
  }
}
