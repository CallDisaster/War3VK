#include "war3_frame_inputs.h"
#include "war3_frame_inputs_core.h"
#include "war3_frame_recorder_memory.h"
#include "war3_frame_recorder_profile.h"
#include "war3_frame_evidence_control.h"
#include "../render/war3_tracked_vk_pipeline.h"
#include <war3_frame_input_gather.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace dxvk::war3::tools::evidence {
namespace {
using json=nlohmann::json;
using namespace inputs;
std::mutex registryMutex;
std::weak_ptr<InputCapture> registry;
std::string utf8(const std::wstring& s) {
  int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
  std::string out(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;
}
struct File {
  HANDLE h=INVALID_HANDLE_VALUE;
  explicit File(const std::wstring& p):h(CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,
      nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)){}
  ~File(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
  bool write(const void* p,size_t bytes){DWORD n=0;return h!=INVALID_HANDLE_VALUE&&bytes<=MAXDWORD&&
      WriteFile(h,p,DWORD(bytes),&n,nullptr)&&n==bytes;}
  bool finish(){if(h==INVALID_HANDLE_VALUE)return false;bool ok=FlushFileBuffers(h);ok=CloseHandle(h)&&ok;h=INVALID_HANDLE_VALUE;return ok;}
};
struct Span {
  uint64_t handle=0,sourceOffset=0,bytes=0,owner=0,allocation=0,generation=0;
  uint64_t sourceBytes=0;
  uint32_t offset=0,status=Absent,usage=0,encoding=0,usageProof=0;
};
struct Draw {
  uint32_t index=0;
  std::array<uint32_t,40> words{};
  std::array<uint64_t,16> provenance{};
  war3::render::skin::Selection skinSelection{};
  uint64_t part=0,metadata=0,geometry=0,alphaFrame=0,boundsGeneration=0;
  std::array<Span,7> spans{}; // position,index,blend,uv,matrices,direct source,direct palette
  std::array<uint32_t,16> world{};
};
static_assert(sizeof(Draw)<=MaxDrawMetadataBytes,"update recorder host-payload admission with metadata layout");
json spanJson(const Span& s){return {{"buffer",std::to_string(s.handle)},
  {"sourceOffset",std::to_string(s.sourceOffset)},{"bytes",std::to_string(s.bytes)},
  {"owner",std::to_string(s.owner)},{"allocation",std::to_string(s.allocation)},
  {"generation",std::to_string(s.generation)},{"offset",s.offset},{"status",s.status},{"usage",s.usage},
  {"sourceBytes",std::to_string(s.sourceBytes)},{"encoding",s.encoding},{"usageProof",s.usageProof}};}
bool shape(const War3ShadowCasterDraw& d){return inputs::focus(uint32_t(d.stage),d.numVertices,d.indexCount);}
}

struct InputCapture::Impl {
  struct Slot {
    Rc<DxvkBuffer> buffer;
    Rc<DxvkFence> fence;
    uint64_t fenceValue=0,session=0,id=0,qpc=0,renderSerial=0,sceneKey=0,upload=0;
    Key key{};bool volume=false,finalized=false;
    uint32_t used=0,eligible=0,omitted=0,focusCount=0,objectBase=0;
    std::vector<Draw> draws;
  };
  Rc<DxvkDevice> device;
  std::mutex mutex;
  std::array<Slot,Slots> slots;
  const uint32_t slotCount;
  uint64_t serial=0;
  std::atomic<uint64_t> drops{0};
  const char* allocationFailure=nullptr; // owner mutex; diagnostic-only fail-stop
  const DxvkPipelineLayout* gatherLayout=nullptr;
  Rc<war3::render::War3TrackedVkPipeline> gatherPipeline;
  explicit Impl(const Rc<DxvkDevice>& d):device(d),
    slotCount(DefaultRecorderProfile(InternalRecorderBuild()).inputSlots){}
  bool complete(const Slot& s,uint64_t& observed){
    observed=0;if(!s.fenceValue)return true;
    return device->getDeviceStatus()==VK_SUCCESS&&s.fence&&
      device->vkd()->vkGetSemaphoreCounterValue(device->vkd()->device(),s.fence->handle(),&observed)==VK_SUCCESS&&observed>=s.fenceValue;
  }
};
InputCapture::InputCapture(const Rc<DxvkDevice>& d):m(new Impl(d)){}
InputCapture::~InputCapture()=default;
std::shared_ptr<InputCapture> InputCapture::Create(const Rc<DxvkDevice>& d){
  std::lock_guard<std::mutex> lock(registryMutex);
  if(auto existing=registry.lock())return {}; // multiple owners must not silently merge
  auto p=std::shared_ptr<InputCapture>(new InputCapture(d));registry=p;RegisterInputExporter(ExportInputs);return p;
}

uint64_t InputCapture::capture(const Rc<DxvkCommandList>& ctx,Key key,bool volume,
    uint64_t renderSerial,const std::vector<const War3ShadowCasterDraw*>& draws,
    const std::vector<uint32_t>& prepared,const void* matrices,size_t matrixBytes,
    uint32_t objectBase,uint64_t sceneKey,uint64_t upload,DxvkResourceBufferInfo matrixInfo) noexcept {
  auto session=ActiveSession();if(!session)return 0;
  std::unique_lock<std::mutex> lock(m->mutex,std::try_to_lock);
  if(!lock.owns_lock()){++m->drops;return 0;}
  if(ActiveSession()!=session)return 0;
  if(m->allocationFailure){++m->drops;return 0;}
  try {
    if(m->serial==UINT64_MAX||m->device->getDeviceStatus()!=VK_SUCCESS){++m->drops;return 0;}
    auto& s=m->slots[m->serial%m->slotCount];uint64_t observed=0;
    if(!m->complete(s,observed)||(s.buffer&&s.buffer->isInUse(DxvkAccess::Write))){++m->drops;return 0;}
    if(!s.buffer){
      const auto memory=QueryRecorderMemory(false);
      const auto rejected=RecorderMemoryAdmission(memory,BytesPerSlot+uint64_t(DrawsPerSlot)*sizeof(Draw));
      if(rejected!=RecorderMemoryReject::None){
        m->allocationFailure=RecorderMemoryReason(rejected);++m->drops;
        Event event{};event.key=key;event.kind=Kind::ShadowState;
        std::memcpy(event.label.data(),"recorder-memory/v1",18);
        event.data={uint64_t(rejected),memory.totalVirtual,memory.availableVirtual,
          memory.availableCommit,m->serial,BytesPerSlot+uint64_t(DrawsPerSlot)*sizeof(Draw)};
        Record(session,event);return 0;
      }
      DxvkBufferCreateInfo ci{};ci.size=BytesPerSlot;ci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      ci.stages=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT;
      ci.access=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;ci.debugName="WarVK bounded input evidence";
      // Publish all-or-nothing: a fence/vector failure must not leave a buffer
      // installed in a slot that the next call mistakes for fully initialized.
      auto buffer=m->device->createBuffer(ci,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
      constexpr auto requiredMemory=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if((buffer->storage()->getMemoryProperties()&requiredMemory)!=requiredMemory)
        throw DxvkError("Input evidence requires actual coherent host memory");
      auto fence=m->device->createFence(DxvkFenceCreateInfo{0});
      std::vector<Draw> storage;storage.reserve(DrawsPerSlot);
      s.buffer=std::move(buffer);s.fence=std::move(fence);s.draws.swap(storage);
    }
    if(!s.buffer||!s.fence||!s.buffer->mapPtr(0)||s.fenceValue==UINT64_MAX){++m->drops;return 0;}
    s.id=++m->serial;s.session=session;s.key=key;s.volume=volume;s.renderSerial=renderSerial;
    s.sceneKey=sceneKey;s.upload=upload;s.objectBase=objectBase;s.used=s.eligible=s.omitted=s.focusCount=0;s.draws.clear();
    LARGE_INTEGER tick{};QueryPerformanceCounter(&tick);s.qpc=uint64_t(tick.QuadPart);
    const auto dst=s.buffer->getSliceInfo();
    s.finalized=false;
    // Even an exception after the first copy retains the destination and has
    // a completion value. Such a partial batch is never exported as readable.
    ctx->track(s.buffer,DxvkAccess::Write);ctx->signalFence(s.fence,++s.fenceValue);
    // Avoid exporting stale padding between short (e.g. 16-bit index) copies.
    std::memset(s.buffer->mapPtr(0),0,BytesPerSlot);
    // One visibility boundary for the entire immutable invocation, not a
    // whole-pipeline serialization between every tiny diagnostic gather.
    VkMemoryBarrier2 inputReady{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    inputReady.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_2_HOST_BIT;
    inputReady.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_HOST_WRITE_BIT;
    inputReady.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    inputReady.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo inputDep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};inputDep.memoryBarrierCount=1;inputDep.pMemoryBarriers=&inputReady;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer,&inputDep);
    auto gpuSpan=[&](const Rc<DxvkBuffer>& owner,const Rc<DxvkResourceAllocation>& pinned,
                     const DxvkResourceBufferInfo& info)->Span {
      Span out{};out.handle=uint64_t(info.buffer);out.sourceOffset=info.offset;out.bytes=info.size;out.sourceBytes=info.size;
      out.owner=uint64_t(reinterpret_cast<uintptr_t>(owner.ptr()));out.allocation=uint64_t(reinterpret_cast<uintptr_t>(pinned.ptr()));
      if(!owner||!info.buffer){out.status=MissingOwner;return out;}
      out.usage=owner->info().usage;out.generation=owner->diagnosticStorageGeneration();
      out.usageProof=1;
      const auto allocation=pinned?pinned:owner->storage();
      // An internal pooled VkBuffer is created with MinGlobalBufferUsage,
      // independent of the virtual resource's narrower m_info. Dedicated and
      // imported buffers do not get this inference (memory.cpp audited).
      if(allocation&&!allocation->flags().test(DxvkAllocationFlag::OwnsBuffer)&&
          !allocation->flags().test(DxvkAllocationFlag::Imported)&&!owner->info().flags){
        out.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;out.usageProof=2;
      }
      if(!(out.usage&VK_BUFFER_USAGE_TRANSFER_SRC_BIT)){out.status=Usage;return out;}
      const auto backing=pinned?pinned->getBufferInfo():owner->getSliceInfo();
      if(backing.buffer!=info.buffer||!contains(backing.offset,backing.size,info.offset,info.size)){out.status=Range;return out;}
      // Within one command-recording invocation these spans are immutable.
      // Deduplicate only exact physical ranges, never a hash or model identity.
      for(const auto& prior:s.draws)for(const auto& p:prior.spans)
        if(p.status==Copied&&p.encoding==0&&p.handle==out.handle&&p.sourceOffset==out.sourceOffset&&p.bytes==out.bytes){out.offset=p.offset;out.status=Copied;return out;}
      if(!reserve(s.used,info.size,out.offset)){out.status=Capacity;return out;}
      VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};region.srcOffset=info.offset;region.dstOffset=dst.offset+out.offset;region.size=info.size;
      VkCopyBufferInfo2 copy{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};copy.srcBuffer=info.buffer;copy.dstBuffer=dst.buffer;copy.regionCount=1;copy.pRegions=&region;
      ctx->cmdCopyBuffer(DxvkCmdBuffer::ExecBuffer,&copy);
      ctx->track(owner,DxvkAccess::Read);if(pinned)ctx->track(pinned);
      out.status=Copied;return out;
    };
    struct GatherPush {
      uint64_t source,indices,destination;
      uint32_t sourceBytes,indexBytes;
      int32_t vertexOffset;
      uint32_t stride,indexSize,count,first,reserved;
    };
    static_assert(sizeof(GatherPush)==56);
    if(!m->gatherPipeline){
      m->gatherLayout=m->device->createBuiltInPipelineLayout(0,VK_SHADER_STAGE_COMPUTE_BIT,sizeof(GatherPush),0,nullptr);
      m->gatherPipeline=war3::render::AdoptWar3TrackedVkPipeline(m->device,
          m->device->createBuiltInComputePipeline(m->gatherLayout,util::DxvkBuiltInShaderStage(war3_frame_input_gather,nullptr)));
    }
    ctx->track(m->gatherPipeline);
    ctx->cmdBindPipeline(DxvkCmdBuffer::ExecBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,m->gatherPipeline->handle());
    auto gather=[&](const War3ShadowCasterDraw& draw,const Rc<DxvkBuffer>& owner,
                    const Rc<DxvkResourceAllocation>& pinned,const DxvkResourceBufferInfo& info,uint32_t stride)->Span {
      Span out{};out.handle=uint64_t(info.buffer);out.sourceOffset=info.offset;out.sourceBytes=info.size;out.encoding=1;
      out.owner=uint64_t(reinterpret_cast<uintptr_t>(owner.ptr()));out.allocation=uint64_t(reinterpret_cast<uintptr_t>(pinned.ptr()));
      out.status=Unsupported;
      if(!owner||!info.gpuAddress||info.gpuAddress%4||!stride||stride>256||stride%4||info.size>UINT32_MAX)return out;
      out.usage=owner->info().usage;out.usageProof=3;out.generation=owner->diagnosticStorageGeneration();
      if(!(out.usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))return out;
      auto physical=pinned?pinned:owner->storage();if(!physical)return out;
      const auto backing=physical->getBufferInfo();
      if(backing.buffer!=info.buffer||!contains(backing.offset,backing.size,info.offset,info.size)){out.status=Range;return out;}
      const uint32_t count=draw.indexed?draw.indexCount:draw.vertexCount;
      const uint32_t first=draw.indexed?draw.firstIndex:draw.firstVertex;
      uint32_t indexSize=0;
      if(draw.indexed){
        if(draw.indexType!=VK_INDEX_TYPE_UINT16&&draw.indexType!=VK_INDEX_TYPE_UINT32)return out;
        indexSize=draw.indexType==VK_INDEX_TYPE_UINT16?2:4;
        if(!draw.indexStorage||!draw.indexInfo.gpuAddress||draw.indexInfo.gpuAddress%4||draw.indexInfo.size>UINT32_MAX||
           !(draw.indexStorage->info().usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))return out;
        auto ibAllocation=draw.indexPinnedAllocation?draw.indexPinnedAllocation:draw.indexStorage->storage();
        if(!ibAllocation)return out;const auto ibBacking=ibAllocation->getBufferInfo();
        const uint64_t requiredIndexBytes=(uint64_t(first)+count)*indexSize;
        const uint64_t rounded=(requiredIndexBytes+3)&~uint64_t(3);
        if(requiredIndexBytes>draw.indexInfo.size||ibBacking.buffer!=draw.indexInfo.buffer||
           !contains(ibBacking.offset,ibBacking.size,draw.indexInfo.offset,rounded)){out.status=Range;return out;}
        ctx->track(draw.indexStorage,DxvkAccess::Read);ctx->track(ibAllocation);
      }else if(uint64_t(first)+count>UINT32_MAX)return out;
      out.bytes=uint64_t(count)*(16+stride);
      if(!reserve(s.used,out.bytes,out.offset)){out.status=Capacity;return out;}
      const GatherPush push{info.gpuAddress,draw.indexed?draw.indexInfo.gpuAddress:info.gpuAddress,dst.gpuAddress+out.offset,
        uint32_t(info.size),uint32_t(draw.indexInfo.size),draw.indexed?draw.vertexOffset:0,stride,indexSize,count,first,0};
      ctx->cmdPushConstants(DxvkCmdBuffer::ExecBuffer,m->gatherLayout->getPipelineLayout(),VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
      ctx->cmdDispatch(DxvkCmdBuffer::ExecBuffer,(count+63)/64,1,1);
      ctx->track(owner,DxvkAccess::Read);ctx->track(physical);out.status=Copied;return out;
    };
    // Prioritize the two observed signatures, but retain OTHER Stage11 inputs
    // too. The contract reports omitted counts instead of claiming all draws.
    for(uint32_t priority=0;priority<2;++priority)for(auto idx:prepared){
      if(idx>=draws.size()||!draws[idx])continue;
      const auto& d=*draws[idx];if(d.stage!=11||shape(d)!=(priority==0))continue;
      ++s.eligible;if(shape(d))++s.focusCount;
      if(s.draws.size()>=DrawsPerSlot){++s.omitted;continue;}
      // Capture can consume geometry even when its subsequent shadow draw is
      // rejected. Retain before issuing any copy/dispatch, including unwind.
      dxvk::war3::memory::TrackSnapshotSlices(*ctx, d);
      Draw r{};r.index=idx;r.part=uint64_t(reinterpret_cast<uintptr_t>(d.shadowRenderablePart));
      r.metadata=d.shadowMetadataKeyHash;r.geometry=d.shadowExactGeometryKeyHash;r.alphaFrame=d.alphaMetadataFrameSerial;r.boundsGeneration=d.boundsSourceGeneration;
      r.provenance=d.inputEvidenceProvenance;
      r.skinSelection=d.inputSkinSelection;
      r.words={uint32_t(d.stage),d.jHandle,d.rawcode,d.shadowLayerIndex,d.positionStride,d.positionOffset,uint32_t(d.positionFormat),
        uint32_t(d.indexType),d.indexCount,d.firstIndex,uint32_t(d.vertexOffset),d.vertexCount,d.firstVertex,d.minVertexIndex,d.numVertices,
        uint32_t(d.vertexBlendEnabled),uint32_t(d.vertexBlendIndexed),d.vertexBlendCount,d.paletteIndex,
        d.blendBinding,d.blendStride,d.blendWeightOffset,uint32_t(d.blendWeightFormat),d.blendIndexOffset,uint32_t(d.blendIndexFormat),
        d.uvBinding,d.uvStride,d.uvOffset,uint32_t(d.uvFormat),uint32_t(d.alphaTestEnabled),uint32_t(d.alphaBlendEnabled),
        uint32_t(d.shadowPartLifecycleState),uint32_t(d.alphaPayloadComplete),uint32_t(d.indexed),uint32_t(d.topology),
        uint32_t(d.replayBindingsResolved),uint32_t(d.gpuSkinInput.valid),uint32_t(shape(d)),uint32_t(d.shadowActualIndexDomainKnown),uint32_t(d.boundsProvenance)};
      std::memcpy(r.world.data(),&d.worldMatrix,sizeof(d.worldMatrix));
      r.spans[0]=gather(d,d.positionStorage,d.positionPinnedAllocation,d.positionInfo,d.positionStride);
      if(d.indexed)r.spans[1]=gpuSpan(d.indexStorage,d.indexPinnedAllocation,d.indexInfo);
      if(d.blendBinding==1)r.spans[2]=gather(d,d.blendStorage,{},d.blendInfo,d.blendStride);
      if(d.uvBinding!=0&&d.HasUsableUvBinding())r.spans[3]=gather(d,d.uvStorage,d.uvPinnedAllocation,d.uvInfo,d.uvStride);
      auto& matrix=r.spans[4];matrix.handle=uint64_t(matrixInfo.buffer);
      const uint64_t start=(d.vertexBlendEnabled?uint64_t(d.paletteIndex)*256:uint64_t(objectBase)+idx)*64;
      matrix.sourceOffset=matrixInfo.offset+start;matrix.bytes=d.vertexBlendEnabled?256*64:64;
      matrix.sourceBytes=matrix.bytes;
      matrix.status=MatrixRange;
      if(matrices&&contains(0,matrixBytes,start,matrix.bytes)){
        matrix.status=Capacity;
        // Matrix upload is CPU-owned, coherent, populated at this exact point
        // and not overwritten while GPU readers exist. No source GPU mapping.
        bool reused=false;
        for(const auto& p:s.draws){const auto& a=p.spans[4];if(a.status==Copied&&a.sourceOffset==matrix.sourceOffset&&a.bytes==matrix.bytes){matrix.offset=a.offset;reused=true;break;}}
        if(reused||reserve(s.used,matrix.bytes,matrix.offset)){
          if(!reused)std::memcpy(static_cast<uint8_t*>(s.buffer->mapPtr(0))+matrix.offset,static_cast<const uint8_t*>(matrices)+start,size_t(matrix.bytes));
          matrix.status=Copied;
        }
      }
      if(d.gpuSkinInput.valid){
        r.spans[5]=gpuSpan(d.gpuSkinInput.staticSource.buffer(),{},d.gpuSkinInput.staticSource.getSliceInfo());
        r.spans[6]=gpuSpan(d.gpuSkinInput.palette.buffer(),{},d.gpuSkinInput.palette.getSliceInfo());
      }
      s.draws.push_back(r);
    }
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
    host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    host.buffer=dst.buffer;host.offset=dst.offset;host.size=BytesPerSlot;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.bufferMemoryBarrierCount=1;dep.pBufferMemoryBarriers=&host;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer,&dep);
    inputReady.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT|VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    inputReady.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT|VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT;
    inputReady.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    inputReady.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    ctx->cmdPipelineBarrier(DxvkCmdBuffer::ExecBuffer,&inputDep);
    s.finalized=true;
    return s.id;
  }catch(...){m->allocationFailure="recorder-input-capture-failed";++m->drops;return 0;}
}

json InputCapture::exportNew(uint64_t session,const std::wstring& prefix,const std::atomic<bool>* stopping){
  std::lock_guard<std::mutex> lock(m->mutex);
  if(ActiveSession())return {{"ok",false},{"error","freeze before input export"}};
  json result={{"schema",2},{"session",std::to_string(session)},{"processNonce",std::to_string(ProcessNonce())},
    {"slots",m->slotCount},{"maxSlots",Slots},{"bytesPerSlot",BytesPerSlot},{"drawsPerSlot",DrawsPerSlot},{"captureDrops",std::to_string(m->drops.load())},
    {"rootCauseReady",false},{"pixelDrawId",false},{"alphaTextureBytes",false},{"intermediateAttachments",false},
    {"selection","Stage11; observed 52/105 and 614/1587 signatures first; bounded remaining Stage11"},
    {"binary",utf8(prefix+L".inputs.bin")},{"batches",json::array()}};
  File binary(prefix+L".inputs.bin");if(binary.h==INVALID_HANDLE_VALUE)return {{"ok",false},{"error","input binary CreateNew failed"}};
  File manifest(prefix+L".inputs.json");
  const std::string opening="{\"batches\":[";
  if(!manifest.write(opening.data(),opening.size()))return {{"ok",false},{"error","input manifest CreateNew failed"}};
  std::vector<const Impl::Slot*> order;for(uint32_t i=0;i<m->slotCount;++i){const auto& s=m->slots[i];if(s.id&&s.session==session)order.push_back(&s);}
  std::sort(order.begin(),order.end(),[](auto a,auto b){return a->id<b->id;});
  uint64_t offset=0;bool firstBatch=true;
  for(const auto* slot:order){
    if(stopping&&stopping->load())return {{"ok",false},{"error","input export cancelled; partial retained"}};
    const auto& s=*slot;uint64_t observed=0;bool ready=s.finalized&&m->complete(s,observed);
    json batch={{"id",std::to_string(s.id)},{"owner",std::to_string(s.key.owner)},{"frame",std::to_string(s.key.frame)},
      {"mapEpoch",std::to_string(s.key.mapEpoch)},{"deviceEpoch",std::to_string(s.key.deviceEpoch)},
      {"qpc",std::to_string(s.qpc)},{"volume",s.volume},{"renderSerial",std::to_string(s.renderSerial)},
      {"sceneKey",std::to_string(s.sceneKey)},{"uploadSerial",std::to_string(s.upload)},{"objectBase",s.objectBase},
      {"eligible",s.eligible},{"omitted",s.omitted},{"focusCount",s.focusCount},
      {"gpuComplete",ready},{"fenceValue",std::to_string(s.fenceValue)},{"observedValue",std::to_string(observed)},
      {"fileOffset",std::to_string(offset)},{"bytes",ready?s.used:0},{"draws",json::array()}};
    if(ready&&s.used){if(!binary.write(s.buffer->mapPtr(0),s.used))return {{"ok",false},{"error","input binary write failed; partial retained"}};offset+=s.used;}
    for(const auto& r:s.draws){json spans=json::array();for(const auto& p:r.spans)spans.push_back(spanJson(p));
      json provenance=json::array();for(auto v:r.provenance)provenance.push_back(std::to_string(v));
      const auto& p=r.skinSelection;
      json selection={{"schema",1},{"source",uint32_t(p.source)},{"space",uint32_t(p.space)},
        {"domain",uint32_t(p.domain)},{"runtimeModel",std::to_string(p.runtimeModel)},
        {"part",std::to_string(p.part)},{"meshPayload",std::to_string(p.meshPayload)},
        {"ownerEpoch",std::to_string(p.ownerEpoch)},{"publicationTicket",std::to_string(p.publicationTicket)},
        {"captureSerial",std::to_string(p.captureSerial)},{"hash",std::to_string(p.hash)},
        {"slotAllocationGeneration",std::to_string(p.slotAllocationGeneration)},
        {"slot",p.slot},{"actualGroupCount",p.actualGroupCount},{"frameTag",p.frameTag}};
      batch["draws"].push_back({{"index",r.index},{"words",r.words},{"worldMatrixBits",r.world},{"provenance",provenance},
        {"skinPaletteSelection",selection},
        {"part",std::to_string(r.part)},{"metadata",std::to_string(r.metadata)},{"geometry",std::to_string(r.geometry)},
        {"alphaFrame",std::to_string(r.alphaFrame)},{"boundsGeneration",std::to_string(r.boundsGeneration)},{"spans",spans}});
    }
    // One bounded batch at a time. A second full nlohmann tree can exhaust a
    // 32-bit game's address space even though the binary ring itself is bounded.
    auto encoded=batch.dump();
    if(!firstBatch&&!manifest.write(",",1))return {{"ok",false},{"error","input manifest write failed"}};
    firstBatch=false;
    if(!manifest.write(encoded.data(),encoded.size()))return {{"ok",false},{"error","input manifest write failed"}};
  }
  result["binaryBytes"]=std::to_string(offset);
  if(!binary.finish())return {{"ok",false},{"error","input binary flush failed"}};
  result.erase("batches");const auto text="],"+result.dump().substr(1);
  if(!manifest.write(text.data(),text.size())||!manifest.finish())return {{"ok",false},{"error","input manifest CreateNew failed"}};
  return {{"ok",m->allocationFailure==nullptr},{"error",m->allocationFailure?m->allocationFailure:""},
    {"manifest",utf8(prefix+L".inputs.json")},{"binary",utf8(prefix+L".inputs.bin")}};
}
json ExportInputs(uint64_t session,const std::wstring& prefix,const std::atomic<bool>* stopping){
  if(!InputsEnabled())return {{"ok",true},{"enabled",false}};
  std::shared_ptr<InputCapture> p;{std::lock_guard<std::mutex> lock(registryMutex);p=registry.lock();}
  return p?p->exportNew(session,prefix,stopping):json{{"ok",false},{"error","input provider never initialized"}};
}
} // namespace dxvk::war3::tools::evidence
