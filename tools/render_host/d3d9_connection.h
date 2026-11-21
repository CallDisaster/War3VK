#pragma once
#include <d3d9.h>
#include "d3d9_wire.h"
#include "d3d9_draw_wire.h"
#include "d3d9_texture_wire.h"
#include "d3d9_ff_delta_wire.h"
#include "d3d9_annotation_wire.h"
#include "d3d9_content_wire.h"
#include "render_command_queue.h"
#include "d3d9_ring_channel.h"
#include "d3d9_sync_profile.h"
#include "owned_child.h"
#include "d3d9_window_identity.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace warvk::host::d3d9proxy {
// Isolated test DLL owner, NOT the shipping asynchronous service. All COM calls
// are serialized on its creation thread; a worker owns blocking IPC/child drain.
// Clear and the explicit bounded-draw-batch profile may return after local
// admission. Every other remote call flushes the draw prefix and is an ordered
// synchronous barrier. This is still a laboratory owner: final
// joins/cancellation may block, and an external watchdog remains mandatory.
// One owner per nonce. Allocated only for either opt-in; sync creates and
// ordered creates share the same quota and only exact sync success proves a format.
struct AsyncResourceState {
  uint32_t features=0;
  d9::ResourceQuota quota;
  uint16_t attempted=0,proven=0;
  uint64_t destroys=0,creates=0,first=0,unproven=0,disabled=0;
  uint64_t invalid=0,outOfMemory=0,outOfVideoMemory=0,notAvailable=0;
  bool canCreate(uint16_t bit) const {return (features&d9::InitializeAsyncCreateTexture)&&(proven&bit);}
  void published(const d9::Header& h,bool sync) {
    if(h.op==d9::Op::Destroy){if(!sync)++destroys;return;}
    if(h.op!=d9::Op::CreateTexture2D)return;
    if(!sync){++creates;return;}
    if(!(features&d9::InitializeAsyncCreateTexture)){++disabled;return;}
    const auto bit=d9::TextureFormatBit(h.args[3]);
    if(bit&&!(attempted&bit))++first;else ++unproven;
    attempted|=bit;
  }
  void report() const {
    std::fprintf(stderr,"D3D9 async-resource-proxy: {\"schema\":\"warvk-d3d9-async-resource-proxy/v1\","
      "\"asyncDestroy\":%s,\"asyncCreateTexture\":%s,\"destroyPublished\":%llu,\"createTexturePublished\":%llu,"
      "\"syncTextureCreates\":%llu,\"firstFormat\":%llu,\"unprovenFormat\":%llu,\"switchOff\":%llu,"
      "\"localRejectedByHRESULT\":{\"D3DERR_INVALIDCALL\":%llu,\"E_OUTOFMEMORY\":%llu,"
      "\"D3DERR_OUTOFVIDEOMEMORY\":%llu,\"D3DERR_NOTAVAILABLE\":%llu}}\n",
      features&d9::InitializeAsyncDestroy?"true":"false",features&d9::InitializeAsyncCreateTexture?"true":"false",
      destroys,creates,first+unproven+disabled,first,unproven,disabled,invalid,outOfMemory,outOfVideoMemory,notAvailable);
  }
};
class Connection final {
  struct PreparedOnly { };
  Connection(PreparedOnly,ipc::Nonce scope) : owner(GetCurrentThreadId()),nonce(scope),
    queue(commands::Scope{nonce.low,nonce.high,1,1}),
    workAvailable(makeNotification()),producerProgress(makeNotification()) { }
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
  // The test drives this exact queue/admission/reply code without child/pipe/GPU.
  // No alternate worker or successful initialization is installed by this seam.
  friend struct ConnectionCpuTest;
  static inline const wchar_t* resourceFixtureArgument=nullptr;
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
    const auto strictSwitch=[](const wchar_t* name,bool prerequisite,const char* error) {
      wchar_t value[8]{};
      SetLastError(ERROR_SUCCESS);
      const DWORD length=GetEnvironmentVariableW(name,value,8);
      if(!length&&GetLastError()==ERROR_ENVVAR_NOT_FOUND)return false;
      if(length!=1||value[0]!=L'1'||!prerequisite)throw std::runtime_error(error);
      return true;
    };
    fixedDelta=strictSwitch(L"WARVK_D3D9_FIXED_DELTA",fixedBatching,"fixed delta requires value 1 and fixed batches");
    fixedDeltaVerify=strictSwitch(L"WARVK_D3D9_FIXED_DELTA_VERIFY",fixedDelta,"fixed delta verification requires value 1 and fixed delta");
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
    // WARVK_D3D9_COMMAND_RING=1: after Initialize every message travels on the
    // shared-memory ring (d3d9_command_ring.h) instead of one pipe round trip
    // each. WARVK_D3D9_COMMAND_RING_KIB overrides the 4 MiB default.
    wchar_t commandRing[8]{};
    const DWORD ringMode=GetEnvironmentVariableW(L"WARVK_D3D9_COMMAND_RING",commandRing,8);
    if(ringMode&&(ringMode!=1||commandRing[0]!=L'1'))throw std::runtime_error("unknown command ring profile");
    // The admission account samples per pipe reply; the ring has none.
    if(ringMode&&admissionProfile)throw std::runtime_error("command ring excludes the admission profile");
    ringRequested=ringMode!=0;
    wchar_t ringKib[16]{};
    const DWORD kibLength=GetEnvironmentVariableW(L"WARVK_D3D9_COMMAND_RING_KIB",ringKib,16);
    if(kibLength) {
      wchar_t* end=nullptr;const auto kib=kibLength<16?std::wcstoul(ringKib,&end,10):0;
      if(!ringRequested||!end||*end||kib>d9::ring::MaxCapacity/1024||!d9::ring::ValidCapacity(uint32_t(kib)*1024))
        throw std::runtime_error("command ring capacity: a power of two, 256..65536 KiB");
      ringCapacity=uint32_t(kib)*1024;
    }
    // WARVK_D3D9_HIGH_VA: where the ring's two x86 views go in a
    // large-address-aware game process (d3d9_high_va.h). Unset or "1": above
    // 2 GiB when the process has such addresses; "0": never; "2": as "1" but
    // without MapViewOfFile3, which exercises the address scan. A process
    // that is not large-address-aware maps as before whatever this says.
    wchar_t highVaText[8]{};
    const DWORD highVaLength=GetEnvironmentVariableW(L"WARVK_D3D9_HIGH_VA",highVaText,8);
    if(highVaLength&&(highVaLength!=1||highVaText[0]<L'0'||highVaText[0]>L'2'))
      throw std::runtime_error("unknown high-address profile");
    highVaSetting=highVaLength?uint32_t(highVaText[0]-L'0'):1;
    // P2 (docs/research/2026-11-18-x64-p2-async-design.md). Both ride on the
    // asynchronous profile's ordered admission: no reply, FIFO behind the
    // staged prefix, and a host refusal ends the session (AsyncRefused).
    //   WARVK_D3D9_ASYNC_UPLOADS=1  a texture level's Begin/Chunk/Commit and
    //                               SetLOD; x86 runs the host's admission rules.
    //   WARVK_D3D9_ASYNC_FLUSH=1    EndScene's FlushDraws and ClearRegions.
    const auto orderedSwitch=[&](const wchar_t* name) {
      wchar_t value[8]{};const DWORD length=GetEnvironmentVariableW(name,value,8);
      if(length&&(!asyncDrawBatches||length!=1||value[0]!=L'1'))
        throw std::runtime_error("ordered asynchronous profile requires asynchronous draw batches");
      return length!=0;
    };
    //   WARVK_D3D9_ASYNC_SCENE=1    FrameScene and StageMarker (ring only). A
    //                               host refusal of one is advisory, not
    //                               terminal: the host ignores every later
    //                               scene record and says so in its ring block;
    //                               the next scene record here is refused
    //                               locally, which stops the data layer as the
    //                               synchronous reply did.
    //   WARVK_D3D9_PRESENT_AHEAD=1  Present (ring only, no presented readback)
    //                               is published without waiting for it; the
    //                               call returns once the previous Present was
    //                               applied: x86 is at most one frame ahead.
    const uint32_t resourceBits=(strictSwitch(L"WARVK_D3D9_ASYNC_DESTROY",asyncDrawBatches,
      "async destroy requires value 1 and async batches")?d9::InitializeAsyncDestroy:0)
      |(strictSwitch(L"WARVK_D3D9_ASYNC_CREATE_TEXTURE",asyncDrawBatches,
        "async texture creation requires value 1 and async batches")?d9::InitializeAsyncCreateTexture:0);
    if(!d9::ValidResourceTransport(resourceBits,ringRequested,asyncDrawBatches))
      throw std::runtime_error("asynchronous resources require the command ring and asynchronous batches");
    if(resourceBits){resourceState=std::make_unique<AsyncResourceState>();resourceState->features=resourceBits;}
    asyncUploads=orderedSwitch(L"WARVK_D3D9_ASYNC_UPLOADS");
    asyncFlush=orderedSwitch(L"WARVK_D3D9_ASYNC_FLUSH");
    asyncScene=orderedSwitch(L"WARVK_D3D9_ASYNC_SCENE");
    presentAhead=orderedSwitch(L"WARVK_D3D9_PRESENT_AHEAD");
    if((asyncScene||presentAhead)&&!ringRequested)
      throw std::runtime_error("asynchronous scene records and Present require the command ring");
    // An offscreen session never presents: the host answers its Present with
    // E_NOTIMPL. That is a reply to wait for, not a frame to run ahead of, so
    // the switch moves nothing there.
    presentAhead=presentAhead&&presenter;
    // P0 measurement only: periodic sync-profile snapshots (0: at exit only).
    profileEvery=d9::profile::Every();profileNextSnapshot=profileEvery;
    wchar_t exe[32768]{},log[32768]{};
    const DWORD a=GetEnvironmentVariableW(L"WARVK_D3D9_HOST_EXE",exe,32768);
    const DWORD b=GetEnvironmentVariableW(L"WARVK_D3D9_HOST_LOG",log,32768);
    // Game.dll only under the sandbox game profile (d3d9_window_identity.h).
    if(!a||a>=32768||!b||b>=32768||(GetModuleHandleW(L"Game.dll")&&!d9::GameSandboxProfile()))
      throw std::runtime_error("D3D9 development host/log required; game prohibited");
    const auto desktop=presenter?d9::RequireIsolatedDesktop():std::wstring();
    // The host log is CREATE_NEW. The game makes its Direct3D9 object more than
    // once, unloading this DLL in between, so in a game process each later
    // host takes the next free <log>.<n>. The laboratory stays strict.
    std::wstring hostLog(log);
    if(GetModuleHandleW(L"Game.dll"))
      for(uint32_t n=1;n<256&&GetFileAttributesW(hostLog.c_str())!=INVALID_FILE_ATTRIBUTES;++n)
        hostLog=std::wstring(log)+L"."+std::to_wstring(n);
    hostLogPath=hostLog;
    worker=std::thread([this,path=std::wstring(exe),out=hostLog,desktop] {
      try {
        if(!desktop.empty()&&d9::RequireIsolatedDesktop()!=desktop)
          throw std::runtime_error("D3D9 worker desktop mismatch");
        // The host adopts an inherited handle to this process: it cannot open
        // a game process by PID (win32_transport.h, AdoptPeer).
        auto self=lab::ShareSelf();
        auto args=lab::NonceText(nonce)+L" "+std::to_wstring(GetCurrentProcessId())
          +L" "+std::to_wstring(lab::CreationTime(GetCurrentProcess()))
          +L" "+std::to_wstring(reinterpret_cast<uintptr_t>(self.get()));
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
        if(resourceFixtureArgument)args+=std::wstring(L" ")+resourceFixtureArgument;
#endif
        // The author's trial runs the host at the game's priority; every other
        // session keeps it BelowNormal.
        lab::OwnedChild child(path,args,out,desktop,self.get(),d9::AuthorTrialProfile());
        self.reset();
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
              if(h.op==d9::Op::DrawBatch||d9::IsFixedBatch(h.op)){++a.batchMessages;a.batchWriteNs+=writeNs;a.batchReplyNs+=replyNs;}
            }
            acceptReply(h,batch);
            if(h.op==d9::Op::AttachRing&&HRESULT(reply.result)==D3D_OK){watchRing(child.process());break;}
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
            hostExit.store(childExit,std::memory_order_release);
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
    const bool ringClean=ring&&ringFinish.load(std::memory_order_acquire)==1&&!failed.load(std::memory_order_acquire);
    if(ring) {
      // Ring records have no per-message reply. The host stops at the first
      // asynchronous record it refuses, so an executed Finish proves every
      // earlier record succeeded; without it nothing is claimed.
      exchanges+=ringClean?ringRecords:ringApplied-ringAttachSequence;
      if(ringClean) {
        // Barriers were counted from their own replies (ringCall).
        clearRemoteSucceeded+=ringAsyncClears;drawBatchesRemoteSucceeded+=ringAsyncDrawBatches;
        fixedBatchesRemoteSucceeded+=ringAsyncFixedBatches;orderedRemoteSucceeded=orderedUploadsPublished+orderedStatesPublished;
      }
    }
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
      settledSequence(),asyncFailureSequence,asyncFailureResult);
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
    // Separate versioned line: ordered-async/v1 above keeps its own identity
    // (remoteSucceeded == uploadsPublished + statesPublished).
    if(resourceState)resourceState->report();
    if(asyncUploads||asyncFlush||asyncScene||presentAhead)
      std::fprintf(stderr,"D3D9 ordered-extra: {\"schema\":\"warvk-d3d9-ordered-extra/v1\",\"asyncUploads\":%s,"
        "\"asyncFlush\":%s,\"asyncScene\":%s,\"presentAhead\":%s,\"texturePublished\":%llu,\"flushPublished\":%llu,"
        "\"scenePublished\":%llu,\"sceneRefusalsObserved\":%llu,\"presentsAhead\":%llu,\"presentAheadWaits\":%llu,"
        "\"presentAheadSettles\":%u,\"remoteSucceeded\":%llu}\n",
        asyncUploads?"true":"false",asyncFlush?"true":"false",asyncScene?"true":"false",presentAhead?"true":"false",
        orderedTexturePublished,orderedFlushPublished,orderedScenePublished,sceneRefusalsObserved,presentsAhead,
        presentAheadWaits,presentAheadSettles.load(std::memory_order_relaxed),
        ringClean?orderedTexturePublished+orderedFlushPublished+orderedScenePublished+presentsAhead
          :orderedExtraRemoteSucceeded);
    // Separate versioned line: the frozen draw-batch-proxy/v1 schema above
    // counts programmable DrawBatch traffic only.
    std::fprintf(stderr,"D3D9 fixed-batch-proxy: {\"schema\":\"warvk-d3d9-fixed-batch-proxy/v1\","
      "\"enabled\":%s,\"asyncAdmission\":%s,\"admittedDraws\":%llu,\"flushedBatches\":%llu,"
      "\"pendingDraws\":%u,\"remoteSucceededBatches\":%llu,\"capacity\":%u,\"payloadCapacityBytes\":%zu}\n",
      fixedBatching?"true":"false",fixedBatching&&asyncDrawBatches?"true":"false",fixedDrawsAdmitted,fixedBatchesFlushed,
      fixedBatchCount,fixedBatchesRemoteSucceeded,d9::MaxFixedDrawBatch,sizeof(d9::FixedDrawBatchPacket));
    if(fixedDelta) {
      uint64_t messages=0,records=0;
      for(size_t i=0;i<deltaHistogram.size();++i){messages+=deltaHistogram[i];records+=(i+1)*deltaHistogram[i];}
      char text[1024];d9::profile::Line line(text,sizeof(text));
      line.add("D3D9 fixed-delta-proxy: {\"schema\":\"warvk-d3d9-fixed-delta-proxy/v1\","
        "\"enabled\":true,\"verify\":%s,\"messages\":%llu,\"records\":%llu,\"fullBytes\":%llu,\"sentBytes\":%llu,\"batchHistogram\":[",
        fixedDeltaVerify?"true":"false",messages,records,records*d9::FixedDrawBytes,deltaSentBytes);
      for(size_t i=0;i<deltaHistogram.size();++i)line.add("%s%llu",i?",":"",deltaHistogram[i]);
      line.close("]}\n");line.write(stderr);
    }
    if(annotations)
      std::fprintf(stderr,"D3D9 annotation-proxy: {\"schema\":\"warvk-d3d9-annotation-proxy/v1\","
        "\"annotatedDraws\":%llu,\"records\":%llu,\"messages\":%llu,\"frameScenes\":%llu,\"stageMarkers\":%llu,"
        "\"discardedRecords\":%llu,\"sessionResetsRequested\":%llu,\"sessionResetsSent\":%llu,"
        "\"sessionResetsRefused\":%llu}\n",annotatedDrawsAdmitted,annotationRecordsAdmitted,annotationMessages,
        frameScenesSent,stageMarkersSent,annotationRecordsDiscarded,sessionResetsRequested,sessionResetsSent,
        sessionResetsRefused);
    if(annotations)
      std::fprintf(stderr,"D3D9 content-proxy: {\"schema\":\"warvk-d3d9-content-proxy/v1\",\"uploads\":%llu,\"chunks\":%llu,"
        "\"uploadedBytes\":%llu,\"releases\":%llu,\"reads\":%llu,\"readBytes\":%llu,\"refs\":%llu,\"localRefusals\":%llu,"
        "\"blobs\":%llu,\"bytes\":%llu,\"cleared\":%llu,\"lastRevision\":%llu}\n",
        contentUploadsSent,contentChunksSent,contentBytesSent,contentReleasesSent,contentReadsSent,contentBytesRead,
        contentRefsAdmitted,contentLocalRefusals,static_cast<unsigned long long>(contentIndex.count()),contentIndex.bytes(),
        contentIndex.clearedBlobs,contentIndex.lastRevision());
    if(ringRequested) {
      std::fprintf(stderr,"D3D9 command-ring-proxy: {\"schema\":\"warvk-d3d9-command-ring-proxy/v1\",\"attached\":%s,"
        "\"capacity\":%u,\"attachSequence\":%llu,\"records\":%llu,\"bytes\":%llu,\"syncCalls\":%llu,\"parks\":%llu,"
        "\"wakes\":%llu,\"published\":%u,\"applied\":%llu,\"finishExecuted\":%s,\"noProgressLimitMs\":%u}\n",
        ring?"true":"false",ringCapacity,ringAttachSequence,ringRecords,ringBytes,ringSyncCalls,ringParks,ringWakes,
        sequence,ringApplied,ringClean?"true":"false",ringNoProgressMs);
      // The first terminal cause of this session, or none.
      std::fprintf(stderr,"D3D9 terminal: {\"schema\":\"warvk-d3d9-terminal/v1\",\"kind\":\"%s\",\"result\":%lu,"
        "\"detail\":\"%s\",\"hostExit\":%lu,\"published\":%u,\"applied\":%llu}\n",
        TerminalName(Terminal(terminalKind.load(std::memory_order_acquire))),DWORD(terminal.result),terminal.detail,
        childExit,sequence,ring?ringApplied:uint64_t(settledSequence()));
    }
    // P0 measurement only: the cumulative account of this session, once.
    if(sequence)profileSnapshot("exit");
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
  // The device window is going (WM_DESTROY, on the window's thread: the
  // owner's in the game, another one in the laboratory). The Present that may
  // still be ahead of the host needs that window and its session mark, and no
  // later call would wait for it in time. This reads the host's published
  // progress only, so any thread may call it and no producer state changes. A
  // stopped host, a terminal session or PresentSettleMs end the wait; the
  // owner's next call reports those as it would have.
  static constexpr ULONGLONG PresentSettleMs=5000;
  void settlePresentAhead() noexcept {
    const uint64_t sequence=presentAheadSequence.load(std::memory_order_acquire);
    if(!sequence||!ring||ring->peekApplied()>=sequence)return;
    presentAheadSettles.fetch_add(1,std::memory_order_relaxed);
    const auto until=GetTickCount64()+PresentSettleMs;
    while(ring->peekApplied()<sequence&&healthy()&&ring->hostState().state!=d9::ring::State::Faulted
      &&GetTickCount64()<until)Sleep(1);
  }
  // P0 measurement only (warvk-d3d9-sync-profile/v1). Owner thread: names the
  // D3D9 or data-layer entry the messages sent inside it belong to. The
  // outermost entry wins; `own` (the data layer's exports) always takes it.
  using SyncApi=d9::profile::SyncApi;
  class ProfileScope {
  public:
    ProfileScope(Connection& c,SyncApi api,bool own=false) noexcept
      : connection(c),previous(c.profileApi),entered(c.isOwner()&&(own||c.profileApi==SyncApi::Other)) {
      if(!entered)return;
      connection.profileApi=api;++connection.profileApis[size_t(api)].entries;
    }
    ~ProfileScope() { if(entered)connection.profileApi=previous; }
    ProfileScope(const ProfileScope&)=delete;
    ProfileScope& operator=(const ProfileScope&)=delete;
  private:
    Connection& connection;SyncApi previous;bool entered;
  };
  // Owner thread, after each D3D9 Present: a snapshot every N Present
  // messages (WARVK_D3D9_SYNC_PROFILE_EVERY), written to stderr.
  // Also keeps the time between successive D3D9 Present returns (the game
  // thread's frame time) in a fixed ring; each snapshot prints the frames
  // since the previous one (at most the ring's size, the rest counted dropped).
  void profilePresented() noexcept {
    if(!isOwner())return;
    const auto now=d9::profile::Ticks();
    if(profileLastPresent&&now) {
      const auto us=d9::profile::Micros(uint64_t(now-profileLastPresent),profileFrequency);
      profileFrameUs[profileFrames++%profileFrameUs.size()]=uint32_t(std::min<uint64_t>(us,UINT32_MAX));
    }
    profileLastPresent=now;
    if(!profileEvery||profilePresents<profileNextSnapshot)return;
    while(profileNextSnapshot<=profilePresents)profileNextSnapshot+=profileEvery;
    profileSnapshot("periodic");
  }
  // Owner thread, after a texture level's Begin/Chunk.../Commit succeeded:
  // first uploads are only totalled; updates of an initialized level are kept
  // per (id, generation, level) in a fixed table, overflow counted untracked.
  // creator: the return address of the game's CreateTexture call; usage/pool
  // as created. stage: the last StageMarker sent before the update.
  void profileTextureUpload(uint32_t id,uint32_t generation,uint32_t level,uint32_t width,uint32_t height,
      uint32_t format,uint32_t levels,uint32_t bytes,uint32_t chunks,bool initial,bool partial,uintptr_t creator,
      uint32_t usage,uint32_t pool) noexcept {
    if(!isOwner())return;
    profileTextureChunks+=chunks;
    if(initial){++profileTextureInitial;profileTextureInitialBytes+=bytes;return;}
    ++profileTextureUpdates;profileTextureUpdateBytes+=bytes;
    const uint32_t hash=(id*16u+level)*2654435761u;
    for(size_t probe=0;probe<ProfileTextureProbes;++probe) {
      auto& t=profileTextures[(hash+probe)%ProfileTextureSlots];
      if(!t.updates) {
        t.id=id;t.generation=generation;t.level=level;t.width=width;t.height=height;t.format=format;t.levels=levels;
        t.creator=creator;t.usage=usage;t.pool=pool;
      } else if(t.id!=id||t.generation!=generation||t.level!=level)continue;
      ++t.updates;t.partial+=partial;t.bytes+=bytes;t.chunks+=chunks;t.stage=profileStage;
      t.frameScene=frameScenesSent;return;
    }
    ++profileTextureUntracked;
  }
  bool healthy() const noexcept { return !failed.load(std::memory_order_acquire)&&!deviceLost.load(std::memory_order_acquire); }
  // Why the session ended; the first cause wins and later ones are ignored.
  // (docs/plan/2026-11-13-x64-phase1-transport-design.md, section 5.)
  enum class Terminal : uint32_t { None, LocalContract, HostExited, HostHung, HostRejected, HostDeviceLost, RingCorrupt };
  static const char* TerminalName(Terminal kind) noexcept {
    switch(kind) {
      case Terminal::None:return "none";
      case Terminal::LocalContract:return "local-contract";
      case Terminal::HostExited:return "host-exited";
      case Terminal::HostHung:return "host-hung";
      case Terminal::HostRejected:return "host-rejected";
      case Terminal::HostDeviceLost:return "host-device-lost";
      case Terminal::RingCorrupt:return "ring-corrupt";
    }
    return "unknown";
  }
  struct TerminalReport { Terminal kind=Terminal::None;uint32_t result=0;ULONGLONG tick=0;char detail[48]{}; };
  // The one place policy attaches (safe exit and a report, in the product).
  // Called once, on the thread that found the cause; it must only register the
  // fact and return. Install before the first call.
  using TerminalHook=void(*)(void* context,const TerminalReport& report);
  void setTerminalHook(TerminalHook hook,void* context) noexcept {
    terminalContext.store(context,std::memory_order_release);terminalHook.store(hook,std::memory_order_release);
  }
  bool terminalReport(TerminalReport& result) const noexcept {
    if(!terminalReady.load(std::memory_order_acquire))return false;
    result=terminal;return true;
  }
  // Owner thread: where the command stream stood, for a terminal report. The
  // host's last published progress is read once more - its region outlives it.
  struct SessionMarks {
    uint64_t published=0,applied=0;uint32_t hostExit=STILL_ACTIVE;const wchar_t* hostLog=L"";
  };
  SessionMarks sessionMarks() noexcept {
    if(ring){d9::ring::Producer::Progress now;if(ring->observe(now))ringApplied=now.applied;}
    return {sequence,ring?ringApplied:uint64_t(settledSequence()),hostExit.load(std::memory_order_acquire),hostLogPath.c_str()};
  }
  bool asynchronousEvents() const noexcept { return asyncEvents; }
  struct DiagnosticIdentity {
    uint64_t nonceLow=0,nonceHigh=0,sequence=0,childCreation=0;
    uint32_t childPid=0;
  };
  // Cold, owner-only snapshot after a completed call. The existing done acquire
  // publishes the worker's actual child identity; no extra transport or wait.
  bool diagnosticIdentity(DiagnosticIdentity& result) const noexcept {
    if(!isOwner()||!healthy()||!sequence||settledSequence()!=sequence)return false;
    if(!childPid||!childCreation)return false;
    result={nonce.low,nonce.high,sequence,childCreation,childPid};return true;
  }
  // Local metadata disagreement or an unacknowledged destroy cannot be healed
  // by recycling a handle. This is a local fault, not a fabricated remote
  // DEVICELOST receipt or proof of GPU retirement.
  void failClosed() noexcept {
    // A blocked resource publisher can unwind through draw/batch admission.
    // Preserve an acknowledged device loss when those outer catches stop it.
    if(resourceState&&isOwner()&&deviceLost.load(std::memory_order_acquire)&&ring
      &&ring->hostState().code==d9::ring::Fault::DeviceLost)return;
    failClosed(Terminal::LocalContract,nullptr);
  }
  void failClosed(Terminal kind,const char* detail,uint32_t result=0) noexcept {
    noteTerminal(kind,detail,result);
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
    if(!usable())return D3DERR_DEVICELOST;
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
    if(!usable())return D3DERR_DEVICELOST;
    if(!initializedAcknowledged.load(std::memory_order_acquire))return D3DERR_INVALIDCALL;
    if(drawBatchCount){const auto prior=flushDraws();if(FAILED(prior))return prior;}
    if(fixedBatchCount>=d9::MaxFixedDrawBatch){failClosed();return D3DERR_DEVICELOST;}
    if(fixedDelta) {
      if(!d9::FixedDeltaHasRoom(deltaBytes,fixedDeltaVerify)) {
        const auto prior=flushDraws();if(FAILED(prior))return prior;
      }
      if(!fixedBatchCount)deltaPrevious.fill(0);
      const auto bytes=d9::EncodeFixedDeltaRecord(packet,deltaPrevious,fixedBatch.data()+deltaBytes,
        uint32_t(fixedBatch.size())-deltaBytes,fixedDeltaVerify);
      if(!bytes){failClosed();return D3DERR_DEVICELOST;}
      deltaBytes+=bytes;
    } else std::memcpy(fixedBatch.data()+fixedBatchCount*d9::FixedDrawBytes,packet.data(),packet.size());
    attachNextRecords(fixedBatchCount);
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
  // P2 additions to the ordered set, each behind its own profile switch.
  static bool AsyncUploadOp(d9::Op op) noexcept {
    return op==d9::Op::BeginTextureUpload||op==d9::Op::TextureUploadChunk||op==d9::Op::CommitTextureUpload
      ||op==d9::Op::TextureLod;
  }
  static bool AsyncFlushOp(d9::Op op) noexcept { return op==d9::Op::FlushDraws||op==d9::Op::ClearRegions; }
  static bool AsyncSceneOp(d9::Op op) noexcept { return op==d9::Op::FrameScene||op==d9::Op::StageMarker; }
  bool orderedOp(d9::Op op) const noexcept {
    return AsyncOrderedOp(op)||(asyncUploads&&AsyncUploadOp(op))||(asyncFlush&&AsyncFlushOp(op))
      ||(asyncScene&&AsyncSceneOp(op));
  }
  AsyncResourceState* asyncResources() const noexcept {return resourceState.get();}
  uint32_t resourceFeatures() const noexcept {return resourceState?resourceState->features:0;}
  bool resourceUsable() noexcept {return isOwner()&&usable();}
  HRESULT localResourceRefusal(HRESULT hr) noexcept {
    if(resourceState&&isOwner()) {
      if(hr==D3DERR_INVALIDCALL)++resourceState->invalid;
      else if(hr==E_OUTOFMEMORY)++resourceState->outOfMemory;
      else if(hr==D3DERR_OUTOFVIDEOMEMORY)++resourceState->outOfVideoMemory;
      else if(hr==D3DERR_NOTAVAILABLE)++resourceState->notAvailable;
    }
    return hr;
  }
  bool asyncUploadsEnabled() const noexcept { return asyncUploads; }
  bool asyncFlushEnabled() const noexcept { return asyncFlush; }
  // passStaged is for NOOVERWRITE content only: by the Lock contract no staged
  // draw reads its range, so it may reach the host before them. Everything
  // else first flushes the staged prefix, as call() does.
  HRESULT enqueueOrdered(d9::Header h,const void* payload,bool passStaged=false) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    const auto feature=d9::AsyncResourceFeature(h.op);
    if(feature&&(!ring||!(resourceFeatures()&feature)
      ||(h.op==d9::Op::CreateTexture2D&&!resourceState->canCreate(d9::TextureFormatBit(h.args[3])))))return D3DERR_INVALIDCALL;
    if(!asyncDrawBatches||!initializedAcknowledged.load(std::memory_order_acquire)||(!feature&&!orderedOp(h.op))
      ||(passStaged&&h.op!=d9::Op::UploadBuffer))return D3DERR_INVALIDCALL;
    const bool staged=drawBatchCount||fixedBatchCount;
    if(!passStaged){const auto prior=flushDraws();if(FAILED(prior))return prior;}
    try {
      publishUntil(h,payload,GetTickCount64()+lab::DeadlineMs);
      if(h.op==d9::Op::UploadBuffer)++orderedUploadsPublished;
      else if(AsyncOrderedOp(h.op))++orderedStatesPublished;
      else if(AsyncUploadOp(h.op))++orderedTexturePublished;
      else if(AsyncSceneOp(h.op))++orderedScenePublished;
      else if(!feature)++orderedFlushPublished;
      if(passStaged&&staged)++orderedPassedStaged;
      return (feature?usable():healthy())?D3D_OK:D3DERR_DEVICELOST;
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
    if(!usable())return D3DERR_DEVICELOST;
    if(!annotations||!initializedAcknowledged.load(std::memory_order_acquire)
      ||nextRecordCount>=d9::MaxRecordsPerDraw||(nextRecordCount&&schema<=nextLastSchema)
      ||!d9::CompanionFollowsCaster(schema,nextRecordCount?nextLastSchema:0))return D3DERR_INVALIDCALL;
    // A content reference names a live blob of its kind, as the host will require.
    const auto contentKind=d9::ContentKindOf(schema);
    if(contentKind&&(!data||bytes!=d9::ContentRefBytes
        ||!contentIndex.bound(d9::DecodeContentRef(static_cast<const uint8_t*>(data)),contentKind))) {
      ++contentLocalRefusals;return D3DERR_INVALIDCALL;
    }
    const auto size=d9::AppendDrawRecord(nextRecords.data()+nextRecordBytes,
      uint32_t(nextRecords.size()-nextRecordBytes),0,schema,data,bytes);
    if(!size)return D3DERR_INVALIDCALL;
    if(contentKind)++contentRefsAdmitted;
    nextRecordBytes+=size;++nextRecordCount;nextLastSchema=schema;return D3D_OK;
  }
  void discardNextDrawAnnotations() noexcept {
    annotationRecordsDiscarded+=nextRecordCount;nextRecordBytes=nextRecordCount=0;
  }
  // Ordered barriers like any other non-draw call: the staged prefix and its
  // records reach the host first.
  HRESULT frameScene(uint32_t schema,const void* data,uint32_t bytes) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    if(!annotations||!d9::ValidFrameScene(schema,static_cast<const uint8_t*>(data),bytes))return D3DERR_INVALIDCALL;
    d9::Header h;h.op=d9::Op::FrameScene;h.args[0]=schema;h.payloadBytes=bytes;
    const auto hr=sceneSend(h,data);if(hr==D3D_OK)++frameScenesSent;return hr;
  }
  // WARVK_D3D9_ASYNC_SCENE, once the ring is attached: the record is ordered
  // like any other and gets no reply. A refusal the host published for an
  // earlier scene record is returned for this one, before anything is sent;
  // the host has ignored every scene record since that one.
  HRESULT sceneSend(const d9::Header& h,const void* data) noexcept {
    if(!asyncScene||!ring)return call(h,data);
    if(ring->sceneRefused()){++sceneRefusalsObserved;return D3DERR_INVALIDCALL;}
    return enqueueOrdered(h,data);
  }
  HRESULT stageMarker(uint32_t stage) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    if(!annotations||!d9::ValidStage(stage))return D3DERR_INVALIDCALL;
    d9::Header h;h.op=d9::Op::StageMarker;h.args[0]=stage;
    const auto hr=sceneSend(h,nullptr);if(hr==D3D_OK)++stageMarkersSent;
    if(hr==D3D_OK)profileStage=stage; // P0 measurement only
    return hr;
  }
  // Any thread: the data layer may learn of a map change off the render
  // thread. Nothing is sent here; the owner sends the newest request ahead of
  // its next Present (call), so the host sees it at a frame boundary. A later
  // generation replaces an unsent one.
  HRESULT requestSessionReset(uint32_t reason,uint64_t generation) noexcept {
    if(!annotations||!generation||(reason!=uint32_t(d9::SessionResetReason::MapTransition)
      &&reason!=uint32_t(d9::SessionResetReason::GameExit)))return D3DERR_INVALIDCALL;
    std::lock_guard<std::mutex> lock(sessionResetMutex);
    if(generation<=sessionResetGeneration)return D3DERR_INVALIDCALL;
    sessionResetReason=reason;sessionResetGeneration=generation;++sessionResetsRequested;
    sessionResetPending.store(true,std::memory_order_release);return D3D_OK;
  }
  // Content payloads (d3d9_content_wire.h): ordered barriers, so the staged
  // prefix reaches the host first. x86 runs the host's own admission rules on
  // its index and keeps no content bytes. What it sends the host accepts: the
  // asynchronous profile takes no replies for it and a refusal is terminal.
  // The blob's revision is returned for the draw records that will name it.
  HRESULT uploadContent(uint32_t kind,uint64_t key,const void* data,uint32_t bytes,uint64_t* revision) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    d9::ContentChunk chunk{kind,key,contentIndex.lastRevision()+1,bytes,0,0};
    const auto* source=static_cast<const uint8_t*>(data);
    // Records already taken for the next draw could name what this replaces.
    if(!annotations||!initializedAcknowledged.load(std::memory_order_acquire)||nextRecordCount||!revision
      ||!d9::ValidContentKind(kind)||!contentIndex.canOpen(key,bytes)||!d9::ValidContentPayload(kind,0,source,bytes)) {
      ++contentLocalRefusals;return D3DERR_INVALIDCALL;
    }
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    for(;chunk.offset<bytes;chunk.offset+=chunk.count) {
      chunk.count=std::min(d9::MaxPayload,bytes-chunk.offset);
      if(contentIndex.admit(chunk)==d9::ContentIndex::Admit::Refused
          ||contentSend(d9::EncodeContentUpload(chunk),source+chunk.offset)!=D3D_OK) {
        if(healthy())failClosed();
        return D3DERR_DEVICELOST;
      }
      ++contentChunksSent;
    }
    ++contentUploadsSent;contentBytesSent+=bytes;*revision=chunk.revision;return D3D_OK;
  }
  HRESULT releaseContent(uint64_t key,uint64_t revision) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    const d9::ContentRef ref{key,revision};
    if(!annotations||nextRecordCount||!contentIndex.find(ref)){++contentLocalRefusals;return D3DERR_INVALIDCALL;}
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    if(!contentIndex.release(ref)||contentSend(d9::EncodeContentRelease(ref),nullptr)!=D3D_OK) {
      if(healthy())failClosed();
      return D3DERR_DEVICELOST;
    }
    ++contentReleasesSent;return D3D_OK;
  }
  // The host's stored bytes; x86 has none to answer from.
  HRESULT readContent(uint64_t key,uint64_t revision,uint32_t offset,void* out,uint32_t count) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    const d9::ContentRef ref{key,revision};const auto* meta=annotations?contentIndex.find(ref):nullptr;
    if(!meta||!out||!count||count>d9::MaxPayload||offset>meta->bytes||count>meta->bytes-offset) {
      ++contentLocalRefusals;return D3DERR_INVALIDCALL;
    }
    if(call(d9::EncodeContentRead(ref,offset,count),nullptr,out,count)!=D3D_OK) {
      if(healthy())failClosed();
      return D3DERR_DEVICELOST;
    }
    ++contentReadsSent;contentBytesRead+=count;return D3D_OK;
  }
  HRESULT flushDraws() noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    // At most one kind is staged: each submit flushes the other kind first.
    if(fixedBatchCount) {
      const auto op=fixedDelta?d9::Op::DrawFixedDeltaBatch:d9::Op::DrawFixedBatch;
      const auto bytes=fixedDelta?deltaBytes:fixedBatchCount*d9::FixedDrawBytes;
      const auto hr=publishStaged(op,fixedBatch.data(),fixedBatchCount,bytes,fixedBatchesFlushed);
      if(!fixedBatchCount)deltaBytes=0;
      return hr;
    }
    if(!drawBatchCount)return D3D_OK;
    return publishStaged(d9::Op::DrawBatch,drawBatch.data(),drawBatchCount,drawBatchCount*d9::DrawBytes,drawBatchesFlushed);
  }
  HRESULT endScene() noexcept {
    const auto result=flushDraws();if(FAILED(result)||!groupedSubmissions)return result;
    d9::Header h;h.op=d9::Op::FlushDraws;return asyncFlush?enqueueOrdered(h,nullptr):call(h);
  }
  HRESULT enqueueClear(d9::Header h) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
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
    if(!usable())return D3DERR_DEVICELOST;
    const auto serial=uint64_t(h.args[2])|(uint64_t(h.args[3])<<32);
    if(!asyncEvents||!initializedAcknowledged.load(std::memory_order_acquire)
      ||h.op!=d9::Op::Poll||!d9::ValidRef({h.id,h.generation})||h.payloadBytes
      ||(!h.args[0]&&!h.args[1])||!serial)return D3DERR_INVALIDCALL;
    for(size_t i=4;i<h.args.size();++i)if(h.args[i])return D3DERR_INVALIDCALL;
    const auto prior=flushDraws();if(FAILED(prior))return prior;
    if(ring&&!ringSettleEvent())return D3DERR_DEVICELOST;
    const auto settled=settledSequence();
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
      eventWaitSerial=serial;eventWaitSettled=false;
      return healthy()?S_FALSE:D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  HRESULT call(d9::Header h,const void* payload=nullptr,void* result=nullptr,uint32_t resultSize=0) noexcept {
    if(!isOwner())return D3DERR_INVALIDCALL;
    if(!usable())return D3DERR_DEVICELOST;
    if(h.op==d9::Op::Initialize)h.args[6]|=(fixedDelta?d9::InitializeFixedDelta:0)|resourceFeatures();
    // A read-only diagnostic must not implicitly submit locally staged draws.
    // Caller may explicitly EndScene/flush first, then observe the ordered
    // remote prefix. A refusal changes neither sequence nor destination bytes.
    if(h.op==d9::Op::DiagnosticCheckpoint&&(drawBatchCount||fixedBatchCount))return D3DERR_INVALIDCALL;
    // A requested session reset precedes the Present that ends its frame. A
    // host refusal is counted and the request dropped; the frame still ends.
    if(h.op==d9::Op::Present&&sessionResetPending.load(std::memory_order_acquire)) {
      d9::Header reset;reset.op=d9::Op::SessionReset;
      {
        std::lock_guard<std::mutex> lock(sessionResetMutex);
        reset.args[0]=sessionResetReason;reset.args[1]=uint32_t(sessionResetGeneration);
        reset.args[2]=uint32_t(sessionResetGeneration>>32);
        sessionResetPending.store(false,std::memory_order_release);
      }
      const auto hr=call(reset);
      if(hr==D3D_OK) {
        // The host dropped every blob with the map; so do the index and any
        // record still waiting for a draw, which could name one of them.
        ++sessionResetsSent;contentIndex.clear();discardNextDrawAnnotations();
      } else ++sessionResetsRefused;
      if(!usable())return D3DERR_DEVICELOST;
    }
    // Draw records precede their own batch; publishNextRecords flushes first.
    if(h.op!=d9::Op::DrawBatch&&!d9::IsFixedBatch(h.op)&&h.op!=d9::Op::AnnotateDraws) {
      const auto prior=flushDraws();if(FAILED(prior))return prior;
    }
    if(ring&&presentAhead&&h.op==d9::Op::Present&&!result&&!resultSize)return ringPresentAhead(h);
    if(ring)return ringCall(h,payload,result,resultSize);
    try {
      // One original 30-second call budget includes the new admission wait.
      // Admission is additionally capped at 5 seconds; publication does not
      // restart the reply clock. Worker framing/primary-exit stay 5000 ms.
      const auto deadlines=callDeadlinesAt(GetTickCount64());
      // The worker keeps this reply recoverable, also for ordered state.
      callSequence.store(sequence+1,std::memory_order_release);
      publishUntil(h,payload,deadlines.admission);
      const auto waitBegin=admissionProfile?AdmissionClock::now():AdmissionClock::time_point{};
      const auto profileBegin=d9::profile::Ticks();
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
      profileSync(h.op,profileBegin);
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
      if(hr==D3D_OK&&h.op==d9::Op::Initialize&&ringRequested)return attachRing();
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
  void noteTerminal(Terminal kind,const char* detail,uint32_t result) noexcept {
    uint32_t none=0;
    if(!terminalKind.compare_exchange_strong(none,uint32_t(kind),std::memory_order_acq_rel))return;
    terminal.kind=kind;terminal.result=result;terminal.tick=GetTickCount64();
    if(detail)std::snprintf(terminal.detail,sizeof(terminal.detail),"%s",detail);
    terminalReady.store(true,std::memory_order_release);
    if(const auto hook=terminalHook.load(std::memory_order_acquire))hook(terminalContext.load(std::memory_order_acquire),terminal);
  }
  uint32_t settledSequence() const noexcept { return ring?uint32_t(ringApplied):done.load(std::memory_order_acquire); }
  // Owner thread. In ring mode the host's published state is authoritative: a
  // host that stopped ends the session here, with its reason recorded.
  bool usable() noexcept {
    if(!healthy())return false;
    if(!ring)return true;
    const auto host=ring->hostState();
    if(host.state!=d9::ring::State::Faulted)return true;
    ringHostStopped(host);return false;
  }
  void ringHostStopped(const d9::ring::Producer::HostFault& host) noexcept {
    if(host.code==d9::ring::Fault::DeviceLost) {
      // An acknowledged remote terminal, as a DEVICELOST reply is on the pipe:
      // the device is lost, the transport did not fail.
      noteTerminal(Terminal::HostDeviceLost,"device-lost",host.result);
      deviceLost.store(true,std::memory_order_release);SetEvent(workAvailable.get());
    } else if(host.code==d9::ring::Fault::AsyncRefused)
      failClosed(Terminal::HostRejected,"async-refused",host.result);
    else failClosed(Terminal::RingCorrupt,d9::ring::FaultName(host.code));
  }
  // After a successful Initialize: create the regions, tell the host on the
  // pipe, and switch once its reply proves both ends see the same session.
  HRESULT attachRing() noexcept {
    try {
      const auto process=d9::highva::Probe();
      ringChannel=std::make_unique<d9::ring::OwnerChannel>(nonce,ringCapacity,highVaSetting?&process:nullptr,
        highVaSetting==2?unsigned(d9::highva::Method::Scan):d9::highva::AllMethods);
      {
        // One line per attached ring: which process this is and where the two
        // views ended up.
        const auto row=[&](const d9::ring::Region& region,char* text,size_t size) {
          const auto base=uint64_t(reinterpret_cast<uintptr_t>(region.base()));
          std::snprintf(text,size,"{\"base\":\"0x%08llx\",\"bytes\":%u,\"high\":%s,\"method\":\"%s\",\"attempts\":%u,\"error\":%u}",
            static_cast<unsigned long long>(base),region.bytes(),
            d9::highva::WhollyHigh(region.base(),region.bytes(),process.maxApp)?"true":"false",
            d9::highva::MethodName(region.placement().method),region.placement().attempts,region.placement().error);
        };
        char producerRow[160]{},consumerRow[160]{};
        row(ringChannel->producerRegion(),producerRow,sizeof(producerRow));
        row(ringChannel->consumerRegion(),consumerRow,sizeof(consumerRow));
        std::fprintf(stderr,"D3D9 ring-placement: {\"schema\":\"warvk-d3d9-ring-placement/v1\",\"pid\":%lu,"
          "\"exeLargeAddress\":%s,\"maxAppAddress\":\"0x%08llx\",\"eligible\":%s,\"setting\":%u,\"capacity\":%u,"
          "\"producer\":%s,\"consumer\":%s}\n",GetCurrentProcessId(),process.exeFlag?"true":"false",
          static_cast<unsigned long long>(process.maxApp),process.eligible()?"true":"false",highVaSetting,ringCapacity,
          producerRow,consumerRow);
      }
      d9::Header h;h.op=d9::Op::AttachRing;h.args[0]=ringCapacity;h.args[1]=d9::ring::ReplyCapacity;h.args[2]=d9::ring::Version;
      const auto hr=call(h);
      if(hr!=D3D_OK) {
        // A host that cannot attach is not driven half over the pipe instead.
        if(healthy())failClosed(Terminal::LocalContract,"command ring attach refused",uint32_t(hr));
        return D3DERR_DEVICELOST;
      }
      if(!d9::ring::ValidConsumerBlock(*ringChannel->consumerBlock(),nonce,sequence)) {
        failClosed(Terminal::RingCorrupt,"consumer region");return D3DERR_DEVICELOST;
      }
      ringAttachSequence=ringApplied=sequence;
      ring=std::make_unique<d9::ring::Producer>(ringChannel->producerBlock(),ringChannel->data(),
        ringChannel->consumerBlock(),ringChannel->reply(),ringCapacity,sequence);
      return D3D_OK;
    } catch(const std::exception& e) {
      failClosed();std::fprintf(stderr,"D3D9 command ring attach: %s\n",e.what());return D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  // The host's validated progress; throws once what it published is invalid.
  d9::ring::Producer::Progress ringObserve() {
    d9::ring::Producer::Progress now;
    if(!ring->observe(now)) {
      failClosed(Terminal::RingCorrupt,d9::ring::FaultName(ring->fault()));
      throw std::runtime_error("D3D9 command ring: host progress invalid");
    }
    ringApplied=now.applied;return now;
  }
  // space: the wait is for ring space (admission), not for an applied record.
  struct RingWait { bool started=false,polled=false,space=false;uint64_t head=0,applied=0;ULONGLONG moved=0;uint32_t phase=0; };
  // WARVK_D3D9_RING_WAKE_FIX (x64 lost-wakeup fix; reviewed 2026-11-18, on by
  // default): unset or anything else: on; "0": the earlier wait, kept for a
  // control arm; alternate:N (1..1000000, by the sync profile's frame count,
  // so alternate needs the profile on). Read once.
  struct WakeFixSetting { uint32_t setting=1,every=0; };
  static WakeFixSetting ReadWakeFix() noexcept {
    static const WakeFixSetting value=[] {
      WakeFixSetting s;wchar_t v[32]{};
      const DWORD n=GetEnvironmentVariableW(L"WARVK_D3D9_RING_WAKE_FIX",v,32);
      if(!n||n>=32)return s;
      if(!std::wcscmp(v,L"0")){s.setting=0;return s;}
      if(!std::wcsncmp(v,L"alternate:",10)) {
        uint64_t every=0;const wchar_t* c=v+10;
        for(;*c>=L'0'&&*c<=L'9'&&every<=1000000;++c)every=every*10+uint64_t(*c-L'0');
        if(!*c&&c!=v+10&&every>=1&&every<=1000000){s.setting=2;s.every=uint32_t(every);}
      }
      return s;
    }();
    return value;
  }
  uint32_t wakeFixPhase() const noexcept {
    const auto s=ReadWakeFix();
    if(s.setting==1)return 1;
    return s.setting==2&&(profileFrames/s.every)%2==1?1u:0u;
  }
  // One bounded step of waiting for the host: returns after progress, a wake or
  // a recheck interval. Throws when the host stopped, made no progress for
  // RingNoProgressMs (a rolling limit: a host that keeps consuming is not
  // hung), or `until` (0: none) passed.
  void ringAwait(RingWait& wait,ULONGLONG until) {
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
    if(beforeRingWaitForTest)beforeRingWaitForTest(*this,wait.space);
#endif
    auto now=ringObserve();
    const auto tick=GetTickCount64();
    if(!wait.started||now.head!=wait.head||now.applied!=wait.applied) {
      const bool first=!wait.started;
      wait.started=true;wait.head=now.head;wait.applied=now.applied;wait.moved=tick;
      if(!first)return;
      // The caller tested its condition before this baseline was read: progress
      // published in between is not a change against it, so the sleep below
      // could wait out RecheckMs although the condition already holds (the host
      // signals only a producer it sees waiting). With the fix the first call
      // only takes the baseline; the caller tests again, and the next call
      // compares against a baseline older than that test.
      wait.phase=wakeFixPhase();
      ++profileWakeFix[wait.phase][0];
      if(wait.phase){++profileWakeFix[1][1];return;}
    }
    if(!healthy())throw std::runtime_error("D3D9 ring wait after terminal failure");
    if(const auto host=ring->hostState();host.state==d9::ring::State::Faulted) {
      ringHostStopped(host);throw std::runtime_error("D3D9 host stopped");
    }
    const bool deadline=until&&tick>=until;
    if(deadline||tick-wait.moved>=ringNoProgressMs) {
      failClosed(Terminal::HostHung,deadline?"call deadline":"no progress");
      throw std::runtime_error("D3D9 host made no progress");
    }
    if(!wait.polled) {
      wait.polled=true;
      for(uint32_t i=0;i<d9::ring::SpinPolls;++i) {
        YieldProcessor();now=ringObserve();
        if(now.head!=wait.head||now.applied!=wait.applied)return;
      }
    }
    // Say so, look once more, then sleep: the host signals only a waiting producer.
    ring->setWaiting(true,wait.space);
    now=ringObserve();
    if(now.head!=wait.head||now.applied!=wait.applied||ring->hostState().state==d9::ring::State::Faulted) {
      ring->setWaiting(false);return;
    }
    ++ringParks;
    ULONGLONG remaining=ringNoProgressMs-(tick-wait.moved);
    if(until)remaining=std::min<ULONGLONG>(remaining,until-tick);
    HANDLE waits[2]={ringChannel->progress(),producerProgress.get()};
    const auto sleepBegin=d9::profile::Clock();
    const auto status=WaitForMultipleObjects(2,waits,FALSE,DWORD(std::min<ULONGLONG>(remaining,d9::ring::RecheckMs)));
    ring->setWaiting(false);
    if(status==WAIT_FAILED)throw std::runtime_error("D3D9 ring wait failed");
    profileWoken(status,wait,sleepBegin);
  }
  // Lost-wakeup measurement: what ended this sleep, by unvalidated peeks only
  // (the caller's next ringObserve is still the one accepted reading).
  void profileWoken(DWORD status,const RingWait& wait,int64_t begin) noexcept {
    const auto applied=ring->peekApplied(),head=ring->peekHead();
    const bool progress=applied!=wait.applied||head!=wait.head;
    d9::profile::WakeEvent e;e.begin=begin;e.end=d9::profile::Clock();e.status=status;
    e.op=profileAwaitOp;e.sequence=profileAwaitSequence;e.before=wait.applied;e.after=applied;
    e.peerFlag=ring->consumerParked()?1u:0u;e.signaled=profileLastSignal?1u:0u;
    profileWake.ended(e,status==WAIT_OBJECT_0,status==WAIT_OBJECT_0+1,progress,true);
    // [waits,earlyReturns,sleeps,timeouts,satisfiedTimeouts,timeoutUs] by phase:
    // a satisfied timeout slept although its baseline already met the awaited record.
    auto& m=profileWakeFix[wait.phase];++m[2];
    if(status==WAIT_TIMEOUT) {
      ++m[3];if(profileAwaitSequence&&wait.applied>=profileAwaitSequence)++m[4];
      m[5]+=d9::profile::Micros(uint64_t(e.end-e.begin),profileFrequency);
    }
  }
  // Admission on the ring: one copy into shared memory, no reply. A full ring
  // is backpressure; nothing is dropped and no sequence is used until written.
  void ringPublish(d9::Header& h,const void* payload,bool sync) {
    if(sequence==UINT32_MAX)throw std::runtime_error("D3D9 sequence exhausted");
    h.nonce=nonce;h.sequence=uint64_t(sequence)+1;
    if((h.payloadBytes&&!payload)||!d9::EncodeHeader(h,ringHeader.data()))throw std::runtime_error("D3D9 invalid local command");
    RingWait wait;wait.space=true;bool waited=false;ULONGLONG began=0;int64_t profileFullBegin=0;uint64_t profileFullParks=0;
    for(;;) {
      if(!(resourceState?usable():healthy()))throw std::runtime_error("D3D9 admission after terminal failure");
      const auto written=ring->tryWrite(ringHeader.data(),payload,h.payloadBytes,h.sequence,sync);
      if(written==d9::ring::Producer::Write::Written)break;
      if(written==d9::ring::Producer::Write::Faulted) {
        failClosed(Terminal::RingCorrupt,d9::ring::FaultName(ring->fault()));
        throw std::runtime_error("D3D9 command ring: host progress invalid");
      }
      ++fullObservations;
      if(!waited){waited=true;began=GetTickCount64();++backpressureCalls;profileFullBegin=d9::profile::Ticks();profileFullParks=ringParks;}
      profileAwaitOp=uint32_t(h.op);profileAwaitSequence=0;
      ringAwait(wait,0);
    }
    if(waited) {
      const auto elapsed=GetTickCount64()-began;
      backpressureMs+=elapsed;maxBackpressureMs=std::max(maxBackpressureMs,uint64_t(elapsed));
      profileFull(h.op,profileFullBegin,ringParks-profileFullParks);
    }
    profilePublished(h,sync);
    if(resourceState)resourceState->published(h,sync);
    sequence=uint32_t(h.sequence);++producerAdmission.publishes;
    ++ringRecords;ringBytes+=d9::ring::RecordBytes(d9::HeaderBytes+h.payloadBytes);
    if(!sync) {
      if(h.op==d9::Op::Clear)++ringAsyncClears;
      else if(h.op==d9::Op::DrawBatch)++ringAsyncDrawBatches;
      else if(d9::IsFixedBatch(h.op))++ringAsyncFixedBatches;
    }
    maxObservedOutstanding=std::max(maxObservedOutstanding,sequence-uint32_t(ringApplied));
    const bool consumerParked=ring->consumerParked();
    ++profileWake.notifyChecks;profileLastSignal=consumerParked;
    if(consumerParked){++ringWakes;++profileWake.notified;signalNotification(ringChannel->work());}
  }
  // WARVK_D3D9_PRESENT_AHEAD: this Present is published like an ordered record
  // and the call returns once the previous one was applied. The host turns a
  // skipped frame (window resized ahead of its Reset) into success, as
  // presentFrame does with the synchronous reply; any other failure of it
  // ends the session, as for every asynchronous record. Whatever synchronous
  // call follows (Reset, Finish, a readback) still waits for all of it.
  HRESULT ringPresentAhead(d9::Header h) noexcept {
    try {
      const auto until=callDeadlinesAt(GetTickCount64()).call;
      const uint64_t previous=presentAheadSequence.load(std::memory_order_relaxed);
      ringPublish(h,nullptr,false);
      presentAheadSequence.store(h.sequence,std::memory_order_release);++presentsAhead;
      const auto profileBegin=d9::profile::Ticks();const auto profileParks=ringParks;
      profileAwaitOp=uint32_t(h.op);profileAwaitSequence=previous;
      RingWait wait;
      while(ringObserve().applied<previous) {
        if(const auto host=ring->hostState();host.state==d9::ring::State::Faulted) {
          ringHostStopped(host);return D3DERR_DEVICELOST;
        }
        if(!healthy())return D3DERR_DEVICELOST;
        ringAwait(wait,until);
      }
      if(wait.started)++presentAheadWaits;
      profileAhead(h.op,profileBegin,ringParks-profileParks);
      return healthy()?D3D_OK:D3DERR_DEVICELOST;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 dev ring present: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
  // A synchronous barrier on the ring: publish, then wait for the host to
  // report this record executed and read its reply from the reply slot.
  HRESULT ringCall(d9::Header h,const void* payload,void* result,uint32_t resultSize) noexcept {
    try {
      const auto until=callDeadlinesAt(GetTickCount64()).call;
      ringPublish(h,payload,true);++ringSyncCalls;
      const auto profileBegin=d9::profile::Ticks();const auto profileParks=ringParks;
      profileAwaitOp=uint32_t(h.op);profileAwaitSequence=h.sequence;
      RingWait wait;
      while(ringObserve().applied<h.sequence) {
        // This record's own reply outranks a stop the host published after it.
        if(const auto host=ring->hostState();host.state==d9::ring::State::Faulted) {
          ringHostStopped(host);return D3DERR_DEVICELOST;
        }
        if(!healthy())return D3DERR_DEVICELOST;
        ringAwait(wait,until);
      }
      profileSync(h.op,profileBegin,ringParks-profileParks);
      if(!ring->readReply(h,reply,replyPacket.data()+d9::ReplyBytes)) {
        failClosed(Terminal::RingCorrupt,d9::ring::FaultName(ring->fault()));return D3DERR_DEVICELOST;
      }
      const HRESULT hr=HRESULT(reply.result);
      if(hr==D3DERR_DEVICELOST) {
        noteTerminal(Terminal::HostDeviceLost,"device-lost reply",uint32_t(hr));
        deviceLost.store(true,std::memory_order_release);SetEvent(workAvailable.get());
      }
      if(SUCCEEDED(hr)) {
        if(reply.payloadBytes!=resultSize||(resultSize&&!result))
          throw std::runtime_error("D3D9 response payload size");
        if(resultSize)std::memcpy(result,replyPacket.data()+d9::ReplyBytes,resultSize);
      } else if(reply.payloadBytes)throw std::runtime_error("D3D9 failed result carried payload");
      // The same per-reply account the pipe keeps (acceptReply).
      if(hr==D3D_OK) {
        if(h.op==d9::Op::Clear)++clearRemoteSucceeded;
        else if(h.op==d9::Op::DrawBatch)++drawBatchesRemoteSucceeded;
        else if(d9::IsFixedBatch(h.op))++fixedBatchesRemoteSucceeded;
      }
      if(h.op==d9::Op::Finish) {
        ringFinish.store(SUCCEEDED(hr)?1u:2u,std::memory_order_release);SetEvent(workAvailable.get());
      }
      return hr;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 dev ring call: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {
      failClosed();return D3DERR_DEVICELOST;
    }
  }
  // Ring mode has no WaitEvent reply: once the host executed the wait, its
  // published GPU watermark must cover the serial that wait asked for.
  bool ringSettleEvent() noexcept {
    try {
      const auto now=ringObserve();
      if(eventWaitSequence&&!eventWaitSettled&&now.applied>=eventWaitSequence) {
        const auto completed=ring->gpuCompleted();
        if(completed<eventWaitSerial||completed<eventGpuCompleted.load(std::memory_order_relaxed)) {
          failClosed(Terminal::RingCorrupt,"event watermark");return false;
        }
        eventGpuCompleted.store(completed,std::memory_order_release);++eventWaitsSucceeded;eventWaitSettled=true;
      }
      return true;
    } catch(...) {failClosed();return false;}
  }
  // Worker thread, once the ring carries the commands: it only watches the
  // host process, so an exit is noticed even while the producer is busy.
  void watchRing(HANDLE host) {
    // A session that ends here without a Finish ends the stream for the host
    // too, as closing the pipe does: an idle host must not wait out its parent.
    struct EndOfStream {
      Connection& c;bool armed=true;
      ~EndOfStream() {
        if(!armed)return;
        c.ringChannel->producerBlock()->closed.store(1,std::memory_order_seq_cst);SetEvent(c.ringChannel->work());
      }
    } end{*this};
    const auto* block=ringChannel->consumerBlock();
    HANDLE waits[2]={host,workAvailable.get()};
    for(;;) {
      const auto finish=ringFinish.load(std::memory_order_acquire);
      if(finish==1){end.armed=false;return;}
      if(finish==2)throw std::runtime_error("D3D9 Finish was rejected");
      if(failed.load(std::memory_order_acquire))throw std::runtime_error("D3D9 ring session failed");
      if(deviceLost.load(std::memory_order_acquire)){queue.requestStop();remoteTerminal=true;return;}
      if(producerClosed.load(std::memory_order_acquire))return;
      const auto status=WaitForMultipleObjects(2,waits,FALSE,d9::ring::RecheckMs);
      if(status==WAIT_OBJECT_0) {
        // The host is gone; what it last published says whether that was expected.
        const auto state=block->state.load(std::memory_order_acquire);
        const auto code=block->faultCode.load(std::memory_order_acquire);
        const auto result=block->faultResult.load(std::memory_order_acquire);
        if(state==uint32_t(d9::ring::State::Finished))return;
        if(state==uint32_t(d9::ring::State::Faulted)&&code==uint32_t(d9::ring::Fault::DeviceLost)) {
          noteTerminal(Terminal::HostDeviceLost,"device-lost",result);
          deviceLost.store(true,std::memory_order_release);queue.requestStop();remoteTerminal=true;
          SetEvent(producerProgress.get());return;
        }
        // The exit code before the cause is published: its reader sees both.
        {DWORD exit=STILL_ACTIVE;if(GetExitCodeProcess(host,&exit))hostExit.store(exit,std::memory_order_release);}
        if(state!=uint32_t(d9::ring::State::Faulted))failClosed(Terminal::HostExited,"host process exited");
        else if(code==uint32_t(d9::ring::Fault::AsyncRefused))failClosed(Terminal::HostRejected,"async-refused",result);
        else failClosed(Terminal::RingCorrupt,d9::ring::FaultName(d9::ring::Fault(code)));
        throw std::runtime_error("D3D9 host exited");
      }
      if(status!=WAIT_OBJECT_0+1&&status!=WAIT_TIMEOUT)throw std::runtime_error("D3D9 ring watch failed");
    }
  }
  HRESULT contentSend(d9::Header h,const void* payload) noexcept {
    if(!asyncDrawBatches)return call(h,payload);
    try {
      publishUntil(h,payload,GetTickCount64()+lab::DeadlineMs);
      return healthy()?D3D_OK:D3DERR_DEVICELOST;
    } catch(const std::exception& e) {
      failClosed();
      std::fprintf(stderr,"D3D9 content admission: %s\n",e.what());
      return D3DERR_DEVICELOST;
    } catch(...) {failClosed();return D3DERR_DEVICELOST;}
  }
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
    d9::Header h;h.op=op;h.args[0]=count;h.payloadBytes=bytes;
    if(op==d9::Op::DrawFixedDeltaBatch)h.args[1]=fixedDeltaVerify;
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
    producerClosed.store(true,std::memory_order_release);
    // Without a Finish the host reads this as the end of the stream.
    if(ring){ring->close();SetEvent(ringChannel->work());}
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
    // The ring's own rolling no-progress limit replaces the admission deadline.
    if(ring){ringPublish(h,payload,false);return;}
    const auto bytes=prepare(h,payload);
    bool waited=false;ULONGLONG began=0;AdmissionClock::time_point waitBegin{};int64_t profileFullBegin=0;
    const auto accountWait=[&] {
      if(!waited)return;
      const auto elapsed=GetTickCount64()-began;
      backpressureMs+=elapsed;maxBackpressureMs=std::max(maxBackpressureMs,uint64_t(elapsed));
      if(admissionProfile) {
        ++producerAdmission.backpressureEpisodes;
        producerAdmission.backpressureNs+=Nanoseconds(waitBegin,AdmissionClock::now());
      }
      profileFull(h.op,profileFullBegin);
    };
    try {
      for(;;) {
        if(!healthy())throw std::runtime_error("D3D9 admission after terminal failure");
        if(GetTickCount64()>=until)throw std::runtime_error("D3D9 command admission deadline");
        const auto admission=tryPublishPrepared(h,bytes);
        if(admission.mayCommitLocalState()) {
          accountWait();
          // The record a call() waits for is synchronous; everything else is not.
          profilePublished(h,uint32_t(h.sequence)==callSequence.load(std::memory_order_relaxed));
          return;
        }
        if(admission.outcome!=commands::Publish::Full)
          throw std::runtime_error("D3D9 command admission refused");
        ++fullObservations;
#ifdef WARVK_D3D9_CONNECTION_CPU_TEST
        if(fullObservedForTest)fullObservedForTest->store(true,std::memory_order_release);
#endif
        if(!waited) {
          waited=true;began=GetTickCount64();++backpressureCalls;profileFullBegin=d9::profile::Ticks();
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
    const bool orderedAsync=asyncDrawBatches&&orderedOp(h.op)
      &&uint32_t(h.sequence)!=callSequence.load(std::memory_order_acquire);
    const bool asynchronous=h.op==d9::Op::Clear
      ||((h.op==d9::Op::DrawBatch||d9::IsFixedBatch(h.op)||h.op==d9::Op::AnnotateDraws
        ||h.op==d9::Op::ContentUpload||h.op==d9::Op::ContentRelease)&&asyncDrawBatches)
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
    if(d9::IsFixedBatch(h.op)&&hr==D3D_OK)++fixedBatchesRemoteSucceeded;
    if(orderedAsync&&hr==D3D_OK)++(AsyncOrderedOp(h.op)?orderedRemoteSucceeded:orderedExtraRemoteSucceeded);
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
  // P0 measurement only (warvk-d3d9-sync-profile/v1): per wire op, the
  // synchronous calls and the time from publication to their reply, the
  // records published without one, and the waits for queue or ring space;
  // per API entry the same; synchronous calls by (API, op). Owner thread
  // only; the destructor reads them after the worker join. Fixed tables,
  // QueryPerformanceCounter ticks, no allocation.
  // Histograms: <10 us, <50 us, <200 us, <1 ms, <5 ms, <20 ms, longer.
  static constexpr size_t ProfileBucketCount=7;
  using ProfileHistogram=std::array<uint64_t,ProfileBucketCount>;
  struct ProfileOp {
    uint64_t syncCalls=0,syncTicks=0,syncMaxTicks=0,syncBytes=0,syncParks=0,asyncPublishes=0,asyncBytes=0;
    uint64_t fullWaits=0,fullTicks=0,fullMaxTicks=0,fullParks=0;
    ProfileHistogram histogram{},fullHistogram{};
  };
  struct ProfileApiCounters {
    uint64_t entries=0,syncCalls=0,syncTicks=0,asyncPublishes=0,asyncBytes=0,fullWaits=0,fullTicks=0;
  };
  struct ProfilePair { uint64_t calls=0,ticks=0; };
  // Updates of an initialized texture level (not first uploads), by level.
  struct ProfileTexture {
    uint32_t id=0,generation=0,level=0,width=0,height=0,format=0,levels=0,usage=0,pool=0,stage=0;
    uintptr_t creator=0;
    uint64_t updates=0,partial=0,bytes=0,chunks=0,frameScene=0;
  };
  // "module+0xRVA" of a code address, or the bare address outside any module.
  static void ProfileAddress(uintptr_t address,char* text,size_t size) noexcept {
    HMODULE module=nullptr;wchar_t path[MAX_PATH]{};
    if(address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module)&&module&&GetModuleFileNameW(module,path,MAX_PATH)) {
      const wchar_t* name=path;for(const wchar_t* c=path;*c;++c)if(*c==L'\\'||*c==L'/')name=c+1;
      char narrow[64]{};size_t n=0;
      for(;name[n]&&n+1<sizeof(narrow);++n)narrow[n]=(name[n]>=32&&name[n]<127&&name[n]!=L'"')?char(name[n]):'?';
      std::snprintf(text,size,"%s+0x%llx",narrow,static_cast<unsigned long long>(address-reinterpret_cast<uintptr_t>(module)));
      return;
    }
    std::snprintf(text,size,"0x%llx",static_cast<unsigned long long>(address));
  }
  static constexpr size_t ProfileTextureSlots=512,ProfileTextureProbes=32,ProfileTextureTop=32;
  static std::array<uint64_t,ProfileBucketCount-1> ProfileBuckets(int64_t frequency) noexcept {
    constexpr uint64_t limits[ProfileBucketCount-1]={10,50,200,1000,5000,20000};
    std::array<uint64_t,ProfileBucketCount-1> result{};
    for(size_t i=0;i<result.size();++i)result[i]=uint64_t(frequency)*limits[i]/1000000u;
    return result;
  }
  void profileBucket(ProfileHistogram& histogram,uint64_t ticks) const noexcept {
    size_t bucket=0;while(bucket<profileBuckets.size()&&ticks>=profileBuckets[bucket])++bucket;
    ++histogram[bucket];
  }
  void profileSync(d9::Op op,int64_t begin,uint64_t parks=0) noexcept {
    const auto ticks=uint64_t(d9::profile::Ticks()-begin);const auto index=d9::profile::OpIndex(op);
    auto& o=profileOps[index];++o.syncCalls;o.syncTicks+=ticks;o.syncMaxTicks=std::max(o.syncMaxTicks,ticks);
    o.syncParks+=parks;profileBucket(o.histogram,ticks);
    auto& a=profileApis[size_t(profileApi)];++a.syncCalls;a.syncTicks+=ticks;
    auto& p=profilePairs[size_t(profileApi)][index];++p.calls;p.ticks+=ticks;
    if(op==d9::Op::Present)++profilePresents;
  }
  // A Present published ahead: the time x86 was blocked in it (waiting for the
  // previous one) is kept as the op's synchronous time, but it is no
  // synchronous call; profilePublished counted the record as asynchronous.
  void profileAhead(d9::Op op,int64_t begin,uint64_t parks) noexcept {
    const auto ticks=uint64_t(d9::profile::Ticks()-begin);const auto index=d9::profile::OpIndex(op);
    auto& o=profileOps[index];o.syncTicks+=ticks;o.syncMaxTicks=std::max(o.syncMaxTicks,ticks);
    o.syncParks+=parks;profileBucket(o.histogram,ticks);
    profileApis[size_t(profileApi)].syncTicks+=ticks;profilePairs[size_t(profileApi)][index].ticks+=ticks;
    ++profilePresents;
  }
  void profilePublished(const d9::Header& h,bool sync) noexcept {
    const auto op=h.op;const auto payloadBytes=h.payloadBytes;
    if(op==d9::Op::DrawFixedDeltaBatch&&h.args[0]&&h.args[0]<=deltaHistogram.size()) {
      ++deltaHistogram[h.args[0]-1];deltaSentBytes+=payloadBytes;
    }
    const uint64_t bytes=d9::HeaderBytes+uint64_t(payloadBytes);auto& o=profileOps[d9::profile::OpIndex(op)];
    if(sync){o.syncBytes+=bytes;return;}
    ++o.asyncPublishes;o.asyncBytes+=bytes;
    auto& a=profileApis[size_t(profileApi)];++a.asyncPublishes;a.asyncBytes+=bytes;
  }
  // A full queue/ring, charged to the record that could not be written.
  void profileFull(d9::Op op,int64_t begin,uint64_t parks=0) noexcept {
    const auto ticks=uint64_t(d9::profile::Ticks()-begin);
    auto& o=profileOps[d9::profile::OpIndex(op)];++o.fullWaits;o.fullTicks+=ticks;
    o.fullMaxTicks=std::max(o.fullMaxTicks,ticks);o.fullParks+=parks;profileBucket(o.fullHistogram,ticks);
    auto& a=profileApis[size_t(profileApi)];++a.fullWaits;a.fullTicks+=ticks;
  }
  // One cumulative JSON line on stderr (the game's stderr in proxy mode).
  void profileSnapshot(const char* reason) noexcept {
    using namespace d9::profile;
    const auto now=Clock();
    const auto us=[this](uint64_t ticks){return static_cast<unsigned long long>(Micros(ticks,profileFrequency));};
    const auto n=[](uint64_t value){return static_cast<unsigned long long>(value);};
    Line line(profileText.data(),profileText.size());
    line.add("D3D9 sync-profile: {\"schema\":\"warvk-d3d9-sync-profile/v1\",\"reason\":\"%s\",\"pid\":%lu,"
      "\"every\":%llu,\"timing\":%s,\"presents\":%llu,\"frameScenes\":%llu,\"stageMarkers\":%llu,\"seconds\":%.6f,\"qpc\":%lld,"
      "\"qpcFrequency\":%lld,\"unixMs\":%llu,"
      "\"ring\":%s,\"ringCapacity\":%u,\"published\":%u,\"ringRecords\":%llu,\"ringBytes\":%llu,"
      "\"ringSyncCalls\":%llu,\"ringParks\":%llu,\"ringWakes\":%llu,\"fullObservations\":%llu,"
      "\"backpressureCalls\":%llu,\"backpressureMs\":%llu,\"ops\":[",
      reason,GetCurrentProcessId(),n(profileEvery),TimingEnabled()?"true":"false",n(profilePresents),n(frameScenesSent),n(stageMarkersSent),
      double(now-profileStart)/double(profileFrequency),
      static_cast<long long>(now),static_cast<long long>(profileFrequency),n(UnixMs()),
      ring?"true":"false",ringCapacity,sequence,n(ringRecords),n(ringBytes),n(ringSyncCalls),n(ringParks),n(ringWakes),
      n(fullObservations),n(backpressureCalls),n(backpressureMs));
    bool first=true;
    for(size_t i=1;i<Ops;++i) {
      const auto& o=profileOps[i];
      if(!o.syncCalls&&!o.syncBytes&&!o.asyncPublishes&&!o.fullWaits)continue;
      const auto& s=o.histogram;const auto& f=o.fullHistogram;
      line.add("%s{\"op\":%u,\"name\":\"%s\",\"sync\":%llu,\"syncUs\":%llu,\"syncMaxUs\":%llu,\"syncBytes\":%llu,"
        "\"syncParks\":%llu,\"hist\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu],\"async\":%llu,\"asyncBytes\":%llu,"
        "\"full\":%llu,\"fullUs\":%llu,\"fullMaxUs\":%llu,\"fullParks\":%llu,"
        "\"fullHist\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
        first?"":",",unsigned(i),OpName(i),n(o.syncCalls),us(o.syncTicks),us(o.syncMaxTicks),n(o.syncBytes),
        n(o.syncParks),n(s[0]),n(s[1]),n(s[2]),n(s[3]),n(s[4]),n(s[5]),n(s[6]),n(o.asyncPublishes),n(o.asyncBytes),
        n(o.fullWaits),us(o.fullTicks),us(o.fullMaxTicks),n(o.fullParks),
        n(f[0]),n(f[1]),n(f[2]),n(f[3]),n(f[4]),n(f[5]),n(f[6]));
      first=false;
    }
    line.close("],\"apis\":[");first=true;
    for(size_t i=0;i<Apis;++i) {
      const auto& a=profileApis[i];
      if(!a.entries&&!a.syncCalls&&!a.asyncPublishes&&!a.fullWaits)continue;
      line.add("%s{\"api\":\"%s\",\"entries\":%llu,\"sync\":%llu,\"syncUs\":%llu,\"async\":%llu,\"asyncBytes\":%llu,"
        "\"full\":%llu,\"fullUs\":%llu}",first?"":",",ApiName(i),n(a.entries),n(a.syncCalls),us(a.syncTicks),
        n(a.asyncPublishes),n(a.asyncBytes),n(a.fullWaits),us(a.fullTicks));
      first=false;
    }
    line.close("],\"pairs\":[");first=true;
    for(size_t i=0;i<Apis;++i)for(size_t j=1;j<Ops;++j) {
      const auto& p=profilePairs[i][j];
      if(!p.calls)continue;
      line.add("%s[\"%s\",\"%s\",%llu,%llu]",first?"":",",ApiName(i),OpName(j),n(p.calls),us(p.ticks));
      first=false;
    }
    // The most updated texture levels so far, most first (fixed selection, no allocation).
    std::array<const ProfileTexture*,ProfileTextureTop> top{};size_t topCount=0,tracked=0;
    for(const auto& texture:profileTextures) {
      if(!texture.updates)continue;
      ++tracked;size_t at=topCount;
      if(topCount<top.size())++topCount;
      else if(texture.updates<=top.back()->updates)continue;
      else at=top.size()-1;
      while(at>0&&top[at-1]->updates<texture.updates){top[at]=top[at-1];--at;}
      top[at]=&texture;
    }
    line.close("],\"textures\":{\"initial\":%llu,\"initialBytes\":%llu,\"updates\":%llu,\"updateBytes\":%llu,"
      "\"chunks\":%llu,\"tracked\":%llu,\"untracked\":%llu,\"top\":[",n(profileTextureInitial),
      n(profileTextureInitialBytes),n(profileTextureUpdates),n(profileTextureUpdateBytes),n(profileTextureChunks),
      n(tracked),n(profileTextureUntracked));
    for(size_t i=0;i<topCount;++i) {
      const auto& t=*top[i];
      char creator[96]{};ProfileAddress(t.creator,creator,sizeof(creator));
      line.add("%s[%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,\"%s\",%u,%u,%u,%llu]",i?",":"",t.id,t.generation,t.level,
        t.width,t.height,t.format,t.levels,n(t.updates),n(t.partial),n(t.bytes),n(t.chunks),creator,t.usage,t.pool,
        t.stage,n(t.frameScene));
    }
    // Game-thread frame times (us between Present returns) since the previous snapshot.
    const auto pending=profileFrames-profileFramesPrinted;
    const auto kept=std::min<uint64_t>(pending,profileFrameUs.size());
    line.close("]},\"frameUsDropped\":%llu,\"frameUs\":[",n(pending-kept));
    for(uint64_t i=profileFrames-kept;i<profileFrames;++i)
      line.add("%s%u",i==profileFrames-kept?"":",",profileFrameUs[i%profileFrameUs.size()]);
    profileFramesPrinted=profileFrames;
    line.close("]");
    profileWake.print(line,profileFrequency);
    {
      const auto s=ReadWakeFix();const auto& a=profileWakeFix[0];const auto& b=profileWakeFix[1];
      line.add(",\"wakeFix\":{\"setting\":%u,\"every\":%u,\"modes\":[[%llu,%llu,%llu,%llu,%llu,%llu],"
        "[%llu,%llu,%llu,%llu,%llu,%llu]]}",s.setting,s.every,n(a[0]),n(a[1]),n(a[2]),n(a[3]),n(a[4]),n(a[5]),
        n(b[0]),n(b[1]),n(b[2]),n(b[3]),n(b[4]),n(b[5]));
    }
    line.close(",\"truncated\":%s}\n",line.truncated()?"true":"false");
    line.write(stderr);
  }
  SyncApi profileApi=SyncApi::Other;
  int64_t profileFrequency=d9::profile::Frequency(),profileStart=d9::profile::Clock();
  std::array<uint64_t,ProfileBucketCount-1> profileBuckets=ProfileBuckets(profileFrequency);
  uint64_t profileEvery=0,profileNextSnapshot=0,profilePresents=0;
  std::array<ProfileOp,d9::profile::Ops> profileOps{};
  std::array<ProfileApiCounters,d9::profile::Apis> profileApis{};
  std::array<std::array<ProfilePair,d9::profile::Ops>,d9::profile::Apis> profilePairs{};
  std::array<ProfileTexture,ProfileTextureSlots> profileTextures{};
  uint64_t profileTextureInitial=0,profileTextureInitialBytes=0,profileTextureUpdates=0,profileTextureUpdateBytes=0;
  uint64_t profileTextureChunks=0,profileTextureUntracked=0;
  uint32_t profileStage=0;
  int64_t profileLastPresent=0;uint64_t profileFrames=0,profileFramesPrinted=0;
  std::array<uint32_t,512> profileFrameUs{};
  // Lost-wakeup measurement: the record the producer sleeps for and whether
  // its publish signalled `work`.
  d9::profile::WakeLog profileWake;uint32_t profileAwaitOp=0;uint64_t profileAwaitSequence=0;bool profileLastSignal=false;
  std::array<std::array<uint64_t,6>,2> profileWakeFix{};
  std::array<char,32768> profileText{};
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
  // Full and delta batches have exclusive ownership of the same payload space.
  std::array<uint8_t,d9::MaxPayload> fixedBatch{};
  d9::FixedDrawPacket deltaPrevious{};
  uint32_t deltaBytes=0;
  uint64_t deltaSentBytes=0;
  std::array<uint64_t,d9::MaxFixedDrawBatch> deltaHistogram{};
  bool fixedDelta=false,fixedDeltaVerify=false;
  // WARVK_D3D9_ANNOTATIONS=1: the next draw's records, then the staged batch's.
  bool annotations=false;
  std::array<uint8_t,d9::MaxRecordsPerDraw*(d9::RecordHeaderBytes+d9::MaxAnnotationData)> nextRecords{};
  uint32_t nextRecordBytes=0,nextRecordCount=0;uint16_t nextLastSchema=0;
  std::array<uint8_t,d9::MaxDrawAnnotationBytes> batchRecords{};
  uint32_t batchRecordBytes=0,batchRecordCount=0;
  uint64_t annotatedDrawsAdmitted=0,annotationRecordsAdmitted=0,annotationMessages=0;
  uint64_t frameScenesSent=0,stageMarkersSent=0,annotationRecordsDiscarded=0;
  // requestSessionReset: the mutex guards the request; the flag keeps Present cheap.
  std::mutex sessionResetMutex;
  std::atomic<bool> sessionResetPending{false};
  uint32_t sessionResetReason=0;
  uint64_t sessionResetGeneration=0,sessionResetsRequested=0,sessionResetsSent=0,sessionResetsRefused=0;
  // The host's content store as x86 admitted it: identities and sizes only.
  d9::ContentIndex contentIndex;
  uint64_t contentUploadsSent=0,contentChunksSent=0,contentBytesSent=0,contentReleasesSent=0,contentReadsSent=0;
  uint64_t contentBytesRead=0,contentRefsAdmitted=0,contentLocalRefusals=0;
  bool drawBatching=false,asyncDrawBatches=false,asyncEvents=false,groupedSubmissions=false,fixedBatching=false;
  uint32_t drawBatchCount=0,fixedBatchCount=0;
  uint64_t batchedDrawsAdmitted=0,drawBatchesFlushed=0,fixedDrawsAdmitted=0,fixedBatchesFlushed=0;
  std::atomic<uint64_t> eventGpuCompleted{0};
  uint32_t eventWaitSequence=0;
  // Ring mode: the serial the outstanding WaitEvent asked for, until settled.
  uint64_t eventWaitSerial=0;bool eventWaitSettled=true;
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
  // P2 profile switches and their own account (ordered-extra/v1).
  bool asyncUploads=false,asyncFlush=false,asyncScene=false,presentAhead=false;
  std::unique_ptr<AsyncResourceState> resourceState;
  uint64_t orderedTexturePublished=0,orderedFlushPublished=0,orderedExtraRemoteSucceeded=0;
  uint64_t orderedScenePublished=0,sceneRefusalsObserved=0,presentsAhead=0,presentAheadWaits=0;
  // Written by the owner, read by the window's thread (settlePresentAhead).
  std::atomic<uint64_t> presentAheadSequence{0};
  std::atomic<uint32_t> presentAheadSettles{0};
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
  void (*beforeRingWaitForTest)(Connection&,bool)=nullptr;
#endif
  // Command ring. The channel is created by the owner before AttachRing is
  // published and outlives the worker; `ring` is the owner thread's alone.
  static constexpr uint32_t RingNoProgressMs=10000;
  uint32_t ringNoProgressMs=RingNoProgressMs;
  // Records published without a reply; counted as succeeded only by an executed Finish.
  uint64_t ringAsyncClears=0,ringAsyncDrawBatches=0,ringAsyncFixedBatches=0;
  bool ringRequested=false;uint32_t ringCapacity=d9::ring::DefaultCapacity;
  uint32_t highVaSetting=1; // WARVK_D3D9_HIGH_VA
  std::unique_ptr<d9::ring::OwnerChannel> ringChannel;
  std::unique_ptr<d9::ring::Producer> ring;
  std::array<uint8_t,d9::HeaderBytes> ringHeader{};
  uint64_t ringAttachSequence=0,ringApplied=0;
  uint64_t ringRecords=0,ringBytes=0,ringSyncCalls=0,ringParks=0,ringWakes=0;
  std::atomic<uint32_t> ringFinish{0}; // 1: Finish executed, 2: Finish rejected
  std::atomic<bool> producerClosed{false};
  std::atomic<uint32_t> terminalKind{0};
  std::atomic<bool> terminalReady{false};
  TerminalReport terminal; // written once by the thread that won terminalKind
  std::atomic<TerminalHook> terminalHook{nullptr};
  std::atomic<void*> terminalContext{nullptr};
  std::thread worker;
  DWORD childPid=0,childExit=STILL_ACTIVE;
  // childExit for the owner thread; STILL_ACTIVE until the host exited.
  std::atomic<uint32_t> hostExit{STILL_ACTIVE};
  std::wstring hostLogPath; // fixed before the worker starts
  uint64_t childCreation=0,exchanges=0;
  uint64_t exitWaitMs=0;
  bool peerVerified=false;
  bool remoteTerminal=false,childNaturalExit=false;
  bool childProcessSettled=false,childSelfFailStop=false;
};
} // namespace warvk::host::d3d9proxy
