#pragma once
#include <d3d9.h>
#include "d3d9_wire.h"
#include "d3d9_draw_wire.h"
#include "d3d9_ff_wire.h"
#include "d3d9_annotation_wire.h"
#include "render_command_queue.h"
#include "owned_child.h"
#include "d3d9_window_identity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>

namespace warvk::host::d3d9proxy {
// Isolated test DLL owner, NOT the shipping asynchronous service. All COM calls
// are serialized on its creation thread; a worker owns blocking IPC/child drain.
// Clear and the explicit bounded-draw-batch profile may return after local
// admission. Every other remote call flushes the draw prefix and is an ordered
// synchronous barrier. This is still a laboratory owner: final
// joins/cancellation may block, and an external watchdog remains mandatory.
class Connection final {
  struct PreparedOnly { };
  Connection(PreparedOnly,ipc::Nonce scope) : owner(GetCurrentThreadId()),nonce(scope),
    queue(commands::Scope{nonce.low,nonce.high,1,1}),
    workAvailable(makeNotification()),producerProgress(makeNotification()) { }
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
  // The test drives this exact queue/admission/reply code without child/pipe/GPU.
  // No alternate worker or successful initialization is installed by this seam.
  friend struct ConnectionCpuTest;
#endif
public:
  explicit Connection(bool presenter=false) : Connection(PreparedOnly{},lab::RandomNonce()) {
    wchar_t batching[8]{};
    const DWORD mode=GetEnvironmentVariableW(L"WARVK_D3D9_BATCH_DRAWS",batching,8);
    if(mode && (mode!=1 || batching[0]!=L'1'))throw std::runtime_error("unknown bounded draw batch profile");
    drawBatching=mode!=0;
    wchar_t asynchronous[8]{};
    const DWORD asyncMode=GetEnvironmentVariableW(L"WARVK_D3D9_ASYNC_BATCH",asynchronous,8);
    if(asyncMode && (!drawBatching || asyncMode!=1 || asynchronous[0]!=L'1'))
      throw std::runtime_error("asynchronous draw admission requires bounded draw batches");
    asyncDrawBatches=asyncMode!=0;
    wchar_t fixedBatches[8]{};
    const DWORD fixedMode=GetEnvironmentVariableW(L"WARVK_D3D9_FIXED_BATCH",fixedBatches,8);
    if(fixedMode && (!drawBatching || fixedMode!=1 || fixedBatches[0]!=L'1'))
      throw std::runtime_error("fixed-function draw batches require bounded draw batches");
    fixedBatching=fixedMode!=0;
    wchar_t events[8]{};
    const DWORD eventMode=GetEnvironmentVariableW(L"WARVK_D3D9_ASYNC_EVENTS",events,8);
    if(eventMode && (!asyncDrawBatches || eventMode!=1 || events[0]!=L'1'))
      throw std::runtime_error("asynchronous event completion requires asynchronous draw batches");
    asyncEvents=eventMode!=0;
    wchar_t group[8]{};
    const DWORD groupMode=GetEnvironmentVariableW(L"WARVK_D3D9_GPU_GROUP",group,8);
    if(groupMode&&(groupMode!=1||group[0]!=L'4'))throw std::runtime_error("unknown GPU submission group");
    wchar_t stream[8]{};
    const DWORD streamMode=GetEnvironmentVariableW(L"WARVK_D3D9_STREAM",stream,8);
    if(streamMode&&(groupMode||streamMode!=1||stream[0]!=L'1'))
      throw std::runtime_error("unknown host stream profile or combined with GPU groups");
    // Both host profiles keep recorded GPU work unsubmitted until FlushDraws.
    groupedSubmissions=groupMode!=0||streamMode!=0;
    wchar_t admission[8]{};
    const DWORD admissionMode=GetEnvironmentVariableW(L"WARVK_D3D9_ADMISSION_PROFILE",admission,8);
    if(admissionMode&&(admissionMode!=1||admission[0]!=L'1'))throw std::runtime_error("unknown admission diagnostic mode");
    admissionProfile=admissionMode!=0;
    wchar_t annotate[8]{};
    const DWORD annotateMode=GetEnvironmentVariableW(L"WARVK_D3D9_ANNOTATIONS",annotate,8);
    if(annotateMode&&(annotateMode!=1||annotate[0]!=L'1'))throw std::runtime_error("unknown annotation profile");
    annotations=annotateMode!=0;
    wchar_t exe[32768]{},log[32768]{};
    const DWORD a=GetEnvironmentVariableW(L"WARVK_D3D9_HOST_EXE",exe,32768);
    const DWORD b=GetEnvironmentVariableW(L"WARVK_D3D9_HOST_LOG",log,32768);
    if(!a||a>=32768||!b||b>=32768||GetModuleHandleW(L"Game.dll"))
      throw std::runtime_error("D3D9 development host/log required; game prohibited");
    const auto desktop=presenter?d9::RequireIsolatedDesktop():std::wstring();
    worker=std::thread([this,path=std::wstring(exe),out=std::wstring(log),desktop] {
      try {
        if(!desktop.empty()&&d9::RequireIsolatedDesktop()!=desktop)
          throw std::runtime_error("D3D9 worker desktop mismatch");
        const auto args=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())
          +L" "+std::to_wstring(lab::CreationTime(GetCurrentProcess()));
        lab::OwnedChild child(path,args,out,desktop);
        childPid=child.pid();childCreation=child.creation();
        try {
          auto pipe=lab::Connect(nonce,child.process(),child.pid(),lab::Channel::D3D9Dev5);
          peerVerified=true;
          commands::Batch batch;
          for(;;) {
            const auto idleBegin=admissionProfile?AdmissionClock::now():AdmissionClock::time_point{};
            const auto status=waitForWorkerBatch(batch);
            if(status==commands::Dequeue::Finished){pipe.reset();break;}
            if(status!=commands::Dequeue::Item)throw std::runtime_error("D3D9 queue terminal");
            d9::Header h;
            if(!d9::DecodeHeader(batch.payload.data(),h)||h.sequence!=batch.sequence)
              throw std::runtime_error("D3D9 local header mismatch");
            const auto writeBegin=admissionProfile?AdmissionClock::now():AdmissionClock::time_point{};
            lab::IoStats io;
            lab::WriteBytes(pipe.get(),child.process(),batch.payload.data(),batch.bytes,io);
            const auto replyBegin=admissionProfile?AdmissionClock::now():AdmissionClock::time_point{};
            const lab::ReadWindow deadline;
            lab::ReadExact(pipe.get(),child.process(),replyPacket.data(),d9::ReplyBytes,deadline,lab::Stage::Header,io);
            if(!d9::DecodeReply(h,replyPacket.data(),reply))throw std::runtime_error("D3D9 reply identity");
            lab::ReadExact(pipe.get(),child.process(),replyPacket.data()+d9::ReplyBytes,
              reply.payloadBytes,deadline,lab::Stage::Body,io);
            if(admissionProfile) {
              const auto end=AdmissionClock::now();auto& a=workerAdmission;
              const auto writeNs=Nanoseconds(writeBegin,replyBegin),replyNs=Nanoseconds(replyBegin,end);
              ++a.messages;a.idleNs+=Nanoseconds(idleBegin,writeBegin);a.writeNs+=writeNs;a.replyNs+=replyNs;
              if(h.op==d9::Op::DrawBatch||h.op==d9::Op::DrawFixedBatch){++a.batchMessages;a.batchWriteNs+=writeNs;a.batchReplyNs+=replyNs;}
            }
            acceptReply(h,batch);
            if(admissionProfile&&h.op==d9::Op::Issue&&HRESULT(reply.result)==D3D_OK)
              sampleAdmission(workerSamples,workerSampleCount,workerSamplesDropped,workerAdmission);
            if(remoteTerminal)break;
            if(h.op==d9::Op::Finish) {
              if(FAILED(HRESULT(reply.result)))throw std::runtime_error("D3D9 Finish was rejected");
              break;
            }
          }
          const auto exitStart=GetTickCount64();
          std::fprintf(stderr,"D3D9 dev primary-exit-wait begin pid=%lu remoteTerminal=%s limitMs=5000\n",
            childPid,remoteTerminal?"true":"false");std::fflush(stderr);
          try {
            childExit=child.wait(5000);
            // Only a returned primary wait proves this phase settled. A later
            // catch-path drain or Job cleanup must not upgrade a failed run.
            childProcessSettled=true;
            childNaturalExit=childExit==0u||childExit==2u;
            childSelfFailStop=childExit==4u;
          }
          catch(...) { exitWaitMs=GetTickCount64()-exitStart;throw; }
          exitWaitMs=GetTickCount64()-exitStart;
          std::fprintf(stderr,"D3D9 dev primary-exit-wait complete pid=%lu exit=%lu elapsedMs=%llu\n",
            childPid,childExit,exitWaitMs);std::fflush(stderr);
          if(remoteTerminal?(childExit!=2u&&childExit!=4u):childExit!=0u)
            throw std::runtime_error("D3D9 child exit");
        } catch(...) {
          const auto original=std::current_exception();
          // Publish terminal admission immediately, BEFORE any diagnostic
          // stream operation or secondary child drain can block the worker.
          failClosed();
          // Log before any drain/destructor can itself stall. The rethrow
          // below still delivers this same exception to the outer handler.
          try { std::rethrow_exception(original); }
          catch(const lab::Failure& e) {
            std::fprintf(stderr,"D3D9 dev primary failure=%s stage=%s win32=%lu primaryExitWaitMs=%llu\n",
              lab::FaultName(e.fault),lab::StageName(e.stage),e.win32,exitWaitMs);
          } catch(const std::exception& e) {
            std::fprintf(stderr,"D3D9 dev primary failure=%s primaryExitWaitMs=%llu\n",e.what(),exitWaitMs);
          } catch(...) {
            std::fprintf(stderr,"D3D9 dev primary failure=unknown primaryExitWaitMs=%llu\n",exitWaitMs);
          }
          std::fflush(stderr);
          // The try-scope pipe has already closed while unwinding, so the
          // exact peer sees end-of-stream before we wait for its process.
          // A secondary timeout must never replace the original transport,
          // identity or primary-exit error. Nor can this drain certify an exit.
          const auto secondaryStart=GetTickCount64();
          try {
            const auto secondaryExit=child.wait(5000);
            std::fprintf(stderr,"D3D9 dev secondary-exit-drain pid=%lu exit=%lu elapsedMs=%llu originalPreserved=true\n",
              childPid,secondaryExit,GetTickCount64()-secondaryStart);
          } catch(const lab::Failure& e) {
            std::fprintf(stderr,"D3D9 dev secondary-exit-drain failure=%s stage=%s win32=%lu elapsedMs=%llu originalPreserved=true\n",
              lab::FaultName(e.fault),lab::StageName(e.stage),e.win32,GetTickCount64()-secondaryStart);
          } catch(const std::exception& e) {
            std::fprintf(stderr,"D3D9 dev secondary-exit-drain failure=%s elapsedMs=%llu originalPreserved=true\n",
              e.what(),GetTickCount64()-secondaryStart);
          } catch(...) {
            std::fprintf(stderr,"D3D9 dev secondary-exit-drain failure=unknown elapsedMs=%llu originalPreserved=true\n",
              GetTickCount64()-secondaryStart);
          }
          std::fflush(stderr);
          std::rethrow_exception(original);
        }
      } catch(const lab::Failure& e) {
        failClosed();
        std::fprintf(stderr,"D3D9 dev worker: %s stage=%s win32=%lu primaryExitWaitMs=%llu\n",
          lab::FaultName(e.fault),lab::StageName(e.stage),e.win32,exitWaitMs);
      } catch(const std::exception& e) {
        failClosed();
        std::fprintf(stderr,"D3D9 dev worker: %s primaryExitWaitMs=%llu\n",e.what(),exitWaitMs);
      } catch(...) {
        failClosed();
      }
    });
  }
  ~Connection() {
    closeProducer();
    if(worker.joinable())worker.join(); // laboratory only; not a DllMain/game API
    std::printf("{\"schema\":\"warvk-d3d9-proxy/v1\",\"childPid\":%lu,\"childCreation\":%llu,"
      "\"childExit\":%lu,\"exchanges\":%llu,\"peerVerified\":%s,\"transportFailed\":%s,"
      "\"x86VulkanAbsent\":%s,\"deviceLost\":%s,\"remoteTerminal\":%s,\"childNaturalExit\":%s,"
      "\"childProcessSettled\":%s,\"childSelfFailStop\":%s,"
      "\"primaryExitWaitMs\":%llu,"
      "\"productIntegrated\":false,\"memorySavingsProven\":false}\n",
      childPid,childCreation,childExit,exchanges,peerVerified?"true":"false",failed?"true":"false",
      GetModuleHandleW(L"vulkan-1.dll")?"false":"true",deviceLost?"true":"false",
      remoteTerminal?"true":"false",childNaturalExit?"true":"false",
      childProcessSettled?"true":"false",childSelfFailStop?"true":"false",exitWaitMs);
    // Cold, after worker join. Producer and worker private counters are never
    // read concurrently, and CPU reply progress is not a GPU-completion claim.
    std::fprintf(stderr,"D3D9 async-clear: {\"schema\":\"warvk-d3d9-async-clear/v1\","
      "\"initializedAcknowledged\":%s,\"clearPublished\":%llu,\"clearRemoteSucceeded\":%llu,"
      "\"fullObservations\":%llu,\"backpressureCalls\":%llu,\"backpressureMs\":%llu,\"maxBackpressureMs\":%llu,"
      "\"maxObservedOutstanding\":%u,\"lastPublished\":%u,\"lastReplySettled\":%u,"
      "\"asyncFailureSequence\":%llu,\"asyncFailureResult\":%lu,"
      "\"queueCapacity\":4,\"workerPrivateBatchCapacity\":1,\"admissionDeadlineMs\":5000,"
      "\"callDeadlineMs\":30000,\"replyUsesRemainingCallBudget\":true,\"transportDeadlineMs\":5000,\"primaryExitDeadlineMs\":5000,"
      "\"gpuCompletionInferredFromAdmission\":false,\"productOwnerComplete\":false}\n",
      initializedAcknowledged.load(std::memory_order_acquire)?"true":"false",clearPublished,clearRemoteSucceeded,
      fullObservations,backpressureCalls,backpressureMs,maxBackpressureMs,maxObservedOutstanding,sequence,
      done.load(std::memory_order_acquire),asyncFailureSequence,asyncFailureResult);
    std::fprintf(stderr,"D3D9 draw-batch-proxy: {\"schema\":\"warvk-d3d9-draw-batch-proxy/v1\","
      "\"enabled\":%s,\"admittedDraws\":%llu,\"flushedBatches\":%llu,\"pendingDraws\":%u,"
      "\"capacity\":%u,\"payloadCapacityBytes\":%zu,\"asyncAdmission\":%s,"
      "\"remoteSucceededBatches\":%llu,\"asyncEvents\":%s,\"eventWaitsPublished\":%llu,"
      "\"eventWaitsSucceeded\":%llu,\"eventPendingObservations\":%llu,\"eventLocalCompletions\":%llu,"
      "\"eventGpuCompleted\":%llu,\"gpuCompletionInferredFromAdmission\":false}\n",
      drawBatching?"true":"false",batchedDrawsAdmitted,drawBatchesFlushed,drawBatchCount,d9::MaxDrawBatch,drawBatch.size(),
      asyncDrawBatches?"true":"false",drawBatchesRemoteSucceeded,asyncEvents?"true":"false",
      eventWaitsPublished,eventWaitsSucceeded,eventPendingObservations,eventLocalCompletions,
      eventGpuCompleted.load(std::memory_order_acquire));
    // remoteSucceeded counts enqueued ordered state; a call() of it is not counted.
    std::fprintf(stderr,"D3D9 ordered-async: {\"schema\":\"warvk-d3d9-ordered-async/v1\",\"enabled\":%s,"
      "\"uploadsPublished\":%llu,\"statesPublished\":%llu,\"passedStaged\":%llu,\"remoteSucceeded\":%llu}\n",
      asyncDrawBatches?"true":"false",orderedUploadsPublished,orderedStatesPublished,orderedPassedStaged,
      orderedRemoteSucceeded);
    // Separate versioned line: the frozen draw-batch-proxy/v1 schema above
    // counts programmable DrawBatch traffic only.
    std::fprintf(stderr,"D3D9 fixed-batch-proxy: {\"schema\":\"warvk-d3d9-fixed-batch-proxy/v1\","
      "\"enabled\":%s,\"asyncAdmission\":%s,\"admittedDraws\":%llu,\"flushedBatches\":%llu,"
      "\"pendingDraws\":%u,\"remoteSucceededBatches\":%llu,\"capacity\":%u,\"payloadCapacityBytes\":%zu}\n",
      fixedBatching?"true":"false",fixedBatching&&asyncDrawBatches?"true":"false",fixedDrawsAdmitted,fixedBatchesFlushed,
      fixedBatchCount,fixedBatchesRemoteSucceeded,d9::MaxFixedDrawBatch,fixedBatch.size());
    if(annotations)
      std::fprintf(stderr,"D3D9 annotation-proxy: {\"schema\":\"warvk-d3d9-annotation-proxy/v1\","
        "\"annotatedDraws\":%llu,\"records\":%llu,\"messages\":%llu,\"frameScenes\":%llu,\"stageMarkers\":%llu,"
        "\"discardedRecords\":%llu}\n",annotatedDrawsAdmitted,annotationRecordsAdmitted,annotationMessages,
        frameScenesSent,stageMarkersSent,annotationRecordsDiscarded);
    for(uint32_t i=0;i<workerSampleCount;++i) {
      const auto& a=workerSamples[i];
      std::fprintf(stderr,"D3D9 admission-worker: {\"schema\":\"warvk-d3d9-admission-worker/v1\",\"query\":%u,"
        "\"messages\":%llu,\"idleNs\":%llu,\"writeNs\":%llu,\"replyNs\":%llu,\"batchMessages\":%llu,"
        "\"batchWriteNs\":%llu,\"batchReplyNs\":%llu,\"dropped\":%u}\n",i+1,a.messages,a.idleNs,a.writeNs,a.replyNs,
        a.batchMessages,a.batchWriteNs,a.batchReplyNs,workerSamplesDropped);
    }
    for(uint32_t i=0;i<producerSampleCount;++i) {
      const auto& a=producerSamples[i];
      std::fprintf(stderr,"D3D9 admission-producer: {\"schema\":\"warvk-d3d9-admission-producer/v1\",\"query\":%u,"
        "\"publishes\":%llu,\"backpressureEpisodes\":%llu,\"backpressureNs\":%llu,\"calls\":%llu,\"callWaitNs\":%llu,"
        "\"dropped\":%u}\n",i+1,a.publishes,a.backpressureEpisodes,a.backpressureNs,a.calls,a.callWaitNs,producerSamplesDropped);
    }
  }
  bool isOwner() const noexcept { return GetCurrentThreadId()==owner; }
  bool healthy() const noexcept { return !failed.load(std::memory_order_acquire)&&!deviceLost.load(std::memory_order_acquire); }
  bool asynchronousEvents() const noexcept { return asyncEvents; }
  struct DiagnosticIdentity {
    uint64_t nonceLow=0,nonceHigh=0,sequence=0,childCreation=0;
    uint32_t childPid=0;
  };
  // Cold, owner-only snapshot after a completed call. The existing done acquire
  // publishes the worker's actual child identity; no extra transport or wait.
  bool diagnosticIdentity(DiagnosticIdentity& result) const noexcept {
    if(!isOwner()||!healthy()||!sequence||done.load(std::memory_order_acquire)!=sequence)return false;
    if(!childPid||!childCreation)return false;
    result={nonce.low,nonce.high,sequence,childCreation,childPid};return true;
  }
  // Local metadata disagreement or an unacknowledged destroy cannot be healed
  // by recycling a handle. This is a local fault, not a fabricated remote
  // DEVICELOST receipt or proof of GPU retirement.
  void failClosed() noexcept {
    failed.store(true,std::memory_order_release);
    queue.requestFault();
    // Irreversible state is authoritative; notifications only wake its reader.
    // Even a damaged notification is bounded by each wait's finite recheck.
    SetEvent(workAvailable.get());
    SetEvent(producerProgress.get());
  }
  std::wstring windowMarker() const { return d9::WindowMarker(nonce); }
  HRESULT submitDraw(const d9::DrawPacket& packet) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!initializedAcknowledged.load(std::memory_order_acquire))return D3DERR_INVALIDCALL;
    if(!drawBatching) {
      const auto annotated=publishNextRecords(d9::Op::Draw);if(FAILED(annotated))return annotated;
      d9::Header h;h.op=d9::Op::Draw;h.payloadBytes=d9::DrawBytes;
      return call(h,packet.data());
    }
    // One staged kind at a time preserves the remote order of mixed draws.
    if(fixedBatchCount){const auto prior=flushDraws();if(FAILED(prior))return prior;}
    if(drawBatchCount>=d9::MaxDrawBatch){failClosed();return D3DERR_DEVICELOST;}
    attachNextRecords(drawBatchCount);
    std::memcpy(drawBatch.data()+drawBatchCount*d9::DrawBytes,packet.data(),packet.size());
    ++drawBatchCount;++batchedDrawsAdmitted;
    // Local, owned bytes only. Resource mutations/destruction and every remote
    // observation first flush this prefix; no resource ID can be recycled here.
    return drawBatchCount==d9::MaxDrawBatch?flushDraws():D3D_OK;
  }
  // Fixed-function draws share the bounded batch profile and its terminal
  // deferred-refusal rule. Without WARVK_D3D9_FIXED_BATCH each stays one
  // synchronous DrawFixed whose INVALIDCALL the caller can still recover from.
  HRESULT submitFixedDraw(const d9::FixedDrawPacket& packet) noexcept {
    if(!fixedBatching) {
      const auto annotated=publishNextRecords(d9::Op::DrawFixed);if(FAILED(annotated))return annotated;
      d9::Header h;h.op=d9::Op::DrawFixed;h.payloadBytes=d9::FixedDrawBytes;
      return call(h,packet.data());
    }
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!initializedAcknowledged.load(std::memory_order_acquire))return D3DERR_INVALIDCALL;
    if(drawBatchCount){const auto prior=flushDraws();if(FAILED(prior))return prior;}
    if(fixedBatchCount>=d9::MaxFixedDrawBatch){failClosed();return D3DERR_DEVICELOST;}
    attachNextRecords(fixedBatchCount);
    std::memcpy(fixedBatch.data()+fixedBatchCount*d9::FixedDrawBytes,packet.data(),packet.size());
    ++fixedBatchCount;++fixedDrawsAdmitted;
    return fixedBatchCount==d9::MaxFixedDrawBatch?flushDraws():D3D_OK;
  }
  bool fixedBatchingEnabled() const noexcept { return fixedBatching; }
  // Asynchronous profile: ordered state and buffer content are admitted like a
  // batch, without waiting for their replies, and a refusal is terminal.
  bool asyncOrderedEnabled() const noexcept { return asyncDrawBatches; }
  static bool AsyncOrderedOp(d9::Op op) noexcept {
    return op==d9::Op::UploadBuffer||op==d9::Op::SetEffects||op==d9::Op::SetSamplerState||op==d9::Op::SetTextureStages;
  }
  // passStaged is for NOOVERWRITE content only: by the Lock contract no staged
  // draw reads its range, so it may reach the host before them. Everything
  // else first flushes the staged prefix, as call() does.
  HRESULT enqueueOrdered(d9::Header h,const void* payload,bool passStaged=false) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!asyncDrawBatches||!initializedAcknowledged.load(std::memory_order_acquire)||!AsyncOrderedOp(h.op)
      ||(passStaged&&h.op!=d9::Op::UploadBuffer))return D3DERR_INVALIDCALL;
    const bool staged=drawBatchCount||fixedBatchCount;
    if(!passStaged){const auto prior=flushDraws();if(FAILED(prior))return prior;}
    try {
      publishUntil(h,payload,GetTickCount64()+lab::DeadlineMs);
      ++(h.op==d9::Op::UploadBuffer?orderedUploadsPublished:orderedStatesPublished);
      if(passStaged&&staged)++orderedPassedStaged;
      return healthy()?D3D_OK:D3DERR_DEVICELOST;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 async ordered admission: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  bool annotationsEnabled() const noexcept { return annotations; }
  // Records for the next draw call, ascending by schema. The draw that
  // consumes them carries them into its batch; one that submits nothing
  // discards them (Device draw calls always end with the discard).
  HRESULT annotateNextDraw(uint16_t schema,const void* data,uint16_t bytes) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!annotations||!initializedAcknowledged.load(std::memory_order_acquire)
      ||nextRecordCount>=d9::MaxRecordsPerDraw||(nextRecordCount&&schema<=nextLastSchema))return D3DERR_INVALIDCALL;
    const auto size=d9::AppendDrawRecord(nextRecords.data()+nextRecordBytes,
      uint32_t(nextRecords.size()-nextRecordBytes),0,schema,data,bytes);
    if(!size)return D3DERR_INVALIDCALL;
    nextRecordBytes+=size;++nextRecordCount;nextLastSchema=schema;return D3D_OK;
  }
  void discardNextDrawAnnotations() noexcept {
    annotationRecordsDiscarded+=nextRecordCount;nextRecordBytes=nextRecordCount=0;
  }
  // Ordered barriers like any other non-draw call: the staged prefix and its
  // records reach the host first.
  HRESULT frameScene(uint32_t schema,const void* data,uint32_t bytes) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!annotations||!d9::ValidFrameScene(schema,static_cast<const uint8_t*>(data),bytes))return D3DERR_INVALIDCALL;
    d9::Header h;h.op=d9::Op::FrameScene;h.args[0]=schema;h.payloadBytes=bytes;
    const auto hr=call(h,data);if(hr==D3D_OK)++frameScenesSent;return hr;
  }
  HRESULT stageMarker(uint32_t stage) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    if(!annotations||!d9::ValidStage(stage))return D3DERR_INVALIDCALL;
    d9::Header h;h.op=d9::Op::StageMarker;h.args[0]=stage;
    const auto hr=call(h);if(hr==D3D_OK)++stageMarkersSent;return hr;
  }
  HRESULT flushDraws() noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    // At most one kind is staged: each submit flushes the other kind first.
    if(fixedBatchCount)
      return publishStaged(d9::Op::DrawFixedBatch,fixedBatch.data(),fixedBatchCount,d9::FixedDrawBytes,fixedBatchesFlushed);
    if(!drawBatchCount)return D3D_OK;
    return publishStaged(d9::Op::DrawBatch,drawBatch.data(),drawBatchCount,d9::DrawBytes,drawBatchesFlushed);
  }
  HRESULT endScene() noexcept {
    const auto result=flushDraws();if(FAILED(result)||!groupedSubmissions)return result;
    d9::Header h;h.op=d9::Op::FlushDraws;return call(h);
  }
  HRESULT enqueueClear(d9::Header h) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    // A live worker/pipe or queue publication is not successful initialization.
    // Flags/depth and D3D9 argument validity stay with Device::Clear and host.
    if(!initializedAcknowledged.load(std::memory_order_acquire)||h.op!=d9::Op::Clear
      ||h.id||h.generation||h.payloadBytes)return D3DERR_INVALIDCALL;
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    try {
      const auto until=GetTickCount64()+lab::DeadlineMs;
      publishUntil(h,nullptr,until);
      ++clearPublished;
      // A failure may have arrived while this transaction was admitted. Never
      // turn that already-visible failure into a fresh success or retry scope.
      return healthy()?D3D_OK:D3DERR_DEVICELOST;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 async Clear admission: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  // Query has already checked its live handle and successful Issue revision.
  // A connection-scoped monotonic GPU watermark can satisfy multiple issued
  // events, including repeated Issue at the same serial. No Query pointer or
  // caller output buffer crosses to the worker, and a fresh nonce starts at 0.
  HRESULT pollEvent(d9::Header h) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    const auto serial=uint64_t(h.args[2])|(uint64_t(h.args[3])<<32);
    if(!asyncEvents||!initializedAcknowledged.load(std::memory_order_acquire)
      ||h.op!=d9::Op::Poll||!d9::ValidRef({h.id,h.generation})||h.payloadBytes
      ||(!h.args[0]&&!h.args[1])||!serial)return D3DERR_INVALIDCALL;
    for(size_t i=4;i<h.args.size();++i)if(h.args[i])return D3DERR_INVALIDCALL;
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    const auto settled=done.load(std::memory_order_acquire);
    if(eventGpuCompleted.load(std::memory_order_acquire)>=serial) {
      ++eventLocalCompletions;return healthy()?S_OK:D3DERR_DEVICELOST;
    }
    // At most one outstanding wait in the existing ordered queue. Polling
    // another Query while it is pending observes S_FALSE, not another message.
    if(eventWaitSequence>settled) {
      ++eventPendingObservations;return healthy()?S_FALSE:D3DERR_DEVICELOST;
    }
    try {
      h.op=d9::Op::WaitEvent;
      publishUntil(h,nullptr,GetTickCount64()+lab::DeadlineMs);
      eventWaitSequence=uint32_t(h.sequence);++eventWaitsPublished;
      return healthy()?S_FALSE:D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  HRESULT call(d9::Header h,const void* payload=nullptr,void* result=nullptr,uint32_t resultSize=0) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!healthy())return D3DERR_DEVICELOST;
    // A read-only diagnostic must not implicitly submit locally staged draws.
    // Caller may explicitly EndScene/flush first, then observe the ordered
    // remote prefix. A refusal changes neither sequence nor destination bytes.
    if(h.op==d9::Op::DiagnosticCheckpoint&&(drawBatchCount||fixedBatchCount))return D3DERR_INVALIDCALL;
    // Draw records precede their own batch; publishNextRecords flushes first.
    if(h.op!=d9::Op::DrawBatch&&h.op!=d9::Op::DrawFixedBatch&&h.op!=d9::Op::AnnotateDraws) {
      const auto prior=flushDraws();if(FAILED(prior))return prior;
    }
    try {
      // One original 30-second call budget includes the new admission wait.
      // Admission is additionally capped at 5 seconds; publication does not
      // restart the reply clock. Worker framing/primary-exit stay 5000 ms.
      const auto deadlines=callDeadlinesAt(GetTickCount64());
      // The worker keeps this reply recoverable, also for ordered state.
      callSequence.store(sequence+1,std::memory_order_release);
      publishUntil(h,payload,deadlines.admission);
      const auto waitBegin=admissionProfile?AdmissionClock::now():AdmissionClock::time_point{};
      for(;;) {
        const auto settled=done.load(std::memory_order_acquire);
        if(settled==h.sequence)break;
        if(settled>h.sequence)throw std::runtime_error("D3D9 barrier reply was skipped");
        if(!healthy())return D3DERR_DEVICELOST;
        if(GetTickCount64()>=deadlines.call)throw std::runtime_error("D3D9 response deadline");
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
        if(replyWaitObservedForTest)replyWaitObservedForTest->store(true,std::memory_order_release);
        if(beforeReplyWaitForTest)beforeReplyWaitForTest(*this);
#endif
        // No arbitrary message dispatch/reentry. The isolated WSI profile
        // requires a DIFFERENT, continuously pumping HWND owner thread. This
        // synchronous laboratory owner is still forbidden in Game.dll.
        waitNotification(producerProgress.get(),remainingWait(deadlines.call));
      }
      if(admissionProfile){++producerAdmission.calls;producerAdmission.callWaitNs+=Nanoseconds(waitBegin,AdmissionClock::now());}
      // This fixed single producer cannot queue anything after its synchronous
      // barrier until returning. Thus the worker cannot overwrite this reply
      // with a later command while the producer reads/copies its payload.
      const HRESULT hr=HRESULT(reply.result);
      if(admissionProfile&&h.op==d9::Op::Issue&&hr==D3D_OK)
        sampleAdmission(producerSamples,producerSampleCount,producerSamplesDropped,producerAdmission);
      if(hr==D3DERR_DEVICELOST)deviceLost.store(true,std::memory_order_release);
      if(SUCCEEDED(hr)) {
        if(reply.payloadBytes!=resultSize||(resultSize&&!result))
          throw std::runtime_error("D3D9 response payload size");
        if(resultSize)std::memcpy(result,replyPacket.data()+d9::ReplyBytes,resultSize);
      } else if(reply.payloadBytes)throw std::runtime_error("D3D9 failed result carried payload");
      return hr;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 dev call: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {
      failClosed();return D3DERR_DEVICELOST;
    }
  }
private:
  // Moves the next draw's records into the staged batch at this draw index.
  // The batch bound (MaxDrawAnnotationBytes) holds every draw's full records.
  void attachNextRecords(uint32_t index) noexcept {
    if(!nextRecordCount)return;
    std::memcpy(batchRecords.data()+batchRecordBytes,nextRecords.data(),nextRecordBytes);
    for(uint32_t at=batchRecordBytes;at<batchRecordBytes+nextRecordBytes;
        at+=d9::RecordBytes(batchRecords[at+4]|(batchRecords[at+5]<<8))) {
      batchRecords[at]=uint8_t(index);batchRecords[at+1]=uint8_t(index>>8);
    }
    batchRecordBytes+=nextRecordBytes;batchRecordCount+=nextRecordCount;
    ++annotatedDrawsAdmitted;annotationRecordsAdmitted+=nextRecordCount;
    nextRecordBytes=nextRecordCount=0;
  }
  // AnnotateDraws for a single synchronous draw. The staged prefix goes first,
  // so nothing can come between these records and their draw.
  HRESULT publishNextRecords(d9::Op op) noexcept {
    if(!nextRecordCount)return D3D_OK;
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    d9::Header h;h.op=d9::Op::AnnotateDraws;h.args[0]=nextRecordCount;h.args[1]=1;h.args[2]=uint32_t(op);
    h.payloadBytes=nextRecordBytes;
    const auto records=nextRecordCount;nextRecordBytes=nextRecordCount=0;
    if(call(h,nextRecords.data())!=D3D_OK){failClosed();return D3DERR_DEVICELOST;}
    ++annotatedDrawsAdmitted;annotationRecordsAdmitted+=records;++annotationMessages;return D3D_OK;
  }
  HRESULT publishStaged(d9::Op op,const uint8_t* data,uint32_t& count,uint32_t bytes,uint64_t& flushed) noexcept {
    if(batchRecordCount) {
      d9::Header a;a.op=d9::Op::AnnotateDraws;a.args[0]=batchRecordCount;a.args[1]=count;a.args[2]=uint32_t(op);
      a.payloadBytes=batchRecordBytes;batchRecordBytes=batchRecordCount=0;
      if(asyncDrawBatches) {
        try {publishUntil(a,batchRecords.data(),GetTickCount64()+lab::DeadlineMs);}
        catch(...) {failClosed();return D3DERR_DEVICELOST;}
      } else if(call(a,batchRecords.data())!=D3D_OK){failClosed();return D3DERR_DEVICELOST;}
      ++annotationMessages;
    }
    d9::Header h;h.op=op;h.args[0]=count;h.payloadBytes=count*bytes;
    if(asyncDrawBatches) {
      try {
        // The same four-slot SPSC queue owns a complete copy before return.
        // Full waits against the original five-second admission deadline; no
        // batch is dropped, and an asynchronous refusal is terminal in worker.
        publishUntil(h,data,GetTickCount64()+lab::DeadlineMs);
        count=0;++flushed;
        return healthy()?D3D_OK:D3DERR_DEVICELOST;
      } catch(...) {failClosed();return D3DERR_DEVICELOST;}
    }
    const auto hr=call(h,data);
    // An admitted prefix cannot be replayed after rejection/transport ambiguity.
    // Deferred INVALIDCALL is terminal, not a silently omitted draw.
    if(hr!=D3D_OK){failClosed();return D3DERR_DEVICELOST;}
    count=0;++flushed;return D3D_OK;
  }
  // Two connection-owned, non-inherited auto-reset hints. Each has exactly one
  // waiter (worker or producer); queue/done acquire loads, never event state,
  // grant progress. No ResetEvent: a notification between predicate and wait
  // stays pending, while coalesced/old notifications merely cause a recheck.
  static lab::Handle makeNotification() {
    lab::Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    if(!event)throw std::runtime_error("D3D9 notification creation failed");
    return event;
  }
  static void signalNotification(HANDLE event) {
    if(!SetEvent(event))throw std::runtime_error("D3D9 notification signal failed");
  }
  static void waitNotification(HANDLE event,DWORD remaining) {
    const auto status=WaitForSingleObject(event,remaining);
    if(status!=WAIT_OBJECT_0&&status!=WAIT_TIMEOUT)
      throw std::runtime_error("D3D9 notification wait failed");
  }
  static DWORD remainingWait(ULONGLONG until) noexcept {
    const auto now=GetTickCount64();
    return now>=until?0:DWORD(std::min<ULONGLONG>(until-now,lab::DeadlineMs));
  }
  void closeProducer() noexcept {
    queue.closeProducer();
    if(!SetEvent(workAvailable.get()))failClosed();
  }
  commands::Dequeue tryTakeBatch(commands::Batch& batch) {
    const auto status=queue.tryDequeue(batch);
    // The private Batch already owns these bytes. A producer blocked on Full
    // can use the freed ring slot before this batch's IPC reply or GPU work.
    if(status==commands::Dequeue::Item)signalNotification(producerProgress.get());
    return status;
  }
  commands::Dequeue waitForWorkerBatch(commands::Batch& batch) {
    for(;;) {
      const auto status=tryTakeBatch(batch);
      if(status!=commands::Dequeue::Empty)return status;
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
      if(workerWaitObservedForTest)workerWaitObservedForTest->store(true,std::memory_order_release);
#endif
      // Healthy idle has no transaction deadline. A finite safety recheck is
      // not a failed idle command, nor does it restart any admission/call clock.
      waitNotification(workAvailable.get(),lab::DeadlineMs);
    }
  }
  struct CallDeadlines { ULONGLONG admission,call; };
  // Shared pure calculation is directly exercised by the CPU fixture. The
  // caller takes this once, before admission; only the remaining budget waits.
  static CallDeadlines callDeadlinesAt(ULONGLONG began) noexcept {
    const auto call=began+30000;
    return {std::min(began+lab::DeadlineMs,call),call};
  }
  uint32_t prepare(d9::Header& h,const void* payload) {
    if(sequence==UINT32_MAX)throw std::runtime_error("D3D9 sequence exhausted");
    h.nonce=nonce;h.sequence=uint64_t(sequence)+1;
    if(!d9::Encode(h,payload,request))throw std::runtime_error("D3D9 invalid local command");
    return d9::HeaderBytes+h.payloadBytes;
  }
  commands::PublishResult tryPublishPrepared(const d9::Header& h,uint32_t bytes) {
    const auto admission=queue.tryPublish(commands::Scope{nonce.low,nonce.high,1,1},request.data(),bytes);
    if(admission.mayCommitLocalState()) {
      if(admission.sequence!=h.sequence)throw std::runtime_error("D3D9 enqueue sequence disagreement");
      // Full and every other rejection leave sequence and caller state intact.
      sequence=uint32_t(h.sequence);
      ++producerAdmission.publishes;
      const auto settled=done.load(std::memory_order_acquire);
      if(settled>sequence)throw std::runtime_error("D3D9 settled prefix exceeds producer");
      maxObservedOutstanding=std::max(maxObservedOutstanding,sequence-settled);
      signalNotification(workAvailable.get());
    }
    return admission;
  }
  void publishUntil(d9::Header& h,const void* payload,ULONGLONG until) {
    const auto bytes=prepare(h,payload);
    bool waited=false;ULONGLONG began=0;AdmissionClock::time_point waitBegin{};
    const auto accountWait=[&] {
      if(!waited)return;
      const auto elapsed=GetTickCount64()-began;
      backpressureMs+=elapsed;maxBackpressureMs=std::max(maxBackpressureMs,uint64_t(elapsed));
      if(admissionProfile) {
        ++producerAdmission.backpressureEpisodes;
        producerAdmission.backpressureNs+=Nanoseconds(waitBegin,AdmissionClock::now());
      }
    };
    try {
      for(;;) {
        if(!healthy())throw std::runtime_error("D3D9 admission after terminal failure");
        if(GetTickCount64()>=until)throw std::runtime_error("D3D9 command admission deadline");
        const auto admission=tryPublishPrepared(h,bytes);
        if(admission.mayCommitLocalState()){accountWait();return;}
        if(admission.outcome!=commands::Publish::Full)
          throw std::runtime_error("D3D9 command admission refused");
        ++fullObservations;
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
        if(fullObservedForTest)fullObservedForTest->store(true,std::memory_order_release);
#endif
        if(!waited) {
          waited=true;began=GetTickCount64();++backpressureCalls;
          if(admissionProfile)waitBegin=AdmissionClock::now();
        }
        // Backpressure is explicit and bounded, not a successful dropped Clear.
        // The original immutable packet is retried; no new sequence or storage.
        waitNotification(producerProgress.get(),remainingWait(until));
      }
    } catch(...) {accountWait();throw;}
  }
  void acceptReply(const d9::Header& h,const commands::Batch& batch) {
    const auto hr=HRESULT(reply.result);
    const bool eventWait=h.op==d9::Op::WaitEvent&&asyncEvents;
    // Ordered state is asynchronous unless it is the reply a call() waits for.
    const bool orderedAsync=asyncDrawBatches&&AsyncOrderedOp(h.op)
      &&uint32_t(h.sequence)!=callSequence.load(std::memory_order_acquire);
    const bool asynchronous=h.op==d9::Op::Clear
      ||((h.op==d9::Op::DrawBatch||h.op==d9::Op::DrawFixedBatch||h.op==d9::Op::AnnotateDraws)&&asyncDrawBatches)
      ||orderedAsync||eventWait;
    const bool remoteLost=hr==D3DERR_DEVICELOST;
    if(remoteLost) {
      // Publish the failure BEFORE done. Async callers have no synchronous
      // return path in which to latch the remote terminal on the worker's behalf.
      deviceLost.store(true,std::memory_order_release);
      queue.requestStop();remoteTerminal=true;
    }
    const auto requestedSerial=uint64_t(h.args[2])|(uint64_t(h.args[3])<<32);
    if((asynchronous&&(hr!=D3D_OK||reply.payloadBytes))
      ||(eventWait&&hr==D3D_OK&&(reply.completed<requestedSerial
        ||reply.completed<eventGpuCompleted.load(std::memory_order_relaxed)))
      ||(h.op==d9::Op::Initialize&&SUCCEEDED(hr)&&(hr!=D3D_OK||reply.payloadBytes))
      ||(FAILED(hr)&&reply.payloadBytes)) {
      if(asynchronous) {
        asyncFailureSequence=h.sequence;asyncFailureResult=DWORD(hr);
      }
      // Exact, payload-free DEVICELOST is an acknowledged remote terminal,
      // never a successful Clear. All other asynchronous rejection/malformed
      // replies fail the session BEFORE ack/done and close the existing pipe.
      if(!remoteLost||reply.payloadBytes) {
        failClosed();throw std::runtime_error("D3D9 asynchronous/init reply contract");
      }
    }
    if(queue.acknowledgeCpu(batch.scope,batch.sequence)!=commands::CpuAck::Acknowledged) {
      failClosed();throw std::runtime_error("D3D9 CPU ack mismatch");
    }
    ++exchanges;
    if(h.op==d9::Op::Clear&&hr==D3D_OK)++clearRemoteSucceeded;
    if(h.op==d9::Op::DrawBatch&&hr==D3D_OK)++drawBatchesRemoteSucceeded;
    if(h.op==d9::Op::DrawFixedBatch&&hr==D3D_OK)++fixedBatchesRemoteSucceeded;
    if(orderedAsync&&hr==D3D_OK)++orderedRemoteSucceeded;
    if(eventWait&&hr==D3D_OK) {
      ++eventWaitsSucceeded;
      eventGpuCompleted.store(reply.completed,std::memory_order_release);
    }
    if(h.op==d9::Op::Initialize&&hr==D3D_OK)
      initializedAcknowledged.store(true,std::memory_order_release);
    // A settled remote reply prefix, not a GPU-ready or resource-retired fence.
    done.store(uint32_t(h.sequence),std::memory_order_release);
    signalNotification(producerProgress.get());
  }
  DWORD owner;
  ipc::Nonce nonce;
  commands::SpscRenderCommandQueue queue;
  // Construct before starting the worker; destroy only after its join in the
  // Connection destructor. Fixed handles, no per-call allocation or global lock.
  lab::Handle workAvailable,producerProgress;
  d9::Packet request{},replyPacket{};
  // Fixed caller-owned staging, separate from the four IPC queue slots. No
  // allocation per draw, and no changes to transport/admission deadlines.
  d9::DrawBatchPacket drawBatch{};
  d9::FixedDrawBatchPacket fixedBatch{};
  // WARVK_D3D9_ANNOTATIONS=1: the next draw's records, then the staged batch's.
  bool annotations=false;
  std::array<uint8_t,d9::MaxRecordsPerDraw*(d9::RecordHeaderBytes+d9::MaxAnnotationData)> nextRecords{};
  uint32_t nextRecordBytes=0,nextRecordCount=0;uint16_t nextLastSchema=0;
  std::array<uint8_t,d9::MaxDrawAnnotationBytes> batchRecords{};
  uint32_t batchRecordBytes=0,batchRecordCount=0;
  uint64_t annotatedDrawsAdmitted=0,annotationRecordsAdmitted=0,annotationMessages=0;
  uint64_t frameScenesSent=0,stageMarkersSent=0,annotationRecordsDiscarded=0;
  bool drawBatching=false,asyncDrawBatches=false,asyncEvents=false,groupedSubmissions=false,fixedBatching=false;
  uint32_t drawBatchCount=0,fixedBatchCount=0;
  uint64_t batchedDrawsAdmitted=0,drawBatchesFlushed=0,fixedDrawsAdmitted=0,fixedBatchesFlushed=0;
  std::atomic<uint64_t> eventGpuCompleted{0};
  uint32_t eventWaitSequence=0;
  uint64_t eventWaitsPublished=0,eventPendingObservations=0,eventLocalCompletions=0;
  d9::Reply reply{};
  std::atomic<uint32_t> done{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> deviceLost{false};
  std::atomic<bool> initializedAcknowledged{false};
  std::atomic<uint32_t> callSequence{0};
  uint32_t sequence=0;
  // Producer-owned until join; include the consumer's one private batch when
  // reporting outstanding work. The existing four-slot queue is not enlarged.
  uint64_t clearPublished=0,fullObservations=0,backpressureCalls=0,backpressureMs=0,maxBackpressureMs=0;
  uint64_t orderedUploadsPublished=0,orderedStatesPublished=0,orderedPassedStaged=0;
  uint32_t maxObservedOutstanding=0;
  // Worker-owned until join, no live unsynchronized diagnostics.
  uint64_t clearRemoteSucceeded=0,drawBatchesRemoteSucceeded=0,fixedBatchesRemoteSucceeded=0,asyncFailureSequence=0;
  uint64_t eventWaitsSucceeded=0,orderedRemoteSucceeded=0;
  DWORD asyncFailureResult=0;
  // Opt-in admission account (WARVK_D3D9_ADMISSION_PROFILE=1). Each thread
  // owns its counters and fixed samples; the destructor reads them only after
  // the worker join. Disabled connections take no clock reads.
  using AdmissionClock=std::chrono::steady_clock;
  static uint64_t Nanoseconds(AdmissionClock::time_point begin,AdmissionClock::time_point end) noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count());
  }
  struct WorkerAdmission {
    uint64_t messages=0,idleNs=0,writeNs=0,replyNs=0,batchMessages=0,batchWriteNs=0,batchReplyNs=0;
  };
  struct ProducerAdmission {
    uint64_t publishes=0,backpressureEpisodes=0,backpressureNs=0,calls=0,callWaitNs=0;
  };
  template<typename T>
  static void sampleAdmission(std::array<T,128>& samples,uint32_t& count,uint32_t& dropped,const T& value) noexcept {
    if(count==samples.size()){++dropped;return;}
    samples[count++]=value;
  }
  bool admissionProfile=false;
  WorkerAdmission workerAdmission;
  std::array<WorkerAdmission,128> workerSamples{};
  uint32_t workerSampleCount=0,workerSamplesDropped=0;
  ProducerAdmission producerAdmission;
  std::array<ProducerAdmission,128> producerSamples{};
  uint32_t producerSampleCount=0,producerSamplesDropped=0;
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
  // Observation-only synchronization for deterministic paused-consumer tests.
  // Null by default and absent from ordinary builds; no alternate queue logic.
  std::atomic<bool>* fullObservedForTest=nullptr;
  std::atomic<bool>* replyWaitObservedForTest=nullptr;
  std::atomic<bool>* workerWaitObservedForTest=nullptr;
  void (*beforeReplyWaitForTest)(Connection&)=nullptr;
#endif
  std::thread worker;
  DWORD childPid=0,childExit=STILL_ACTIVE;
  uint64_t childCreation=0,exchanges=0;
  uint64_t exitWaitMs=0;
  bool peerVerified=false;
  bool remoteTerminal=false,childNaturalExit=false;
  bool childProcessSettled=false,childSelfFailStop=false;
};
} // namespace warvk::host::d3d9proxy
