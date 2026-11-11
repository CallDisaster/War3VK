#include "d3d9_backend.h"
#include "d3d9_factory.h"
#include "host_surface_target.h"
#include "../d3d9_draw_wire.h"
#include "../d3d9_annotation_wire.h"
#include "../d3d9_declaration_wire.h"
#include "../d3d9_ff_wire.h"
#include "../d3d9_shader_tokens.h"
#include "../d3d9_window_identity.h"
#include "../win32_transport.h"
#include "../../../src/util/log/log.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace dxvk { Logger Logger::s_instance("warvk-d3d9-host.log"); }
using namespace warvk::host;
using GPU=dxvkcore::D3D9Backend;
static_assert(sizeof(void*)==8,"D3D9 DXVK execution owner must be x64");
namespace {
void Require(bool c,const char* r){if(!c)throw std::runtime_error(r);}
void ExitMilestone(const char* phase,ULONGLONG processStart) noexcept {
  // Cold-path diagnostics only. Use the inherited file handle directly, so a
  // CRT stream lock cannot hide which teardown boundary was last reached.
  char line[256]{};
  const int count=std::snprintf(line,sizeof(line),
    "D3D9 host exit-milestone pid=%lu tid=%lu phase=%s tickMs=%llu processElapsedMs=%llu\n",
    GetCurrentProcessId(),GetCurrentThreadId(),phase,GetTickCount64(),GetTickCount64()-processStart);
  if(count>0&&size_t(count)<sizeof(line)) {
    DWORD written=0;WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,DWORD(count),&written,nullptr);
  }
}
struct MainExitWitness {
  ULONGLONG start;
  ~MainExitWitness(){ExitMilestone("wmain-locals-complete",start);}
};
void LogExitIntent(const char* strategy,DWORD code,ULONGLONG processStart) noexcept {
  std::fprintf(stderr,"D3D9 host exit-intent pid=%lu strategy=%s code=%lu processElapsedMs=%llu\n",
    GetCurrentProcessId(),strategy,code,GetTickCount64()-processStart);
  std::fflush(stdout);std::fflush(stderr);
  ExitMilestone("exit-intent-flushed",processStart);
}
[[noreturn]] void SelfFailStop(ULONGLONG processStart) noexcept {
  LogExitIntent("self-fail-stop",4,processStart);
  // Never run process-detach callbacks while an uncertain GPU/driver owner is
  // deliberately retained. This handle can only terminate this host itself.
  const BOOL requested=TerminateProcess(GetCurrentProcess(),4);
  const DWORD error=requested?ERROR_SUCCESS:GetLastError();
  // Successful self-termination does not return. If it unexpectedly does, or
  // the request fails, abort is the final fail-stop fallback, not normal exit.
  std::fprintf(stderr,"D3D9 host self-fail-stop unexpectedly returned requested=%s win32=%lu; abort fallback\n",
    requested?"true":"false",error);std::fflush(stderr);
  std::abort();
}
struct Entry {
  d9::ResourceRef ref{};
  d9::Type type=d9::Type::None;
  uint32_t slot=0,size=0,width=0,height=0,initializedPrefix=0;
  bool uploaded=false,dynamic=false;
  uint64_t queryRevision=0,querySerial=0;
  d9::TextureLayout textureLayout;
  uint32_t textureRevision=0,initializedLevels=0;
  // RetainContent only: vertex-buffer bytes or the packed texture layout.
  std::vector<uint8_t> content;
};
struct HandleStats {
  uint64_t logicalHandlesCreated=0,logicalHandlesDestroyed=0,logicalHandlesReused=0;
  // resourcesReused counts the six GPU author-resource types, not Query handles.
  uint64_t resourcesReused=0,queryHandlesCreated=0,queryHandlesDestroyed=0,queryHandlesReused=0;
  uint64_t queryIssues=0,queryPolls=0,queryWaits=0;
  uint32_t liveLogicalHandles=0,liveQueries=0;
  void print() const {
    std::printf("\"logicalHandlesCreated\":%llu,\"logicalHandlesDestroyed\":%llu,\"logicalHandlesReused\":%llu,"
      "\"resourcesReused\":%llu,\"queryHandlesCreated\":%llu,\"queryHandlesDestroyed\":%llu,\"queryHandlesReused\":%llu,"
      "\"queryIssues\":%llu,\"queryPolls\":%llu,\"queryWaits\":%llu,\"liveLogicalHandles\":%u,\"liveQueries\":%u,",
      logicalHandlesCreated,logicalHandlesDestroyed,logicalHandlesReused,resourcesReused,
      queryHandlesCreated,queryHandlesDestroyed,queryHandlesReused,queryIssues,queryPolls,queryWaits,
      liveLogicalHandles,liveQueries);
  }
};
using Clock=std::chrono::steady_clock;
uint64_t ElapsedNs(Clock::time_point begin) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count());
}
// Opt-in admission account (WARVK_D3D9_ADMISSION_PROFILE=1), cumulative CPU
// wall time. Disabled sessions take no clock reads. Samples are cut only after
// an accepted EVENT Issue, never per message or draw; full is counted dropped.
struct AdmissionCounters {
  uint64_t messages=0,idleNs=0,readNs=0,replyNs=0;
  uint64_t batchMessages=0,batchDraws=0,batchReadNs=0,batchDecodeNs=0,batchApplyNs=0,batchReplyNs=0;
  uint64_t prepareNs=0,recordNs=0,constantNs=0,bindNs=0,samplerNs=0,drawNs=0;
  uint64_t flushApplyNs=0,waitEventApplyNs=0,otherApplyNs=0;
};
struct AdmissionSample {
  uint64_t query=0;
  AdmissionCounters cumulative;
};
struct Session {
  explicit Session(DWORD peer,ipc::Nonce nonce,HANDLE process=nullptr,surface::Mode mode=surface::Mode::Peer)
    :peerPid(peer),peerProcess(process),surfaceMode(mode),marker(d9::WindowMarker(nonce)){}
  DWORD peerPid;
  HANDLE peerProcess; // Borrowed from wmain OpenPeer; owned-target start duplicates it.
  surface::Mode surfaceMode;
  std::wstring marker;
  // Allocate no GPU owner before the entire initialization preflight accepts.
  std::unique_ptr<dxvkcore::D3D9Factory> factory;
  std::unique_ptr<GPU> gpu;
  GPU::Stats uninitializedStats{};
  const GPU::Stats& stats() const { return gpu?gpu->stats():uninitializedStats; }
  std::array<Entry,d9::MaxResources+1> entries{};
  d9::ResourceRegistry registry;
  d9::TextureUpload textureUpload;
  d9::TextureStages textureStages;
  HandleStats handles;
  uint64_t accepted=0,rejected=0,backendCalls=0;
  // Accepted DrawFixedBatch messages only; one-draw DrawFixed is not counted.
  uint64_t fixedBatches=0,fixedBatchDraws=0;uint32_t largestFixedBatch=0;
  // Session-owned decode targets (about 60 KiB), never per-message stack copies.
  std::array<GPU::DrawState,d9::MaxFixedDrawBatch> fixedBatchStates{};
  std::array<d9::FixedFunctionState,d9::MaxFixedDrawBatch> fixedBatchFixed{};
  bool initialized=false,finished=false;
  bool presentedReadback=false;
  // A captured diagnostic frame at the current extent; Reset drops it.
  bool presentedCurrent=false;
  uint32_t targetWidth=0,targetHeight=0; // Negotiated at Initialize, changed by Reset.
  HWND presenterWindow=nullptr; // The authenticated peer window of a WSI session.
  // Negotiated at Initialize. Authoritative x64 CPU copies let the x86 owner
  // drop its own. Index buffers keep a session copy too, apart from the
  // backend's indexBytes, so ReadContent never reaches into the backend.
  bool retainContent=false;
  uint64_t retainedBytes=0,peakRetainedBytes=0,contentReads=0,contentReadBytes=0;
  std::vector<uint8_t> retained(d9::Type type,uint32_t bytes) {
    if(!retainContent||(type!=d9::Type::VertexBuffer&&type!=d9::Type::IndexBuffer&&type!=d9::Type::Texture))return {};
    return std::vector<uint8_t>(bytes);
  }
  void adopt(Entry& entry,std::vector<uint8_t> content) {
    retainedBytes+=content.size();peakRetainedBytes=std::max(peakRetainedBytes,retainedBytes);
    entry.content=std::move(content);
  }
  // Negotiated at Initialize: x86 data-layer records. Until enhancement
  // executors consume them, the recorder proves order, payloads and binding.
  bool annotations=false,annotationsPending=false;
  d9::DrawAnnotations pendingAnnotations;
  d9::AnnotationRecorder annotationRecorder;
  void deliverAnnotations(const GPU::DrawState* states) {
    uint32_t last=UINT32_MAX;
    pendingAnnotations.forEach([&](uint16_t index,uint16_t schema,const uint8_t* data,uint16_t bytes) {
      const auto& s=states[index];
      if(index!=last){++annotationRecorder.annotatedDraws;last=index;}
      annotationRecorder.draw(schema,data,bytes,uint32_t(s.primitiveType),s.primitiveCount,s.numVertices,s.startIndex);
    });
  }
  bool admissionProfile=false;
  AdmissionCounters admission;
  std::array<AdmissionSample,128> admissionSamples{};
  uint32_t admissionSampleCount=0,admissionSamplesDropped=0;
  void sampleAdmission() {
    if(admissionSampleCount==admissionSamples.size()){++admissionSamplesDropped;return;}
    auto& sample=admissionSamples[admissionSampleCount++];
    sample.query=handles.queryIssues;sample.cumulative=admission;
    const auto& s=stats();
    sample.cumulative.prepareNs=s.admissionPrepareNs;sample.cumulative.recordNs=s.admissionRecordNs;
    sample.cumulative.constantNs=s.admissionConstantNs;sample.cumulative.bindNs=s.admissionBindNs;
    sample.cumulative.samplerNs=s.admissionSamplerNs;sample.cumulative.drawNs=s.admissionDrawNs;
  }
  Entry* find(d9::ResourceRef ref,d9::Type type) {
    return registry.matches(ref,type)?&entries[ref.id]:nullptr;
  }
  HandleStats handleStats() const {
    auto result=handles;
    result.liveLogicalHandles=registry.liveCount();
    result.liveQueries=0;
    for(const auto& entry:entries)
      if(registry.matches(entry.ref,d9::Type::EventQuery))++result.liveQueries;
    return result;
  }
  static bool buffer(d9::Type t){return t==d9::Type::VertexBuffer||t==d9::Type::IndexBuffer;}
  bool resolveDraw(const d9::DrawCommand& command,GPU::DrawState& state,bool fixed=false) {
    const auto& refs=command.resources;
    auto vb=find(refs[0],d9::Type::VertexBuffer),ib=find(refs[1],d9::Type::IndexBuffer),t=find(refs[2],d9::Type::Texture);
    auto vs=find(refs[3],d9::Type::VertexShader),ps=find(refs[4],d9::Type::PixelShader),d=find(refs[5],d9::Type::Declaration);
    if(!vb||!d||!vb->initializedPrefix
      ||(refs[2].id?(!t||!t->uploaded||textureUpload.owns(refs[2])):refs[2].generation!=0)
      ||(refs[3].id?!vs:(!fixed||refs[3].generation!=0))
      ||(refs[4].id?!ps:(!fixed||refs[4].generation!=0))
      ||(refs[1].id?(!ib||!ib->initializedPrefix):refs[1].generation!=0))return false;
    state=command.state;state.vertexBuffer=vb->slot;state.indexBuffer=ib?ib->slot:0;
    state.texture=t?t->slot:0;state.vertexShader=vs?vs->slot:0;state.pixelShader=ps?ps->slot:0;state.declaration=d->slot;
    for(uint32_t i=0;i<d9::AdditionalTextureStages;++i) {
      const auto ref=textureStages.textures[i];
      if(!ref.id){state.additionalTextures[i]=0;continue;}
      const auto* entry=find(ref,d9::Type::Texture);
      if(!entry||!entry->uploaded||textureUpload.owns(ref))return false;
      state.additionalTextures[i]=entry->slot;
    }
    return true;
  }
  uint32_t slot(d9::Type type){
    if(!d9::ValidType(type)||type==d9::Type::EventQuery)return 0;
    const uint32_t max=buffer(type)?GPU::BufferSlots:type==d9::Type::Texture?GPU::TextureSlots:type==d9::Type::Declaration?GPU::DeclarationSlots:GPU::ShaderSlots;
    for(uint32_t n=1;n<=max;++n){bool used=false;for(const auto& e:entries)if(registry.matches(e.ref,e.type)&&e.slot==n&&(e.type==type||(buffer(type)&&buffer(e.type))))used=true;
      if(!used)return n;}return 0;
  }
  HRESULT apply(const d9::Header& h,const uint8_t* payload,d9::Packet& result,uint32_t& resultBytes){
    resultBytes=0;
    // Annotations bind only to the very next message, which must be the draw
    // message they declared. Anything else is refused and drops them.
    const bool annotated=annotationsPending;annotationsPending=false;
    if(annotated&&!pendingAnnotations.binds(h))return D3DERR_INVALIDCALL;
    // Keep the actual dispatcher safe for direct CPU tests as well as decoded
    // wire packets. No malformed identity or payload reaches a backend call.
    if(!d9::ValidOp(uint32_t(h.op))||h.payloadBytes>d9::MaxPayload
      ||(h.payloadBytes&&!payload)||h.id>d9::MaxResources
      ||(h.id?!h.generation:h.generation!=0))return D3DERR_INVALIDCALL;
    if(finished)return D3DERR_INVALIDCALL;
    if(!initialized&&h.op==d9::Op::Finish&&!gpu) {
      if(h.id||h.payloadBytes||std::any_of(h.args.begin(),h.args.end(),[](uint32_t v){return v!=0;}))
        return D3DERR_INVALIDCALL;
      // Rejected/never-sent Initialize is a CPU-only close, not GPU teardown.
      uninitializedStats.normalTeardown=true;finished=true;return S_OK;
    }
    if(h.op==d9::Op::FactoryQuery) {
      if(!d9::ValidFactoryQuery(h))return D3DERR_INVALIDCALL;
      if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
      return factory->query(h,result.data()+d9::ReplyBytes,resultBytes);
    }
    if(!initialized&&h.op!=d9::Op::Initialize)return D3DERR_INVALIDCALL;
    auto unused=[&](size_t n){for(;n<h.args.size();++n)if(h.args[n])return false;return true;};
    const d9::ResourceRef ref{h.id,h.generation};
    auto commit=[&](d9::Type type,uint32_t index,uint32_t size,uint32_t w=0,uint32_t height=0){
      // The backend has already succeeded. An impossible commit is terminal,
      // never a recoverable rejection that would let the proxy reuse this ID.
      Require(registry.commitCreate(ref,type),"D3D9 resource create commit invariant");
      Entry entry{};entry.ref=ref;entry.type=type;entry.slot=index;entry.size=size;
      entry.width=w;entry.height=height;entries[ref.id]=entry;
      ++handles.logicalHandlesCreated;
      if(ref.generation>1)++handles.logicalHandlesReused;
      if(type==d9::Type::EventQuery) {
        ++handles.queryHandlesCreated;
        if(ref.generation>1)++handles.queryHandlesReused;
      } else if(ref.generation>1)++handles.resourcesReused;
    };
    switch(h.op){
      case d9::Op::Initialize:{
        d9::Initialization init;
        if(initialized||!d9::DecodeInitialization(h,init))return D3DERR_INVALIDCALL;
        if(init.profile==d9::PresentationProfile::Offscreen&&surfaceMode!=surface::Mode::Peer)
          return D3DERR_INVALIDCALL; // Explicit mode mismatch is not a fallback.
        if(init.profile!=d9::PresentationProfile::Offscreen) {
          d9::RequireIsolatedDesktop();
          const HWND window=d9::DecodeWindow(init.window);DWORD pid=0;
          const auto thread=GetWindowThreadProcessId(window,&pid);RECT client{};
          if(!IsWindow(window)||pid!=peerPid||thread!=init.thread
            ||d9::ObjectName(GetThreadDesktop(thread))!=d9::CurrentDesktop()
            ||!GetClientRect(window,&client)||client.left||client.top
            ||uint32_t(client.right)!=init.width||uint32_t(client.bottom)!=init.height
            ||GetPropW(window,marker.c_str())!=reinterpret_cast<HANDLE>(uintptr_t(1)))return D3DERR_INVALIDCALL;
          // Host-owned surface windows remain at the 64x64 lab extent.
          if(surfaceMode!=surface::Mode::Peer&&(init.width!=d9::Width||init.height!=d9::Height))
            return D3DERR_INVALIDCALL;
          std::printf("{\"schema\":\"warvk-d3d9-desktop/v1\",\"pid\":%lu,\"threadId\":%lu,\"peerPid\":%lu,"
            "\"windowThreadId\":%lu,\"windowStation\":%s,\"desktop\":%s,\"inputDesktop\":%s,\"beforeGpu\":true}\n",
            GetCurrentProcessId(),GetCurrentThreadId(),peerPid,thread,
            d9::NameJson(d9::ObjectName(GetProcessWindowStation())).c_str(),
            d9::NameJson(d9::CurrentDesktop()).c_str(),d9::NameJson(d9::InputDesktop()).c_str());
          std::fflush(stdout);
          presentedReadback=init.profile==d9::PresentationProfile::IsolatedPresenterReadback;
          if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
          gpu=std::make_unique<GPU>();++backendCalls;
          gpu->initialize(init.width,init.height,window,peerPid,thread,marker.c_str(),presentedReadback,peerProcess,surfaceMode,factory.get(),
            init.opaque,init.interval);
          presenterWindow=window;
        } else {
          // Observe the actual GPU owner desktop even for offscreen work. This
          // is evidence only; it does not change the profile's admission rule.
          std::printf("{\"schema\":\"warvk-d3d9-desktop/v1\",\"pid\":%lu,\"threadId\":%lu,\"peerPid\":%lu,"
            "\"windowThreadId\":0,\"windowStation\":%s,\"desktop\":%s,\"inputDesktop\":%s,\"beforeGpu\":true}\n",
            GetCurrentProcessId(),GetCurrentThreadId(),peerPid,
            d9::NameJson(d9::ObjectName(GetProcessWindowStation())).c_str(),
            d9::NameJson(d9::CurrentDesktop()).c_str(),d9::NameJson(d9::InputDesktop()).c_str());
          std::fflush(stdout);
          if(!factory)factory=std::make_unique<dxvkcore::D3D9Factory>();
          gpu=std::make_unique<GPU>();++backendCalls;
          gpu->initialize(init.width,init.height,nullptr,0,0,nullptr,false,nullptr,surface::Mode::Peer,factory.get(),init.opaque,0);
        }
        targetWidth=init.width;targetHeight=init.height;
        retainContent=init.retainContent;annotations=init.annotations;initialized=true;break;
      }
      case d9::Op::CreateBuffer:{
        if(h.payloadBytes||!unused(3)||!h.args[0]||h.args[0]>d9::MaxBuffer||h.args[1]>1||h.args[2]>1
          ||(h.args[1]&&(h.args[0]&1)))return D3DERR_INVALIDCALL;
        const auto type=h.args[1]?d9::Type::IndexBuffer:d9::Type::VertexBuffer;
        if(!registry.canCreate(ref,type))return D3DERR_INVALIDCALL;
        const auto index=slot(type);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(type,h.args[0]);
        ++backendCalls;gpu->createBuffer(index,h.args[0],bool(h.args[1]),bool(h.args[2]));commit(type,index,h.args[0]);
        entries[ref.id].dynamic=h.args[2]!=0;adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::UploadBuffer:{
        auto* e=find(ref,d9::Type::VertexBuffer);if(!e)e=find(ref,d9::Type::IndexBuffer);
        if(!e||!unused(2)||!d9::ValidUploadMode(h.args[1],e->dynamic)||!h.payloadBytes||h.args[0]>e->initializedPrefix
          ||h.args[0]>e->size||h.payloadBytes>e->size-h.args[0])return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->uploadBuffer(e->slot,h.args[0],payload,h.payloadBytes,d9::UploadMode(h.args[1]));
        if(!e->content.empty())std::memcpy(e->content.data()+h.args[0],payload,h.payloadBytes);
        e->initializedPrefix=std::max(e->initializedPrefix,h.args[0]+h.payloadBytes);break;
      }
      case d9::Op::CreateTexture:{
        if(h.payloadBytes||!unused(2)||!h.args[0]||!h.args[1]||h.args[0]>64||h.args[1]>64
          ||!registry.canCreate(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Texture);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(d9::Type::Texture,h.args[0]*h.args[1]*4);
        ++backendCalls;gpu->createTexture(index,h.args[0],h.args[1]);commit(d9::Type::Texture,index,h.args[0]*h.args[1]*4,h.args[0],h.args[1]);
        Require(d9::MakeTextureLayout(h.args[0],h.args[1],1,entries[ref.id].textureLayout),"legacy texture layout");
        adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::UploadTexture:{
        auto* e=find(ref,d9::Type::Texture);if(!e||!unused(0)||h.payloadBytes!=e->size||e->textureLayout.levels!=1||textureUpload.owns(ref))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->uploadTexture(e->slot,payload,h.payloadBytes);e->uploaded=true;e->initializedLevels=1;
        if(!e->content.empty())std::memcpy(e->content.data(),payload,h.payloadBytes);break;
      }
      case d9::Op::CreateTexture2D:{
        d9::TextureLayout layout;
        if(h.payloadBytes||!unused(4)
            ||!d9::MakeTextureLayout(h.args[0],h.args[1],h.args[2],layout,h.args[3])
            ||!registry.canCreate(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        // The original adapter policy must accept the requested format before
        // any GPU allocation or logical resource publication is attempted.
        d9::Header query;query.op=d9::Op::FactoryQuery;
        query.args={uint32_t(d9::FactoryQuery::Format),D3DDEVTYPE_HAL,D3DFMT_X8R8G8B8,0,D3DRTYPE_TEXTURE,layout.format};
        uint8_t ignored[4]{};uint32_t resultBytes=0;
        if(!factory||factory->query(query,ignored,resultBytes)!=D3D_OK)return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Texture);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        auto content=retained(d9::Type::Texture,layout.bytes);
        ++backendCalls;gpu->createTexture2D(index,layout);commit(d9::Type::Texture,index,layout.bytes,layout.width,layout.height);
        entries[ref.id].textureLayout=layout;adopt(entries[ref.id],std::move(content));break;
      }
      case d9::Op::BeginTextureUpload:{
        auto* e=find(ref,d9::Type::Texture);d9::TextureRegion region;
        if(!e||h.payloadBytes||!unused(6)||textureUpload.active()||e->textureRevision==UINT32_MAX
            ||h.args[0]!=e->textureRevision+1
            ||!d9::MakeTextureRegion(e->textureLayout,h.args[1],h.args[2],h.args[3],h.args[4],h.args[5],region)
            ||(!(e->initializedLevels&(1u<<region.level))&&!d9::FullTextureLevel(e->textureLayout,region)))
          return D3DERR_INVALIDCALL;
        if(!textureUpload.begin(ref,h.args[0],region))return D3DERR_INVALIDCALL;
        e->textureRevision=h.args[0];break;
      }
      case d9::Op::TextureUploadChunk:{
        if(!find(ref,d9::Type::Texture)||!unused(2)
            ||!textureUpload.append(ref,h.args[0],h.args[1],payload,h.payloadBytes))return D3DERR_INVALIDCALL;
        break;
      }
      case d9::Op::CommitTextureUpload:{
        auto* e=find(ref,d9::Type::Texture);
        if(!e||h.payloadBytes||!unused(1)||!textureUpload.complete(ref,h.args[0]))return D3DERR_INVALIDCALL;
        const auto region=textureUpload.region();++backendCalls;
        gpu->uploadTextureRegion(e->slot,region,textureUpload.data());
        if(!e->content.empty())d9::PatchTextureLayout(e->textureLayout,region,textureUpload.data(),e->content.data());
        if(d9::FullTextureLevel(e->textureLayout,region))e->initializedLevels|=1u<<region.level;
        e->uploaded=e->initializedLevels==((1u<<e->textureLayout.levels)-1);
        textureUpload.reset();break;
      }
      case d9::Op::AbortTextureUpload:{
        if(!find(ref,d9::Type::Texture)||h.payloadBytes||!unused(1)||!textureUpload.matches(ref,h.args[0]))return D3DERR_INVALIDCALL;
        textureUpload.reset();break;
      }
      case d9::Op::ReadContent:{
        // Retained CPU copy only: no GPU readback, settle, or ordering change.
        // A texture level is readable once initialized and outside an upload.
        auto* e=find(ref,d9::Type::VertexBuffer);if(!e)e=find(ref,d9::Type::IndexBuffer);
        const bool texture=!e;if(texture)e=find(ref,d9::Type::Texture);
        const auto level=h.args[0],offset=h.args[1],count=h.args[2];
        if(!retainContent||!e||e->content.empty()||h.payloadBytes||!unused(3)||!count||count>d9::MaxContentRead)
          return D3DERR_INVALIDCALL;
        uint32_t base=0,limit=e->initializedPrefix;
        if(texture) {
          if(level>=e->textureLayout.levels||!(e->initializedLevels&(1u<<level))||textureUpload.owns(ref))
            return D3DERR_INVALIDCALL;
          base=e->textureLayout.level[level].offset;limit=e->textureLayout.level[level].bytes;
        } else if(level)return D3DERR_INVALIDCALL;
        if(offset>limit||count>limit-offset)return D3DERR_INVALIDCALL;
        std::memcpy(result.data()+d9::ReplyBytes,e->content.data()+base+offset,count);
        resultBytes=count;++contentReads;contentReadBytes+=count;break;
      }
      case d9::Op::TextureLod:{
        auto* e=find(ref,d9::Type::Texture);
        if(!e||h.payloadBytes||!unused(1)||h.args[0]>=e->textureLayout.levels||textureUpload.owns(ref))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setTextureLod(e->slot,h.args[0]);break;
      }
      case d9::Op::CreateShader:{
        if(!unused(1)||h.args[0]>1||(h.payloadBytes&3)||!h.payloadBytes||h.payloadBytes>d3d9tokens::MaxWords*4)return D3DERR_INVALIDCALL;
        std::array<uint32_t,d3d9tokens::MaxWords> words{};for(uint32_t i=0;i<h.payloadBytes/4;++i)words[i]=d9::U32(payload+i*4);
        const auto scan=d3d9tokens::Scan(words.data(),h.payloadBytes/4);
        if(!scan.accepted()||scan.stage!=(h.args[0]?d3d9tokens::Stage::Pixel:d3d9tokens::Stage::Vertex))return D3DERR_INVALIDCALL;
        const auto type=h.args[0]?d9::Type::PixelShader:d9::Type::VertexShader;
        if(!registry.canCreate(ref,type))return D3DERR_INVALIDCALL;
        const auto index=slot(type);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        ++backendCalls;gpu->createShader(index,h.args[0]?GPU::ShaderStage::Pixel:GPU::ShaderStage::Vertex,words.data(),h.payloadBytes/4);
        commit(type,index,h.payloadBytes);break;
      }
      case d9::Op::CreateDeclaration:{
        d9::DeclarationCommand command;
        if(!unused(0)||!registry.canCreate(ref,d9::Type::Declaration)
          ||!d9::DecodeDeclaration(payload,h.payloadBytes,command))return D3DERR_INVALIDCALL;
        const auto index=slot(d9::Type::Declaration);if(!index)return D3DERR_OUTOFVIDEOMEMORY;
        ++backendCalls;gpu->createDeclaration(index,command.elements.data(),command.count);
        commit(d9::Type::Declaration,index,h.payloadBytes);break;
      }
      case d9::Op::Clear:
        if(h.id||h.payloadBytes||!unused(3)||!h.args[2]||(h.args[2]&~uint32_t(D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER))
          ||!std::isfinite(d9::Float(h.args[1]))||d9::Float(h.args[1])<0||d9::Float(h.args[1])>1)return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->clear(h.args[0],d9::Float(h.args[1]),h.args[2]);break;
      case d9::Op::Draw:{
        d9::DrawCommand command;if(h.id||!unused(0)||!d9::DecodeDraw(payload,h.payloadBytes,command))return D3DERR_INVALIDCALL;
        GPU::DrawState s;if(!resolveDraw(command,s))return D3DERR_INVALIDCALL;
        ++backendCalls;
        try { gpu->draw(s); }
        catch (const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        if(annotated)deliverAnnotations(&s);
        break;
      }
      case d9::Op::DrawFixed:{
        d9::DrawCommand command;d9::FixedFunctionState fixed;GPU::DrawState state;
        if(h.id||!unused(0)||!d9::DecodeFixedDraw(payload,h.payloadBytes,command,fixed)
          ||!resolveDraw(command,state,true))return D3DERR_INVALIDCALL;
        ++backendCalls;
        try {gpu->drawFixed(state,fixed);}
        catch(const GPU::ProfileRejected&) {return D3DERR_INVALIDCALL;}
        if(annotated)deliverAnnotations(&state);
        break;
      }
      case d9::Op::SetSamplerState:{
        d9::SamplerState state;
        if(h.id||!unused(0)||!d9::DecodeSamplerState(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setSamplerState(state);break;
      }
      case d9::Op::SetEffects:{
        d9::EffectsState state;
        if(h.id||!unused(0)||!d9::DecodeEffects(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setEffects(state);break;
      }
      case d9::Op::SetTextureStages:{
        d9::TextureStages state;
        if(h.id||!unused(0)||!d9::DecodeTextureStages(payload,h.payloadBytes,state))return D3DERR_INVALIDCALL;
        // Validate the whole identity table before either the samplers or the
        // bindings change. Draw resolves generations again after any Destroy.
        for(const auto ref:state.textures)if(ref.id&&!find(ref,d9::Type::Texture))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->setAdditionalSamplers(state.samplers);textureStages=state;break;
      }
      case d9::Op::DrawBatch:{
        const auto count=h.args[0];
        if(h.id||!unused(1)||!count||count>d9::MaxDrawBatch||h.payloadBytes!=count*d9::DrawBytes)
          return D3DERR_INVALIDCALL;
        std::array<GPU::DrawState,d9::MaxDrawBatch> states{};
        const auto decodeBegin=admissionProfile?Clock::now():Clock::time_point{};
        for(uint32_t i=0;i<count;++i) {
          d9::DrawCommand command;
          if(!d9::DecodeDraw(payload+i*d9::DrawBytes,d9::DrawBytes,command))return D3DERR_INVALIDCALL;
          if(!resolveDraw(command,states[i]))return D3DERR_INVALIDCALL;
        }
        if(admissionProfile)admission.batchDecodeNs+=ElapsedNs(decodeBegin);
        ++backendCalls;
        try { gpu->drawBatch(states.data(),count); }
        catch(const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        admission.batchDraws+=count;
        if(annotated)deliverAnnotations(states.data());
        break;
      }
      case d9::Op::DrawFixedBatch:{
        const auto count=h.args[0];
        if(h.id||!unused(1)||!count||count>d9::MaxFixedDrawBatch||h.payloadBytes!=count*d9::FixedDrawBytes)
          return D3DERR_INVALIDCALL;
        const auto decodeBegin=admissionProfile?Clock::now():Clock::time_point{};
        for(uint32_t i=0;i<count;++i) {
          d9::DrawCommand command;
          if(!d9::DecodeFixedDraw(payload+i*d9::FixedDrawBytes,d9::FixedDrawBytes,command,fixedBatchFixed[i])
            ||!resolveDraw(command,fixedBatchStates[i],true))return D3DERR_INVALIDCALL;
        }
        if(admissionProfile)admission.batchDecodeNs+=ElapsedNs(decodeBegin);
        ++backendCalls;
        try { gpu->drawFixedBatch(fixedBatchStates.data(),fixedBatchFixed.data(),count); }
        catch(const GPU::ProfileRejected&) { return D3DERR_INVALIDCALL; }
        admission.batchDraws+=count;
        ++fixedBatches;fixedBatchDraws+=count;largestFixedBatch=std::max(largestFixedBatch,count);
        if(annotated)deliverAnnotations(fixedBatchStates.data());
        break;
      }
      case d9::Op::AnnotateDraws:
        if(!annotations||!d9::DecodeDrawAnnotations(h,payload,pendingAnnotations))return D3DERR_INVALIDCALL;
        annotationsPending=true;break;
      case d9::Op::FrameScene:
        if(!annotations||h.id||!unused(1)||!d9::ValidFrameScene(h.args[0],payload,h.payloadBytes))return D3DERR_INVALIDCALL;
        annotationRecorder.scene(h.args[0],payload,h.payloadBytes);break;
      case d9::Op::StageMarker:
        if(!annotations||h.id||h.payloadBytes||!unused(1)||!d9::ValidStage(h.args[0]))return D3DERR_INVALIDCALL;
        annotationRecorder.stage(h.args[0]);break;
      case d9::Op::ClearRegions:{
        d9::ClearRegions regions;
        if(!d9::DecodeClearRegions(h,payload,regions,targetWidth,targetHeight))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->clearRegions(regions);break;
      }
      case d9::Op::FlushDraws:
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->flushDraws();break;
      case d9::Op::Readback:{
        uint32_t first=h.args[0],rows=h.args[1];
        if(h.id||h.payloadBytes||!unused(5)||!d9::ResolveReadbackRows(targetWidth,targetHeight,first,rows))return D3DERR_INVALIDCALL;
        const auto source=d9::ReadbackSource(h.args[2]);
        const auto frame=uint64_t(h.args[3])|(uint64_t(h.args[4])<<32);
        std::vector<uint8_t> bytes;
        if(source==d9::ReadbackSource::Target) {
          if(h.args[3]||h.args[4])return D3DERR_INVALIDCALL;
          ++backendCalls;bytes=gpu->readback(first,rows);
        } else if(source==d9::ReadbackSource::Presented) {
          // Bands of the one frozen diagnostic frame that the last Present
          // reported; an old frame number is refused, never served.
          if(!presentedReadback||!presentedCurrent||!frame||frame!=gpu->stats().presentedFrame)return D3DERR_INVALIDCALL;
          ++backendCalls;bytes=gpu->readPresented(first,rows);
        } else return D3DERR_INVALIDCALL;
        Require(bytes.size()==size_t(targetWidth)*rows*4,"actual readback band extent");
        resultBytes=uint32_t(bytes.size());std::memcpy(result.data()+d9::ReplyBytes,bytes.data(),bytes.size());break;
      }
      case d9::Op::CreateQuery:{
        if(h.payloadBytes||!unused(0)||!registry.canCreate(ref,d9::Type::EventQuery))return D3DERR_INVALIDCALL;
        // A Query is a logical observation handle, not a GPU resource. Its
        // bounded entry is committed without fabricating a backend allocation.
        commit(d9::Type::EventQuery,0,0);break;
      }
      case d9::Op::Issue:{
        auto* e=find(ref,d9::Type::EventQuery);
        const auto revision=uint64_t(h.args[0])|(uint64_t(h.args[1])<<32);
        if(!e||h.payloadBytes||!unused(2)||!revision||e->queryRevision==UINT64_MAX
          ||revision!=e->queryRevision+1||handles.queryIssues==UINT64_MAX)return D3DERR_INVALIDCALL;
        ++backendCalls;const auto serial=gpu->issueEvent();Require(serial!=0,"GPU event serial");
        // A repeated Issue may observe the SAME GPU serial. Only its own
        // monotonically increasing revision replaces the old observation.
        e->queryRevision=revision;e->querySerial=serial;++handles.queryIssues;
        resultBytes=16;d9::P64(result.data()+d9::ReplyBytes,revision);d9::P64(result.data()+d9::ReplyBytes+8,serial);break;
      }
      case d9::Op::Poll:
      case d9::Op::WaitEvent:{
        auto* e=find(ref,d9::Type::EventQuery);
        const auto revision=uint64_t(h.args[0])|(uint64_t(h.args[1])<<32);
        const auto serial=uint64_t(h.args[2])|(uint64_t(h.args[3])<<32);
        if(!e||h.payloadBytes||!unused(4)||!revision||!serial
          ||revision!=e->queryRevision||serial!=e->querySerial||handles.queryPolls==UINT64_MAX
          ||handles.queryWaits==UINT64_MAX)return D3DERR_INVALIDCALL;
        if(h.op==d9::Op::WaitEvent) {
          // This command waits only for an already-issued prefix; no later
          // message is needed to make it progress. The client worker owns it.
          ++backendCalls;gpu->waitEvent(serial);++handles.queryWaits;break;
        }
        ++backendCalls;const bool complete=gpu->eventComplete(serial);++handles.queryPolls;
        resultBytes=4;d9::P32(result.data()+d9::ReplyBytes,complete?1:0);break;
      }
      case d9::Op::Destroy:{
        const auto type=d9::Type(h.args[0]);
        if(h.payloadBytes||!unused(1)||!d9::ValidType(type))return D3DERR_INVALIDCALL;
        auto* e=find(ref,type);if(!e)return D3DERR_INVALIDCALL;
        if(type!=d9::Type::EventQuery) {
          ++backendCalls;gpu->destroy(GPU::ResourceType(h.args[0]-1),e->slot);
        }
        // GPU resources are tombstoned only AFTER the original retirement path
        // succeeds. Query destruction only forgets its observation; no cancel.
        Require(registry.destroy(ref,type),"D3D9 resource destroy commit invariant");
        if(textureUpload.owns(ref))textureUpload.reset();
        retainedBytes-=e->content.size();
        *e=Entry{};++handles.logicalHandlesDestroyed;
        if(type==d9::Type::EventQuery)++handles.queryHandlesDestroyed;
        break;
      }
      case d9::Op::Present:{
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        // Offscreen remains unsupported and retains its old command counts.
        if(!gpu->stats().presenterImplemented)return E_NOTIMPL;
        ++backendCalls;const auto hr=gpu->present();
        if(hr==D3D_OK&&presentedReadback) {
          presentedCurrent=true;
          // The frame number, plus the whole frozen image when it fits; larger
          // targets read their bands with Readback(source=Presented, frame).
          d9::P64(result.data()+d9::ReplyBytes,gpu->stats().presentedFrame);resultBytes=8;
          if(d9::PresentedInline(targetWidth,targetHeight)) {
            const auto bytes=gpu->readPresented(0,targetHeight);
            Require(bytes.size()==size_t(targetWidth)*targetHeight*4,"actual presented image extent");
            resultBytes=8+uint32_t(bytes.size());std::memcpy(result.data()+d9::ReplyBytes+8,bytes.data(),bytes.size());
          }
        }
        return hr;
      }
      case d9::Op::Reset:{
        d9::Resize resize;
        const bool presenter=gpu->stats().presenterImplemented;
        if(!d9::DecodeReset(h,presenter,resize))return D3DERR_INVALIDCALL;
        // D3D9 Reset needs every DEFAULT-pool resource released first; buffers
        // are the only ones in this profile. The window's own resize keeps
        // them. Nothing half-created or uploading either way.
        if(!resize.keepResources)
          for(const auto& entry:entries)if(buffer(entry.type)&&registry.matches(entry.ref,entry.type))return D3DERR_INVALIDCALL;
        if(registry.reservedCount()||textureUpload.active())return D3DERR_INVALIDCALL;
        if(presenter) {
          // The client must already have the new extent; host-owned surfaces stay 64x64.
          RECT client{};
          if(!GetClientRect(presenterWindow,&client)||client.left||client.top
            ||uint32_t(client.right)!=resize.width||uint32_t(client.bottom)!=resize.height
            ||(surfaceMode!=surface::Mode::Peer&&(resize.width!=d9::Width||resize.height!=d9::Height)))
            return D3DERR_INVALIDCALL;
        }
        ++backendCalls;gpu->reset(resize.width,resize.height,resize.interval,resize.opaque,resize.keepResources);
        targetWidth=resize.width;targetHeight=resize.height;presentedCurrent=false;break;
      }
      case d9::Op::Finish:
        if(h.id||h.payloadBytes||!unused(0))return D3DERR_INVALIDCALL;
        if(registry.liveCount()!=0||registry.reservedCount()!=0)return D3DERR_INVALIDCALL;
        ++backendCalls;gpu->finish();finished=true;break;
      case d9::Op::DiagnosticCheckpoint: {
        if(gpu->hasPendingDraws()||textureUpload.active())return D3DERR_INVALIDCALL;
        if(h.id||h.payloadBytes||!unused(2)||!d9::memory::ValidPhase(h.args[0])
          ||(h.args[0]==uint32_t(d9::memory::Phase::DeviceReady)?h.args[1]!=0:h.args[1]==0))
          return D3DERR_INVALIDCALL;
        // A checkpoint is an explicit observation, not a backend render call.
        // Do not settle GPU work or treat the CPU reply as GPU completion.
        const auto begin=GetTickCount64();auto sample=gpu->diagnosticMemory();
        sample.phase=h.args[0];sample.ordinal=h.args[1];sample.pid=GetCurrentProcessId();
        sample.thread=GetCurrentThreadId();sample.creation=lab::CreationTime(GetCurrentProcess());
        sample.nonceLow=h.nonce.low;sample.nonceHigh=h.nonce.high;sample.sequence=h.sequence;
        sample.tickBegin=begin;sample.fixedPacketPayloadBytes=2ull*sizeof(d9::Packet);
        sample.process=lab::ProbeOwnProcessMemory();sample.tickEnd=GetTickCount64();
        const auto encoded=d9::memory::EncodeHost(sample);resultBytes=uint32_t(encoded.size());
        std::memcpy(result.data()+d9::ReplyBytes,encoded.data(),encoded.size());
        // Exact bounded wire mirror for independent host -> proxy comparison.
        // This is diagnostic text, never a process-memory or image dump.
        std::printf("{\"schema\":\"warvk-d3d9-memory-host/v1\",\"phase\":%u,\"ordinal\":%u,\"sequence\":%llu,\"wireHex\":\"",
          sample.phase,sample.ordinal,sample.sequence);
        for(const auto byte:encoded)std::printf("%02x",unsigned(byte));
        std::printf("\"}\n");std::fflush(stdout);
        // Independent versioned cold observation, not new reserved wire bits.
        // Existing M0 author-storage accounts do NOT include device-lifetime
        // compiler/pipeline caches. Keep their identities and scopes separate.
        const auto cache=gpu->diagnosticShaderCache();
        std::printf("D3D9 shader-cache: {\"schema\":\"warvk-d3d9-shader-cache/v1\","
          "\"pid\":%u,\"thread\":%u,\"creation\":%llu,\"nonceLow\":%llu,\"nonceHigh\":%llu,"
          "\"sequence\":%llu,\"phase\":%u,\"ordinal\":%u,"
          "\"requests\":%llu,\"hits\":%llu,\"compileAttempts\":%llu,\"compilesSucceeded\":%llu,"
          "\"registrations\":%llu,\"raceLosers\":%llu,\"validationFailures\":%llu,"
          "\"registrationFailures\":%llu,\"readyEntries\":%llu,\"failedEntries\":%llu,"
          "\"keyBytes\":%llu,\"keyCapacityBytes\":%llu,\"constantBytes\":%llu,\"constantCapacityBytes\":%llu,"
          "\"vertexCookies\":[",
          sample.pid,sample.thread,sample.creation,sample.nonceLow,sample.nonceHigh,
          sample.sequence,sample.phase,sample.ordinal,
          cache.requests,cache.hits,cache.compileAttempts,cache.compilesSucceeded,
          cache.registrations,cache.raceLosers,cache.validationFailures,
          cache.registrationFailures,cache.readyEntries,cache.failedEntries,
          cache.keyBytes,cache.keyCapacityBytes,cache.constantBytes,cache.constantCapacityBytes);
        for(size_t i=0;i<cache.vertexCookies.size();++i)
          std::printf("%s%llu",i?",":"",cache.vertexCookies[i]);
        std::printf("],\"pixelCookies\":[");
        for(size_t i=0;i<cache.pixelCookies.size();++i)
          std::printf("%s%llu",i?",":"",cache.pixelCookies[i]);
        std::printf("],\"includesPipelineOrDriverBytes\":false}\n");std::fflush(stdout);
        break;
      }
    }
    return S_OK;
  }
};
}
int wmain(int argc,wchar_t**argv){
  const auto processStart=GetTickCount64();
  MainExitWitness finalLocals{processStart};
  std::unique_ptr<Session> session;
  try{
    Require(argc==4,"nonce parent-pid parent-creation");const auto nonce=lab::ParseNonce(argv[1]);const auto pid=std::stoull(argv[2]);
    Require(pid>0&&pid<=UINT32_MAX,"parent PID range");auto parent=lab::OpenPeer(DWORD(pid),std::stoull(argv[3]));
    const auto surfaceMode=surface::ReadStartupMode();
    std::fprintf(stderr,"D3D9 host dev surface-owner mode=%s productIntegrated=false\n",surface::ModeName(surfaceMode));
    std::fflush(stderr);
    auto pipe=lab::CreateServer(nonce,lab::Channel::D3D9Dev5);lab::IoStats io;lab::Accept(pipe.get(),parent.get(),io);lab::VerifyPeer(pipe.get(),true,DWORD(pid));
    Require(!GetModuleHandleW(L"Game.dll"),"game forbidden");
    session=std::make_unique<Session>(DWORD(pid),nonce,parent.get(),surfaceMode);
    wchar_t admissionMode[8]{};
    const DWORD admissionLength=GetEnvironmentVariableW(L"WARVK_D3D9_ADMISSION_PROFILE",admissionMode,8);
    Require(!admissionLength||(admissionLength==1&&admissionMode[0]==L'1'),"unknown admission diagnostic mode");
    session->admissionProfile=admissionLength!=0;
    const bool profile=session->admissionProfile;auto& admission=session->admission;
    d9::Packet request{},response{};uint64_t sequence=1;bool initializationLogged=false;
    while(!session->finished){
      // Only D3D9dev opts in: a live idle producer is not a truncated frame.
      // The first received byte starts the unchanged shared 5 s header/body
      // window, so sending one byte cannot hold an incomplete frame forever.
      const auto idleBegin=profile?Clock::now():Clock::time_point{};
      lab::ReadFirstByteAfterIdle(pipe.get(),parent.get(),request.data(),io);
      const auto readBegin=profile?Clock::now():Clock::time_point{};
      const lab::ReadWindow deadline;lab::ReadExact(pipe.get(),parent.get(),request.data()+1,d9::HeaderBytes-1,deadline,lab::Stage::Header,io);
      d9::Header h;Require(d9::DecodeHeader(request.data(),h),"D3D9 header invalid");
      Require(h.nonce==nonce&&h.sequence==sequence&&sequence!=UINT64_MAX,"D3D9 scope/sequence");
      lab::ReadExact(pipe.get(),parent.get(),request.data()+d9::HeaderBytes,h.payloadBytes,deadline,lab::Stage::Body,io);
      const auto applyBegin=profile?Clock::now():Clock::time_point{};
      const bool batchOp=h.op==d9::Op::DrawBatch||h.op==d9::Op::DrawFixedBatch;
      if(profile) {
        const auto read=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(applyBegin-readBegin).count());
        admission.idleNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(readBegin-idleBegin).count());
        admission.readNs+=read;
        if(batchOp)admission.batchReadNs+=read;
      }
      uint32_t bytes=0;HRESULT hr=E_UNEXPECTED;
      if(h.op==d9::Op::Initialize&&!initializationLogged) {
        initializationLogged=true;const auto initStart=GetTickCount64();
        std::fprintf(stderr,"D3D9 host init-apply begin pid=%lu sequence=%llu processElapsedMs=%llu\n",
          GetCurrentProcessId(),h.sequence,initStart-processStart);std::fflush(stderr);
        try { hr=session->apply(h,request.data()+d9::HeaderBytes,response,bytes); }
        catch(...) {
          std::fprintf(stderr,"D3D9 host init-apply exception elapsedMs=%llu\n",GetTickCount64()-initStart);
          std::fflush(stderr);throw;
        }
        std::fprintf(stderr,"D3D9 host init-apply complete result=%lu elapsedMs=%llu\n",DWORD(hr),GetTickCount64()-initStart);
        std::fflush(stderr);
      } else hr=session->apply(h,request.data()+d9::HeaderBytes,response,bytes);
      const auto replyBegin=profile?Clock::now():Clock::time_point{};
      if(profile) {
        const auto applied=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(replyBegin-applyBegin).count());
        ++admission.messages;
        if(batchOp){++admission.batchMessages;admission.batchApplyNs+=applied;}
        else if(h.op==d9::Op::FlushDraws)admission.flushApplyNs+=applied;
        else if(h.op==d9::Op::WaitEvent)admission.waitEventApplyNs+=applied;
        else admission.otherApplyNs+=applied;
      }
      if(SUCCEEDED(hr))++session->accepted;else ++session->rejected;
      const auto&s=session->stats();d9::Reply r{uint32_t(hr),s.submitted,s.gpuCompleted,s.retired,s.draws,bytes};
      d9::EncodeReply(h,r,response);lab::WriteBytes(pipe.get(),parent.get(),response.data(),d9::ReplyBytes+bytes,io);++sequence;
      if(profile) {
        const auto replied=ElapsedNs(replyBegin);
        admission.replyNs+=replied;
        if(batchOp)admission.batchReplyNs+=replied;
        if(h.op==d9::Op::Issue&&SUCCEEDED(hr))session->sampleAdmission();
      }
      if(h.op==d9::Op::Finish)ExitMilestone("finish-reply-written",processStart);
      if(hr==D3DERR_DEVICELOST) {
        std::printf("{\"schema\":\"warvk-d3d9-host-fault/v1\",\"ok\":false,\"fault\":\"gpu-or-window-terminal\","
          "\"faultKind\":\"%s\",\"gpuCleanupCompleted\":%s,\"gpuCleanupMs\":%llu,\"exitStrategy\":\"%s\","
          "\"shutdownQueueDrained\":%s,"
          "\"result\":%lu,\"quarantined\":%s,\"normalTeardown\":false,\"gpuInitialized\":%s,"
          "\"bits\":64,\"actualDxvkCore\":true,\"actualDxso\":true,\"accepted\":%llu,\"rejected\":%llu,"
          "\"backendCalls\":%llu,\"presentAttempts\":%llu,\"presentApiSucceeded\":%llu,\"presentRetired\":%llu,"
          "\"acquiredImages\":%llu,\"presentQueued\":%llu,\"presentGpuCompleted\":%llu,"
          "\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,\"draws\":%llu,\"clears\":%llu,"
          "\"liveResources\":%u,\"liveLogicalBytes\":%llu,"
          "\"windowProcessId\":%u,\"windowThreadId\":%u,"
          "\"presentedReadbacks\":%llu,\"presentedFrame\":%llu,",
          s.windowPreflightFailed?"window-preflight":"gpu-or-window-terminal",s.faultCleanupCompleted?"true":"false",
          s.faultCleanupMs,s.faultCleanupCompleted?"owner-cleanup-return":"self-fail-stop",
          s.shutdownQueueDrained?"true":"false",
          DWORD(hr),s.quarantined?"true":"false",s.initialized?"true":"false",session->accepted,session->rejected,session->backendCalls,
          s.presentAttempts,s.presentApiSucceeded,s.presentRetired,s.acquiredImages,s.presentQueued,s.presentGpuCompleted,
          s.submitted,s.gpuCompleted,s.retired,s.draws,s.clears,s.liveResources,s.liveLogicalBytes,
          s.windowProcessId,s.windowThreadId,s.presentedReadbacks,s.presentedFrame);
        session->handleStats().print();
        std::printf("\"productIntegrated\":false,\"gameTouched\":false,\"memorySavingsProven\":false}\n");
        std::fflush(stdout);std::fflush(stderr);
        if(s.faultCleanupCompleted) {
          session.reset();LogExitIntent("owner-cleanup-return",2,processStart);return 2;
        }
        (void)session.release();SelfFailStop(processStart);
      }
    }
    const auto s=session->stats();const auto accepted=session->accepted,rejected=session->rejected,calls=session->backendCalls;
    const auto handles=session->handleStats();
    Require(s.normalTeardown,"normal DXVK teardown");
    std::fprintf(stderr,"D3D9 draw-batch-host: {\"schema\":\"warvk-d3d9-draw-batch-host/v1\","
      "\"batches\":%llu,\"batchedDraws\":%llu,\"largestBatch\":%u,\"constantCapacityBytes\":%llu,"
      "\"draws\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,"
      "\"flightCapacity\":%u,\"flightSubmissions\":%llu,\"flightReuses\":%llu,"
      "\"flightRetirements\":%llu,\"maxOccupiedFlightSlots\":%u,\"submissionGroupCapacity\":%u,"
      "\"groupedSubmissions\":%llu,\"nonIndexedDraws\":%llu,\"batchStateReuse\":%u,\"redundantStateBindingsSkipped\":%llu}\n",
      s.drawBatches,s.batchedDraws,s.largestDrawBatch,s.batchConstantBytes,s.draws,s.submitted,s.gpuCompleted,s.retired,
      s.flightCapacity,s.flightSubmissions,s.flightReuses,s.flightRetirements,s.maxOccupiedFlightSlots,
      s.submissionGroupCapacity,s.groupedSubmissions,s.nonIndexedDraws,s.batchStateReuse,s.redundantStateBindingsSkipped);
    // Separate versioned line: the frozen v1 batch schema above is unchanged.
    if (s.streamMode)
      std::fprintf(stderr,"D3D9 stream-host: {\"schema\":\"warvk-d3d9-stream-host/v1\","
        "\"streamDraws\":%llu,\"streamFixedDraws\":%llu,\"streamSubmissions\":%llu,\"streamCapSubmissions\":%llu,"
        "\"largestStreamList\":%u,\"maxStreamDraws\":%u,\"maxStreamLatency\":%u,\"constantRenames\":%llu,"
        "\"latencyWaits\":%llu,\"ringBytes\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu}\n",
        s.streamDraws,s.streamFixedDraws,s.streamSubmissions,s.streamCapSubmissions,s.largestStreamList,
        GPU::MaxStreamDraws,GPU::MaxStreamLatency,s.streamConstantRenames,s.streamLatencyWaits,s.batchConstantBytes,
        s.submitted,s.gpuCompleted,s.retired);
    // Printed for every profile: zero when no dynamic buffer was written.
    std::fprintf(stderr,"D3D9 dynamic-buffer-host: {\"schema\":\"warvk-d3d9-dynamic-buffer-host/v1\","
      "\"dynamicBuffers\":%llu,\"orderedUploads\":%llu,\"noOverwriteUploads\":%llu,\"discardUploads\":%llu,"
      "\"discardRenames\":%llu,\"discardCarriedBytes\":%llu,\"uploadSettles\":%llu}\n",
      s.dynamicBuffers,s.orderedDynamicUploads,s.noOverwriteUploads,s.discardUploads,
      s.discardRenames,s.discardCarriedBytes,s.uploadSettles);
    // Printed for every profile: zero resets and skips when unused. Window
    // resets are the resets of the window's own resize, which kept every resource.
    std::fprintf(stderr,"D3D9 reset-host: {\"schema\":\"warvk-d3d9-reset-host/v1\",\"resets\":%llu,"
      "\"windowResets\":%llu,\"presentsSkipped\":%llu,\"width\":%u,\"height\":%u,\"opaqueTarget\":%s,\"syncInterval\":%u}\n",
      s.resets,s.windowResets,s.presentsSkipped,s.width,s.height,s.opaqueTarget?"true":"false",s.syncInterval);
    // Accepted DrawFixedBatch messages; printed for every profile, zero if unused.
    std::fprintf(stderr,"D3D9 fixed-batch-host: {\"schema\":\"warvk-d3d9-fixed-batch-host/v1\","
      "\"batches\":%llu,\"draws\":%llu,\"largestBatch\":%u,\"capacity\":%u}\n",
      session->fixedBatches,session->fixedBatchDraws,session->largestFixedBatch,d9::MaxFixedDrawBatch);
    std::fprintf(stderr,"D3D9 backing-host: {\"schema\":\"warvk-d3d9-backing-host/v1\",\"retainContent\":%s,"
      "\"retainedBytes\":%llu,\"peakRetainedBytes\":%llu,\"contentReads\":%llu,\"contentReadBytes\":%llu}\n",
      session->retainContent?"true":"false",session->retainedBytes,session->peakRetainedBytes,
      session->contentReads,session->contentReadBytes);
    // Only a negotiated session prints its annotation account.
    if(session->annotations) {
      const auto& a=session->annotationRecorder;
      std::fprintf(stderr,"D3D9 annotation-host: {\"schema\":\"warvk-d3d9-annotation-host/v1\",\"annotatedDraws\":%llu,"
        "\"drawRecords\":%llu,\"frameScenes\":%llu,\"stageMarkers\":%llu,\"digest\":\"%016llx\"}\n",
        a.annotatedDraws,a.drawRecords,a.frameScenes,a.stageMarkers,a.digest);
    }
    if (s.reuseWaitProfile) {
      for (uint32_t i = 0; i < s.reuseWaitSampleCount; ++i) {
        const auto& sample = s.reuseWaitSamples[i];
        const auto& wait = sample.cumulative;
        std::fprintf(stderr, "D3D9 reuse-wait: {\"schema\":\"warvk-d3d9-reuse-wait/v1\","
          "\"query\":%llu,\"draws\":%llu,\"episodes\":%llu,\"gpuCalls\":%llu,"
          "\"totalNs\":%llu,\"cpuSubmissionNs\":%llu,\"gpuWaitNs\":%llu,\"retirementNs\":%llu,"
          "\"dropped\":%u}\n", sample.query, sample.draws, wait.episodes, wait.gpuCalls,
          wait.totalNs, wait.cpuSubmissionNs, wait.gpuWaitNs, wait.retirementNs, s.reuseWaitSamplesDropped);
      }
    }
    for(uint32_t i=0;i<session->admissionSampleCount;++i) {
      const auto& sample=session->admissionSamples[i];const auto& a=sample.cumulative;
      std::fprintf(stderr,"D3D9 admission-host: {\"schema\":\"warvk-d3d9-admission-host/v1\",\"query\":%llu,"
        "\"messages\":%llu,\"idleNs\":%llu,\"readNs\":%llu,\"replyNs\":%llu,\"batchMessages\":%llu,\"batchDraws\":%llu,"
        "\"batchReadNs\":%llu,\"batchDecodeNs\":%llu,\"batchApplyNs\":%llu,\"batchReplyNs\":%llu,"
        "\"prepareNs\":%llu,\"recordNs\":%llu,\"constantNs\":%llu,\"bindNs\":%llu,\"samplerNs\":%llu,\"drawNs\":%llu,"
        "\"flushApplyNs\":%llu,\"waitEventApplyNs\":%llu,\"otherApplyNs\":%llu,\"dropped\":%u}\n",
        sample.query,a.messages,a.idleNs,a.readNs,a.replyNs,a.batchMessages,a.batchDraws,a.batchReadNs,a.batchDecodeNs,
        a.batchApplyNs,a.batchReplyNs,a.prepareNs,a.recordNs,a.constantNs,a.bindNs,a.samplerNs,a.drawNs,a.flushApplyNs,a.waitEventApplyNs,a.otherApplyNs,
        session->admissionSamplesDropped);
    }
    ExitMilestone("before-session-reset",processStart);session.reset();
    ExitMilestone("after-session-reset",processStart);
    std::printf("{\"schema\":\"warvk-d3d9-host/v1\",\"ok\":true,\"bits\":64,\"actualDxvkCore\":true,\"actualDxso\":true,"
      "\"accepted\":%llu,\"rejected\":%llu,\"backendCalls\":%llu,\"submitted\":%llu,\"gpuCompleted\":%llu,\"retired\":%llu,"
      "\"draws\":%llu,\"clears\":%llu,\"uploads\":%llu,\"uploadedBytes\":%llu,\"readbacks\":%llu,\"queries\":%llu,"
      "\"buffersCreated\":%llu,\"texturesCreated\":%llu,\"shadersCompiled\":%llu,\"declarationsCreated\":%llu,"
      "\"liveResources\":%u,\"liveLogicalBytes\":%llu,\"peakLogicalBytes\":%llu,\"vendorId\":%u,\"deviceId\":%u,"
      "\"normalTeardown\":true,\"presentImplemented\":%s,\"presentAttempts\":%llu,\"acquiredImages\":%llu,"
      "\"presentQueued\":%llu,\"presentApiSucceeded\":%llu,\"presentGpuCompleted\":%llu,\"presentRetired\":%llu,"
      "\"lastAcquireVkResult\":%d,\"lastPresentVkResult\":%d,\"windowProcessId\":%u,\"windowThreadId\":%u,"
      "\"gpuInitialized\":%s,\"shutdownQueueDrained\":%s,"
      "\"presentedReadbacks\":%llu,\"presentedFrame\":%llu,",
      accepted,rejected,calls,s.submitted,s.gpuCompleted,s.retired,s.draws,s.clears,s.uploads,s.uploadedBytes,s.readbacks,s.queries,
      s.buffersCreated,s.texturesCreated,s.shadersCompiled,s.declarationsCreated,s.liveResources,s.liveLogicalBytes,s.peakLogicalBytes,s.vendorId,s.deviceId,
      s.presenterImplemented?"true":"false",s.presentAttempts,s.acquiredImages,s.presentQueued,s.presentApiSucceeded,
      s.presentGpuCompleted,s.presentRetired,s.lastAcquireVkResult,s.lastPresentVkResult,s.windowProcessId,s.windowThreadId,
      s.initialized?"true":"false",s.shutdownQueueDrained?"true":"false",s.presentedReadbacks,s.presentedFrame);
    handles.print();
    std::printf("\"productIntegrated\":false,\"gameTouched\":false,\"memorySavingsProven\":false}\n");
    LogExitIntent("normal-return",0,processStart);return 0;
  }catch(const std::exception&e){(void)session.release();std::fprintf(stderr,"D3D9 host failure: %s\n",e.what());SelfFailStop(processStart);}
  catch(...){(void)session.release();std::fprintf(stderr,"D3D9 host failure: unknown exception\n");SelfFailStop(processStart);}
}
