#include "war3_jass_native_plan_cache.h"

#include "war3_jass_native_invoke_x86.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace dxvk::war3::hooks {

namespace {

constexpr size_t kNativeEntryFuncPtrOffset = 0x1C;
constexpr size_t kNativeEntryParamCountOffset = 0x20;
constexpr size_t kNativeEntrySigPtrOffset = 0x24;
constexpr size_t kNativeEntryRetTypeOffset = 0x38;

constexpr size_t kVmStackPtrOffset = 0x2868;
constexpr size_t kStackTopOffset = 0x8C;
constexpr size_t kStackItemsBaseOffset = 0x08;
constexpr size_t kStackValueTypeOffset = 0x18;
constexpr size_t kStackValueDataOffset = 0x20;

constexpr uint32_t kJassStackValueTypeCode = 3;
constexpr uint32_t kThreadL1Slots = 8;

struct ThreadL1Entry {
  bool valid = false;
  uint32_t stamp = 0;
  NativeCallPlan plan = {};
};

struct ThreadL1State {
  std::array<ThreadL1Entry, kThreadL1Slots> slots = {};
  uint32_t clock = 0;
};

static thread_local ThreadL1State t_l1State;
static thread_local std::array<uint32_t,
                               dxvk::war3::internal::kNativeJassNativeCallMaxArgs>
    t_callArgs = {};
static thread_local std::array<uint32_t,
                               dxvk::war3::internal::kNativeJassNativeCallMaxArgs>
    t_callScratch = {};

static std::mutex s_l2Mutex;
static std::unordered_map<uintptr_t, NativeCallPlan> s_l2Plans;

static std::atomic<uintptr_t> s_getTlsJassDataFn{0};
static std::atomic<uintptr_t> s_regFuncAddr2HandleFn{0};
static std::atomic<uintptr_t> s_computeHandleMemoryAddrFn{0};

static std::atomic<uint64_t> s_statsCalls{0};
static std::atomic<uint64_t> s_statsHits{0};
static std::atomic<uint64_t> s_statsMisses{0};
static std::atomic<uint64_t> s_statsFallbacks{0};
static std::atomic<uint64_t> s_statsRebuilds{0};
static std::atomic<uint64_t> s_statsLastLoggedCall{0};

static void MaybeLogStats() {
  if constexpr (!dxvk::war3::internal::kNativeJassNativeCallStatsLogging)
    return;

  constexpr uint64_t kInterval =
      dxvk::war3::internal::kNativeJassNativeCallStatsInterval;
  if (kInterval == 0)
    return;

  const uint64_t calls = s_statsCalls.load(std::memory_order_relaxed);
  if (calls == 0 || (calls % kInterval) != 0)
    return;

  const uint64_t last = s_statsLastLoggedCall.exchange(
      calls, std::memory_order_relaxed);
  if (last == calls)
    return;

  const uint64_t hits = s_statsHits.load(std::memory_order_relaxed);
  const uint64_t misses = s_statsMisses.load(std::memory_order_relaxed);
  const uint64_t fallbacks = s_statsFallbacks.load(std::memory_order_relaxed);
  const uint64_t rebuilds = s_statsRebuilds.load(std::memory_order_relaxed);
  const double hitPct =
      calls > 0
          ? (100.0 * static_cast<double>(hits) / static_cast<double>(calls))
          : 0.0;

  war3dbg::Print(
      "DXVK War3Hook[JassNative]: calls=%llu hit=%llu miss=%llu fallback=%llu rebuild=%llu hitPct=%.2f\n",
      static_cast<unsigned long long>(calls),
      static_cast<unsigned long long>(hits),
      static_cast<unsigned long long>(misses),
      static_cast<unsigned long long>(fallbacks),
      static_cast<unsigned long long>(rebuilds), hitPct);
}

static inline void TrackCall() {
  s_statsCalls.fetch_add(1, std::memory_order_relaxed);
  MaybeLogStats();
}

static inline void TrackHit() {
  s_statsHits.fetch_add(1, std::memory_order_relaxed);
  MaybeLogStats();
}

static inline void TrackMiss() {
  s_statsMisses.fetch_add(1, std::memory_order_relaxed);
  MaybeLogStats();
}

static inline void TrackFallback() {
  s_statsFallbacks.fetch_add(1, std::memory_order_relaxed);
  MaybeLogStats();
}

static inline void TrackRebuild() {
  s_statsRebuilds.fetch_add(1, std::memory_order_relaxed);
  MaybeLogStats();
}

static inline bool ReadByte(const char *p, char &out) {
  if (!p || !dxvk::war3::IsReadableRange(p, sizeof(char)))
    return false;
  out = *p;
  return true;
}

static bool ReadNativeMeta(void *nativeEntry, void *&funcPtr, const char *&sigPtr,
                           uint32_t &paramCount, uint32_t &retType) {
  if (!nativeEntry)
    return false;
  if (!dxvk::war3::SafeReadPtr(nativeEntry, kNativeEntryFuncPtrOffset, funcPtr))
    return false;
  if (!dxvk::war3::SafeRead<const char *>(nativeEntry, kNativeEntrySigPtrOffset,
                                          sigPtr))
    return false;
  if (!dxvk::war3::SafeReadU32(nativeEntry, kNativeEntryParamCountOffset,
                               paramCount))
    return false;
  if (!dxvk::war3::SafeReadU32(nativeEntry, kNativeEntryRetTypeOffset, retType))
    return false;
  if (!funcPtr || !sigPtr)
    return false;
  if (paramCount > dxvk::war3::internal::kNativeJassNativeCallMaxArgs)
    return false;
  return true;
}

static bool IsPlanStillValid(const NativeCallPlan &plan) {
  void *funcPtr = nullptr;
  const char *sigPtr = nullptr;
  uint32_t paramCount = 0;
  uint32_t retType = 0;
  if (!ReadNativeMeta(plan.nativeEntry, funcPtr, sigPtr, paramCount, retType))
    return false;
  return funcPtr == plan.funcPtr && sigPtr == plan.sigPtr &&
         paramCount == plan.paramCount;
}

static bool BuildPlanFromNativeEntry(void *nativeEntry, NativeCallPlan &outPlan) {
  outPlan = {};

  void *funcPtr = nullptr;
  const char *sigPtr = nullptr;
  uint32_t paramCount = 0;
  uint32_t retType = 0;
  if (!ReadNativeMeta(nativeEntry, funcPtr, sigPtr, paramCount, retType))
    return false;

  char sigHead = 0;
  if (!ReadByte(sigPtr, sigHead) || sigHead != '(')
    return false;

  outPlan.nativeEntry = nativeEntry;
  outPlan.funcPtr = funcPtr;
  outPlan.sigPtr = sigPtr;
  outPlan.paramCount = paramCount;
  outPlan.retType = retType;

  const char *cursor = sigPtr + 1;
  uint32_t scratchCount = 0;
  for (uint32_t i = 0; i < paramCount; ++i) {
    char sigChar = 0;
    if (!ReadByte(cursor, sigChar))
      return false;
    if (sigChar == '\0' || sigChar == ')')
      return false;

    NativeArgPlan argPlan = {};
    argPlan.sigChar = static_cast<uint8_t>(sigChar);
    argPlan.sigAdvance = 1;
    argPlan.op = NativeArgOp::DirectValue;

    switch (sigChar) {
    case 'C':
      argPlan.op = NativeArgOp::FuncAddrToHandle;
      break;
    case 'S':
      argPlan.op = NativeArgOp::HandleToMemoryAddr;
      break;
    case 'R':
      argPlan.op = NativeArgOp::RealByRefTemp;
      scratchCount += 1;
      break;
    case 'H': {
      argPlan.op = NativeArgOp::HandleSigSkipToSemicolon;
      uint32_t advance = 1;
      const char *scan = cursor;
      for (;;) {
        ++scan;
        ++advance;
        char c = 0;
        if (!ReadByte(scan, c))
          return false;
        if (c == ';')
          break;
        if (c == '\0')
          return false;
      }
      if (advance > 255)
        return false;
      argPlan.sigAdvance = static_cast<uint8_t>(advance);
      cursor = scan + 1;
      outPlan.argOps[i] = argPlan;
      continue;
    }
    default:
      argPlan.op = NativeArgOp::ZeroIfType3AndNotC;
      break;
    }

    outPlan.argOps[i] = argPlan;
    cursor += argPlan.sigAdvance;
  }

  char closeParen = 0;
  if (!ReadByte(cursor, closeParen) || closeParen != ')')
    return false;

  outPlan.scratchCount = scratchCount;
  return true;
}

static void TouchThreadL1Slot(ThreadL1Entry &slot) {
  ++t_l1State.clock;
  if (t_l1State.clock == 0) {
    t_l1State.clock = 1;
    for (auto &e : t_l1State.slots)
      e.stamp >>= 1;
  }
  slot.stamp = t_l1State.clock;
}

static ThreadL1Entry *FindThreadL1Entry(void *nativeEntry) {
  for (auto &slot : t_l1State.slots) {
    if (!slot.valid)
      continue;
    if (slot.plan.nativeEntry != nativeEntry)
      continue;
    if (!IsPlanStillValid(slot.plan)) {
      slot.valid = false;
      TrackRebuild();
      return nullptr;
    }
    TouchThreadL1Slot(slot);
    return &slot;
  }
  return nullptr;
}

static void InsertThreadL1Entry(const NativeCallPlan &plan) {
  ThreadL1Entry *victim = &t_l1State.slots[0];
  for (auto &slot : t_l1State.slots) {
    if (!slot.valid) {
      victim = &slot;
      break;
    }
    if (slot.stamp < victim->stamp)
      victim = &slot;
  }

  victim->valid = true;
  victim->plan = plan;
  TouchThreadL1Slot(*victim);
}

static NativeCallHelperFns LoadHelpers() {
  NativeCallHelperFns helpers = {};
  helpers.getTlsJassData = reinterpret_cast<GetTlsJassDataFn>(
      s_getTlsJassDataFn.load(std::memory_order_relaxed));
  helpers.regFuncAddr2Handle = reinterpret_cast<RegFuncAddr2HandleFn>(
      s_regFuncAddr2HandleFn.load(std::memory_order_relaxed));
  helpers.computeHandleMemoryAddr = reinterpret_cast<ComputeHandleMemoryAddrFn>(
      s_computeHandleMemoryAddrFn.load(std::memory_order_relaxed));
  return helpers;
}

} // namespace

void ConfigureNativeCallHelperFns(const NativeCallHelperFns &helpers) {
  s_getTlsJassDataFn.store(reinterpret_cast<uintptr_t>(helpers.getTlsJassData),
                           std::memory_order_relaxed);
  s_regFuncAddr2HandleFn.store(
      reinterpret_cast<uintptr_t>(helpers.regFuncAddr2Handle),
      std::memory_order_relaxed);
  s_computeHandleMemoryAddrFn.store(
      reinterpret_cast<uintptr_t>(helpers.computeHandleMemoryAddr),
      std::memory_order_relaxed);
}

void ResetNativeCallPlanCaches() {
  {
    std::lock_guard<std::mutex> lock(s_l2Mutex);
    s_l2Plans.clear();
  }

  t_l1State = {};
}

void RecordNativeCallFallback() {
  TrackFallback();
}

bool BuildOrGetNativeCallPlan(void *vm, void *nativeEntry, NativeCallPlan &out) {
  (void)vm;
  if (!nativeEntry)
    return false;

  if constexpr (!dxvk::war3::internal::kNativeJassNativeCallPlanCacheEnabled) {
    return BuildPlanFromNativeEntry(nativeEntry, out);
  }

  if (ThreadL1Entry *l1 = FindThreadL1Entry(nativeEntry)) {
    out = l1->plan;
    TrackHit();
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(s_l2Mutex);
    auto it = s_l2Plans.find(reinterpret_cast<uintptr_t>(nativeEntry));
    if (it != s_l2Plans.end()) {
      if (IsPlanStillValid(it->second)) {
        out = it->second;
        InsertThreadL1Entry(out);
        TrackHit();
        return true;
      }

      s_l2Plans.erase(it);
      TrackRebuild();
    }
  }

  TrackMiss();

  NativeCallPlan built = {};
  if (!BuildPlanFromNativeEntry(nativeEntry, built))
    return false;

  {
    std::lock_guard<std::mutex> lock(s_l2Mutex);
    const size_t capacity =
        static_cast<size_t>(dxvk::war3::internal::kNativeJassNativeCallCacheCapacity);
    if (capacity > 0 && s_l2Plans.size() >= capacity && !s_l2Plans.empty()) {
      s_l2Plans.erase(s_l2Plans.begin());
    }

    s_l2Plans[reinterpret_cast<uintptr_t>(nativeEntry)] = built;
  }

  out = built;
  InsertThreadL1Entry(out);
  return true;
}

bool ExecuteNativeCallFast(void *vm, const NativeCallPlan &plan, int &ret) {
  TrackCall();

  if (!vm || !plan.funcPtr)
    return false;
  if (plan.paramCount > dxvk::war3::internal::kNativeJassNativeCallMaxArgs)
    return false;
  if (!IsPlanStillValid(plan))
    return false;
  if (!IsCdeclPackedInvokeSupported())
    return false;

  auto helpers = LoadHelpers();
  if (!helpers.regFuncAddr2Handle || !helpers.computeHandleMemoryAddr)
    return false;

  void *stackPtr = nullptr;
  if (!dxvk::war3::SafeReadPtr(vm, kVmStackPtrOffset, stackPtr) || !stackPtr)
    return false;

  uint32_t stackTop = 0;
  if (!dxvk::war3::SafeReadU32(stackPtr, kStackTopOffset, stackTop))
    return false;
  if (stackTop + 1 < plan.paramCount)
    return false;

  uint32_t scratchCount = 0;
  for (uint32_t i = 0; i < plan.paramCount; ++i) {
    const uint32_t reverseIdx = plan.paramCount - 1 - i;
    if (stackTop < reverseIdx)
      return false;
    const uint32_t sourceIndex = stackTop - reverseIdx;

    const uintptr_t itemSlotAddr =
        reinterpret_cast<uintptr_t>(stackPtr) + kStackItemsBaseOffset +
        static_cast<uintptr_t>(sourceIndex) * sizeof(void *);
    void *valueNode = nullptr;
    if (!dxvk::war3::IsReadableRange(reinterpret_cast<const void *>(itemSlotAddr),
                                     sizeof(void *)))
      return false;
    valueNode = *reinterpret_cast<void *const *>(itemSlotAddr);
    if (!valueNode)
      return false;

    uint32_t valueType = 0;
    uint32_t rawValue = 0;
    if (!dxvk::war3::SafeReadU32(valueNode, kStackValueTypeOffset, valueType))
      return false;
    if (!dxvk::war3::SafeReadU32(valueNode, kStackValueDataOffset, rawValue))
      return false;

    const NativeArgPlan &argPlan = plan.argOps[i];
    uint32_t packed = rawValue;

    if (valueType == kJassStackValueTypeCode && argPlan.sigChar != 'C') {
      t_callArgs[i] = 0;
      continue;
    }

    switch (argPlan.op) {
    case NativeArgOp::FuncAddrToHandle:
      if (valueType == kJassStackValueTypeCode) {
        packed = helpers.regFuncAddr2Handle(vm, rawValue);
      } else {
        // ASM 对齐：'C' 分支在 valueType!=3 时写入 0，而不是透传原值。
        packed = 0;
      }
      break;
    case NativeArgOp::HandleToMemoryAddr:
      packed = helpers.computeHandleMemoryAddr(vm, rawValue);
      break;
    case NativeArgOp::RealByRefTemp:
      if (scratchCount >=
          dxvk::war3::internal::kNativeJassNativeCallMaxArgs) {
        return false;
      }
      t_callScratch[scratchCount] = rawValue;
      packed = static_cast<uint32_t>(
          reinterpret_cast<uintptr_t>(&t_callScratch[scratchCount]));
      scratchCount += 1;
      break;
    case NativeArgOp::HandleSigSkipToSemicolon:
    case NativeArgOp::DirectValue:
    case NativeArgOp::ZeroIfType3AndNotC:
    default:
      packed = rawValue;
      break;
    }

    t_callArgs[i] = packed;
  }

  ret = InvokeCdeclPacked(plan.funcPtr, t_callArgs.data(), plan.paramCount);

  if (!dxvk::war3::IsReadableRange(
          reinterpret_cast<const uint8_t *>(stackPtr) + kStackTopOffset,
          sizeof(uint32_t))) {
    return false;
  }
  *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(stackPtr) +
                                kStackTopOffset) = stackTop - plan.paramCount;
  return true;
}

} // namespace dxvk::war3::hooks
