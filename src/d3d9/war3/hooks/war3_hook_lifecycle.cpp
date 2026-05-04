#include "war3_hook_lifecycle.h"
#include "war3_hook_address_book.h"
#include "war3_hook_install_util.h"
#include "war3_hook_render.h"
#include "../../d3d9_war3_hook.h"
#include "../../d3d9_war3_debug.h"

#include "../core/war3_events.h"
#include "../core/war3_internal_test_config.h"
#include "../core/war3_net_event_hook.h"
#include "../render/war3_render_exec_batch.h"
#include "../render/war3_render_queue_tracker.h"
#include "../render/war3_renderer.h"
#include "../state/war3_render_state.h"
#include "../tools/war3_perf_monitor.h"
#include "../tools/war3_internal_test_api.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace dxvk {
extern thread_local bool s_inMainRunner;

extern uint32_t *g_numOfElementsPtr;
extern uint32_t *g_numOfTransparentPtr;
extern uint32_t *g_stateCleanupPendingPtr;
} // namespace dxvk

namespace dxvk::war3::hooks {

// ---------------------------------------------------------------------------
// 生命周期域 Hook：
// - MainRunner / MainRunner_Alt：触发运行时激活；
// - FlushAndReset：帧尾收口、可选空队列跳过、状态复位；
// - GetD3d9Parameters：覆盖 PresentInterval 以解除帧率限制。
// ---------------------------------------------------------------------------

static bool EnvFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value)
    return false;
  if (value[0] == '0')
    return false;
  if ((value[0] == 'f' || value[0] == 'F') &&
      (value[1] == 'a' || value[1] == 'A'))
    return false;
  if ((value[0] == 'n' || value[0] == 'N') &&
      (value[1] == 'o' || value[1] == 'O'))
    return false;
  return true;
}

static bool ShouldBlockGamePauseForAutoTest() {
  if constexpr (dxvk::war3::internal::kAutoTestDisableGamePause)
    return true;
  static const bool s_runtimeEnabled =
      EnvFlagEnabled("DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE");
  return s_runtimeEnabled;
}

using MainRunnerFn = int(__fastcall *)(void *, void *);
using MainRunnerAltFn = int(__fastcall *)(void *, void *);
using GetD3d9ParametersFn = DWORD(__fastcall *)(void *, void *,
                                                D3DPRESENT_PARAMETERS *);
using FlushAndResetFn = int(__cdecl *)();
using EventMainCallbackFn = int(__cdecl *)();
using EventMessagePumpFn = int(__fastcall *)(int, int);
using EventDispatchFn = void(__fastcall *)(int, int, void *, void *);
using EngineTlsPumpFn = int(__fastcall *)(int, int);
using EngineSelectWorkerFn = int(__fastcall *)(int, int);
using EngineRunCallbacksFn = int(__thiscall *)(int);
using EngineQueueFlushFn = int(__thiscall *)(int);
using EngineFinalizeTickFn = int(__thiscall *)(int);
using EngineRescheduleFn =
    int(__fastcall *)(uint32_t, uint32_t *, int, uint32_t);
using EnginePrepareWaitFn = int(__fastcall *)(int, int);
using EngineWaitGateFn = DWORD(__thiscall *)(HANDLE *, DWORD);
using EnginePrepareDispatchFn = int(__thiscall *)(int);
using EngineFinalizeDispatchFn = int(__thiscall *)(int);
using EngineTickUpdateFn = int(__thiscall *)(int);
using EngineFinalizeWorkerFn = int(__fastcall *)(int, int);
using EngineComputeWakeDeltaFn = int(__fastcall *)(int, int);
using EngineSleepGateFn = void(__thiscall *)(uint32_t *, DWORD);
using EngineSleepGateInnerFn = void(__thiscall *)(uint32_t *, DWORD);
using GamePauseFn =
    void(__fastcall *)(void *, void *, BOOL, uint32_t, uint32_t, uint32_t,
                       uint32_t);
using WindowMessageTargetLookupFn = void *(__thiscall *)(void *);
using WindowMessageOnSizeFn = void(__thiscall *)(void *);
using SleepFn = VOID(WINAPI *)(DWORD);
using SleepExFn = DWORD(WINAPI *)(DWORD, BOOL);
using WaitMessageFn = BOOL(WINAPI *)();
using MsgWaitForMultipleObjectsFn =
    DWORD(WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD, DWORD);
using MsgWaitForMultipleObjectsExFn =
    DWORD(WINAPI *)(DWORD, const HANDLE *, DWORD, DWORD, DWORD);
using WaitForSingleObjectFn = DWORD(WINAPI *)(HANDLE, DWORD);
using WaitForMultipleObjectsFn =
    DWORD(WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD);
using WaitForMultipleObjectsExFn =
    DWORD(WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD, BOOL);
using SignalObjectAndWaitFn = DWORD(WINAPI *)(HANDLE, HANDLE, DWORD, BOOL);
using WaitForSingleObjectExFn = DWORD(WINAPI *)(HANDLE, DWORD, BOOL);
using NtDelayExecutionFn = LONG(NTAPI *)(BOOLEAN, PLARGE_INTEGER);
using NtWaitForSingleObjectFn = LONG(NTAPI *)(HANDLE, BOOLEAN,
                                              PLARGE_INTEGER);
using NtWaitForMultipleObjectsFn = LONG(NTAPI *)(ULONG, HANDLE *, ULONG,
                                                 BOOLEAN, PLARGE_INTEGER);

static MainRunnerFn g_originalMainRunner = nullptr;
static MainRunnerFn g_trampolineMainRunner = nullptr;

static MainRunnerAltFn g_originalMainRunnerAlt = nullptr;
static MainRunnerAltFn g_trampolineMainRunnerAlt = nullptr;

static GetD3d9ParametersFn g_originalGetD3d9Parameters = nullptr;
static GetD3d9ParametersFn g_trampolineGetD3d9Parameters =
    nullptr; // Not hooked with MinHook traditionally in original, but keeping
             // structure

static FlushAndResetFn g_originalFlushAndReset = nullptr;
static FlushAndResetFn g_trampolineFlushAndReset = nullptr;

static EventMainCallbackFn g_originalEventMainCallback = nullptr;
static EventMainCallbackFn g_trampolineEventMainCallback = nullptr;

static EventMessagePumpFn g_originalEventMessagePump = nullptr;
static EventMessagePumpFn g_trampolineEventMessagePump = nullptr;

static EventDispatchFn g_originalEventDispatch = nullptr;
static EventDispatchFn g_trampolineEventDispatch = nullptr;

static EngineTlsPumpFn g_originalEngineTlsPump = nullptr;
static EngineTlsPumpFn g_trampolineEngineTlsPump = nullptr;

static EngineSelectWorkerFn g_originalEngineSelectWorker = nullptr;
static EngineSelectWorkerFn g_trampolineEngineSelectWorker = nullptr;

static EngineRunCallbacksFn g_originalEngineRunCallbacks = nullptr;
static EngineRunCallbacksFn g_trampolineEngineRunCallbacks = nullptr;

static EngineQueueFlushFn g_originalEngineQueueFlush = nullptr;
static EngineQueueFlushFn g_trampolineEngineQueueFlush = nullptr;

static EngineFinalizeTickFn g_originalEngineFinalizeTick = nullptr;
static EngineFinalizeTickFn g_trampolineEngineFinalizeTick = nullptr;

static EngineRescheduleFn g_originalEngineReschedule = nullptr;
static EngineRescheduleFn g_trampolineEngineReschedule = nullptr;

static EnginePrepareWaitFn g_originalEnginePrepareWait = nullptr;
static EnginePrepareWaitFn g_trampolineEnginePrepareWait = nullptr;

static EngineWaitGateFn g_originalEngineWaitGate = nullptr;
static EngineWaitGateFn g_trampolineEngineWaitGate = nullptr;

static EnginePrepareDispatchFn g_originalEnginePrepareDispatch = nullptr;
static EnginePrepareDispatchFn g_trampolineEnginePrepareDispatch = nullptr;

static EngineFinalizeDispatchFn g_originalEngineFinalizeDispatch = nullptr;
static EngineFinalizeDispatchFn g_trampolineEngineFinalizeDispatch = nullptr;

static EngineTickUpdateFn g_originalEngineTickUpdate = nullptr;
static EngineTickUpdateFn g_trampolineEngineTickUpdate = nullptr;

static EngineFinalizeWorkerFn g_originalEngineFinalizeWorker = nullptr;
static EngineFinalizeWorkerFn g_trampolineEngineFinalizeWorker = nullptr;

static EngineComputeWakeDeltaFn g_originalEngineComputeWakeDelta = nullptr;
static EngineComputeWakeDeltaFn g_trampolineEngineComputeWakeDelta = nullptr;

static EngineSleepGateFn g_originalEngineSleepGate = nullptr;
static EngineSleepGateFn g_trampolineEngineSleepGate = nullptr;

static EngineSleepGateInnerFn g_originalEngineSleepGateInner = nullptr;
static EngineSleepGateInnerFn g_trampolineEngineSleepGateInner = nullptr;

static GamePauseFn g_originalGamePause = nullptr;
static GamePauseFn g_trampolineGamePause = nullptr;
static WindowMessageTargetLookupFn g_windowMessageTargetLookup = nullptr;
static std::atomic<uintptr_t> g_windowSizeLParamStateAddr = 0u;

static SleepFn g_trampolineSleep = nullptr;
static SleepExFn g_trampolineSleepEx = nullptr;
static WaitMessageFn g_trampolineWaitMessage = nullptr;
static MsgWaitForMultipleObjectsFn g_trampolineMsgWaitForMultipleObjects =
    nullptr;
static MsgWaitForMultipleObjectsExFn g_trampolineMsgWaitForMultipleObjectsEx =
    nullptr;
static WaitForSingleObjectFn g_trampolineWaitForSingleObject = nullptr;
static WaitForMultipleObjectsFn g_trampolineWaitForMultipleObjects = nullptr;
static WaitForMultipleObjectsExFn g_trampolineWaitForMultipleObjectsEx =
    nullptr;
static SignalObjectAndWaitFn g_trampolineSignalObjectAndWait = nullptr;
static WaitForSingleObjectExFn g_trampolineWaitForSingleObjectEx = nullptr;
static NtDelayExecutionFn g_trampolineNtDelayExecution = nullptr;
static NtWaitForSingleObjectFn g_trampolineNtWaitForSingleObject = nullptr;
static NtWaitForMultipleObjectsFn g_trampolineNtWaitForMultipleObjects =
    nullptr;

struct D3d9WindowedSizeFieldPair {
  uint32_t widthOffset = 0u;
  uint32_t heightOffset = 0u;
};

static std::atomic<uintptr_t> g_d3d9ParamsOwner = 0u;
static std::array<D3d9WindowedSizeFieldPair, 8> g_d3d9WindowedSizeFields = {};
static std::atomic<uint32_t> g_d3d9WindowedSizeFieldCount = 0u;
static std::atomic<uintptr_t> g_windowedSizeFloatHeightAddr = 0u;
static std::atomic<uintptr_t> g_windowedSizeFloatWidthAddr = 0u;

using PerfClock = std::chrono::steady_clock;

struct MainLoopPerfStats {
  std::atomic<uint64_t> pumpCalls{0};
  std::atomic<uint64_t> pumpUs{0};
  std::atomic<uint64_t> dispatchCalls{0};
  std::atomic<uint64_t> dispatchUs{0};
  std::atomic<uint64_t> callbackCalls{0};
  std::atomic<uint64_t> callbackUs{0};

  std::atomic<uint64_t> sleepCalls{0};
  std::atomic<uint64_t> sleepUs{0};
  std::atomic<uint64_t> sleepExCalls{0};
  std::atomic<uint64_t> sleepExUs{0};
  std::atomic<uint64_t> waitMessageCalls{0};
  std::atomic<uint64_t> waitMessageUs{0};
  std::atomic<uint64_t> msgWaitCalls{0};
  std::atomic<uint64_t> msgWaitUs{0};
  std::atomic<uint64_t> msgWaitExCalls{0};
  std::atomic<uint64_t> msgWaitExUs{0};
  std::atomic<uint64_t> waitSingleCalls{0};
  std::atomic<uint64_t> waitSingleUs{0};
  std::atomic<uint64_t> waitMultiCalls{0};
  std::atomic<uint64_t> waitMultiUs{0};
  std::atomic<uint64_t> waitMultiExCalls{0};
  std::atomic<uint64_t> waitMultiExUs{0};
  std::atomic<uint64_t> signalWaitCalls{0};
  std::atomic<uint64_t> signalWaitUs{0};
  std::atomic<uint64_t> waitSingleExCalls{0};
  std::atomic<uint64_t> waitSingleExUs{0};
  std::atomic<uint64_t> ntDelayCalls{0};
  std::atomic<uint64_t> ntDelayUs{0};
  std::atomic<uint64_t> ntWaitSingleCalls{0};
  std::atomic<uint64_t> ntWaitSingleUs{0};
  std::atomic<uint64_t> ntWaitMultiCalls{0};
  std::atomic<uint64_t> ntWaitMultiUs{0};

  std::atomic<uint64_t> mainLoopLastLoggedPump{0};
  std::atomic<uint64_t> waitLastLoggedPump{0};
};

static bool IsReadableRange(const void *ptr, size_t size) {
  if (!ptr || size == 0)
    return false;

  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) != sizeof(mbi))
    return false;

  if (mbi.State != MEM_COMMIT)
    return false;

  const DWORD prot = mbi.Protect & 0xFFu;
  const bool readable =
      prot == PAGE_READONLY || prot == PAGE_READWRITE ||
      prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
      prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
  if (!readable)
    return false;

  const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t end = start + size;
  const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  const uintptr_t regionEnd = regionStart + mbi.RegionSize;
  return end <= regionEnd;
}

static void DiscoverWindowedSizeFields(void *thisPtr, HWND window, UINT width,
                                       UINT height) {
  if (!thisPtr || width == 0u || height == 0u)
    return;

  const uintptr_t owner = reinterpret_cast<uintptr_t>(thisPtr);
  if (g_d3d9ParamsOwner.load(std::memory_order_acquire) == owner &&
      g_d3d9WindowedSizeFieldCount.load(std::memory_order_acquire) != 0u)
    return;

  constexpr size_t kScanBytes = 0x200;
  if (!IsReadableRange(thisPtr, kScanBytes))
    return;

  const auto *words = reinterpret_cast<const uint32_t *>(thisPtr);
  const size_t wordCount = kScanBytes / sizeof(uint32_t);
  std::array<D3d9WindowedSizeFieldPair, 8> found = {};
  uint32_t foundCount = 0u;

  for (size_t i = 0; i + 1 < wordCount && foundCount < found.size(); ++i) {
    if (words[i] != width || words[i + 1] != height)
      continue;

    found[foundCount++] = {
        static_cast<uint32_t>(i * sizeof(uint32_t)),
        static_cast<uint32_t>((i + 1) * sizeof(uint32_t)),
    };
  }

  if (foundCount == 0u)
    return;

  g_d3d9WindowedSizeFields = found;
  g_d3d9WindowedSizeFieldCount.store(foundCount, std::memory_order_release);
  g_d3d9ParamsOwner.store(owner, std::memory_order_release);

  war3dbg::Print(
      "DXVK War3Hook[Lifecycle]: windowed size owner=%p hwnd=%p client=%ux%u "
      "candidates=%u first=[0x%X,0x%X]\n",
      thisPtr, window, width, height, foundCount, found[0].widthOffset,
      found[0].heightOffset);
}

static uint32_t FloatBits(float value) {
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static bool IsReadableWritableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
  if (mbi.State != MEM_COMMIT)
    return false;
  if ((mbi.Protect & PAGE_GUARD) != 0u || mbi.Protect == PAGE_NOACCESS)
    return false;
  if (mbi.Type != MEM_PRIVATE)
    return false;

  const DWORD prot = mbi.Protect & 0xFFu;
  return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
         prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static void DiscoverWindowedSizeFloatFields(UINT previousWidth,
                                            UINT previousHeight) {
  if (previousWidth == 0u || previousHeight == 0u)
    return;

  if (g_windowedSizeFloatHeightAddr.load(std::memory_order_acquire) != 0u &&
      g_windowedSizeFloatWidthAddr.load(std::memory_order_acquire) != 0u)
    return;

  const uint32_t heightBits = FloatBits(static_cast<float>(previousHeight));
  const uint32_t widthBits = FloatBits(static_cast<float>(previousWidth));

  SYSTEM_INFO sysInfo = {};
  GetSystemInfo(&sysInfo);

  uintptr_t addr =
      reinterpret_cast<uintptr_t>(sysInfo.lpMinimumApplicationAddress);
  const uintptr_t maxAddr =
      reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);

  while (addr < maxAddr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) !=
        sizeof(mbi)) {
      break;
    }

    const uintptr_t regionBase =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionBase + mbi.RegionSize;

    if (IsReadableWritableRegion(mbi) && mbi.RegionSize >= sizeof(uint32_t) * 5u) {
      const auto* words = reinterpret_cast<const uint32_t*>(regionBase);
      const size_t wordCount = mbi.RegionSize / sizeof(uint32_t);

      for (size_t i = 0; i + 4 < wordCount; ++i) {
        if (words[i] != heightBits || words[i + 1] != widthBits ||
            words[i + 2] != 0u) {
          continue;
        }

        const uint32_t refWidth = words[i + 3];
        const uint32_t refHeight = words[i + 4];
        const bool plausibleRefSize =
            refWidth >= 640u && refWidth <= 8192u && refHeight >= 480u &&
            refHeight <= 8192u;
        if (!plausibleRefSize)
          continue;

        const uintptr_t heightAddr =
            regionBase + i * sizeof(uint32_t);
        const uintptr_t widthAddr =
            regionBase + (i + 1) * sizeof(uint32_t);
        g_windowedSizeFloatHeightAddr.store(heightAddr,
                                            std::memory_order_release);
        g_windowedSizeFloatWidthAddr.store(widthAddr,
                                           std::memory_order_release);
        war3dbg::Print(
            "DXVK War3Hook[Lifecycle]: windowed float size fields "
            "height=0x%p width=0x%p ref=%ux%u prev=%ux%u\n",
            reinterpret_cast<void*>(heightAddr),
            reinterpret_cast<void*>(widthAddr), refWidth, refHeight,
            previousWidth, previousHeight);
        return;
      }
    }

    if (regionEnd <= addr)
      break;
    addr = regionEnd;
  }
}

static std::atomic<DWORD> g_mainLoopThreadId{0};
enum class MainLoopCyclePhase : uint32_t {
  Pump = 0,
  Dispatch,
  Callback,
  EngineTlsPump,
  EngineSelectWorker,
  EngineRunCallbacks,
  EngineQueueFlush,
  EngineFinalizeTick,
  EngineReschedule,
  EnginePrepareWait,
  EngineWaitGate,
  EnginePrepareDispatch,
  EngineFinalizeDispatch,
  EngineTickUpdate,
  EngineFinalizeWorker,
  EngineComputeWakeDelta,
  EngineSleepGate,
  EngineSleepGateInner,
  WaitSleep,
  WaitSleepEx,
  WaitMessage,
  WaitMsgWait,
  WaitMsgWaitEx,
  WaitSingle,
  WaitMulti,
  WaitMultiEx,
  WaitSignalWait,
  WaitSingleEx,
  WaitNtDelay,
  WaitNtWaitSingle,
  WaitNtWaitMulti,
  Count
};

static constexpr size_t kDispatchBucketCount = 16; // case0~14 + Other
static constexpr size_t kDispatchOtherBucket = 15;
static constexpr size_t kRunCallbackCallerTopK = 8;
enum class TickUpdateSubBucket : size_t {
  Dispatch = 0,
  Callback,
  RunCallbacks,
  QueueFlush,
  PrepareDispatch,
  FinalizeDispatch,
  Reschedule,
  ComputeWakeDelta,
  TlsPump,
  Count
};
static constexpr size_t kTickUpdateSubBucketCount =
    static_cast<size_t>(TickUpdateSubBucket::Count);

struct MainLoopCycleSample {
  std::array<uint64_t, static_cast<size_t>(MainLoopCyclePhase::Count)> us{};
  std::array<uint32_t, static_cast<size_t>(MainLoopCyclePhase::Count)> calls{};
  std::array<uint64_t, kDispatchBucketCount> dispatchCaseUs{};
  std::array<uint32_t, kDispatchBucketCount> dispatchCaseCalls{};
  std::array<uint64_t, kDispatchBucketCount> dispatchModuleUs{};
  std::array<uint32_t, kDispatchBucketCount> dispatchModuleCalls{};
  std::array<uintptr_t, kRunCallbackCallerTopK> runCallbackCallerPc{};
  std::array<uint64_t, kRunCallbackCallerTopK> runCallbackCallerUs{};
  std::array<uint32_t, kRunCallbackCallerTopK> runCallbackCallerCalls{};
  uint64_t runCallbackOtherUs = 0;
  uint32_t runCallbackOtherCalls = 0;
  std::array<uint64_t, kTickUpdateSubBucketCount> tickUpdateSubUs{};
  std::array<uint32_t, kTickUpdateSubBucketCount> tickUpdateSubCalls{};
  uint64_t tickUpdateSubTotalUs = 0;
  uint32_t tickUpdateSubTotalCalls = 0;
  uint64_t tickUpdateSelfUs = 0;
  uint32_t tickUpdateSelfCalls = 0;
  uint32_t pumpDepth = 0;
};

static thread_local MainLoopCycleSample t_mainLoopCycle;
static thread_local bool t_waitGateCycleReady = false;
static thread_local PerfClock::time_point t_waitGateLastEnd{};

static inline size_t CyclePhaseIndex(MainLoopCyclePhase phase) {
  return static_cast<size_t>(phase);
}

static inline double UsToMs(uint64_t us) {
  return static_cast<double>(us) / 1000.0;
}

static inline void ResetMainLoopCycleSample(MainLoopCycleSample &sample) {
  sample.us.fill(0);
  sample.calls.fill(0);
  sample.dispatchCaseUs.fill(0);
  sample.dispatchCaseCalls.fill(0);
  sample.dispatchModuleUs.fill(0);
  sample.dispatchModuleCalls.fill(0);
  sample.runCallbackCallerPc.fill(0);
  sample.runCallbackCallerUs.fill(0);
  sample.runCallbackCallerCalls.fill(0);
  sample.runCallbackOtherUs = 0;
  sample.runCallbackOtherCalls = 0;
  sample.tickUpdateSubUs.fill(0);
  sample.tickUpdateSubCalls.fill(0);
  sample.tickUpdateSubTotalUs = 0;
  sample.tickUpdateSubTotalCalls = 0;
  sample.tickUpdateSelfUs = 0;
  sample.tickUpdateSelfCalls = 0;
}

static inline size_t DispatchBucketFromMsgType(int msgType) {
  if (msgType >= 0 && msgType < static_cast<int>(kDispatchOtherBucket))
    return static_cast<size_t>(msgType);
  return kDispatchOtherBucket;
}

static inline size_t DispatchModuleBucketFromMsgType(int msgType) {
  // 当前模块分桶为“case 语义映射”，不是子函数入口独立 Hook：
  // case -> module 的映射关系可以在此处单点维护。
  switch (msgType) {
  case 0:
    return 0; // StateFinalize
  case 1:
    return 1; // LoadBlockType1_Single
  case 2:
    return 2; // LoadBlockType1_Batch
  case 3:
    return 3; // LoadBlockType27
  case 4:
    return 4; // LoadBlockType28
  case 5:
    return 5; // MainCallbackGate
  case 6:
    return 6; // LoadBlockType2_ResetSync
  case 7:
    return 7; // LoadBlockType8_InputSet
  case 8:
    return 8; // LoadBlockType9_InputClear
  case 9:
    return 9; // LoadBlockType11_InputCompose
  case 10:
    return 10; // LoadBlockType12
  case 11:
    return 11; // LoadBlockType16_Command
  case 12:
    return 12; // LoadBlockType13_TimeState
  case 13:
    return 13; // LoadBlockType14_Finalize
  case 14:
    return 14; // MainCallbackGateAlt
  default:
    return kDispatchOtherBucket;
  }
}

static inline const char *DispatchCaseBucketName(size_t idx) {
  switch (idx) {
  case 0:
    return "Case0";
  case 1:
    return "Case1";
  case 2:
    return "Case2";
  case 3:
    return "Case3";
  case 4:
    return "Case4";
  case 5:
    return "Case5";
  case 6:
    return "Case6";
  case 7:
    return "Case7";
  case 8:
    return "Case8";
  case 9:
    return "Case9";
  case 10:
    return "Case10";
  case 11:
    return "Case11";
  case 12:
    return "Case12";
  case 13:
    return "Case13";
  case 14:
    return "Case14";
  default:
    return "Other";
  }
}

static inline const char *DispatchCaseFunctionToken(size_t idx) {
  switch (idx) {
  case 0:
    return "sub_6F059D70@0x059D70";
  case 1:
    return "sub_6F059DC0@0x059DC0";
  case 2:
    return "sub_6F05A2A0@0x05A2A0";
  case 3:
    return "sub_6F059E40@0x059E40";
  case 4:
    return "sub_6F05A270@0x05A270";
  case 5:
    return "GateCheck+Commit@0x059890+0x059E00";
  case 6:
    return "sub_6F059E10@0x059E10";
  case 7:
    return "sub_6F059E90@0x059E90";
  case 8:
    return "sub_6F059F00@0x059F00";
  case 9:
    return "sub_6F059F70@0x059F70";
  case 10:
    return "sub_6F05A060@0x05A060";
  case 11:
    return "sub_6F05A1F0@0x05A1F0";
  case 12:
    return "sub_6F05A0E0@0x05A0E0";
  case 13:
    return "sub_6F05A160@0x05A160";
  case 14:
    return "GateCheck@0x059890";
  default:
    return "Unknown";
  }
}

static inline const char *DispatchModuleBucketName(size_t idx) {
  switch (idx) {
  case 0:
    return "StateFinalize";
  case 1:
    return "LoadBlockType1_Single";
  case 2:
    return "LoadBlockType1_Batch";
  case 3:
    return "LoadBlockType27";
  case 4:
    return "LoadBlockType28";
  case 5:
    return "MainCallbackGate";
  case 6:
    return "LoadBlockType2_ResetSync";
  case 7:
    return "LoadBlockType8_InputSet";
  case 8:
    return "LoadBlockType9_InputClear";
  case 9:
    return "LoadBlockType11_InputCompose";
  case 10:
    return "LoadBlockType12";
  case 11:
    return "LoadBlockType16_Command";
  case 12:
    return "LoadBlockType13_TimeState";
  case 13:
    return "LoadBlockType14_Finalize";
  case 14:
    return "MainCallbackGateAlt";
  default:
    return "Other";
  }
}

static inline void RecordDispatchBuckets(int msgType, uint64_t deltaUs) {
  if (deltaUs == 0 || t_mainLoopCycle.pumpDepth == 0)
    return;
  const size_t caseBucket = DispatchBucketFromMsgType(msgType);
  const size_t moduleBucket = DispatchModuleBucketFromMsgType(msgType);
  t_mainLoopCycle.dispatchCaseUs[caseBucket] += deltaUs;
  t_mainLoopCycle.dispatchCaseCalls[caseBucket] += 1;
  t_mainLoopCycle.dispatchModuleUs[moduleBucket] += deltaUs;
  t_mainLoopCycle.dispatchModuleCalls[moduleBucket] += 1;
}

static inline void RecordRunCallbacksCaller(uintptr_t callerPc,
                                            uint64_t deltaUs) {
  if (deltaUs == 0 || t_mainLoopCycle.pumpDepth == 0 || callerPc == 0)
    return;

  size_t minIdx = 0;
  uint64_t minUs = std::numeric_limits<uint64_t>::max();
  for (size_t i = 0; i < kRunCallbackCallerTopK; i++) {
    if (t_mainLoopCycle.runCallbackCallerPc[i] == callerPc) {
      t_mainLoopCycle.runCallbackCallerUs[i] += deltaUs;
      t_mainLoopCycle.runCallbackCallerCalls[i] += 1;
      return;
    }

    if (t_mainLoopCycle.runCallbackCallerPc[i] == 0) {
      t_mainLoopCycle.runCallbackCallerPc[i] = callerPc;
      t_mainLoopCycle.runCallbackCallerUs[i] = deltaUs;
      t_mainLoopCycle.runCallbackCallerCalls[i] = 1;
      return;
    }

    if (t_mainLoopCycle.runCallbackCallerUs[i] < minUs) {
      minUs = t_mainLoopCycle.runCallbackCallerUs[i];
      minIdx = i;
    }
  }

  // 热路径策略：仅当新来源贡献显著时替换最小槽，否则归并到 Other。
  if (deltaUs > (minUs >> 1)) {
    t_mainLoopCycle.runCallbackOtherUs +=
        t_mainLoopCycle.runCallbackCallerUs[minIdx];
    t_mainLoopCycle.runCallbackOtherCalls +=
        t_mainLoopCycle.runCallbackCallerCalls[minIdx];
    t_mainLoopCycle.runCallbackCallerPc[minIdx] = callerPc;
    t_mainLoopCycle.runCallbackCallerUs[minIdx] = deltaUs;
    t_mainLoopCycle.runCallbackCallerCalls[minIdx] = 1;
  } else {
    t_mainLoopCycle.runCallbackOtherUs += deltaUs;
    t_mainLoopCycle.runCallbackOtherCalls += 1;
  }
}

static inline const char *TickUpdateSubBucketName(size_t idx) {
  switch (idx) {
  case static_cast<size_t>(TickUpdateSubBucket::Dispatch):
    return "Dispatch";
  case static_cast<size_t>(TickUpdateSubBucket::Callback):
    return "Callback";
  case static_cast<size_t>(TickUpdateSubBucket::RunCallbacks):
    return "RunCallbacks";
  case static_cast<size_t>(TickUpdateSubBucket::QueueFlush):
    return "QueueFlush";
  case static_cast<size_t>(TickUpdateSubBucket::PrepareDispatch):
    return "PrepareDispatch";
  case static_cast<size_t>(TickUpdateSubBucket::FinalizeDispatch):
    return "FinalizeDispatch";
  case static_cast<size_t>(TickUpdateSubBucket::Reschedule):
    return "Reschedule";
  case static_cast<size_t>(TickUpdateSubBucket::ComputeWakeDelta):
    return "ComputeWakeDelta";
  case static_cast<size_t>(TickUpdateSubBucket::TlsPump):
    return "TlsPump";
  default:
    return "Other";
  }
}

static inline void AddMainLoopCyclePhaseSample(MainLoopCyclePhase phase,
                                               uint64_t deltaUs) {
  if (deltaUs == 0)
    return;
  if (t_mainLoopCycle.pumpDepth == 0)
    return;

  // 只统计首个主循环线程，避免多线程消息泵导致 Cycle 重复累计。
  const DWORD tid = ::GetCurrentThreadId();
  DWORD tracked = g_mainLoopThreadId.load(std::memory_order_relaxed);
  if (tracked == 0) {
    DWORD expected = 0;
    g_mainLoopThreadId.compare_exchange_strong(expected, tid,
                                               std::memory_order_relaxed);
    tracked = g_mainLoopThreadId.load(std::memory_order_relaxed);
  }
  if (tracked != tid)
    return;

  const size_t idx = CyclePhaseIndex(phase);
  t_mainLoopCycle.us[idx] += deltaUs;
  t_mainLoopCycle.calls[idx] += 1;
}

static inline uint64_t SumCycleUs(
    std::initializer_list<MainLoopCyclePhase> phases) {
  uint64_t total = 0;
  for (const auto phase : phases)
    total += t_mainLoopCycle.us[CyclePhaseIndex(phase)];
  return total;
}

static inline uint32_t SumCycleCalls(
    std::initializer_list<MainLoopCyclePhase> phases) {
  uint64_t total = 0;
  for (const auto phase : phases)
    total += t_mainLoopCycle.calls[CyclePhaseIndex(phase)];
  if (total == 0)
    return 0;
  return static_cast<uint32_t>(
      std::min<uint64_t>(total, std::numeric_limits<uint32_t>::max()));
}

static inline void SubmitMainLoopCycleNode(war3::War3PerfMonitor &perf,
                                           MainLoopCyclePhase phase,
                                           const char *name,
                                           const char *parentPath) {
  const size_t idx = CyclePhaseIndex(phase);
  const uint64_t sumUs = t_mainLoopCycle.us[idx];
  const uint32_t sumCalls = t_mainLoopCycle.calls[idx];
  if (sumUs == 0 || sumCalls == 0)
    return;
  perf.addCpuSample(name, UsToMs(sumUs), parentPath, sumCalls);
}

static void FlushMainLoopCycleToPerf() {
  if (t_mainLoopCycle.pumpDepth != 0)
    return;

  const uint64_t pumpUs =
      t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Pump)];
  if (pumpUs == 0) {
    ResetMainLoopCycleSample(t_mainLoopCycle);
    return;
  }

  constexpr const char *kPumpPath = "War3MainLoop/Pump";
  constexpr const char *kCyclePath = "War3MainLoop/Pump/Cycle";
  constexpr const char *kCycleEnginePath = "War3MainLoop/Pump/Cycle/Engine";
  constexpr const char *kCycleWaitPath = "War3MainLoop/Pump/Cycle/WaitApi";

  war3::War3PerfMonitor &perf = war3::War3PerfMonitor::instance();
  perf.addCpuSample("Cycle", UsToMs(pumpUs), kPumpPath, 1);

  const uint64_t dispatchUs =
      t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Dispatch)];
  const uint64_t callbackUs =
      t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Callback)];
  const uint64_t runCallbacksUs =
      t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineRunCallbacks)];
  const uint64_t prepareDispatchUs = t_mainLoopCycle.us[CyclePhaseIndex(
      MainLoopCyclePhase::EnginePrepareDispatch)];
  const uint64_t tickUpdateUs =
      t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineTickUpdate)];
  const uint32_t dispatchCalls =
      std::max<uint32_t>(
          1, t_mainLoopCycle.calls[CyclePhaseIndex(MainLoopCyclePhase::Dispatch)]);
  const uint32_t callbackCalls =
      std::max<uint32_t>(
          1, t_mainLoopCycle.calls[CyclePhaseIndex(MainLoopCyclePhase::Callback)]);
  const uint32_t runCallbacksCalls = t_mainLoopCycle.calls[CyclePhaseIndex(
      MainLoopCyclePhase::EngineRunCallbacks)];
  const uint32_t prepareDispatchCalls = t_mainLoopCycle.calls[CyclePhaseIndex(
      MainLoopCyclePhase::EnginePrepareDispatch)];
  const uint32_t tickUpdateCalls = t_mainLoopCycle.calls[CyclePhaseIndex(
      MainLoopCyclePhase::EngineTickUpdate)];

  const uint64_t engineUs = SumCycleUs({
      MainLoopCyclePhase::EngineTlsPump,
      MainLoopCyclePhase::EngineSelectWorker,
      MainLoopCyclePhase::EngineRunCallbacks,
      MainLoopCyclePhase::EngineQueueFlush,
      MainLoopCyclePhase::EngineFinalizeTick,
      MainLoopCyclePhase::EngineReschedule,
      MainLoopCyclePhase::EnginePrepareWait,
      MainLoopCyclePhase::EngineWaitGate,
      MainLoopCyclePhase::EnginePrepareDispatch,
      MainLoopCyclePhase::EngineFinalizeDispatch,
      MainLoopCyclePhase::EngineTickUpdate,
      MainLoopCyclePhase::EngineFinalizeWorker,
      MainLoopCyclePhase::EngineComputeWakeDelta,
      MainLoopCyclePhase::EngineSleepGate,
      MainLoopCyclePhase::EngineSleepGateInner,
  });
  const uint32_t engineCalls = SumCycleCalls({
      MainLoopCyclePhase::EngineTlsPump,
      MainLoopCyclePhase::EngineSelectWorker,
      MainLoopCyclePhase::EngineRunCallbacks,
      MainLoopCyclePhase::EngineQueueFlush,
      MainLoopCyclePhase::EngineFinalizeTick,
      MainLoopCyclePhase::EngineReschedule,
      MainLoopCyclePhase::EnginePrepareWait,
      MainLoopCyclePhase::EngineWaitGate,
      MainLoopCyclePhase::EnginePrepareDispatch,
      MainLoopCyclePhase::EngineFinalizeDispatch,
      MainLoopCyclePhase::EngineTickUpdate,
      MainLoopCyclePhase::EngineFinalizeWorker,
      MainLoopCyclePhase::EngineComputeWakeDelta,
      MainLoopCyclePhase::EngineSleepGate,
      MainLoopCyclePhase::EngineSleepGateInner,
  });

  const uint64_t waitApiUs = SumCycleUs({
      MainLoopCyclePhase::WaitSleep,
      MainLoopCyclePhase::WaitSleepEx,
      MainLoopCyclePhase::WaitMessage,
      MainLoopCyclePhase::WaitMsgWait,
      MainLoopCyclePhase::WaitMsgWaitEx,
      MainLoopCyclePhase::WaitSingle,
      MainLoopCyclePhase::WaitMulti,
      MainLoopCyclePhase::WaitMultiEx,
      MainLoopCyclePhase::WaitSignalWait,
      MainLoopCyclePhase::WaitSingleEx,
      MainLoopCyclePhase::WaitNtDelay,
      MainLoopCyclePhase::WaitNtWaitSingle,
      MainLoopCyclePhase::WaitNtWaitMulti,
  });
  const uint32_t waitApiCalls = SumCycleCalls({
      MainLoopCyclePhase::WaitSleep,
      MainLoopCyclePhase::WaitSleepEx,
      MainLoopCyclePhase::WaitMessage,
      MainLoopCyclePhase::WaitMsgWait,
      MainLoopCyclePhase::WaitMsgWaitEx,
      MainLoopCyclePhase::WaitSingle,
      MainLoopCyclePhase::WaitMulti,
      MainLoopCyclePhase::WaitMultiEx,
      MainLoopCyclePhase::WaitSignalWait,
      MainLoopCyclePhase::WaitSingleEx,
      MainLoopCyclePhase::WaitNtDelay,
      MainLoopCyclePhase::WaitNtWaitSingle,
      MainLoopCyclePhase::WaitNtWaitMulti,
  });

  const uint64_t gateIdleUs = SumCycleUs({
      MainLoopCyclePhase::EngineWaitGate,
      MainLoopCyclePhase::EngineSleepGate,
      MainLoopCyclePhase::EngineSleepGateInner,
  });
  const uint64_t idleUs = gateIdleUs + waitApiUs;
  const uint64_t activeUs = pumpUs > idleUs ? (pumpUs - idleUs) : 0;

  perf.addCpuSample("Active", UsToMs(activeUs), kCyclePath, 1);
  perf.addCpuSample("Idle", UsToMs(idleUs), kCyclePath, 1);

  if (dispatchUs > 0)
    perf.addCpuSample("Dispatch", UsToMs(dispatchUs), kCyclePath, dispatchCalls);
  if (callbackUs > 0)
    perf.addCpuSample("Callback", UsToMs(callbackUs), kCyclePath, callbackCalls);
  if (engineUs > 0 && engineCalls > 0)
    perf.addCpuSample("EngineTotal", UsToMs(engineUs), kCyclePath, engineCalls);
  if (waitApiUs > 0 && waitApiCalls > 0)
    perf.addCpuSample("WaitApiTotal", UsToMs(waitApiUs), kCyclePath, waitApiCalls);

  if (runCallbacksUs > 0 && runCallbacksCalls > 0) {
    perf.addCpuSample("War3MainLoop/Engine/RunCallbacks", UsToMs(runCallbacksUs),
                      nullptr, runCallbacksCalls);
  }
  if (prepareDispatchUs > 0 && prepareDispatchCalls > 0) {
    perf.addCpuSample("War3MainLoop/Engine/PrepareDispatch",
                      UsToMs(prepareDispatchUs), nullptr, prepareDispatchCalls);
  }
  if (tickUpdateUs > 0 && tickUpdateCalls > 0) {
    perf.addCpuSample("War3MainLoop/Engine/TickUpdate", UsToMs(tickUpdateUs),
                      nullptr, tickUpdateCalls);
  }

  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineTlsPump, "TlsPump",
                          kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSelectWorker,
                          "SelectWorker", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineRunCallbacks,
                          "RunCallbacks", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineQueueFlush,
                          "QueueFlush", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineFinalizeTick,
                          "FinalizeTick", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineReschedule,
                          "Reschedule", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EnginePrepareWait,
                          "PrepareWait", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineWaitGate, "WaitGate",
                          kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EnginePrepareDispatch,
                          "PrepareDispatch", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineFinalizeDispatch,
                          "FinalizeDispatch", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineTickUpdate,
                          "TickUpdate", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineFinalizeWorker,
                          "FinalizeWorker", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineComputeWakeDelta,
                          "ComputeWakeDelta", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSleepGate,
                          "SleepGate", kCycleEnginePath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSleepGateInner,
                          "SleepGateInner", kCycleEnginePath);

  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitSleep, "Sleep",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitSleepEx, "SleepEx",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitMessage, "WaitMessage",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitMsgWait, "MsgWait",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitMsgWaitEx, "MsgWaitEx",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitSingle, "WaitSingle",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitMulti, "WaitMulti",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitMultiEx, "WaitMultiEx",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitSignalWait,
                          "SignalObjectAndWait", kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitSingleEx,
                          "WaitSingleEx", kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitNtDelay, "NtDelay",
                          kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitNtWaitSingle,
                          "NtWaitSingle", kCycleWaitPath);
  SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::WaitNtWaitMulti,
                          "NtWaitMulti", kCycleWaitPath);

  if constexpr (dxvk::war3::internal::kNativeMainLoopDispatchBreakdownEnabled) {
    // Dispatch 聚合快路径：
    // 将高频 case/module 统计改为“每循环一次上报”，减少每次 Dispatch 的
    // PerfMonitor 锁竞争与路径构建开销。
    if (dispatchUs > 0 && dispatchCalls > 0) {
      perf.addCpuSample("War3MainLoop/Dispatch", UsToMs(dispatchUs), nullptr,
                        dispatchCalls);
      perf.addCpuSample("War3MainLoop/DispatchModule", UsToMs(dispatchUs),
                        nullptr, dispatchCalls);
    }
    for (size_t i = 0; i < kDispatchBucketCount; i++) {
      const uint64_t caseUs = t_mainLoopCycle.dispatchCaseUs[i];
      const uint32_t caseCalls = t_mainLoopCycle.dispatchCaseCalls[i];
      if (caseUs > 0 && caseCalls > 0) {
        perf.addCpuSample(DispatchCaseBucketName(i), UsToMs(caseUs),
                          "War3MainLoop/Dispatch", caseCalls);
        // 函数级归因视图：
        // Dispatch case 当前仍以 case->callee 语义映射为主，用于与真实子函数 Hook
        // 对齐前的稳定观测。
        perf.addCpuSample(DispatchCaseFunctionToken(i), UsToMs(caseUs),
                          "War3MainLoop/Function/DispatchCaseFunctions",
                          caseCalls);
      }

      const uint64_t moduleUs = t_mainLoopCycle.dispatchModuleUs[i];
      const uint32_t moduleCalls = t_mainLoopCycle.dispatchModuleCalls[i];
      if (moduleUs > 0 && moduleCalls > 0) {
        perf.addCpuSample(DispatchModuleBucketName(i), UsToMs(moduleUs),
                          "War3MainLoop/DispatchModule", moduleCalls);
      }
    }
  }

  if constexpr (dxvk::war3::internal::kNativeMainLoopFunctionBreakdownEnabled) {
    // MainLoop 函数级拆解（主线程）：统一挂在 War3MainLoop/Function 根节点。
    // 注意：这里按“已 Hook 的真实函数入口”与“case 映射函数入口”两类输出。
    perf.addCpuSample("sub_6F059B00@0x059B00", UsToMs(pumpUs),
                      "War3MainLoop/Function", 1);
    if (dispatchUs > 0 && dispatchCalls > 0) {
      perf.addCpuSample("sub_6F05A310@0x05A310", UsToMs(dispatchUs),
                        "War3MainLoop/Function", dispatchCalls);
    }
    if (runCallbacksUs > 0 && runCallbacksCalls > 0) {
      perf.addCpuSample("sub_6F0603B0@0x0603B0", UsToMs(runCallbacksUs),
                        "War3MainLoop/Function", runCallbacksCalls);
    }
    if (prepareDispatchUs > 0 && prepareDispatchCalls > 0) {
      perf.addCpuSample("sub_6F05FCA0@0x05FCA0", UsToMs(prepareDispatchUs),
                        "War3MainLoop/Function", prepareDispatchCalls);
    }
    if (tickUpdateUs > 0 && tickUpdateCalls > 0) {
      perf.addCpuSample("sub_6F05FC10@0x05FC10", UsToMs(tickUpdateUs),
                        "War3MainLoop/Function", tickUpdateCalls);
    }
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineFinalizeDispatch,
                            "sub_6F05FD10@0x05FD10", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineQueueFlush,
                            "sub_6F05B080@0x05B080", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineFinalizeWorker,
                            "sub_6F05DCE0@0x05DCE0", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineReschedule,
                            "sub_6F05EE90@0x05EE90", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineComputeWakeDelta,
                            "sub_6F060500@0x060500", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EnginePrepareWait,
                            "sub_6F05DEE0@0x05DEE0", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSelectWorker,
                            "sub_6F05DE80@0x05DE80", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineWaitGate,
                            "sub_6F158940@0x158940", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSleepGate,
                            "sub_6F1648A0@0x1648A0", "War3MainLoop/Function");
    SubmitMainLoopCycleNode(perf, MainLoopCyclePhase::EngineSleepGateInner,
                            "sub_6F164B00@0x164B00", "War3MainLoop/Function");
  }

  if constexpr (
      dxvk::war3::internal::kNativeMainLoopRunCallbacksCallerBreakdownEnabled) {
    // RunCallbacks TopN 来源分桶（基于 caller return address）：
    // 用于定位“谁在驱动回调执行”而不侵入回调内部逻辑。
    for (size_t i = 0; i < kRunCallbackCallerTopK; i++) {
      const uintptr_t pc = t_mainLoopCycle.runCallbackCallerPc[i];
      const uint64_t us = t_mainLoopCycle.runCallbackCallerUs[i];
      const uint32_t calls = t_mainLoopCycle.runCallbackCallerCalls[i];
      if (pc == 0 || us == 0 || calls == 0)
        continue;

      char callerName[32];
      std::snprintf(callerName, sizeof(callerName), "Caller_%08X",
                    static_cast<unsigned>(pc & 0xFFFFFFFFu));
      perf.addCpuSample(callerName, UsToMs(us),
                        "War3MainLoop/Engine/RunCallbacks", calls);
    }
    if (t_mainLoopCycle.runCallbackOtherUs > 0 &&
        t_mainLoopCycle.runCallbackOtherCalls > 0) {
      perf.addCpuSample("Caller_Other", UsToMs(t_mainLoopCycle.runCallbackOtherUs),
                        "War3MainLoop/Engine/RunCallbacks",
                        t_mainLoopCycle.runCallbackOtherCalls);
    }
  }

  if constexpr (dxvk::war3::internal::kNativeMainLoopTickUpdateSubBreakdownEnabled) {
    if (t_mainLoopCycle.tickUpdateSubTotalUs > 0 &&
        t_mainLoopCycle.tickUpdateSubTotalCalls > 0) {
      perf.addCpuSample("Sub", UsToMs(t_mainLoopCycle.tickUpdateSubTotalUs),
                        "War3MainLoop/Engine/TickUpdate",
                        t_mainLoopCycle.tickUpdateSubTotalCalls);
    }
    if (t_mainLoopCycle.tickUpdateSelfUs > 0 &&
        t_mainLoopCycle.tickUpdateSelfCalls > 0) {
      perf.addCpuSample("Self", UsToMs(t_mainLoopCycle.tickUpdateSelfUs),
                        "War3MainLoop/Engine/TickUpdate",
                        t_mainLoopCycle.tickUpdateSelfCalls);
    }
    for (size_t i = 0; i < kTickUpdateSubBucketCount; i++) {
      const uint64_t subUs = t_mainLoopCycle.tickUpdateSubUs[i];
      const uint32_t subCalls = t_mainLoopCycle.tickUpdateSubCalls[i];
      if (subUs == 0 || subCalls == 0)
        continue;
      perf.addCpuSample(TickUpdateSubBucketName(i), UsToMs(subUs),
                        "War3MainLoop/Engine/TickUpdate/Sub", subCalls);
    }
  }

  ResetMainLoopCycleSample(t_mainLoopCycle);
}

static MainLoopPerfStats &GetMainLoopPerfStats() {
  static MainLoopPerfStats s_stats;
  return s_stats;
}

static inline void MarkMainLoopThread() {
  // 热路径优化：
  // 主循环线程一旦绑定后无需重复 CAS；先做一次 relaxed 读，只有未绑定时才尝试写入。
  if (g_mainLoopThreadId.load(std::memory_order_relaxed) != 0)
    return;

  const DWORD tid = ::GetCurrentThreadId();
  DWORD expected = 0;
  g_mainLoopThreadId.compare_exchange_strong(expected, tid,
                                             std::memory_order_relaxed);
}

static inline bool IsMainLoopThread() {
  const DWORD tracked = g_mainLoopThreadId.load(std::memory_order_relaxed);
  return tracked != 0 && tracked == ::GetCurrentThreadId();
}

static inline bool ShouldCollectMainLoopPerfSamples() {
  // 性能优先：仅在 PerfMonitor 处于“启用+录制”时进入高成本计时路径。
  // 避免日常游玩/压测时被大量诊断 Hook 放大开销。
  auto &perf = war3::War3PerfMonitor::instance();
  return perf.isEnabled() && perf.isRecording();
}

static inline uint64_t DurationUs(PerfClock::time_point begin,
                                  PerfClock::time_point end) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count());
}

static inline void AddPerfSample(std::atomic<uint64_t> &calls,
                                 std::atomic<uint64_t> &sumUs,
                                 uint64_t deltaUs) {
  calls.fetch_add(1, std::memory_order_relaxed);
  sumUs.fetch_add(deltaUs, std::memory_order_relaxed);
}

static FARPROC ResolveWinApiProc(const char *modulePrimary,
                                 const char *moduleFallback,
                                 const char *procName) {
  if (!procName || !procName[0])
    return nullptr;

  FARPROC proc = nullptr;
  HMODULE mod = modulePrimary ? ::GetModuleHandleA(modulePrimary) : nullptr;
  if (mod)
    proc = ::GetProcAddress(mod, procName);
  if (!proc && moduleFallback && moduleFallback[0]) {
    HMODULE modFallback = ::GetModuleHandleA(moduleFallback);
    if (modFallback)
      proc = ::GetProcAddress(modFallback, procName);
  }
  return proc;
}

static void MaybeLogMainLoopPerfStats() {
  if constexpr (!dxvk::war3::internal::kNativeMainLoopStatsLogging)
    return;

  constexpr uint64_t kInterval =
      dxvk::war3::internal::kNativeMainLoopStatsIntervalPumpCalls;
  if (kInterval == 0)
    return;

  auto &stats = GetMainLoopPerfStats();
  const uint64_t pumpCalls = stats.pumpCalls.load(std::memory_order_relaxed);
  if (pumpCalls == 0 || (pumpCalls % kInterval) != 0)
    return;

  const uint64_t last = stats.mainLoopLastLoggedPump.exchange(
      pumpCalls, std::memory_order_relaxed);
  if (last == pumpCalls)
    return;

  const uint64_t dispatchCalls =
      stats.dispatchCalls.load(std::memory_order_relaxed);
  const uint64_t callbackCalls =
      stats.callbackCalls.load(std::memory_order_relaxed);
  const uint64_t pumpUs = stats.pumpUs.load(std::memory_order_relaxed);
  const uint64_t dispatchUs =
      stats.dispatchUs.load(std::memory_order_relaxed);
  const uint64_t callbackUs =
      stats.callbackUs.load(std::memory_order_relaxed);

  const double pumpAvgMs =
      pumpCalls ? (static_cast<double>(pumpUs) / static_cast<double>(pumpCalls) /
                   1000.0)
                : 0.0;
  const double dispatchAvgMs =
      dispatchCalls
          ? (static_cast<double>(dispatchUs) /
             static_cast<double>(dispatchCalls) / 1000.0)
          : 0.0;
  const double callbackAvgMs =
      callbackCalls
          ? (static_cast<double>(callbackUs) /
             static_cast<double>(callbackCalls) / 1000.0)
          : 0.0;
  const double dispatchPerPump =
      pumpCalls ? (static_cast<double>(dispatchCalls) /
                   static_cast<double>(pumpCalls))
                : 0.0;
  const double callbackPerPump =
      pumpCalls ? (static_cast<double>(callbackCalls) /
                   static_cast<double>(pumpCalls))
                : 0.0;

  war3dbg::Print(
      "DXVK War3MainLoopStats: pump=%llu avg=%.3fms dispatch=%llu avg=%.3fms perPump=%.2f callback=%llu avg=%.3fms perPump=%.2f\n",
      static_cast<unsigned long long>(pumpCalls), pumpAvgMs,
      static_cast<unsigned long long>(dispatchCalls), dispatchAvgMs,
      dispatchPerPump, static_cast<unsigned long long>(callbackCalls),
      callbackAvgMs, callbackPerPump);
}

static void MaybeLogMainLoopWaitStats() {
  if constexpr (!dxvk::war3::internal::kNativeMainThreadWaitStatsLogging)
    return;

  constexpr uint64_t kInterval =
      dxvk::war3::internal::kNativeMainThreadWaitStatsIntervalPumpCalls;
  if (kInterval == 0)
    return;

  auto &stats = GetMainLoopPerfStats();
  const uint64_t pumpCalls = stats.pumpCalls.load(std::memory_order_relaxed);
  if (pumpCalls == 0 || (pumpCalls % kInterval) != 0)
    return;

  const uint64_t last = stats.waitLastLoggedPump.exchange(
      pumpCalls, std::memory_order_relaxed);
  if (last == pumpCalls)
    return;

  const uint64_t sleepCalls = stats.sleepCalls.load(std::memory_order_relaxed);
  const uint64_t sleepExCalls =
      stats.sleepExCalls.load(std::memory_order_relaxed);
  const uint64_t waitMessageCalls =
      stats.waitMessageCalls.load(std::memory_order_relaxed);
  const uint64_t msgWaitCalls =
      stats.msgWaitCalls.load(std::memory_order_relaxed);
  const uint64_t msgWaitExCalls =
      stats.msgWaitExCalls.load(std::memory_order_relaxed);
  const uint64_t waitSingleCalls =
      stats.waitSingleCalls.load(std::memory_order_relaxed);
  const uint64_t waitMultiCalls =
      stats.waitMultiCalls.load(std::memory_order_relaxed);
  const uint64_t waitMultiExCalls =
      stats.waitMultiExCalls.load(std::memory_order_relaxed);
  const uint64_t signalWaitCalls =
      stats.signalWaitCalls.load(std::memory_order_relaxed);
  const uint64_t waitSingleExCalls =
      stats.waitSingleExCalls.load(std::memory_order_relaxed);
  const uint64_t ntDelayCalls =
      stats.ntDelayCalls.load(std::memory_order_relaxed);
  const uint64_t ntWaitSingleCalls =
      stats.ntWaitSingleCalls.load(std::memory_order_relaxed);
  const uint64_t ntWaitMultiCalls =
      stats.ntWaitMultiCalls.load(std::memory_order_relaxed);

  const uint64_t sleepUs = stats.sleepUs.load(std::memory_order_relaxed);
  const uint64_t sleepExUs = stats.sleepExUs.load(std::memory_order_relaxed);
  const uint64_t waitMessageUs =
      stats.waitMessageUs.load(std::memory_order_relaxed);
  const uint64_t msgWaitUs = stats.msgWaitUs.load(std::memory_order_relaxed);
  const uint64_t msgWaitExUs =
      stats.msgWaitExUs.load(std::memory_order_relaxed);
  const uint64_t waitSingleUs =
      stats.waitSingleUs.load(std::memory_order_relaxed);
  const uint64_t waitMultiUs =
      stats.waitMultiUs.load(std::memory_order_relaxed);
  const uint64_t waitMultiExUs =
      stats.waitMultiExUs.load(std::memory_order_relaxed);
  const uint64_t signalWaitUs =
      stats.signalWaitUs.load(std::memory_order_relaxed);
  const uint64_t waitSingleExUs =
      stats.waitSingleExUs.load(std::memory_order_relaxed);
  const uint64_t ntDelayUs = stats.ntDelayUs.load(std::memory_order_relaxed);
  const uint64_t ntWaitSingleUs =
      stats.ntWaitSingleUs.load(std::memory_order_relaxed);
  const uint64_t ntWaitMultiUs =
      stats.ntWaitMultiUs.load(std::memory_order_relaxed);

  const uint64_t totalWaitUs = sleepUs + sleepExUs + waitMessageUs + msgWaitUs +
                               msgWaitExUs + waitSingleUs + waitMultiUs +
                               waitMultiExUs + signalWaitUs + waitSingleExUs +
                               ntDelayUs + ntWaitSingleUs + ntWaitMultiUs;
  const uint64_t totalWaitCalls = sleepCalls + sleepExCalls + waitMessageCalls +
                                  msgWaitCalls + msgWaitExCalls +
                                  waitSingleCalls + waitMultiCalls +
                                  waitMultiExCalls + signalWaitCalls +
                                  waitSingleExCalls + ntDelayCalls +
                                  ntWaitSingleCalls + ntWaitMultiCalls;

  const double avgWaitPerPumpMs =
      pumpCalls
          ? (static_cast<double>(totalWaitUs) / static_cast<double>(pumpCalls) /
             1000.0)
          : 0.0;
  const double avgWaitPerCallMs =
      totalWaitCalls
          ? (static_cast<double>(totalWaitUs) /
             static_cast<double>(totalWaitCalls) / 1000.0)
          : 0.0;

  war3dbg::Print(
      "DXVK War3MainLoopWaitStats: pump=%llu waitCalls=%llu waitPerPump=%.3fms waitPerCall=%.3fms "
      "Sleep=%llu SleepEx=%llu WaitMessage=%llu MsgWait=%llu MsgWaitEx=%llu "
      "WaitSingle=%llu WaitMulti=%llu WaitMultiEx=%llu SignalWait=%llu "
      "WaitSingleEx=%llu NtDelay=%llu NtWaitSingle=%llu NtWaitMulti=%llu\n",
      static_cast<unsigned long long>(pumpCalls),
      static_cast<unsigned long long>(totalWaitCalls), avgWaitPerPumpMs,
      avgWaitPerCallMs, static_cast<unsigned long long>(sleepCalls),
      static_cast<unsigned long long>(sleepExCalls),
      static_cast<unsigned long long>(waitMessageCalls),
      static_cast<unsigned long long>(msgWaitCalls),
      static_cast<unsigned long long>(msgWaitExCalls),
      static_cast<unsigned long long>(waitSingleCalls),
      static_cast<unsigned long long>(waitMultiCalls),
      static_cast<unsigned long long>(waitMultiExCalls),
      static_cast<unsigned long long>(signalWaitCalls),
      static_cast<unsigned long long>(waitSingleExCalls),
      static_cast<unsigned long long>(ntDelayCalls),
      static_cast<unsigned long long>(ntWaitSingleCalls),
      static_cast<unsigned long long>(ntWaitMultiCalls));
}

static VOID WINAPI Hook_Sleep(DWORD dwMilliseconds) {
  if (!g_trampolineSleep)
    return;

  if (!ShouldCollectMainLoopPerfSamples()) {
    g_trampolineSleep(dwMilliseconds);
    return;
  }

  if (!IsMainLoopThread()) {
    g_trampolineSleep(dwMilliseconds);
    return;
  }

  const auto begin = PerfClock::now();
  g_trampolineSleep(dwMilliseconds);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.sleepCalls, stats.sleepUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitSleep, deltaUs);
}

static DWORD WINAPI Hook_SleepEx(DWORD dwMilliseconds, BOOL bAlertable) {
  if (!g_trampolineSleepEx)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineSleepEx(dwMilliseconds, bAlertable);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineSleepEx(dwMilliseconds, bAlertable);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineSleepEx(dwMilliseconds, bAlertable);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.sleepExCalls, stats.sleepExUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitSleepEx, deltaUs);
  return result;
}

static BOOL WINAPI Hook_WaitMessage() {
  if (!g_trampolineWaitMessage)
    return FALSE;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineWaitMessage();
  }

  if (!IsMainLoopThread()) {
    return g_trampolineWaitMessage();
  }

  const auto begin = PerfClock::now();
  const BOOL result = g_trampolineWaitMessage();
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.waitMessageCalls, stats.waitMessageUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitMessage, deltaUs);
  return result;
}

static DWORD WINAPI
Hook_MsgWaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles,
                               BOOL fWaitAll, DWORD dwMilliseconds,
                               DWORD dwWakeMask) {
  if (!g_trampolineMsgWaitForMultipleObjects)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineMsgWaitForMultipleObjects(nCount, pHandles, fWaitAll,
                                                 dwMilliseconds, dwWakeMask);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineMsgWaitForMultipleObjects(nCount, pHandles, fWaitAll,
                                                 dwMilliseconds, dwWakeMask);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineMsgWaitForMultipleObjects(
      nCount, pHandles, fWaitAll, dwMilliseconds, dwWakeMask);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.msgWaitCalls, stats.msgWaitUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitMsgWait, deltaUs);
  return result;
}

static DWORD WINAPI
Hook_MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE *pHandles,
                                 DWORD dwMilliseconds, DWORD dwWakeMask,
                                 DWORD dwFlags) {
  if (!g_trampolineMsgWaitForMultipleObjectsEx)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineMsgWaitForMultipleObjectsEx(
        nCount, pHandles, dwMilliseconds, dwWakeMask, dwFlags);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineMsgWaitForMultipleObjectsEx(
        nCount, pHandles, dwMilliseconds, dwWakeMask, dwFlags);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineMsgWaitForMultipleObjectsEx(
      nCount, pHandles, dwMilliseconds, dwWakeMask, dwFlags);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.msgWaitExCalls, stats.msgWaitExUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitMsgWaitEx, deltaUs);
  return result;
}

static DWORD WINAPI Hook_WaitForSingleObject(HANDLE hHandle,
                                             DWORD dwMilliseconds) {
  if (!g_trampolineWaitForSingleObject)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineWaitForSingleObject(hHandle, dwMilliseconds);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineWaitForSingleObject(hHandle, dwMilliseconds);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineWaitForSingleObject(hHandle, dwMilliseconds);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.waitSingleCalls, stats.waitSingleUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitSingle, deltaUs);
  return result;
}

static DWORD WINAPI
Hook_WaitForMultipleObjects(DWORD nCount, const HANDLE *pHandles, BOOL fWaitAll,
                            DWORD dwMilliseconds) {
  if (!g_trampolineWaitForMultipleObjects)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineWaitForMultipleObjects(nCount, pHandles, fWaitAll,
                                              dwMilliseconds);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineWaitForMultipleObjects(nCount, pHandles, fWaitAll,
                                              dwMilliseconds);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineWaitForMultipleObjects(
      nCount, pHandles, fWaitAll, dwMilliseconds);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.waitMultiCalls, stats.waitMultiUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitMulti, deltaUs);
  return result;
}

static DWORD WINAPI Hook_WaitForMultipleObjectsEx(DWORD nCount,
                                                  const HANDLE *pHandles,
                                                  BOOL fWaitAll,
                                                  DWORD dwMilliseconds,
                                                  BOOL bAlertable) {
  if (!g_trampolineWaitForMultipleObjectsEx)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineWaitForMultipleObjectsEx(
        nCount, pHandles, fWaitAll, dwMilliseconds, bAlertable);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineWaitForMultipleObjectsEx(
        nCount, pHandles, fWaitAll, dwMilliseconds, bAlertable);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineWaitForMultipleObjectsEx(
      nCount, pHandles, fWaitAll, dwMilliseconds, bAlertable);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.waitMultiExCalls, stats.waitMultiExUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitMultiEx, deltaUs);
  return result;
}

static DWORD WINAPI Hook_SignalObjectAndWait(HANDLE hObjectToSignal,
                                             HANDLE hObjectToWaitOn,
                                             DWORD dwMilliseconds,
                                             BOOL bAlertable) {
  if (!g_trampolineSignalObjectAndWait)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineSignalObjectAndWait(hObjectToSignal, hObjectToWaitOn,
                                           dwMilliseconds, bAlertable);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineSignalObjectAndWait(hObjectToSignal, hObjectToWaitOn,
                                           dwMilliseconds, bAlertable);
  }

  const auto begin = PerfClock::now();
  const DWORD result = g_trampolineSignalObjectAndWait(
      hObjectToSignal, hObjectToWaitOn, dwMilliseconds, bAlertable);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.signalWaitCalls, stats.signalWaitUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitSignalWait, deltaUs);
  return result;
}

static DWORD WINAPI Hook_WaitForSingleObjectEx(HANDLE hHandle,
                                                DWORD dwMilliseconds,
                                                BOOL bAlertable) {
  if (!g_trampolineWaitForSingleObjectEx)
    return WAIT_FAILED;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineWaitForSingleObjectEx(hHandle, dwMilliseconds,
                                             bAlertable);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineWaitForSingleObjectEx(hHandle, dwMilliseconds,
                                             bAlertable);
  }

  const auto begin = PerfClock::now();
  const DWORD result =
      g_trampolineWaitForSingleObjectEx(hHandle, dwMilliseconds, bAlertable);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.waitSingleExCalls, stats.waitSingleExUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitSingleEx, deltaUs);
  return result;
}

static LONG NTAPI Hook_NtDelayExecution(BOOLEAN alertable,
                                        PLARGE_INTEGER delayInterval) {
  if (!g_trampolineNtDelayExecution)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineNtDelayExecution(alertable, delayInterval);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineNtDelayExecution(alertable, delayInterval);
  }

  const auto begin = PerfClock::now();
  const LONG result = g_trampolineNtDelayExecution(alertable, delayInterval);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.ntDelayCalls, stats.ntDelayUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitNtDelay, deltaUs);
  return result;
}

static LONG NTAPI Hook_NtWaitForSingleObject(HANDLE handle, BOOLEAN alertable,
                                             PLARGE_INTEGER timeout) {
  if (!g_trampolineNtWaitForSingleObject)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineNtWaitForSingleObject(handle, alertable, timeout);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineNtWaitForSingleObject(handle, alertable, timeout);
  }

  const auto begin = PerfClock::now();
  const LONG result =
      g_trampolineNtWaitForSingleObject(handle, alertable, timeout);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.ntWaitSingleCalls, stats.ntWaitSingleUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitNtWaitSingle, deltaUs);
  return result;
}

static LONG NTAPI Hook_NtWaitForMultipleObjects(ULONG count, HANDLE *handles,
                                                ULONG waitType,
                                                BOOLEAN alertable,
                                                PLARGE_INTEGER timeout) {
  if (!g_trampolineNtWaitForMultipleObjects)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return g_trampolineNtWaitForMultipleObjects(count, handles, waitType,
                                                alertable, timeout);
  }

  if (!IsMainLoopThread()) {
    return g_trampolineNtWaitForMultipleObjects(count, handles, waitType,
                                                alertable, timeout);
  }

  const auto begin = PerfClock::now();
  const LONG result = g_trampolineNtWaitForMultipleObjects(
      count, handles, waitType, alertable, timeout);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.ntWaitMultiCalls, stats.ntWaitMultiUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::WaitNtWaitMulti, deltaUs);
  return result;
}

static void InstallMainThreadWaitHooks() {
  if constexpr (!dxvk::war3::internal::kNativeMainThreadWaitHookEnabled) {
    return;
  }

  static std::atomic<bool> s_installed{false};
  bool expected = false;
  if (!s_installed.compare_exchange_strong(expected, true,
                                           std::memory_order_relaxed)) {
    return;
  }

  const struct {
    const char *modulePrimary;
    const char *moduleFallback;
    const char *procName;
    LPVOID detour;
    LPVOID *trampoline;
  } hookItems[] = {
      {"kernel32.dll", "KernelBase.dll", "Sleep",
       reinterpret_cast<LPVOID>(&Hook_Sleep),
       reinterpret_cast<LPVOID *>(&g_trampolineSleep)},
      {"kernel32.dll", "KernelBase.dll", "SleepEx",
       reinterpret_cast<LPVOID>(&Hook_SleepEx),
       reinterpret_cast<LPVOID *>(&g_trampolineSleepEx)},
      {"user32.dll", nullptr, "WaitMessage",
       reinterpret_cast<LPVOID>(&Hook_WaitMessage),
       reinterpret_cast<LPVOID *>(&g_trampolineWaitMessage)},
      {"user32.dll", nullptr, "MsgWaitForMultipleObjects",
       reinterpret_cast<LPVOID>(&Hook_MsgWaitForMultipleObjects),
       reinterpret_cast<LPVOID *>(&g_trampolineMsgWaitForMultipleObjects)},
      {"user32.dll", nullptr, "MsgWaitForMultipleObjectsEx",
       reinterpret_cast<LPVOID>(&Hook_MsgWaitForMultipleObjectsEx),
       reinterpret_cast<LPVOID *>(&g_trampolineMsgWaitForMultipleObjectsEx)},
      {"kernel32.dll", "KernelBase.dll", "WaitForSingleObjectEx",
       reinterpret_cast<LPVOID>(&Hook_WaitForSingleObjectEx),
       reinterpret_cast<LPVOID *>(&g_trampolineWaitForSingleObjectEx)},
  };

  for (const auto &it : hookItems) {
    FARPROC proc =
        ResolveWinApiProc(it.modulePrimary, it.moduleFallback, it.procName);
    if (!proc) {
      war3dbg::Print(
          "DXVK War3Hook[Lifecycle]: Skip WaitHook %s (proc not found)\n",
          it.procName);
      continue;
    }

    InstallMinHook(reinterpret_cast<LPVOID>(proc), it.detour, it.trampoline,
                   "Lifecycle", it.procName, false, false);
  }

  if constexpr (dxvk::war3::internal::kNativeMainThreadWaitDeepHookEnabled) {
    const struct {
      const char *modulePrimary;
      const char *moduleFallback;
      const char *procName;
      LPVOID detour;
      LPVOID *trampoline;
    } deepHookItems[] = {
        {"kernel32.dll", "KernelBase.dll", "WaitForSingleObject",
         reinterpret_cast<LPVOID>(&Hook_WaitForSingleObject),
         reinterpret_cast<LPVOID *>(&g_trampolineWaitForSingleObject)},
        {"kernel32.dll", "KernelBase.dll", "WaitForMultipleObjects",
         reinterpret_cast<LPVOID>(&Hook_WaitForMultipleObjects),
         reinterpret_cast<LPVOID *>(&g_trampolineWaitForMultipleObjects)},
        {"kernel32.dll", "KernelBase.dll", "WaitForMultipleObjectsEx",
         reinterpret_cast<LPVOID>(&Hook_WaitForMultipleObjectsEx),
         reinterpret_cast<LPVOID *>(&g_trampolineWaitForMultipleObjectsEx)},
        {"kernel32.dll", "KernelBase.dll", "SignalObjectAndWait",
         reinterpret_cast<LPVOID>(&Hook_SignalObjectAndWait),
         reinterpret_cast<LPVOID *>(&g_trampolineSignalObjectAndWait)},
        {"ntdll.dll", nullptr, "NtDelayExecution",
         reinterpret_cast<LPVOID>(&Hook_NtDelayExecution),
         reinterpret_cast<LPVOID *>(&g_trampolineNtDelayExecution)},
        {"ntdll.dll", nullptr, "NtWaitForSingleObject",
         reinterpret_cast<LPVOID>(&Hook_NtWaitForSingleObject),
         reinterpret_cast<LPVOID *>(&g_trampolineNtWaitForSingleObject)},
        {"ntdll.dll", nullptr, "NtWaitForMultipleObjects",
         reinterpret_cast<LPVOID>(&Hook_NtWaitForMultipleObjects),
         reinterpret_cast<LPVOID *>(&g_trampolineNtWaitForMultipleObjects)},
    };

    for (const auto &it : deepHookItems) {
      FARPROC proc =
          ResolveWinApiProc(it.modulePrimary, it.moduleFallback, it.procName);
      if (!proc) {
        war3dbg::Print(
            "DXVK War3Hook[Lifecycle]: Skip DeepWaitHook %s (proc not found)\n",
            it.procName);
        continue;
      }

      InstallMinHook(reinterpret_cast<LPVOID>(proc), it.detour, it.trampoline,
                     "Lifecycle", it.procName, false, false);
    }
  }
}

#define WAR3_HOOK_HOTPATH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if constexpr (dxvk::war3::internal::kNativeHookHotpathVerboseLogging) {    \
      WAR3_RENDER_LOG(fmt, ##__VA_ARGS__);                                     \
    }                                                                          \
  } while (0)

#define WAR3_NATIVE_PATCH_LOG(fmt, ...)                                        \
  do {                                                                         \
    if constexpr (dxvk::war3::internal::kNativePatchVerboseLogging) {          \
      ::dxvk::war3dbg::Print(fmt, ##__VA_ARGS__);                              \
    }                                                                          \
  } while (0)

/**
 * @brief 生命周期域热路径可选性能分段。
 *
 * 关闭 `kNativeOptimizationPerfTrackingEnabled` 时返回空 Scope，
 * 避免高频调用（如 FlushAndReset）反复进入 PerfMonitor 造成额外开销。
 */
static inline war3::War3PerfMonitor::ScopedCpuScope
MakeLifecycleCpuScope(const char *name) {
  if constexpr (dxvk::war3::internal::kNativeOptimizationPerfTrackingEnabled) {
    return war3::War3PerfMonitor::instance().cpuScope(name);
  }
  return {};
}

static int __fastcall Hook_MainRunner(void *thisPtr, void *edx) {
  // 进入主循环时设置线程内标志，避免递归初始化/重入干扰。
  s_inMainRunner = true;
  MarkMainLoopThread();
  war3dbg::Print("DXVK War3Hook: Hook_MainRunner ENTER\n");

  // 在首轮主循环执行前抢先安装 Shadow 域 Hook，避免首帧/首轮写入漏采样。
  const uintptr_t gameBase =
      reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
  TryInstallShadowHooksEarly(gameBase, "MainRunner_ENTER");

  // 先执行原逻辑，保持游戏原始时序。
  int result = 0;
  if (g_trampolineMainRunner)
    result = g_trampolineMainRunner(thisPtr, edx);

  // 在原逻辑返回后触发运行时激活，确保必要上下文已就绪。
  war3dbg::Print("DXVK War3Hook: Hook_MainRunner EXIT (Triggering Init)\n");
  s_inMainRunner = false;

  ActivateWar3Runtime(gameBase, "MainRunner");

  return result;
}

static int __fastcall Hook_MainRunner_Alt(void *thisPtr, void *edx) {
  // 备用主循环入口，与主入口保持一致的激活策略。
  s_inMainRunner = true;
  MarkMainLoopThread();
  war3dbg::Print("DXVK War3Hook: Hook_MainRunner_Alt ENTER\n");

  // 备用入口同样前置安装 Shadow Hook，保证任一路径都能捕获首轮写入。
  const uintptr_t gameBase =
      reinterpret_cast<uintptr_t>(::GetModuleHandleA("Game.dll"));
  TryInstallShadowHooksEarly(gameBase, "MainRunner_Alt_ENTER");

  int result = 0;
  if (g_trampolineMainRunnerAlt)
    result = g_trampolineMainRunnerAlt(thisPtr, edx);

  war3dbg::Print("DXVK War3Hook: Hook_MainRunner_Alt EXIT (Triggering Init)\n");
  s_inMainRunner = false;

  ActivateWar3Runtime(gameBase, "MainRunner_Alt");

  return result;
}

DWORD __fastcall Hook_GetD3d9Parameters(void *thisPtr, void *edx,
                                        D3DPRESENT_PARAMETERS *params) {
  // 先走原函数填充参数，再覆盖 PresentationInterval，避免破坏其他字段。
  static bool s_loggedOnce = false;
  DWORD result = 0;
  if (g_originalGetD3d9Parameters)
    result = g_originalGetD3d9Parameters(thisPtr, edx, params);
  if (params) {
    if (params->Windowed) {
      UINT width = params->BackBufferWidth;
      UINT height = params->BackBufferHeight;

      if ((width == 0u || height == 0u) && params->hDeviceWindow) {
        RECT clientRect = {};
        if (::GetClientRect(params->hDeviceWindow, &clientRect)) {
          width = UINT(clientRect.right - clientRect.left);
          height = UINT(clientRect.bottom - clientRect.top);
        }
      }

      if (!std::exchange(s_loggedOnce, true)) {
        war3dbg::Print(
            "DXVK War3Hook[Lifecycle]: GetD3d9Parameters this=%p hwnd=%p "
            "width=%u height=%u windowed=%u\n",
            thisPtr, params->hDeviceWindow, width, height,
            params->Windowed ? 1u : 0u);
      }

      DiscoverWindowedSizeFields(thisPtr, params->hDeviceWindow, width, height);
    }

    if (dxvk::war3::internal::kWar3ForceImmediatePresentEnabled)
      params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  }
  return result;
}

int __cdecl Hook_EventMainCallback() {
  // 游戏主事件回调（消息泵 case5/case14）：
  // 该点可覆盖大量原版主循环 CPU，便于拆解 Untracked 区域。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEventMainCallback)
      return g_trampolineEventMainCallback();
    if (g_originalEventMainCallback)
      return g_originalEventMainCallback();
    return 1;
  }

  auto perfScope =
      war3::War3PerfMonitor::instance().cpuScope("War3MainLoop/Callback");

  const auto begin = PerfClock::now();
  int result = 1;
  if (g_trampolineEventMainCallback) {
    result = g_trampolineEventMainCallback();
  } else if (g_originalEventMainCallback) {
    result = g_originalEventMainCallback();
  }
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.callbackCalls, stats.callbackUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::Callback, deltaUs);
  return result;
}

int __fastcall Hook_EventMessagePump(int a1, int a2) {
  // 主消息泵稳定入口（PeekMessage + Dispatch 循环）：
  // 相比 case5/case14 回调，该入口命中率更高，用于覆盖 MainLoop CPU 时间。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    int result = 0;
    if (g_trampolineEventMessagePump)
      result = g_trampolineEventMessagePump(a1, a2);
    else if (g_originalEventMessagePump)
      result = g_originalEventMessagePump(a1, a2);
    dxvk::war3::tools::ProcessPendingInternalTestRequest();
    return result;
  }

  auto perfScope =
      war3::War3PerfMonitor::instance().cpuScope("War3MainLoop/Pump");
  const bool outermostPump = (++t_mainLoopCycle.pumpDepth == 1);
  if (outermostPump) {
    ResetMainLoopCycleSample(t_mainLoopCycle);
  }

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEventMessagePump) {
    result = g_trampolineEventMessagePump(a1, a2);
  } else if (g_originalEventMessagePump) {
    result = g_originalEventMessagePump(a1, a2);
  }
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.pumpCalls, stats.pumpUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::Pump, deltaUs);
  if (t_mainLoopCycle.pumpDepth > 0) {
    --t_mainLoopCycle.pumpDepth;
  }
  if (outermostPump) {
    FlushMainLoopCycleToPerf();
  }
  MaybeLogMainLoopPerfStats();
  MaybeLogMainLoopWaitStats();
  dxvk::war3::tools::ProcessPendingInternalTestRequest();
  return result;
}

void __fastcall Hook_EventDispatch(int a1, int a2, void *a3, void *a4) {
  // 事件分发点：细分 MainLoop 消耗来源（系统/输入/游戏/回调）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEventDispatch) {
      g_trampolineEventDispatch(a1, a2, a3, a4);
    } else if (g_originalEventDispatch) {
      g_originalEventDispatch(a1, a2, a3, a4);
    }
    return;
  }

  const auto begin = PerfClock::now();
  if (g_trampolineEventDispatch) {
    g_trampolineEventDispatch(a1, a2, a3, a4);
  } else if (g_originalEventDispatch) {
    g_originalEventDispatch(a1, a2, a3, a4);
  }
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  auto &stats = GetMainLoopPerfStats();
  AddPerfSample(stats.dispatchCalls, stats.dispatchUs, deltaUs);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::Dispatch, deltaUs);
  if constexpr (dxvk::war3::internal::kNativeMainLoopDispatchBreakdownEnabled) {
    if (IsMainLoopThread()) {
      RecordDispatchBuckets(a2, deltaUs);
    }
  }
}

int __fastcall Hook_EngineTlsPump(int a1, int a2) {
  // 引擎线程 TLS 消息泵包装：用于定位 EventPump 之外的主循环时间。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineTlsPump)
      return g_trampolineEngineTlsPump(a1, a2);
    if (g_originalEngineTlsPump)
      return g_originalEngineTlsPump(a1, a2);
    return 0;
  }

  auto perfScope =
      war3::War3PerfMonitor::instance().cpuScope("War3MainLoop/Engine/TlsPump");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineTlsPump)
    result = g_trampolineEngineTlsPump(a1, a2);
  else if (g_originalEngineTlsPump)
    result = g_originalEngineTlsPump(a1, a2);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineTlsPump,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineSelectWorker(int a1, int a2) {
  // 从调度队列选择当前 worker/context。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineSelectWorker)
      return g_trampolineEngineSelectWorker(a1, a2);
    if (g_originalEngineSelectWorker)
      return g_originalEngineSelectWorker(a1, a2);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/SelectWorker");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineSelectWorker)
    result = g_trampolineEngineSelectWorker(a1, a2);
  else if (g_originalEngineSelectWorker)
    result = g_originalEngineSelectWorker(a1, a2);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineSelectWorker,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineRunCallbacks(int thisPtr, void *edx) {
  // 处理到期回调（逻辑/系统任务），通常是主循环 CPU 大头候选。
  MarkMainLoopThread();
  const auto targetFn =
      g_trampolineEngineRunCallbacks ? g_trampolineEngineRunCallbacks
                                     : g_originalEngineRunCallbacks;
  if (!targetFn)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return targetFn(thisPtr);
  }

  uintptr_t callerPc = 0;
  if constexpr (
      dxvk::war3::internal::kNativeMainLoopRunCallbacksCallerBreakdownEnabled) {
    callerPc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  }
  const auto begin = PerfClock::now();
  const int result = targetFn(thisPtr);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineRunCallbacks, deltaUs);
  if constexpr (
      dxvk::war3::internal::kNativeMainLoopRunCallbacksCallerBreakdownEnabled) {
    RecordRunCallbacksCaller(callerPc, deltaUs);
  }
  return result;
}

int __fastcall Hook_EngineQueueFlush(int thisPtr, void *edx) {
  // 清空并执行 deferred 队列块（LoadResourceBlock stage 触发点）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineQueueFlush)
      return g_trampolineEngineQueueFlush(thisPtr);
    if (g_originalEngineQueueFlush)
      return g_originalEngineQueueFlush(thisPtr);
    return 0;
  }

  auto perfScope =
      war3::War3PerfMonitor::instance().cpuScope("War3MainLoop/Engine/QueueFlush");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineQueueFlush)
    result = g_trampolineEngineQueueFlush(thisPtr);
  else if (g_originalEngineQueueFlush)
    result = g_originalEngineQueueFlush(thisPtr);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineQueueFlush,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineFinalizeTick(int thisPtr, void *edx) {
  // Tick 结束收口（状态机阶段 3/4 + 虚函数提交）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineFinalizeTick)
      return g_trampolineEngineFinalizeTick(thisPtr);
    if (g_originalEngineFinalizeTick)
      return g_originalEngineFinalizeTick(thisPtr);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/FinalizeTick");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineFinalizeTick)
    result = g_trampolineEngineFinalizeTick(thisPtr);
  else if (g_originalEngineFinalizeTick)
    result = g_originalEngineFinalizeTick(thisPtr);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineFinalizeTick,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineReschedule(uint32_t engineIndex, uint32_t *ctx, int lane,
                                     uint32_t wakeAt) {
  // 任务重排/迁移：决定下一次 wake 时间与 lane 分配。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineReschedule)
      return g_trampolineEngineReschedule(engineIndex, ctx, lane, wakeAt);
    if (g_originalEngineReschedule)
      return g_originalEngineReschedule(engineIndex, ctx, lane, wakeAt);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/Reschedule");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineReschedule)
    result = g_trampolineEngineReschedule(engineIndex, ctx, lane, wakeAt);
  else if (g_originalEngineReschedule)
    result = g_originalEngineReschedule(engineIndex, ctx, lane, wakeAt);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineReschedule,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EnginePrepareWait(int laneIndex, int workerCtx) {
  // 进入 WaitGate 前的句柄准备（sub_6F05DEE0）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEnginePrepareWait)
      return g_trampolineEnginePrepareWait(laneIndex, workerCtx);
    if (g_originalEnginePrepareWait)
      return g_originalEnginePrepareWait(laneIndex, workerCtx);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/PrepareWait");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEnginePrepareWait)
    result = g_trampolineEnginePrepareWait(laneIndex, workerCtx);
  else if (g_originalEnginePrepareWait)
    result = g_originalEnginePrepareWait(laneIndex, workerCtx);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EnginePrepareWait,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EnginePrepareDispatch(int thisPtr, void *edx) {
  // 超时分支前置阶段（sub_6F05FCA0）。
  MarkMainLoopThread();
  const auto targetFn =
      g_trampolineEnginePrepareDispatch ? g_trampolineEnginePrepareDispatch
                                        : g_originalEnginePrepareDispatch;
  if (!targetFn)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    return targetFn(thisPtr);
  }

  const auto begin = PerfClock::now();
  const int result = targetFn(thisPtr);
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EnginePrepareDispatch, deltaUs);
  return result;
}

int __fastcall Hook_EngineFinalizeDispatch(int thisPtr, void *edx) {
  // Dispatch 后收口（sub_6F05FD10）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineFinalizeDispatch)
      return g_trampolineEngineFinalizeDispatch(thisPtr);
    if (g_originalEngineFinalizeDispatch)
      return g_originalEngineFinalizeDispatch(thisPtr);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/FinalizeDispatch");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineFinalizeDispatch)
    result = g_trampolineEngineFinalizeDispatch(thisPtr);
  else if (g_originalEngineFinalizeDispatch)
    result = g_originalEngineFinalizeDispatch(thisPtr);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineFinalizeDispatch,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineTickUpdate(int thisPtr, void *edx) {
  // Tick 时间推进与阶段5写入（sub_6F05FC10）。
  MarkMainLoopThread();
  const auto targetFn = g_trampolineEngineTickUpdate ? g_trampolineEngineTickUpdate
                                                     : g_originalEngineTickUpdate;
  if (!targetFn)
    return 0;

  if (!ShouldCollectMainLoopPerfSamples()) {
    const int result = targetFn(thisPtr);
    dxvk::war3::tools::ProcessPendingInternalTestRequest();
    return result;
  }

  uint64_t preDispatchUs = 0;
  uint64_t preCallbackUs = 0;
  uint64_t preRunCallbacksUs = 0;
  uint64_t preQueueFlushUs = 0;
  uint64_t prePrepareDispatchUs = 0;
  uint64_t preFinalizeDispatchUs = 0;
  uint64_t preRescheduleUs = 0;
  uint64_t preComputeWakeDeltaUs = 0;
  uint64_t preTlsPumpUs = 0;
  if constexpr (dxvk::war3::internal::kNativeMainLoopTickUpdateSubBreakdownEnabled) {
    // TickUpdate 子路径拆分：
    // 通过“调用前后 Cycle 相位增量”估算 Tick 内部主要阶段占比，避免继续加侵入 Hook。
    preDispatchUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Dispatch)];
    preCallbackUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Callback)];
    preRunCallbacksUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineRunCallbacks)];
    preQueueFlushUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineQueueFlush)];
    prePrepareDispatchUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EnginePrepareDispatch)];
    preFinalizeDispatchUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineFinalizeDispatch)];
    preRescheduleUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineReschedule)];
    preComputeWakeDeltaUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineComputeWakeDelta)];
    preTlsPumpUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineTlsPump)];
  }

  const auto begin = PerfClock::now();
  const int result = targetFn(thisPtr);
  dxvk::war3::tools::ProcessPendingInternalTestRequest();
  const auto end = PerfClock::now();
  const uint64_t deltaUs = DurationUs(begin, end);

  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineTickUpdate, deltaUs);

  if constexpr (dxvk::war3::internal::kNativeMainLoopTickUpdateSubBreakdownEnabled) {
    const uint64_t subDispatchUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Dispatch)] -
        preDispatchUs;
    const uint64_t subCallbackUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::Callback)] -
        preCallbackUs;
    const uint64_t subRunCallbacksUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineRunCallbacks)] - preRunCallbacksUs;
    const uint64_t subQueueFlushUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineQueueFlush)] - preQueueFlushUs;
    const uint64_t subPrepareDispatchUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EnginePrepareDispatch)] - prePrepareDispatchUs;
    const uint64_t subFinalizeDispatchUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineFinalizeDispatch)] - preFinalizeDispatchUs;
    const uint64_t subRescheduleUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineReschedule)] - preRescheduleUs;
    const uint64_t subComputeWakeDeltaUs = t_mainLoopCycle.us[CyclePhaseIndex(
        MainLoopCyclePhase::EngineComputeWakeDelta)] - preComputeWakeDeltaUs;
    const uint64_t subTlsPumpUs =
        t_mainLoopCycle.us[CyclePhaseIndex(MainLoopCyclePhase::EngineTlsPump)] -
        preTlsPumpUs;

    const uint64_t subTotalUs =
        subDispatchUs + subCallbackUs + subRunCallbacksUs + subQueueFlushUs +
        subPrepareDispatchUs + subFinalizeDispatchUs + subRescheduleUs +
        subComputeWakeDeltaUs + subTlsPumpUs;
    const uint64_t selfUs = (deltaUs > subTotalUs) ? (deltaUs - subTotalUs) : 0;

    if (subTotalUs > 0) {
      t_mainLoopCycle.tickUpdateSubTotalUs += subTotalUs;
      t_mainLoopCycle.tickUpdateSubTotalCalls += 1;
    }
    if (selfUs > 0) {
      t_mainLoopCycle.tickUpdateSelfUs += selfUs;
      t_mainLoopCycle.tickUpdateSelfCalls += 1;
    }
    if (subDispatchUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::Dispatch);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subDispatchUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subCallbackUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::Callback);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subCallbackUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subRunCallbacksUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::RunCallbacks);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subRunCallbacksUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subQueueFlushUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::QueueFlush);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subQueueFlushUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subPrepareDispatchUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::PrepareDispatch);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subPrepareDispatchUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subFinalizeDispatchUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::FinalizeDispatch);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subFinalizeDispatchUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subRescheduleUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::Reschedule);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subRescheduleUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subComputeWakeDeltaUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::ComputeWakeDelta);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subComputeWakeDeltaUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
    if (subTlsPumpUs > 0) {
      constexpr size_t kBucket =
          static_cast<size_t>(TickUpdateSubBucket::TlsPump);
      t_mainLoopCycle.tickUpdateSubUs[kBucket] += subTlsPumpUs;
      t_mainLoopCycle.tickUpdateSubCalls[kBucket] += 1;
    }
  }
  return result;
}

int __fastcall Hook_EngineFinalizeWorker(int laneIndex, int workerCtx) {
  // Worker 完结/回收路径（sub_6F05DCE0）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineFinalizeWorker)
      return g_trampolineEngineFinalizeWorker(laneIndex, workerCtx);
    if (g_originalEngineFinalizeWorker)
      return g_originalEngineFinalizeWorker(laneIndex, workerCtx);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/FinalizeWorker");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineFinalizeWorker)
    result = g_trampolineEngineFinalizeWorker(laneIndex, workerCtx);
  else if (g_originalEngineFinalizeWorker)
    result = g_originalEngineFinalizeWorker(laneIndex, workerCtx);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineFinalizeWorker,
                              DurationUs(begin, end));
  return result;
}

int __fastcall Hook_EngineComputeWakeDelta(int thisPtr, int tickNow) {
  // 计算下一次 wake delta（sub_6F060500）。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineComputeWakeDelta)
      return g_trampolineEngineComputeWakeDelta(thisPtr, tickNow);
    if (g_originalEngineComputeWakeDelta)
      return g_originalEngineComputeWakeDelta(thisPtr, tickNow);
    return 0;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/ComputeWakeDelta");

  const auto begin = PerfClock::now();
  int result = 0;
  if (g_trampolineEngineComputeWakeDelta)
    result = g_trampolineEngineComputeWakeDelta(thisPtr, tickNow);
  else if (g_originalEngineComputeWakeDelta)
    result = g_originalEngineComputeWakeDelta(thisPtr, tickNow);
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineComputeWakeDelta,
                              DurationUs(begin, end));
  return result;
}

DWORD __fastcall Hook_EngineWaitGate(HANDLE *thisPtr, void *edx,
                                     DWORD dwMilliseconds) {
  // 内部等待门（sub_6F158940）：主循环中高频出现，优先用于定位未捕获时间。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineWaitGate)
      return g_trampolineEngineWaitGate(thisPtr, dwMilliseconds);
    if (g_originalEngineWaitGate)
      return g_originalEngineWaitGate(thisPtr, dwMilliseconds);
    return WAIT_FAILED;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/WaitGate");
  const auto begin = PerfClock::now();
  DWORD result = WAIT_FAILED;
  if (g_trampolineEngineWaitGate)
    result = g_trampolineEngineWaitGate(thisPtr, dwMilliseconds);
  else if (g_originalEngineWaitGate)
    result = g_originalEngineWaitGate(thisPtr, dwMilliseconds);
  const auto end = PerfClock::now();
  const uint64_t waitUs = DurationUs(begin, end);
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineWaitGate, waitUs);

  // WaitGate 周期拆解：用“上次 WaitGate 结束 -> 本次 WaitGate 开始”近似活跃段，
  // 再叠加本次 Wait 段，得到主循环节拍的活跃/空转比例。
  uint64_t activeUs = 0;
  if (t_waitGateCycleReady && begin > t_waitGateLastEnd) {
    activeUs = DurationUs(t_waitGateLastEnd, begin);
  }
  t_waitGateLastEnd = end;
  t_waitGateCycleReady = true;

  const uint64_t cycleUs = activeUs + waitUs;
  if (cycleUs > 0) {
    constexpr const char *kWaitGatePath = "War3MainLoop/Engine/WaitGate";
    constexpr const char *kWaitGateCyclePath =
        "War3MainLoop/Engine/WaitGate/Cycle";
    war3::War3PerfMonitor &perf = war3::War3PerfMonitor::instance();
    perf.addCpuSample("Cycle", UsToMs(cycleUs), kWaitGatePath, 1);
    perf.addCpuSample("Active", UsToMs(activeUs), kWaitGateCyclePath, 1);
    perf.addCpuSample("Idle", UsToMs(waitUs), kWaitGateCyclePath, 1);
  }
  return result;
}

void __fastcall Hook_EngineSleepGate(uint32_t *thisPtr, void *edx,
                                     DWORD dwMilliseconds) {
  // 内部 Sleep 门（sub_6F1648A0）：会走 Sleep 或高精度等待分支。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineSleepGate) {
      g_trampolineEngineSleepGate(thisPtr, dwMilliseconds);
    } else if (g_originalEngineSleepGate) {
      g_originalEngineSleepGate(thisPtr, dwMilliseconds);
    }
    return;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/SleepGate");
  const auto begin = PerfClock::now();
  if (g_trampolineEngineSleepGate) {
    g_trampolineEngineSleepGate(thisPtr, dwMilliseconds);
  } else if (g_originalEngineSleepGate) {
    g_originalEngineSleepGate(thisPtr, dwMilliseconds);
  }
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineSleepGate,
                              DurationUs(begin, end));
}

void __fastcall Hook_EngineSleepGateInner(uint32_t *thisPtr, void *edx,
                                          DWORD dwMilliseconds) {
  // SleepGate 内部分支（sub_6F164B00）：区分真正 Sleep 与自旋等待路径。
  MarkMainLoopThread();
  if (!ShouldCollectMainLoopPerfSamples()) {
    if (g_trampolineEngineSleepGateInner) {
      g_trampolineEngineSleepGateInner(thisPtr, dwMilliseconds);
    } else if (g_originalEngineSleepGateInner) {
      g_originalEngineSleepGateInner(thisPtr, dwMilliseconds);
    }
    return;
  }

  auto perfScope = war3::War3PerfMonitor::instance().cpuScope(
      "War3MainLoop/Engine/SleepGateInner");
  const auto begin = PerfClock::now();
  if (g_trampolineEngineSleepGateInner) {
    g_trampolineEngineSleepGateInner(thisPtr, dwMilliseconds);
  } else if (g_originalEngineSleepGateInner) {
    g_originalEngineSleepGateInner(thisPtr, dwMilliseconds);
  }
  const auto end = PerfClock::now();
  AddMainLoopCyclePhaseSample(MainLoopCyclePhase::EngineSleepGateInner,
                              DurationUs(begin, end));
}

void __fastcall Hook_GamePause(void *gameUi, void *edx, BOOL isPause,
                               uint32_t a2, uint32_t a3, uint32_t a4,
                               uint32_t a5) {
  if (ShouldBlockGamePauseForAutoTest() && isPause) {
    Logger::info("DXVK War3Hook[Lifecycle]: blocked GamePause request");
    return;
  }

  if (g_trampolineGamePause) {
    g_trampolineGamePause(gameUi, edx, isPause, a2, a3, a4, a5);
  } else if (g_originalGamePause) {
    g_originalGamePause(gameUi, edx, isPause, a2, a3, a4, a5);
  }
}

int __cdecl Hook_FlushAndReset() {
  // 帧尾主入口：
  // 1) 可选跳过空队列 flush；
  // 2) 收口 DXVK 渲染器；
  // 3) 重置队列跟踪与批次状态。
  auto perfScope = MakeLifecycleCpuScope("Hook_FlushAndReset");
  static bool s_first = true;
  if (s_first) {
    s_first = false;
    WAR3_HOOK_HOTPATH_LOG("DXVK War3Hook: Hook_FlushAndReset FIRST_CALL\n");
  }

  bool skipOriginalFlush = false;
  if constexpr (dxvk::war3::internal::kNativePatchSkipFlushWhenQueueEmpty) {
    // 仅在关键指针可用时执行“空队列跳过”判断，避免误跳过必须的状态收口：
    // - Opaque/Transparent 计数均为 0；
    // - 且 StateCleanupPending 为 0（否则仍需执行原生清理路径）。
    if (g_numOfElementsPtr && g_numOfTransparentPtr && g_stateCleanupPendingPtr) {
      if constexpr (dxvk::war3::internal::
                        kNativeOptimizationPerfTrackingEnabled) {
        auto optScope = war3::War3PerfMonitor::instance().cpuScope(
            "Opt/Flush/SkipEmptyCheck");
        const uint32_t opaqueCount = *g_numOfElementsPtr;
        const uint32_t transparentCount = *g_numOfTransparentPtr;
        const uint32_t cleanupPending = *g_stateCleanupPendingPtr;
        skipOriginalFlush =
            (opaqueCount == 0u && transparentCount == 0u &&
             cleanupPending == 0u);
      } else {
        const uint32_t opaqueCount = *g_numOfElementsPtr;
        const uint32_t transparentCount = *g_numOfTransparentPtr;
        const uint32_t cleanupPending = *g_stateCleanupPendingPtr;
        skipOriginalFlush =
            (opaqueCount == 0u && transparentCount == 0u &&
             cleanupPending == 0u);
      }
    }
  }

  int result = 0;
  if (!skipOriginalFlush) {
    // 优先调用 trampoline 保持 Hook 链完整，original 作为兜底。
    if (g_trampolineFlushAndReset) {
      auto origScope = MakeLifecycleCpuScope("Hook_FlushAndReset/Orig");
      result = g_trampolineFlushAndReset();
    } else if (g_originalFlushAndReset) {
      auto origScope = MakeLifecycleCpuScope("Hook_FlushAndReset/Orig");
      result = g_originalFlushAndReset();
    }
  } else {
    auto skipScope = MakeLifecycleCpuScope("Hook_FlushAndReset/SkipEmpty");
    WAR3_NATIVE_PATCH_LOG(
        "DXVK War3Patch: Skip empty FlushAndReset (opaque=0, transparent=0, cleanup=0)\n");
  }

  // 原生 flush 之后执行 DXVK 侧帧结束与状态清理。
  {
    auto endFrameScope = MakeLifecycleCpuScope("Hook_FlushAndReset/EndFrame");
    dxvk::war3::render::War3Renderer::instance().EndFrame();
  }

  {
    auto resetScope = MakeLifecycleCpuScope("Hook_FlushAndReset/ResetCaches");
    dxvk::war3::hooks::War3HookRender::ResetDispatchMergeContext();
    dxvk::war3::render::RenderQueueTracker::instance().Reset();
    dxvk::war3::render::ExecBatchProcessor::ResetFrameCaches();
  }

  War3RenderState::SetTlsBatchHandle(0);
  War3RenderState::SetBatchTag(War3BatchTag::Unknown);
  return result;
}

void War3HookLifecycle::Install(uintptr_t gameBase) {
  const auto &book = GetWar3HookAddressBook127a();
  // 统一地址解析函数，减少重复指针转换样板代码。
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  // 当前版本已验证 RVA。
  LPVOID mainRunnerAddr = resolveCode(book.mainRunner);
  LPVOID mainRunnerAltAddr = resolveCode(book.mainRunnerAlt);
  LPVOID eventMainCallbackAddr = resolveCode(book.eventMainCallback);
  LPVOID eventMessagePumpAddr = resolveCode(book.eventMessagePump);
  LPVOID eventDispatchAddr = resolveCode(book.eventDispatch);
  LPVOID engineTlsPumpAddr = resolveCode(book.engineTlsPump);
  LPVOID engineSelectWorkerAddr = resolveCode(book.engineSelectWorker);
  LPVOID engineRunCallbacksAddr = resolveCode(book.engineRunCallbacks);
  LPVOID engineQueueFlushAddr = resolveCode(book.engineQueueFlush);
  LPVOID engineFinalizeTickAddr = resolveCode(book.engineFinalizeTick);
  LPVOID engineRescheduleAddr = resolveCode(book.engineReschedule);
  LPVOID enginePrepareWaitAddr = resolveCode(book.enginePrepareWait);
  LPVOID engineWaitGateAddr = resolveCode(book.engineWaitGate);
  LPVOID enginePrepareDispatchAddr = resolveCode(book.enginePrepareDispatch);
  LPVOID engineFinalizeDispatchAddr = resolveCode(book.engineFinalizeDispatch);
  LPVOID engineTickUpdateAddr = resolveCode(book.engineTickUpdate);
  LPVOID engineFinalizeWorkerAddr = resolveCode(book.engineFinalizeWorker);
  LPVOID engineComputeWakeDeltaAddr = resolveCode(book.engineComputeWakeDelta);
  LPVOID engineSleepGateAddr = resolveCode(book.engineSleepGate);
  LPVOID engineSleepGateInnerAddr = resolveCode(book.engineSleepGateInner);
  LPVOID gamePauseAddr = resolveCode(book.gamePause);
  LPVOID flushResetAddr = resolveCode(book.flushAndReset);
  // GetD3d9Parameters is hooked via direct memory patch elsewhere typically,
  // but MinHooking it is safer
  LPVOID getD3d9ParametersAddr = resolveCode(book.getD3d9Parameters);
  LPVOID windowMessageTargetLookupAddr =
      resolveCode(book.windowMessageTargetLookup);
  LPVOID windowSizeLParamStateAddr = resolveCode(book.windowSizeLParamState);

  g_originalMainRunner = reinterpret_cast<MainRunnerFn>(mainRunnerAddr);
  g_originalMainRunnerAlt =
      reinterpret_cast<MainRunnerAltFn>(mainRunnerAltAddr);
  g_originalEventMainCallback =
      reinterpret_cast<EventMainCallbackFn>(eventMainCallbackAddr);
  g_originalEventMessagePump =
      reinterpret_cast<EventMessagePumpFn>(eventMessagePumpAddr);
  g_originalEventDispatch = reinterpret_cast<EventDispatchFn>(eventDispatchAddr);
  g_originalEngineTlsPump = reinterpret_cast<EngineTlsPumpFn>(engineTlsPumpAddr);
  g_originalEngineSelectWorker =
      reinterpret_cast<EngineSelectWorkerFn>(engineSelectWorkerAddr);
  g_originalEngineRunCallbacks =
      reinterpret_cast<EngineRunCallbacksFn>(engineRunCallbacksAddr);
  g_originalEngineQueueFlush =
      reinterpret_cast<EngineQueueFlushFn>(engineQueueFlushAddr);
  g_originalEngineFinalizeTick =
      reinterpret_cast<EngineFinalizeTickFn>(engineFinalizeTickAddr);
  g_originalEngineReschedule =
      reinterpret_cast<EngineRescheduleFn>(engineRescheduleAddr);
  g_originalEnginePrepareWait =
      reinterpret_cast<EnginePrepareWaitFn>(enginePrepareWaitAddr);
  g_originalEngineWaitGate = reinterpret_cast<EngineWaitGateFn>(engineWaitGateAddr);
  g_originalEnginePrepareDispatch =
      reinterpret_cast<EnginePrepareDispatchFn>(enginePrepareDispatchAddr);
  g_originalEngineFinalizeDispatch =
      reinterpret_cast<EngineFinalizeDispatchFn>(engineFinalizeDispatchAddr);
  g_originalEngineTickUpdate =
      reinterpret_cast<EngineTickUpdateFn>(engineTickUpdateAddr);
  g_originalEngineFinalizeWorker =
      reinterpret_cast<EngineFinalizeWorkerFn>(engineFinalizeWorkerAddr);
  g_originalEngineComputeWakeDelta =
      reinterpret_cast<EngineComputeWakeDeltaFn>(engineComputeWakeDeltaAddr);
  g_originalEngineSleepGate =
      reinterpret_cast<EngineSleepGateFn>(engineSleepGateAddr);
  g_originalEngineSleepGateInner =
      reinterpret_cast<EngineSleepGateInnerFn>(engineSleepGateInnerAddr);
  g_originalGamePause = reinterpret_cast<GamePauseFn>(gamePauseAddr);
  g_originalFlushAndReset = reinterpret_cast<FlushAndResetFn>(flushResetAddr);
  g_originalGetD3d9Parameters =
      reinterpret_cast<GetD3d9ParametersFn>(getD3d9ParametersAddr);
  g_windowMessageTargetLookup =
      reinterpret_cast<WindowMessageTargetLookupFn>(windowMessageTargetLookupAddr);
  g_windowSizeLParamStateAddr.store(
      reinterpret_cast<uintptr_t>(windowSizeLParamStateAddr),
      std::memory_order_release);

  // 逐项安装 Hook，允许单项失败但继续尝试其余入口，提升兼容性与可诊断性。
  InstallMinHook(mainRunnerAddr, reinterpret_cast<LPVOID>(&Hook_MainRunner),
                 reinterpret_cast<LPVOID *>(&g_trampolineMainRunner),
                 "Lifecycle", "MainRunner", false, false);
  InstallMinHook(mainRunnerAltAddr,
                 reinterpret_cast<LPVOID>(&Hook_MainRunner_Alt),
                 reinterpret_cast<LPVOID *>(&g_trampolineMainRunnerAlt),
                 "Lifecycle", "MainRunner_Alt", false, false);
  InstallMinHook(eventMainCallbackAddr,
                 reinterpret_cast<LPVOID>(&Hook_EventMainCallback),
                 reinterpret_cast<LPVOID *>(&g_trampolineEventMainCallback),
                 "Lifecycle", "EventMainCallback", false, false);
  InstallMinHook(eventMessagePumpAddr,
                 reinterpret_cast<LPVOID>(&Hook_EventMessagePump),
                 reinterpret_cast<LPVOID *>(&g_trampolineEventMessagePump),
                 "Lifecycle", "EventMessagePump", false, false);
  InstallMinHook(eventDispatchAddr, reinterpret_cast<LPVOID>(&Hook_EventDispatch),
                 reinterpret_cast<LPVOID *>(&g_trampolineEventDispatch),
                 "Lifecycle", "EventDispatch", false, false);
  if constexpr (dxvk::war3::internal::kNativeMainLoopDeepPhaseHookEnabled) {
    // 主循环深度阶段：用于拆分 Engine Loop 的 Remaining Untracked。
    InstallMinHook(engineTlsPumpAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineTlsPump),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineTlsPump),
                   "Lifecycle", "EngineTlsPump", false, false);
    InstallMinHook(engineSelectWorkerAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineSelectWorker),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineSelectWorker),
                   "Lifecycle", "EngineSelectWorker", false, false);
    InstallMinHook(engineRunCallbacksAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineRunCallbacks),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineRunCallbacks),
                   "Lifecycle", "EngineRunCallbacks", false, false);
    InstallMinHook(engineQueueFlushAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineQueueFlush),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineQueueFlush),
                   "Lifecycle", "EngineQueueFlush", false, false);
    InstallMinHook(engineFinalizeTickAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineFinalizeTick),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineFinalizeTick),
                   "Lifecycle", "EngineFinalizeTick", false, false);
    InstallMinHook(engineRescheduleAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineReschedule),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineReschedule),
                   "Lifecycle", "EngineReschedule", false, false);
    InstallMinHook(enginePrepareWaitAddr,
                   reinterpret_cast<LPVOID>(&Hook_EnginePrepareWait),
                   reinterpret_cast<LPVOID *>(&g_trampolineEnginePrepareWait),
                   "Lifecycle", "EnginePrepareWait", false, false);
    InstallMinHook(engineWaitGateAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineWaitGate),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineWaitGate),
                   "Lifecycle", "EngineWaitGate", false, false);
    InstallMinHook(enginePrepareDispatchAddr,
                   reinterpret_cast<LPVOID>(&Hook_EnginePrepareDispatch),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineEnginePrepareDispatch),
                   "Lifecycle", "EnginePrepareDispatch", false, false);
    InstallMinHook(engineFinalizeDispatchAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineFinalizeDispatch),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineEngineFinalizeDispatch),
                   "Lifecycle", "EngineFinalizeDispatch", false, false);
    InstallMinHook(engineTickUpdateAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineTickUpdate),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineTickUpdate),
                   "Lifecycle", "EngineTickUpdate", false, false);
    InstallMinHook(engineFinalizeWorkerAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineFinalizeWorker),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineEngineFinalizeWorker),
                   "Lifecycle", "EngineFinalizeWorker", false, false);
    InstallMinHook(engineComputeWakeDeltaAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineComputeWakeDelta),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineEngineComputeWakeDelta),
                   "Lifecycle", "EngineComputeWakeDelta", false, false);
    InstallMinHook(engineSleepGateAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineSleepGate),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineSleepGate),
                   "Lifecycle", "EngineSleepGate", false, false);
    InstallMinHook(engineSleepGateInnerAddr,
                   reinterpret_cast<LPVOID>(&Hook_EngineSleepGateInner),
                   reinterpret_cast<LPVOID *>(&g_trampolineEngineSleepGateInner),
                   "Lifecycle", "EngineSleepGateInner", false, false);
  }
  InstallMinHook(gamePauseAddr, reinterpret_cast<LPVOID>(&Hook_GamePause),
                 reinterpret_cast<LPVOID *>(&g_trampolineGamePause),
                 "Lifecycle", "GamePause", false, false);
  InstallMinHook(flushResetAddr, reinterpret_cast<LPVOID>(&Hook_FlushAndReset),
                 reinterpret_cast<LPVOID *>(&g_trampolineFlushAndReset),
                 "Lifecycle", "FlushAndReset", false, false);
  InstallMinHook(getD3d9ParametersAddr,
                 reinterpret_cast<LPVOID>(&Hook_GetD3d9Parameters),
                 reinterpret_cast<LPVOID *>(&g_trampolineGetD3d9Parameters),
                 "Lifecycle", "GetD3d9Parameters", false, false);

  // 主线程 Wait 链路 Hook：用于拆分 Untracked（Sleep/MsgWait/WaitMessage）。
  InstallMainThreadWaitHooks();
}

void *War3HookLifecycle::GetTrampolineFlushAndReset() {
  return reinterpret_cast<void *>(g_trampolineFlushAndReset);
}

bool TryOverrideWindowedClientSize(UINT previousWidth, UINT previousHeight,
                                   UINT width, UINT height) {
  const uintptr_t owner = g_d3d9ParamsOwner.load(std::memory_order_acquire);
  const uint32_t fieldCount =
      g_d3d9WindowedSizeFieldCount.load(std::memory_order_acquire);
  if (width == 0u || height == 0u)
    return false;

  bool wroteAny = false;
  if (owner && fieldCount != 0u) {
    uint8_t *base = reinterpret_cast<uint8_t *>(owner);

    for (uint32_t i = 0; i < fieldCount; ++i) {
      const auto pair = g_d3d9WindowedSizeFields[i];
      if (!IsReadableRange(base + pair.widthOffset, sizeof(uint32_t) * 2u))
        continue;

      DWORD oldProtect = 0u;
      if (!VirtualProtect(base + pair.widthOffset, sizeof(uint32_t) * 2u,
                          PAGE_EXECUTE_READWRITE, &oldProtect)) {
        continue;
      }

      *reinterpret_cast<uint32_t *>(base + pair.widthOffset) = width;
      *reinterpret_cast<uint32_t *>(base + pair.heightOffset) = height;
      DWORD ignored = 0u;
      VirtualProtect(base + pair.widthOffset, sizeof(uint32_t) * 2u, oldProtect,
                     &ignored);
      wroteAny = true;
    }
  }

  DiscoverWindowedSizeFloatFields(previousWidth, previousHeight);
  const uintptr_t floatHeightAddr =
      g_windowedSizeFloatHeightAddr.load(std::memory_order_acquire);
  const uintptr_t floatWidthAddr =
      g_windowedSizeFloatWidthAddr.load(std::memory_order_acquire);
  if (floatHeightAddr != 0u && floatWidthAddr != 0u) {
    const float heightValue = static_cast<float>(height);
    const float widthValue = static_cast<float>(width);
    if (IsReadableRange(reinterpret_cast<const void*>(floatHeightAddr),
                        sizeof(float) * 2u)) {
      DWORD oldProtect = 0u;
      if (VirtualProtect(reinterpret_cast<void*>(floatHeightAddr),
                         sizeof(float) * 2u, PAGE_EXECUTE_READWRITE,
                         &oldProtect)) {
        *reinterpret_cast<float*>(floatHeightAddr) = heightValue;
        *reinterpret_cast<float*>(floatWidthAddr) = widthValue;
        DWORD ignored = 0u;
        VirtualProtect(reinterpret_cast<void*>(floatHeightAddr),
                       sizeof(float) * 2u, oldProtect, &ignored);
        wroteAny = true;
      }
    }
  }

  if (wroteAny && owner && fieldCount != 0u) {
    war3dbg::Print(
        "DXVK War3Hook[Lifecycle]: override windowed size -> %ux%u "
        "fields=%u owner=%p\n",
        width, height, fieldCount, reinterpret_cast<void *>(owner));
  }
  if (wroteAny && floatHeightAddr != 0u && floatWidthAddr != 0u) {
    war3dbg::Print(
        "DXVK War3Hook[Lifecycle]: override windowed float size -> %ux%u "
        "height=0x%p width=0x%p\n",
        width, height, reinterpret_cast<void*>(floatHeightAddr),
        reinterpret_cast<void*>(floatWidthAddr));
  }

  return wroteAny;
}

bool TryNotifyWindowedUiSizeChanged(HWND window, UINT width, UINT height) {
  if (window == nullptr || width == 0u || height == 0u ||
      g_windowMessageTargetLookup == nullptr) {
    return false;
  }

  const uintptr_t lParamStateAddr =
      g_windowSizeLParamStateAddr.load(std::memory_order_acquire);
  if (lParamStateAddr == 0u ||
      !IsReadableRange(reinterpret_cast<const void *>(lParamStateAddr),
                       sizeof(LPARAM))) {
    return false;
  }

  DWORD oldProtect = 0u;
  const LPARAM sizeLParam = MAKELPARAM(width, height);
  if (!VirtualProtect(reinterpret_cast<void *>(lParamStateAddr), sizeof(LPARAM),
                      PAGE_EXECUTE_READWRITE, &oldProtect)) {
    return false;
  }

  *reinterpret_cast<LPARAM *>(lParamStateAddr) = sizeLParam;
  DWORD ignored = 0u;
  VirtualProtect(reinterpret_cast<void *>(lParamStateAddr), sizeof(LPARAM),
                 oldProtect, &ignored);

  void *target = g_windowMessageTargetLookup(reinterpret_cast<void *>(window));
  if (target == nullptr || !IsReadableRange(target, sizeof(uintptr_t)))
    return false;

  const uintptr_t vtbl = *reinterpret_cast<const uintptr_t *>(target);
  if (vtbl == 0u ||
      !IsReadableRange(reinterpret_cast<const void *>(vtbl + sizeof(uintptr_t)),
                       sizeof(uintptr_t))) {
    return false;
  }

  auto onSize =
      *reinterpret_cast<WindowMessageOnSizeFn const *>(vtbl + sizeof(uintptr_t));
  if (onSize == nullptr)
    return false;

  onSize(target);
  war3dbg::Print(
      "DXVK War3Hook[Lifecycle]: notify native UI windowed resize -> %ux%u "
      "target=%p\n",
      width, height, target);
  return true;
}

DWORD GetMainLoopThreadId() {
  return g_mainLoopThreadId.load(std::memory_order_relaxed);
}

} // namespace dxvk::war3::hooks
