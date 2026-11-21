// Real x86 Connection in command-ring mode against the real x64 host. One
// session per --case: each stages one terminal cause and reports what the
// Connection concluded; the gate reads the host's own account beside it.
// The friend seam is used only to damage what x86 itself published, to stop
// the host process, and to measure the views the ring adds. No game, no COM.
#define WARVK_D3D9_CONNECTION_CPU_TEST 1
#include "../tools/render_host/d3d9_connection.h"
#include <cstring>
#include <string>
#include <vector>

namespace warvk::host::d3d9proxy {
namespace {
void Require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
struct View { uintptr_t base; size_t bytes; };
// Every mapped view of this process, one entry per allocation.
std::vector<View> MappedViews() {
  std::vector<View> views; MEMORY_BASIC_INFORMATION info{};
  uintptr_t at = 0x10000;
  while (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info)) {
    if (info.State != MEM_FREE && info.Type == MEM_MAPPED) {
      const auto base = uintptr_t(info.AllocationBase);
      if (!views.empty() && views.back().base == base) views.back().bytes += info.RegionSize;
      else views.push_back({base, info.RegionSize});
    }
    const auto next = uintptr_t(info.BaseAddress) + info.RegionSize;
    if (next <= at) break;
    at = next;
  }
  return views;
}
}
struct ConnectionCpuTest {
  using Terminal = Connection::Terminal;
  static std::atomic<int> hookCalls;
  static void hook(void*, const Connection::TerminalReport&) { ++hookCalls; }
  static d9::Header clear(uint32_t color) {
    d9::Header h; h.op = d9::Op::Clear; h.args[0] = color; h.args[1] = d9::FloatBits(1.0f); h.args[2] = D3DCLEAR_TARGET; return h;
  }
  static HRESULT barrier(Connection& c) { d9::Header h; h.op = d9::Op::FlushDraws; return c.call(h); }
  // Raw bytes at the published tail, as a wild write or a hostile producer would leave them.
  static void inject(Connection& c, const uint8_t* bytes, uint32_t count, uint64_t advance) {
    auto* block = c.ringChannel->producerBlock(); const auto tail = block->tail.load();
    const uint32_t offset = uint32_t(tail) & (c.ringCapacity - 1);
    Require(offset + count <= c.ringCapacity, "injection must not wrap");
    if (count) std::memcpy(c.ringChannel->data() + offset, bytes, count);
    block->tail.store(tail + advance); SetEvent(c.ringChannel->work());
  }
  // A record framed exactly as the producer frames it, around header bytes of the case's choosing.
  static std::array<uint8_t, 144> record(const uint8_t* header, uint32_t sequenceLow, uint32_t checkDamage = 0) {
    std::array<uint8_t, 144> r{};
    d9::P32(r.data(), d9::HeaderBytes); d9::P32(r.data() + 8, sequenceLow);
    d9::P32(r.data() + 12, d9::ring::Check(r.data(), header) ^ checkDamage);
    std::memcpy(r.data() + d9::ring::RecordHeaderBytes, header, d9::HeaderBytes); return r;
  }
  static void awaitHostFault(Connection& c, d9::ring::Fault expected) {
    const auto deadline = GetTickCount64() + 10000;
    while (c.ring->hostState().state != d9::ring::State::Faulted) {
      Require(GetTickCount64() < deadline, "host did not refuse the damaged ring"); Sleep(1);
    }
    Require(c.ring->hostState().code == expected, "host refused for another reason");
  }
  static HANDLE openHost(Connection& c) {
    const HANDLE host = OpenProcess(PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE, FALSE, c.childPid);
    Require(host != nullptr, "host process handle"); return host;
  }
  static void suspendOrResume(HANDLE host, const char* name) {
    using Fn = LONG(NTAPI*)(HANDLE);
    const auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name)));
    Require(fn && fn(host) >= 0, "host suspend/resume");
  }
  static int run(const std::string& name) {
    if(name=="resource-full")Connection::resourceFixtureArgument=L"--test-resource-one-slot";
    if(name=="resource-allocation")Connection::resourceFixtureArgument=L"--test-resource-allocation";
    if(name=="resource-after-apply")Connection::resourceFixtureArgument=L"--test-resource-after-apply";
    if(name=="resource-device-lost")Connection::resourceFixtureArgument=L"--test-resource-device-lost";
    if(name=="resource-hung")Connection::resourceFixtureArgument=L"--test-resource-delay";
    Connection c; c.setTerminalHook(&ConnectionCpuTest::hook, nullptr);
    Require(c.ringRequested && c.ringCapacity == d9::ring::DefaultCapacity, "WARVK_D3D9_COMMAND_RING=1 required");
    d9::Header init; init.op = d9::Op::Initialize; init.args[0] = init.args[1] = 64;
    const bool resourceCase=name.rfind("resource-",0)==0;
    if(resourceCase)init.args[6]=d9::InitializeRetainContent;
    if (name == "scene-stopped-reset") init.args[6] = d9::InitializeAnnotations;
    size_t newViews = 0, newViewBytes = 0, smallestView = 0, largestView = 0;
    if (name == "healthy") {
      // Attach by hand so the views it adds can be told from everything else.
      c.ringRequested = false; Require(c.call(init) == D3D_OK && !c.ring, "initialize on the pipe");
      const auto before = MappedViews();
      c.ringRequested = true; Require(c.attachRing() == D3D_OK, "attach");
      for (const auto& view : MappedViews()) {
        bool known = false;
        for (const auto& old : before) known = known || old.base == view.base;
        if (known) continue;
        ++newViews; newViewBytes += view.bytes;
        smallestView = smallestView ? std::min(smallestView, view.bytes) : view.bytes;
        largestView = std::max(largestView, view.bytes);
      }
    } else Require(c.call(init) == D3D_OK, "initialize and attach");
    Require(c.ring && c.ringAttachSequence == 2 && barrier(c) == D3D_OK && c.healthy(), "healthy ring barrier");
    const uint32_t next = c.sequence + 1;
    d9::Header valid; valid.op = d9::Op::FlushDraws; valid.nonce = c.nonce; valid.sequence = next;
    std::array<uint8_t, d9::HeaderBytes> header{};
    Terminal kind = Terminal::None; const char* detail = ""; uint32_t result = 0;
    ULONGLONG elapsed = 0; HRESULT last = D3D_OK; bool sceneRefused = false;
    auto damaged = [&](d9::ring::Fault fault) {
      awaitHostFault(c, fault); kind = Terminal::RingCorrupt; detail = d9::ring::FaultName(fault);
    };
    if (name == "healthy") {
      for (uint32_t i = 0; i < 2000; ++i) Require(c.enqueueClear(clear(0xff000000u | i)) == D3D_OK, "clear");
      Require(barrier(c) == D3D_OK && c.settledSequence() == c.sequence, "executed prefix");
      d9::Header finish; finish.op = d9::Op::Finish; Require(c.call(finish) == D3D_OK, "finish");
    } else if(resourceCase) {
      using Op=d9::Op;
      d9::Header create;create.op=Op::CreateTexture2D;create.id=create.generation=1;
      create.args={1,1,1,D3DFMT_A8R8G8B8};
      d9::Header destroy;destroy.op=Op::Destroy;destroy.id=destroy.generation=1;destroy.args[0]=uint32_t(d9::Type::Texture);
      if(name=="resource-reuse") {
        auto bad=create;bad.args[3]=D3DFMT_UNKNOWN;
        Require(c.call(bad)==D3DERR_INVALIDCALL&&c.call(create)==D3D_OK,"sync refusal preserved generation");
        d9::Header upload;upload.op=Op::UploadTexture;upload.id=upload.generation=1;upload.payloadBytes=4;
        d9::Header read;read.op=Op::ReadContent;read.id=read.generation=1;read.args[2]=4;
        uint32_t pixel=0;
        for(uint32_t color:{0x11223344u,0xaabbccddu}) {
          Require(c.call(upload,&color)==D3D_OK&&c.call(read,nullptr,&pixel,4)==D3D_OK&&pixel==color,"generation content");
          c.ringPublish(destroy,nullptr,false);
          if(create.generation==1) {
            ++create.generation;c.ringPublish(create,nullptr,false);
            Require(c.call(read,nullptr,&pixel,4)==D3DERR_INVALIDCALL,"old generation denied");
            ++read.generation;++upload.generation;++destroy.generation;
            Require(c.call(read,nullptr,&pixel,4)==D3DERR_INVALIDCALL,"new generation has no old content");
          }
        }
        d9::Header finish;finish.op=Op::Finish;Require(c.call(finish)==D3D_OK,"finish after reuse");
      } else if(name=="resource-allocation"||name=="resource-after-apply"||name=="resource-device-lost"||name=="resource-hung") {
        Require(c.call(create)==D3D_OK,"sync first texture");++create.id;
        const auto prior=c.sequence;
        if(name=="resource-hung")c.ringNoProgressMs=1000;
        auto target=name=="resource-allocation"||name=="resource-hung"?create:destroy;
        c.ringPublish(target,nullptr,false);
        if(name=="resource-device-lost") {
          awaitHostFault(c,d9::ring::Fault::DeviceLost);const auto fault=c.ring->hostState();
          result=uint32_t(D3DERR_DEVICELOST);kind=Terminal::HostDeviceLost;detail="device-lost";
          Require(fault.sequence==target.sequence&&fault.result==result,"device loss sequence/result");
        } else if(name=="resource-hung") {
          const auto start=GetTickCount64();last=barrier(c);elapsed=GetTickCount64()-start;
          Require(last==D3DERR_DEVICELOST&&elapsed>=1000&&elapsed<10000,"bounded resource stall");
          kind=Terminal::HostHung;detail="no progress";
        } else {
          const auto deadline=GetTickCount64()+10000;
          while(c.healthy()){Require(GetTickCount64()<deadline,"host exit unobserved");Sleep(1);}
          kind=Terminal::HostExited;detail="host process exited";
        }
        Require(c.ring->peekApplied()==prior,"unacknowledged operation advanced applied");
      } else {
        auto bad=create;
        if(name=="resource-unnegotiated-create") {
          Require(c.resourceFeatures()==d9::InitializeAsyncDestroy,"only destroy negotiated");
        } else if(name=="resource-format")bad.args[3]=D3DFMT_UNKNOWN;
        else {
          Require(c.call(create)==D3D_OK,"first sync creation");
          if(name=="resource-full")++bad.id;
          else if(name!="resource-duplicate") {
            bad=destroy;
            if(name=="resource-stale") {
              Require(c.call(destroy)==D3D_OK,"negotiated sync destroy");++create.generation;
              Require(c.call(create)==D3D_OK,"same id new generation");
            } else if(name=="resource-wrong-type")bad.args[0]=uint32_t(d9::Type::IndexBuffer);
            else Require(name=="resource-unnegotiated-destroy"&&c.resourceFeatures()==d9::InitializeAsyncCreateTexture,
              "only creation negotiated");
          }
        }
        // Frame both records together: the successor really is available when
        // the host refuses the first, regardless of process scheduling.
        const uint32_t seq=c.sequence+1;
        bad.nonce=c.nonce;bad.sequence=seq;Require(d9::EncodeHeader(bad,header.data()),"resource header");
        const auto first=record(header.data(),seq);
        auto after=clear(0u);after.nonce=c.nonce;after.sequence=seq+1;
        Require(d9::EncodeHeader(after,header.data()),"successor header");const auto second=record(header.data(),seq+1);
        std::array<uint8_t,288> pair{};std::copy(first.begin(),first.end(),pair.begin());
        std::copy(second.begin(),second.end(),pair.begin()+first.size());inject(c,pair.data(),pair.size(),pair.size());
        awaitHostFault(c,d9::ring::Fault::AsyncRefused);const auto fault=c.ring->hostState();
        const uint32_t expected=uint32_t(name=="resource-full"?D3DERR_OUTOFVIDEOMEMORY:D3DERR_INVALIDCALL);
        Require(fault.sequence==seq&&fault.result==expected&&c.ring->peekApplied()==seq-1
          &&c.ringChannel->consumerBlock()->head.load()+second.size()==c.ringChannel->producerBlock()->tail.load(),
          "fault result, applied prefix and unread successor");
        kind=Terminal::HostRejected;detail="async-refused";result=fault.result;
      }
    } else if (name == "record-header") {
      const std::array<uint8_t, 32> noise = [] { std::array<uint8_t, 32> a{}; a.fill(0xff); return a; }();
      inject(c, noise.data(), 32, 32); damaged(d9::ring::Fault::RecordHeader);
    } else if (name == "truncated") {
      inject(c, nullptr, 0, 8); damaged(d9::ring::Fault::Truncated);
    } else if (name == "tail-invalid") {
      inject(c, nullptr, 0, uint64_t(c.ringCapacity) + 16); damaged(d9::ring::Fault::TailInvalid);
    } else if (name == "checksum") {
      Require(d9::EncodeHeader(valid, header.data()), "header");
      const auto r = record(header.data(), next, 1); inject(c, r.data(), 144, 144); damaged(d9::ring::Fault::Checksum);
    } else if (name == "message-header") {
      Require(d9::EncodeHeader(valid, header.data()), "header"); header[0] ^= 0x20;
      const auto r = record(header.data(), next); inject(c, r.data(), 144, 144); damaged(d9::ring::Fault::MessageHeader);
    } else if (name == "scope") {
      auto other = valid; other.nonce.low ^= 1; Require(d9::EncodeHeader(other, header.data()), "header");
      const auto r = record(header.data(), next); inject(c, r.data(), 144, 144); damaged(d9::ring::Fault::Scope);
    } else if (name == "sequence") {
      auto skipped = valid; ++skipped.sequence; Require(d9::EncodeHeader(skipped, header.data()), "header");
      const auto r = record(header.data(), next + 1); inject(c, r.data(), 144, 144); damaged(d9::ring::Fault::Sequence);
    } else if (name == "async-refused") {
      // A draw annotation without negotiated annotations, published without a
      // reply: the host refuses the record and that ends the session.
      d9::Header annotate; annotate.op = d9::Op::AnnotateDraws;
      c.ringPublish(annotate, nullptr, false);
      const auto deadline = GetTickCount64() + 10000;
      while (c.ring->hostState().state != d9::ring::State::Faulted) { Require(GetTickCount64() < deadline, "host kept running"); Sleep(1); }
      kind = Terminal::HostRejected; detail = "async-refused"; result = uint32_t(D3DERR_INVALIDCALL);
    } else if (name == "scene-refused") {
      // A scene record is advice to the host, not part of the game's picture:
      // a refused one (here a stage marker without negotiated annotations,
      // published without a reply) is reported in the host's ring block and
      // the session goes on. Every later one is refused or ignored the same
      // way, and everything else still executes.
      d9::Header marker; marker.op = d9::Op::StageMarker; marker.args[0] = 1;
      c.ringPublish(marker, nullptr, false);
      const auto deadline = GetTickCount64() + 10000;
      while (!c.ring->sceneRefused()) {
        Require(GetTickCount64() < deadline && c.ring->hostState().state != d9::ring::State::Faulted,
          "host did not report the refused scene record");
        Sleep(1);
      }
      d9::Header again; again.op = d9::Op::StageMarker; again.args[0] = 1;
      c.ringPublish(again, nullptr, false);
      for (uint32_t i = 0; i < 8; ++i) Require(c.enqueueClear(clear(0xff000000u | i)) == D3D_OK, "clear after a refused scene record");
      Require(barrier(c) == D3D_OK && c.healthy() && c.settledSequence() == c.sequence
        && c.ring->hostState().state != d9::ring::State::Faulted, "session did not continue after refused scene records");
      sceneRefused = true;
      d9::Header finish; finish.op = d9::Op::Finish; Require(c.call(finish) == D3D_OK, "finish");
    } else if (name == "scene-stopped-reset") {
      // The same, refused inside an open world scene (annotations negotiated:
      // WARVK_D3D9_ANNOTATIONS=1). The first enter is accepted, the nested one
      // is refused, and the exit that follows is ignored like every later
      // scene record. The scene the host was in must not stay open: a map
      // change's session reset is refused inside a world scene, and nothing
      // would ever close this one.
      Require(c.annotationsEnabled(), "WARVK_D3D9_ANNOTATIONS=1 required");
      const auto marker = [&](d9::Stage stage) {
        d9::Header h; h.op = d9::Op::StageMarker; h.args[0] = uint32_t(stage); c.ringPublish(h, nullptr, false);
      };
      marker(d9::Stage::WorldSceneEnter); marker(d9::Stage::WorldSceneEnter);
      const auto deadline = GetTickCount64() + 10000;
      while (!c.ring->sceneRefused()) {
        Require(GetTickCount64() < deadline && c.ring->hostState().state != d9::ring::State::Faulted,
          "host did not report the refused scene record");
        Sleep(1);
      }
      marker(d9::Stage::WorldSceneExit);
      Require(barrier(c) == D3D_OK && c.healthy(), "session did not continue after the refused scene record");
      d9::Header reset; reset.op = d9::Op::SessionReset;
      reset.args[0] = uint32_t(d9::SessionResetReason::MapTransition); reset.args[1] = 1;
      Require(c.call(reset) == D3D_OK, "session reset refused after the scene records stopped");
      sceneRefused = true;
      d9::Header finish; finish.op = d9::Op::Finish; Require(c.call(finish) == D3D_OK, "finish");
    } else if (name == "host-killed") {
      const HANDLE host = openHost(c); Require(TerminateProcess(host, 77), "terminate host"); CloseHandle(host);
      kind = Terminal::HostExited; detail = "host process exited";
    } else if (name == "host-suspended") {
      c.ringNoProgressMs = 1500;
      const HANDLE host = openHost(c); suspendOrResume(host, "NtSuspendProcess");
      const auto begin = GetTickCount64(); last = barrier(c); elapsed = GetTickCount64() - begin;
      // Let it run again: a failed session is the end of the stream for the host.
      suspendOrResume(host, "NtResumeProcess"); CloseHandle(host);
      Require(last == D3DERR_DEVICELOST && elapsed >= 1500 && elapsed < 10000, "hang was not bounded by the no-progress limit");
      kind = Terminal::HostHung; detail = "no progress";
    } else throw std::runtime_error("unknown case");
    Connection::TerminalReport report;
    if (kind != Terminal::None) {
      if (name != "host-suspended" && name != "resource-hung") {
        const auto begin = GetTickCount64(); last = barrier(c); elapsed = GetTickCount64() - begin;
        Require(last == D3DERR_DEVICELOST && elapsed < 5000, "terminal cause was not reported in time");
      }
      Require(c.terminalReport(report) && report.kind == kind && report.result == result && !std::strcmp(report.detail, detail),
        "terminal report");
      Require(hookCalls == 1 && !c.healthy() && barrier(c) == D3DERR_DEVICELOST
        && c.enqueueClear(clear(1)) == D3DERR_DEVICELOST, "session stayed closed");
    } else Require(!c.terminalReport(report) && !hookCalls && c.healthy(), "healthy session reported a terminal cause");
    std::printf("{\"schema\":\"warvk-d3d9-ring-fault/v1\",\"case\":\"%s\",\"ok\":true,\"terminal\":\"%s\",\"detail\":\"%s\","
      "\"result\":%lu,\"elapsedMs\":%llu,\"published\":%u,\"applied\":%llu,\"newViews\":%zu,\"newViewBytes\":%zu,"
      "\"smallestView\":%zu,\"largestView\":%zu,\"sceneRefused\":%s,\"bits\":%u}\n", name.c_str(), Connection::TerminalName(kind), detail,
      static_cast<unsigned long>(result), elapsed, c.sequence, c.ringApplied, newViews, newViewBytes, smallestView, largestView,
      sceneRefused ? "true" : "false", unsigned(sizeof(void*) * 8));
    std::fflush(stdout);
    return 0;
  }
};
std::atomic<int> ConnectionCpuTest::hookCalls{0};
} // namespace warvk::host::d3d9proxy

int wmain(int argc, wchar_t** argv) {
  try {
    namespace d9 = warvk::host::d9;
    if (argc != 7 || std::wcscmp(argv[1], L"--case") || std::wcscmp(argv[3], L"--expect-desktop")
        || std::wcscmp(argv[5], L"--input-desktop") || !argv[2][0] || !argv[4][0] || !argv[6][0])
      throw std::runtime_error("expected --case <name> --expect-desktop <bareName> --input-desktop <bareName>");
    const std::wstring desktop = argv[4], input = argv[6];
    if (!_wcsicmp(desktop.c_str(), L"Default") || d9::RequireIsolatedDesktop() != L"WinSta0\\" + desktop
        || d9::CurrentDesktop() != desktop || d9::InputDesktop() != input)
      throw std::runtime_error("ring fault fixture desktop mismatch/default forbidden");
    if (GetModuleHandleW(L"Game.dll") || GetModuleHandleW(L"vulkan-1.dll"))
      throw std::runtime_error("x86 game/Vulkan module prohibited");
    std::string name; for (const wchar_t* p = argv[2]; *p; ++p) name.push_back(char(*p));
    return warvk::host::d3d9proxy::ConnectionCpuTest::run(name);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ring fault fixture: %s\n", e.what());
    return 1;
  }
}
