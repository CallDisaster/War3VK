// Reuse only the established public-COM fixture's window, resource setup and
// teardown. The workload and pixel oracle below never include proxy/backend
// headers and run unchanged against local DXVK and the remote x86 COM proxy.
#define wmain ExistingD3D9FixtureMain
#include "test_render_host_d3d9_parent.cpp"
#undef wmain
#include <algorithm>
#include "../tools/render_host/d3d9_memory_probe.h" // Explicit cold diagnostic ABI, never the draw oracle.

namespace {
#ifndef WARVK_BATCH_TEST_DRAWS
#define WARVK_BATCH_TEST_DRAWS 97
#endif
#ifndef WARVK_BATCH_TEST_FIXED
#define WARVK_BATCH_TEST_FIXED 0
#endif
constexpr unsigned WarmFrames = 4, MeasuredFrames = 40, DrawsPerFrame = WARVK_BATCH_TEST_DRAWS;
static_assert(DrawsPerFrame == 97 || DrawsPerFrame == 1025);
// The fixed-function build draws the same workload with no shaders bound:
// WORLD reproduces the SM3 shape constant and TEXTUREFACTOR x texture the tint.
constexpr bool FixedFunction = WARVK_BATCH_TEST_FIXED != 0;
uint64_t Tick() { LARGE_INTEGER value{}; Require(QueryPerformanceCounter(&value)!=FALSE,"QPC"); return value.QuadPart; }
namespace memory=warvk::host::d9::memory;
using CheckpointFn=HRESULT(WINAPI*)(IDirect3DDevice9*,UINT,UINT,memory::Checkpoint*,UINT);
CheckpointFn Diagnostic(Owner& owner) {
  const auto fn=reinterpret_cast<CheckpointFn>(GetProcAddress(owner.renderer,"WarVKD3D9DiagnosticCheckpoint"));
  Require(fn!=nullptr,"explicit cold diagnostic export");return fn;
}
void PendingDiagnostic(Owner& owner) {
  memory::Checkpoint sample{};
  std::array<uint8_t,sizeof(sample)> before{};std::memcpy(before.data(),&sample,sizeof(sample));
  Require(Diagnostic(owner)(owner.device.get(),UINT(memory::Phase::Uploaded),1,&sample,sizeof(sample))==D3DERR_INVALIDCALL,
    "diagnostic submitted locally pending draws");
  Require(std::memcmp(before.data(),&sample,sizeof(sample))==0,"refused diagnostic modified output");
}
void DrainedDiagnostic(Owner& owner) {
  memory::Checkpoint sample{};
  Check(Diagnostic(owner)(owner.device.get(),UINT(memory::Phase::Uploaded),3,&sample,sizeof(sample)),"explicit drained diagnostic");
  Require(sample.version==1&&sample.bits==32&&sample.host.bits==64&&sample.cpu.valid&&sample.host.cpu.valid
    &&sample.expectedHostPid==sample.host.pid&&sample.expectedHostCreation==sample.host.creation
    &&sample.expectedSequence==sample.host.sequence,"cold diagnostic identity");
  Require(sample.host.draws==44*DrawsPerFrame+193&&sample.host.submitted==sample.host.completed
    &&sample.host.completed==sample.host.retired,"diagnostic changed or fabricated draw completion");
  Require(sample.before.valid&&sample.after.valid&&sample.host.process.valid,"cold process memory unavailable");
  std::fprintf(stderr,"BATCH_COLD_MEMORY parentPrivateBefore=%llu parentPrivateAfter=%llu hostPrivate=%llu connectionInline=%llu hostOwnerInline=%llu\n",
    sample.before.privateBytes,sample.after.privateBytes,sample.host.process.privateBytes,
    sample.connectionInlineBytes,sample.host.ownerInlineBytes);
}
std::array<float,4> Color(unsigned draw,unsigned frame) {
  const unsigned bits=1+(draw+frame)%7;
  return {float((bits>>0)&1),float((bits>>1)&1),float((bits>>2)&1),1.f};
}
void DrawConstants(Owner& owner,const float* shape,const std::array<float,4>& color,const char* stage) {
  if(!FixedFunction) {
    Check(owner.device->SetVertexShaderConstantF(0,shape,1),stage);
    Check(owner.device->SetPixelShaderConstantF(0,color.data(),1),stage);
    return;
  }
  // Positions carry w=1, so row-vector translation equals the VS shape.yz offset.
  D3DMATRIX world{};world._11=world._22=shape[0];world._33=world._44=1;world._41=shape[1];world._42=shape[2];
  Check(owner.device->SetTransform(D3DTS_WORLD,&world),stage);
  Check(owner.device->SetRenderState(D3DRS_TEXTUREFACTOR,D3DCOLOR_ARGB(255,unsigned(color[0]*255),
    unsigned(color[1]*255),unsigned(color[2]*255))),stage);
}
void FixedStages(Owner& owner) {
  Check(owner.device->SetVertexShader(nullptr),"fixed VS");Check(owner.device->SetPixelShader(nullptr),"fixed PS");
  Check(owner.device->SetRenderState(D3DRS_LIGHTING,FALSE),"fixed lighting off");
  const std::pair<D3DTEXTURESTAGESTATETYPE,DWORD> stage0[]={{D3DTSS_COLOROP,D3DTOP_MODULATE},{D3DTSS_COLORARG1,D3DTA_TEXTURE},
    {D3DTSS_COLORARG2,D3DTA_TFACTOR},{D3DTSS_ALPHAOP,D3DTOP_MODULATE},{D3DTSS_ALPHAARG1,D3DTA_TEXTURE},{D3DTSS_ALPHAARG2,D3DTA_TFACTOR}};
  for(const auto& [type,value]:stage0)Check(owner.device->SetTextureStageState(0,type,value),"fixed stage 0");
}
void ConstantShaders(Owner& owner) {
  if(FixedFunction) {
    FixedStages(owner);
    D3DLOCKED_RECT rect{};Check(owner.texture->LockRect(0,&rect,nullptr,0),"white texture lock");
    for(unsigned y=0;y<2;++y)std::memset(static_cast<uint8_t*>(rect.pBits)+y*rect.Pitch,255,8);
    Check(owner.texture->UnlockRect(0),"white texture upload");
    Check(owner.device->SetRenderState(D3DRS_ZENABLE,FALSE),"depth off");
    Check(owner.device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE),"depth write off");
    Check(owner.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"cull off");
    Check(owner.device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"blend off");
    return;
  }
  constexpr char vs[] = "float4 shape:register(c0); struct In {float4 p:POSITION0;float2 uv:TEXCOORD0;float4 c:COLOR0;};"
    "struct Out {float4 p:POSITION0;float2 uv:TEXCOORD0;float4 c:COLOR0;};"
    "Out main(In i){Out o;o.p=i.p;o.p.xy=i.p.xy*shape.x+shape.yz;o.uv=i.uv;o.c=i.c;return o;}";
  constexpr char ps[] = "sampler2D tex:register(s0);float4 tint:register(c0);"
    "float4 main(float2 uv:TEXCOORD0,float4 c:COLOR0):COLOR0{return tex2D(tex,uv)*c*tint;}";
  const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(owner.compiler,"D3DCompile"));
  Com<ID3DBlob> code,errors;
  Check(compile(vs,sizeof(vs)-1,"batch-oracle-vs",nullptr,nullptr,"main","vs_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,
    code.put(),errors.put()),"batch VS compile");
  Check(owner.device->SetVertexShader(nullptr),"unbind old VS");owner.vs.reset();
  Check(owner.device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()),owner.vs.put()),"batch VS");
  Check(owner.device->SetVertexShader(owner.vs.get()),"batch VS bind");
  Check(compile(ps,sizeof(ps)-1,"batch-oracle-ps",nullptr,nullptr,"main","ps_3_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,
    code.put(),errors.put()),"batch PS compile");
  Check(owner.device->SetPixelShader(nullptr),"unbind old PS");owner.ps.reset();
  Check(owner.device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()),owner.ps.put()),"batch PS");
  Check(owner.device->SetPixelShader(owner.ps.get()),"batch PS bind");
  D3DLOCKED_RECT rect{};Check(owner.texture->LockRect(0,&rect,nullptr,0),"white texture lock");
  for(unsigned y=0;y<2;++y)std::memset(static_cast<uint8_t*>(rect.pBits)+y*rect.Pitch,255,8);
  Check(owner.texture->UnlockRect(0),"white texture upload");
  Check(owner.device->SetRenderState(D3DRS_ZENABLE,FALSE),"depth off");
  Check(owner.device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE),"depth write off");
  Check(owner.device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE),"cull off");
  Check(owner.device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE),"blend off");
}
uint64_t VerifyTiles(Owner& owner,unsigned frame) {
  if(!owner.readback.get())Check(owner.device->CreateOffscreenPlainSurface(64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,
    owner.readback.put(),nullptr),"readback create");
  Check(owner.device->GetRenderTargetData(owner.target.get(),owner.readback.get()),"readback completed frame");
  D3DLOCKED_RECT lock{};Check(owner.readback->LockRect(&lock,nullptr,D3DLOCK_READONLY),"readback lock");
  uint64_t hash=FnvOffset;unsigned failures=0;
  for(unsigned tile=0;tile<32;++tile) {
    const auto lastDraw=tile+((DrawsPerFrame-1-tile)/32)*32;
    const auto color=Color(lastDraw,frame);
    for(unsigned dy=6;dy<=10;dy+=2)for(unsigned dx=3;dx<=5;++dx) {
      const auto* p=static_cast<const uint8_t*>(lock.pBits)+(tile/8*16+dy)*lock.Pitch+(tile%8*8+dx)*4;
      const uint8_t expected[]={uint8_t(color[2]*255),uint8_t(color[1]*255),uint8_t(color[0]*255),255};
      for(unsigned c=0;c<4;++c)if(std::abs(int(p[c])-int(expected[c]))>1)++failures;
      hash=Hash(p,4,hash);
    }
  }
  Check(owner.readback->UnlockRect(),"readback unlock");
  Require(!failures,"constant slices or draw ordering changed tile colors");
  return hash;
}
uint64_t VerifyCoverageAndUploadBarrier(Owner& owner,unsigned& diagnosticRefusals) {
  uint64_t hash=FnvOffset;
  Com<IDirect3DVertexBuffer9> sequential;Com<IDirect3DIndexBuffer9> sequentialIndices;
  // 129 draws: 128 independently visible tiles, then one deliberate overwrite.
  // Five batches wrap the four-flight ring; unlike the timed stress scene,
  // the first four batches remain observable instead of being overdrawn away.
  for(unsigned pass=0;pass<2;++pass) {
    if(pass) {
      std::array<Vertex,4> quad{};FillQuad(quad.data(),.75f,.6f,1.f);
      std::array<Vertex,9> vertices{};const unsigned order[]={0,1,2,0,2,3};
      for(unsigned i=0;i<6;++i)vertices[i+3]=quad[order[i]];
      Check(owner.device->CreateVertexBuffer(sizeof(vertices),D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT,sequential.put(),nullptr),"sequential VB");
      void* mapped=nullptr;Check(sequential->Lock(0,sizeof(vertices),&mapped,0),"sequential VB lock");
      std::memcpy(mapped,vertices.data(),sizeof(vertices));Check(sequential->Unlock(),"sequential VB upload");
      const uint16_t indices[]={0,1,2,3,4,5};
      Check(owner.device->CreateIndexBuffer(sizeof(indices),D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,sequentialIndices.put(),nullptr),"sequential IB");
      Check(sequentialIndices->Lock(0,sizeof(indices),&mapped,0),"sequential IB lock");
      std::memcpy(mapped,indices,sizeof(indices));Check(sequentialIndices->Unlock(),"sequential IB upload");
      Check(owner.device->SetStreamSource(0,sequential.get(),0,sizeof(Vertex)),"sequential stream");
    }
    // D3D9 Clear without rectangles still respects the CURRENT viewport.
    // The last tile of the previous pass is not a whole-target clear domain.
    const D3DVIEWPORT9 full{0,0,64,64,0,1};
    Check(owner.device->SetViewport(&full),"coverage full clear viewport");
    Check(owner.device->Clear(0,nullptr,D3DCLEAR_TARGET|(pass?D3DCLEAR_ZBUFFER:0),0xff101820,pass?0.f:1.f,0),"coverage clear");
    Check(owner.device->BeginScene(),"coverage begin");
    const unsigned draws=pass?64:129;
    for(unsigned draw=0;draw<draws;++draw) {
      if(pass&&draw==32) {
        // Mutate a texture while prior draws are admitted. Earlier draws must
        // keep the white version; only subsequent draws may sample gray.
        D3DLOCKED_RECT rect{};Check(owner.texture->LockRect(0,&rect,nullptr,0),"ordered texture lock");
        for(unsigned y=0;y<2;++y)std::memset(static_cast<uint8_t*>(rect.pBits)+y*rect.Pitch,128,8);
        Check(owner.texture->UnlockRect(0),"ordered texture upload");
      }
      const unsigned tile=draw%128,height=pass?8:4;
      const D3DVIEWPORT9 vp{tile%8*8,tile/8*height,8,height,0,1};
      Check(owner.device->SetViewport(&vp),"coverage viewport");
      const float shape[]={draw%2?1.f:.9f,draw%2?.02f:-.02f,0,1};
      const auto color=Color(draw,pass?3:0);
      DrawConstants(owner,shape,color,"coverage constants");
      if(pass) {
        // Alternate visible depth/blend/write-mask changes INSIDE one batch.
        // Class 0 writes color; classes 1/2/3 retain Clear independently.
        const unsigned mode=draw%4;
        Check(owner.device->SetRenderState(D3DRS_ZENABLE,mode==1||mode==2),"coverage depth enable");
        Check(owner.device->SetRenderState(D3DRS_ZFUNC,mode==1?D3DCMP_LESS:D3DCMP_ALWAYS),"coverage depth compare");
        Check(owner.device->SetRenderState(D3DRS_ALPHABLENDENABLE,mode==2),"coverage blend enable");
        Check(owner.device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO),"coverage blend source");
        Check(owner.device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE),"coverage blend destination");
        Check(owner.device->SetRenderState(D3DRS_COLORWRITEENABLE,mode==3?0:15),"coverage color mask");
        if(!(draw%2)) {
          Check(owner.device->SetIndices(nullptr),"nonindexed no IB");
          Check(owner.device->DrawPrimitive(D3DPT_TRIANGLELIST,3,2),"nonindexed real draw");
        } else {
          Check(owner.device->SetIndices(sequentialIndices.get()),"mixed indexed IB");
          Check(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,3,0,6,0,2),"mixed indexed draw");
        }
      } else Check(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"coverage draw");
      if(!pass&&!draw&&!owner.options.reference) {
        wchar_t mode[8]{};
        // Fixed-function draws stage locally only in the fixed batch profile.
        if(GetEnvironmentVariableW(FixedFunction?L"WARVK_D3D9_FIXED_BATCH":L"WARVK_D3D9_BATCH_DRAWS",mode,8)) {
          PendingDiagnostic(owner);++diagnosticRefusals;
        }
      }
    }
    Check(owner.device->EndScene(),"coverage end");
    if(!pass&&!owner.options.reference) {
      // No readback or Query may hide an unflushed final GPU group.
      memory::Checkpoint sample{};
      Check(Diagnostic(owner)(owner.device.get(),UINT(memory::Phase::Uploaded),2,&sample,sizeof(sample)),"EndScene submitted group tail");
    }
    Check(owner.device->GetRenderTargetData(owner.target.get(),owner.readback.get()),"coverage readback");
    D3DLOCKED_RECT rect{};Check(owner.readback->LockRect(&rect,nullptr,D3DLOCK_READONLY),"coverage read lock");
    unsigned failures=0;
    for(unsigned tile=0;tile<(pass?64:128);++tile) {
      const auto color=Color(!pass&&tile==0?128:tile,pass?3:0);
      const unsigned intensity=pass&&tile>=32?128:255,height=pass?8:4;
      std::array<uint8_t,4> expected={uint8_t(color[2]*intensity),uint8_t(color[1]*intensity),
        uint8_t(color[0]*intensity),uint8_t(intensity)};
      if(pass&&tile%4)expected={0x20,0x18,0x10,0xff};
      for(unsigned x=3;x<=5;++x) {
        const auto* p=static_cast<const uint8_t*>(rect.pBits)+(tile/8*height+height/2)*rect.Pitch+tile%8*32+x*4;
        for(unsigned c=0;c<4;++c)if(std::abs(int(p[c])-int(expected[c]))>1){
          if(failures<12)std::fprintf(stderr,"coverage mismatch pass=%u tile=%u x=%u channel=%u actual=%u expected=%u\n",pass,tile,x,c,unsigned(p[c]),unsigned(expected[c]));
          ++failures;
        }
        hash=Hash(p,4,hash);
      }
    }
    Check(owner.readback->UnlockRect(),"coverage unlock");
    Require(!failures,"batch reuse or texture mutation changed an earlier draw");
  }
  Check(owner.device->SetStreamSource(0,owner.vb.get(),StreamOffset,sizeof(Vertex)),"restore original stream");
  Check(owner.device->SetIndices(owner.ib.get()),"restore original IB");
  return hash;
}

uint64_t FileTicks(FILETIME value){return uint64_t(value.dwLowDateTime)|(uint64_t(value.dwHighDateTime)<<32);}
struct CpuTimes {uint64_t creation,kernel,user,cycles;};
CpuTimes ProcessTimes(HANDLE process) {
  FILETIME creation{},exit{},kernel{},user{};
  Require(GetProcessTimes(process,&creation,&exit,&kernel,&user)!=FALSE,"process CPU times");
  Require(WaitForSingleObject(process,0)==WAIT_TIMEOUT,"measured process exited");
  ULONG64 cycles=0;Require(QueryProcessCycleTime(process,&cycles)!=FALSE,"process CPU cycle counter");
  return {FileTicks(creation),FileTicks(kernel),FileTicks(user),cycles};
}
struct SystemTimes {uint64_t idle,kernel,user;};
SystemTimes SystemCpu() {
  FILETIME idle{},kernel{},user{};Require(GetSystemTimes(&idle,&kernel,&user)!=FALSE,"system CPU times");
  return {FileTicks(idle),FileTicks(kernel),FileTicks(user)};
}
struct HostCpuHandle {
  HANDLE value=nullptr;uint64_t creation=0;
  ~HostCpuHandle(){if(value)CloseHandle(value);}
  void open(Owner& owner) {
    if(owner.options.reference)return;
    memory::Checkpoint sample{};
    Check(Diagnostic(owner)(owner.device.get(),UINT(memory::Phase::Uploaded),1,&sample,sizeof(sample)),"CPU measure peer identity");
    Require(sample.expectedHostPid==sample.host.pid&&sample.expectedHostCreation==sample.host.creation,
      "CPU measure authenticated host identity");
    value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,sample.expectedHostPid);
    Require(value!=nullptr,"CPU measure retained host handle");creation=sample.expectedHostCreation;
    Require(ProcessTimes(value).creation==creation,"CPU measure host creation mismatch");
  }
};
}

unsigned VerifyQueryTransitions(Owner& owner) {
  const auto observe=[](IDirect3DQuery9* query) {
    const auto deadline=GetTickCount64()+5000;
    for(;;) {
      struct {DWORD before=0x12345678;BOOL ready=FALSE;DWORD after=0x87654321;} value;
      // The reference may retain Clear on its CPU command stream. A blocking
      // acceptance loop must request FLUSH; flags=0 is observation-only.
      const auto hr=query->GetData(&value.ready,sizeof(BOOL),D3DGETDATA_FLUSH);
      Require((hr==S_OK||hr==S_FALSE)&&value.before==0x12345678&&value.after==0x87654321,
        "query transition result/guard");
      Require(value.ready==(hr==S_OK?TRUE:FALSE),"query transition BOOL");
      if(hr==S_OK)return;
      Require(GetTickCount64()<deadline,"query transition deadline");SwitchToThread();
    }
  };
  unsigned checks=0;
  Check(owner.query->Issue(D3DISSUE_END),"same serial re-Issue");observe(owner.query.get());++checks;
  Check(owner.query->Issue(D3DISSUE_END),"second same serial re-Issue");observe(owner.query.get());++checks;
  IDirect3DQuery9* other=nullptr;Check(owner.device->CreateQuery(D3DQUERYTYPE_EVENT,&other),"independent query");
  Check(owner.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"later query GPU prefix");
  Check(other->Issue(D3DISSUE_END),"independent query Issue");
  BOOL ready=FALSE;const auto first=other->GetData(&ready,sizeof(BOOL),0);
  Require(first==S_FALSE||first==S_OK,"first independent observation");
  other->Release();other=nullptr;++checks; // Destruction orders after any pending completion.
  Check(owner.device->CreateQuery(D3DQUERYTYPE_EVENT,&other),"replacement query");
  Check(other->Issue(D3DISSUE_END),"replacement Issue");observe(other);other->Release();++checks;
  Check(owner.query->Issue(D3DISSUE_END),"original query after replacement");observe(owner.query.get());++checks;
  return checks;
}

int wmain(int argc,wchar_t** argv) {
  Owner owner;
  try {
    owner.options=ParseOptions(argc,argv);
    Require(!owner.options.invalid&&!owner.options.wsi&&!owner.options.present,"batch fixture offscreen profile");
    const auto desktop=NonInputDesktop();
    CreateDevice(owner);CreateGeometry(owner);CreateShadersAndTexture(owner);ConstantShaders(owner);
    Check(owner.device->CreateQuery(D3DQUERYTYPE_EVENT,owner.query.put()),"batch EVENT create");
    std::array<uint64_t,MeasuredFrames> admission{},completed{};
    uint64_t imageHash=FnvOffset,checked=0;
    LARGE_INTEGER frequency{};Require(QueryPerformanceFrequency(&frequency)!=FALSE,"QPC frequency");
    HostCpuHandle cpuHost;cpuHost.open(owner);
    const auto parentBefore=ProcessTimes(GetCurrentProcess());
    const auto hostBefore=cpuHost.value?ProcessTimes(cpuHost.value):CpuTimes{};
    const auto systemBefore=SystemCpu();const auto workloadStart=Tick();
    for(unsigned frame=0;frame<WarmFrames+MeasuredFrames;++frame) {
      const auto begin=Tick();
      const D3DVIEWPORT9 full{0,0,64,64,0,1};
      Check(owner.device->SetViewport(&full),"frame full clear viewport");
      Check(owner.device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff101820,1,0),"frame clear");
      Check(owner.device->BeginScene(),"frame begin");
      for(unsigned draw=0;draw<DrawsPerFrame;++draw) {
        const unsigned tile=draw%32;
        const D3DVIEWPORT9 viewport{tile%8*8,tile/8*16,8,16,0,1};
        Check(owner.device->SetViewport(&viewport),"tile viewport");
        const float shape[]={draw%2?1.f:.9f,draw%2?.02f:-.02f,0,1};
        const auto color=Color(draw,frame);
        DrawConstants(owner,shape,color,"owned draw constants");
        Check(owner.device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,0,2),"tile draw");
      }
      Check(owner.device->EndScene(),"flush partial batch at EndScene");
      const auto admitted=Tick();
      Check(owner.query->Issue(D3DISSUE_END),"ordered EVENT barrier");
      const auto deadline=GetTickCount64()+5000;
      for(;;) {
        BOOL done=FALSE;const auto hr=owner.query->GetData(&done,sizeof(done),D3DGETDATA_FLUSH);
        Require(hr==D3D_OK||hr==S_FALSE,"EVENT failure");
        if(hr==D3D_OK){Require(done!=FALSE,"EVENT false success");break;}
        Require(GetTickCount64()<deadline,"GPU EVENT deadline");SwitchToThread();
      }
      const auto settled=Tick();
      // Readback/pixel validation is OUTSIDE the timed window in every arm.
      const auto pixels=VerifyTiles(owner,frame);imageHash=Hash(&pixels,sizeof(pixels),imageHash);checked+=32*9;
      if(frame>=WarmFrames){admission[frame-WarmFrames]=admitted-begin;completed[frame-WarmFrames]=settled-begin;}
    }
    const auto workloadEnd=Tick();const auto systemAfter=SystemCpu();
    const auto hostAfter=cpuHost.value?ProcessTimes(cpuHost.value):CpuTimes{};
    const auto parentAfter=ProcessTimes(GetCurrentProcess());
    Require(parentBefore.creation==parentAfter.creation&&hostBefore.creation==hostAfter.creation
      &&parentAfter.kernel>=parentBefore.kernel&&parentAfter.user>=parentBefore.user
      &&hostAfter.kernel>=hostBefore.kernel&&hostAfter.user>=hostBefore.user
      &&parentAfter.cycles>=parentBefore.cycles&&hostAfter.cycles>=hostBefore.cycles
      &&systemAfter.kernel>=systemBefore.kernel&&systemAfter.user>=systemBefore.user&&systemAfter.idle>=systemBefore.idle,
      "CPU counter identity/regression");
    const auto queryTransitions=VerifyQueryTransitions(owner);
    unsigned diagnosticRefusals=0;
    const auto coverageHash=VerifyCoverageAndUploadBarrier(owner,diagnosticRefusals);
    if(!owner.options.reference)DrainedDiagnostic(owner);
    Teardown(owner);
    const auto after=NonInputDesktop();Require(after.thread==desktop.thread&&after.input==desktop.input,"desktop changed");
    std::printf("{\"schema\":\"warvk-d3d9-batch-fixture/v1\",\"ok\":true,\"bits\":32,\"reference\":%s,\"fixedFunction\":%s,"
      "\"frames\":44,\"warmFrames\":4,\"drawsPerFrame\":%u,\"pixelChecks\":%llu,\"pixelFailures\":0,"
      "\"imageHash\":\"%016llx\",\"qpcFrequency\":%llu,\"admissionTicks\":[",JsonBool(owner.options.reference),JsonBool(FixedFunction),DrawsPerFrame,checked,imageHash,uint64_t(frequency.QuadPart));
    for(unsigned i=0;i<MeasuredFrames;++i)std::printf("%s%llu",i?",":"",admission[i]);
    std::printf("],\"completedTicks\":[");
    for(unsigned i=0;i<MeasuredFrames;++i)std::printf("%s%llu",i?",":"",completed[i]);
    std::printf("],\"cpuIncludesReadback\":true,\"hostCpuMeasured\":%s,\"workloadWallTicks\":%llu,"
      "\"parentKernel100ns\":%llu,\"parentUser100ns\":%llu,\"hostKernel100ns\":%llu,\"hostUser100ns\":%llu,"
      "\"parentCpuCycles\":%llu,\"hostCpuCycles\":%llu,"
      "\"systemKernel100ns\":%llu,\"systemUser100ns\":%llu,\"systemIdle100ns\":%llu,",
      JsonBool(!owner.options.reference),workloadEnd-workloadStart,parentAfter.kernel-parentBefore.kernel,
      parentAfter.user-parentBefore.user,hostAfter.kernel-hostBefore.kernel,hostAfter.user-hostBefore.user,
      parentAfter.cycles-parentBefore.cycles,hostAfter.cycles-hostBefore.cycles,
      systemAfter.kernel-systemBefore.kernel,systemAfter.user-systemBefore.user,systemAfter.idle-systemBefore.idle);
    std::printf("\"coverageDraws\":193,\"nonIndexedDraws\":32,\"stateTransitionDraws\":64,\"coverageChecks\":576,\"coverageHash\":\"%016llx\","
      "\"diagnosticPendingRefusals\":%u,\"diagnosticAfterFlush\":%u,\"queryTransitions\":%u,"
      "\"parentVulkanAbsent\":%s,\"normalTeardown\":true,\"gameTouched\":false,\"playerFpsProven\":false}\n",
      coverageHash,diagnosticRefusals,owner.options.reference?0u:1u,queryTransitions,JsonBool(owner.parentVulkanAbsent));return 0;
  }catch(const std::exception& e){std::fprintf(stderr,"batch fixture stage=%s error=%s\n",owner.stage,e.what());return 1;}
}
