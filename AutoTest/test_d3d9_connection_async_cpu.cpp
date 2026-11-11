// Draft CPU fixture: executes the exact candidate Connection admission/reply
// helpers and SPSC queue. No child, pipe, Vulkan, window, or game is created.
// NOT COM/IPC/GPU evidence. Build/run authority remains with root.
#define WARVK_D3D9_CONNECTION_CPU_TEST 1
#include "d3d9_connection.h"
#include <array>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace warvk::host::d3d9proxy {
struct ConnectionCpuTest {
  static std::atomic<unsigned> checks;
  static void check(bool value,unsigned line) {
    checks.fetch_add(1,std::memory_order_relaxed);
    if(!value)throw std::runtime_error("X1a CPU check line "+std::to_string(line));
  }
#define CHECK(value) ConnectionCpuTest::check(bool(value),__LINE__)
  static std::unique_ptr<Connection> make() {
    return std::unique_ptr<Connection>(new Connection(Connection::PreparedOnly{},ipc::Nonce{3,7}));
  }
  static d9::Header init() {
    d9::Header h;h.op=d9::Op::Initialize;h.args[0]=64;h.args[1]=64;return h;
  }
  static d9::Header clear(uint32_t color) {
    d9::Header h;h.op=d9::Op::Clear;h.args[0]=color;
    h.args[1]=d9::FloatBits(1.0f);h.args[2]=D3DCLEAR_TARGET;return h;
  }
  static commands::PublishResult publish(Connection& c,d9::Header h,const void* payload=nullptr) {
    const auto bytes=c.prepare(h,payload);return c.tryPublishPrepared(h,bytes);
  }
  static commands::Batch take(Connection& c) {
    commands::Batch batch;CHECK(c.tryTakeBatch(batch)==commands::Dequeue::Item);return batch;
  }
  static d9::Header header(const commands::Batch& batch) {
    d9::Header h;CHECK(d9::DecodeHeader(batch.payload.data(),h));return h;
  }
  static void complete(Connection& c,const commands::Batch& batch,HRESULT hr=D3D_OK,
      uint32_t payloadBytes=0,uint64_t submitted=0,uint64_t completed=0,uint64_t retired=0) {
    const auto h=header(batch);
    d9::Reply response{DWORD(hr),submitted,completed,retired,0,payloadBytes};
    d9::EncodeReply(h,response,c.replyPacket);
    for(uint32_t i=0;i<payloadBytes;++i)c.replyPacket[d9::ReplyBytes+i]=uint8_t(i^0x5a);
    CHECK(d9::DecodeReply(h,c.replyPacket.data(),c.reply));
    c.acceptReply(h,batch);
  }
  static void ready(Connection& c) {
    CHECK(publish(c,init()).mayCommitLocalState());
    complete(c,take(c));
    CHECK(c.initializedAcknowledged.load(std::memory_order_acquire));
    CHECK(c.done.load(std::memory_order_acquire)==1&&c.healthy());
  }
  static void workerDesktop(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop,const char* testCase) {
    const auto isolated=d9::RequireIsolatedDesktop();
    const auto desktop=d9::CurrentDesktop(),input=d9::InputDesktop();
    if(isolated!=L"WinSta0\\"+expectedDesktop||desktop!=expectedDesktop||input!=expectedInputDesktop)
      throw std::runtime_error("X1a CPU worker desktop mismatch");
    std::printf("{\"schema\":\"warvk-x1a-cpu-desktop/v1\",\"role\":\"worker\",\"case\":\"%s\","
      "\"pid\":%lu,\"tid\":%lu,\"bits\":%u,\"desktop\":%s,\"inputDesktop\":%s,"
      "\"isolatedDesktop\":%s,\"gpuTested\":false}\n",
      testCase,GetCurrentProcessId(),GetCurrentThreadId(),unsigned(sizeof(void*)*8),
      d9::NameJson(desktop).c_str(),d9::NameJson(input).c_str(),d9::NameJson(isolated).c_str());
    std::fflush(stdout);
  }
  static void awaitFlag(const std::atomic<bool>& flag,const Connection& c) {
    const auto deadline=GetTickCount64()+5000;
    while(!flag.load(std::memory_order_acquire)&&c.healthy()) {
      if(GetTickCount64()>=deadline)throw std::runtime_error("fixture handoff timeout");
      Sleep(1);
    }
    if(!flag.load(std::memory_order_acquire))throw std::runtime_error("fixture handoff failed");
  }
  static commands::Batch awaitBatch(Connection& c) {
    commands::Batch batch;
    if(c.waitForWorkerBatch(batch)!=commands::Dequeue::Item)
      throw std::runtime_error("fixture batch unavailable");
    return batch;
  }
  static void oneAbsoluteCallBudget() {
    constexpr ULONGLONG start=1000;
    const auto budget=Connection::callDeadlinesAt(start);
    CHECK(budget.admission==6000&&budget.call==31000);
    const ULONGLONG admissionFinished=5999;
    CHECK(admissionFinished<budget.admission);
    CHECK(budget.call-admissionFinished==25001); // Not a fresh 30-second wait.
    const ULONGLONG restartBugDeadline=admissionFinished+30000;
    CHECK(restartBugDeadline>budget.call&&restartBugDeadline-budget.call==4999);
    // A reply at 32 seconds would pass the old restarted-deadline draft but
    // has expired under the exact production deadline helper used by call().
    constexpr ULONGLONG lateReply=33000;
    CHECK(lateReply>=budget.call&&lateReply<restartBugDeadline);
    CHECK(Connection::remainingWait(GetTickCount64())==0);
    CHECK(Connection::remainingWait(GetTickCount64()+30000)==lab::DeadlineMs);
    const auto shortDeadline=GetTickCount64()+50;
    CHECK(Connection::remainingWait(shortDeadline)<=50);
  }
  static void beforeInitialization() {
    auto c=make();
    CHECK(c->enqueueClear(clear(0xff010203))==D3DERR_INVALIDCALL);
    CHECK(c->sequence==0&&!c->initializedAcknowledged.load());
    CHECK(publish(*c,init()).sequence==1);
    CHECK(c->enqueueClear(clear(0xff040506))==D3DERR_INVALIDCALL);
    CHECK(c->sequence==1&&c->done.load()==0); // Published Init is not ready.
    complete(*c,take(*c));
    CHECK(c->enqueueClear(clear(0xff070809))==D3D_OK);
    CHECK(c->sequence==2&&c->done.load()==1); // Clear return is not its reply.
    complete(*c,take(*c),D3D_OK,0,7,0,0);
    CHECK(c->done.load()==2&&c->reply.submitted==7);
    CHECK(c->reply.completed==0&&c->reply.retired==0); // No invented GPU result.
    auto wrong=clear(0);wrong.op=d9::Op::Draw;
    CHECK(c->enqueueClear(wrong)==D3DERR_INVALIDCALL&&c->sequence==2);
  }
  static void initializationFailure() {
    auto c=make();CHECK(publish(*c,init()).sequence==1);
    complete(*c,take(*c),D3DERR_INVALIDCALL);
    CHECK(c->healthy()&&!c->initializedAcknowledged.load()&&c->done.load()==1);
    CHECK(c->enqueueClear(clear(0))==D3DERR_INVALIDCALL);
    d9::Header finish;finish.op=d9::Op::Finish;
    CHECK(publish(*c,finish).sequence==2);complete(*c,take(*c));
    c->closeProducer();
    const auto refused=publish(*c,clear(0));
    CHECK(refused.outcome==commands::Publish::Closed&&refused.sequence==0&&c->sequence==2);
    for(const auto bad:std::array<d9::Reply,2>{{{DWORD(S_FALSE),0,0,0,0,0},{DWORD(D3D_OK),0,0,0,0,4}}}) {
      auto other=make();CHECK(publish(*other,init()).sequence==1);auto batch=take(*other);
      bool rejected=false;
      try{complete(*other,batch,HRESULT(bad.result),bad.payloadBytes);}catch(const std::exception&){rejected=true;}
      CHECK(rejected&&!other->healthy()&&!other->initializedAcknowledged.load()&&other->done.load()==0);
    }
  }
  static void fullDoesNotConsumeAndPrivateCopy() {
    auto c=make();ready(*c);
    for(uint32_t i=0;i<commands::Capacity;++i)CHECK(c->enqueueClear(clear(0xff100000+i))==D3D_OK);
    CHECK(c->sequence==5&&c->done.load()==1);
    auto fifth=clear(0xff200000);const auto bytes=c->prepare(fifth,nullptr);
    for(unsigned i=0;i<5;++i) {
      const auto full=c->tryPublishPrepared(fifth,bytes);
      CHECK(full.outcome==commands::Publish::Full&&full.sequence==0);
      CHECK(c->sequence==5&&c->healthy()&&fifth.sequence==6);
    }
    auto held=take(*c);CHECK(header(held).args[0]==0xff100000);
    // One consumer-private copy frees a queue slot without completing the GPU.
    CHECK(c->enqueueClear(clear(0xff200000))==D3D_OK);
    CHECK(c->sequence==6&&c->done.load()==1&&c->maxObservedOutstanding==5);
    CHECK(header(held).args[0]==0xff100000);complete(*c,held);
    for(uint32_t i=1;i<4;++i) {
      const auto batch=take(*c);CHECK(header(batch).args[0]==0xff100000+i);complete(*c,batch);
    }
    const auto last=take(*c);CHECK(header(last).args[0]==0xff200000);complete(*c,last);
    CHECK(c->done.load()==6&&c->clearRemoteSucceeded==5&&c->healthy());
    const auto counters=c->queue.countersAfterJoin(); // Neither thread ever started.
    CHECK(counters.queued==0&&!counters.pendingCpuAck&&counters.published==6);
  }
  static void invalidAndExpiredAdmission() {
    auto c=make();ready(*c);bool failed=false;
    auto invalid=clear(0);invalid.payloadBytes=1;
    try{(void)c->prepare(invalid,nullptr);}catch(const std::exception&){failed=true;}
    CHECK(failed&&c->sequence==1&&c->healthy());
    auto expired=clear(0);failed=false;
    try{c->publishUntil(expired,nullptr,GetTickCount64());}catch(const std::exception&){failed=true;}
    CHECK(failed&&c->sequence==1&&c->healthy());
    CHECK(c->queue.countersAfterJoin().published==1);
  }
  static void asynchronousReplyFailures() {
    for(const auto bad:std::array<d9::Reply,3>{{{DWORD(S_FALSE),0,0,0,0,0},
        {DWORD(D3DERR_INVALIDCALL),0,0,0,0,0},{DWORD(D3D_OK),0,0,0,0,4}}}) {
      auto c=make();ready(*c);CHECK(c->enqueueClear(clear(0))==D3D_OK);auto batch=take(*c);
      bool rejected=false;
      try{complete(*c,batch,HRESULT(bad.result),bad.payloadBytes);}catch(const std::exception&){rejected=true;}
      CHECK(rejected&&!c->healthy()&&c->done.load()==1);
      CHECK(c->asyncFailureSequence==2&&c->asyncFailureResult==bad.result);
      CHECK(c->enqueueClear(clear(1))==D3DERR_DEVICELOST&&c->sequence==2);
      CHECK(c->queue.countersAfterJoin().faulted);
    }
    auto c=make();ready(*c);CHECK(c->enqueueClear(clear(0))==D3D_OK);
    complete(*c,take(*c),D3DERR_DEVICELOST);
    CHECK(c->deviceLost.load()&&!c->healthy()&&c->remoteTerminal&&c->done.load()==2);
    CHECK(c->asyncFailureSequence==2&&c->clearRemoteSucceeded==0);
    CHECK(c->enqueueClear(clear(1))==D3DERR_DEVICELOST);
  }
  static void realMethodsBarrierAndBackpressure(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    auto c=make();ready(*c);
    for(uint32_t i=0;i<4;++i)CHECK(c->enqueueClear(clear(0xff300000+i))==D3D_OK);
    std::atomic<bool> workerOkay{true};
    std::atomic<bool> fullObserved{false};c->fullObservedForTest=&fullObserved;
    std::array<uint32_t,4> colors{};
    c->worker=std::thread([&] {
      // Do not rely on the scheduler making a 30-ms pause sufficient. Release
      // this consumer only after the real publication code observes Full.
      // This is a CPU handoff, not a pipe delay or a GPU-pending fixture.
      unsigned count=0;
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"readback-backpressure");
        awaitFlag(fullObserved,*c);
        for(;;) {
          commands::Batch batch;const auto status=c->waitForWorkerBatch(batch);
          if(status==commands::Dequeue::Finished)break;
          if(status!=commands::Dequeue::Item)throw std::runtime_error("fixture dequeue");
          const auto h=header(batch);
          if(h.op==d9::Op::Clear) {
            if(count>=colors.size())throw std::runtime_error("fixture clear count");
            colors[count++]=h.args[0];complete(*c,batch);
          } else if(h.op==d9::Op::Readback)complete(*c,batch,D3D_OK,16,9,9,9);
          else throw std::runtime_error("fixture unexpected operation");
        }
      } catch(...) {workerOkay.store(false);c->failClosed();}
    });
    d9::Header readback;readback.op=d9::Op::Readback;
    std::array<uint8_t,16> output{};
    const auto result=c->call(readback,nullptr,output.data(),uint32_t(output.size()));
    c->closeProducer();c->worker.join();
    CHECK(result==D3D_OK&&workerOkay.load()&&c->sequence==6&&c->done.load()==6);
    for(uint32_t i=0;i<4;++i)CHECK(colors[i]==0xff300000+i);
    for(uint32_t i=0;i<output.size();++i)CHECK(output[i]==uint8_t(i^0x5a));
    CHECK(c->backpressureCalls==1&&c->fullObservations>0);
    CHECK(c->maxObservedOutstanding<=5&&c->clearPublished==4&&c->clearRemoteSucceeded==4);
    const auto counters=c->queue.countersAfterJoin();
    CHECK(counters.published==6&&counters.cpuAcknowledged==6&&counters.queued==0);
  }
  static void clearFailureWhileBarrierWaits(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    auto c=make();ready(*c);
    CHECK(c->enqueueClear(clear(0xff445566))==D3D_OK);
    std::atomic<bool> waiting{false},workerOkay{true};c->replyWaitObservedForTest=&waiting;
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"clear-failure-waiting-barrier");
        const auto batch=take(*c);CHECK(header(batch).op==d9::Op::Clear);
        awaitFlag(waiting,*c); // Real call(Readback) published and entered its wait.
        CHECK(c->done.load(std::memory_order_acquire)==1);
        bool rejected=false;
        try{complete(*c,batch,S_FALSE);}catch(const std::exception&){rejected=true;}
        CHECK(rejected&&!c->healthy()&&c->done.load(std::memory_order_acquire)==1);
      } catch(...) {workerOkay.store(false);c->failClosed();}
    });
    d9::Header readback;readback.op=d9::Op::Readback;
    std::array<uint8_t,16> output{};output.fill(0xcd);
    const auto result=c->call(readback,nullptr,output.data(),uint32_t(output.size()));
    c->closeProducer();c->worker.join();
    CHECK(result==D3DERR_DEVICELOST&&workerOkay.load()&&waiting.load());
    for(const auto byte:output)CHECK(byte==0xcd); // No stale/shared reply copied.
    CHECK(c->sequence==3&&c->done.load()==1&&c->asyncFailureSequence==2);
    CHECK(c->enqueueClear(clear(1))==D3DERR_DEVICELOST&&c->sequence==3);
    const auto counters=c->queue.countersAfterJoin();
    CHECK(counters.faulted&&counters.published==3&&counters.cpuAcknowledged==1);
    CHECK(counters.queued==1&&counters.pendingCpuAck); // Retained, not invented retirement.
  }
  static void actualDrawCallWaitsForItsReply(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    auto c=make();ready(*c);
    std::atomic<bool> waiting{false},workerOkay{true};c->replyWaitObservedForTest=&waiting;
    uint32_t waitsObserved=0;
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"draw-own-reply");
        for(uint32_t i=0;i<2;++i) {
          const auto batch=awaitBatch(*c);const auto h=header(batch);
          CHECK(h.op==d9::Op::Draw&&h.sequence==i+2);
          awaitFlag(waiting,*c); // Cannot reply until the actual call is waiting.
          CHECK(c->done.load(std::memory_order_acquire)==i+1);++waitsObserved;
          complete(*c,batch,i?D3D_OK:D3DERR_INVALIDCALL);
        }
      } catch(...) {workerOkay.store(false);c->failClosed();}
    });
    // This tests Connection::call(Draw) with synthetic CPU replies. It is not
    // host shader validation or public-COM/GPU Draw evidence.
    d9::Header draw;draw.op=d9::Op::Draw;
    const auto rejected=c->call(draw);
    const bool healthyAfterRejection=c->healthy();
    waiting.store(false,std::memory_order_release);
    const auto recovered=c->call(draw);
    c->closeProducer();c->worker.join();
    CHECK(workerOkay.load()&&waitsObserved==2&&waiting.load());
    CHECK(rejected==D3DERR_INVALIDCALL&&healthyAfterRejection&&recovered==D3D_OK);
    CHECK(c->healthy()&&c->sequence==3&&c->done.load()==3);
    CHECK(c->clearPublished==0&&c->clearRemoteSucceeded==0);
    const auto counters=c->queue.countersAfterJoin();
    CHECK(counters.published==3&&counters.cpuAcknowledged==3&&counters.queued==0);
  }
  static lab::Handle fixtureEvent() {
    lab::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event)throw std::runtime_error("fixture event creation");
    return event;
  }
  static void awaitEvent(HANDLE event) {
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)
      throw std::runtime_error("fixture event handoff timeout");
  }
  static void autoResetHintsAndCoalescing() {
    auto c=make(),other=make();
    CHECK(c->workAvailable.get()&&c->producerProgress.get());
    CHECK(c->workAvailable.get()!=c->producerProgress.get());
    for(const auto event:std::array<HANDLE,2>{{c->workAvailable.get(),c->producerProgress.get()}}) {
      DWORD flags=HANDLE_FLAG_INHERIT;
      CHECK(GetHandleInformation(event,&flags));
      CHECK(!(flags&HANDLE_FLAG_INHERIT));
      CHECK(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
      // No waiter exists yet. Two notifications coalesce but the one hint is
      // retained until this sole waiter consumes it; no ResetEvent is used.
      CHECK(SetEvent(event));CHECK(SetEvent(event));
      CHECK(WaitForSingleObject(event,0)==WAIT_OBJECT_0);
      CHECK(WaitForSingleObject(event,0)==WAIT_TIMEOUT);
    }
    CHECK(SetEvent(c->workAvailable.get()));
    CHECK(WaitForSingleObject(other->workAvailable.get(),0)==WAIT_TIMEOUT);
    CHECK(c->done.load()==0&&c->sequence==0&&other->done.load()==0);
    ready(*c);
    CHECK(WaitForSingleObject(c->workAvailable.get(),0)==WAIT_OBJECT_0);
    for(uint32_t i=0;i<4;++i)CHECK(c->enqueueClear(clear(0xff990000+i))==D3D_OK);
    CHECK(WaitForSingleObject(c->workAvailable.get(),0)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(c->workAvailable.get(),0)==WAIT_TIMEOUT);
    // The consumer has used the single coalesced hint. All four authoritative
    // batches still drain without a new signal or timer tick.
    for(uint32_t i=0;i<4;++i) {
      commands::Batch batch;
      CHECK(c->waitForWorkerBatch(batch)==commands::Dequeue::Item);
      CHECK(header(batch).args[0]==0xff990000+i);complete(*c,batch);
    }
    c->closeProducer();
    commands::Batch untouched;untouched.sequence=0xdead;
    CHECK(c->waitForWorkerBatch(untouched)==commands::Dequeue::Finished);
    CHECK(untouched.sequence==0xdead&&c->done.load()==5&&c->healthy());
  }
  static void invalidNotificationFailsClosed() {
    // No concurrent waiter exists when the fixture closes these handles.
    // This exercises real WAIT_FAILED and real SetEvent failure, not a fake
    // successful wake. It does not test closing a live worker's handle.
    auto waitFailure=make();waitFailure->producerProgress.reset();
    d9::Header readback;readback.op=d9::Op::Readback;
    std::array<uint8_t,32> guarded{};guarded.fill(0xa7);
    CHECK(waitFailure->call(readback,nullptr,guarded.data()+8,16)==D3DERR_DEVICELOST);
    for(const auto value:guarded)CHECK(value==0xa7);
    CHECK(!waitFailure->healthy()&&waitFailure->done.load()==0&&waitFailure->sequence==1);
    const auto waitCounters=waitFailure->queue.countersAfterJoin();
    CHECK(waitCounters.faulted&&waitCounters.queued==1&&waitCounters.published==1);
    CHECK(waitFailure->enqueueClear(clear(0))==D3DERR_DEVICELOST);
    auto signalFailure=make();signalFailure->workAvailable.reset();
    CHECK(signalFailure->call(init())==D3DERR_DEVICELOST);
    CHECK(!signalFailure->healthy()&&signalFailure->sequence==1&&signalFailure->done.load()==0);
    const auto signalCounters=signalFailure->queue.countersAfterJoin();
    CHECK(signalCounters.faulted&&signalCounters.queued==1&&signalCounters.cpuAcknowledged==0);
  }
  static void slotReleaseBeforeAcknowledgement(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    std::atomic<bool> full{false},admitted{false},workerOkay{true};
    auto c=make();ready(*c);c->fullObservedForTest=&full;
    for(uint32_t i=0;i<4;++i)CHECK(c->enqueueClear(clear(0xffaa0000+i))==D3D_OK);
    // Remove the old Init hint before concurrency. Only actual dequeue can
    // wake the following Full wait while the first Clear reply is withheld.
    CHECK(WaitForSingleObject(c->producerProgress.get(),0)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(c->producerProgress.get(),0)==WAIT_TIMEOUT);
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"slot-release-before-ack");
        awaitFlag(full,*c);
        auto held=awaitBatch(*c);CHECK(header(held).args[0]==0xffaa0000);
        awaitFlag(admitted,*c); // Admission MUST complete before this reply.
        complete(*c,held);
        for(uint32_t i=1;i<=4;++i) {
          auto batch=awaitBatch(*c);CHECK(header(batch).args[0]==0xffaa0000+i);
          complete(*c,batch);
        }
        commands::Batch end;CHECK(c->waitForWorkerBatch(end)==commands::Dequeue::Finished);
      } catch(...) {workerOkay.store(false);c->failClosed();}
    });
    const auto result=c->enqueueClear(clear(0xffaa0004));
    const auto doneBeforePermit=c->done.load(std::memory_order_acquire);
    admitted.store(true,std::memory_order_release);
    c->closeProducer();c->worker.join();
    CHECK(result==D3D_OK&&doneBeforePermit==1&&full.load()&&workerOkay.load());
    CHECK(c->sequence==6&&c->done.load()==6&&c->maxObservedOutstanding==5);
    CHECK(c->backpressureCalls==1&&c->fullObservations>=1);
  }
  static void emptyWorkerTerminalWake(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop,bool fault) {
    auto ended=fixtureEvent();
    std::atomic<bool> emptyObserved{false},workerOkay{true};
    commands::Dequeue result=commands::Dequeue::Unconfigured;
    auto c=make();c->workerWaitObservedForTest=&emptyObserved;
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,fault?"empty-fault":"empty-close");
        commands::Batch batch;result=c->waitForWorkerBatch(batch);
      } catch(...) {workerOkay.store(false);c->failClosed();}
      SetEvent(ended.get());
    });
    awaitFlag(emptyObserved,*c); // Actual Empty predicate, before actual wait.
    if(fault)c->failClosed();else c->closeProducer();
    const auto woke=WaitForSingleObject(ended.get(),1000);
    c->closeProducer();c->worker.join();
    CHECK(woke==WAIT_OBJECT_0&&workerOkay.load()); // Not the 5-second idle recheck.
    CHECK(result==(fault?commands::Dequeue::Faulted:commands::Dequeue::Finished));
    const auto counts=c->queue.countersAfterJoin();
    CHECK(counts.published==0&&counts.cpuAcknowledged==0&&counts.queued==0);
    CHECK(counts.faulted==fault&&c->healthy()!=fault);
  }
  static std::atomic<uint32_t>* replyWaitCounter;
  static void countReplyWait(Connection&) {
    replyWaitCounter->fetch_add(1,std::memory_order_release);
  }
  static void staleHintCannotComplete(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    std::atomic<uint32_t> waits{0};std::atomic<bool> workerOkay{true};
    auto c=make();ready(*c);
    replyWaitCounter=&waits;c->beforeReplyWaitForTest=&countReplyWait;
    // Init/dequeue has left a stale producer hint. The pending Draw must
    // consume it, recheck done, and enter its wait again before the fault.
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"stale-hint-then-fault");
        const auto batch=awaitBatch(*c);CHECK(header(batch).op==d9::Op::Draw);
        const auto until=GetTickCount64()+5000;
        while(waits.load(std::memory_order_acquire)<2&&c->healthy()&&GetTickCount64()<until)Sleep(1);
        CHECK(waits.load(std::memory_order_acquire)>=2);
        CHECK(c->done.load(std::memory_order_acquire)==1);
        c->failClosed();
      } catch(...) {workerOkay.store(false);c->failClosed();}
    });
    d9::Header draw;draw.op=d9::Op::Draw;
    const auto began=GetTickCount64();
    const auto result=c->call(draw);
    const auto elapsed=GetTickCount64()-began;
    c->closeProducer();c->worker.join();
    c->beforeReplyWaitForTest=nullptr;replyWaitCounter=nullptr;
    CHECK(result==D3DERR_DEVICELOST&&workerOkay.load()&&waits.load()>=2);
    CHECK(elapsed<1000); // A lost hint must not be hidden by the 5-second recheck.
    CHECK(c->done.load()==1&&c->sequence==2&&!c->healthy());
    const auto counts=c->queue.countersAfterJoin();
    CHECK(counts.published==2&&counts.cpuAcknowledged==1&&counts.pendingCpuAck);
  }
  static HANDLE replyParked,replyRelease;
  static void parkReplyWait(Connection& c) {
    c.beforeReplyWaitForTest=nullptr; // Only this first actual wait is paused.
    if(!SetEvent(replyParked))throw std::runtime_error("fixture parked signal");
    awaitEvent(replyRelease);
  }
  static void exactDoneBeforeFault(const std::wstring& expectedDesktop,
      const std::wstring& expectedInputDesktop) {
    auto parked=fixtureEvent(),release=fixtureEvent();
    std::atomic<bool> workerOkay{true};
    auto c=make();ready(*c);
    replyParked=parked.get();replyRelease=release.get();c->beforeReplyWaitForTest=&parkReplyWait;
    c->worker=std::thread([&] {
      try {
        workerDesktop(expectedDesktop,expectedInputDesktop,"settled-reply-before-fault");
        const auto batch=awaitBatch(*c);CHECK(header(batch).op==d9::Op::Readback);
        awaitEvent(parked.get());
        complete(*c,batch,D3D_OK,16);c->failClosed();
      } catch(...) {workerOkay.store(false);c->failClosed();}
      SetEvent(release.get()); // Both done and fault precede owner resumption.
    });
    std::array<uint8_t,32> guarded{};guarded.fill(0xcb);
    d9::Header readback;readback.op=d9::Op::Readback;
    const auto began=GetTickCount64();
    const auto result=c->call(readback,nullptr,guarded.data()+8,16);
    const auto elapsed=GetTickCount64()-began;
    c->closeProducer();c->worker.join();replyParked=nullptr;replyRelease=nullptr;
    CHECK(result==D3D_OK&&workerOkay.load()&&!c->healthy());
    CHECK(elapsed<1000); // The completion signal precedes the actual event wait.
    CHECK(c->done.load()==2&&c->sequence==2);
    for(uint32_t i=0;i<guarded.size();++i)
      CHECK(guarded[i]==(i>=8&&i<24?uint8_t((i-8)^0x5a):uint8_t(0xcb)));
    CHECK(c->enqueueClear(clear(0))==D3DERR_DEVICELOST);
  }
  static void run(const std::wstring& expectedDesktop,const std::wstring& expectedInputDesktop) {
    groupedEndScene(expectedDesktop,expectedInputDesktop);
    boundedDrawBatch(expectedDesktop,expectedInputDesktop);
    boundedFixedBatch(expectedDesktop,expectedInputDesktop);
    annotatedDrawBatch(expectedDesktop,expectedInputDesktop);
    asyncDrawBatch();
    asyncOrderedState();
    asyncEventCompletion();
    oneAbsoluteCallBudget();
    beforeInitialization();initializationFailure();fullDoesNotConsumeAndPrivateCopy();
    invalidAndExpiredAdmission();asynchronousReplyFailures();
    realMethodsBarrierAndBackpressure(expectedDesktop,expectedInputDesktop);
    clearFailureWhileBarrierWaits(expectedDesktop,expectedInputDesktop);
    actualDrawCallWaitsForItsReply(expectedDesktop,expectedInputDesktop);
    autoResetHintsAndCoalescing();invalidNotificationFailsClosed();
    slotReleaseBeforeAcknowledgement(expectedDesktop,expectedInputDesktop);
    emptyWorkerTerminalWake(expectedDesktop,expectedInputDesktop,false);
    emptyWorkerTerminalWake(expectedDesktop,expectedInputDesktop,true);
    staleHintCannotComplete(expectedDesktop,expectedInputDesktop);
    exactDoneBeforeFault(expectedDesktop,expectedInputDesktop);
  }
  // Draw records ride immediately before their own batch, at their draw's
  // index; a stage marker is an ordered barrier like any non-draw call.
  static void annotatedDrawBatch(const std::wstring& desktop,const std::wstring& input) {
    auto c=make();ready(*c);c->drawBatching=true;c->fixedBatching=true;c->annotations=true;
    const auto tagSchema=uint16_t(d9::AnnotationSchema::DrawTag);
    using Seen=std::vector<std::pair<uint16_t,uint64_t>>;
    auto objects=[](const commands::Batch& batch,const d9::Header& h) {
      d9::DrawAnnotations records;CHECK(d9::DecodeDrawAnnotations(h,batch.payload.data()+d9::HeaderBytes,records));
      Seen seen;records.forEach([&](uint16_t index,uint16_t,const uint8_t* data,uint16_t){seen.emplace_back(index,d9::U64(data));});
      return seen;
    };
    std::atomic<bool> workerOkay{true};
    c->worker=std::thread([&] {
      try {
        workerDesktop(desktop,input,"annotated-draw-batch");
        auto batch=awaitBatch(*c);auto h=header(batch);
        CHECK(h.op==d9::Op::AnnotateDraws&&h.args[0]==2&&h.args[1]==3&&h.args[2]==uint32_t(d9::Op::DrawBatch));
        CHECK((objects(batch,h)==Seen{{0,100},{2,102}}));complete(*c,batch);
        batch=awaitBatch(*c);CHECK(header(batch).op==d9::Op::DrawBatch&&header(batch).args[0]==3);complete(*c,batch);
        batch=awaitBatch(*c);h=header(batch);
        CHECK(h.op==d9::Op::StageMarker&&h.args[0]==uint32_t(d9::Stage::BeforeUi)&&!h.payloadBytes);complete(*c,batch);
        batch=awaitBatch(*c);h=header(batch);
        CHECK(h.op==d9::Op::AnnotateDraws&&h.args[0]==1&&h.args[1]==1&&h.args[2]==uint32_t(d9::Op::DrawFixedBatch));
        CHECK((objects(batch,h)==Seen{{0,103}}));complete(*c,batch);
        batch=awaitBatch(*c);CHECK(header(batch).op==d9::Op::DrawFixedBatch&&header(batch).args[0]==1);complete(*c,batch);
        batch=awaitBatch(*c);CHECK(header(batch).op==d9::Op::Readback);complete(*c,batch,D3D_OK,4);
      }catch(...){workerOkay=false;c->failClosed();}
    });
    auto tag=[&](Connection& owner,uint64_t object){
      const auto bytes=d9::EncodeDrawTag({object,1,0});return owner.annotateNextDraw(tagSchema,bytes.data(),d9::DrawTagBytes);};
    d9::DrawPacket draw{};d9::FixedDrawPacket fixed{};
    CHECK(tag(*c,100)==D3D_OK&&tag(*c,101)==D3DERR_INVALIDCALL); // One DrawTag per draw.
    CHECK(c->submitDraw(draw)==D3D_OK&&c->submitDraw(draw)==D3D_OK);
    CHECK(tag(*c,102)==D3D_OK&&c->submitDraw(draw)==D3D_OK);
    CHECK(c->drawBatchCount==3&&c->batchRecordCount==2&&c->annotatedDrawsAdmitted==2&&!c->nextRecordCount);
    CHECK(c->stageMarker(uint32_t(d9::Stage::BeforeUi))==D3D_OK);
    CHECK(c->drawBatchCount==0&&c->batchRecordCount==0&&c->annotationMessages==1&&c->stageMarkersSent==1);
    // A draw call that submits nothing discards its records.
    CHECK(tag(*c,999)==D3D_OK);c->discardNextDrawAnnotations();
    CHECK(c->annotationRecordsDiscarded==1&&!c->nextRecordCount);
    CHECK(tag(*c,103)==D3D_OK&&c->submitFixedDraw(fixed)==D3D_OK&&c->fixedBatchCount==1&&c->batchRecordCount==1);
    std::array<uint8_t,4> out{};d9::Header read;read.op=d9::Op::Readback;
    CHECK(c->call(read,nullptr,out.data(),4)==D3D_OK);
    c->closeProducer();c->worker.join();
    CHECK(workerOkay&&c->annotationMessages==2&&c->annotationRecordsAdmitted==3&&c->annotatedDrawsAdmitted==3);
    // Local refusals publish nothing: a session without the profile, bad
    // schema/size/data, an unknown stage and a nonfinite camera.
    auto off=make();ready(*off);
    CHECK(tag(*off,1)==D3DERR_INVALIDCALL&&off->stageMarker(1)==D3DERR_INVALIDCALL&&off->sequence==1);
    auto on=make();ready(*on);on->annotations=true;
    const auto bytes=d9::EncodeDrawTag({1,1,0});
    CHECK(on->annotateNextDraw(2,bytes.data(),d9::DrawTagBytes)==D3DERR_INVALIDCALL
      &&on->annotateNextDraw(tagSchema,bytes.data(),12)==D3DERR_INVALIDCALL
      &&on->annotateNextDraw(tagSchema,nullptr,d9::DrawTagBytes)==D3DERR_INVALIDCALL);
    CHECK(on->stageMarker(0)==D3DERR_INVALIDCALL&&on->stageMarker(5)==D3DERR_INVALIDCALL);
    d9::FrameCamera camera;camera.view[0]=std::numeric_limits<float>::quiet_NaN();
    const auto scene=d9::EncodeFrameCamera(camera);
    CHECK(on->frameScene(uint32_t(d9::AnnotationSchema::FrameCamera),scene.data(),d9::FrameCameraBytes)==D3DERR_INVALIDCALL);
    CHECK(on->sequence==1&&!on->nextRecordCount&&!on->annotationRecordsAdmitted);
  }
  static void boundedDrawBatch(const std::wstring& desktop,const std::wstring& input) {
    auto c=make();ready(*c);c->drawBatching=true;
    std::atomic<bool> workerOkay{true};
    c->worker=std::thread([&] {
      try {
        workerDesktop(desktop,input,"bounded-draw-batch");
        for(const unsigned count:{32u,3u}) {
          const auto batch=awaitBatch(*c);const auto h=header(batch);
          CHECK(h.op==d9::Op::DrawBatch&&h.args[0]==count&&h.payloadBytes==count*d9::DrawBytes);
          for(unsigned i=0;i<count;++i)for(unsigned j=0;j<d9::DrawBytes;++j)
            CHECK(batch.payload[d9::HeaderBytes+i*d9::DrawBytes+j]==uint8_t(i+(count==3?32:0)));
          complete(*c,batch,D3D_OK,0,1,0,0); // CPU reply is explicitly not GPU completion.
        }
        const auto read=awaitBatch(*c);CHECK(header(read).op==d9::Op::Readback);complete(*c,read,D3D_OK,4);
      }catch(...){workerOkay=false;c->failClosed();}
    });
    d9::DrawPacket packet{};
    for(unsigned i=0;i<35;++i){packet.fill(uint8_t(i));CHECK(c->submitDraw(packet)==D3D_OK);packet.fill(0xfe);}
    CHECK(c->drawBatchCount==3&&c->batchedDrawsAdmitted==35&&c->drawBatchesFlushed==1);
    const auto sequence=c->sequence;
    d9::Header diagnostic;diagnostic.op=d9::Op::DiagnosticCheckpoint;
    std::array<uint8_t,4> untouched{0xa1,0xb2,0xc3,0xd4};
    CHECK(c->call(diagnostic,nullptr,untouched.data(),4)==D3DERR_INVALIDCALL);
    CHECK(c->sequence==sequence&&c->drawBatchCount==3&&c->drawBatchesFlushed==1);
    CHECK((untouched==std::array<uint8_t,4>{0xa1,0xb2,0xc3,0xd4}));
    std::array<uint8_t,4> out{};d9::Header read;read.op=d9::Op::Readback;
    CHECK(c->call(read,nullptr,out.data(),4)==D3D_OK);
    c->closeProducer();c->worker.join();CHECK(workerOkay&&c->drawBatchCount==0&&c->drawBatchesFlushed==2);
    for(unsigned i=0;i<4;++i)CHECK(out[i]==uint8_t(i^0x5a));
    for(const HRESULT refusal:{D3DERR_INVALIDCALL,D3DERR_DEVICELOST}) {
      auto bad=make();ready(*bad);bad->drawBatching=true;
      bad->worker=std::thread([&]{try{complete(*bad,awaitBatch(*bad),refusal);}catch(...){bad->failClosed();}});
      CHECK(bad->submitDraw(packet)==D3D_OK);
      CHECK(bad->enqueueClear(clear(0))==D3DERR_DEVICELOST);
      bad->closeProducer();bad->worker.join();
      CHECK(!bad->healthy()&&bad->sequence==2&&bad->clearPublished==0&&bad->drawBatchCount==1);
      CHECK(bad->submitDraw(packet)==D3DERR_DEVICELOST&&bad->flushDraws()==D3DERR_DEVICELOST);
      CHECK(bad->sequence==2); // No retry/replay of an ambiguous admitted prefix.
    }
  }
  static void boundedFixedBatch(const std::wstring& desktop,const std::wstring& input) {
    auto c=make();ready(*c);c->drawBatching=true;c->fixedBatching=true;
    std::atomic<bool> workerOkay{true};
    // Programmable prefix, one full fixed packet, a fixed tail, then a
    // programmable draw: a kind switch flushes first, so remote order is call order.
    c->worker=std::thread([&] {
      try {
        workerDesktop(desktop,input,"bounded-fixed-batch");
        const std::pair<d9::Op,unsigned> expected[]={{d9::Op::DrawBatch,2},{d9::Op::DrawFixedBatch,20},
          {d9::Op::DrawFixedBatch,3},{d9::Op::DrawBatch,1}};
        unsigned first=0;
        for(const auto& [op,count]:expected) {
          const auto batch=awaitBatch(*c);const auto h=header(batch);
          const auto bytes=op==d9::Op::DrawBatch?d9::DrawBytes:d9::FixedDrawBytes;
          CHECK(h.op==op&&h.args[0]==count&&h.payloadBytes==count*bytes);
          for(unsigned i=0;i<count;++i)CHECK(batch.payload[d9::HeaderBytes+i*bytes]==uint8_t(first+i)
            &&batch.payload[d9::HeaderBytes+(i+1)*bytes-1]==uint8_t(first+i));
          first+=count;
          complete(*c,batch,D3D_OK,0,1,0,0);
        }
        const auto read=awaitBatch(*c);CHECK(header(read).op==d9::Op::Readback);complete(*c,read,D3D_OK,4);
      }catch(...){workerOkay=false;c->failClosed();}
    });
    d9::DrawPacket draw{};d9::FixedDrawPacket fixed{};unsigned index=0;
    for(unsigned i=0;i<2;++i){draw.fill(uint8_t(index++));CHECK(c->submitDraw(draw)==D3D_OK);}
    CHECK(c->drawBatchCount==2&&c->fixedBatchCount==0);
    for(unsigned i=0;i<23;++i){fixed.fill(uint8_t(index++));CHECK(c->submitFixedDraw(fixed)==D3D_OK);}
    CHECK(c->drawBatchCount==0&&c->fixedBatchCount==3&&c->drawBatchesFlushed==1&&c->fixedBatchesFlushed==1);
    const auto sequence=c->sequence;
    d9::Header diagnostic;diagnostic.op=d9::Op::DiagnosticCheckpoint;
    std::array<uint8_t,4> untouched{0xa1,0xb2,0xc3,0xd4};
    CHECK(c->call(diagnostic,nullptr,untouched.data(),4)==D3DERR_INVALIDCALL);
    CHECK(c->sequence==sequence&&c->fixedBatchCount==3&&c->fixedBatchesFlushed==1);
    CHECK((untouched==std::array<uint8_t,4>{0xa1,0xb2,0xc3,0xd4}));
    draw.fill(uint8_t(index++));CHECK(c->submitDraw(draw)==D3D_OK);
    CHECK(c->fixedBatchCount==0&&c->drawBatchCount==1&&c->fixedBatchesFlushed==2);
    std::array<uint8_t,4> out{};d9::Header read;read.op=d9::Op::Readback;
    CHECK(c->call(read,nullptr,out.data(),4)==D3D_OK);
    c->closeProducer();c->worker.join();
    CHECK(workerOkay&&c->drawBatchCount==0&&c->drawBatchesFlushed==2&&c->fixedDrawsAdmitted==23);
    CHECK(c->fixedBatchesRemoteSucceeded==2&&c->drawBatchesRemoteSucceeded==2);
    // Without the fixed profile a draw stays one synchronous, recoverable DrawFixed.
    auto plain=make();ready(*plain);plain->drawBatching=true;
    plain->worker=std::thread([&]{try{const auto b=awaitBatch(*plain);
      CHECK(header(b).op==d9::Op::DrawFixed&&header(b).payloadBytes==d9::FixedDrawBytes);
      complete(*plain,b,D3DERR_INVALIDCALL);}catch(...){plain->failClosed();}});
    CHECK(plain->submitFixedDraw(fixed)==D3DERR_INVALIDCALL&&plain->fixedBatchCount==0&&plain->sequence==2);
    plain->closeProducer();plain->worker.join();CHECK(plain->healthy()&&plain->fixedDrawsAdmitted==0);
    // A deferred fixed refusal is terminal and never replayed, as for DrawBatch.
    for(const HRESULT refusal:{D3DERR_INVALIDCALL,D3DERR_DEVICELOST}) {
      auto bad=make();ready(*bad);bad->drawBatching=true;bad->fixedBatching=true;
      bad->worker=std::thread([&]{try{complete(*bad,awaitBatch(*bad),refusal);}catch(...){bad->failClosed();}});
      CHECK(bad->submitFixedDraw(fixed)==D3D_OK);
      CHECK(bad->enqueueClear(clear(0))==D3DERR_DEVICELOST);
      bad->closeProducer();bad->worker.join();
      CHECK(!bad->healthy()&&bad->sequence==2&&bad->clearPublished==0&&bad->fixedBatchCount==1);
      CHECK(bad->submitFixedDraw(fixed)==D3DERR_DEVICELOST&&bad->flushDraws()==D3DERR_DEVICELOST);
      CHECK(bad->sequence==2);
    }
  }
  static void groupedEndScene(const std::wstring& desktop,const std::wstring& input) {
    auto c=make();ready(*c);c->drawBatching=true;c->groupedSubmissions=true;c->asyncDrawBatches=true;
    std::atomic<bool> workerOkay{true};
    c->worker=std::thread([&] {
      try {
        workerDesktop(desktop,input,"gpu-group-end-scene");
        const auto draw=awaitBatch(*c);CHECK(header(draw).op==d9::Op::DrawBatch&&header(draw).args[0]==1);
        complete(*c,draw,D3D_OK,0,0,0,0); // Recording is not GPU submission or completion.
        const auto flush=awaitBatch(*c);CHECK(header(flush).op==d9::Op::FlushDraws);
        complete(*c,flush,D3D_OK,0,1,0,0); // Submission still does not authorize GPU retirement.
      } catch(...) {workerOkay=false;c->failClosed();}
    });
    d9::DrawPacket packet{};CHECK(c->submitDraw(packet)==D3D_OK);
    CHECK(c->drawBatchCount==1&&c->endScene()==D3D_OK);
    c->closeProducer();c->worker.join();
    CHECK(workerOkay&&c->drawBatchCount==0&&c->drawBatchesRemoteSucceeded==1);
    CHECK(c->reply.completed==0&&c->reply.retired==0&&c->healthy());
  }
  static void asyncEventCompletion() {
    auto c=make();ready(*c);c->asyncEvents=true;
    d9::Header h;h.op=d9::Op::Poll;h.id=7;h.generation=1;h.args[0]=1;h.args[2]=9;
    CHECK(c->pollEvent(h)==S_FALSE&&c->sequence==2);
    const auto waiting=take(*c);CHECK(header(waiting).op==d9::Op::WaitEvent);
    for(unsigned i=0;i<20000;++i)CHECK(c->pollEvent(h)==S_FALSE&&c->sequence==2);
    // Different Query and later successful Issue wait behind the same single
    // outstanding observation, without consuming another queue slot.
    auto later=h;later.id=8;later.args[0]=2;later.args[2]=10;
    CHECK(c->pollEvent(later)==S_FALSE&&c->sequence==2);
    CHECK(c->eventGpuCompleted.load()==0&&c->eventWaitsSucceeded==0);
    complete(*c,waiting,D3D_OK,0,10,9,0);
    CHECK(c->pollEvent(h)==S_OK&&c->sequence==2&&c->reply.retired==0);
    h.args[0]=2;CHECK(c->pollEvent(h)==S_OK); // Re-Issue may share GPU serial.
    CHECK(c->pollEvent(later)==S_FALSE&&c->sequence==3);
    complete(*c,take(*c),D3D_OK,0,10,10,0);
    CHECK(c->pollEvent(later)==S_OK&&c->eventWaitsSucceeded==2);
    CHECK(c->eventWaitsPublished==2&&c->eventPendingObservations==20001);
    for(const unsigned mode:{0u,1u,2u,3u,4u}) {
      auto bad=make();ready(*bad);bad->asyncEvents=true;
      CHECK(bad->pollEvent(h)==S_FALSE);
      try {complete(*bad,take(*bad),mode==1?D3DERR_INVALIDCALL:mode==2?D3DERR_DEVICELOST:
          mode==3?S_FALSE:D3D_OK,mode==4?4:0,9,mode==0?8:9,0);} catch(...) {}
      CHECK(!bad->healthy()&&bad->eventGpuCompleted.load()==0&&bad->eventWaitsSucceeded==0);
      CHECK(bad->pollEvent(h)==D3DERR_DEVICELOST&&bad->sequence==2);
    }
    auto fresh=make();ready(*fresh);fresh->asyncEvents=true;
    CHECK(fresh->eventGpuCompleted.load()==0&&fresh->pollEvent(h)==S_FALSE);
    CHECK(fresh->eventWaitsPublished==1); // No old Connection watermark reuse.
    fresh->failClosed();
    for(unsigned mode=0;mode<5;++mode) {
      auto invalid=h;
      if(mode==0)invalid.id=0;
      if(mode==1)invalid.generation=0;
      if(mode==2)invalid.args[0]=0;
      if(mode==3)invalid.args[2]=0;
      if(mode==4)invalid.args[4]=1;
      CHECK(c->pollEvent(invalid)==D3DERR_INVALIDCALL&&c->sequence==3);
    }
    c->failClosed();CHECK(c->pollEvent(later)==D3DERR_DEVICELOST); // Cached completion is not health.
  }
  static void asyncOrderedState();
  static void asyncDrawBatch() {
    auto c=make();ready(*c);c->drawBatching=true;c->asyncDrawBatches=true;
    d9::DrawPacket packet{};
    // No consumer: four whole batches must fit without any acknowledgement.
    // The first prefix's bytes must survive mutation of the caller's packet.
    for(unsigned i=0;i<128;++i) {
      packet.fill(uint8_t(i));CHECK(c->submitDraw(packet)==D3D_OK);packet.fill(0xfe);
    }
    CHECK(c->drawBatchCount==0&&c->drawBatchesFlushed==4&&c->done.load()==1);
    CHECK(c->queue.countersAfterJoin().queued==4&&c->maxObservedOutstanding==4); // No worker exists here.
    for(unsigned b=0;b<4;++b) {
      const auto batch=take(*c);CHECK(header(batch).op==d9::Op::DrawBatch);
      for(unsigned i=0;i<32;++i)for(unsigned j=0;j<d9::DrawBytes;++j)
        CHECK(batch.payload[d9::HeaderBytes+i*d9::DrawBytes+j]==uint8_t(b*32+i));
      complete(*c,batch,D3D_OK,0,b+1,0,0);
    }
    CHECK(c->drawBatchesRemoteSucceeded==4&&c->reply.completed==0&&c->reply.retired==0);
    // Delayed refusal/malformed reply cannot disappear behind local success.
    for(const auto outcome:std::array<std::pair<HRESULT,unsigned>,4>{{
        {D3DERR_INVALIDCALL,0},{D3DERR_DEVICELOST,0},{D3D_OK,4},{S_FALSE,0}}}) {
      auto bad=make();ready(*bad);bad->drawBatching=true;bad->asyncDrawBatches=true;
      CHECK(bad->submitDraw(packet)==D3D_OK&&bad->flushDraws()==D3D_OK);
      const auto batch=take(*bad);
      try {complete(*bad,batch,outcome.first,outcome.second);}catch(...) {}
      CHECK(!bad->healthy()&&bad->asyncFailureSequence==2);
      CHECK(bad->flushDraws()==D3DERR_DEVICELOST&&bad->submitDraw(packet)==D3DERR_DEVICELOST);
      CHECK(bad->sequence==2&&bad->drawBatchesRemoteSucceeded==0);
    }
  }
};
// S1 G6. NOOVERWRITE content passes the staged prefix; DISCARD content and
// state flush it first. Payloads are private copies; a refusal is terminal.
void ConnectionCpuTest::asyncOrderedState() {
  const auto upload=[](uint32_t mode,uint32_t bytes) {
    d9::Header h;h.op=d9::Op::UploadBuffer;h.id=1;h.generation=1;h.args[1]=mode;h.payloadBytes=bytes;return h;
  };
  const auto effects=d9::EncodeEffects(d9::EffectsState{});
  d9::Header state;state.op=d9::Op::SetEffects;state.payloadBytes=d9::EffectsBytes;
  std::array<uint8_t,64> content{};content.fill(0x11);
  {
    auto plain=make();ready(*plain);plain->drawBatching=true;plain->fixedBatching=true;
    CHECK(plain->enqueueOrdered(upload(1,64),content.data(),true)==D3DERR_INVALIDCALL&&plain->sequence==1);
    CHECK(!plain->asyncOrderedEnabled()&&plain->orderedUploadsPublished==0);
  }
  auto c=make();
  c->drawBatching=true;c->fixedBatching=true;c->asyncDrawBatches=true;
  CHECK(c->enqueueOrdered(upload(1,64),content.data(),true)==D3DERR_INVALIDCALL&&c->sequence==0); // Not initialized.
  ready(*c);
  CHECK(c->enqueueOrdered(clear(0),nullptr)==D3DERR_INVALIDCALL&&c->sequence==1);
  CHECK(c->enqueueOrdered(state,effects.data(),true)==D3DERR_INVALIDCALL&&c->sequence==1);
  d9::FixedDrawPacket fixed{};fixed.fill(0x21);CHECK(c->submitFixedDraw(fixed)==D3D_OK&&c->fixedBatchCount==1);
  CHECK(c->enqueueOrdered(upload(1,64),content.data(),true)==D3D_OK);
  content.fill(0x33); // The admitted copy is private.
  CHECK(c->sequence==2&&c->fixedBatchCount==1&&c->orderedPassedStaged==1);
  fixed.fill(0x22);CHECK(c->submitFixedDraw(fixed)==D3D_OK&&c->fixedBatchCount==2);
  CHECK(c->enqueueOrdered(upload(2,64),content.data())==D3D_OK); // DISCARD: the two draws go first.
  CHECK(c->sequence==4&&c->fixedBatchCount==0&&c->fixedBatchesFlushed==1&&c->orderedPassedStaged==1);
  CHECK(c->enqueueOrdered(upload(1,64),content.data(),true)==D3D_OK&&c->orderedPassedStaged==1); // Nothing staged.
  // Four admitted without any reply: the queue is full and nothing waited.
  CHECK(c->sequence==5&&c->done.load()==1&&c->queue.countersAfterJoin().queued==4);
  const auto expect=[&](d9::Op op,uint8_t first) {
    const auto batch=take(*c);const auto h=header(batch);CHECK(h.op==op);
    if(op==d9::Op::UploadBuffer)CHECK(h.payloadBytes==64&&batch.payload[d9::HeaderBytes]==first
      &&batch.payload[d9::HeaderBytes+63]==first);
    if(op==d9::Op::DrawFixedBatch)CHECK(h.args[0]==2&&batch.payload[d9::HeaderBytes]==0x21
      &&batch.payload[d9::HeaderBytes+d9::FixedDrawBytes]==0x22);
    complete(*c,batch);
  };
  expect(d9::Op::UploadBuffer,0x11);expect(d9::Op::DrawFixedBatch,0);
  CHECK(c->enqueueOrdered(state,effects.data())==D3D_OK&&c->sequence==6);
  CHECK(c->orderedUploadsPublished==3&&c->orderedStatesPublished==1);
  expect(d9::Op::UploadBuffer,0x33);expect(d9::Op::UploadBuffer,0x33);expect(d9::Op::SetEffects,0);
  CHECK(c->healthy()&&c->done.load()==6&&c->orderedRemoteSucceeded==4&&c->fixedBatchesRemoteSucceeded==1);
  // A call() of ordered state keeps its recoverable refusal in this profile.
  auto sync=make();ready(*sync);sync->drawBatching=true;sync->asyncDrawBatches=true;
  sync->worker=std::thread([&]{try{const auto b=awaitBatch(*sync);CHECK(header(b).op==d9::Op::SetEffects);
    complete(*sync,b,D3DERR_INVALIDCALL);}catch(...){sync->failClosed();}});
  CHECK(sync->call(state,effects.data())==D3DERR_INVALIDCALL);
  sync->closeProducer();sync->worker.join();
  CHECK(sync->healthy()&&sync->asyncFailureSequence==0&&sync->orderedRemoteSucceeded==0);
  // A refusal, malformed or lost reply cannot vanish behind the local success.
  for(const auto outcome:std::array<std::pair<HRESULT,unsigned>,4>{{
      {D3DERR_INVALIDCALL,0},{D3DERR_DEVICELOST,0},{D3D_OK,4},{S_FALSE,0}}}) {
    auto bad=make();ready(*bad);bad->drawBatching=true;bad->asyncDrawBatches=true;
    CHECK(bad->enqueueOrdered(upload(0,64),content.data())==D3D_OK);
    try {complete(*bad,take(*bad),outcome.first,outcome.second);} catch(...) {}
    CHECK(!bad->healthy()&&bad->asyncFailureSequence==2&&bad->orderedRemoteSucceeded==0);
    CHECK(bad->enqueueOrdered(upload(1,64),content.data(),true)==D3DERR_DEVICELOST&&bad->sequence==2);
  }
}
std::atomic<unsigned> ConnectionCpuTest::checks{0};
std::atomic<uint32_t>* ConnectionCpuTest::replyWaitCounter=nullptr;
HANDLE ConnectionCpuTest::replyParked=nullptr;
HANDLE ConnectionCpuTest::replyRelease=nullptr;
} // namespace warvk::host::d3d9proxy

int wmain(int argc,wchar_t** argv) {
  try {
    namespace d9=warvk::host::d9;
    if(argc!=5||std::wcscmp(argv[1],L"--expect-desktop")||std::wcscmp(argv[3],L"--input-desktop")
      ||!argv[2][0]||!argv[4][0])
      throw std::runtime_error("expected --expect-desktop <bareName> --input-desktop <bareName>");
    const std::wstring expectedDesktop=argv[2],expectedInputDesktop=argv[4];
    const auto isolated=d9::RequireIsolatedDesktop();
    const auto desktop=d9::CurrentDesktop(),input=d9::InputDesktop();
    if(!_wcsicmp(desktop.c_str(),L"Default")||!_wcsicmp(expectedDesktop.c_str(),L"Default")
      ||isolated!=L"WinSta0\\"+expectedDesktop||desktop!=expectedDesktop||input!=expectedInputDesktop)
      throw std::runtime_error("X1a CPU owner desktop mismatch/default forbidden");
    std::printf("{\"schema\":\"warvk-x1a-cpu-desktop/v1\",\"role\":\"owner\","
      "\"pid\":%lu,\"tid\":%lu,\"bits\":%u,\"desktop\":%s,\"inputDesktop\":%s,"
      "\"isolatedDesktop\":%s,\"gpuTested\":false}\n",
      GetCurrentProcessId(),GetCurrentThreadId(),unsigned(sizeof(void*)*8),
      d9::NameJson(desktop).c_str(),d9::NameJson(input).c_str(),d9::NameJson(isolated).c_str());
    std::fflush(stdout);
    warvk::host::d3d9proxy::ConnectionCpuTest::run(expectedDesktop,expectedInputDesktop);
    std::printf("X1A_CPU checks=%u ipcTested=0 gpuTested=0 productOwnerComplete=0\n",
      warvk::host::d3d9proxy::ConnectionCpuTest::checks.load(std::memory_order_relaxed));
    return 0;
  } catch(const std::exception& e) {std::fprintf(stderr,"X1A_CPU failure: %s\n",e.what());return 1;}
}
