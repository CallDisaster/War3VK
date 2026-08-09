#include "war3_hook_jass.h"
#include "war3_hook_address_book.h"
#include "war3_hook_install_util.h"
#include "war3_hook_perf.h"
#include "war3_jass_command_bridge.h"
#include "war3_jass_native_plan_cache.h"
#include "../../d3d9_war3_debug.h"
#include "../japi/war3_japi_v1.h"

#include "../core/war3_internal_test_config.h"
#include "../tools/war3_perf_monitor.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace dxvk::war3::hooks {

// ---------------------------------------------------------------------------
// JASS 域 Hook：负责 Native 注册入口与 JASS 执行入口的安装。
// 设计目标：
// 1) 尽量复用原生执行路径；
// 2) 只在安全可读/可执行区安装 Hook，降低错误地址导致的崩溃风险。
// ---------------------------------------------------------------------------

using InitJassNativesFn = int(__cdecl *)();
using ExecuteJassFunctionFn = int(__fastcall *)(void *, void *, int, uint32_t *,
                                                int *, int, int, uint32_t *);
using ExecuteJassFunctionInternalFn =
    int(__fastcall *)(void *, void *, int, int *, uint32_t *, int, int, int);
using JassInterpreterMainLoopFn =
    int(__fastcall *)(void *, void *, int, uint32_t *, int, int);
using ExecuteNativeFunctionFn = int(__fastcall *)(void *, void *, void *);

// 原函数与 trampoline 分离保存：
// - original 用于兜底直接调用；
// - trampoline 用于经过 MinHook 桥接后的原逻辑调用。
static InitJassNativesFn g_originalInitJassNatives = nullptr;
static InitJassNativesFn g_trampolineInitJassNatives = nullptr;

static ExecuteJassFunctionFn g_originalExecuteJassFunction = nullptr;
static ExecuteJassFunctionFn g_trampolineExecuteJassFunction = nullptr;
static ExecuteJassFunctionInternalFn g_originalExecuteJassFunctionInternal =
    nullptr;
static ExecuteJassFunctionInternalFn g_trampolineExecuteJassFunctionInternal =
    nullptr;
static JassInterpreterMainLoopFn g_originalJassInterpreterMainLoop = nullptr;
static JassInterpreterMainLoopFn g_trampolineJassInterpreterMainLoop = nullptr;
static ExecuteNativeFunctionFn g_originalExecuteNativeFunction = nullptr;
static ExecuteNativeFunctionFn g_trampolineExecuteNativeFunction = nullptr;

static GetTlsJassDataFn g_getTlsJassData = nullptr;
static RegFuncAddr2HandleFn g_regFuncAddr2Handle = nullptr;
static ComputeHandleMemoryAddrFn g_computeHandleMemoryAddr = nullptr;

static std::atomic<bool> g_nativeExecHookTried{false};
static std::atomic<bool> g_nativeExecHookInstalled{false};
static std::atomic<bool> g_nativeExecHookReadable{false};
static std::atomic<bool> g_nativeExecHookPrologueOk{false};

namespace {

// 校验地址区间是否位于“已提交 + 可读 + 可执行”的内存区域。
// 用于 Hook 安装前的防御性检查，避免版本偏移不匹配时误 Hook。
bool IsExecutableReadableRange(const void *addr, size_t size) {
  if (!addr || size == 0)
    return false;

  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;

  const DWORD prot = mbi.Protect & 0xFFu;
  if (prot == PAGE_NOACCESS)
    return false;

  const bool readable =
      prot == PAGE_READONLY || prot == PAGE_READWRITE ||
      prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
      prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
  const bool executable =
      prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
      prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
  if (!readable || !executable)
    return false;

  const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
  const uintptr_t end = start + size;
  const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  const uintptr_t regionEnd = regionStart + mbi.RegionSize;
  return end <= regionEnd;
}

// 深层 JASS Hook 的最小入口校验：
// - 仅接受常见 x86 函数序言（push ebp; mov ebp, esp）；
// - 防止地址落在函数中段时仍被误判为“可执行可读”并安装 Hook。
bool HasClassicX86FunctionPrologue(const void *addr) {
  if (!IsExecutableReadableRange(addr, 5))
    return false;

  const auto *p = reinterpret_cast<const uint8_t *>(addr);
  const bool normal = (p[0] == 0x55u && p[1] == 0x8Bu && p[2] == 0xECu);
  const bool hotpatch =
      (p[0] == 0x8Bu && p[1] == 0xFFu && p[2] == 0x55u && p[3] == 0x8Bu &&
       p[4] == 0xECu);
  return normal || hotpatch;
}

using PerfClock = std::chrono::steady_clock;

struct JassExecStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> totalUs{0};
  std::atomic<uint64_t> maxUs{0};
  std::atomic<uint64_t> failedRet{0};
  std::atomic<uint64_t> lastLoggedCalls{0};
};

JassExecStats &GetJassExecStats() {
  static JassExecStats s_stats;
  return s_stats;
}

static void UpdateMaxAtomic(std::atomic<uint64_t> &dst, uint64_t value) {
  uint64_t current = dst.load(std::memory_order_relaxed);
  while (current < value &&
         !dst.compare_exchange_weak(current, value,
                                    std::memory_order_relaxed)) {
  }
}

static void RecordJassExecuteStats(int retCode, uint64_t deltaUs) {
  auto &stats = GetJassExecStats();
  const uint64_t calls =
      stats.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  stats.totalUs.fetch_add(deltaUs, std::memory_order_relaxed);
  UpdateMaxAtomic(stats.maxUs, deltaUs);
  if (retCode != 1) {
    stats.failedRet.fetch_add(1, std::memory_order_relaxed);
  }

  if constexpr (dxvk::war3::internal::kNativeJassVmExecStatsLogging) {
    constexpr uint64_t kInterval =
        dxvk::war3::internal::kNativeJassVmExecStatsLogIntervalCalls;
    if (kInterval == 0 || (calls % kInterval) != 0)
      return;

    const uint64_t last = stats.lastLoggedCalls.exchange(
        calls, std::memory_order_relaxed);
    if (last == calls)
      return;

    const uint64_t totalUs = stats.totalUs.load(std::memory_order_relaxed);
    const uint64_t maxUs = stats.maxUs.load(std::memory_order_relaxed);
    const uint64_t failed = stats.failedRet.load(std::memory_order_relaxed);
    const double avgMs =
        calls ? (static_cast<double>(totalUs) / static_cast<double>(calls) /
                 1000.0)
              : 0.0;
    const double maxMs = static_cast<double>(maxUs) / 1000.0;
    const double failPct =
        calls ? (100.0 * static_cast<double>(failed) / static_cast<double>(calls))
              : 0.0;

    war3dbg::Print(
        "DXVK War3Hook[Jass]: Execute stats calls=%llu avg=%.4fms max=%.4fms failed=%llu(%.2f%%)\n",
        static_cast<unsigned long long>(calls), avgMs, maxMs,
        static_cast<unsigned long long>(failed), failPct);
  }
}

// JASS VM 运行态统计与自适应预算状态。
// 说明：
// - JASS 主解释循环在主线程串行执行，这里使用普通计数结构即可；
// - 统计用于回答“是解释器本身慢，还是频繁 timeout 导致调度开销大”。 
struct JassVmRuntimeState {
  uint64_t totalCalls = 0;
  uint64_t retCompleted = 0;
  uint64_t retTimeout = 0;
  uint64_t retNativeError = 0;
  uint64_t retPaused = 0;
  uint64_t retInvalidStack = 0;
  uint64_t retArithError = 0;
  uint64_t retOther = 0;

  uint64_t windowCalls = 0;
  uint64_t windowTimeout = 0;
  uint64_t windowPaused = 0;

  int32_t adaptiveBudget = dxvk::war3::internal::kNativeJassOpBudgetAdaptiveInitial;
};

JassVmRuntimeState &GetJassVmRuntimeState() {
  static JassVmRuntimeState s_state = {};
  return s_state;
}

int ClampJassOpBudget(int budget) {
  budget = std::max(
      budget, static_cast<int>(dxvk::war3::internal::kNativeJassOpBudgetAdaptiveMin));
  budget = std::min(
      budget, static_cast<int>(dxvk::war3::internal::kNativeJassOpBudgetAdaptiveMax));
  return budget;
}

int ResolveJassOpBudget(int originalBudget) {
  if constexpr (dxvk::war3::internal::kNativeJassOpBudgetOverrideEnabled) {
    if (dxvk::war3::internal::kNativeJassOpBudgetOverrideValue > 0)
      return dxvk::war3::internal::kNativeJassOpBudgetOverrideValue;
  }

  if constexpr (dxvk::war3::internal::kNativeJassOpBudgetAdaptiveEnabled) {
    auto &state = GetJassVmRuntimeState();
    return ClampJassOpBudget(state.adaptiveBudget);
  }

  return originalBudget;
}

void RecordJassMainLoopResult(int retCode, int budgetInCall) {
  auto &state = GetJassVmRuntimeState();

  state.totalCalls += 1;
  state.windowCalls += 1;

  switch (retCode) {
  case 1:
    state.retCompleted += 1;
    break;
  case 2:
    state.retTimeout += 1;
    state.windowTimeout += 1;
    break;
  case 3:
    state.retNativeError += 1;
    break;
  case 4:
    state.retPaused += 1;
    state.windowPaused += 1;
    break;
  case 6:
    state.retInvalidStack += 1;
    break;
  case 7:
    state.retArithError += 1;
    break;
  default:
    state.retOther += 1;
    break;
  }

  if constexpr (dxvk::war3::internal::kNativeJassOpBudgetAdaptiveEnabled) {
    constexpr uint32_t kWindow =
        dxvk::war3::internal::kNativeJassOpBudgetAdaptiveWindowCalls;
    if (kWindow > 0 && state.windowCalls >= kWindow) {
      const double timeoutRatio =
          state.windowCalls > 0
              ? static_cast<double>(state.windowTimeout) /
                    static_cast<double>(state.windowCalls)
              : 0.0;

      const int oldBudget = state.adaptiveBudget;
      if (timeoutRatio >
          dxvk::war3::internal::kNativeJassOpBudgetAdaptiveTimeoutHighRatio) {
        state.adaptiveBudget +=
            dxvk::war3::internal::kNativeJassOpBudgetAdaptiveStep;
      } else if (timeoutRatio <
                     dxvk::war3::internal::
                         kNativeJassOpBudgetAdaptiveTimeoutLowRatio &&
                 state.windowPaused == 0) {
        state.adaptiveBudget -=
            dxvk::war3::internal::kNativeJassOpBudgetAdaptiveStep;
      }

      state.adaptiveBudget = ClampJassOpBudget(state.adaptiveBudget);
      if (state.adaptiveBudget != oldBudget) {
        war3dbg::Print(
            "DXVK War3Hook[Jass]: adaptive budget %d -> %d (window=%llu timeout=%llu ratio=%.3f)\n",
            oldBudget, state.adaptiveBudget,
            static_cast<unsigned long long>(state.windowCalls),
            static_cast<unsigned long long>(state.windowTimeout), timeoutRatio);
      }

      state.windowCalls = 0;
      state.windowTimeout = 0;
      state.windowPaused = 0;
    }
  }

  if constexpr (dxvk::war3::internal::kNativeJassVmResultStatsLogging) {
    constexpr uint32_t kLogInterval =
        dxvk::war3::internal::kNativeJassVmResultStatsLogIntervalCalls;
    if (kLogInterval > 0 && (state.totalCalls % kLogInterval) == 0) {
      const double timeoutPct =
          state.totalCalls > 0
              ? (100.0 * static_cast<double>(state.retTimeout) /
                 static_cast<double>(state.totalCalls))
              : 0.0;
      const int adaptiveBudget = ClampJassOpBudget(state.adaptiveBudget);
      war3dbg::Print(
          "DXVK War3Hook[Jass]: MainLoop stats calls=%llu done=%llu timeout=%llu pause=%llu "
          "nativeErr=%llu stackErr=%llu arithErr=%llu other=%llu timeoutPct=%.2f "
          "budget(in)=%d budget(cur)=%d\n",
          static_cast<unsigned long long>(state.totalCalls),
          static_cast<unsigned long long>(state.retCompleted),
          static_cast<unsigned long long>(state.retTimeout),
          static_cast<unsigned long long>(state.retPaused),
          static_cast<unsigned long long>(state.retNativeError),
          static_cast<unsigned long long>(state.retInvalidStack),
          static_cast<unsigned long long>(state.retArithError),
          static_cast<unsigned long long>(state.retOther), timeoutPct, budgetInCall,
          adaptiveBudget);
    }
  }
}

} // namespace

[[maybe_unused]] static const char *JassMainLoopReturnToString(int retCode) {
  switch (retCode) {
  case 1:
    return "completed";
  case 2:
    return "timeout";
  case 3:
    return "native_error";
  case 4:
    return "paused";
  case 6:
    return "invalid_stack";
  case 7:
    return "arith_error";
  default:
    return "other";
  }
}

static int __cdecl Hook_InitJassNatives() {
  // 阶段1：先执行原始初始化，保证游戏侧 Native 表先建立完成。
  war3dbg::Print("DXVK War3Hook: Hook_InitJassNatives ENTER\n");

  int result = 0;
  if (g_trampolineInitJassNatives) {
    result = g_trampolineInitJassNatives();
  }

  // A rebuilt JASS VM starts a new map generation. Retire only objects owned
  // by the public WarVK JAPI before publishing the new carrier table.
  dxvk::war3::japi::ResetAuthorState();
  ResetJassCommandBridgeInstallState();
  TryInstallJassCommandBridge("InitJassNatives");

  war3dbg::Print("DXVK War3Hook: Hook_InitJassNatives EXIT\n");
  return result;
}

int __fastcall Hook_ExecuteJassFunction(void *thisPtr, void *edx, int a2,
                                        uint32_t *a3, int *a4, int a5, int a6,
                                        uint32_t *a7) {
  War3HotHookCallTiming hookTiming(
      War3HotHookId::ExecuteJassFunction, 8u);
  const auto callNativeOriginal = [&]() {
    War3HotHookNativeScope nativeTiming(hookTiming);
    if (g_trampolineExecuteJassFunction) {
      return g_trampolineExecuteJassFunction(
          thisPtr, edx, a2, a3, a4, a5, a6, a7);
    }
    if (g_originalExecuteJassFunction) {
      return g_originalExecuteJassFunction(
          thisPtr, edx, a2, a3, a4, a5, a6, a7);
    }
    return 1;
  };
  // 当前策略：保持完全透传，仅保留可观测性与预算策略切入点。
  static bool s_firstCallLogged = false;
  if (!s_firstCallLogged) {
    s_firstCallLogged = true;
    war3dbg::Print(
        "DXVK War3Hook[Jass]: ExecuteJassFunction FIRST_CALL this=%p vm=%p fn=%08X a5=%d a6=%d\n",
        thisPtr, a4 ? reinterpret_cast<void *>(*a4) : nullptr,
        a3 ? static_cast<unsigned int>(*a3) : 0u, a5, a6);
    if constexpr (dxvk::war3::internal::kNativeJassNativeCallHookEnabled) {
      war3dbg::Print(
          "DXVK War3Hook[JassNative]: status tried=%d installed=%d readable=%d prologue=%d orig=%p tramp=%p tls=%p reg=%p mem=%p fast=%d cache=%d stats=%d\n",
          g_nativeExecHookTried.load(std::memory_order_relaxed) ? 1 : 0,
          g_nativeExecHookInstalled.load(std::memory_order_relaxed) ? 1 : 0,
          g_nativeExecHookReadable.load(std::memory_order_relaxed) ? 1 : 0,
          g_nativeExecHookPrologueOk.load(std::memory_order_relaxed) ? 1 : 0,
          reinterpret_cast<void *>(g_originalExecuteNativeFunction),
          reinterpret_cast<void *>(g_trampolineExecuteNativeFunction),
          reinterpret_cast<void *>(g_getTlsJassData),
          reinterpret_cast<void *>(g_regFuncAddr2Handle),
          reinterpret_cast<void *>(g_computeHandleMemoryAddr),
          dxvk::war3::internal::kNativeJassNativeCallFastInvokeEnabled ? 1 : 0,
          dxvk::war3::internal::kNativeJassNativeCallPlanCacheEnabled ? 1 : 0,
          dxvk::war3::internal::kNativeJassNativeCallStatsLogging ? 1 : 0);
    }
  }

  TryInstallJassCommandBridge("ExecuteJassFunction");

  war3::War3PerfMonitor::ScopedCpuScope perfScope;
  if constexpr (dxvk::war3::internal::kNativeJassVmPerfTrackingEnabled) {
    perfScope =
        war3::War3PerfMonitor::instance().cpuScope("JassVM/ExecuteJassFunction");
  }

  const auto begin = PerfClock::now();
  int result = 1;
  if (g_trampolineExecuteJassFunction || g_originalExecuteJassFunction) {
    war3::War3PerfMonitor::ScopedCpuScope origScope;
    if constexpr (dxvk::war3::internal::kNativeJassVmDetailedScopesEnabled) {
      origScope = war3::War3PerfMonitor::instance().cpuScope(
          "JassVM/ExecuteJassFunction/NativeOriginal");
    }
    result = callNativeOriginal();
  }
  const auto end = PerfClock::now();
  RecordJassExecuteStats(
      result, static_cast<uint64_t>(std::chrono::duration_cast<
                                        std::chrono::microseconds>(end - begin)
                                        .count()));
  return result;
}

int __fastcall Hook_ExecuteJassFunctionInternal(void *thisPtr, void *edx, int a2,
                                                int *a3, uint32_t *a4, int a5,
                                                int a6, int a7) {
  war3::War3PerfMonitor::ScopedCpuScope perfScope;
  if constexpr (dxvk::war3::internal::kNativeJassVmPerfTrackingEnabled) {
    perfScope = war3::War3PerfMonitor::instance().cpuScope(
        "JassVM/ExecuteFunctionInternal");
  }

  const int opBudget = ResolveJassOpBudget(a5);
  if constexpr (dxvk::war3::internal::kNativeJassVmVerboseLogging) {
    static bool s_logged = false;
    if (!s_logged) {
      s_logged = true;
      war3dbg::Print(
          "DXVK War3Hook[Jass]: ExecuteInternal FIRST_CALL budget(in=%d out=%d) "
          "override=%d adaptive=%d\n",
          a5, opBudget,
          dxvk::war3::internal::kNativeJassOpBudgetOverrideEnabled ? 1 : 0,
          dxvk::war3::internal::kNativeJassOpBudgetAdaptiveEnabled ? 1 : 0);
    }
  }

  if (g_trampolineExecuteJassFunctionInternal) {
    war3::War3PerfMonitor::ScopedCpuScope origScope;
    if constexpr (dxvk::war3::internal::kNativeJassVmDetailedScopesEnabled) {
      origScope = war3::War3PerfMonitor::instance().cpuScope(
          "JassVM/ExecuteFunctionInternal/NativeOriginal");
    }
    return g_trampolineExecuteJassFunctionInternal(thisPtr, edx, a2, a3, a4,
                                                   opBudget, a6, a7);
  }
  if (g_originalExecuteJassFunctionInternal) {
    war3::War3PerfMonitor::ScopedCpuScope origScope;
    if constexpr (dxvk::war3::internal::kNativeJassVmDetailedScopesEnabled) {
      origScope = war3::War3PerfMonitor::instance().cpuScope(
          "JassVM/ExecuteFunctionInternal/NativeOriginal");
    }
    return g_originalExecuteJassFunctionInternal(thisPtr, edx, a2, a3, a4,
                                                 opBudget, a6, a7);
  }
  return 1;
}

int __fastcall Hook_JassInterpreterMainLoop(void *thisPtr, void *edx, int a2,
                                            uint32_t *a3, int a4, int a5) {
  war3::War3PerfMonitor::ScopedCpuScope perfScope;
  if constexpr (dxvk::war3::internal::kNativeJassVmPerfTrackingEnabled) {
    perfScope = war3::War3PerfMonitor::instance().cpuScope("JassVM/MainLoop");
  }

  static bool s_firstCallLogged = false;
  if (!s_firstCallLogged) {
    s_firstCallLogged = true;
    war3dbg::Print(
        "DXVK War3Hook[Jass]: MainLoop FIRST_CALL this=%p ip=%08X budget=%d slice=%d\n",
        thisPtr, static_cast<unsigned int>(a2), a4, a5);
  }

  int ret = 1;
  if (g_trampolineJassInterpreterMainLoop) {
    war3::War3PerfMonitor::ScopedCpuScope origScope;
    if constexpr (dxvk::war3::internal::kNativeJassVmDetailedScopesEnabled) {
      origScope =
          war3::War3PerfMonitor::instance().cpuScope(
              "JassVM/MainLoop/NativeOriginal");
    }
    ret = g_trampolineJassInterpreterMainLoop(thisPtr, edx, a2, a3, a4, a5);
  } else if (g_originalJassInterpreterMainLoop) {
    war3::War3PerfMonitor::ScopedCpuScope origScope;
    if constexpr (dxvk::war3::internal::kNativeJassVmDetailedScopesEnabled) {
      origScope =
          war3::War3PerfMonitor::instance().cpuScope(
              "JassVM/MainLoop/NativeOriginal");
    }
    ret = g_originalJassInterpreterMainLoop(thisPtr, edx, a2, a3, a4, a5);
  }

  RecordJassMainLoopResult(ret, a4);

  if constexpr (dxvk::war3::internal::kNativeJassVmVerboseLogging) {
    static uint32_t s_sampleCount = 0;
    ++s_sampleCount;
    if ((s_sampleCount % 1000u) == 0u) {
      war3dbg::Print(
          "DXVK War3Hook[Jass]: MainLoop ret=%d(%s) budget=%d call=%u\n", ret,
          JassMainLoopReturnToString(ret), a4, s_sampleCount);
    }
  }

  return ret;
}

static int CallOriginalExecuteNativeFunction(void *thisPtr, void *edx,
                                             void *arg0) {
  if (g_trampolineExecuteNativeFunction)
    return g_trampolineExecuteNativeFunction(thisPtr, edx, arg0);
  if (g_originalExecuteNativeFunction)
    return g_originalExecuteNativeFunction(thisPtr, edx, arg0);
  return 0;
}

int __fastcall Hook_ExecuteNativeFunction(void *thisPtr, void *edx, void *arg0) {
  if constexpr (!dxvk::war3::internal::kNativeJassNativeCallHookEnabled) {
    return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
  }

#if !defined(_M_IX86) && !defined(__i386__)
  return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
#else
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    war3dbg::Print(
        "DXVK War3Hook[JassNative]: FIRST_CALL vm=%p arg0=%p fast=%d planCache=%d stats=%d\n",
        thisPtr, arg0,
        dxvk::war3::internal::kNativeJassNativeCallFastInvokeEnabled ? 1 : 0,
        dxvk::war3::internal::kNativeJassNativeCallPlanCacheEnabled ? 1 : 0,
        dxvk::war3::internal::kNativeJassNativeCallStatsLogging ? 1 : 0);
  }

  if (!thisPtr || !arg0 || !g_getTlsJassData)
    return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);

  void *nativeEntry = g_getTlsJassData(arg0);
  if (!nativeEntry) {
    RecordNativeCallFallback();
    return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
  }

  NativeCallPlan plan = {};
  if (!BuildOrGetNativeCallPlan(thisPtr, nativeEntry, plan)) {
    RecordNativeCallFallback();
    return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
  }

  if constexpr (!dxvk::war3::internal::kNativeJassNativeCallFastInvokeEnabled) {
    RecordNativeCallFallback();
    return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
  }

  int fastRet = 0;
  if (ExecuteNativeCallFast(thisPtr, plan, fastRet))
    return fastRet;

  RecordNativeCallFallback();
  return CallOriginalExecuteNativeFunction(thisPtr, edx, arg0);
#endif
}

void War3HookJass::Install(uintptr_t gameBase) {
  // 偏移来自统一 AddressBook（1.27a）。
  const auto &book = GetWar3HookAddressBook127a();
  constexpr size_t kProbeSize = 32;

  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID initJassNativesAddr = resolveCode(book.initJassNatives);
  LPVOID executeJassFunctionAddr = resolveCode(book.executeJassFunction);
  LPVOID executeJassFunctionInternalAddr =
      resolveCode(book.executeJassFunctionInternal);
  LPVOID jassInterpreterMainLoopAddr = resolveCode(book.jassInterpreterMainLoop);
  LPVOID executeNativeFunctionAddr = resolveCode(book.executeNativeFunction);

  LPVOID getTlsJassDataAddr = resolveCode(book.getTlsJassData);
  LPVOID regFuncAddr2HandleAddr = resolveCode(book.regFuncAddr2Handle);
  LPVOID computeHandleMemoryAddrAddr =
      resolveCode(book.computeHandleMemoryAddr);

  war3dbg::Print(
      "DXVK War3Hook[Jass]: Install addresses init=%p exec=%p execInternal=%p mainLoop=%p nativeExec=%p\n",
      initJassNativesAddr, executeJassFunctionAddr,
      executeJassFunctionInternalAddr, jassInterpreterMainLoopAddr,
      executeNativeFunctionAddr);

  g_nativeExecHookTried.store(false, std::memory_order_relaxed);
  g_nativeExecHookInstalled.store(false, std::memory_order_relaxed);
  g_nativeExecHookReadable.store(false, std::memory_order_relaxed);
  g_nativeExecHookPrologueOk.store(false, std::memory_order_relaxed);

  g_getTlsJassData = nullptr;
  g_regFuncAddr2Handle = nullptr;
  g_computeHandleMemoryAddr = nullptr;
  if (IsExecutableReadableRange(getTlsJassDataAddr, kProbeSize)) {
    g_getTlsJassData = reinterpret_cast<GetTlsJassDataFn>(getTlsJassDataAddr);
  } else {
    war3dbg::Print("DXVK War3Hook[Jass]: helper getTlsJassData invalid (%p)\n",
                   getTlsJassDataAddr);
  }
  if (IsExecutableReadableRange(regFuncAddr2HandleAddr, kProbeSize)) {
    g_regFuncAddr2Handle =
        reinterpret_cast<RegFuncAddr2HandleFn>(regFuncAddr2HandleAddr);
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: helper regFuncAddr2Handle invalid (%p)\n",
        regFuncAddr2HandleAddr);
  }
  if (IsExecutableReadableRange(computeHandleMemoryAddrAddr, kProbeSize)) {
    g_computeHandleMemoryAddr = reinterpret_cast<ComputeHandleMemoryAddrFn>(
        computeHandleMemoryAddrAddr);
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: helper computeHandleMemoryAddr invalid (%p)\n",
        computeHandleMemoryAddrAddr);
  }
  ConfigureNativeCallHelperFns(
      {g_getTlsJassData, g_regFuncAddr2Handle, g_computeHandleMemoryAddr});
  ConfigureJassCommandBridge(g_getTlsJassData);
  ResetNativeCallPlanCaches();
  if constexpr (dxvk::war3::internal::kNativeJassNativeCallHookEnabled) {
    war3dbg::Print(
        "DXVK War3Hook[JassNative]: helpers tls=%p regCode=%p handleMem=%p fast=%d stats=%d interval=%u\n",
        reinterpret_cast<void *>(g_getTlsJassData),
        reinterpret_cast<void *>(g_regFuncAddr2Handle),
        reinterpret_cast<void *>(g_computeHandleMemoryAddr),
        dxvk::war3::internal::kNativeJassNativeCallFastInvokeEnabled ? 1 : 0,
        dxvk::war3::internal::kNativeJassNativeCallStatsLogging ? 1 : 0,
        static_cast<unsigned>(
            dxvk::war3::internal::kNativeJassNativeCallStatsInterval));
  }

  // Hook 安装步骤：
  // 1) 可执行区校验；
  // 2) 保存原函数指针；
  // 3) InstallMinHook（统一 MinHook 安装/启用与日志）。
  if (IsExecutableReadableRange(initJassNativesAddr, kProbeSize)) {
    g_originalInitJassNatives =
        reinterpret_cast<InitJassNativesFn>(initJassNativesAddr);
    InstallMinHook(initJassNativesAddr,
                   reinterpret_cast<LPVOID>(&Hook_InitJassNatives),
                   reinterpret_cast<LPVOID *>(&g_trampolineInitJassNatives),
                   "Jass", "InitJassNatives", false, false);
  } else {
    war3dbg::Print("DXVK War3Hook: Skip InitJassNatives hook (addr=%p)\n",
                   initJassNativesAddr);
  }

  // ExecuteJassFunction 入口同样采用防御式安装，避免错误偏移导致崩溃。
  if (IsExecutableReadableRange(executeJassFunctionAddr, kProbeSize)) {
    g_originalExecuteJassFunction =
        reinterpret_cast<ExecuteJassFunctionFn>(executeJassFunctionAddr);
    InstallMinHook(executeJassFunctionAddr,
                   reinterpret_cast<LPVOID>(&Hook_ExecuteJassFunction),
                   reinterpret_cast<LPVOID *>(
                       &g_trampolineExecuteJassFunction),
                   "Jass", "ExecuteJassFunction", false, false);
  } else {
    war3dbg::Print("DXVK War3Hook: Skip ExecuteJassFunction hook (addr=%p)\n",
                   executeJassFunctionAddr);
  }

  // Task-4：Native 调用计划缓存入口。
  // 默认关闭，仅在专项验证时开启，确保联机/发布档不受影响。
  if constexpr (dxvk::war3::internal::kNativeJassNativeCallHookEnabled) {
    const bool nativeReadable =
        IsExecutableReadableRange(executeNativeFunctionAddr, kProbeSize);
    const bool nativePrologueOk =
        nativeReadable && HasClassicX86FunctionPrologue(executeNativeFunctionAddr);
    g_nativeExecHookTried.store(true, std::memory_order_relaxed);
    g_nativeExecHookReadable.store(nativeReadable, std::memory_order_relaxed);
    g_nativeExecHookPrologueOk.store(nativePrologueOk, std::memory_order_relaxed);

    const bool canHookNativeExec = nativeReadable && nativePrologueOk;
    if (canHookNativeExec) {
      g_originalExecuteNativeFunction =
          reinterpret_cast<ExecuteNativeFunctionFn>(executeNativeFunctionAddr);
      const bool installed =
          InstallMinHook(executeNativeFunctionAddr,
                         reinterpret_cast<LPVOID>(&Hook_ExecuteNativeFunction),
                         reinterpret_cast<LPVOID *>(
                             &g_trampolineExecuteNativeFunction),
                         "Jass", "ExecuteNativeFunction", false, true);
      g_nativeExecHookInstalled.store(installed, std::memory_order_relaxed);
      war3dbg::Print(
          "DXVK War3Hook[JassNative]: install attempted addr=%p installed=%d tramp=%p\n",
          executeNativeFunctionAddr, installed ? 1 : 0,
          reinterpret_cast<void *>(g_trampolineExecuteNativeFunction));
    } else {
      uint8_t b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0;
      if (nativeReadable && IsExecutableReadableRange(executeNativeFunctionAddr, 5)) {
        const auto *p = reinterpret_cast<const uint8_t *>(executeNativeFunctionAddr);
        b0 = p[0];
        b1 = p[1];
        b2 = p[2];
        b3 = p[3];
        b4 = p[4];
      }
      war3dbg::Print(
          "DXVK War3Hook[Jass]: Skip ExecuteNativeFunction hook (addr=%p, readable=%d, prologue=%d, bytes=%02X %02X %02X %02X %02X)\n",
          executeNativeFunctionAddr, nativeReadable ? 1 : 0,
          nativePrologueOk ? 1 : 0, b0, b1, b2, b3, b4);
    }
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: Native call hook disabled (Task-4)\n");
  }

  // 深层 VM Hook 默认关闭：这两个地址在小版本差异下更容易漂移，
  // 一旦命中函数中段会直接导致启动期崩溃。
  if constexpr (dxvk::war3::internal::kNativeJassVmDeepHooksEnabled) {
    const bool canHookExecInternal =
        IsExecutableReadableRange(executeJassFunctionInternalAddr, kProbeSize) &&
        HasClassicX86FunctionPrologue(executeJassFunctionInternalAddr);
    if (canHookExecInternal) {
      g_originalExecuteJassFunctionInternal =
          reinterpret_cast<ExecuteJassFunctionInternalFn>(
              executeJassFunctionInternalAddr);
      InstallMinHook(executeJassFunctionInternalAddr,
                     reinterpret_cast<LPVOID>(&Hook_ExecuteJassFunctionInternal),
                     reinterpret_cast<LPVOID *>(
                         &g_trampolineExecuteJassFunctionInternal),
                     "Jass", "ExecuteJassFunctionInternal", false, false);
    } else {
      war3dbg::Print(
          "DXVK War3Hook: Skip ExecuteJassFunctionInternal hook (addr=%p, prologue check failed)\n",
          executeJassFunctionInternalAddr);
    }

    const bool canHookMainLoop =
        IsExecutableReadableRange(jassInterpreterMainLoopAddr, kProbeSize) &&
        HasClassicX86FunctionPrologue(jassInterpreterMainLoopAddr);
    if (canHookMainLoop) {
      g_originalJassInterpreterMainLoop =
          reinterpret_cast<JassInterpreterMainLoopFn>(
              jassInterpreterMainLoopAddr);
      InstallMinHook(jassInterpreterMainLoopAddr,
                     reinterpret_cast<LPVOID>(&Hook_JassInterpreterMainLoop),
                     reinterpret_cast<LPVOID *>(
                         &g_trampolineJassInterpreterMainLoop),
                     "Jass", "JassInterpreterMainLoop", false, false);
    } else {
      war3dbg::Print(
          "DXVK War3Hook: Skip JassInterpreterMainLoop hook (addr=%p, prologue check failed)\n",
          jassInterpreterMainLoopAddr);
    }
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: Deep VM hooks disabled (ExecuteInternal/MainLoop)\n");
  }
}

bool War3HookJass::InstallCommandBridgeOnly(uintptr_t gameBase,
                                            const char *reason) {
  if (gameBase == 0u) {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: bridge-only install skipped - Game.dll base is null reason=%s\n",
        reason ? reason : "<unknown>");
    return false;
  }

  const auto &book = GetWar3HookAddressBook127a();
  constexpr size_t kProbeSize = 32;
  auto resolveCode = [&](uintptr_t rva) -> LPVOID {
    return reinterpret_cast<LPVOID>(gameBase + rva);
  };

  LPVOID getTlsJassDataAddr = resolveCode(book.getTlsJassData);
  LPVOID regFuncAddr2HandleAddr = resolveCode(book.regFuncAddr2Handle);
  LPVOID computeHandleMemoryAddrAddr =
      resolveCode(book.computeHandleMemoryAddr);

  g_getTlsJassData = nullptr;
  g_regFuncAddr2Handle = nullptr;
  g_computeHandleMemoryAddr = nullptr;

  if (IsExecutableReadableRange(getTlsJassDataAddr, kProbeSize)) {
    g_getTlsJassData = reinterpret_cast<GetTlsJassDataFn>(getTlsJassDataAddr);
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: bridge-only helper getTlsJassData invalid (%p) reason=%s\n",
        getTlsJassDataAddr, reason ? reason : "<unknown>");
  }

  if (IsExecutableReadableRange(regFuncAddr2HandleAddr, kProbeSize)) {
    g_regFuncAddr2Handle =
        reinterpret_cast<RegFuncAddr2HandleFn>(regFuncAddr2HandleAddr);
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: bridge-only helper regFuncAddr2Handle invalid (%p) reason=%s\n",
        regFuncAddr2HandleAddr, reason ? reason : "<unknown>");
  }

  if (IsExecutableReadableRange(computeHandleMemoryAddrAddr, kProbeSize)) {
    g_computeHandleMemoryAddr =
        reinterpret_cast<ComputeHandleMemoryAddrFn>(
            computeHandleMemoryAddrAddr);
  } else {
    war3dbg::Print(
        "DXVK War3Hook[Jass]: bridge-only helper computeHandleMemoryAddr invalid (%p) reason=%s\n",
        computeHandleMemoryAddrAddr, reason ? reason : "<unknown>");
  }

  ConfigureNativeCallHelperFns(
      {g_getTlsJassData, g_regFuncAddr2Handle, g_computeHandleMemoryAddr});
  ConfigureJassCommandBridge(g_getTlsJassData);
  ResetNativeCallPlanCaches();
  ResetJassCommandBridgeInstallState();
  TryInstallJassCommandBridge(reason ? reason : "CommandBridgeOnly");

  const bool installed = IsJassCommandBridgeInstalled();
  war3dbg::Print(
      "DXVK War3Hook[Jass]: bridge-only install reason=%s installed=%d tls=%p regCode=%p handleMem=%p\n",
      reason ? reason : "<unknown>", installed ? 1 : 0,
      reinterpret_cast<void *>(g_getTlsJassData),
      reinterpret_cast<void *>(g_regFuncAddr2Handle),
      reinterpret_cast<void *>(g_computeHandleMemoryAddr));
  return installed;
}

} // namespace dxvk::war3::hooks
