// war3_perf_monitor.cpp - War3 性能监控器实现（增强版）

#include "war3_perf_monitor.h"
#include "war3_perf_report_template.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <functional>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include "../hooks/war3_hook_lifecycle.h"
#include "../hooks/war3_hook_install_util.h"
#include "../hooks/war3_hook_render.h"
#include "../core/war3_runtime_profile.h"
#include "../model/war3_model_registry.h"
#include "../render/war3_shadow_object_registry.h"
#include "../render/war3_shadow_lifecycle.h"
#include "../render/war3_shadow_producer_policy.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "../shadow/war3_shadow_alpha_test_payload.h"
#include "../../util/util_env.h"
namespace dxvk::war3 {

namespace {

double GpuTimestampIntervalUnionMs(
    const std::vector<std::array<uint64_t, 2>>& intervals,
    double timestampPeriodNs) {
  if (intervals.empty() || timestampPeriodNs <= 0.0)
    return 0.0;

  std::vector<std::array<uint64_t, 2>> sorted;
  sorted.reserve(intervals.size());
  for (const auto& interval : intervals) {
    if (interval[1] > interval[0])
      sorted.push_back(interval);
  }
  if (sorted.empty())
    return 0.0;

  std::sort(sorted.begin(), sorted.end(),
      [](const auto& a, const auto& b) {
        return a[0] < b[0] || (a[0] == b[0] && a[1] < b[1]);
      });

  uint64_t unionTicks = 0u;
  uint64_t begin = sorted.front()[0];
  uint64_t end = sorted.front()[1];
  for (size_t i = 1u; i < sorted.size(); ++i) {
    if (sorted[i][0] <= end) {
      end = std::max(end, sorted[i][1]);
      continue;
    }
    unionTicks += end - begin;
    begin = sorted[i][0];
    end = sorted[i][1];
  }
  unionTicks += end - begin;
  return double(unionTicks) * timestampPeriodNs / 1.0e6;
}

} // namespace

using Clock = std::chrono::steady_clock;

static double toMs(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

static const char* gpuSkinModeName(gpu_skin::GpuSkinMode mode) {
  switch (mode) {
    case gpu_skin::GpuSkinMode::Disabled: return "disabled";
    case gpu_skin::GpuSkinMode::Observe:  return "observe";
    case gpu_skin::GpuSkinMode::Dual:     return "dual";
    case gpu_skin::GpuSkinMode::Shadow:   return "shadow";
    case gpu_skin::GpuSkinMode::Main:     return "main";
    case gpu_skin::GpuSkinMode::Bypass:   return "bypass";
  }
  return "unknown";
}

static const char* worldObjectsPhase1ReasonName(
    render::WorldObjectsPhase1TrackingReason reason) {
  using Reason = render::WorldObjectsPhase1TrackingReason;
  switch (reason) {
    case Reason::None: return "none";
    case Reason::ColdBootstrap: return "coldBootstrap";
    case Reason::Warmup: return "warmup";
    case Reason::PeriodicMaintenance: return "periodicMaintenance";
    case Reason::RepairBurst: return "repairBurst";
    case Reason::RuntimeChainRepair: return "runtimeChainRepair";
    case Reason::Count: break;
  }
  return "unknown";
}

static const char* worldObjectsPhase1OutcomeName(
    render::WorldObjectsPhase1CollectorOutcome outcome) {
  using Outcome = render::WorldObjectsPhase1CollectorOutcome;
  switch (outcome) {
    case Outcome::Unclassified: return "unclassified";
    case Outcome::ValidNonEmpty: return "validNonEmpty";
    case Outcome::ValidEmptyAfterFilter: return "validEmptyAfterFilter";
    case Outcome::NullWorld: return "nullWorld";
    case Outcome::InvalidGroup: return "invalidGroup";
    case Outcome::UnreadableWorld: return "unreadableWorld";
    case Outcome::NullList: return "nullList";
    case Outcome::UnreadableList: return "unreadableList";
    case Outcome::NullData: return "nullData";
    case Outcome::CountZero: return "countZero";
    case Outcome::CountCap: return "countCap";
    case Outcome::UnreadableData: return "unreadableData";
    case Outcome::FilteredNoTargets: return "filteredNoTargets";
    case Outcome::Count: break;
  }
  return "unknown";
}

static bool startsWith(const std::string &text, const char *prefix) {
  return text.rfind(prefix, 0) == 0;
}

static bool endsWith(const std::string &text, const char *suffix) {
  if (!suffix)
    return false;
  const size_t suffixLen = std::strlen(suffix);
  if (suffixLen == 0 || text.size() < suffixLen)
    return false;
  return text.compare(text.size() - suffixLen, suffixLen, suffix) == 0;
}

// 判定“空转/等待”段：用于将主循环等待时间与有效计算时间分离展示。
static bool isIdleWaitSectionPath(const std::string &path) {
  if (path == "War3MainLoop/Engine/WaitGate")
    return true;
  if (path == "War3MainLoop/Engine/SleepGate")
    return true;
  if (path == "War3MainLoop/Engine/SleepGateInner")
    return true;
  if (startsWith(path, "War3MainLoop/Wait/"))
    return true;
  return false;
}

// 覆盖型诊断树会对已经属于动态调用树的同一段 CPU 再做正交归因。
// 它们必须继续显示在热点/调用树中，但不能再次进入 root coverage、
// untracked 或 thread-lane 的加法预算，否则会把同一时间算两遍。
static bool isNonAdditiveOverlaySectionPath(const std::string& path) {
  return path == "ShadowCapture" || startsWith(path, "ShadowCapture/") ||
         path == "FramePipeline" || startsWith(path, "FramePipeline/") ||
         // FlushMainLoopCycleToPerf 回填的这些节点是正交阶段聚合，
         // 不是 TLS scope。若把它们当 root，会与真实 Hook scope 重叠，
         // 从而破坏“同一主线程 root 区间互斥”的 CPU 上界前提。
         path == "War3MainLoop/Dispatch" ||
         startsWith(path, "War3MainLoop/Dispatch/") ||
         path == "War3MainLoop/DispatchModule" ||
         startsWith(path, "War3MainLoop/DispatchModule/") ||
         path == "War3MainLoop/Engine/RunCallbacks" ||
         startsWith(path, "War3MainLoop/Engine/RunCallbacks/") ||
         path == "War3MainLoop/Engine/PrepareDispatch" ||
         startsWith(path, "War3MainLoop/Engine/PrepareDispatch/") ||
         path == "War3MainLoop/Engine/TickUpdate" ||
         startsWith(path, "War3MainLoop/Engine/TickUpdate/");
}

// “未覆盖帧墙钟”是 Present 帧墙钟减去 additive root scopes 的差值。
// 它不是一次函数调用、不是线程 CPU，也不能进入 CPU 热点或 root coverage
// 的分母。单独标识以便报告前端把它作为预算缺口而非优化热点展示。
static bool isUncoveredFrameWallSectionPath(const std::string& path) {
  return path == "Other/UncoveredFrameWall" ||
         startsWith(path, "Other/UncoveredFrameWall/");
}

struct FrameStateCohortSummary {
  uint32_t frameCount = 0;
  double avgFrameWallMs = 0.0;
  double avgGpuMs = 0.0;
  double avgProcessCpuMs = 0.0;
  double avgMainCpuMs = 0.0;
  double avgWorkerCpuMs = 0.0;
  double processCpuPerWallCore = 0.0;
  uint32_t gpuSamples = 0;
  uint32_t processCpuSamples = 0;
  uint32_t mainCpuSamples = 0;
};

struct FrameStateRunSummary {
  uint32_t runCount = 0;
  uint32_t transitionCount = 0;
  uint32_t maxFastRunFrames = 0;
  uint32_t maxSlowRunFrames = 0;
  double maxFastRunWallMs = 0.0;
  double maxSlowRunWallMs = 0.0;
};

struct FrameStateAnalysisResult {
  bool valid = false;
  double winsorLowMs = 0.0;
  double winsorHighMs = 0.0;
  double fastCenterMs = 0.0;
  double slowCenterMs = 0.0;
  double thresholdMs = 0.0;
  double relativeFrameWallDeltaPct = 0.0;
  double relativeCentroidDeltaPct = 0.0;
  double pooledWithinStddevMs = 0.0;
  std::vector<int8_t> stateByFrame;
  double rollingWindowTargetMs = 1000.0;
  double rollingHysteresisHalfBandMs = 0.0;
  double rollingMeanMinMs = 0.0;
  double rollingMeanMaxMs = 0.0;
  std::vector<double> rollingMeanByFrame;
  std::vector<int8_t> rollingStateByFrame;
  bool rollingStableValid = false;
  std::vector<int8_t> rollingStableStateByFrame;
  FrameStateRunSummary instantaneousRuns;
  FrameStateRunSummary rollingRuns;
  FrameStateCohortSummary fast;
  FrameStateCohortSummary slow;
  FrameStateCohortSummary rollingStableFast;
  FrameStateCohortSummary rollingStableSlow;
};

static double sortedQuantile(const std::vector<double>& sorted, double q) {
  if (sorted.empty())
    return 0.0;
  q = std::clamp(q, 0.0, 1.0);
  const double pos = q * static_cast<double>(sorted.size() - 1u);
  const size_t lo = static_cast<size_t>(std::floor(pos));
  const size_t hi = std::min(lo + 1u, sorted.size() - 1u);
  const double fraction = pos - static_cast<double>(lo);
  return sorted[lo] * (1.0 - fraction) + sorted[hi] * fraction;
}

static FrameStateRunSummary summarizeFrameStateRuns(
    const std::vector<int8_t>& states,
    const std::vector<const FrameSnapshot*>& frames) {
  FrameStateRunSummary result;
  int activeState = -1;
  uint32_t activeFrames = 0u;
  double activeWallMs = 0.0;

  auto finishRun = [&]() {
    if (activeState < 0 || activeFrames == 0u)
      return;
    ++result.runCount;
    if (activeState == 0) {
      result.maxFastRunFrames =
          std::max(result.maxFastRunFrames, activeFrames);
      result.maxFastRunWallMs =
          std::max(result.maxFastRunWallMs, activeWallMs);
    } else {
      result.maxSlowRunFrames =
          std::max(result.maxSlowRunFrames, activeFrames);
      result.maxSlowRunWallMs =
          std::max(result.maxSlowRunWallMs, activeWallMs);
    }
  };

  const size_t count = std::min(states.size(), frames.size());
  for (size_t i = 0u; i < count; ++i) {
    const int state = static_cast<int>(states[i]);
    const FrameSnapshot* frame = frames[i];
    if ((state != 0 && state != 1) || frame == nullptr) {
      finishRun();
      activeState = -1;
      activeFrames = 0u;
      activeWallMs = 0.0;
      continue;
    }
    if (activeState != state) {
      if (activeState >= 0) {
        finishRun();
        ++result.transitionCount;
      }
      activeState = state;
      activeFrames = 0u;
      activeWallMs = 0.0;
    }
    ++activeFrames;
    activeWallMs += std::max(0.0, frame->totalCpuMs);
  }
  finishRun();
  return result;
}

struct FrameStateCohortPair {
  FrameStateCohortSummary fast;
  FrameStateCohortSummary slow;
};

static FrameStateCohortPair summarizeFrameStateCohorts(
    const std::vector<int8_t>& states,
    const std::vector<const FrameSnapshot*>& frames) {
  struct CohortAccumulator {
    double frameWall = 0.0;
    double gpu = 0.0;
    double process = 0.0;
    double processMatchedWall = 0.0;
    double main = 0.0;
    double worker = 0.0;
    uint32_t frames = 0u;
    uint32_t gpuSamples = 0u;
    uint32_t processSamples = 0u;
    uint32_t mainSamples = 0u;
  };

  CohortAccumulator accumulators[2] = {};
  const size_t count = std::min(states.size(), frames.size());
  for (size_t i = 0u; i < count; ++i) {
    const int state = static_cast<int>(states[i]);
    const FrameSnapshot* frame = frames[i];
    if ((state != 0 && state != 1) || frame == nullptr)
      continue;
    auto& acc = accumulators[state];
    ++acc.frames;
    acc.frameWall += frame->totalCpuMs;
    if (frame->hasGpuTiming) {
      ++acc.gpuSamples;
      acc.gpu += frame->totalGpuMs;
    }
    if (frame->hasProcessCpu) {
      ++acc.processSamples;
      acc.process += frame->processCpuMs;
      acc.worker += frame->workerThreadsCpuMs;
      acc.processMatchedWall += frame->totalCpuMs;
    }
    if (frame->hasMainThreadCpu) {
      ++acc.mainSamples;
      acc.main += frame->mainThreadCpuMs;
    }
  }

  auto finishCohort = [](const CohortAccumulator& acc) {
    FrameStateCohortSummary cohort;
    cohort.frameCount = acc.frames;
    cohort.gpuSamples = acc.gpuSamples;
    cohort.processCpuSamples = acc.processSamples;
    cohort.mainCpuSamples = acc.mainSamples;
    if (acc.frames != 0u) {
      cohort.avgFrameWallMs =
          acc.frameWall / static_cast<double>(acc.frames);
    }
    if (acc.gpuSamples != 0u) {
      cohort.avgGpuMs =
          acc.gpu / static_cast<double>(acc.gpuSamples);
    }
    if (acc.processSamples != 0u) {
      const double processDenominator =
          static_cast<double>(acc.processSamples);
      cohort.avgProcessCpuMs = acc.process / processDenominator;
      cohort.avgWorkerCpuMs = acc.worker / processDenominator;
      cohort.processCpuPerWallCore =
          acc.processMatchedWall > 1e-9
              ? acc.process / acc.processMatchedWall
              : 0.0;
    }
    if (acc.mainSamples != 0u) {
      cohort.avgMainCpuMs =
          acc.main / static_cast<double>(acc.mainSamples);
    }
    return cohort;
  };

  FrameStateCohortPair result;
  result.fast = finishCohort(accumulators[0]);
  result.slow = finishCohort(accumulators[1]);
  return result;
}

// This runs only on the export worker's immutable snapshot. Winsorization
// prevents isolated loading/jank frames from pulling the slow centroid while
// still preserving a sustained 9 ms / 11 ms style state transition.
static FrameStateAnalysisResult analyzeFrameStates(
    const std::vector<const FrameSnapshot*>& frames) {
  FrameStateAnalysisResult result;
  result.stateByFrame.assign(frames.size(), int8_t(-1));
  if (frames.size() < 32u)
    return result;

  std::vector<double> sorted;
  sorted.reserve(frames.size());
  for (const FrameSnapshot* frame : frames) {
    if (frame && std::isfinite(frame->totalCpuMs) &&
        frame->totalCpuMs > 0.0) {
      sorted.push_back(frame->totalCpuMs);
    }
  }
  if (sorted.size() < 32u)
    return result;
  std::sort(sorted.begin(), sorted.end());

  result.winsorLowMs = sortedQuantile(sorted, 0.05);
  result.winsorHighMs = sortedQuantile(sorted, 0.95);
  double fastCenter = sortedQuantile(sorted, 0.25);
  double slowCenter = sortedQuantile(sorted, 0.75);
  if (!(slowCenter > fastCenter))
    return result;

  uint32_t fitFastCount = 0u;
  uint32_t fitSlowCount = 0u;
  for (uint32_t iteration = 0u; iteration < 32u; ++iteration) {
    const double threshold = (fastCenter + slowCenter) * 0.5;
    double fastSum = 0.0;
    double slowSum = 0.0;
    fitFastCount = 0u;
    fitSlowCount = 0u;
    for (double sample : sorted) {
      const double value =
          std::clamp(sample, result.winsorLowMs, result.winsorHighMs);
      if (value <= threshold) {
        fastSum += value;
        ++fitFastCount;
      } else {
        slowSum += value;
        ++fitSlowCount;
      }
    }
    if (fitFastCount == 0u || fitSlowCount == 0u)
      return result;
    const double nextFast = fastSum / static_cast<double>(fitFastCount);
    const double nextSlow = slowSum / static_cast<double>(fitSlowCount);
    const double movement =
        std::abs(nextFast - fastCenter) + std::abs(nextSlow - slowCenter);
    fastCenter = nextFast;
    slowCenter = nextSlow;
    if (movement < 1e-6)
      break;
  }

  result.fastCenterMs = fastCenter;
  result.slowCenterMs = slowCenter;
  result.thresholdMs = (fastCenter + slowCenter) * 0.5;

  double squaredResidual = 0.0;
  for (double sample : sorted) {
    const double value =
        std::clamp(sample, result.winsorLowMs, result.winsorHighMs);
    const double center =
        value <= result.thresholdMs ? fastCenter : slowCenter;
    const double residual = value - center;
    squaredResidual += residual * residual;
  }
  result.pooledWithinStddevMs =
      std::sqrt(squaredResidual / static_cast<double>(sorted.size()));

  const uint32_t minimumCohort =
      std::max<uint32_t>(8u, static_cast<uint32_t>(sorted.size() / 10u));
  const double separation = slowCenter - fastCenter;
  const double minimumSeparation =
      std::max({0.15, sortedQuantile(sorted, 0.50) * 0.015,
                result.pooledWithinStddevMs * 0.65});
  if (fitFastCount < minimumCohort || fitSlowCount < minimumCohort ||
      separation < minimumSeparation) {
    return result;
  }

  result.valid = true;
  result.relativeCentroidDeltaPct =
      fastCenter > 1e-9 ? (separation / fastCenter * 100.0) : 0.0;

  for (size_t i = 0u; i < frames.size(); ++i) {
    const FrameSnapshot* frame = frames[i];
    if (!frame || !std::isfinite(frame->totalCpuMs) ||
        frame->totalCpuMs <= 0.0) {
      continue;
    }
    const int state = frame->totalCpuMs <= result.thresholdMs ? 0 : 1;
    result.stateByFrame[i] = static_cast<int8_t>(state);
  }

  const FrameStateCohortPair instantaneousCohorts =
      summarizeFrameStateCohorts(result.stateByFrame, frames);
  result.fast = instantaneousCohorts.fast;
  result.slow = instantaneousCohorts.slow;
  result.relativeFrameWallDeltaPct =
      result.fast.avgFrameWallMs > 1e-9
          ? (result.slow.avgFrameWallMs - result.fast.avgFrameWallMs) /
                result.fast.avgFrameWallMs * 100.0
          : 0.0;

  result.instantaneousRuns =
      summarizeFrameStateRuns(result.stateByFrame, frames);

  // A second, deliberately slower view answers a different question: whether
  // instantaneous fast/slow samples persist for roughly seconds. The rolling
  // mean spans about one second of frame wall, and hysteresis prevents values
  // near the fitted threshold from creating artificial state chatter. This is
  // still descriptive correlation, not proof of an engine mode switch.
  result.rollingMeanByFrame.assign(frames.size(), -1.0);
  result.rollingStateByFrame.assign(frames.size(), int8_t(-1));
  result.rollingHysteresisHalfBandMs =
      std::max(0.05, separation * 0.10);
  size_t rollingLeft = 0u;
  double rollingWallMs = 0.0;
  uint32_t rollingFrameCount = 0u;
  int rollingState = -1;
  bool haveRollingRange = false;
  for (size_t i = 0u; i < frames.size(); ++i) {
    const FrameSnapshot* frame = frames[i];
    if (frame == nullptr || !std::isfinite(frame->totalCpuMs) ||
        frame->totalCpuMs <= 0.0) {
      rollingLeft = i + 1u;
      rollingWallMs = 0.0;
      rollingFrameCount = 0u;
      rollingState = -1;
      continue;
    }

    rollingWallMs += frame->totalCpuMs;
    ++rollingFrameCount;
    while (rollingLeft < i) {
      const FrameSnapshot* oldest = frames[rollingLeft];
      if (oldest == nullptr || !std::isfinite(oldest->totalCpuMs) ||
          oldest->totalCpuMs <= 0.0) {
        ++rollingLeft;
        continue;
      }
      if (rollingWallMs - oldest->totalCpuMs <
          result.rollingWindowTargetMs) {
        break;
      }
      rollingWallMs -= oldest->totalCpuMs;
      --rollingFrameCount;
      ++rollingLeft;
    }

    if (rollingWallMs < result.rollingWindowTargetMs * 0.80 ||
        rollingFrameCount == 0u) {
      continue;
    }

    const double rollingMean =
        rollingWallMs / static_cast<double>(rollingFrameCount);
    result.rollingMeanByFrame[i] = rollingMean;
    if (!haveRollingRange) {
      result.rollingMeanMinMs = rollingMean;
      result.rollingMeanMaxMs = rollingMean;
      haveRollingRange = true;
    } else {
      result.rollingMeanMinMs =
          std::min(result.rollingMeanMinMs, rollingMean);
      result.rollingMeanMaxMs =
          std::max(result.rollingMeanMaxMs, rollingMean);
    }

    if (rollingState < 0) {
      rollingState = rollingMean <= result.thresholdMs ? 0 : 1;
    } else if (rollingState == 0 &&
               rollingMean >
                   result.thresholdMs +
                       result.rollingHysteresisHalfBandMs) {
      rollingState = 1;
    } else if (rollingState == 1 &&
               rollingMean <
                   result.thresholdMs -
                       result.rollingHysteresisHalfBandMs) {
      rollingState = 0;
    }
    result.rollingStateByFrame[i] = static_cast<int8_t>(rollingState);
  }
  result.rollingRuns =
      summarizeFrameStateRuns(result.rollingStateByFrame, frames);

  // The rolling label is intentionally sticky inside the hysteresis band.
  // For correlation, however, those ambiguous/deadband frames would dilute
  // both sides. rollingStable keeps only frames whose rolling mean is on the
  // matching outer side of threshold±band; all deadband frames remain -1.
  result.rollingStableStateByFrame.assign(frames.size(), int8_t(-1));
  const double stableFastBoundary =
      result.thresholdMs - result.rollingHysteresisHalfBandMs;
  const double stableSlowBoundary =
      result.thresholdMs + result.rollingHysteresisHalfBandMs;
  for (size_t i = 0u; i < frames.size(); ++i) {
    if (i >= result.rollingMeanByFrame.size() ||
        i >= result.rollingStateByFrame.size()) {
      continue;
    }
    const double rollingMean = result.rollingMeanByFrame[i];
    const int state = static_cast<int>(result.rollingStateByFrame[i]);
    if (rollingMean < 0.0)
      continue;
    if (state == 0 && rollingMean < stableFastBoundary) {
      result.rollingStableStateByFrame[i] = int8_t(0);
    } else if (state == 1 && rollingMean > stableSlowBoundary) {
      result.rollingStableStateByFrame[i] = int8_t(1);
    }
  }
  const FrameStateCohortPair stableCohorts =
      summarizeFrameStateCohorts(result.rollingStableStateByFrame, frames);
  result.rollingStableFast = stableCohorts.fast;
  result.rollingStableSlow = stableCohorts.slow;
  result.rollingStableValid =
      result.rollingStableFast.frameCount >= 8u &&
      result.rollingStableSlow.frameCount >= 8u;
  return result;
}

static std::string getWarVkBaseDir() {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) <= 0)
    return {};
  std::string exeDir(exePath);
  size_t lastSlash = exeDir.find_last_of("\\/");
  if (lastSlash == std::string::npos)
    return {};
  exeDir = exeDir.substr(0, lastSlash + 1);
  return exeDir + "WarVK\\";
}

static uint64_t combineFileTimes100ns(const FILETIME& kernel,
                                      const FILETIME& user) {
  ULARGE_INTEGER k = {};
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  ULARGE_INTEGER u = {};
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  return k.QuadPart + u.QuadPart;
}

static void decrementPendingJobs(std::atomic<uint32_t>& value) {
  uint32_t current = value.load(std::memory_order_relaxed);
  while (current > 0 &&
         !value.compare_exchange_weak(current, current - 1,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
  }
}

// ============================================================================
// ScopedSection
// ============================================================================

War3PerfMonitor::ScopedSection::ScopedSection(War3PerfMonitor *monitor,
                                              uint32_t id,
                                              const Rc<DxvkCommandList> &ctx)
    : m_monitor(monitor), m_id(id), m_cpuStart(Clock::now()), m_ctx(ctx) {
  if (m_monitor) {
    m_frameEpoch = m_monitor->m_activeFrameEpoch.load(std::memory_order_acquire);
    m_gpuStart = m_monitor->writeTimestamp(ctx);
  }
}

War3PerfMonitor::ScopedSection::~ScopedSection() {
  if (!m_monitor)
    return;

  const auto cpuEnd = Clock::now();
  const double cpuMs = toMs(cpuEnd - m_cpuStart);
  Rc<DxvkGpuQuery> gpuEnd = m_monitor->writeTimestamp(m_ctx);
  m_monitor->submitSample(m_id, m_frameEpoch, cpuMs, std::move(m_gpuStart),
                          std::move(gpuEnd));
}

War3PerfMonitor::ScopedSection::ScopedSection(ScopedSection &&other) noexcept
    : m_monitor(other.m_monitor), m_id(other.m_id),
      m_frameEpoch(other.m_frameEpoch), m_cpuStart(other.m_cpuStart), m_gpuStart(std::move(other.m_gpuStart)),
      m_ctx(std::move(other.m_ctx)) {
  other.m_monitor = nullptr;
}

War3PerfMonitor::ScopedSection &
War3PerfMonitor::ScopedSection::operator=(ScopedSection &&other) noexcept {
  if (this != &other) {
    m_monitor = other.m_monitor;
    m_id = other.m_id;
    m_frameEpoch = other.m_frameEpoch;
    m_cpuStart = other.m_cpuStart;
    m_gpuStart = std::move(other.m_gpuStart);
    m_ctx = std::move(other.m_ctx);
    other.m_monitor = nullptr;
  }
  return *this;
}

// ============================================================================
// FrameGuard
// ============================================================================

War3PerfMonitor::FrameGuard::FrameGuard(War3PerfMonitor *monitor)
    : m_monitor(monitor) {
  if (m_monitor) {
    m_monitor->beginFrame();
  }
}

War3PerfMonitor::FrameGuard::~FrameGuard() {
  if (m_monitor) {
    m_monitor->endFrame();
  }
}

// ============================================================================
// ScopedCpuScope
// ============================================================================

War3PerfMonitor::ScopedCpuScope::ScopedCpuScope(War3PerfMonitor *monitor,
                                                const char *name,
                                                uint32_t sampleWeight,
                                                bool detailSample)
    : m_monitor(monitor), m_sampleWeight(sampleWeight),
      m_detailSample(detailSample) {
  if (m_monitor) {
    m_monitor->pushScope(name, m_sampleWeight, m_detailSample);
  }
}

War3PerfMonitor::ScopedCpuScope::~ScopedCpuScope() {
  if (m_monitor) {
    m_monitor->popScope(m_sampleWeight);
  }
}

War3PerfMonitor::ScopedCpuScope::ScopedCpuScope(ScopedCpuScope &&other) noexcept
    : m_monitor(other.m_monitor), m_sampleWeight(other.m_sampleWeight),
      m_detailSample(other.m_detailSample) {
  other.m_monitor = nullptr;
}

War3PerfMonitor::ScopedCpuScope &
War3PerfMonitor::ScopedCpuScope::operator=(ScopedCpuScope &&other) noexcept {
  if (this != &other) {
    // Move-assignment transfers ownership just like replacing any other RAII
    // guard: release the scope currently owned by *this before overwriting its
    // monitor pointer. Without this, `scope = ScopedCpuScope{}` leaks one
    // pushScope entry and eventually corrupts the per-thread scope stack.
    if (m_monitor)
      m_monitor->popScope(m_sampleWeight);
    m_monitor = other.m_monitor;
    m_sampleWeight = other.m_sampleWeight;
    m_detailSample = other.m_detailSample;
    other.m_monitor = nullptr;
  }
  return *this;
}

// ============================================================================
// War3PerfMonitor 核心
// ============================================================================

namespace {

// 紧凑 SHA-256（仅用于报告元数据标识构建产物，无外部依赖）。
struct Sha256Ctx {
  uint32_t state[8];
  uint64_t bitLen = 0;
  uint8_t buffer[64] = {};
  size_t bufferLen = 0;
};

constexpr uint32_t kSha256RoundK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t Sha256Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

void Sha256Compress(Sha256Ctx &c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
           (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Sha256Rotr(w[i - 15], 7) ^ Sha256Rotr(w[i - 15], 18) ^
                        (w[i - 15] >> 3);
    const uint32_t s1 = Sha256Rotr(w[i - 2], 17) ^ Sha256Rotr(w[i - 2], 19) ^
                        (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
  uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Sha256Rotr(e, 6) ^ Sha256Rotr(e, 11) ^ Sha256Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kSha256RoundK[i] + w[i];
    const uint32_t s0 = Sha256Rotr(a, 2) ^ Sha256Rotr(a, 13) ^ Sha256Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    const uint32_t t2 = s0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c.state[0] += a; c.state[1] += b; c.state[2] += cc; c.state[3] += d;
  c.state[4] += e; c.state[5] += f; c.state[6] += g; c.state[7] += h;
}

void Sha256Init(Sha256Ctx &c) {
  c.state[0] = 0x6a09e667u; c.state[1] = 0xbb67ae85u;
  c.state[2] = 0x3c6ef372u; c.state[3] = 0xa54ff53au;
  c.state[4] = 0x510e527fu; c.state[5] = 0x9b05688cu;
  c.state[6] = 0x1f83d9abu; c.state[7] = 0x5be0cd19u;
  c.bitLen = 0;
  c.bufferLen = 0;
}

void Sha256Update(Sha256Ctx &c, const uint8_t *data, size_t len) {
  c.bitLen += uint64_t(len) * 8u;
  while (len > 0) {
    const size_t take = std::min(len, 64 - c.bufferLen);
    std::memcpy(c.buffer + c.bufferLen, data, take);
    c.bufferLen += take;
    data += take;
    len -= take;
    if (c.bufferLen == 64) {
      Sha256Compress(c, c.buffer);
      c.bufferLen = 0;
    }
  }
}

void Sha256Final(Sha256Ctx &c, uint8_t (&out)[32]) {
  c.buffer[c.bufferLen++] = 0x80u;
  if (c.bufferLen > 56) {
    while (c.bufferLen < 64)
      c.buffer[c.bufferLen++] = 0;
    Sha256Compress(c, c.buffer);
    c.bufferLen = 0;
  }
  while (c.bufferLen < 56)
    c.buffer[c.bufferLen++] = 0;
  for (int i = 7; i >= 0; --i)
    c.buffer[c.bufferLen++] = uint8_t(c.bitLen >> (i * 8));
  Sha256Compress(c, c.buffer);
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = uint8_t(c.state[i] >> 24);
    out[i * 4 + 1] = uint8_t(c.state[i] >> 16);
    out[i * 4 + 2] = uint8_t(c.state[i] >> 8);
    out[i * 4 + 3] = uint8_t(c.state[i]);
  }
}

// 计算当前模块（d3d9.dll）的 SHA-256 与文件大小，进程内只算一次。
const std::pair<std::string, uint64_t> &GetOwnModuleIdentity() {
  static const std::pair<std::string, uint64_t> identity = [] {
    std::pair<std::string, uint64_t> result{"", 0};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModuleIdentity), &self)) {
      return result;
    }
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0)
      return result;
    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return result;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size))
      result.second = static_cast<uint64_t>(size.QuadPart);
    Sha256Ctx ctx;
    Sha256Init(ctx);
    uint8_t chunk[65536];
    DWORD readBytes = 0;
    while (ReadFile(file, chunk, sizeof(chunk), &readBytes, nullptr) &&
           readBytes > 0) {
      Sha256Update(ctx, chunk, readBytes);
    }
    CloseHandle(file);
    uint8_t digest[32];
    Sha256Final(ctx, digest);
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(64);
    for (uint8_t b : digest) {
      hex.push_back(kHex[b >> 4]);
      hex.push_back(kHex[b & 0xF]);
    }
    result.first = std::move(hex);
    return result;
  }();
  return identity;
}

// 记录到报告里的关键环境变量（schema v9 meta.env）。
std::string BuildPerfEnvJson() {
  static constexpr const char *kNames[] = {
      "DXVK_WAR3_DISABLE",
      "DXVK_WAR3_PROFILE",
      "DXVK_WAR3_GPU_SKIN_MODE",
      "DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE",
      "DXVK_WAR3_PERF_MONITOR",
      "DXVK_WAR3_PERF_LEVEL",
      "DXVK_WAR3_PERF_TRACE",
      "DXVK_WAR3_PERF_SPRITE_FRAME_HOOKS",
      "DXVK_WAR3_PERF_SPRITE_NATIVE_BREAKDOWN_HOOKS",
      "DXVK_WAR3_PERF_OVERRIDE_GRAPH_BREAKDOWN_HOOKS",
      "DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS",
      "DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS",
      "DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS",
      "DXVK_WAR3_PERF_SHADOW_PHASE_BREAKDOWN",
      "DXVK_WAR3_PERF_SHADOW_PHASE_SAMPLE_PERIOD",
      "DXVK_WAR3_CSM_DESCRIPTOR_REUSE",
      "DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY",
      "DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY_ASSERT",
      "DXVK_WAR3_SHADOW_GATE_BREAKDOWN",
      "DXVK_WAR3_SHADOW_GATE_TRACE_PERIOD",
      "DXVK_WAR3_SHADOW_DRAWTIME_BREAKDOWN",
      "DXVK_WAR3_DRAWTIME_VB_CACHE",
      "DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY",
      "DXVK_WAR3_SHADOW_METADATA_CAPTURE",
      "DXVK_WAR3_SHADOW_METADATA_ALPHA",
      "DXVK_WAR3_SHADOW_METADATA_BLOCKER",
      "DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE",
      "DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE_VERIFY",
      "DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE",
      "DXVK_WAR3_SEMANTIC_DRAW_TIME_DIRECT_PRODUCER",
      "DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND",
      "DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS",
      "DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE",
      "DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE_VERIFY",
      "DXVK_WAR3_SEMANTIC_DYNAMIC_EVIDENCE_STATS",
      "DXVK_WAR3_SHADOW_POSE_FULL_TRACE",
      "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTERS",
      "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CASTERS",
      "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES",
      "DXVK_WAR3_S1_PERSISTENT_BORROW_STATIC",
      "DXVK_WAR3_S1_PERSISTENT_UNSTABLE_SOURCE",
      "DXVK_WAR3_S1_EARLY_FALLBACK_BACKING",
      "DXVK_WAR3_PERF_PUBLISH_VISIBLE_BREAKDOWN",
      "DXVK_WAR3_PERF_PUBLISH_VISIBLE_SAMPLE_PERIOD",
      "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY",
      "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY",
      "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_ASSERT",
      "DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_SAMPLE_PERIOD",
      "DXVK_WAR3_SHADOW_CAPTURE_BREAKDOWN",
      "DXVK_WAR3_SHADOW_CAPTURE_TRACE_PERIOD",
      "DXVK_WAR3_NATIVE_HINT_PRODUCERLESS_SKIP",
      "DXVK_WAR3_SEMANTIC_DIRECT_PHASE_BREAKDOWN",
      "DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_BREAKDOWN",
      "DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_TRACE_PERIOD",
      "DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_BREAKDOWN",
      "DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_TRACE_PERIOD",
      "DXVK_WAR3_PERF_CURRENT_DRAW_BREAKDOWN",
      "DXVK_WAR3_CURRENT_DRAW_REDUNDANT_ATOMICS",
      "DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE",
      "DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE_STATS",
      "DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER",
      "DXVK_WAR3_REGISTRY_HEALTH_VERIFY",
      "DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT",
      "DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY",
      "DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY_ASSERT",
      "DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH",
      "DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY",
      "DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY_ASSERT",
      "DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE",
      "DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY",
      "DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY_ASSERT",
      "DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX",
      "DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY",
      "DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY_ASSERT",
      "DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VIEW",
      "DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY",
      "DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY_ASSERT",
      "DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE",
      "DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES",
      "DXVK_WAR3_PERF_RECORD_ON_START",
      "DXVK_WAR3_WORKER_PREPARE",
      "DXVK_WAR3_SHADOW_TAA",
      "DXVK_WAR3_SHADOW_TAA_MODE",
      "DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT",
      "DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC",
      "DXVK_WAR3_SHADOW_TAA_DISABLE_ON_SUN_MOTION",
      "DXVK_WAR3_SHADOW_STAGE_LIFECYCLE",
      "DXVK_WAR3_SHADOW_STAGE_HISTOGRAM",
      "DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE",
      "DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE",
      "DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW",
      "DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP",
  };
  auto escape = [](const std::string &v) {
    std::string out;
    out.reserve(v.size());
    for (const char ch : v) {
      if (ch == '\\' || ch == '"')
        out.push_back('\\');
      out.push_back(ch);
    }
    return out;
  };
  std::ostringstream s;
  s << "{";
  bool first = true;
  for (const char *name : kNames) {
    const std::string value = env::getEnvVar(name);
    if (value.empty())
      continue;
    if (!first)
      s << ", ";
    first = false;
    s << "\"" << name << "\": \"" << escape(value) << "\"";
  }
  s << "}";
  return s.str();
}

} // namespace

War3PerfMonitor &War3PerfMonitor::instance() {
  static War3PerfMonitor s_instance;
  return s_instance;
}

War3PerfMonitor::War3PerfMonitor()
    : m_lastReport(Clock::now()), m_lastAutoExport(Clock::now()) {
  const std::string historyFrames =
      env::getEnvVar("DXVK_WAR3_PERF_HISTORY_FRAMES");
  if (!historyFrames.empty()) {
    const int frames = std::max(0, std::atoi(historyFrames.c_str()));
    if (frames > 0)
      m_maxHistorySize = static_cast<size_t>(frames);
  } else {
    const std::string historySec = env::getEnvVar("DXVK_WAR3_PERF_HISTORY_SEC");
    if (!historySec.empty()) {
      const int sec = std::max(0, std::atoi(historySec.c_str()));
      if (sec > 0) {
        const int maxFrames = std::clamp(sec * 120, 600, 200000);
        m_maxHistorySize = static_cast<size_t>(maxFrames);
      }
    }
  }

  const std::string pendingMax = env::getEnvVar("DXVK_WAR3_PERF_PENDING_MAX");
  if (!pendingMax.empty()) {
    const int maxPending = std::max(0, std::atoi(pendingMax.c_str()));
    if (maxPending > 0) {
      m_maxPendingSamples = static_cast<size_t>(maxPending);
    }
  }

  // 全局主开关：=0 时整个性能监控系统关闭（所有 scope/采样入口均为空操作），
  // 用于量化监控自身的运行时开销（monitor on/off A/B）。
  const std::string monitorEnabled =
      env::getEnvVar("DXVK_WAR3_PERF_MONITOR");
  if (!monitorEnabled.empty() &&
      std::strtoul(monitorEnabled.c_str(), nullptr, 10) == 0) {
    m_enabled.store(false, std::memory_order_relaxed);
  }

  // 自动录制开关：便于无人值守压测/自动化回归。
  const std::string autoRecord =
      env::getEnvVar("DXVK_WAR3_PERF_RECORD_ON_START");
  if (!autoRecord.empty() &&
      std::strtoul(autoRecord.c_str(), nullptr, 10) != 0) {
    m_recording.store(true, std::memory_order_relaxed);
  }

  // 周期自动导出（秒）：用于无人值守自动化采样。
  const std::string autoExportSec =
      env::getEnvVar("DXVK_WAR3_PERF_AUTO_EXPORT_SEC");
  if (!autoExportSec.empty()) {
    const int sec = std::max(0, std::atoi(autoExportSec.c_str()));
    if (sec > 0) {
      m_autoExportInterval = std::chrono::seconds(sec);
    }
  }
}

void War3PerfMonitor::noteBusinessFrameSerial(uint64_t serial) {
  m_lastBusinessFrameSerial.store(serial, std::memory_order_relaxed);
}

void War3PerfMonitor::noteShadowMetadataFrame(uint64_t captureUs,
                                               uint64_t captureCalls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  m_currentFrameWorkload.shadowMetadataCaptureUs = captureUs;
  m_currentFrameWorkload.shadowMetadataCaptureCalls = captureCalls;
}

void War3PerfMonitor::noteShadowBudgetFrame(
    const War3ShadowCaptureStats& stats) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  m_currentFrameWorkload.hasShadowBudget = true;
  m_currentFrameWorkload.capturedDrawCount = stats.captured;
  m_currentFrameWorkload.terrainDoodadCaptureAttemptCount =
      stats.terrainDoodadCaptureAttemptCount;
  m_currentFrameWorkload.terrainDoodadCaptureAcceptedCount =
      stats.terrainDoodadCaptureAcceptedCount;
  m_currentFrameWorkload.terrainDoodadDynamicSourceCount =
      stats.terrainDoodadDynamicSourceCount;
  m_currentFrameWorkload.terrainDoodadWorldIdentityLikeCount =
      stats.terrainDoodadWorldIdentityLikeCount;
  m_currentFrameWorkload.terrainDoodadWorldNonIdentityCount =
      stats.terrainDoodadWorldNonIdentityCount;
  m_currentFrameWorkload.terrainS1CaptureAttemptCount =
      stats.terrainS1CaptureAttemptCount;
  m_currentFrameWorkload.terrainS1CaptureAcceptedCount =
      stats.terrainS1CaptureAcceptedCount;
  m_currentFrameWorkload.terrainS1WorldIdentityLikeCount =
      stats.terrainS1WorldIdentityLikeCount;
  m_currentFrameWorkload.terrainS1WorldNonIdentityCount =
      stats.terrainS1WorldNonIdentityCount;
  m_currentFrameWorkload.terrainS1WorldNonFiniteCount =
      stats.terrainS1WorldNonFiniteCount;
  m_currentFrameWorkload.terrainS1ForceIdentityWorldCount =
      stats.terrainS1ForceIdentityWorldCount;
  m_currentFrameWorkload.terrainS1WorldMatrixHash =
      stats.terrainS1WorldMatrixHash;
  m_currentFrameWorkload.terrainS1WorldTranslationMilliMax =
      stats.terrainS1WorldTranslationMilliMax;
  m_currentFrameWorkload.stage13CaptureAttemptCount =
      stats.stage13CaptureAttemptCount;
  m_currentFrameWorkload.stage13CaptureRejectedNoDemandCount =
      stats.stage13CaptureRejectedNoDemandCount;
  m_currentFrameWorkload.stage13CaptureRejectedAfterBeforeUiCount =
      stats.stage13CaptureRejectedAfterBeforeUiCount;
  m_currentFrameWorkload.stage13CaptureConsideredCount =
      stats.stage13CaptureConsideredCount;
  m_currentFrameWorkload.stage13FreezeCopyBytes =
      stats.stage13FreezeCopyBytes;
  m_currentFrameWorkload.stage13CpuSnapshotCopyBytes =
      stats.stage13CpuSnapshotCopyBytes;
  m_currentFrameWorkload.stage13RetentionSnapshotBytes =
      stats.stage13RetentionSnapshotBytes;
  m_currentFrameWorkload.stage13ReplayDrawCount =
      stats.semanticSceneStage13ReplayDrawCount;
  m_currentFrameWorkload.receiverCameraDeltaNano =
      stats.semanticSceneReceiverCameraDeltaNano;
  m_currentFrameWorkload.receiverSunDeltaNano =
      stats.semanticSceneReceiverSunDeltaNano;
  m_currentFrameWorkload.receiverCsmDeltaNano =
      stats.semanticSceneReceiverCsmDeltaNano;
  m_currentFrameWorkload.receiverSnappedCenterDeltaTexelsNano =
      stats.semanticSceneReceiverSnappedCenterDeltaTexelsNano;
  m_currentFrameWorkload.receiverTexelSizeDeltaNano =
      stats.semanticSceneReceiverTexelSizeDeltaNano;
  m_currentFrameWorkload.shadowMapRenderSerial =
      stats.semanticSceneShadowMapRenderSerial;
  m_currentFrameWorkload.uniqueGeometryCount = stats.uniqueGeometryCount;
  m_currentFrameWorkload.staticPersistentCount = stats.staticPersistentCount;
  m_currentFrameWorkload.dynamicPoseCount = stats.dynamicPoseCount;
  m_currentFrameWorkload.dynamicSkinnedOutputCount =
      stats.dynamicSkinnedOutputCount;
  m_currentFrameWorkload.fallbackDrawCount = stats.fallbackDrawCount;
  m_currentFrameWorkload.drawTimeVBCacheCaptureCount =
      stats.drawTimeVBCacheCaptureCount;
  m_currentFrameWorkload.drawTimeVBCacheConsumeHitCount =
      stats.drawTimeVBCacheConsumeHitCount;
  m_currentFrameWorkload.drawTimeVBCacheConsumeMissCount =
      stats.drawTimeVBCacheConsumeMissCount;
  m_currentFrameWorkload.drawTimeVBCacheRejectNoLayerContext =
      stats.drawTimeVBCacheRejectNoLayerContext;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractLookup =
      stats.drawTimeVBCacheRejectContractLookup;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractFreshness =
      stats.drawTimeVBCacheRejectContractFreshness;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractStage =
      stats.drawTimeVBCacheRejectContractStage;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractRenderFrame =
      stats.drawTimeVBCacheRejectContractRenderFrame;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractInstance =
      stats.drawTimeVBCacheRejectContractInstance;
  m_currentFrameWorkload.drawTimeVBCacheRejectContractSlice =
      stats.drawTimeVBCacheRejectContractSlice;
  m_currentFrameWorkload.drawTimeVBCacheSameFrameDedupMiss =
      stats.drawTimeVBCacheSameFrameDedupMiss;
  m_currentFrameWorkload.semanticSceneSubmitted =
      stats.semanticSceneSubmitted;
  m_currentFrameWorkload.semanticSceneSubmittedSkinned =
      stats.semanticSceneSubmittedSkinned;
  m_currentFrameWorkload.skippedCasterCap = stats.skippedCasterCap;
  m_currentFrameWorkload.skippedDistanceCull = stats.skippedPositionT;
  m_currentFrameWorkload.terrainDoodadPreparedCount =
      stats.semanticSceneShadowMapTerrainDoodadPreparedCount;
  m_currentFrameWorkload.terrainS1PreparedCount =
      stats.semanticSceneShadowMapTerrainS1PreparedCount;
  m_currentFrameWorkload.terrainDoodadCascade0DrawnCount =
      stats.semanticSceneShadowMapTerrainDoodadCascade0DrawnCount;
  m_currentFrameWorkload.terrainDoodadCascade1DrawnCount =
      stats.semanticSceneShadowMapTerrainDoodadCascade1DrawnCount;
  m_currentFrameWorkload.terrainDoodadCascade2DrawnCount =
      stats.semanticSceneShadowMapTerrainDoodadCascade2DrawnCount;
  m_currentFrameWorkload.terrainDoodadCascade3DrawnCount =
      stats.semanticSceneShadowMapTerrainDoodadCascade3DrawnCount;
  m_currentFrameWorkload.terrainS1Cascade0DrawnCount =
      stats.semanticSceneShadowMapTerrainS1Cascade0DrawnCount;
  m_currentFrameWorkload.terrainS1Cascade1DrawnCount =
      stats.semanticSceneShadowMapTerrainS1Cascade1DrawnCount;
  m_currentFrameWorkload.terrainS1Cascade2DrawnCount =
      stats.semanticSceneShadowMapTerrainS1Cascade2DrawnCount;
  m_currentFrameWorkload.terrainS1Cascade3DrawnCount =
      stats.semanticSceneShadowMapTerrainS1Cascade3DrawnCount;
  auto& agg = m_shadowBudgetAggregate;
  agg.framesObserved++;
  agg.stage13CaptureAttemptCount += stats.stage13CaptureAttemptCount;
  agg.stage13CaptureRejectedNoDemandCount +=
      stats.stage13CaptureRejectedNoDemandCount;
  agg.stage13CaptureRejectedAfterBeforeUiCount +=
      stats.stage13CaptureRejectedAfterBeforeUiCount;
  agg.stage13CaptureConsideredCount += stats.stage13CaptureConsideredCount;
  agg.stage13FreezeCopyBytes += stats.stage13FreezeCopyBytes;
  agg.stage13CpuSnapshotCopyBytes += stats.stage13CpuSnapshotCopyBytes;
  agg.stage13RetentionSnapshotBytes +=
      stats.stage13RetentionSnapshotBytes;
  agg.stage13ReplayDrawCount +=
      stats.semanticSceneStage13ReplayDrawCount;
  agg.framesBudgetExceeded += stats.budgetExceeded != 0 ? 1u : 0u;
  agg.totalBudgetBytes += stats.fallbackBudgetBytes;
  agg.totalUsedBytes += stats.fallbackBudgetUsedBytes;
  agg.maxBudgetBytes =
      (std::max)(agg.maxBudgetBytes, stats.fallbackBudgetBytes);
  agg.maxUsedBytes =
      (std::max)(agg.maxUsedBytes, stats.fallbackBudgetUsedBytes);
  agg.totalArenaBytes += stats.fallbackArenaBytes;
  agg.maxArenaBytes =
      (std::max)(agg.maxArenaBytes, stats.fallbackArenaBytes);
  agg.legacyCaptured += stats.captured;
  agg.skippedUpload += stats.skippedMissingPerDrawUpload;
  agg.arenaRejectUnsupported += stats.skippedPosFormat;
  agg.arenaRejectValidation += stats.skippedNoPosition;
  agg.arenaRejectUninitialized += stats.skippedNoZTest;
  agg.arenaRejectOverflow += stats.skippedVertexShader;
  agg.skippedCasterCap += stats.skippedCasterCap;
  agg.skippedDistanceCull += stats.skippedPositionT;
  agg.skippedFreezeBudget += stats.skippedFreezeBudget;
  agg.skippedPriorityBudget += stats.skippedPriorityBudget;
  agg.degradedAlphaBudget += stats.degradedAlphaBudget;
  agg.uniqueGeometryCount += stats.uniqueGeometryCount;
  agg.uniqueInstanceableGeometryCount += stats.persistentGeometryCount;
  agg.duplicateGeometryInstances += stats.duplicateGeometryInstances;
  agg.reuseEligibleDuplicates += stats.reuseEligibleDuplicates;
  agg.potentialFreezeReuseHits += stats.potentialFreezeReuseHits;
  agg.staticPersistentCount += stats.staticPersistentCount;
  agg.dynamicPoseCount += stats.dynamicPoseCount;
  agg.dynamicSkinnedOutputCount += stats.dynamicSkinnedOutputCount;
  agg.fallbackDrawCount += stats.fallbackDrawCount;
  agg.fallbackDrawCountTerrain += stats.fallbackDrawCountTerrain;
  agg.fallbackDrawCountWorldObject += stats.fallbackDrawCountWorldObject;
  agg.fallbackDrawCountUnitObject += stats.fallbackDrawCountUnitObject;
  agg.objectFallbackDrawCount +=
      stats.fallbackDrawCountWorldObject + stats.fallbackDrawCountUnitObject;
  agg.drawTimeVBCacheCaptureCount += stats.drawTimeVBCacheCaptureCount;
  agg.drawTimeVBCacheConsumeHitCount += stats.drawTimeVBCacheConsumeHitCount;
  agg.drawTimeVBCacheConsumeMissCount += stats.drawTimeVBCacheConsumeMissCount;
  agg.drawTimeVBCacheRejectNoLayerContext +=
      stats.drawTimeVBCacheRejectNoLayerContext;
  agg.drawTimeVBCacheSameFrameDedupMiss +=
      stats.drawTimeVBCacheSameFrameDedupMiss;
  agg.semanticBridgeHit += stats.semanticBridgeHit;
  agg.semanticBridgeMiss += stats.semanticBridgeMiss;
  agg.semanticBridgeBypassed += stats.semanticBridgeBypassed;
  agg.semanticSceneSubmitted += stats.semanticSceneSubmitted;
  agg.semanticSceneSubmittedUnit += stats.semanticSceneSubmittedUnit;
  agg.semanticSceneSubmittedSkinned += stats.semanticSceneSubmittedSkinned;
  agg.semanticSceneSubmittedSkinnedNonUnitResolvedCount +=
      stats.semanticSceneSubmittedSkinnedNonUnitResolvedCount;
  agg.semanticSceneSubmittedSkinnedUnknownPacketKindCount +=
      stats.semanticSceneSubmittedSkinnedUnknownPacketKindCount;
  agg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount +=
      stats.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount;
  agg.semanticSceneSubmittedSkinnedGroupNonZeroCount +=
      stats.semanticSceneSubmittedSkinnedGroupNonZeroCount;
  agg.semanticSceneSubmittedSkinnedTransparentQueueCount +=
      stats.semanticSceneSubmittedSkinnedTransparentQueueCount;
  agg.semanticSceneSubmittedSkinnedMissingUnitPtrCount +=
      stats.semanticSceneSubmittedSkinnedMissingUnitPtrCount;
  agg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount +=
      stats.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount;
  agg.semanticSceneSubmittedBuilding += stats.semanticSceneSubmittedBuilding;
  agg.semanticSceneSubmittedDestructible +=
      stats.semanticSceneSubmittedDestructible;
  agg.semanticSceneSubmittedCutout += stats.semanticSceneSubmittedCutout;
  agg.semanticSceneSubmittedAlphaBlend +=
      stats.semanticSceneSubmittedAlphaBlend;
  agg.semanticSceneMaterialObservedCutoutCount +=
      stats.semanticSceneMaterialObservedCutoutCount;
  agg.semanticSceneMaterialObservedAlphaBlendCount +=
      stats.semanticSceneMaterialObservedAlphaBlendCount;
  agg.semanticSceneRejectedCutoutSkinnedContract +=
      stats.semanticSceneRejectedCutoutSkinnedContract;
  agg.semanticSceneRejectedAlphaBlendSkinnedContract +=
      stats.semanticSceneRejectedAlphaBlendSkinnedContract;
  agg.semanticSceneRejectedCutoutGeometry +=
      stats.semanticSceneRejectedCutoutGeometry;
  agg.semanticSceneRejectedAlphaBlendGeometry +=
      stats.semanticSceneRejectedAlphaBlendGeometry;
  agg.semanticSceneRejectedCutoutVisualPolicy +=
      stats.semanticSceneRejectedCutoutVisualPolicy;
  agg.semanticSceneRejectedAlphaBlendVisualPolicy +=
      stats.semanticSceneRejectedAlphaBlendVisualPolicy;
  agg.semanticSceneMaterialLayerContractResolvedCount +=
      stats.semanticSceneMaterialLayerContractResolvedCount;
  agg.semanticSceneMaterialLayerContractFailedCount +=
      stats.semanticSceneMaterialLayerContractFailedCount;
  agg.semanticSceneMaterialBlendMode0Count +=
      stats.semanticSceneMaterialBlendMode0Count;
  agg.semanticSceneMaterialBlendMode1Count +=
      stats.semanticSceneMaterialBlendMode1Count;
  agg.semanticSceneMaterialBlendMode2PlusCount +=
      stats.semanticSceneMaterialBlendMode2PlusCount;
  agg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount +=
      stats.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount;
  agg.semanticSceneFastAppendBoundsPoseAvailableCount +=
      stats.semanticSceneFastAppendBoundsPoseAvailableCount;
  agg.semanticSceneFastAppendBoundsSceneReadSuccessCount +=
      stats.semanticSceneFastAppendBoundsSceneReadSuccessCount;
  agg.semanticSceneFastAppendBoundsPoseDeltaLe1Count +=
      stats.semanticSceneFastAppendBoundsPoseDeltaLe1Count;
  agg.semanticSceneFastAppendBoundsPoseDeltaLe4Count +=
      stats.semanticSceneFastAppendBoundsPoseDeltaLe4Count;
  agg.semanticSceneFastAppendBoundsPoseDeltaLe16Count +=
      stats.semanticSceneFastAppendBoundsPoseDeltaLe16Count;
  agg.semanticSceneFastAppendBoundsPoseDeltaGt16Count +=
      stats.semanticSceneFastAppendBoundsPoseDeltaGt16Count;
  agg.semanticSceneFastAppendBoundsPoseDeltaMaxMilli =
      (std::max)(agg.semanticSceneFastAppendBoundsPoseDeltaMaxMilli,
                 stats.semanticSceneFastAppendBoundsPoseDeltaMaxMilli);
  agg.semanticSceneLivePaletteRefreshAttemptCount +=
      stats.semanticSceneLivePaletteRefreshAttemptCount;
  agg.semanticSceneLivePaletteRefreshHitCount +=
      stats.semanticSceneLivePaletteRefreshHitCount;
  agg.semanticSceneLivePaletteRefreshMissCount +=
      stats.semanticSceneLivePaletteRefreshMissCount;
  agg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount +=
      stats.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount;
  agg.semanticScenePaletteOverrideNoComposeCount +=
      stats.semanticScenePaletteOverrideNoComposeCount;
  agg.semanticScenePaletteOverrideWouldComposeCount +=
      stats.semanticScenePaletteOverrideWouldComposeCount;
  agg.semanticScenePalettePacketWorldComposeCount +=
      stats.semanticScenePalettePacketWorldComposeCount;
  agg.semanticSceneLivePaletteMotionSampleCount +=
      stats.semanticSceneLivePaletteMotionSampleCount;
  agg.semanticSceneLivePaletteMotionNewRuntimeCount +=
      stats.semanticSceneLivePaletteMotionNewRuntimeCount;
  agg.semanticSceneLivePaletteMotionRawChangedCount +=
      stats.semanticSceneLivePaletteMotionRawChangedCount;
  agg.semanticSceneLivePaletteMotionRawStableCount +=
      stats.semanticSceneLivePaletteMotionRawStableCount;
  agg.semanticSceneLivePaletteMotionGroupChangedCount +=
      stats.semanticSceneLivePaletteMotionGroupChangedCount;
  agg.semanticSceneLivePaletteMotionGroupStableCount +=
      stats.semanticSceneLivePaletteMotionGroupStableCount;
  agg.semanticSceneDrawTimePoseAttemptCount +=
      stats.semanticSceneDrawTimePoseAttemptCount;
  agg.semanticSceneDrawTimePosePublishedCount +=
      stats.semanticSceneDrawTimePosePublishedCount;
  agg.semanticSceneDrawTimePoseRejectUiOrEffectCount +=
      stats.semanticSceneDrawTimePoseRejectUiOrEffectCount;
  agg.semanticSceneDrawTimePoseRejectVertexShaderCount +=
      stats.semanticSceneDrawTimePoseRejectVertexShaderCount;
  agg.semanticSceneDrawTimePoseRejectNoVertexBlendCount +=
      stats.semanticSceneDrawTimePoseRejectNoVertexBlendCount;
  agg.semanticSceneDrawTimePoseRejectNoContextCount +=
      stats.semanticSceneDrawTimePoseRejectNoContextCount;
  agg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount +=
      stats.semanticSceneDrawTimePoseRejectNoRuntimeModelCount;
  agg.semanticSceneDrawTimePoseDedupedCount +=
      stats.semanticSceneDrawTimePoseDedupedCount;
  agg.semanticSceneDrawTimePoseChangedCount +=
      stats.semanticSceneDrawTimePoseChangedCount;
  agg.semanticSceneDrawTimePoseStableCount +=
      stats.semanticSceneDrawTimePoseStableCount;
  agg.semanticSceneSubmittedPaletteMotionSampleCount +=
      stats.semanticSceneSubmittedPaletteMotionSampleCount;
  agg.semanticSceneSubmittedPaletteMotionNewRuntimeCount +=
      stats.semanticSceneSubmittedPaletteMotionNewRuntimeCount;
  agg.semanticSceneSubmittedPaletteMotionChangedCount +=
      stats.semanticSceneSubmittedPaletteMotionChangedCount;
  agg.semanticSceneSubmittedPaletteMotionStableCount +=
      stats.semanticSceneSubmittedPaletteMotionStableCount;
  agg.semanticSceneSkinnedDynamicIndexSliceCount +=
      stats.semanticSceneSkinnedDynamicIndexSliceCount;
  agg.semanticSceneSubmittedOwnedGroupSlots +=
      stats.semanticSceneSubmittedOwnedGroupSlots;
  agg.semanticSceneCurrentDrawContractKnownCount +=
      stats.semanticSceneCurrentDrawContractKnownCount;
  agg.semanticSceneCurrentDrawPaletteReadyCount +=
      stats.semanticSceneCurrentDrawPaletteReadyCount;
  agg.semanticSceneCurrentDrawGroupSlotReadyCount +=
      stats.semanticSceneCurrentDrawGroupSlotReadyCount;
  agg.semanticSceneCurrentDrawResolveReadyCount +=
      stats.semanticSceneCurrentDrawResolveReadyCount;
  agg.semanticSceneCurrentDrawMissNoContract +=
      stats.semanticSceneCurrentDrawMissNoContract;
  agg.semanticSceneCurrentDrawMissNoPalette +=
      stats.semanticSceneCurrentDrawMissNoPalette;
  agg.semanticSceneCurrentDrawMissNoGroupSlots +=
      stats.semanticSceneCurrentDrawMissNoGroupSlots;
  agg.semanticSceneCurrentDrawMissStaleVisibleFrame +=
      stats.semanticSceneCurrentDrawMissStaleVisibleFrame;
  agg.semanticSceneCurrentDrawResolveReadyRejectedCount +=
      stats.semanticSceneCurrentDrawResolveReadyRejectedCount;
  agg.semanticSceneCanonicalReadyCount +=
      stats.semanticSceneCanonicalReadyCount;
  agg.semanticSceneCanonicalReadyCutoutCount +=
      stats.semanticSceneCanonicalReadyCutoutCount;
  agg.semanticSceneCanonicalReadyAlphaBlendCount +=
      stats.semanticSceneCanonicalReadyAlphaBlendCount;
  agg.semanticSceneCanonicalRejectNoStableIdentity +=
      stats.semanticSceneCanonicalRejectNoStableIdentity;
  agg.semanticSceneCanonicalRejectNoMesh +=
      stats.semanticSceneCanonicalRejectNoMesh;
  agg.semanticSceneCanonicalRejectNoWorldTransform +=
      stats.semanticSceneCanonicalRejectNoWorldTransform;
  agg.semanticSceneCanonicalRejectNoPalette +=
      stats.semanticSceneCanonicalRejectNoPalette;
  agg.semanticSceneCanonicalRejectNoSlotContract +=
      stats.semanticSceneCanonicalRejectNoSlotContract;
  agg.semanticSceneCanonicalRejectStaleProducer +=
      stats.semanticSceneCanonicalRejectStaleProducer;
  agg.semanticSceneCanonicalRejectInvalidVertexIndex +=
      stats.semanticSceneCanonicalRejectInvalidVertexIndex;
  agg.semanticSceneCanonicalRejectExplicitBlendIncomplete +=
      stats.semanticSceneCanonicalRejectExplicitBlendIncomplete;
  agg.semanticSceneCanonicalRejectAfterReadyCount +=
      stats.semanticSceneCanonicalRejectAfterReadyCount;
  agg.semanticSceneSubmittedExplicitBlendContract +=
      stats.semanticSceneSubmittedExplicitBlendContract;
  agg.semanticSceneSubmittedSingleMatrixGroupSkinning +=
      stats.semanticSceneSubmittedSingleMatrixGroupSkinning;
  agg.semanticSceneSubmittedMultiGroupSlotSkinning +=
      stats.semanticSceneSubmittedMultiGroupSlotSkinning;
  if (stats.semanticSceneSkinnedMinUniqueGroupSlots != 0u &&
      (agg.semanticSceneSkinnedMinUniqueGroupSlots == 0u ||
       stats.semanticSceneSkinnedMinUniqueGroupSlots <
           agg.semanticSceneSkinnedMinUniqueGroupSlots)) {
    agg.semanticSceneSkinnedMinUniqueGroupSlots =
        stats.semanticSceneSkinnedMinUniqueGroupSlots;
  }
  agg.semanticSceneSkinnedMaxUniqueGroupSlots = std::max<uint64_t>(
      agg.semanticSceneSkinnedMaxUniqueGroupSlots,
      stats.semanticSceneSkinnedMaxUniqueGroupSlots);
  agg.semanticSceneSkinnedGroupSlotsUnique1Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique1Count;
  agg.semanticSceneSkinnedGroupSlotsUnique2To4Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique2To4Count;
  agg.semanticSceneSkinnedGroupSlotsUnique5To8Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique5To8Count;
  agg.semanticSceneSkinnedGroupSlotsUnique9To16Count +=
      stats.semanticSceneSkinnedGroupSlotsUnique9To16Count;
  agg.semanticSceneSkinnedGroupSlotsUnique17PlusCount +=
      stats.semanticSceneSkinnedGroupSlotsUnique17PlusCount;
  agg.semanticSceneExplicitBlendUnavailableCurrentDraw +=
      stats.semanticSceneExplicitBlendUnavailableCurrentDraw;
  agg.semanticSceneSkinnedFullIndexFallbackCount +=
      stats.semanticSceneSkinnedFullIndexFallbackCount;
  agg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount +=
      stats.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount;
  agg.semanticSceneSubmittedFrameLocal +=
      stats.semanticSceneSubmittedFrameLocal;
  agg.semanticSceneSubmittedPersistent +=
      stats.semanticSceneSubmitted > stats.semanticSceneSubmittedFrameLocal
          ? (stats.semanticSceneSubmitted -
             stats.semanticSceneSubmittedFrameLocal)
          : 0u;
  agg.semanticScenePopulateAttemptCount +=
      stats.semanticScenePopulateAttemptCount;
  agg.semanticScenePopulateUnitsOnlyCount +=
      stats.semanticScenePopulateUnitsOnlyCount;
  agg.semanticScenePopulateLastReturnReason =
      stats.semanticScenePopulateLastReturnReason;
  agg.semanticScenePopulateLastProducerPublishAttemptDelta =
      stats.semanticScenePopulateLastProducerPublishAttemptDelta;
  agg.semanticScenePopulateLastProducerPublishReadyDelta =
      stats.semanticScenePopulateLastProducerPublishReadyDelta;
  agg.semanticScenePopulateLastProducerQueryAttemptDelta =
      stats.semanticScenePopulateLastProducerQueryAttemptDelta;
  agg.semanticScenePopulateLastProducerQueryHitDelta =
      stats.semanticScenePopulateLastProducerQueryHitDelta;
  agg.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta =
      stats.semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta;
  agg.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta =
      stats.semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta;
  agg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta =
      stats.semanticScenePopulateLastProducerGroupDecodeAttemptDelta;
  agg.semanticScenePopulateLastProducerGroupDecodeHitDelta =
      stats.semanticScenePopulateLastProducerGroupDecodeHitDelta;
  agg.semanticSceneDirectCurrentDrawRecordCount +=
      stats.semanticSceneDirectCurrentDrawRecordCount;
  agg.semanticSceneDirectCurrentDrawBuiltPacketCount +=
      stats.semanticSceneDirectCurrentDrawBuiltPacketCount;
  agg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount +=
      stats.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount +=
      stats.semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount +=
      stats.semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount;
  agg.semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount +=
      stats
          .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount;
  agg.drawTimeSemanticProducerVisibleCandidateCount +=
      stats.drawTimeSemanticProducerVisibleCandidateCount;
  agg.drawTimeSemanticProducerFreshEntryCount +=
      stats.drawTimeSemanticProducerFreshEntryCount;
  agg.drawTimeSemanticProducerClaimedCount +=
      stats.drawTimeSemanticProducerClaimedCount;
  agg.drawTimeSemanticProducerSubmittedCount +=
      stats.drawTimeSemanticProducerSubmittedCount;
  agg.drawTimeSemanticProducerMissNoFreshEntryCount +=
      stats.drawTimeSemanticProducerMissNoFreshEntryCount;
  agg.drawTimeSemanticProducerFallbackCurrentDrawCount +=
      stats.drawTimeSemanticProducerFallbackCurrentDrawCount;
  agg.drawTimeSemanticProducerOwnedDirectGroupedSkipCount +=
      stats.drawTimeSemanticProducerOwnedDirectGroupedSkipCount;
  agg.drawTimeSemanticProducerLifecycleMergedCount +=
      stats.drawTimeSemanticProducerLifecycleMergedCount;
  agg.semanticSceneProducerClaimObserveMode = std::max(
      agg.semanticSceneProducerClaimObserveMode,
      uint64_t(stats.semanticSceneProducerClaimObserveMode));
  agg.semanticSceneProducerClaimExactKeyCount +=
      stats.semanticSceneProducerClaimExactKeyCount;
  agg.semanticSceneProducerClaimCandidateCount +=
      stats.semanticSceneProducerClaimCandidateCount;
  agg.semanticSceneProducerClaimCanonicalOwnedCount +=
      stats.semanticSceneProducerClaimCanonicalOwnedCount;
  agg.semanticSceneProducerClaimMissingKeyCount +=
      stats.semanticSceneProducerClaimMissingKeyCount;
  agg.semanticSceneProducerClaimUnresolvedCount +=
      stats.semanticSceneProducerClaimUnresolvedCount;
  agg.semanticSceneProducerClaimStrictPredictedCount +=
      stats.semanticSceneProducerClaimStrictPredictedCount;
  agg.semanticSceneProducerClaimStrictMatchCount +=
      stats.semanticSceneProducerClaimStrictMatchCount;
  agg.semanticSceneProducerClaimStrictFalsePositiveCount +=
      stats.semanticSceneProducerClaimStrictFalsePositiveCount;
  agg.semanticSceneProducerClaimStrictFalseNegativeCount +=
      stats.semanticSceneProducerClaimStrictFalseNegativeCount;
  agg.semanticSceneProducerClaimLogicalPredictedCount +=
      stats.semanticSceneProducerClaimLogicalPredictedCount;
  agg.semanticSceneProducerClaimLogicalMatchCount +=
      stats.semanticSceneProducerClaimLogicalMatchCount;
  agg.semanticSceneProducerClaimLogicalFalsePositiveCount +=
      stats.semanticSceneProducerClaimLogicalFalsePositiveCount;
  agg.semanticSceneProducerClaimLogicalFalseNegativeCount +=
      stats.semanticSceneProducerClaimLogicalFalseNegativeCount;
  agg.semanticSceneProducerClaimConsumeDeniedCount +=
      stats.semanticSceneProducerClaimConsumeDeniedCount;
  // Phase 7.2: flicker diagnostics + reconciliation
  agg.semanticSceneDirectLastRawRecordCount = std::max(
      agg.semanticSceneDirectLastRawRecordCount,
      uint64_t(stats.semanticSceneDirectLastRawRecordCount));
  agg.semanticSceneDirectLastEligibleRecordCount = std::max(
      agg.semanticSceneDirectLastEligibleRecordCount,
      uint64_t(stats.semanticSceneDirectLastEligibleRecordCount));
  agg.semanticSceneCompactWorkTableMode = std::max(
      agg.semanticSceneCompactWorkTableMode,
      uint64_t(stats.semanticSceneCompactWorkTableMode));
  agg.semanticSceneCompactWorkTableCandidateCount +=
      stats.semanticSceneCompactWorkTableCandidateCount;
  agg.semanticSceneCompactWorkTableSealedCount +=
      stats.semanticSceneCompactWorkTableSealedCount;
  agg.semanticSceneCompactWorkTableConsumedCount +=
      stats.semanticSceneCompactWorkTableConsumedCount;
  agg.semanticSceneCompactWorkTableFallbackCount +=
      stats.semanticSceneCompactWorkTableFallbackCount;
  agg.semanticSceneCompactWorkTableRejectStageCount +=
      stats.semanticSceneCompactWorkTableRejectStageCount;
  agg.semanticSceneCompactWorkTableRejectFreshnessCount +=
      stats.semanticSceneCompactWorkTableRejectFreshnessCount;
  agg.semanticSceneCompactWorkTableRejectPolicyCount +=
      stats.semanticSceneCompactWorkTableRejectPolicyCount;
  agg.semanticSceneCompactWorkTableRejectFrameCount +=
      stats.semanticSceneCompactWorkTableRejectFrameCount;
  agg.semanticSceneCompactWorkTableRejectIdentityCount +=
      stats.semanticSceneCompactWorkTableRejectIdentityCount;
  agg.semanticSceneCompactWorkTableMismatchCount +=
      stats.semanticSceneCompactWorkTableMismatchCount;
  agg.semanticSceneDirectLastSubmittedRecordCount = std::max(
      agg.semanticSceneDirectLastSubmittedRecordCount,
      uint64_t(stats.semanticSceneDirectLastSubmittedRecordCount));
  agg.semanticSceneDirectLastUniqueObjectCount = std::max(
      agg.semanticSceneDirectLastUniqueObjectCount,
      uint64_t(stats.semanticSceneDirectLastUniqueObjectCount));
  agg.semanticSceneDirectLastSubmittedObjectCount = std::max(
      agg.semanticSceneDirectLastSubmittedObjectCount,
      uint64_t(stats.semanticSceneDirectLastSubmittedObjectCount));
  agg.semanticSceneDirectLastRecordCapPartialObjectCount = std::max(
      agg.semanticSceneDirectLastRecordCapPartialObjectCount,
      uint64_t(stats.semanticSceneDirectLastRecordCapPartialObjectCount));
  agg.semanticSceneDirectLastScanCapPartialObjectCount = std::max(
      agg.semanticSceneDirectLastScanCapPartialObjectCount,
      uint64_t(stats.semanticSceneDirectLastScanCapPartialObjectCount));
  agg.semanticSceneDirectLastMinGeosetsPerObject = std::max(
      agg.semanticSceneDirectLastMinGeosetsPerObject,
      uint64_t(stats.semanticSceneDirectLastMinGeosetsPerObject));
  agg.semanticSceneDirectLastMaxGeosetsPerObject = std::max(
      agg.semanticSceneDirectLastMaxGeosetsPerObject,
      uint64_t(stats.semanticSceneDirectLastMaxGeosetsPerObject));
  agg.semanticSceneDirectLastSubmittedIdentityHash = stats.semanticSceneDirectLastSubmittedIdentityHash;
  agg.semanticSceneDirectIdentityChurnCount += stats.semanticSceneDirectIdentityChurnCount;
  agg.semanticSceneDirectRecordCapHitCount += stats.semanticSceneDirectRecordCapHitCount;
  agg.semanticSceneDirectRecordCapTruncatedRecordCount +=
      stats.semanticSceneDirectRecordCapTruncatedRecordCount;
  agg.semanticSceneDirectScanCapHitCount +=
      stats.semanticSceneDirectScanCapHitCount;
  agg.semanticSceneDirectObjectGroupedSubmitCount +=
      stats.semanticSceneDirectObjectGroupedSubmitCount;
  agg.semanticSceneDirectObjectGroupedSkipCount +=
      stats.semanticSceneDirectObjectGroupedSkipCount;
  agg.semanticSceneDirectRecordCapSkipObjectCount += stats.semanticSceneDirectRecordCapSkipObjectCount;
  agg.semanticSceneDirectRecordCapAppendFailCount += stats.semanticSceneDirectRecordCapAppendFailCount;
  agg.semanticSceneDirectSelectionLeaseActiveKeyCount = std::max(
      agg.semanticSceneDirectSelectionLeaseActiveKeyCount,
      uint64_t(stats.semanticSceneDirectSelectionLeaseActiveKeyCount));
  agg.semanticSceneDirectSelectionLeasePrunedKeyCount +=
      stats.semanticSceneDirectSelectionLeasePrunedKeyCount;
  agg.semanticSceneDirectSelectionLeaseSubmittedKeyCount +=
      stats.semanticSceneDirectSelectionLeaseSubmittedKeyCount;
  agg.semanticSceneDirectStickyFillBudgetRecordCount = std::max(
      agg.semanticSceneDirectStickyFillBudgetRecordCount,
      uint64_t(stats.semanticSceneDirectStickyFillBudgetRecordCount));
  agg.semanticSceneDirectStickyFillAppendedCount +=
      stats.semanticSceneDirectStickyFillAppendedCount;
  agg.semanticSceneDirectStickyFillSubmittedCount +=
      stats.semanticSceneDirectStickyFillSubmittedCount;
  agg.semanticSceneDirectStickyFillMissedCount +=
      stats.semanticSceneDirectStickyFillMissedCount;
  agg.semanticSceneDirectPartLeaseRestoredCount +=
      stats.semanticSceneDirectPartLeaseRestoredCount;
  agg.semanticSceneDirectPartLeaseUpdatedCount +=
      stats.semanticSceneDirectPartLeaseUpdatedCount;
  agg.semanticSceneDirectPartLeaseExpiredCount +=
      stats.semanticSceneDirectPartLeaseExpiredCount;
  agg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount +=
      stats.semanticSceneDirectPartLeaseRejectedDynamicMeshCount;
  agg.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount +=
      stats.semanticSceneDirectPartLeaseRejectedNotSelfContainedCount;
  agg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount +=
      stats.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount;
  agg.semanticSceneDirectPartLeaseRejectedSelfRenewCount +=
      stats.semanticSceneDirectPartLeaseRejectedSelfRenewCount;
  agg.semanticSceneDirectPartLeaseBudgetLimitCount +=
      stats.semanticSceneDirectPartLeaseBudgetLimitCount;
  agg.semanticSceneShadowManifestPartLeaseRestoredCount +=
      stats.semanticSceneShadowManifestPartLeaseRestoredCount;
  agg.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount +=
      stats.semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount;
  agg.semanticSceneShadowManifestPartLeaseExpiredCount +=
      stats.semanticSceneShadowManifestPartLeaseExpiredCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount;
  agg
      .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount +=
      stats
          .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount;
  agg.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount +=
      stats.semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount;
  agg.semanticSceneShadowManifestPartLeaseBudgetLimitCount +=
      stats.semanticSceneShadowManifestPartLeaseBudgetLimitCount;
  agg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount +=
      stats.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount;
  agg.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount +=
      stats.semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount;
  agg.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount +=
      stats.semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount;
  agg.semanticSceneShadowManifestObjectCoreCompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreCompleteCount;
  agg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount +=
      stats.semanticSceneShadowManifestObjectCoreIncompleteSkipCount;
  agg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount +=
      stats.semanticSceneShadowManifestPartOmittedIncompleteCoreCount;
  // Phase 7.25 core epoch planner 专属计数器聚合（累加语义，便于整测窗口对比）。
  agg.semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount;
  agg.semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount;
  agg.semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount;
  agg.semanticSceneShadowManifestObjectCoreEpochMissingPartCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochMissingPartCount;
  agg.semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount +=
      stats.semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount;
  agg.semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount +=
      stats.semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount;
  agg.semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount +=
      stats.semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount;
  agg.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount +=
      stats.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount;
  agg.semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount +=
      stats.semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount;
  agg.semanticSceneShadowManifestMissingRequiredPartCount +=
      stats.semanticSceneShadowManifestMissingRequiredPartCount;
  agg.semanticSceneShadowManifestGraceUsedCount +=
      stats.semanticSceneShadowManifestGraceUsedCount;
  agg.semanticSceneShadowManifestTombstoneRetiredCount +=
      stats.semanticSceneShadowManifestTombstoneRetiredCount;
  // Phase 7.28：skinned palette content stability probe（累加 / max）。
  agg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceNoneCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount;
  agg.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStablePartSampleCount;
  agg.semanticSceneSubmittedSkinnedPaletteHashChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteHashChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteSourceChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSourceChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax = std::max(
      agg.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax,
      uint64_t(stats.semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax));
  agg.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax = std::max(
      agg.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax,
      uint64_t(
          stats.semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax));
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteCountChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteCountChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount;
  agg.semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount +=
      stats.semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount;
  agg.semanticSceneDirectPaletteAttributionSnapshotHitCount +=
      stats.semanticSceneDirectPaletteAttributionSnapshotHitCount;
  agg.semanticSceneDirectPaletteCaptureTrustedSourceHitCount +=
      stats.semanticSceneDirectPaletteCaptureTrustedSourceHitCount;
  agg.semanticSceneDirectPaletteCaptureTrustedSourceMissCount +=
      stats.semanticSceneDirectPaletteCaptureTrustedSourceMissCount;
  // Phase 7.30 Step A：stale→live 过渡归因。
  agg.semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount;
  agg.semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount +=
      stats
          .semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount;
  agg.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount +=
      stats.semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount;
  agg.semanticSceneDirectManifestObjectCount = std::max(
      agg.semanticSceneDirectManifestObjectCount,
      uint64_t(stats.semanticSceneDirectManifestObjectCount));
  agg.semanticSceneDirectManifestObservedPartCount = std::max(
      agg.semanticSceneDirectManifestObservedPartCount,
      uint64_t(stats.semanticSceneDirectManifestObservedPartCount));
  agg.semanticSceneDirectManifestShadowEligiblePartCount = std::max(
      agg.semanticSceneDirectManifestShadowEligiblePartCount,
      uint64_t(stats.semanticSceneDirectManifestShadowEligiblePartCount));
  agg.semanticSceneDirectObjectCompleteEligibleCount = std::max(
      agg.semanticSceneDirectObjectCompleteEligibleCount,
      uint64_t(stats.semanticSceneDirectObjectCompleteEligibleCount));
  agg.semanticSceneDirectObjectIncompleteByScanCapCount +=
      stats.semanticSceneDirectObjectIncompleteByScanCapCount;
  agg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount +=
      stats.semanticSceneDirectObjectIncompleteByAlphaPolicyCount;
  agg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount +=
      stats.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount;
  agg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount +=
      stats.semanticSceneDirectObjectIncompleteByPacketBuildFailCount;
  agg.semanticSceneDirectObjectIncompleteByAppendFailCount +=
      stats.semanticSceneDirectObjectIncompleteByAppendFailCount;
  agg.semanticSceneDirectSubmittedCompleteObjectCount = std::max(
      agg.semanticSceneDirectSubmittedCompleteObjectCount,
      uint64_t(stats.semanticSceneDirectSubmittedCompleteObjectCount));
  agg.semanticSceneDirectSubmittedPartialObjectCount = std::max(
      agg.semanticSceneDirectSubmittedPartialObjectCount,
      uint64_t(stats.semanticSceneDirectSubmittedPartialObjectCount));
  agg.semanticSceneDirectPreparedSliceAuthoritativeCount +=
      stats.semanticSceneDirectPreparedSliceAuthoritativeCount;
  agg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount +=
      stats.semanticSceneDirectPreparedSliceFallbackLayerIndexCount;
  agg.semanticSceneDirectPreparedSliceMissingCount +=
      stats.semanticSceneDirectPreparedSliceMissingCount;
  agg.semanticScenePreparedProbeAttemptCount +=
      stats.semanticScenePreparedProbeAttemptCount;
  agg.semanticScenePreparedProbeContextReadyCount +=
      stats.semanticScenePreparedProbeContextReadyCount;
  agg.semanticScenePreparedProbeBackingReadableCount +=
      stats.semanticScenePreparedProbeBackingReadableCount;
  agg.semanticScenePreparedSliceRecordedCount +=
      stats.semanticScenePreparedSliceRecordedCount;
  agg.semanticScenePreparedSliceQueryAttemptCount +=
      stats.semanticScenePreparedSliceQueryAttemptCount;
  agg.semanticScenePreparedSliceQueryHitCount +=
      stats.semanticScenePreparedSliceQueryHitCount;
  agg.semanticScenePreparedSliceQueryMissCount +=
      stats.semanticScenePreparedSliceQueryMissCount;
  agg.semanticSceneShadowManifestObjectCount = std::max(
      agg.semanticSceneShadowManifestObjectCount,
      uint64_t(stats.semanticSceneShadowManifestObjectCount));
  agg.semanticSceneShadowManifestPartCount = std::max(
      agg.semanticSceneShadowManifestPartCount,
      uint64_t(stats.semanticSceneShadowManifestPartCount));
  agg.semanticSceneShadowManifestStableObjectCount = std::max(
      agg.semanticSceneShadowManifestStableObjectCount,
      uint64_t(stats.semanticSceneShadowManifestStableObjectCount));
  agg.semanticSceneShadowManifestNewObjectCount +=
      stats.semanticSceneShadowManifestNewObjectCount;
  agg.semanticSceneShadowManifestExpiredObjectCount +=
      stats.semanticSceneShadowManifestExpiredObjectCount;
  agg.semanticSceneShadowManifestFreshPartCount = std::max(
      agg.semanticSceneShadowManifestFreshPartCount,
      uint64_t(stats.semanticSceneShadowManifestFreshPartCount));
  agg.semanticSceneShadowManifestLeaseablePartCount = std::max(
      agg.semanticSceneShadowManifestLeaseablePartCount,
      uint64_t(stats.semanticSceneShadowManifestLeaseablePartCount));
  agg.semanticSceneShadowManifestPoseStalePartCount = std::max(
      agg.semanticSceneShadowManifestPoseStalePartCount,
      uint64_t(stats.semanticSceneShadowManifestPoseStalePartCount));
  agg.semanticSceneShadowManifestSliceStalePartCount = std::max(
      agg.semanticSceneShadowManifestSliceStalePartCount,
      uint64_t(stats.semanticSceneShadowManifestSliceStalePartCount));
  agg.semanticSceneShadowManifestExpiredPartCount +=
      stats.semanticSceneShadowManifestExpiredPartCount;
  agg.semanticSceneShadowManifestMultiSlicePartCount = std::max(
      agg.semanticSceneShadowManifestMultiSlicePartCount,
      uint64_t(stats.semanticSceneShadowManifestMultiSlicePartCount));
  agg.semanticSceneShadowManifestPayload11CChurnCount +=
      stats.semanticSceneShadowManifestPayload11CChurnCount;
  agg.semanticSceneShadowManifestRenderablePartChurnCount +=
      stats.semanticSceneShadowManifestRenderablePartChurnCount;
  agg.semanticSceneShadowManifestCModelPoseHitCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseHitCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseHitCount));
  agg.semanticSceneShadowManifestCModelPoseMissCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseMissCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseMissCount));
  agg.semanticSceneShadowManifestCModelPoseNoRuntimeCount = std::max(
      agg.semanticSceneShadowManifestCModelPoseNoRuntimeCount,
      uint64_t(stats.semanticSceneShadowManifestCModelPoseNoRuntimeCount));
  agg.semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount =
      std::max(
          agg.semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount,
          uint64_t(stats
                       .semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount));
  agg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr =
      stats.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr;
  agg.semanticSceneShadowManifestCModelPoseLastMatrixCount =
      stats.semanticSceneShadowManifestCModelPoseLastMatrixCount;
  agg.semanticSceneShadowManifestCModelPoseLastMatrixHash =
      stats.semanticSceneShadowManifestCModelPoseLastMatrixHash;
  agg.semanticSceneSubmittedObjectJaccardMilli = std::max(
      agg.semanticSceneSubmittedObjectJaccardMilli,
      uint64_t(stats.semanticSceneSubmittedObjectJaccardMilli));
  agg.semanticSceneSubmittedPartJaccardMilli = std::max(
      agg.semanticSceneSubmittedPartJaccardMilli,
      uint64_t(stats.semanticSceneSubmittedPartJaccardMilli));
  agg.semanticSceneVisibleLookupPartLayerHitCount =
      stats.semanticSceneVisibleLookupPartLayerHitCount;
  agg.semanticSceneVisibleLookupSingleFallbackCount =
      stats.semanticSceneVisibleLookupSingleFallbackCount;
  agg.semanticSceneVisibleLookupMissCount =
      stats.semanticSceneVisibleLookupMissCount;
  agg.semanticSceneDirectMainWorldBackingNotCheckedCount +=
      stats.semanticSceneDirectMainWorldBackingNotCheckedCount;
  agg.semanticSceneDirectMainWorldBackingPassCount +=
      stats.semanticSceneDirectMainWorldBackingPassCount;
  agg.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount +=
      stats.semanticSceneDirectMainWorldBackingFailNoRenderablePartCount;
  agg.semanticSceneDirectMainWorldBackingFailLookupMissCount +=
      stats.semanticSceneDirectMainWorldBackingFailLookupMissCount;
  agg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount +=
      stats.semanticSceneDirectMainWorldBackingFailNonMainQueueCount;
  agg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount +=
      stats.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount;
  agg.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailIdentityMismatchCount;
  agg.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount;
  agg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount +=
      stats.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount;
  agg.semanticSceneDirectPaletteHashChurnCount += stats.semanticSceneDirectPaletteHashChurnCount;
  agg.semanticSceneDirectGroupHashChurnCount += stats.semanticSceneDirectGroupHashChurnCount;
  agg.semanticSceneDirectStableGroupHashChurnCount += stats.semanticSceneDirectStableGroupHashChurnCount;
  agg.semanticSceneDirectStream1PtrChurnCount += stats.semanticSceneDirectStream1PtrChurnCount;
  agg.semanticSceneDirectGeometrySourceHashChurnCount += stats.semanticSceneDirectGeometrySourceHashChurnCount;
  agg.semanticSceneDirectSameCasterComparisonCount +=
      stats.semanticSceneDirectSameCasterComparisonCount;
  agg.semanticSceneDirectIdentitySkippedChurnCount +=
      stats.semanticSceneDirectIdentitySkippedChurnCount;
  agg.semanticSceneDirectPaletteRootDeltaSampleCount +=
      stats.semanticSceneDirectPaletteRootDeltaSampleCount;
  agg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount;
  agg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount +=
      stats.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount;
  agg.semanticSceneDirectPaletteRootMaxDeltaMilli = std::max(
      agg.semanticSceneDirectPaletteRootMaxDeltaMilli,
      uint64_t(stats.semanticSceneDirectPaletteRootMaxDeltaMilli));
  agg.semanticSceneDirectSelectionKeyUnitPtrCount +=
      stats.semanticSceneDirectSelectionKeyUnitPtrCount;
  agg.semanticSceneDirectSelectionKeyJHandleCount +=
      stats.semanticSceneDirectSelectionKeyJHandleCount;
  agg.semanticSceneDirectSelectionKeyRuntimeModelCount +=
      stats.semanticSceneDirectSelectionKeyRuntimeModelCount;
  agg.semanticSceneDirectSelectionKeyWorldObjectCount +=
      stats.semanticSceneDirectSelectionKeyWorldObjectCount;
  agg.semanticSceneDirectSelectionKeySceneNodeCount +=
      stats.semanticSceneDirectSelectionKeySceneNodeCount;
  agg.semanticSceneDirectSelectionKeyModelMeshCount +=
      stats.semanticSceneDirectSelectionKeyModelMeshCount;
  agg.semanticSceneDirectSelectionKeyRenderablePartCount +=
      stats.semanticSceneDirectSelectionKeyRenderablePartCount;
  agg.semanticSceneLastAppendedGeometrySourceHash =
      stats.semanticSceneLastAppendedGeometrySourceHash;
  agg.semanticSceneLastAppendedGeometryId =
      stats.semanticSceneLastAppendedGeometryId;
  agg.semanticSceneShadowCastersCount = std::max(
      agg.semanticSceneShadowCastersCount,
      uint64_t(stats.semanticSceneShadowCastersCount));
  agg.semanticSceneReplayDrawsCount = std::max(
      agg.semanticSceneReplayDrawsCount,
      uint64_t(stats.semanticSceneReplayDrawsCount));
  agg.semanticSceneShadowMapDrawnCasters = std::max(
      agg.semanticSceneShadowMapDrawnCasters,
      uint64_t(stats.semanticSceneShadowMapDrawnCasters));
  agg.semanticSceneShadowMapCascadeCulledCount += stats.semanticSceneShadowMapCascadeCulledCount;
  agg.semanticSceneTerrainBoundsCullMode = std::max(
      agg.semanticSceneTerrainBoundsCullMode,
      uint64_t(stats.semanticSceneTerrainBoundsCullMode));
  agg.semanticSceneTerrainBoundsCandidateCount +=
      stats.semanticSceneTerrainBoundsCandidateCount;
  agg.semanticSceneTerrainBoundsProofAcceptedCount +=
      stats.semanticSceneTerrainBoundsProofAcceptedCount;
  agg.semanticSceneTerrainBoundsFailVisibleCount +=
      stats.semanticSceneTerrainBoundsFailVisibleCount;
  agg.semanticSceneTerrainBoundsWouldCullCount +=
      stats.semanticSceneTerrainBoundsWouldCullCount;
  agg.semanticSceneTerrainBoundsAppliedCullCount +=
      stats.semanticSceneTerrainBoundsAppliedCullCount;
  agg.semanticSceneTerrainBoundsC0WouldCullCount +=
      stats.semanticSceneTerrainBoundsC0WouldCullCount;
  agg.semanticSceneTerrainBoundsC1WouldCullCount +=
      stats.semanticSceneTerrainBoundsC1WouldCullCount;
  agg.semanticSceneTerrainBoundsC2WouldCullCount +=
      stats.semanticSceneTerrainBoundsC2WouldCullCount;
  agg.semanticSceneTerrainBoundsC3WouldCullCount +=
      stats.semanticSceneTerrainBoundsC3WouldCullCount;
  agg.semanticSceneObjectBoundsCandidateCount +=
      stats.semanticSceneObjectBoundsCandidateCount;
  agg.semanticSceneObjectBoundsProofAcceptedCount +=
      stats.semanticSceneObjectBoundsProofAcceptedCount;
  agg.semanticSceneObjectBoundsFailVisibleCount +=
      stats.semanticSceneObjectBoundsFailVisibleCount;
  agg.semanticSceneObjectBoundsWouldCullCount +=
      stats.semanticSceneObjectBoundsWouldCullCount;
  agg.semanticSceneObjectBoundsAppliedCullCount +=
      stats.semanticSceneObjectBoundsAppliedCullCount;
  agg.semanticSceneUnionCullMode = std::max(
      agg.semanticSceneUnionCullMode,
      uint64_t(stats.semanticSceneUnionCullMode));
  agg.semanticSceneUnionCullObserveFrameCount +=
      stats.semanticSceneUnionCullObserveFrameCount;
  agg.semanticSceneUnionCullCandidateCount +=
      stats.semanticSceneUnionCullCandidateCount;
  agg.semanticSceneUnionCullProofAcceptedCount +=
      stats.semanticSceneUnionCullProofAcceptedCount;
  agg.semanticSceneUnionCullFailVisibleCount +=
      stats.semanticSceneUnionCullFailVisibleCount;
  agg.semanticSceneUnionCullDynamicConservativeCount +=
      stats.semanticSceneUnionCullDynamicConservativeCount;
  agg.semanticSceneUnionCullUnknownOrStaleCount +=
      stats.semanticSceneUnionCullUnknownOrStaleCount;
  agg.semanticSceneUnionCullC2WouldCullCount +=
      stats.semanticSceneUnionCullC2WouldCullCount;
  agg.semanticSceneUnionCullC3WouldCullCount +=
      stats.semanticSceneUnionCullC3WouldCullCount;
  agg.semanticSceneUnionCullBothFarWouldCullCount +=
      stats.semanticSceneUnionCullBothFarWouldCullCount;
  agg.semanticSceneUnionCullFalseNegativeCount +=
      stats.semanticSceneUnionCullFalseNegativeCount;
  agg.semanticSceneUnionCullFalsePositiveCount +=
      stats.semanticSceneUnionCullFalsePositiveCount;
  agg.semanticSceneShadowMapSkinnedCasterCount = std::max(
      agg.semanticSceneShadowMapSkinnedCasterCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedCasterCount));
  agg.semanticSceneShadowMapSkinnedPreparedCount = std::max(
      agg.semanticSceneShadowMapSkinnedPreparedCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedPreparedCount));
  agg.semanticSceneShadowMapSkinnedInvalidBufferCount = std::max(
      agg.semanticSceneShadowMapSkinnedInvalidBufferCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedInvalidBufferCount));
  agg.semanticSceneShadowMapSkinnedInvalidPipelineCount = std::max(
      agg.semanticSceneShadowMapSkinnedInvalidPipelineCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedInvalidPipelineCount));
  agg.semanticSceneShadowMapSkinnedDrawnCount = std::max(
      agg.semanticSceneShadowMapSkinnedDrawnCount,
      uint64_t(stats.semanticSceneShadowMapSkinnedDrawnCount));
  agg.semanticSceneShadowTaaActive = std::max(
      agg.semanticSceneShadowTaaActive,
      uint64_t(stats.semanticSceneShadowTaaActive));
  agg.semanticSceneReceiverReuseShadowMap = std::max(
      agg.semanticSceneReceiverReuseShadowMap,
      uint64_t(stats.semanticSceneReceiverReuseShadowMap));
  agg.semanticSceneReceiverInputValid = std::max(
      agg.semanticSceneReceiverInputValid,
      uint64_t(stats.semanticSceneReceiverInputValid));
  agg.semanticSceneReceiverInputRejectReason = std::max(
      agg.semanticSceneReceiverInputRejectReason,
      uint64_t(stats.semanticSceneReceiverInputRejectReason));
  agg.semanticSceneReceiverNeedPass = std::max(
      agg.semanticSceneReceiverNeedPass,
      uint64_t(stats.semanticSceneReceiverNeedPass));
  agg.semanticSceneReceiverNeedShadowMap = std::max(
      agg.semanticSceneReceiverNeedShadowMap,
      uint64_t(stats.semanticSceneReceiverNeedShadowMap));
  agg.semanticSceneReceiverHasCompleteShadowMap = std::max(
      agg.semanticSceneReceiverHasCompleteShadowMap,
      uint64_t(stats.semanticSceneReceiverHasCompleteShadowMap));
  agg.semanticSceneReceiverHasUsableDirectionalShadow = std::max(
      agg.semanticSceneReceiverHasUsableDirectionalShadow,
      uint64_t(stats.semanticSceneReceiverHasUsableDirectionalShadow));
  agg.semanticSceneReceiverActiveStrengthMilli = std::max(
      agg.semanticSceneReceiverActiveStrengthMilli,
      uint64_t(stats.semanticSceneReceiverActiveStrengthMilli));
  agg.semanticSceneReceiverUboStrengthMilli = std::max(
      agg.semanticSceneReceiverUboStrengthMilli,
      uint64_t(stats.semanticSceneReceiverUboStrengthMilli));
  agg.semanticSceneReceiverDebugMode = std::max(
      agg.semanticSceneReceiverDebugMode,
      uint64_t(stats.semanticSceneReceiverDebugMode));
  agg.semanticSceneReceiverCsmCascadeCount = std::max(
      agg.semanticSceneReceiverCsmCascadeCount,
      uint64_t(stats.semanticSceneReceiverCsmCascadeCount));
  agg.semanticSceneReceiverZeroStrengthFrameCount +=
      stats.semanticSceneReceiverZeroStrengthFrameCount;
  agg.semanticSceneReceiverDrawnWithZeroStrengthCount +=
      stats.semanticSceneReceiverDrawnWithZeroStrengthCount;
  agg.semanticSceneReceiverNoCompleteShadowMapCount +=
      stats.semanticSceneReceiverNoCompleteShadowMapCount;
  agg.semanticSceneReceiverNoShadowMapImageCount +=
      stats.semanticSceneReceiverNoShadowMapImageCount;
  agg.semanticSceneReceiverNoShadowMapSampleViewCount +=
      stats.semanticSceneReceiverNoShadowMapSampleViewCount;
  agg.semanticSceneReceiverNoCandidateCsmCount +=
      stats.semanticSceneReceiverNoCandidateCsmCount;
  agg.semanticSceneReceiverCsmFallbackToLastGoodCount +=
      stats.semanticSceneReceiverCsmFallbackToLastGoodCount;
  agg.semanticSceneReceiverHoldInvalidCsmCount +=
      stats.semanticSceneReceiverHoldInvalidCsmCount;
  agg.semanticSceneReceiverHoldEmptyReplayCount +=
      stats.semanticSceneReceiverHoldEmptyReplayCount;
  agg.semanticSceneReceiverHoldIdentityChurnCount +=
      stats.semanticSceneReceiverHoldIdentityChurnCount;
  agg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount +=
      stats.semanticSceneReceiverReuseInvalidatedAfterEnsureCount;
  agg.semanticSceneShadowMapRenderSkippedNoResourcesCount +=
      stats.semanticSceneShadowMapRenderSkippedNoResourcesCount;
  agg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount +=
      stats.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount;
  agg.semanticSceneReceiverViewportX = std::max(
      agg.semanticSceneReceiverViewportX,
      uint64_t(stats.semanticSceneReceiverViewportX));
  agg.semanticSceneReceiverViewportY = std::max(
      agg.semanticSceneReceiverViewportY,
      uint64_t(stats.semanticSceneReceiverViewportY));
  agg.semanticSceneReceiverViewportWidth = std::max(
      agg.semanticSceneReceiverViewportWidth,
      uint64_t(stats.semanticSceneReceiverViewportWidth));
  agg.semanticSceneReceiverViewportHeight = std::max(
      agg.semanticSceneReceiverViewportHeight,
      uint64_t(stats.semanticSceneReceiverViewportHeight));
  agg.semanticSceneSkippedUnitsOnlyFilter +=
      stats.semanticSceneSkippedUnitsOnlyFilter;
  agg.semanticSceneAcceptedExplicitResourceOwnerRigid +=
      stats.semanticSceneAcceptedExplicitResourceOwnerRigid;
  agg.semanticSceneRejectedNoVertex += stats.semanticSceneRejectedNoVertex;
  agg.semanticSceneRejectedSkinnedContract +=
      stats.semanticSceneRejectedSkinnedContract;
  agg.semanticSceneRejectedGeometry += stats.semanticSceneRejectedGeometry;
  agg.semanticSceneRejectedGeometryFrameLocal +=
      stats.semanticSceneRejectedGeometryFrameLocal;
  agg.semanticSceneRejectedGeometryPersistent +=
      stats.semanticSceneRejectedGeometryPersistent;
  agg.persistentRejectNoIdentity += stats.persistentRejectNoIdentity;
  agg.persistentRejectUnsupportedMode +=
      stats.persistentRejectUnsupportedMode;
  agg.persistentRejectDynamicSource += stats.persistentRejectDynamicSource;
  agg.persistentRejectAlphaBlend += stats.persistentRejectAlphaBlend;
  agg.persistentRejectMissingStorage += stats.persistentRejectMissingStorage;
  agg.persistentRejectCreateOrBudget +=
      stats.persistentRejectCreateOrBudget;
  agg.semanticFallbackPruned += stats.semanticFallbackPruned;
  agg.semanticFallbackPrunedByHandle +=
      stats.semanticFallbackPrunedByHandle;
  agg.semanticFallbackPrunedByWorldObjectEntry +=
      stats.semanticFallbackPrunedByWorldObjectEntry;
  agg.semanticFallbackPrunedBySceneNode +=
      stats.semanticFallbackPrunedBySceneNode;
  agg.semanticFallbackPrunedByRuntimeModel +=
      stats.semanticFallbackPrunedByRuntimeModel;
  agg.instancedGeometryInstances += stats.persistentInstanceCount;
  agg.instancedGeometryDrawsSaved += stats.instancedGeometryDrawsSaved;
}

void War3PerfMonitor::notePersistentGeometryFrame(
    const PersistentGeometryFrameStats& stats) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  // Keep the exact completed Present interval beside its frame snapshot. This
  // copy runs once per frame, not in a Hook/draw hot path.
  m_currentFrameWorkload.hasPersistentGeometry = true;
  m_currentFrameWorkload.rejectCapacity = stats.rejectCapacity;
  m_currentFrameWorkload.rejectPositionBufferCreate =
      stats.rejectPositionBufferCreate;
  m_currentFrameWorkload.rejectIndexBufferCreate =
      stats.rejectIndexBufferCreate;
  m_currentFrameWorkload.rejectBlendBufferCreate =
      stats.rejectBlendBufferCreate;
  m_currentFrameWorkload.rejectUvBufferCreate =
      stats.rejectUvBufferCreate;
  m_currentFrameWorkload.rejectRegistryInsert = stats.rejectRegistryInsert;
  m_currentFrameWorkload.rejectOther = stats.rejectOther;
  m_currentFrameWorkload.createAttempts = stats.createAttempts;
  m_currentFrameWorkload.bytesNeededTotal = stats.bytesNeededTotal;
  m_currentFrameWorkload.bytesNeededMax = stats.bytesNeededMax;
  m_currentFrameWorkload.bytesNeededLast = stats.bytesNeededLast;
  m_currentFrameWorkload.forceGcRequests = stats.forceGcRequests;
  m_currentFrameWorkload.forceGcNoBytesFreed = stats.forceGcNoBytesFreed;
  m_currentFrameWorkload.forceGcStillInsufficient =
      stats.forceGcStillInsufficient;
  m_currentFrameWorkload.forceGcBytesFreed = stats.forceGcBytesFreed;
  m_currentFrameWorkload.capacityRejectAllCallers =
      stats.capacityRejectAllCallers;
  m_currentFrameWorkload.capacityFastReject = stats.capacityFastReject;
  m_currentFrameWorkload.expiryTokensPopped = stats.expiryTokensPopped;
  m_currentFrameWorkload.expiryTokensRequeued = stats.expiryTokensRequeued;
  m_currentFrameWorkload.expiryStaleTokens = stats.expiryStaleTokens;
  m_currentFrameWorkload.expiryAgeEvictions = stats.expiryAgeEvictions;
  m_currentFrameWorkload.expiryQueueSize = stats.expiryQueueSize;
  m_currentFrameWorkload.bytesCap = stats.bytesCap;
  m_currentFrameWorkload.bytesUsed = stats.bytesUsed;
  m_currentFrameWorkload.bytesEvicted = stats.bytesEvicted;
  m_currentFrameWorkload.bytesEvictedDelta =
      m_shadowBudgetAggregate.persistentDiagnosticsFramesObserved != 0u &&
              stats.bytesEvicted >=
                  m_shadowBudgetAggregate.persistentPoolBytesEvictedLast
          ? stats.bytesEvicted -
                m_shadowBudgetAggregate.persistentPoolBytesEvictedLast
          : 0u;
  m_currentFrameWorkload.liveGeometryCount = stats.liveGeometryCount;
  m_currentFrameWorkload.s1EarlyEntryCount = stats.s1EarlyEntryCount;
  m_currentFrameWorkload.s1EarlyPersistentBackedCount =
      stats.s1EarlyPersistentBackedCount;
  m_currentFrameWorkload.s1EarlyPersistentReverseIndexCount =
      stats.s1EarlyPersistentReverseIndexCount;
  m_currentFrameWorkload.s1EarlyFallbackBackedCount =
      stats.s1EarlyFallbackBackedCount;
  m_currentFrameWorkload.s1EarlyLogicalReferencedBytes =
      stats.s1EarlyLogicalReferencedBytes;
  m_currentFrameWorkload.s1EarlyAcceptedHitCount =
      stats.s1EarlyAcceptedHitCount;
  m_currentFrameWorkload.s1EarlyReplayPublishedCount =
      stats.s1EarlyReplayPublishedCount;
  m_currentFrameWorkload.s1EarlyReplayInstanceCount =
      stats.s1EarlyReplayInstanceCount;
  m_currentFrameWorkload.s1EarlyReplayFallbackCount =
      stats.s1EarlyReplayFallbackCount;
  m_currentFrameWorkload.s1EarlySourceMismatchEvictCount =
      stats.s1EarlySourceMismatchEvictCount;
  m_currentFrameWorkload.s1EarlyReplayClosureMismatch =
      stats.s1EarlyReplayClosureMismatch;

  auto& agg = m_shadowBudgetAggregate;
  const bool hadPreviousGauge =
      agg.persistentDiagnosticsFramesObserved != 0u;

  agg.persistentDiagnosticsFramesObserved++;
  agg.persistentRejectCapacity += stats.rejectCapacity;
  agg.persistentRejectPositionBufferCreate +=
      stats.rejectPositionBufferCreate;
  agg.persistentRejectIndexBufferCreate += stats.rejectIndexBufferCreate;
  agg.persistentRejectBlendBufferCreate += stats.rejectBlendBufferCreate;
  agg.persistentRejectUvBufferCreate += stats.rejectUvBufferCreate;
  agg.persistentRejectRegistryInsert += stats.rejectRegistryInsert;
  agg.persistentRejectOther += stats.rejectOther;

  agg.persistentCreateAttempts += stats.createAttempts;
  agg.persistentPoolBytesNeededTotal += stats.bytesNeededTotal;
  agg.persistentPoolBytesNeededMax =
      (std::max)(agg.persistentPoolBytesNeededMax, stats.bytesNeededMax);
  agg.persistentPoolBytesNeededLast = stats.bytesNeededLast;

  agg.persistentForceGcRequests += stats.forceGcRequests;
  agg.persistentForceGcNoBytesFreed += stats.forceGcNoBytesFreed;
  agg.persistentForceGcStillInsufficient +=
      stats.forceGcStillInsufficient;
  agg.persistentForceGcBytesFreed += stats.forceGcBytesFreed;
  agg.persistentCapacityRejectAllCallers +=
      stats.capacityRejectAllCallers;
  agg.persistentCapacityFastReject += stats.capacityFastReject;

  agg.persistentPoolBytesCapLast = stats.bytesCap;
  agg.persistentPoolBytesUsedLast = stats.bytesUsed;
  agg.persistentPoolBytesUsedMax =
      (std::max)(agg.persistentPoolBytesUsedMax, stats.bytesUsed);
  agg.persistentPoolBytesUsedGaugeSum += stats.bytesUsed;
  if (hadPreviousGauge &&
      stats.bytesEvicted >= agg.persistentPoolBytesEvictedLast) {
    agg.persistentPoolBytesEvictedDelta +=
        stats.bytesEvicted - agg.persistentPoolBytesEvictedLast;
  }
  agg.persistentPoolBytesEvictedLast = stats.bytesEvicted;
  agg.persistentPoolLiveGeometryCountLast = stats.liveGeometryCount;
  agg.persistentPoolLiveGeometryCountMax =
      (std::max)(agg.persistentPoolLiveGeometryCountMax,
                 stats.liveGeometryCount);
  agg.persistentS1EarlyEntryCountLast = stats.s1EarlyEntryCount;
  agg.persistentS1EarlyEntryCountMax =
      (std::max)(agg.persistentS1EarlyEntryCountMax,
                 stats.s1EarlyEntryCount);
  agg.persistentS1EarlyPersistentBackedCountLast =
      stats.s1EarlyPersistentBackedCount;
  agg.persistentS1EarlyPersistentBackedCountMax =
      (std::max)(agg.persistentS1EarlyPersistentBackedCountMax,
                 stats.s1EarlyPersistentBackedCount);
  agg.persistentS1EarlyPersistentReverseIndexCountLast =
      stats.s1EarlyPersistentReverseIndexCount;
  agg.persistentS1EarlyPersistentReverseIndexCountMax =
      (std::max)(agg.persistentS1EarlyPersistentReverseIndexCountMax,
                 stats.s1EarlyPersistentReverseIndexCount);
  if (stats.s1EarlyPersistentReverseIndexCount !=
      stats.s1EarlyPersistentBackedCount) {
    agg.persistentS1EarlyPersistentReverseIndexMismatchFrames++;
  }
  agg.persistentS1EarlyFallbackBackedCountLast =
      stats.s1EarlyFallbackBackedCount;
  agg.persistentS1EarlyFallbackBackedCountMax =
      (std::max)(agg.persistentS1EarlyFallbackBackedCountMax,
                 stats.s1EarlyFallbackBackedCount);
  agg.persistentS1EarlyLogicalReferencedBytesLast =
      stats.s1EarlyLogicalReferencedBytes;
  agg.persistentS1EarlyLogicalReferencedBytesMax =
      (std::max)(agg.persistentS1EarlyLogicalReferencedBytesMax,
                 stats.s1EarlyLogicalReferencedBytes);
  agg.persistentS1EarlyAcceptedHitCount +=
      stats.s1EarlyAcceptedHitCount;
  agg.persistentS1EarlyReplayPublishedCount +=
      stats.s1EarlyReplayPublishedCount;
  agg.persistentS1EarlyReplayInstanceCount +=
      stats.s1EarlyReplayInstanceCount;
  agg.persistentS1EarlyReplayFallbackCount +=
      stats.s1EarlyReplayFallbackCount;
  agg.persistentS1EarlySourceMismatchEvictCount +=
      stats.s1EarlySourceMismatchEvictCount;
  if (stats.s1EarlyReplayClosureMismatch)
    agg.persistentS1EarlyReplayClosureMismatchFrames++;
}

void War3PerfMonitor::noteShadowMapFallback(bool reusedLastComplete,
                                            bool renderedCurrentPartial) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  m_currentFrameWorkload.reusedLastCompleteShadowMap =
      m_currentFrameWorkload.reusedLastCompleteShadowMap ||
      reusedLastComplete;
  m_currentFrameWorkload.renderedCurrentPartialShadowMap =
      m_currentFrameWorkload.renderedCurrentPartialShadowMap ||
      renderedCurrentPartial;
  if (reusedLastComplete)
    m_shadowBudgetAggregate.framesReuseLastComplete++;
  if (renderedCurrentPartial)
    m_shadowBudgetAggregate.framesRenderCurrentPartial++;
}

void War3PerfMonitor::noteShadowReceiverFrame(
    uint32_t replayCasterCount,
    uint64_t replayGeometryWork,
    uint32_t requestedShadowResolution,
    uint32_t effectiveShadowResolution) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  m_currentFrameWorkload.hasShadowReceiver = true;
  m_currentFrameWorkload.replayCasterCount = replayCasterCount;
  m_currentFrameWorkload.replayGeometryWork = replayGeometryWork;
  m_currentFrameWorkload.requestedShadowResolution =
      requestedShadowResolution;
  m_currentFrameWorkload.effectiveShadowResolution =
      effectiveShadowResolution;
  auto& agg = m_shadowBudgetAggregate;
  agg.shadowReceiverFrames++;
  agg.shadowReceiverReplayCasterCountTotal += replayCasterCount;
  agg.shadowReceiverReplayCasterCountMax =
      std::max<uint64_t>(agg.shadowReceiverReplayCasterCountMax,
                         replayCasterCount);
  agg.shadowReceiverReplayGeometryWorkTotal += replayGeometryWork;
  agg.shadowReceiverReplayGeometryWorkMax =
      std::max<uint64_t>(agg.shadowReceiverReplayGeometryWorkMax,
                         replayGeometryWork);
  if (requestedShadowResolution != effectiveShadowResolution)
    agg.shadowReceiverAdaptiveResolutionFrames++;
  agg.shadowReceiverRequestedResolutionLast = requestedShadowResolution;
  agg.shadowReceiverEffectiveResolutionLast = effectiveShadowResolution;
}

void War3PerfMonitor::noteShadowTaaFrame(
    const ShadowTaaFrameTelemetry& telemetry) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  auto& workload = m_currentFrameWorkload;
  workload.hasShadowTaaTelemetry = true;
  workload.shadowTaaRuntimeModuleEnabled = telemetry.runtimeModuleEnabled;
  workload.shadowTaaRequestedMode = telemetry.requestedMode;
  workload.shadowTaaEffectiveMode = telemetry.effectiveMode;
  workload.shadowTaaShaderMode = telemetry.shaderMode;
  workload.shadowTaaBlockedSemanticDynamic |=
      telemetry.blockedSemanticDynamic;
  workload.shadowTaaBlockedSunMotion |= telemetry.blockedSunMotion;
  workload.shadowTaaBlockedCsmFallback |= telemetry.blockedCsmFallback;
  workload.shadowTaaVisibilityExecuted |= telemetry.visibilityExecuted;
  workload.shadowTaaMotionVectorExecuted |= telemetry.motionVectorExecuted;
  workload.shadowTaaReceiverExecuted |= telemetry.receiverExecuted;
  workload.shadowTaaHistoryWriteExecuted |= telemetry.historyWriteExecuted;
  workload.shadowTaaHistoryAdvanced |= telemetry.historyAdvanced;
  workload.shadowTaaHistoryAdvanceSkippedIncomplete |=
      telemetry.historyAdvanceSkippedIncomplete;
  workload.shadowTaaHistoryValidBefore |= telemetry.historyValidBefore;
  workload.shadowTaaHistoryValidAfter |= telemetry.historyValidAfter;
  workload.shadowTaaHistoryInvalidationMask |=
      telemetry.historyInvalidationMask;

  auto& agg = m_shadowBudgetAggregate;
  agg.shadowTaaFramesObserved++;
  agg.shadowTaaRuntimeModuleDisabledFrames +=
      telemetry.runtimeModuleEnabled == 0u ? 1u : 0u;
  switch (telemetry.requestedMode) {
    case 0u:
      agg.shadowTaaRequestedDirectFrames++;
      break;
    case 1u:
      agg.shadowTaaRequestedPrepassFrames++;
      break;
    default:
      agg.shadowTaaRequestedTemporalFrames++;
      break;
  }
  switch (telemetry.effectiveMode) {
    case 0u:
      agg.shadowTaaEffectiveDirectFrames++;
      break;
    case 1u:
      agg.shadowTaaEffectivePrepassFrames++;
      break;
    default:
      agg.shadowTaaEffectiveTemporalFrames++;
      break;
  }
  agg.shadowTaaBlockedSemanticDynamicFrames +=
      telemetry.blockedSemanticDynamic != 0u ? 1u : 0u;
  agg.shadowTaaBlockedSunMotionFrames +=
      telemetry.blockedSunMotion != 0u ? 1u : 0u;
  agg.shadowTaaBlockedCsmFallbackFrames +=
      telemetry.blockedCsmFallback != 0u ? 1u : 0u;
  agg.shadowTaaVisibilityExecutedFrames +=
      telemetry.visibilityExecuted != 0u ? 1u : 0u;
  agg.shadowTaaMotionVectorExecutedFrames +=
      telemetry.motionVectorExecuted != 0u ? 1u : 0u;
  agg.shadowTaaReceiverExecutedFrames +=
      telemetry.receiverExecuted != 0u ? 1u : 0u;
  agg.shadowTaaHistoryWriteExecutedFrames +=
      telemetry.historyWriteExecuted != 0u ? 1u : 0u;
  agg.shadowTaaHistoryAdvancedFrames +=
      telemetry.historyAdvanced != 0u ? 1u : 0u;
  agg.shadowTaaHistoryAdvanceSkippedIncompleteFrames +=
      telemetry.historyAdvanceSkippedIncomplete != 0u ? 1u : 0u;
  agg.shadowTaaHistoryValidBeforeFrames +=
      telemetry.historyValidBefore != 0u ? 1u : 0u;
  agg.shadowTaaHistoryValidAfterFrames +=
      telemetry.historyValidAfter != 0u ? 1u : 0u;
  agg.shadowTaaHistoryInvalidatedFrames +=
      telemetry.historyInvalidationMask != 0u ? 1u : 0u;
  for (uint32_t reason = 0u;
       reason < agg.shadowTaaHistoryInvalidationReasonFrames.size();
       ++reason) {
    if ((telemetry.historyInvalidationMask & (1u << reason)) != 0u)
      agg.shadowTaaHistoryInvalidationReasonFrames[reason]++;
  }
  agg.shadowTaaHistoryInvalidationMaskLast =
      telemetry.historyInvalidationMask;
  agg.shadowTaaRequestedModeLast = telemetry.requestedMode;
  agg.shadowTaaEffectiveModeLast = telemetry.effectiveMode;
  agg.shadowTaaShaderModeLast = telemetry.shaderMode;
}

void War3PerfMonitor::notePointShadowPersistentFrame(
    const PointShadowPersistentFrameTelemetry& telemetry) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard lock(m_mutex);
  auto& agg = m_shadowBudgetAggregate;
  ++agg.pointShadowPersistentFramesObserved;
  agg.pointShadowPersistentLast = telemetry;
}

War3PerfMonitor::~War3PerfMonitor() { shutdown(); }

std::vector<War3PerfMonitor::CpuOnlyScope> &War3PerfMonitor::scopeStack() {
  static thread_local std::vector<CpuOnlyScope> stack;
  return stack;
}

War3PerfMonitor::ThreadCpuState &War3PerfMonitor::threadCpuState() {
  static thread_local ThreadCpuState state;
  return state;
}

bool War3PerfMonitor::readProcessCpu100ns(uint64_t& out100ns) const {
  FILETIME creation = {};
  FILETIME exit = {};
  FILETIME kernel = {};
  FILETIME user = {};
  if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exit, &kernel, &user))
    return false;
  out100ns = combineFileTimes100ns(kernel, user);
  return true;
}

bool War3PerfMonitor::readThreadCpu100ns(HANDLE threadHandle,
                                         uint64_t& out100ns) const {
  if (!threadHandle)
    return false;
  FILETIME creation = {};
  FILETIME exit = {};
  FILETIME kernel = {};
  FILETIME user = {};
  if (!::GetThreadTimes(threadHandle, &creation, &exit, &kernel, &user))
    return false;
  out100ns = combineFileTimes100ns(kernel, user);
  return true;
}

HANDLE War3PerfMonitor::ensureMainThreadHandleLocked(DWORD tid) {
  if (tid == 0)
    return nullptr;
  if (m_mainThreadHandle && m_mainThreadHandleTid == tid)
    return m_mainThreadHandle;

  closeMainThreadHandleLocked();

  DWORD access = THREAD_QUERY_INFORMATION;
#ifdef THREAD_QUERY_LIMITED_INFORMATION
  access |= THREAD_QUERY_LIMITED_INFORMATION;
#endif
  HANDLE threadHandle = ::OpenThread(access, FALSE, tid);
  if (!threadHandle)
    return nullptr;

  m_mainThreadHandle = threadHandle;
  m_mainThreadHandleTid = tid;
  return m_mainThreadHandle;
}

void War3PerfMonitor::closeMainThreadHandleLocked() {
  if (m_mainThreadHandle) {
    ::CloseHandle(m_mainThreadHandle);
    m_mainThreadHandle = nullptr;
  }
  m_mainThreadHandleTid = 0;
}

War3PerfMonitor::CpuProbeSnapshot
War3PerfMonitor::captureCpuProbeLocked(bool frameEnd) {
  CpuProbeSnapshot probe = {};

  const DWORD mainTid = dxvk::war3::hooks::GetMainLoopThreadId();
  probe.mainThreadId = mainTid;
  auto captureMainThread = [&] {
    if (mainTid == 0)
      return;
    HANDLE mainHandle = ensureMainThreadHandleLocked(mainTid);
    uint64_t main100ns = 0;
    if (readThreadCpu100ns(mainHandle, main100ns)) {
      probe.hasMainThread = true;
      probe.mainThreadCpu100ns = main100ns;
    }
  };
  auto captureProcess = [&] {
    uint64_t process100ns = 0;
    if (readProcessCpu100ns(process100ns)) {
      probe.hasProcess = true;
      probe.processCpu100ns = process100ns;
    }
  };

  // 让进程 CPU 采样窗口严格包住主线程窗口：帧首先读 Process，帧尾
  // 最后读 Process。否则 15.625ms 调度量化边界上偶尔会出现
  // mainDelta > processDelta，破坏 lane 闭合。
  if (frameEnd) {
    captureMainThread();
    captureProcess();
  } else {
    captureProcess();
    captureMainThread();
  }

  return probe;
}

void War3PerfMonitor::setDevice(DxvkDevice *device) {
  std::lock_guard lock(m_mutex);
  m_device = device;
  if (m_device != nullptr) {
    m_timestampPeriodNs = static_cast<double>(
        m_device->properties().core.properties.limits.timestampPeriod);
  } else {
    m_timestampPeriodNs = 0.0;
  }
}

void War3PerfMonitor::setResourceCensusAllocator(
    D3D9MemoryAllocator* allocator) {
  std::lock_guard lock(m_mutex);
  m_resourceCensusAllocator = allocator;
}

void War3PerfMonitor::clearResourceCensusAllocator(
    D3D9MemoryAllocator* allocator) {
  if (allocator == nullptr)
    return;
  std::lock_guard lock(m_mutex);
  // 多 device 交叠时，旧 device 不能清掉后来登记的 active allocator。
  if (m_resourceCensusAllocator == allocator)
    m_resourceCensusAllocator = nullptr;
}

void War3PerfMonitor::shutdown() {
  if (m_shutdownStarted.exchange(true, std::memory_order_acq_rel))
    return;

  flushCurrentThreadCpuDeltas();
  const bool drained = waitForPendingExports(15000);
  if (!drained) {
    Logger::warn("War3PerfMonitor: timeout waiting pending exports, forcing "
                 "export worker stop");
  }

  {
    std::lock_guard exportLock(m_exportMutex);
    m_exportStopRequested = true;
    m_exportAbortRequested = !drained;
    if (!drained) {
      while (!m_exportQueue.empty()) {
        m_exportQueue.pop_front();
      }
      m_pendingExportJobs.store(0, std::memory_order_relaxed);
    }
  }
  m_exportCv.notify_all();

  if (m_exportThread.joinable()) {
    m_exportThread.join();
  }
  m_exportWorkerStarted = false;

  {
    std::lock_guard lock(m_mutex);
    m_enabled.store(false, std::memory_order_relaxed);
    m_recording.store(false, std::memory_order_relaxed);
    m_device = nullptr;
    m_timestampPeriodNs = 0.0;
    m_pending.clear();
    m_sections.clear();
    m_sectionIds.clear();
    m_frameHistory.clear();
    m_shadowBudgetAggregate = {};
    m_lastReport = Clock::now();
    m_frameCpuProbeStart = {};
    m_currentFrameWorkload = {};
    m_currentGpuTimestampIntervals.clear();
    closeMainThreadHandleLocked();
  }
}

void War3PerfMonitor::beginFrame() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  flushCurrentThreadCpuDeltas();

  std::lock_guard lock(m_mutex);
  m_frameStart = Clock::now();
  m_frameCpuProbeStart = captureCpuProbeLocked(false);
  m_currentFrameWorkload = {};
  m_currentGpuTimestampIntervals.clear();
  // Present increments the business serial before archiving the perf frame.
  // Snapshot it at frame start so workloadSeries aligns with the draw and
  // ShadowBudget work recorded during this interval, rather than S+1.
  m_currentFrameWorkload.businessFrameSerial =
      m_lastBusinessFrameSerial.load(std::memory_order_relaxed);
  m_inFrame = true;
  // frameIndex is reset with report history; delayed worker/GPU samples must
  // instead use an epoch that is never reused during the monitor lifetime.
  ++m_frameEpochSerial;
  if (m_frameEpochSerial == 0u)
    ++m_frameEpochSerial;
  m_activeFrameEpoch.store(m_frameEpochSerial, std::memory_order_release);

  // 重置当前帧的计数器
  for (auto &s : m_sections) {
    s.cpuCount = 0;
    s.cpuSumMs = 0.0;
    s.cpuMaxMs = 0.0;
    s.gpuCount = 0;
    s.gpuSumMs = 0.0;
    s.gpuMaxMs = 0.0;
  }
  m_threadSections.clear();
  m_profilerCounters = {};
}

void War3PerfMonitor::endFrame() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  flushCurrentThreadCpuDeltas();
  tick();
  archiveFrame();

  const auto now = Clock::now();
  if (now - m_lastReport >= m_reportInterval) {
    report(now);
  }

  if (m_autoExportInterval.count() > 0 &&
      now - m_lastAutoExport >= m_autoExportInterval) {
    m_lastAutoExport = now;
    exportHtmlReport("war3_perf_report_auto.html");
  }
}

War3PerfMonitor::FrameGuard War3PerfMonitor::makeFrameGuard() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return FrameGuard(nullptr);
  return FrameGuard(this);
}

War3PerfMonitor::ScopedSection
War3PerfMonitor::scope(const char *name, const Rc<DxvkCommandList> &ctx) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedSection();
  }

  std::string parentPath;
  const char* leafName = name;
  std::string ownedLeafName;
  // 调用方有历史包袱：部分边界传 leaf，部分传完整路径。完整路径必须
  // 视为绝对路径，不能再拼进当前 TLS parent 而形成 A/B/A/B 的假树。
  if (const char* slash = std::strrchr(name, '/')) {
    parentPath.assign(name, static_cast<size_t>(slash - name));
    ownedLeafName.assign(slash + 1);
    leafName = ownedLeafName.c_str();
  }
  auto &stack = scopeStack();
  if (parentPath.empty() && !stack.empty()) {
    parentPath = stack.back().path;
  }

  uint32_t id = 0;
  {
    std::lock_guard lock(m_mutex);
    const char *parentPtr = parentPath.empty() ? nullptr : parentPath.c_str();
    id = getSectionIdLocked(leafName, parentPtr);
  }
  return ScopedSection(this, id, ctx);
}

War3PerfMonitor::ScopedCpuScope War3PerfMonitor::cpuScope(const char *name) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedCpuScope();
  }
  return ScopedCpuScope(this, name);
}

War3PerfMonitor::ScopedCpuScope
War3PerfMonitor::cpuScope(const char *name, uint32_t sampleWeight) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedCpuScope();
  }
  return ScopedCpuScope(this, name, sampleWeight);
}

bool War3PerfMonitor::detailTraceEnabled() {
  // 环境变量只在首次调用时解析；热路径后续只读一个静态 bool。
  static const bool enabled = [] {
    const std::string value = env::getEnvVar("DXVK_WAR3_PERF_TRACE");
    return value == "detail" || value == "DETAIL" || value == "1";
  }();
  return enabled;
}

uint32_t War3PerfMonitor::detailTraceSamplePeriod() {
  static const uint32_t period = [] {
    const std::string value =
        env::getEnvVar("DXVK_WAR3_PERF_TRACE_SAMPLE_PERIOD");
    const int parsed = value.empty() ? 64 : std::atoi(value.c_str());
    return static_cast<uint32_t>(std::clamp(parsed, 1, 65536));
  }();
  return period;
}

bool War3PerfMonitor::shouldSampleDetailScope(ThreadCpuState& state) const {
  ++state.detailScopeAttempts;
  // TLS 计数器，无 atomic、无锁；采样节拍按线程独立，避免所有 worker 同时命中。
  const uint64_t ordinal = state.detailScopeAttempts;
  if ((ordinal % detailTraceSamplePeriod()) != 0)
    return false;
  ++state.detailScopeSampled;
  return true;
}

War3PerfMonitor::ScopedCpuScope
War3PerfMonitor::cpuDetailScope(const char* name) {
  // Detail tracing is disabled by default. Check its process-lifetime cached
  // switch before touching monitor atomics on every hot-path call.
  if (!detailTraceEnabled())
    return ScopedCpuScope();
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0]) {
    return ScopedCpuScope();
  }
  ThreadCpuState& state = threadCpuState();
  if (!shouldSampleDetailScope(state))
    return ScopedCpuScope();
  return ScopedCpuScope(this, name, detailTraceSamplePeriod(), true);
}

void War3PerfMonitor::addCpuSample(const char *name, double cpuMs,
                                   const char *parentPath, uint32_t calls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0] ||
      !std::isfinite(cpuMs) || cpuMs < 0.0 || calls == 0) {
    return;
  }

  ThreadCpuState &state = threadCpuState();
  const uint32_t id = resolveSectionIdWithTlsCache(state, name, parentPath);
  queueCpuDelta(state, id, cpuMs, calls);
}

void War3PerfMonitor::addCpuSampleToCurrentScope(const char *name,
                                                 double cpuMs,
                                                 uint32_t calls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0] ||
      !std::isfinite(cpuMs) || cpuMs < 0.0 || calls == 0) {
    return;
  }

  ThreadCpuState &state = threadCpuState();
  const auto &stack = scopeStack();
  const char *parentPath =
      stack.empty() ? nullptr : stack.back().path.c_str();
  const uint32_t id =
      resolveSectionIdWithTlsCache(state, name, parentPath);
  queueCpuDelta(state, id, cpuMs, calls);
}

void War3PerfMonitor::addCpuSampleToCurrentScopeChild(
    const char* parentName, const char* childName, double cpuMs,
    uint32_t calls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !parentName ||
      !parentName[0] || !childName || !childName[0] ||
      !std::isfinite(cpuMs) || cpuMs < 0.0 || calls == 0) {
    return;
  }

  ThreadCpuState& state = threadCpuState();
  const auto& stack = scopeStack();
  std::string syntheticParentPath;
  if (!stack.empty()) {
    syntheticParentPath.reserve(stack.back().path.size() + 1u +
                                std::strlen(parentName));
    syntheticParentPath.assign(stack.back().path);
    syntheticParentPath.push_back('/');
  }
  syntheticParentPath.append(parentName);
  const uint32_t id = resolveSectionIdWithTlsCache(
      state, childName, syntheticParentPath.c_str());
  queueCpuDelta(state, id, cpuMs, calls);
}

void War3PerfMonitor::addCpuSampleToCurrentScopeRelative(
    const char* relativeParentPath, const char* name, double cpuMs,
    uint32_t calls) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0] ||
      !std::isfinite(cpuMs) || cpuMs < 0.0 || calls == 0) {
    return;
  }

  ThreadCpuState& state = threadCpuState();
  const auto& stack = scopeStack();
  std::string syntheticParentPath;
  if (!stack.empty())
    syntheticParentPath.assign(stack.back().path);
  if (relativeParentPath && relativeParentPath[0]) {
    if (!syntheticParentPath.empty())
      syntheticParentPath.push_back('/');
    syntheticParentPath.append(relativeParentPath);
  }
  const char* parentPath = syntheticParentPath.empty()
      ? nullptr : syntheticParentPath.c_str();
  const uint32_t id = resolveSectionIdWithTlsCache(state, name, parentPath);
  queueCpuDelta(state, id, cpuMs, calls);
}

void War3PerfMonitor::pushScope(const char *name, uint32_t sampleWeight,
                                bool detailSample) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed) || !name || !name[0])
    return;

  ThreadCpuState& state = threadCpuState();
  if (!detailSample)
    ++state.coarseScopeAttempts;
  auto &stack = scopeStack();
  CpuOnlyScope scope;
  if (const char* slash = std::strrchr(name, '/')) {
    scope.name.assign(slash + 1);
    if (!stack.empty()) {
      // 带路径的历史调用点只提供稳定 leaf 名；处于活动 scope 内时，
      // 完整 path 必须从真实 TLS 父链继续展开。仅在栈空时才保留其
      // 静态绝对前缀。否则该 scope 自身虽能挂对父节点，它的孩子仍会
      // 从旧绝对 path 继续生长，形成中途断根的假调用树。
      scope.parentPath = stack.back().path;
      scope.path = stack.back().path + "/" + scope.name;
    } else {
      scope.parentPath.assign(name, static_cast<size_t>(slash - name));
      scope.path = name;
    }
  } else if (!stack.empty()) {
    scope.name = name;
    scope.parentPath = stack.back().path;
    scope.path = stack.back().path + "/" + scope.name;
  } else {
    scope.name = name;
    scope.path = scope.name;
  }
  scope.dynamicParentPath =
      stack.empty() ? std::string() : stack.back().path;
  scope.start = Clock::now();
  scope.sampleWeight = std::max(1u, sampleWeight);
  scope.frameEpoch = m_activeFrameEpoch.load(std::memory_order_acquire);
  stack.push_back(std::move(scope));
}

void War3PerfMonitor::popScope(uint32_t sampleWeight) {
  auto &stack = scopeStack();
  if (stack.empty())
    return;

  auto scope = std::move(stack.back());
  stack.pop_back();
  // Always unwind the TLS stack even when recording is toggled while a scope
  // is alive. Otherwise one stopped recording session corrupts every later
  // parent path on this thread.
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;
  const double cpuMs = toMs(Clock::now() - scope.start);

  const uint64_t currentEpoch =
      m_activeFrameEpoch.load(std::memory_order_acquire);
  // scope 不得跨 Present frame 归因。跨边界 wall interval（尤其 WaitGate）
  // 只应在自己的 overlay 中展示，不能污染 per-frame hotspot/coverage。
  if (scope.frameEpoch == 0u || scope.frameEpoch != currentEpoch) {
    ++threadCpuState().lateCpuDeltasDropped;
    return;
  }

  // 有效父：TLS 动态栈父优先；栈空时回退到 name 里的静态前缀。
  // 这修复"绝对路径叶节点挂到扁平路径、嵌套上下文 self 失真"的混用问题。
  const std::string &effectiveParent = !scope.dynamicParentPath.empty()
                                           ? scope.dynamicParentPath
                                           : scope.parentPath;
  const char *parentPtr =
      effectiveParent.empty() ? nullptr : effectiveParent.c_str();
  ThreadCpuState &state = threadCpuState();
  const uint32_t id =
      resolveSectionIdWithTlsCache(state, scope.name.c_str(), parentPtr);
  // 详细 sampled scope 以 period 为 Horvitz-Thompson 权重估算总 wall/calls；
  // max 保留真实观测值，不被权重放大。
  const uint32_t weight = std::max(1u, scope.sampleWeight ?
      scope.sampleWeight : sampleWeight);
  CpuDelta delta = {};
  delta.id = id;
  delta.threadId = ::GetCurrentThreadId();
  delta.frameEpoch = scope.frameEpoch;
  delta.cpuMs = cpuMs * weight;
  delta.maxCpuMs = cpuMs;
  delta.calls = weight;
  if (delta.frameEpoch == 0u) {
    ++state.lateCpuDeltasDropped;
    return;
  }
  state.pendingDeltas.emplace_back(std::move(delta));
  if (state.pendingDeltas.size() >= state.sectionIdCache.size())
    flushThreadCpuDeltas(state);
}

uint32_t War3PerfMonitor::getSectionId(const char *name,
                                       const char *parentPath) {
  std::lock_guard lock(m_mutex);
  return getSectionIdLocked(name, parentPath);
}

uint32_t War3PerfMonitor::resolveSectionIdWithTlsCache(ThreadCpuState &state,
                                                       const char *name,
                                                       const char *parentPath) {
  const size_t nameHash = std::hash<std::string_view>{}(name);
  const bool hasParent = parentPath != nullptr && parentPath[0] != '\0';
  const size_t parentHash =
      hasParent ? std::hash<std::string_view>{}(parentPath) : 0;
  constexpr size_t kProbeCount = 8u;
  static_assert((std::tuple_size_v<decltype(state.sectionIdCache)> &
                 (std::tuple_size_v<decltype(state.sectionIdCache)> - 1u)) ==
                    0u,
                "section cache capacity must be a power of two");
  const size_t cacheMask = state.sectionIdCache.size() - 1u;
  const size_t keyHash = nameHash ^
      (parentHash + size_t(0x9e3779b9u) + (nameHash << 6u) +
       (nameHash >> 2u));
  const size_t baseSlot = keyHash & cacheMask;
  size_t emptySlot = state.sectionIdCache.size();
  for (size_t probe = 0u; probe < kProbeCount; ++probe) {
    const size_t slot = (baseSlot + probe) & cacheMask;
    const auto &entry = state.sectionIdCache[slot];
    if (!entry.occupied) {
      emptySlot = slot;
      break;
    }
    if (entry.nameHash == nameHash && entry.parentHash == parentHash &&
        entry.name == name &&
        entry.parentPath == (hasParent ? parentPath : "")) {
      return entry.id;
    }
  }

  uint32_t id = 0;
  {
    std::lock_guard lock(m_mutex);
    id = getSectionIdLocked(name, parentPath);
  }

  // 超过 8 路的罕见碰撞在该 set 内轮换替换；正常稳定报告会落入空槽，
  // 不再因全局 FIFO 把其它完全无关的热点 section 驱逐出去。
  const size_t slot = emptySlot < state.sectionIdCache.size()
      ? emptySlot
      : ((baseSlot + (state.nextCacheSlot++ & (kProbeCount - 1u))) &
         cacheMask);
  state.sectionIdCache[slot].name.assign(name);
  state.sectionIdCache[slot].parentPath.assign(hasParent ? parentPath : "");
  state.sectionIdCache[slot].nameHash = nameHash;
  state.sectionIdCache[slot].parentHash = parentHash;
  state.sectionIdCache[slot].id = id;
  state.sectionIdCache[slot].occupied = true;
  return id;
}

uint32_t War3PerfMonitor::getSectionIdLocked(const char *name,
                                             const char *parentPath) {

  // 创建唯一键：parentPath/name 或 name
  std::string key = parentPath && parentPath[0]
                        ? (std::string(parentPath) + "/" + name)
                        : std::string(name);

  auto it = m_sectionIds.find(key);
  if (it != m_sectionIds.end())
    return it->second;

  const uint32_t id = static_cast<uint32_t>(m_sections.size());
  SectionStats stats = {};
  stats.name = name;
  stats.path = key;
  stats.parentPath = parentPath ? parentPath : "";
  m_sections.emplace_back(std::move(stats));
  m_sectionIds.emplace(key, id);
  return id;
}

void War3PerfMonitor::queueCpuDelta(ThreadCpuState &state, uint32_t id,
                                    double cpuMs, uint32_t calls) {
  CpuDelta delta = {};
  delta.id = id;
  delta.threadId = ::GetCurrentThreadId();
  delta.frameEpoch = m_activeFrameEpoch.load(std::memory_order_acquire);
  if (delta.frameEpoch == 0u) {
    ++state.lateCpuDeltasDropped;
    return;
  }
  delta.cpuMs = cpuMs;
  delta.maxCpuMs = cpuMs;
  delta.calls = calls;
  state.pendingDeltas.emplace_back(std::move(delta));
  if (state.pendingDeltas.size() >= state.sectionIdCache.size()) {
    flushThreadCpuDeltas(state);
  }
}

void War3PerfMonitor::flushThreadCpuDeltas(ThreadCpuState &state) {
  if (state.pendingDeltas.empty() && state.coarseScopeAttempts == 0 &&
      state.detailScopeAttempts == 0 && state.detailScopeSampled == 0 &&
      state.lateCpuDeltasDropped == 0)
    return;

  std::lock_guard lock(m_mutex);
  for (const CpuDelta &delta : state.pendingDeltas) {
    if (delta.id >= m_sections.size())
      continue;

    const uint64_t activeEpoch =
        m_activeFrameEpoch.load(std::memory_order_acquire);
    if (delta.frameEpoch != 0u && delta.frameEpoch == activeEpoch) {
      SectionStats& combined = m_sections[delta.id];
      combined.cpuCount += delta.calls;
      combined.cpuSumMs += delta.cpuMs;
      combined.cpuMaxMs = std::max(combined.cpuMaxMs, delta.maxCpuMs);

      const uint64_t threadKey = (uint64_t(delta.threadId) << 32u) | delta.id;
      SectionStats& byThread = m_threadSections[threadKey];
      byThread.cpuCount += delta.calls;
      byThread.cpuSumMs += delta.cpuMs;
      byThread.cpuMaxMs = std::max(byThread.cpuMaxMs, delta.maxCpuMs);
      continue;
    }

    // Worker TLS batches may flush after the source Present frame has already
    // been archived. Recover them into that exact frame instead of either
    // assigning them to the next frame or silently losing low-volume workers.
    auto frameIt = std::find_if(
        m_frameHistory.rbegin(), m_frameHistory.rend(),
        [&](const FrameSnapshot& frame) {
          return frame.frameEpoch == delta.frameEpoch;
        });
    if (frameIt == m_frameHistory.rend()) {
      ++state.lateCpuDeltasDropped;
      continue;
    }

    auto timingIt = std::find_if(
        frameIt->sections.begin(), frameIt->sections.end(),
        [&](const SectionTiming& timing) {
          return timing.id == delta.id && timing.threadId == delta.threadId;
        });
    if (timingIt == frameIt->sections.end()) {
      SectionTiming timing = {};
      timing.id = delta.id;
      timing.threadId = delta.threadId;
      frameIt->sections.push_back(timing);
      timingIt = std::prev(frameIt->sections.end());
    }
    timingIt->cpuMs += delta.cpuMs;
    timingIt->maxCpuMs = std::max(timingIt->maxCpuMs, delta.maxCpuMs);
    const uint64_t recoveredCalls =
        uint64_t(timingIt->callCount) + uint64_t(delta.calls);
    timingIt->callCount = static_cast<uint32_t>(std::min<uint64_t>(
        recoveredCalls, std::numeric_limits<uint32_t>::max()));
    ++m_profilerCounters.lateCpuDeltasRecovered;
  }
  m_profilerCounters.coarseScopeAttempts += state.coarseScopeAttempts;
  m_profilerCounters.detailScopeAttempts += state.detailScopeAttempts;
  m_profilerCounters.detailScopeSampled += state.detailScopeSampled;
  m_profilerCounters.lateCpuDeltasDropped += state.lateCpuDeltasDropped;
  state.coarseScopeAttempts = 0;
  state.detailScopeAttempts = 0;
  state.detailScopeSampled = 0;
  state.lateCpuDeltasDropped = 0;
  state.pendingDeltas.clear();
}

void War3PerfMonitor::flushCurrentThreadCpuDeltas() {
  ThreadCpuState &state = threadCpuState();
  flushThreadCpuDeltas(state);
}

Rc<DxvkGpuQuery>
War3PerfMonitor::writeTimestamp(const Rc<DxvkCommandList> &ctx) {
  if (!m_device || !ctx || m_timestampPeriodNs <= 0.0) {
    return nullptr;
  }

  Rc<DxvkGpuQuery> gpuQuery = m_device->createRawQuery(VK_QUERY_TYPE_TIMESTAMP);
  if (!gpuQuery) {
    return nullptr;
  }

  const auto handle = gpuQuery->getQuery();
  ctx->resetQuery(handle.first, handle.second);
  ctx->cmdWriteTimestamp(DxvkCmdBuffer::ExecBuffer,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, handle.first,
                         handle.second);

  ctx->track(gpuQuery);
  return gpuQuery;
}

void War3PerfMonitor::submitSample(uint32_t id, uint64_t frameEpoch, double cpuMs,
                                   Rc<DxvkGpuQuery> gpuBegin,
                                   Rc<DxvkGpuQuery> gpuEnd) {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }

  ThreadCpuState &state = threadCpuState();
  if (frameEpoch == 0u || frameEpoch !=
      m_activeFrameEpoch.load(std::memory_order_acquire)) {
    ++state.lateCpuDeltasDropped;
    return;
  }
  if (cpuMs > 0.0) {
    queueCpuDelta(state, id, cpuMs, 1);
  }

  if (!gpuBegin || !gpuEnd)
    return;

  std::lock_guard lock(m_mutex);
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed)) {
    return;
  }
  if (m_pending.size() >= m_maxPendingSamples) {
    ++m_profilerCounters.gpuSamplesDropped;
    static bool s_loggedPendingDrop = false;
    if (!s_loggedPendingDrop) {
      s_loggedPendingDrop = true;
      Logger::warn("War3PerfMonitor: pending samples overflow, dropping GPU "
                   "timing samples");
    }
    return;
  }
  PendingSample sample = {};
  sample.id = id;
  sample.threadId = ::GetCurrentThreadId();
  sample.frameEpoch = frameEpoch;
  sample.cpuMs = cpuMs;
  sample.gpuBegin = std::move(gpuBegin);
  sample.gpuEnd = std::move(gpuEnd);
  m_pending.emplace_back(std::move(sample));
}

void War3PerfMonitor::tick() {
  if (!m_enabled.load(std::memory_order_relaxed) ||
      !m_recording.load(std::memory_order_relaxed))
    return;

  std::lock_guard lock(m_mutex);
  if (!m_device || m_timestampPeriodNs <= 0.0) {
    m_pending.clear();
    return;
  }

  for (size_t i = 0; i < m_pending.size();) {
    auto &sample = m_pending[i];
    if (!sample.gpuBegin || !sample.gpuEnd) {
      i++;
      continue;
    }

    auto vk = m_device->vkd();
    uint64_t beginTs = 0;
    uint64_t endTs = 0;

    const auto beginHandle = sample.gpuBegin->getQuery();
    const auto endHandle = sample.gpuEnd->getQuery();

    VkResult beginRes = vk->vkGetQueryPoolResults(
        vk->device(), beginHandle.first, beginHandle.second, 1,
        sizeof(uint64_t), &beginTs, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (beginRes == VK_NOT_READY) {
      i++;
      continue;
    }

    VkResult endRes = vk->vkGetQueryPoolResults(
        vk->device(), endHandle.first, endHandle.second, 1, sizeof(uint64_t),
        &endTs, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (endRes == VK_NOT_READY) {
      i++;
      continue;
    }

    if (beginRes != VK_SUCCESS || endRes != VK_SUCCESS) {
      ++m_profilerCounters.gpuSamplesDropped;
      m_pending[i] = std::move(m_pending.back());
      m_pending.pop_back();
      continue;
    }

    if (endTs > beginTs && sample.id < m_sections.size()) {
      const double deltaNs = double(endTs - beginTs) * m_timestampPeriodNs;
      const double gpuMs = deltaNs / 1.0e6;
      const uint64_t activeEpoch =
          m_activeFrameEpoch.load(std::memory_order_acquire);
      if (sample.frameEpoch != 0u && sample.frameEpoch == activeEpoch) {
        m_sections[sample.id].gpuCount += 1;
        m_sections[sample.id].gpuSumMs += gpuMs;
        m_sections[sample.id].gpuMaxMs =
            std::max(m_sections[sample.id].gpuMaxMs, gpuMs);
        const uint64_t threadKey = (uint64_t(sample.threadId) << 32u) |
            sample.id;
        SectionStats& byThread = m_threadSections[threadKey];
        byThread.gpuCount += 1;
        byThread.gpuSumMs += gpuMs;
        byThread.gpuMaxMs = std::max(byThread.gpuMaxMs, gpuMs);
        m_currentGpuTimestampIntervals.push_back({beginTs, endTs});
      } else {
        // Timestamp queries are asynchronous by design. A result becoming
        // ready next frame is valid, not a cross-frame scope. Backfill the
        // archived source frame using its monotonic epoch.
        auto frameIt = std::find_if(
            m_frameHistory.rbegin(), m_frameHistory.rend(),
            [&](const FrameSnapshot& frame) {
              return frame.frameEpoch == sample.frameEpoch;
            });
        if (frameIt == m_frameHistory.rend()) {
          ++m_profilerCounters.gpuSamplesDropped;
        } else {
          auto timingIt = std::find_if(
              frameIt->sections.begin(), frameIt->sections.end(),
              [&](const SectionTiming& timing) {
                return timing.id == sample.id &&
                       timing.threadId == sample.threadId;
              });
          if (timingIt == frameIt->sections.end()) {
            SectionTiming timing = {};
            timing.id = sample.id;
            timing.threadId = sample.threadId;
            frameIt->sections.push_back(timing);
            timingIt = std::prev(frameIt->sections.end());
          }
          timingIt->gpuMs += gpuMs;
          timingIt->maxGpuMs = std::max(timingIt->maxGpuMs, gpuMs);
          if (timingIt->gpuCount != std::numeric_limits<uint32_t>::max())
            ++timingIt->gpuCount;
          frameIt->gpuTimestampIntervals.push_back({beginTs, endTs});
          frameIt->totalGpuMs = GpuTimestampIntervalUnionMs(
              frameIt->gpuTimestampIntervals, m_timestampPeriodNs);
          frameIt->hasGpuTiming = true;
          ++m_profilerCounters.lateGpuSamplesRecovered;
        }
      }
    }

    m_pending[i] = std::move(m_pending.back());
    m_pending.pop_back();
  }
}

void War3PerfMonitor::archiveFrame() {
  std::lock_guard lock(m_mutex);
  if (!m_inFrame)
    return;

  const uint64_t frameEpoch =
      m_activeFrameEpoch.load(std::memory_order_acquire);
  m_inFrame = false;
  m_activeFrameEpoch.store(0u, std::memory_order_release);

  FrameSnapshot snapshot;
  snapshot.frameIndex = m_frameIndex++;
  snapshot.frameEpoch = frameEpoch;
  snapshot.timestamp = Clock::now();
  snapshot.workload = m_currentFrameWorkload;
  snapshot.totalCpuMs = toMs(snapshot.timestamp - m_frameStart);
  CpuProbeSnapshot endProbe = captureCpuProbeLocked(true);

  if (m_frameCpuProbeStart.hasProcess && endProbe.hasProcess &&
      endProbe.processCpu100ns >= m_frameCpuProbeStart.processCpu100ns) {
    const uint64_t delta100ns =
        endProbe.processCpu100ns - m_frameCpuProbeStart.processCpu100ns;
    snapshot.processCpuMs = static_cast<double>(delta100ns) / 10000.0;
    snapshot.hasProcessCpu = true;
  }

  if (m_frameCpuProbeStart.hasMainThread && endProbe.hasMainThread &&
      m_frameCpuProbeStart.mainThreadId != 0 &&
      m_frameCpuProbeStart.mainThreadId == endProbe.mainThreadId &&
      endProbe.mainThreadCpu100ns >= m_frameCpuProbeStart.mainThreadCpu100ns) {
    const uint64_t delta100ns =
        endProbe.mainThreadCpu100ns - m_frameCpuProbeStart.mainThreadCpu100ns;
    snapshot.mainThreadCpuMs = static_cast<double>(delta100ns) / 10000.0;
    snapshot.hasMainThreadCpu = true;
  }

  if (snapshot.hasProcessCpu && snapshot.hasMainThreadCpu &&
      snapshot.mainThreadCpuMs > snapshot.processCpuMs) {
    snapshot.mainThreadCpuMs = snapshot.processCpuMs;
    snapshot.mainThreadCpuClampedToProcess = true;
  }

  if (snapshot.hasProcessCpu && snapshot.hasMainThreadCpu) {
    snapshot.workerThreadsCpuMs =
        std::max(0.0, snapshot.processCpuMs - snapshot.mainThreadCpuMs);
  } else if (snapshot.hasProcessCpu) {
    snapshot.workerThreadsCpuMs = snapshot.processCpuMs;
  }

  bool hasGpuTiming = false;
  // 保留线程维度，而不是在 FrameSnapshot 前把 worker 贡献压扁到同一节点。
  // 键只在批量 flush 时写入，避免默认粗粒度 scope 在热路径加锁。
  for (const auto& entry : m_threadSections) {
    const uint32_t id = static_cast<uint32_t>(entry.first & 0xffffffffu);
    if (id >= m_sections.size())
      continue;
    const SectionStats& s = entry.second;
    if (s.cpuCount == 0 && s.gpuCount == 0)
      continue;
    SectionTiming timing;
    timing.id = id;
    timing.threadId = static_cast<uint32_t>(entry.first >> 32u);
    timing.cpuMs = s.cpuSumMs;
    timing.gpuMs = s.gpuSumMs;
    timing.maxCpuMs = s.cpuMaxMs;
    timing.maxGpuMs = s.gpuMaxMs;
    timing.callCount = static_cast<uint32_t>(std::min<uint64_t>(
        s.cpuCount, std::numeric_limits<uint32_t>::max()));
    timing.gpuCount = static_cast<uint32_t>(std::min<uint64_t>(
        s.gpuCount, std::numeric_limits<uint32_t>::max()));
    snapshot.sections.push_back(std::move(timing));
    hasGpuTiming = hasGpuTiming || s.gpuCount > 0u;
  }
  snapshot.gpuTimestampIntervals = std::move(m_currentGpuTimestampIntervals);
  snapshot.totalGpuMs = GpuTimestampIntervalUnionMs(
      snapshot.gpuTimestampIntervals, m_timestampPeriodNs);
  snapshot.hasGpuTiming = hasGpuTiming;

  m_frameHistory.push_back(std::move(snapshot));

  // 保持环形缓冲区大小
  while (m_frameHistory.size() > m_maxHistorySize) {
    m_frameHistory.pop_front();
  }
}

void War3PerfMonitor::resetHistory() {
  flushCurrentThreadCpuDeltas();
  std::lock_guard lock(m_mutex);
  m_frameHistory.clear();
  m_pending.clear();
  m_shadowBudgetAggregate = {};
  m_frameIndex = 0;
  m_inFrame = false;
  m_frameCpuProbeStart = {};
  m_currentFrameWorkload = {};
  m_currentGpuTimestampIntervals.clear();
  m_lastReport = Clock::now();
  m_lastAutoExport = Clock::now();
}

uint32_t War3PerfMonitor::getPendingExportCount() const {
  return m_pendingExportJobs.load(std::memory_order_relaxed);
}

bool War3PerfMonitor::waitForPendingExports(uint32_t timeoutMs) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  std::unique_lock lock(m_exportMutex);
  auto done = [this]() {
    return m_exportQueue.empty() && m_exportInFlight == 0;
  };

  if (done())
    return true;

  if (timeoutMs == 0)
    return false;

  return m_exportCv.wait_until(lock, deadline, done);
}

void War3PerfMonitor::report(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(m_mutex);
  if (m_sections.empty())
    return;

  bool hasAny = false;
  for (const auto &s : m_sections) {
    if (s.cpuCount > 0 || s.gpuCount > 0) {
      hasAny = true;
      break;
    }
  }

  if (!hasAny) {
    m_lastReport = now;
    return;
  }

  const double intervalSec =
      std::chrono::duration<double>(now - m_lastReport).count();

  war3dbg::Print("DXVK War3Perf: %.1fs, %zu frames in history\n", intervalSec,
                 m_frameHistory.size());

  m_lastReport = now;
}

War3PerfMonitor::ExportSnapshot
War3PerfMonitor::captureExportSnapshotLocked() const {
  ExportSnapshot snapshot;
  snapshot.frameHistory = m_frameHistory;
  snapshot.sections = m_sections;
  snapshot.shadowBudgetAggregate = m_shadowBudgetAggregate;
  snapshot.processId = static_cast<uint32_t>(GetCurrentProcessId());
  snapshot.mainThreadId = m_mainThreadHandleTid;
  FILETIME processCreate = {};
  FILETIME processExit = {};
  FILETIME processKernel = {};
  FILETIME processUser = {};
  if (GetProcessTimes(GetCurrentProcess(), &processCreate, &processExit,
                      &processKernel, &processUser)) {
    ULARGE_INTEGER createTime = {};
    createTime.LowPart = processCreate.dwLowDateTime;
    createTime.HighPart = processCreate.dwHighDateTime;
    snapshot.processStartFileTime100ns = createTime.QuadPart;
  }
  snapshot.gpuSkinHooksEnabled = gpu_skin::NativeBridgeHooksEnabled();
  snapshot.gpuSkinRuntimeConfig = gpu_skin::GetNativeBridgeRuntimeConfig();
  snapshot.gpuSkinNativeFingerprint = gpu_skin::GetNativeBridgeFingerprint();
  snapshot.gpuSkinNativeCounters = gpu_skin::GetNativeBridgeCounters();
  snapshot.resourceResidency =
      resource_census::CaptureSnapshot(m_resourceCensusAllocator);
  snapshot.worldObjectsMaintenanceTiming =
      render::QueryWorldObjectsPhase1Telemetry();
  snapshot.profilerCounters = m_profilerCounters;
  snapshot.detailTraceEnabled = detailTraceEnabled();
  snapshot.detailTraceSamplePeriod = detailTraceSamplePeriod();
  {
    const auto &identity = GetOwnModuleIdentity();
    snapshot.meta.dllSha256Hex = identity.first;
    snapshot.meta.dllFileSize = identity.second;
    snapshot.meta.buildTimestamp = __DATE__ " " __TIME__;
    snapshot.meta.runtimeProfile = runtime::GetWar3RuntimeProfileName();
    snapshot.meta.enabledModulesCsv = runtime::GetWar3RuntimeEnabledModulesCsv();
    snapshot.meta.disabledModulesCsv =
        runtime::GetWar3RuntimeDisabledModulesCsv();
    snapshot.meta.perfEnvJson = BuildPerfEnvJson();
    snapshot.meta.perfFrameEpoch = m_frameEpochSerial;
    snapshot.meta.businessFrameSerial =
        m_lastBusinessFrameSerial.load(std::memory_order_relaxed);
    snapshot.meta.monitorDisabledByEnv =
        !m_enabled.load(std::memory_order_relaxed);
  }
  const auto modelDedup =
      model::ModelInstanceRegistry::instance().sameFrameIdentityDedupStats();
  snapshot.modelIdentitySameFrameDedup = {
      modelDedup.attempts,
      modelDedup.hits,
      modelDedup.missMissingAlias,
      modelDedup.missCrossFrame,
      modelDedup.missNoBatchProof,
      modelDedup.missIncomplete,
      modelDedup.missInputMismatch,
      modelDedup.missAliasConflict,
      modelDedup.missRuntimeOwner,
      modelDedup.batchMarked,
  };
  const auto shadowDedup =
      render::ShadowObjectRegistry::instance().sameFrameIdentityDedupStats();
  snapshot.shadowIdentitySameFrameDedup = {
      shadowDedup.attempts,
      shadowDedup.hits,
      shadowDedup.missMissingAlias,
      shadowDedup.missCrossFrame,
      shadowDedup.missNoBatchProof,
      shadowDedup.missIncomplete,
      shadowDedup.missInputMismatch,
      shadowDedup.missAliasConflict,
      0u,
      shadowDedup.batchMarked,
  };
  return snapshot;
}

std::string War3PerfMonitor::resolveExportPath(const std::string &outputPath) const {
  std::string finalPath = outputPath;
  const std::string baseDir = getWarVkBaseDir();
  if (baseDir.empty()) {
    return finalPath;
  }

  const std::string tempDir = baseDir + "Temp\\";
  const std::string logDir = baseDir + "Log\\";
  CreateDirectoryA(baseDir.c_str(), nullptr);
  CreateDirectoryA(tempDir.c_str(), nullptr);
  CreateDirectoryA(logDir.c_str(), nullptr);

  std::string fileName = outputPath.empty() ? "war3_perf_report.html" : outputPath;
  const size_t slashPos = fileName.find_last_of("\\/");
  if (slashPos == std::string::npos) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char timeBuf[64] = {};
    std::snprintf(timeBuf, sizeof(timeBuf), "_%04u_%02u_%02u_%02u_%02u_%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    const size_t dotPos = fileName.find_last_of('.');
    if (dotPos != std::string::npos) {
      fileName = fileName.substr(0, dotPos) + timeBuf + fileName.substr(dotPos);
    } else {
      fileName += timeBuf;
    }
    finalPath = logDir + fileName;
  } else {
    finalPath = baseDir + fileName;
  }
  return finalPath;
}

double War3PerfMonitor::resolveReportWindowSec() const {
  double windowSec = 0.0;
  const std::string windowVar = env::getEnvVar("DXVK_WAR3_PERF_WINDOW_SEC");
  if (!windowVar.empty()) {
    windowSec = std::strtod(windowVar.c_str(), nullptr);
  }
  if (windowSec <= 0.0) {
    windowSec = 1200.0;
  }
  return windowSec;
}

void War3PerfMonitor::ensureExportWorkerLocked() {
  if (m_exportWorkerStarted)
    return;

  m_exportStopRequested = false;
  m_exportAbortRequested = false;
  m_exportInFlight = 0;
  m_exportThread = std::thread(&War3PerfMonitor::exportWorkerLoop, this);
  m_exportWorkerStarted = true;
}

void War3PerfMonitor::enqueueExportJob(ExportJob &&job) {
  std::unique_lock lock(m_exportMutex);
  if (m_exportStopRequested || m_exportAbortRequested)
    return;

  ensureExportWorkerLocked();

  if (!job.isAuto) {
    while (m_exportQueue.size() >= 2) {
      auto autoIt = std::find_if(
          m_exportQueue.begin(), m_exportQueue.end(),
          [](const ExportJob &queued) { return queued.isAuto; });
      if (autoIt != m_exportQueue.end()) {
        m_exportQueue.erase(autoIt);
        decrementPendingJobs(m_pendingExportJobs);
        continue;
      }
      m_exportCv.wait(lock, [this]() {
        return m_exportQueue.size() < 2 || m_exportAbortRequested ||
               m_exportStopRequested;
      });
      if (m_exportAbortRequested || m_exportStopRequested) {
        return;
      }
    }
  } else if (m_exportQueue.size() >= 2) {
    auto autoIt = std::find_if(
        m_exportQueue.begin(), m_exportQueue.end(),
        [](const ExportJob &queued) { return queued.isAuto; });
    if (autoIt != m_exportQueue.end()) {
      m_exportQueue.erase(autoIt);
      decrementPendingJobs(m_pendingExportJobs);
      Logger::warn("War3PerfMonitor: dropped stale auto export job");
    } else {
      Logger::warn(
          "War3PerfMonitor: auto export skipped because queue is occupied by "
          "manual exports");
      return;
    }
  }

  m_exportQueue.emplace_back(std::move(job));
  m_pendingExportJobs.fetch_add(1, std::memory_order_relaxed);
  lock.unlock();
  m_exportCv.notify_all();
}

void War3PerfMonitor::exportWorkerLoop() {
  for (;;) {
    ExportJob job;
    {
      std::unique_lock lock(m_exportMutex);
      m_exportCv.wait(lock, [this]() {
        return m_exportAbortRequested || !m_exportQueue.empty() ||
               m_exportStopRequested;
      });

      if (m_exportAbortRequested) {
        break;
      }

      if (m_exportQueue.empty()) {
        if (m_exportStopRequested) {
          break;
        }
        continue;
      }

      job = std::move(m_exportQueue.front());
      m_exportQueue.pop_front();
      m_exportInFlight += 1;
    }

    try {
      processExportJob(job);
    } catch (const std::exception &e) {
      Logger::err("War3PerfMonitor: export worker exception");
      Logger::err(e.what());
    } catch (...) {
      Logger::err("War3PerfMonitor: export worker unknown exception");
    }

    {
      std::lock_guard lock(m_exportMutex);
      if (m_exportInFlight > 0) {
        m_exportInFlight -= 1;
      }
    }
    decrementPendingJobs(m_pendingExportJobs);
    m_exportCv.notify_all();
  }

  {
    std::lock_guard lock(m_exportMutex);
    m_exportInFlight = 0;
  }
  m_exportCv.notify_all();
}

std::string War3PerfMonitor::generateJsonData(double windowSec) const {
  ExportSnapshot snapshot;
  {
    std::lock_guard lock(const_cast<std::mutex &>(m_mutex));
    snapshot = captureExportSnapshotLocked();
  }
  return generateJsonDataFromSnapshot(snapshot, windowSec);
}

std::string War3PerfMonitor::generateJsonDataFromSnapshot(
    const ExportSnapshot &snapshot, double windowSec) const {
  const auto now = Clock::now();
  std::vector<const FrameSnapshot *> frames;
  frames.reserve(snapshot.frameHistory.size());

  if (windowSec > 0.0) {
    for (const auto &f : snapshot.frameHistory) {
      const double ageSec =
          std::chrono::duration<double>(now - f.timestamp).count();
      if (ageSec <= windowSec) {
        frames.push_back(&f);
      }
    }
  } else {
    for (const auto &f : snapshot.frameHistory) {
      frames.push_back(&f);
    }
  }

  if (frames.empty()) {
    for (const auto &f : snapshot.frameHistory) {
      frames.push_back(&f);
    }
  }

  std::ostringstream json;
  json << std::fixed << std::setprecision(3);
  json << "{\n";
  json << "  \"frameCount\": " << frames.size() << ",\n";
  if (!frames.empty()) {
    const double windowUsed =
        std::chrono::duration<double>(now - frames.front()->timestamp).count();
    json << "  \"windowSec\": " << windowUsed << ",\n";
  } else {
    json << "  \"windowSec\": 0.0,\n";
  }

  // schema v9：明确区分 Present frame wall、OS thread CPU、scope wall，
  // 并保留 GPU timestamp 缺失状态与稳定快/慢帧核心。
  json << "  \"schemaVersion\": 9,\n";
  json << "  \"meta\": {\n";
  json << "    \"dllSha256\": \"" << snapshot.meta.dllSha256Hex
       << "\",\n";
  json << "    \"dllFileSize\": " << snapshot.meta.dllFileSize << ",\n";
  json << "    \"buildTimestamp\": \"" << snapshot.meta.buildTimestamp
       << "\",\n";
  json << "    \"runtimeProfile\": \"" << snapshot.meta.runtimeProfile
       << "\",\n";
  json << "    \"enabledModules\": \"" << snapshot.meta.enabledModulesCsv
       << "\",\n";
  json << "    \"disabledModules\": \"" << snapshot.meta.disabledModulesCsv
       << "\",\n";
  json << "    \"env\": " << snapshot.meta.perfEnvJson << ",\n";
  json << "    \"perfFrameEpoch\": " << snapshot.meta.perfFrameEpoch
       << ",\n";
  json << "    \"businessFrameSerial\": "
       << snapshot.meta.businessFrameSerial << ",\n";
  json << "    \"monitorDisabledByEnv\": "
       << (snapshot.meta.monitorDisabledByEnv ? "true" : "false") << "\n";
  json << "  },\n";

  const auto shadowLifecycle =
      render::QueryShadowCasterLifecycleDiagnostics();
  json << "  \"shadowLifecycle\": {\n";
  json << "    \"publishedCount\": "
       << shadowLifecycle.publishedCount << ",\n";
  json << "    \"acknowledgedFreshCount\": "
       << shadowLifecycle.acknowledgedFreshCount << ",\n";
  json << "    \"activeCount\": "
       << shadowLifecycle.activeCount << ",\n";
  json << "    \"queueOverflowCount\": "
       << shadowLifecycle.queueOverflowCount << ",\n";
  json << "    \"lastEventSerial\": "
       << shadowLifecycle.lastEventSerial << ",\n";
  json << "    \"stagePolicyRevision\": "
       << shadowLifecycle.stagePolicyRevision << ",\n";
  json << "    \"hiddenCount\": "
       << shadowLifecycle.hiddenCount << ",\n";
  json << "    \"removedCount\": "
       << shadowLifecycle.removedCount << ",\n";
  json << "    \"stageDisabledCount\": "
       << shadowLifecycle.stageDisabledCount << ",\n";
  json << "    \"replacedCount\": "
       << shadowLifecycle.replacedCount << "\n";
  json << "  },\n";

  const auto shadowStageLifecycle =
      render::QueryShadowStageLifecycleSnapshot();
  const auto writeStageLifecycleArray =
      [&json](const char* name, const auto& values, bool trailingComma) {
        json << "    \"" << name << "\": [";
        for (size_t i = 0u; i < values.size(); ++i) {
          if (i != 0u)
            json << ", ";
          json << values[i];
        }
        json << "]" << (trailingComma ? "," : "") << "\n";
      };
  json << "  \"shadowStageLifecycle\": {\n";
  json << "    \"enabled\": "
       << (shadowStageLifecycle.enabled ? "true" : "false") << ",\n";
  json << "    \"stageBins\": "
       << render::kShadowStageLifecycleStageCount
       << ",\n";
  json << "    \"overflowBin\": "
       << render::kShadowStageLifecycleStageCount << ",\n";
  writeStageLifecycleArray(
      "attempt", shadowStageLifecycle.attempt, true);
  writeStageLifecycleArray(
      "policyAccepted", shadowStageLifecycle.policyAccepted, true);
  writeStageLifecycleArray(
      "metadataClassified", shadowStageLifecycle.metadataClassified, true);
  writeStageLifecycleArray(
      "metadataCaptured", shadowStageLifecycle.metadataCaptured, true);
  writeStageLifecycleArray(
      "metadataApplied", shadowStageLifecycle.metadataApplied, true);
  writeStageLifecycleArray(
      "canonicalPublished",
      shadowStageLifecycle.canonicalPublished,
      true);
  writeStageLifecycleArray(
      "replayPrepared", shadowStageLifecycle.replayPrepared, true);
  json << "    \"cascadeDrawn\": [\n";
  for (size_t cascade = 0u;
       cascade < shadowStageLifecycle.cascadeDrawn.size();
       ++cascade) {
    json << "      [";
    const auto& values = shadowStageLifecycle.cascadeDrawn[cascade];
    for (size_t i = 0u; i < values.size(); ++i) {
      if (i != 0u)
        json << ", ";
      json << values[i];
    }
    json << "]"
         << (cascade + 1u < shadowStageLifecycle.cascadeDrawn.size()
                 ? ","
                 : "")
         << "\n";
  }
  json << "    ],\n";
  writeStageLifecycleArray(
      "retiredHidden", shadowStageLifecycle.retiredHidden, true);
  writeStageLifecycleArray(
      "retiredRemoved", shadowStageLifecycle.retiredRemoved, true);
  writeStageLifecycleArray(
      "retiredStageDisabled",
      shadowStageLifecycle.retiredStageDisabled,
      true);
  writeStageLifecycleArray(
      "retiredReplaced", shadowStageLifecycle.retiredReplaced, true);
  writeStageLifecycleArray(
      "rejectedOverlay", shadowStageLifecycle.rejectedOverlay, true);
  writeStageLifecycleArray(
      "rejectedLightning", shadowStageLifecycle.rejectedLightning, true);
  writeStageLifecycleArray(
      "rejectedStage10Owner",
      shadowStageLifecycle.rejectedStage10Owner,
      true);
  writeStageLifecycleArray(
      "rejectedStage13Owner",
      shadowStageLifecycle.rejectedStage13Owner,
      true);
  writeStageLifecycleArray(
      "rejectedAlphaPayload",
      shadowStageLifecycle.rejectedAlphaPayload,
      false);
  json << "  },\n";

  // 计算帧级统计
  double avgCpu = 0.0, avgGpu = 0.0, maxCpu = 0.0, minCpu = 999999.0;
  double totalCpu = 0.0;
  double totalProcessCpuMs = 0.0;
  double totalMainThreadCpuMs = 0.0;
  double totalWorkerThreadsCpuMs = 0.0;
  uint32_t processCpuSamples = 0;
  uint32_t mainThreadCpuSamples = 0;
  uint32_t workerThreadsCpuSamples = 0;
  uint32_t gpuSamples = 0;
  uint32_t mainThreadCpuClampFrames = 0;
  std::vector<double> cpuFrameTimes;
  std::vector<double> gpuFrameTimes;
  cpuFrameTimes.reserve(frames.size());
  gpuFrameTimes.reserve(frames.size());
  uint32_t jank16 = 0; // > 16.67ms (60fps budget)
  uint32_t jank33 = 0; // > 33.33ms (30fps budget)
  uint32_t jank50 = 0; // > 50.00ms (severe stutter)
  for (const auto *f : frames) {
    avgCpu += f->totalCpuMs;
    if (f->hasGpuTiming) {
      avgGpu += f->totalGpuMs;
      gpuFrameTimes.push_back(f->totalGpuMs);
      ++gpuSamples;
    }
    maxCpu = std::max(maxCpu, f->totalCpuMs);
    minCpu = std::min(minCpu, f->totalCpuMs);
    totalCpu += f->totalCpuMs;
    cpuFrameTimes.push_back(f->totalCpuMs);
    if (f->hasProcessCpu) {
      totalProcessCpuMs += f->processCpuMs;
      processCpuSamples++;
    }
    if (f->hasMainThreadCpu) {
      totalMainThreadCpuMs += f->mainThreadCpuMs;
      mainThreadCpuSamples++;
    }
    if (f->mainThreadCpuClampedToProcess)
      ++mainThreadCpuClampFrames;
    if (f->hasProcessCpu) {
      totalWorkerThreadsCpuMs += f->workerThreadsCpuMs;
      workerThreadsCpuSamples++;
    }
    if (f->totalCpuMs > 16.67)
      ++jank16;
    if (f->totalCpuMs > 33.33)
      ++jank33;
    if (f->totalCpuMs > 50.0)
      ++jank50;
  }
  if (!frames.empty()) {
    avgCpu /= frames.size();
  }
  if (gpuSamples != 0u)
    avgGpu /= static_cast<double>(gpuSamples);
  const double avgProcessCpuMs =
      processCpuSamples ? (totalProcessCpuMs / static_cast<double>(processCpuSamples)) : 0.0;
  const double avgMainThreadCpuMs =
      mainThreadCpuSamples ? (totalMainThreadCpuMs / static_cast<double>(mainThreadCpuSamples))
                           : 0.0;
  const double avgWorkerThreadsCpuMs =
      workerThreadsCpuSamples
          ? (totalWorkerThreadsCpuMs / static_cast<double>(workerThreadsCpuSamples))
          : std::max(0.0, avgProcessCpuMs - avgMainThreadCpuMs);
  const double processCpuCoveragePct =
      frames.empty() ? 0.0
                     : (static_cast<double>(processCpuSamples) /
                        static_cast<double>(frames.size()) * 100.0);
  const double mainThreadCpuCoveragePct =
      frames.empty() ? 0.0
                     : (static_cast<double>(mainThreadCpuSamples) /
                        static_cast<double>(frames.size()) * 100.0);
  const double processParallelismCores =
      (avgCpu > 1e-6) ? (avgProcessCpuMs / avgCpu) : 0.0;
  const double mainThreadShareOfProcessPct =
      (avgProcessCpuMs > 1e-6)
          ? std::clamp(avgMainThreadCpuMs / avgProcessCpuMs * 100.0, 0.0, 100.0)
          : 0.0;
  const double workerThreadsShareOfProcessPct =
      (avgProcessCpuMs > 1e-6)
          ? std::clamp(avgWorkerThreadsCpuMs / avgProcessCpuMs * 100.0, 0.0, 100.0)
          : 0.0;

  auto percentile = [](std::vector<double> samples, double p) -> double {
    if (samples.empty())
      return 0.0;
    p = std::clamp(p, 0.0, 100.0);
    std::sort(samples.begin(), samples.end());
    const double rank = (p / 100.0) * static_cast<double>(samples.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(rank));
    const size_t hi = static_cast<size_t>(std::ceil(rank));
    if (lo == hi)
      return samples[lo];
    const double t = rank - static_cast<double>(lo);
    return samples[lo] * (1.0 - t) + samples[hi] * t;
  };

  double varCpu = 0.0;
  for (double v : cpuFrameTimes) {
    const double d = v - avgCpu;
    varCpu += d * d;
  }
  const double stddevCpu =
      cpuFrameTimes.empty() ? 0.0 : std::sqrt(varCpu / cpuFrameTimes.size());
  const double p50Cpu = percentile(cpuFrameTimes, 50.0);
  const double p95Cpu = percentile(cpuFrameTimes, 95.0);
  const double p99Cpu = percentile(cpuFrameTimes, 99.0);
  const double p50Gpu = percentile(gpuFrameTimes, 50.0);
  const double p95Gpu = percentile(gpuFrameTimes, 95.0);
  const double p99Gpu = percentile(gpuFrameTimes, 99.0);
  const FrameStateAnalysisResult frameState = analyzeFrameStates(frames);

  json << "  \"avgFrameTimeMs\": " << avgCpu << ",\n";
  json << "  \"avgGpuTimeMs\": ";
  if (gpuSamples != 0u)
    json << avgGpu;
  else
    json << "null";
  json << ",\n";
  json << "  \"gpuSamples\": " << gpuSamples << ",\n";
  json << "  \"maxFrameTimeMs\": " << maxCpu << ",\n";
  json << "  \"minFrameTimeMs\": " << (minCpu < 999999.0 ? minCpu : 0.0)
       << ",\n";
  json << "  \"p50CpuMs\": " << p50Cpu << ",\n";
  json << "  \"p95CpuMs\": " << p95Cpu << ",\n";
  json << "  \"p99CpuMs\": " << p99Cpu << ",\n";
  json << "  \"p50GpuMs\": ";
  if (gpuSamples != 0u)
    json << p50Gpu;
  else
    json << "null";
  json << ",\n";
  json << "  \"p95GpuMs\": ";
  if (gpuSamples != 0u)
    json << p95Gpu;
  else
    json << "null";
  json << ",\n";
  json << "  \"p99GpuMs\": ";
  if (gpuSamples != 0u)
    json << p99Gpu;
  else
    json << "null";
  json << ",\n";
  json << "  \"stddevCpuMs\": " << stddevCpu << ",\n";
  json << "  \"avgProcessCpuMs\": " << avgProcessCpuMs << ",\n";
  json << "  \"avgMainThreadCpuMs\": " << avgMainThreadCpuMs << ",\n";
  json << "  \"avgWorkerThreadsCpuMs\": " << avgWorkerThreadsCpuMs << ",\n";
  json << "  \"mainThreadId\": " << snapshot.mainThreadId << ",\n";
  json << "  \"mainThreadAuthority\": \"world-frame-boundary-v2\",\n";
  json << "  \"mainThreadCpuClampFrames\": " << mainThreadCpuClampFrames
       << ",\n";
  json << "  \"processCpuCoveragePct\": " << processCpuCoveragePct << ",\n";
  json << "  \"mainThreadCpuCoveragePct\": " << mainThreadCpuCoveragePct
       << ",\n";
  json << "  \"processParallelismCores\": " << processParallelismCores
       << ",\n";
  json << "  \"mainThreadShareOfProcessPct\": " << mainThreadShareOfProcessPct
       << ",\n";
  json << "  \"workerThreadsShareOfProcessPct\": "
       << workerThreadsShareOfProcessPct << ",\n";
  json << "  \"mainLoopThreadSplit\": [\n";
  json << "    {\"name\":\"Process\", \"avgCpuMs\": " << avgProcessCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgProcessCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": 100.0},\n";
  json << "    {\"name\":\"MainThread\", \"avgCpuMs\": " << avgMainThreadCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgMainThreadCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": " << mainThreadShareOfProcessPct
       << "},\n";
  json << "    {\"name\":\"Workers\", \"avgCpuMs\": " << avgWorkerThreadsCpuMs
       << ", \"shareOfFramePct\": "
       << ((avgCpu > 1e-6) ? (avgWorkerThreadsCpuMs / avgCpu * 100.0) : 0.0)
       << ", \"shareOfProcessPct\": " << workerThreadsShareOfProcessPct
       << "}\n";
  json << "  ],\n";
  json << "  \"jank16\": " << jank16 << ",\n";
  json << "  \"jank33\": " << jank33 << ",\n";
  json << "  \"jank50\": " << jank50 << ",\n";
  json << "  \"avgFps\": " << (avgCpu > 0.0 ? 1000.0 / avgCpu : 0.0) << ",\n";
  json << "  \"traceContract\": {\n";
  json << "    \"version\": \"war3-perf-trace-v3\",\n";
  json << "    \"presentClockDomain\": \"wall-ms-between-beginFrame-endFrame\",\n";
  json << "    \"engineCycleClockDomain\": \"waitgate-to-waitgate-overlay-not-frame-attribution\",\n";
  json << "    \"cpuScopeContract\": \"wall-inclusive-scopes; self=inclusive-direct-children; thread-local-tree\",\n";
  json << "    \"detailSamplingContract\": \"sampled scope totals/calls are period-weighted estimates; max is observed sample\",\n";
  json << "    \"detailNestingContract\": \"hot-phase samples are independent periodic estimates, not one transaction's exact nested self tree\",\n";
  json << "    \"nonAdditive\": \"present-wall/process-cpu/main-cpu/workers-cpu/gpu-queue-must-not-be-summed\",\n";
  json << "    \"uncoveredWallContract\": \"frame-wall minus additive root-wall; diagnostic budget gap, never a CPU hotspot\",\n";
  json << "    \"detailEnabled\": "
       << (snapshot.detailTraceEnabled ? "true" : "false") << ",\n";
  json << "    \"detailSamplePeriod\": " << snapshot.detailTraceSamplePeriod << ",\n";
  json << "    \"lastFrameProfiler\": {\"coarseScopeAttempts\": "
       << snapshot.profilerCounters.coarseScopeAttempts
       << ", \"detailScopeAttempts\": "
       << snapshot.profilerCounters.detailScopeAttempts
       << ", \"detailScopeSampled\": "
       << snapshot.profilerCounters.detailScopeSampled
       << ", \"lateCpuDeltasRecovered\": "
       << snapshot.profilerCounters.lateCpuDeltasRecovered
       << ", \"lateGpuSamplesRecovered\": "
       << snapshot.profilerCounters.lateGpuSamplesRecovered
       << ", \"gpuSamplesDropped\": "
       << snapshot.profilerCounters.gpuSamplesDropped
       << ", \"lateCpuDeltasDropped\": "
       << snapshot.profilerCounters.lateCpuDeltasDropped << "}\n";
  json << "  },\n";
  json << "  \"runtimeProfile\": {\n";
  json << "    \"name\": \"" << dxvk::war3::runtime::GetWar3RuntimeProfileName()
       << "\",\n";
  json << "    \"disabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv() << "\",\n";
  json << "    \"enabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv() << "\"\n";
  json << "  },\n";
  json << "  \"moduleMatrix\": [\n";
  json << "    {\"profile\": \""
       << dxvk::war3::runtime::GetWar3RuntimeProfileName()
       << "\", \"disabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeDisabledModulesCsv()
       << "\", \"enabledModules\": \""
       << dxvk::war3::runtime::GetWar3RuntimeEnabledModulesCsv() << "\"}\n";
  json << "  ],\n";

  const auto& worldObjects = snapshot.worldObjectsMaintenanceTiming;
  const auto boolJson = [](bool value) {
    return value ? "true" : "false";
  };
  const auto writeWorldObjectsTiming =
      [&json](const char* indent, const char* name,
              const render::WorldObjectsPhase1RawTiming& timing,
              bool trailingComma) {
        json << indent << "\"" << name << "\":{\"calls\":"
             << timing.calls << ",\"ticks\":" << timing.ticks
             << ",\"maxTicks\":" << timing.maxTicks << "}"
             << (trailingComma ? ",\n" : "\n");
      };
  json << "  \"worldObjectsMaintenanceTiming\": {\n";
  json << "    \"snapshotContract\": "
          "\"export-time-fixed-pod-ring-v3-vacuous-subset\",\n";
  json << "    \"timingContract\": "
          "\"raw-qpc-inclusive-partition-do-not-double-sum\",\n";
  json << "    \"groupContract\": "
          "\"possibly-empty-observed-subset-masks-match-unobserved-zero\",\n";
  json << "    \"periodicDispatchContract\": "
          "\"pure-periodic-plus-adjacent-control-render-tls-present-v3\",\n";
  json << "    \"qpcFrequency\": " << worldObjects.qpcFrequency << ",\n";
  json << "    \"snapshotGenerationBefore\": "
       << worldObjects.snapshotGenerationBefore << ",\n";
  json << "    \"snapshotGenerationAfter\": "
       << worldObjects.snapshotGenerationAfter << ",\n";
  json << "    \"snapshotWrites\": {"
       << "\"startedBefore\":"
       << worldObjects.snapshotWritesStartedBefore << ","
       << "\"startedAfter\":"
       << worldObjects.snapshotWritesStartedAfter << ","
       << "\"completedBefore\":"
       << worldObjects.snapshotWritesCompletedBefore << ","
       << "\"completedAfter\":"
       << worldObjects.snapshotWritesCompletedAfter << ","
       << "\"writersBefore\":" << worldObjects.snapshotWritersBefore
       << ",\"writersAfter\":" << worldObjects.snapshotWritersAfter
       << "},\n";
  json << "    \"snapshotStable\": "
       << boolJson(worldObjects.snapshotStable) << ",\n";
  json << "    \"trackingReasonClosureClean\": "
       << boolJson(worldObjects.trackingReasonClosureClean) << ",\n";
  json << "    \"eventCountClosureClean\": "
       << boolJson(worldObjects.eventCountClosureClean) << ",\n";
  json << "    \"lifecycleClosureClean\": "
       << boolJson(worldObjects.lifecycleClosureClean) << ",\n";
  json << "    \"pairedCaptureFaults\":{"
       << "\"duplicatePublish\":"
       << worldObjects.pairedCaptureDuplicatePublishCount
       << ",\"lostPublish\":"
       << worldObjects.pairedCaptureLostPublishCount
       << ",\"slotMismatch\":"
       << worldObjects.pairedCaptureSlotMismatchCount << "},\n";
  json << "    \"lifetimeGroupMasks\": {"
       << "\"collector\":" << worldObjects.lifetimeCollectorGroupMask
       << ",\"hook\":" << worldObjects.lifetimeHookGroupMask
       << ",\"observed\":" << worldObjects.lifetimeObservedGroupMask
       << "},\n";
  json << "    \"lifetimeObservedGroupsClosureClean\": "
       << boolJson(worldObjects.lifetimeObservedGroupsClosureClean)
       << ",\n";
  json << "    \"lifetimeUnobservedGroupsZeroClean\": "
       << boolJson(worldObjects.lifetimeUnobservedGroupsZeroClean)
       << ",\n";
  json << "    \"retainedEventsClosureClean\": "
       << boolJson(worldObjects.retainedEventsClosureClean) << ",\n";
  json << "    \"overallClosureClean\": "
       << boolJson(worldObjects.overallClosureClean) << ",\n";
  json << "    \"trackingAttempts\": " << worldObjects.trackingAttempts
       << ",\n";
  json << "    \"trackingHealthContract\": "
          "\"o1-aggregate-plus-opt-in-bruteforce-v2\",\n";
  json << "    \"trackingHealth\": {"
       << "\"fastPathCalls\":"
       << worldObjects.trackingHealthFastPathCalls << ","
       << "\"fullSummaryCompatibilityCalls\":"
       << worldObjects.trackingHealthFullSummaryCompatibilityCalls << ","
       << "\"aggregateReadPasses\":{"
       << "\"modelInstance\":"
       << worldObjects.trackingHealthModelInstanceAggregateReadPasses
       << ",\"pose\":"
       << worldObjects.trackingHealthPoseAggregateReadPasses << "},"
       << "\"verifierScanPasses\":{"
       << "\"modelInstance\":"
       << worldObjects.trackingHealthModelInstanceVerifierScanPasses
       << ",\"pose\":"
       << worldObjects.trackingHealthPoseVerifierScanPasses << "},"
       << "\"verifierRecordsScanned\":{"
       << "\"modelInstance\":"
       << worldObjects.trackingHealthModelInstanceVerifierRecordsScanned
       << ",\"pose\":"
       << worldObjects.trackingHealthPoseVerifierRecordsScanned << "},"
       << "\"verifierMismatchCount\":{"
       << "\"modelInstance\":"
       << worldObjects.trackingHealthModelInstanceVerifierMismatchCount
       << ",\"pose\":"
       << worldObjects.trackingHealthPoseVerifierMismatchCount << "},"
       << "\"verifierMismatchMask\":{"
       << "\"modelInstance\":"
       << worldObjects.trackingHealthModelInstanceVerifierMismatchMask
       << ",\"pose\":"
       << worldObjects.trackingHealthPoseVerifierMismatchMask << "},"
       << "\"verifierMismatchBits\":{"
       << "\"modelInstance\":{"
       << "\"recordCount\":1,\"runtimeBoundCount\":2,"
       << "\"completeIdentityCount\":4},"
       << "\"pose\":{"
       << "\"recordCount\":1,\"readyPoseCount\":2,"
       << "\"spriteFramePoseCount\":4,\"matrixPaletteCount\":8}},"
       << "\"closureClean\":"
       << boolJson(worldObjects.trackingHealthPathClosureClean)
       << "},\n";
  json << "    \"identityRequests\": " << worldObjects.identityRequests
       << ",\n";
  json << "    \"fallbackRequests\": " << worldObjects.fallbackRequests
       << ",\n";
  json << "    \"eventCountLifetime\": "
       << worldObjects.eventCountLifetime << ",\n";
  json << "    \"latestEventSequence\": "
       << worldObjects.latestEventSequence << ",\n";
  json << "    \"retainedEventExpectedCount\": "
       << worldObjects.retainedEventExpectedCount << ",\n";
  json << "    \"retainedEventCount\": "
       << worldObjects.retainedEventCount << ",\n";
  json << "    \"missingRetainedEventCount\": "
       << worldObjects.missingRetainedEventCount << ",\n";
  json << "    \"lifecycleErrors\": {"
       << "\"collectorWithoutEvent\":"
       << worldObjects.collectorWithoutEventCount << ","
       << "\"collectorReentry\":"
       << worldObjects.collectorReentryCount << ","
       << "\"collectorWithoutHook\":"
       << worldObjects.collectorWithoutHookCount << ","
       << "\"hookWithoutCollector\":"
       << worldObjects.hookWithoutCollectorCount << ","
       << "\"registryFeedOutsideCollector\":"
       << worldObjects.registryFeedOutsideCollectorCount << ","
       << "\"unexpectedGroup\":" << worldObjects.unexpectedGroupCount
       << "},\n";
  json << "    \"reasonCounts\": {";
  for (uint32_t reason = 0u;
       reason < render::kWorldObjectsPhase1TrackingReasonCount; ++reason) {
    if (reason != 0u)
      json << ",";
    json << "\"" << worldObjectsPhase1ReasonName(
        static_cast<render::WorldObjectsPhase1TrackingReason>(reason))
         << "\":" << worldObjects.reasonCounts[reason];
  }
  json << "},\n";
  json << "    \"tracking\": {\n";
  writeWorldObjectsTiming("      ", "inclusive",
                          worldObjects.trackingInclusive, true);
  writeWorldObjectsTiming("      ", "query",
                          worldObjects.trackingQuery, true);
  writeWorldObjectsTiming("      ", "decision",
                          worldObjects.trackingDecision, false);
  json << "    },\n";
  json << "    \"groups\": [\n";
  for (uint32_t group = 0u;
       group < render::kWorldObjectsPhase1GroupCount; ++group) {
    const auto& groupTiming = worldObjects.groups[group];
    json << "      {\"group\":" << group
         << ",\"observed\":" << boolJson(groupTiming.observed)
         << ",\n";
    json << "       \"timings\": {\n";
    writeWorldObjectsTiming("         ", "hookInclusive",
                            groupTiming.hookInclusive, true);
    writeWorldObjectsTiming("         ", "collectorInclusive",
                            groupTiming.collectorInclusive, true);
    writeWorldObjectsTiming("         ", "collectorSetup",
                            groupTiming.collectorSetup, true);
    writeWorldObjectsTiming("         ", "collectorIterate",
                            groupTiming.collectorIterate, true);
    writeWorldObjectsTiming("         ", "collectorRegister",
                            groupTiming.collectorRegister, true);
    writeWorldObjectsTiming("         ", "collectorTail",
                            groupTiming.collectorTail, true);
    writeWorldObjectsTiming("         ", "modelFeed",
                            groupTiming.modelFeed, true);
    writeWorldObjectsTiming("         ", "shadowFeed",
                            groupTiming.shadowFeed, false);
    json << "       },\n";
    json << "       \"entries\": {\"list\":"
         << groupTiming.listEntries << ",\"accepted\":"
         << groupTiming.acceptedEntries << ",\"sceneNodes\":"
         << groupTiming.sceneNodeEntries << ",\"handles\":"
         << groupTiming.handleEntries << "},\n";
    json << "       \"violations\": {"
         << "\"collectorPartition\":"
         << groupTiming.collectorPartitionMismatchCount << ","
         << "\"hookContainment\":"
         << groupTiming.hookContainmentViolationCount << ","
         << "\"registerFeedContainment\":"
         << groupTiming.registerFeedContainmentViolationCount << ","
         << "\"acceptedBounds\":"
         << groupTiming.acceptedCountViolationCount << ","
         << "\"sceneNodeBounds\":"
         << groupTiming.sceneNodeCountViolationCount << ","
         << "\"handleBounds\":"
         << groupTiming.handleCountViolationCount << "},\n";
    json << "       \"outcomes\": {";
    for (uint32_t outcome = 0u;
         outcome < render::kWorldObjectsPhase1CollectorOutcomeCount;
         ++outcome) {
      if (outcome != 0u)
        json << ",";
      json << "\"" << worldObjectsPhase1OutcomeName(
          static_cast<render::WorldObjectsPhase1CollectorOutcome>(outcome))
           << "\":" << groupTiming.outcomeCounts[outcome];
    }
    json << "},\n";
    json << "       \"closures\": {"
         << "\"outcome\":" << boolJson(groupTiming.outcomeClosureClean)
         << ",\"partition\":"
         << boolJson(groupTiming.collectorPartitionClean)
         << ",\"hookCollectorCalls\":"
         << boolJson(groupTiming.hookCollectorCallClosureClean)
         << ",\"containment\":"
         << boolJson(groupTiming.containmentClean)
         << ",\"entryBounds\":"
         << boolJson(groupTiming.entryCountBoundsClean)
         << ",\"unobservedZero\":"
         << boolJson(groupTiming.unobservedZeroClean) << "}}"
         << (group + 1u < render::kWorldObjectsPhase1GroupCount
                 ? ",\n" : "\n");
  }
  json << "    ],\n";
  json << "    \"events\": [\n";
  for (uint32_t eventIndex = 0u;
       eventIndex < worldObjects.retainedEventCount; ++eventIndex) {
    const auto& event = worldObjects.events[eventIndex];
    json << "      {\"sequence\":" << event.sequence
         << ",\"frameSerial\":" << event.frameSerial
         << ",\"collectionFrameSerial\":"
         << event.collectionFrameSerial
         << ",\"poseSerial\":" << event.poseSerial
         << ",\"reason\":\""
         << worldObjectsPhase1ReasonName(event.reason)
         << "\",\"reasonValue\":"
         << static_cast<uint32_t>(event.reason)
         << ",\"reasonMask\":" << event.reasonMask
         << ",\"refreshPeriod\":" << event.refreshPeriod
         << ",\"warmupFrames\":" << event.warmupFrames
         << ",\"wantsObjectIdentity\":"
         << boolJson(event.wantsObjectIdentity)
         << ",\"wantsFallbackBridge\":"
         << boolJson(event.wantsFallbackBridge)
         << ",\"collectorGroupMask\":" << event.collectorGroupMask
         << ",\"hookGroupMask\":" << event.hookGroupMask
         << ",\"observedGroupMask\":" << event.observedGroupMask
         << ",\"duplicateCollectorGroupMask\":"
         << event.duplicateCollectorGroupMask
         << ",\"duplicateHookGroupMask\":"
         << event.duplicateHookGroupMask
         << ",\"trackingInclusiveTicks\":"
         << event.trackingInclusiveTicks
         << ",\"trackingQueryTicks\":" << event.trackingQueryTicks
         << ",\"trackingDecisionTicks\":"
         << event.trackingDecisionTicks
         << ",\"trackingPartitionClean\":"
         << boolJson(event.trackingPartitionClean)
         << ",\"completeObservedGroups\":"
         << boolJson(event.completeObservedGroups)
         << ",\"unobservedGroupsZeroClean\":"
         << boolJson(event.unobservedGroupsZeroClean)
         << ",\"groupClosureClean\":"
         << boolJson(event.groupClosureClean)
         << ",\"pairLifecycleClosureClean\":"
         << boolJson(event.pairLifecycleClosureClean)
         << ",\"periodicEventSubsetClosureClean\":"
         << boolJson(event.periodicEventSubsetClosureClean)
         << ",\"pairQpcBalancedExcludingGetTag\":"
         << boolJson(event.pairQpcBalancedExcludingGetTag)
         << ",\"pairQpcBalancedIncludingGetTag\":"
         << boolJson(event.pairQpcBalancedIncludingGetTag)
         << ",\"pairTopologyComparable\":"
         << boolJson(event.pairTopologyComparable)
         << ",\"pairComparable\":"
         << boolJson(event.pairComparable) << ",\n";
    static constexpr const char* kPairedTimingNames[] = {
        "presentPreTracking", "worldHookInclusive", "worldCollector",
        "worldOriginal", "worldTrackNewBatches", "flushRoot",
        "flushNotify", "flushTransactionBegin", "flushOriginalBody",
        "flushReimplOpaque", "flushReimplTransparent",
        "flushTransactionEnd", "dispatchRoot",
        "dispatchResolveSemantic", "dispatchNativeBegin",
        "dispatchExecBegin", "dispatchOriginal",
        "dispatchPublishVisible", "dispatchExecEnd",
        "dispatchNativeEnd", "reimplExecBegin", "reimplExecEnd"};
    static_assert(
        sizeof(kPairedTimingNames) / sizeof(kPairedTimingNames[0]) ==
            render::kWorldObjectsPhase1PairedTimingStageCount,
        "paired timing JSON names must cover every stage");
    static constexpr const char* kFlushTerminalNames[] = {
        "unclassified", "missingGlobalsOriginal",
        "missingDispatchOriginal", "decisionFallbackOriginal",
        "opaqueFailureOriginal", "transparentFailureOriginal",
        "takeoverSuccess"};
    static_assert(
        sizeof(kFlushTerminalNames) / sizeof(kFlushTerminalNames[0]) ==
            render::kWorldObjectsPhase1FlushTerminalCount,
        "flush terminal JSON names must cover every terminal");
    const auto writePairedDispatch = [&] (
        const char* name,
        const render::WorldObjectsPhase1PeriodicDispatch& dispatch,
        bool trailingComma) {
      const auto timing = [&](render::WorldObjectsPhase1PairedTimingStage stage)
          -> const render::WorldObjectsPhase1RawTiming& {
        return dispatch.stageTimings[static_cast<uint32_t>(stage)];
      };
      const uint64_t flushKnownTicks =
          timing(render::WorldObjectsPhase1PairedTimingStage::FlushNotify).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     FlushTransactionBegin).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     FlushOriginalBody).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     FlushReimplOpaque).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     FlushReimplTransparent).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     FlushTransactionEnd).ticks;
      const uint64_t dispatchKnownTicks =
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchResolveSemantic).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchNativeBegin).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchExecBegin).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchOriginal).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchPublishVisible).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchExecEnd).ticks +
          timing(render::WorldObjectsPhase1PairedTimingStage::
                     DispatchNativeEnd).ticks;
      const uint64_t flushRootTicks =
          timing(render::WorldObjectsPhase1PairedTimingStage::FlushRoot).ticks;
      const uint64_t dispatchRootTicks =
          timing(render::WorldObjectsPhase1PairedTimingStage::DispatchRoot).ticks;
      json << "       \"" << name << "\":{"
           << "\"captureRequested\":" << boolJson(dispatch.captureRequested)
           << ",\"finalized\":" << boolJson(dispatch.finalized)
           << ",\"captureFrameSerial\":" << dispatch.captureFrameSerial
           << ",\"ownerThreadId\":" << dispatch.ownerThreadId
           << ",\"qpcReadCount\":" << dispatch.qpcReadCount
           << ",\"commonCalls\":" << dispatch.commonCalls
           << ",\"specialCalls\":" << dispatch.specialCalls
           << ",\"group0Calls\":" << dispatch.group0Calls
           << ",\"otherStageCalls\":" << dispatch.otherStageCalls
           << ",\"dispatchRootCalls\":" << dispatch.dispatchRootCalls
           << ",\"dispatchRootTicks\":" << dispatch.dispatchRootTicks
           << ",\"worldFastEligibleIgnoringIdentity\":"
           << dispatch.worldFastEligibleIgnoringIdentity
           << ",\"worldFastBlockedByIdentity\":"
           << dispatch.worldFastBlockedByIdentity
           << ",\"getTagStageCalls\":" << dispatch.getTagStageCalls
           << ",\"getTagStageHits\":" << dispatch.getTagStageHits
           << ",\"getTagStageMisses\":" << dispatch.getTagStageMisses
           << ",\"getTagStageConflicts\":"
           << dispatch.getTagStageConflicts
           << ",\"getTagStageProbes\":" << dispatch.getTagStageProbes
           << ",\"getTagStageTicks\":" << dispatch.getTagStageTicks
           << ",\"flushTopology\":{\"calls\":"
           << dispatch.flushTopologyCalls << ",\"opaqueTotal\":"
           << dispatch.opaqueCountTotal << ",\"transparentTotal\":"
           << dispatch.transparentCountTotal << ",\"hash\":"
           << dispatch.flushTopologyHash << "},\"flushTerminalCounts\":{";
      for (uint32_t i = 0u;
           i < render::kWorldObjectsPhase1FlushTerminalCount; ++i) {
        if (i != 0u)
          json << ",";
        json << "\"" << kFlushTerminalNames[i] << "\":"
             << dispatch.flushTerminalCounts[i];
      }
      json << "},\"stageTimings\":{";
      for (uint32_t i = 0u;
           i < render::kWorldObjectsPhase1PairedTimingStageCount; ++i) {
        if (i != 0u)
          json << ",";
        const auto& stage = dispatch.stageTimings[i];
        json << "\"" << kPairedTimingNames[i] << "\":{\"calls\":"
             << stage.calls << ",\"ticks\":" << stage.ticks
             << ",\"maxTicks\":" << stage.maxTicks << "}";
      }
      json << "},\"residualTicks\":{\"flush\":"
           << (flushRootTicks >= flushKnownTicks
                   ? flushRootTicks - flushKnownTicks : 0u)
           << ",\"dispatch\":"
           << (dispatchRootTicks >= dispatchKnownTicks
                   ? dispatchRootTicks - dispatchKnownTicks : 0u)
           << "},\"closures\":{\"dispatchPath\":"
           << boolJson(dispatch.dispatchPathClosureClean)
           << ",\"dispatchRoot\":"
           << boolJson(dispatch.dispatchRootClosureClean)
           << ",\"worldFast\":" << boolJson(dispatch.worldFastClosureClean)
           << ",\"getTagStage\":"
            << boolJson(dispatch.getTagStageClosureClean)
            << ",\"rawTiming\":"
            << boolJson(dispatch.rawTimingClosureClean)
            << ",\"qpcReads\":"
            << boolJson(dispatch.qpcReadClosureClean)
            << ",\"pairedTiming\":"
           << boolJson(dispatch.pairedTimingClosureClean)
           << ",\"flushTopology\":"
           << boolJson(dispatch.flushTopologyClosureClean)
           << ",\"overall\":" << boolJson(dispatch.closureClean) << "}}"
           << (trailingComma ? ",\n" : "\n");
    };
    writePairedDispatch("periodicDispatch", event.periodicDispatch, true);
    writePairedDispatch(
        "postPeriodicControl", event.postPeriodicControl, true);
    json << "       \"groups\": [\n";
    for (uint32_t group = 0u;
         group < render::kWorldObjectsPhase1GroupCount; ++group) {
      const auto& eventGroup = event.groups[group];
      json << "         {\"group\":" << group
           << ",\"observed\":" << boolJson(eventGroup.observed)
           << ",\"hookCalls\":" << eventGroup.hookCalls
           << ",\"collectorCalls\":" << eventGroup.collectorCalls
           << ",\"modelFeedCalls\":" << eventGroup.modelFeedCalls
           << ",\"shadowFeedCalls\":" << eventGroup.shadowFeedCalls
           << ",\"hookInclusiveTicks\":"
           << eventGroup.hookInclusiveTicks
           << ",\"collectorInclusiveTicks\":"
           << eventGroup.collectorInclusiveTicks
           << ",\"setupTicks\":" << eventGroup.collectorSetupTicks
           << ",\"iterateTicks\":" << eventGroup.collectorIterateTicks
           << ",\"registerTicks\":"
           << eventGroup.collectorRegisterTicks
           << ",\"tailTicks\":" << eventGroup.collectorTailTicks
           << ",\"modelFeedTicks\":" << eventGroup.modelFeedTicks
           << ",\"shadowFeedTicks\":" << eventGroup.shadowFeedTicks
           << ",\"listEntries\":" << eventGroup.listEntries
           << ",\"acceptedEntries\":" << eventGroup.acceptedEntries
           << ",\"sceneNodeEntries\":"
           << eventGroup.sceneNodeEntries
           << ",\"handleEntries\":" << eventGroup.handleEntries
           << ",\"outcomes\":{";
      for (uint32_t outcome = 0u;
           outcome < render::kWorldObjectsPhase1CollectorOutcomeCount;
           ++outcome) {
        if (outcome != 0u)
          json << ",";
        json << "\"" << worldObjectsPhase1OutcomeName(
            static_cast<render::WorldObjectsPhase1CollectorOutcome>(outcome))
             << "\":" << eventGroup.outcomeCounts[outcome];
      }
      json << "},\"closures\":{"
           << "\"outcome\":" << boolJson(eventGroup.outcomeClosureClean)
           << ",\"partition\":"
           << boolJson(eventGroup.collectorPartitionClean)
           << ",\"hookCollectorCalls\":"
           << boolJson(eventGroup.hookCollectorCallClosureClean)
           << ",\"hookContainsCollector\":"
           << boolJson(eventGroup.hookContainsCollector)
           << ",\"registerContainsFeeds\":"
           << boolJson(eventGroup.registerContainsFeeds)
           << ",\"entryBounds\":"
           << boolJson(eventGroup.entryCountBoundsClean)
           << ",\"unobservedZero\":"
           << boolJson(eventGroup.unobservedZeroClean) << "}}"
           << (group + 1u < render::kWorldObjectsPhase1GroupCount
                   ? ",\n" : "\n");
    }
    json << "       ]}"
         << (eventIndex + 1u < worldObjects.retainedEventCount
                 ? ",\n" : "\n");
  }
  json << "    ]\n";
  json << "  },\n";

  const auto& residency = snapshot.resourceResidency;
  json << "  \"resourceResidencyCensus\": {\n";
  json << "    \"enabled\": " << boolJson(residency.enabled) << ",\n";
  json << "    \"contract\": "
          "\"diagnostics-only-observation-v1\",\n";
  json << "    \"evictionAuthority\": false,\n";
  json << "    \"performanceComparable\": "
       << boolJson(!residency.enabled) << ",\n";
  json << "    \"deviceAllocationBytesAreExclusiveVram\": false,\n";
  json << "    \"hostBackingBytesAreAddressSpace\": false,\n";
  json << "    \"allocationColumnsAreAdditive\": false,\n";
  json << "    \"processPrivateCommitComparable\": false,\n";
  json << "    \"snapshotCoverage\": "
          "\"known-live-steady-plus-gpu-skin-pool-peaks-v1\",\n";
  json << "    \"gameDllOriginalAllocationsIncluded\": false,\n";
  json << "    \"driverAllocatorPoolReserveIncluded\": false,\n";
  json << "    \"transientShadowBuildCopiesIncluded\": false,\n";
  json << "    \"retainedOldPublishedStoresIncluded\": false,\n";
  json << "    \"resourceFieldsAreQuiescentSnapshot\": false,\n";
  json << "    \"gpuSkinQueuedMissPeakIncluded\": true,\n";
  json << "    \"quiescentObservationFrames\": 300,\n";
  json << "    \"frameSerial\": " << residency.frameSerial << ",\n";
  json << "    \"lifetimeRegistrations\": "
       << residency.lifetimeRegistrations << ",\n";
  json << "    \"liveResources\": " << residency.liveResources << ",\n";
  json << "    \"totals\": {"
       << "\"logicalBytes\":" << residency.logicalBytes
       << ",\"deviceAllocationBytes\":"
       << residency.deviceAllocationBytes
       << ",\"hostBackingLogicalBytes\":"
       << residency.hostBackingLogicalBytes
       << ",\"hostMappedLogicalBytes\":"
       << residency.hostMappedLogicalBytes
       << ",\"duplicateHostBackingLogicalBytes\":"
       << residency.duplicateHostBackingLogicalBytes
       << ",\"observedCandidateHostBytes\":"
       << residency.observedCandidateHostBytes
       << ",\"lazyReadbackCandidateHostBytes\":"
       << residency.lazyReadbackCandidateHostBytes << "},\n";
  const auto& hostAllocator = residency.d3d9HostAllocator;
  const auto& chunkDiagnostics = hostAllocator.chunkDiagnostics;
  const auto& bindingCoverage = chunkDiagnostics.bindingCoverage;
  json << "    \"d3d9HostAllocator\": {"
       << "\"allocatedBackingBytes\":"
       << hostAllocator.allocatedBackingBytes
       << ",\"usedPayloadBytes\":" << hostAllocator.usedPayloadBytes
       << ",\"mappedAddressBytes\":" << hostAllocator.mappedAddressBytes
       << ",\"chunkDiagnostics\":{"
       << "\"contract\":\"d3d9-host-chunk-diagnostics-v1\""
       << ",\"available\":" << boolJson(chunkDiagnostics.available)
       << ",\"authority\":false"
       << ",\"chunkBacked\":" << boolJson(chunkDiagnostics.chunkBacked)
       << ",\"accountingClosure\":"
       << boolJson(chunkDiagnostics.accountingClosure)
       << ",\"mutationGeneration\":"
       << chunkDiagnostics.mutationGeneration
       << ",\"mutationGenerationBegin\":"
       << chunkDiagnostics.mutationGenerationBegin
       << ",\"mutationGenerationEnd\":"
       << chunkDiagnostics.mutationGenerationEnd
       << ",\"generationStable\":"
       << boolJson(chunkDiagnostics.generationStable)
       << ",\"mutationGenerationSaturated\":"
       << boolJson(chunkDiagnostics.mutationGenerationSaturated)
       << ",\"reserveBytes\":" << chunkDiagnostics.reserveBytes
       << ",\"allocatorUsedPayloadBytes\":"
       << chunkDiagnostics.allocatorUsedPayloadBytes
       << ",\"chunkOccupiedBytes\":"
       << chunkDiagnostics.chunkOccupiedBytes
       << ",\"internalFragmentationBytes\":"
       << chunkDiagnostics.internalFragmentationBytes
       << ",\"freePayloadBytes\":"
       << chunkDiagnostics.freePayloadBytes
       << ",\"sharedMappedRefs\":"
       << chunkDiagnostics.sharedMappedRefs
       << ",\"sharedMappedBytes\":"
       << chunkDiagnostics.sharedMappedBytes
       << ",\"standaloneMappedRefs\":"
       << chunkDiagnostics.standaloneMappedRefs
       << ",\"standaloneMappedBytes\":"
       << chunkDiagnostics.standaloneMappedBytes
       << ",\"mappedRefs\":" << chunkDiagnostics.mappedRefs
       << ",\"mappedBytes\":" << chunkDiagnostics.mappedBytes
       << ",\"mapFailureCount\":"
       << chunkDiagnostics.mapFailureCount
       << ",\"unmapFailureCount\":"
       << chunkDiagnostics.unmapFailureCount
       << ",\"mappingStateFaultCount\":"
       << chunkDiagnostics.mappingStateFaultCount
       << ",\"duplicateAllocatorChunkIdCount\":"
       << chunkDiagnostics.duplicateAllocatorChunkIdCount
       << ",\"candidateOnlyChunkCount\":"
       << chunkDiagnostics.candidateOnlyChunkCount
       << ",\"candidateOnlyReserveBytesUpperBound\":"
       << chunkDiagnostics.candidateOnlyReserveBytesUpperBound
       << ",\"candidateOnlyMappedBytesUpperBound\":"
       << chunkDiagnostics.candidateOnlyMappedBytesUpperBound
       << ",\"bindingCoverage\":{"
       << "\"hostBackingResources\":"
       << bindingCoverage.hostBackingResources
       << ",\"hostBackingLogicalBytes\":"
       << bindingCoverage.hostBackingLogicalBytes
       << ",\"exactD3D9MemoryBindingResources\":"
       << bindingCoverage.exactD3D9MemoryBindingResources
       << ",\"exactD3D9MemoryBindingLogicalBytes\":"
       << bindingCoverage.exactD3D9MemoryBindingLogicalBytes
       << ",\"exactBindings\":" << bindingCoverage.exactBindings
       << ",\"boundAlignedSliceBytes\":"
       << bindingCoverage.boundAlignedSliceBytes
       << ",\"mappedBindingAlignedSliceBytes\":"
       << bindingCoverage.mappedBindingAlignedSliceBytes
       << ",\"externalHostBackingResources\":"
       << bindingCoverage.externalHostBackingResources
       << ",\"externalHostBackingLogicalBytes\":"
       << bindingCoverage.externalHostBackingLogicalBytes
       << ",\"unresolvedD3D9MemoryResources\":"
       << bindingCoverage.unresolvedD3D9MemoryResources
       << ",\"unresolvedD3D9MemoryLogicalBytes\":"
       << bindingCoverage.unresolvedD3D9MemoryLogicalBytes
       << ",\"unregisteredHostBackingResources\":"
       << bindingCoverage.unregisteredHostBackingResources
       << ",\"unregisteredHostBackingLogicalBytes\":"
       << bindingCoverage.unregisteredHostBackingLogicalBytes
       << ",\"missingChunkBindingCount\":"
       << bindingCoverage.missingChunkBindingCount
       << ",\"invalidBindingCount\":"
       << bindingCoverage.invalidBindingCount
       << ",\"outOfBoundsBindingCount\":"
       << bindingCoverage.outOfBoundsBindingCount
       << ",\"duplicateBindingCount\":"
       << bindingCoverage.duplicateBindingCount
       << ",\"overlapBindingCount\":"
       << bindingCoverage.overlapBindingCount
       << ",\"unclassifiedAlignedSliceBytes\":"
       << bindingCoverage.unclassifiedAlignedSliceBytes
       << ",\"unregisteredAllocatorPayloadBytes\":"
       << bindingCoverage.unregisteredAllocatorPayloadBytes
       << ",\"boundBytesExceedAllocatorUsed\":"
       << boolJson(bindingCoverage.boundBytesExceedAllocatorUsed)
       << ",\"bindingBytesClosure\":"
       << boolJson(bindingCoverage.bindingBytesClosure) << "},"
       << "\"chunks\":[\n";
  for (size_t i = 0u; i < chunkDiagnostics.chunks.size(); ++i) {
    const auto& chunk = chunkDiagnostics.chunks[i];
    json << "      {\"chunkId\":" << chunk.chunkId
         << ",\"reserveBytes\":" << chunk.reserveBytes
         << ",\"chunkOccupiedBytes\":" << chunk.chunkOccupiedBytes
         << ",\"freePayloadBytes\":" << chunk.freePayloadBytes
         << ",\"freeRangeCount\":" << chunk.freeRangeCount
         << ",\"sharedMappedRefs\":" << chunk.sharedMappedRefs
         << ",\"sharedMappedBytes\":" << chunk.sharedMappedBytes
         << ",\"standaloneMappedRefs\":"
         << chunk.standaloneMappedRefs
         << ",\"standaloneMappedBytes\":"
         << chunk.standaloneMappedBytes
         << ",\"mapFailureCount\":" << chunk.mapFailureCount
         << ",\"unmapFailureCount\":" << chunk.unmapFailureCount
         << ",\"mappingStateFaultCount\":"
         << chunk.mappingStateFaultCount
         << ",\"boundResources\":" << chunk.boundResources
         << ",\"boundLiveAlignedSliceBytes\":"
         << chunk.boundLiveAlignedSliceBytes
         << ",\"candidateAlignedSliceBytes\":"
         << chunk.candidateAlignedSliceBytes
         << ",\"directCandidateAlignedSliceBytes\":"
         << chunk.directCandidateAlignedSliceBytes
         << ",\"lazyReadbackCandidateAlignedSliceBytes\":"
         << chunk.lazyReadbackCandidateAlignedSliceBytes
         << ",\"nonCandidateAlignedSliceBytes\":"
         << chunk.nonCandidateAlignedSliceBytes
         << ",\"unclassifiedAlignedSliceBytes\":"
         << chunk.unclassifiedAlignedSliceBytes
         << ",\"mappedBindingAlignedSliceBytes\":"
         << chunk.mappedBindingAlignedSliceBytes
         << ",\"observedCandidateOnly\":"
         << boolJson(chunk.observedCandidateOnly)
         << ",\"reserveReclaimUpperBoundBytes\":"
         << chunk.reserveReclaimUpperBoundBytes
         << ",\"mappedReclaimUpperBoundBytes\":"
         << chunk.mappedReclaimUpperBoundBytes << "}"
         << (i + 1u < chunkDiagnostics.chunks.size() ? ",\n" : "\n");
  }
  json << "    ]}},\n";
  const auto& gpuSkinPools = residency.gpuSkinPools;
  json << "    \"gpuSkinPools\": {"
       << "\"staticAtlasReservedBytes\":"
       << gpuSkinPools.staticAtlasReservedBytes
       << ",\"staticAtlasUsedBytes\":"
       << gpuSkinPools.staticAtlasUsedBytes
       << ",\"staticResourceRecords\":"
       << gpuSkinPools.staticResourceRecords
       << ",\"staticReadyRecords\":"
       << gpuSkinPools.staticReadyRecords
       << ",\"staticPendingRecords\":"
       << gpuSkinPools.staticPendingRecords
       << ",\"staticSubmittedRecords\":"
       << gpuSkinPools.staticSubmittedRecords
       << ",\"staticInvalidRecords\":"
       << gpuSkinPools.staticInvalidRecords
       << ",\"queuedStaticMissRecords\":"
       << gpuSkinPools.queuedStaticMissRecords
       << ",\"queuedStaticMissHostBytes\":"
       << gpuSkinPools.queuedStaticMissHostBytes
       << ",\"peakQueuedStaticMissRecords\":"
       << gpuSkinPools.peakQueuedStaticMissRecords
       << ",\"peakQueuedStaticMissHostBytes\":"
       << gpuSkinPools.peakQueuedStaticMissHostBytes
       << ",\"readyStaticUploadCount\":"
       << gpuSkinPools.readyStaticUploadCount
       << ",\"readyStaticUploadBytes\":"
       << gpuSkinPools.readyStaticUploadBytes
       << ",\"retiredStaticUploadCount\":"
       << gpuSkinPools.retiredStaticUploadCount
       << ",\"retiredStaticUploadBytes\":"
       << gpuSkinPools.retiredStaticUploadBytes
       << ",\"uploadResidentBytes\":"
       << gpuSkinPools.uploadResidentBytes
       << ",\"uploadActivePages\":"
       << gpuSkinPools.uploadActivePages
       << ",\"uploadActiveCapacityBytes\":"
       << gpuSkinPools.uploadActiveCapacityBytes
       << ",\"uploadActiveUsedBytes\":"
       << gpuSkinPools.uploadActiveUsedBytes
       << ",\"uploadPendingPages\":"
       << gpuSkinPools.uploadPendingPages
       << ",\"uploadPendingCapacityBytes\":"
       << gpuSkinPools.uploadPendingCapacityBytes
       << ",\"uploadPendingUsedBytes\":"
       << gpuSkinPools.uploadPendingUsedBytes
       << ",\"uploadRetiredPages\":"
       << gpuSkinPools.uploadRetiredPages
       << ",\"uploadRetiredCapacityBytes\":"
       << gpuSkinPools.uploadRetiredCapacityBytes
       << ",\"uploadRetiredUsedBytes\":"
       << gpuSkinPools.uploadRetiredUsedBytes
       << ",\"uploadIdlePages\":"
       << gpuSkinPools.uploadIdlePages
       << ",\"uploadIdleCapacityBytes\":"
       << gpuSkinPools.uploadIdleCapacityBytes
       << ",\"outputResidentBytes\":"
       << gpuSkinPools.outputResidentBytes
       << ",\"outputPages\":" << gpuSkinPools.outputPages
       << ",\"outputCapacityBytes\":"
       << gpuSkinPools.outputCapacityBytes
       << ",\"outputCursorBytes\":"
       << gpuSkinPools.outputCursorBytes
       << ",\"outputOutstandingSlices\":"
       << gpuSkinPools.outputOutstandingSlices
       << ",\"outputActiveLeases\":"
       << gpuSkinPools.outputActiveLeases
       << ",\"outputRetiredLeases\":"
       << gpuSkinPools.outputRetiredLeases << "},\n";
  const auto& modelMemory = residency.modelCache;
  json << "    \"modelCache\": {"
       << "\"uniqueGeosetRecords\":" << modelMemory.uniqueGeosetRecords
       << ",\"residentGeosetRecords\":"
       << modelMemory.residentGeosetRecords
       << ",\"uniqueGeosetCapacityBytes\":"
       << modelMemory.uniqueGeosetCapacityBytes
       << ",\"residentGeosetCapacityBytes\":"
       << modelMemory.residentGeosetCapacityBytes
       << ",\"aliasDuplicateCapacityBytes\":"
       << modelMemory.aliasDuplicateCapacityBytes
       << ",\"positionsCapacityBytes\":"
       << modelMemory.positionsCapacityBytes
       << ",\"normalsCapacityBytes\":"
       << modelMemory.normalsCapacityBytes
       << ",\"groupSlotsCapacityBytes\":"
       << modelMemory.groupSlotsCapacityBytes
       << ",\"uvCapacityBytes\":" << modelMemory.uvCapacityBytes
       << ",\"primitiveCapacityBytes\":"
       << modelMemory.primitiveCapacityBytes
       << ",\"indicesCapacityBytes\":"
       << modelMemory.indicesCapacityBytes
       << ",\"matrixGroupsCapacityBytes\":"
       << modelMemory.matrixGroupsCapacityBytes
       << ",\"matrixIndicesCapacityBytes\":"
       << modelMemory.matrixIndicesCapacityBytes
       << ",\"modelPointerCapacityBytes\":"
       << modelMemory.modelPointerCapacityBytes
       << ",\"hashContainerOverheadIncluded\":"
       << modelMemory.hashContainerOverheadIncluded
       << ",\"shadowStoreRecords\":" << modelMemory.shadowStoreRecords
       << ",\"shadowStoreRecordVectorCapacityBytes\":"
       << modelMemory.shadowStoreRecordVectorCapacityBytes
       << ",\"shadowStorePayloadCapacityBytes\":"
       << modelMemory.shadowStorePayloadCapacityBytes
       << ",\"shadowStoreHashContainerOverheadIncluded\":"
       << modelMemory.shadowStoreHashContainerOverheadIncluded << "},\n";
  json << "    \"buckets\": [\n";
  for (size_t i = 0u; i < residency.buckets.size(); ++i) {
    const auto& bucket = residency.buckets[i];
    json << "      {\"class\":\""
         << resource_census::ResourceClassName(bucket.resourceClass)
         << "\",\"pool\":" << bucket.pool
         << ",\"mapMode\":" << bucket.mapMode
         << ",\"dynamic\":" << boolJson(bucket.dynamic)
         << ",\"writeOnly\":" << boolJson(bucket.writeOnly)
         << ",\"resources\":" << bucket.resources
         << ",\"logicalBytes\":" << bucket.logicalBytes
         << ",\"deviceAllocationBytes\":"
         << bucket.deviceAllocationBytes
         << ",\"hostBackingLogicalBytes\":"
         << bucket.hostBackingLogicalBytes
         << ",\"hostMappedLogicalBytes\":"
         << bucket.hostMappedLogicalBytes
         << ",\"duplicateHostBackingLogicalBytes\":"
         << bucket.duplicateHostBackingLogicalBytes
         << ",\"lockCalls\":" << bucket.lockCalls
         << ",\"activeLocks\":" << bucket.activeLocks
         << ",\"readLocks\":" << bucket.readLocks
         << ",\"writeLocks\":" << bucket.writeLocks
         << ",\"requestedDiscardLocks\":"
         << bucket.requestedDiscardLocks
         << ",\"effectiveDiscardLocks\":"
         << bucket.effectiveDiscardLocks
         << ",\"requestedNoOverwriteLocks\":"
         << bucket.requestedNoOverwriteLocks
         << ",\"effectiveNoOverwriteLocks\":"
         << bucket.effectiveNoOverwriteLocks
         << ",\"fullSubresourceWriteLocks\":"
         << bucket.fullSubresourceWriteLocks
         << ",\"partialSubresourceWriteLocks\":"
         << bucket.partialSubresourceWriteLocks
         << ",\"externalDirtyCalls\":" << bucket.externalDirtyCalls
         << ",\"neverLockedResources\":"
         << bucket.neverLockedResources
         << ",\"neverLockedDuplicateHostBytes\":"
         << bucket.neverLockedDuplicateHostBytes
         << ",\"observedCandidateResources\":"
         << bucket.observedCandidateResources
         << ",\"observedCandidateHostBytes\":"
         << bucket.observedCandidateHostBytes
         << ",\"lazyReadbackCandidateResources\":"
         << bucket.lazyReadbackCandidateResources
         << ",\"lazyReadbackCandidateHostBytes\":"
         << bucket.lazyReadbackCandidateHostBytes << "}"
         << (i + 1u < residency.buckets.size() ? ",\n" : "\n");
  }
  json << "    ],\n";
  json << "    \"largestHostBackings\": [\n";
  for (size_t i = 0u; i < residency.largestHostBackings.size(); ++i) {
    const auto& entry = residency.largestHostBackings[i];
    json << "      {\"id\":" << entry.id << ",\"class\":\""
         << resource_census::ResourceClassName(entry.resourceClass)
         << "\",\"observation\":\""
         << resource_census::CandidateObservationClassName(
                entry.candidateClass)
         << "\",\"pool\":" << entry.pool
         << ",\"usage\":" << entry.usage
         << ",\"mapMode\":" << entry.mapMode
         << ",\"subresourceCount\":" << entry.subresourceCount
         << ",\"logicalBytes\":" << entry.logicalBytes
         << ",\"deviceAllocationBytes\":"
         << entry.deviceAllocationBytes
         << ",\"hostBackingLogicalBytes\":"
         << entry.hostBackingLogicalBytes
         << ",\"hostMappedLogicalBytes\":"
         << entry.hostMappedLogicalBytes
         << ",\"duplicateHostBackingLogicalBytes\":"
         << entry.duplicateHostBackingLogicalBytes
         << ",\"lockCalls\":" << entry.lockCalls
         << ",\"activeLocks\":" << entry.activeLocks
         << ",\"readLocks\":" << entry.readLocks
         << ",\"writeLocks\":" << entry.writeLocks
         << ",\"fullSubresourceWriteLocks\":"
         << entry.fullSubresourceWriteLocks
         << ",\"partialSubresourceWriteLocks\":"
         << entry.partialSubresourceWriteLocks
         << ",\"externalDirtyCalls\":" << entry.externalDirtyCalls
         << ",\"lastLockFrame\":" << entry.lastLockFrame
         << ",\"lastWriteFrame\":" << entry.lastWriteFrame
         << ",\"lastUploadFrame\":" << entry.lastUploadFrame
         << ",\"hostBinding\":{\"class\":\""
         << resource_census::HostBackingBindingClassName(
                entry.hostBinding.bindingClass)
         << "\",\"chunkId\":" << entry.hostBinding.chunkId
         << ",\"offset\":" << entry.hostBinding.offset
         << ",\"alignedSliceBytes\":"
         << entry.hostBinding.alignedSliceBytes
         << ",\"mapped\":" << boolJson(entry.hostBinding.mapped)
         << "}}"
         << (i + 1u < residency.largestHostBackings.size()
                 ? ",\n" : "\n");
  }
  json << "    ]\n";
  json << "  },\n";

  const auto& gpuSkinConfig = snapshot.gpuSkinRuntimeConfig;
  const auto& gpuSkinFingerprint = snapshot.gpuSkinNativeFingerprint;
  const auto& gpuSkin = snapshot.gpuSkinNativeCounters;
  const uint32_t gpuSkinPoisonSidecarPolicyValue = static_cast<uint32_t>(
      gpuSkinConfig.outsidePoisonSidecarPolicy);
  const bool gpuSkinPoisonSidecarPolicyValid =
      gpu_skin::GpuSkinOutsidePoisonSidecarPolicyValid(
          gpuSkinConfig.outsidePoisonSidecarPolicy);
  const char* gpuSkinPoisonSidecarPolicyName =
      gpuSkinPoisonSidecarPolicyValid
      ? gpu_skin::kGpuSkinOutsidePoisonSidecarPolicyNames[
            gpuSkinPoisonSidecarPolicyValue]
      : "invalid";
  const bool gpuSkinPoisonSidecarConfigCounterExact =
      gpuSkinPoisonSidecarPolicyValue ==
          gpuSkin.productionOutsidePoisonSidecarPolicy &&
      gpuSkinConfig.outsidePoisonSidecarPolicyExplicit ==
          gpuSkin.productionOutsidePoisonSidecarPolicyExplicit &&
      gpuSkinConfig.outsidePoisonSidecarPolicyInvalid ==
          gpuSkin.productionOutsidePoisonSidecarPolicyInvalid;
  json << "  \"gpuSkinSnapshot\": {\n";
  json << "    \"snapshotContract\": \"export-time-process-lifetime-v1\",\n";
  json << "    \"counterConsistency\": \"relaxed-non-quiescent\",\n";
  json << "    \"timingAggregation\": \"mixed-inclusive-do-not-sum\",\n";
  json << "    \"processId\": " << snapshot.processId << ",\n";
  json << "    \"processStartFileTime100ns\": "
       << snapshot.processStartFileTime100ns << ",\n";
  json << "    \"processStartFileTime100nsExact\": \""
       << snapshot.processStartFileTime100ns << "\",\n";
  json << "    \"hooksEnabled\": "
       << (snapshot.gpuSkinHooksEnabled ? "true" : "false") << ",\n";
  json << "    \"mode\": \"" << gpuSkinModeName(gpuSkinConfig.mode)
       << "\",\n";
  json << "    \"modeValue\": "
       << static_cast<uint32_t>(gpuSkinConfig.mode) << ",\n";
  json << "    \"fullDiagnostics\": "
       << (gpuSkinConfig.fullDiagnostics ? "true" : "false") << ",\n";
  json << "    \"diagnosticPeriodFrames\": "
       << gpuSkinConfig.diagnosticPeriodFrames << ",\n";
  json << "    \"diffSamplePeriod\": " << gpuSkinConfig.diffSamplePeriod
       << ",\n";
  json << "    \"outsidePoisonSidecar\": {\"policy\": \""
       << gpuSkinPoisonSidecarPolicyName << "\", \"policyValue\": "
       << gpuSkinPoisonSidecarPolicyValue << ", \"explicit\": "
       << (gpuSkinConfig.outsidePoisonSidecarPolicyExplicit
               ? "true" : "false")
       << ", \"invalid\": "
       << (gpuSkinConfig.outsidePoisonSidecarPolicyInvalid
               ? "true" : "false")
       << ", \"counterPolicyValue\": "
       << gpuSkin.productionOutsidePoisonSidecarPolicy
       << ", \"counterExplicit\": "
       << (gpuSkin.productionOutsidePoisonSidecarPolicyExplicit
               ? "true" : "false")
       << ", \"counterInvalid\": "
       << (gpuSkin.productionOutsidePoisonSidecarPolicyInvalid
               ? "true" : "false")
       << ", \"counterClosureClean\": "
       << (gpuSkin.productionOutsidePoisonSidecarPolicyClosureClean
               ? "true" : "false")
       << ", \"configCounterExact\": "
       << (gpuSkinPoisonSidecarConfigCounterExact ? "true" : "false")
       << "},\n";
  json << "    \"telemetry\": {\"flushes\": "
       << gpuSkin.telemetryFlushes << ", \"batchedAdds\": "
       << gpuSkin.telemetryBatchedAdds << ", \"deltaPending\": "
       << gpuSkin.telemetryDeltaPending << ", \"deltaFaulted\": "
       << (gpuSkin.telemetryDeltaFaulted ? "true" : "false") << "},\n";
  json << "    \"nativeFingerprint\": {\n";
  json << "      \"exactMatch\": "
       << (gpuSkinFingerprint.exactMatch ? "true" : "false") << ",\n";
  json << "      \"failureMask\": " << gpuSkinFingerprint.failureMask
       << ",\n";
  json << "      \"peTimestamp\": " << gpuSkinFingerprint.peTimestamp
       << ",\n";
  json << "      \"imageSize\": " << gpuSkinFingerprint.imageSize
       << ",\n";
  json << "      \"imageChecksum\": " << gpuSkinFingerprint.imageChecksum
       << "\n";
  json << "    },\n";
  json << "    \"admission\": {\n";
  json << "      \"flushNotifications\": " << gpuSkin.flushNotifications
       << ",\n";
  json << "      \"dispatchScopes\": " << gpuSkin.dispatchScopes << ",\n";
  json << "      \"commonDispatchScopes\": " << gpuSkin.commonDispatchScopes
       << ",\n";
  json << "      \"specialDispatchScopes\": "
       << gpuSkin.specialDispatchScopes << ",\n";
  json << "      \"semanticScopes\": " << gpuSkin.semanticScopes << ",\n";
  json << "      \"uploads\": " << gpuSkin.uploads << ",\n";
  json << "      \"uploadsOutsideDispatch\": "
       << gpuSkin.uploadsOutsideDispatch << ",\n";
  json << "      \"nativeEligibleUploads\": "
       << gpuSkin.nativeEligibleUploads << ",\n";
  json << "      \"fastRejectScope\": " << gpuSkin.productionFastRejectScope
       << ",\n";
  json << "      \"fastRejectState\": " << gpuSkin.productionFastRejectState
       << ",\n";
  json << "      \"fastRejectSkinFormat\": "
       << gpuSkin.productionFastRejectSkinFormat << ",\n";
  json << "      \"fastRejectInput\": " << gpuSkin.productionFastRejectInput
       << ",\n";
  json << "      \"fastRejectSmall\": " << gpuSkin.productionFastRejectSmall
       << ",\n";
  json << "      \"fastRejectUnknownState\": "
       << gpuSkin.productionFastRejectUnknownState << ",\n";
  json << "      \"candidates\": " << gpuSkin.productionCandidates
       << ",\n";
  json << "      \"poisonRetirementOnly\": "
       << gpuSkin.productionPoisonRetirementOnly << ",\n";
  json << "      \"outsideCallbacksSkipped\": "
       << gpuSkin.productionOutsideCallbacksSkipped << ",\n";
  json << "      \"outsideNativeFastPath\": "
       << gpuSkin.productionOutsideNativeFastPath << ",\n";
  json << "      \"outsideNoPoisonDirectOriginal\": {"
          "\"attempts\": "
       << gpuSkin.productionOutsideNoPoisonDirectAttempts
       << ", \"kernelCalls\": "
       << gpuSkin.productionOutsideNoPoisonDirectKernelCalls
       << ", \"normalReturns\": "
       << gpuSkin.productionOutsideNoPoisonDirectNormalReturns
       << ", \"kernelNoNormalReturns\": "
       << gpuSkin.productionOutsideNoPoisonDirectKernelNoNormalReturns
       << ", \"completed\": "
       << gpuSkin.productionOutsideNoPoisonDirectCompleted
       << ", \"conflicts\": "
       << gpuSkin.productionOutsideNoPoisonDirectConflicts
       << ", \"cancellations\": "
       << gpuSkin.productionOutsideNoPoisonDirectCancellations
       << ", \"active\": "
       << gpuSkin.productionOutsideNoPoisonDirectActive
       << ", \"resetCompletedWhileActive\": "
       << gpuSkin.productionOutsideNoPoisonDirectResetCompletedWhileActive
       << ", \"latePoison\": "
       << gpuSkin.productionOutsideNoPoisonDirectLatePoison << "},\n";
  json << "      \"fastRejectKernelBatches\": "
       << gpuSkin.productionFastRejectKernelBatches << ",\n";
  json << "      \"outsidePoisonScanAttempts\": "
       << gpuSkin.productionOutsidePoisonScanAttempts << ",\n";
  json << "      \"outsidePoisonNoOverlapAdmissions\": "
       << gpuSkin.productionOutsidePoisonNoOverlapAdmissions << ",\n";
  json << "      \"outsidePoisonOverlapRejects\": "
       << gpuSkin.productionOutsidePoisonOverlapRejects << ",\n";
  json << "      \"outsidePoisonReadFailRejects\": "
       << gpuSkin.productionOutsidePoisonReadFailRejects << ",\n";
  json << "      \"outsideFastKernelMarkerConflicts\": "
       << gpuSkin.productionOutsideFastKernelMarkerConflicts << ",\n";
  const size_t outsideUnknownReason = static_cast<size_t>(
      gpu_skin::NativeOutsideUploadRejectReason::Unknown);
  const size_t outsidePoisonReadReason = static_cast<size_t>(
      gpu_skin::NativeOutsideUploadRejectReason::PoisonReadFailure);
  const size_t outsidePoisonOverlapReason = static_cast<size_t>(
      gpu_skin::NativeOutsideUploadRejectReason::PoisonOverlap);
  const size_t outsidePoisonRevalidationReason = static_cast<size_t>(
      gpu_skin::NativeOutsideUploadRejectReason::
          PoisonPostScanRevalidation);
  const size_t outsideIndependentRevalidationReason = static_cast<size_t>(
      gpu_skin::NativeOutsideUploadRejectReason::
          IndependentPinRevalidation);
  bool outsideAdmissionExportArithmeticClean = true;
  const auto checkedOutsideExportAdd = [
      &outsideAdmissionExportArithmeticClean](uint64_t& destination,
                                                uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - destination) {
      outsideAdmissionExportArithmeticClean = false;
      return;
    }
    destination += value;
  };
  uint64_t outsidePoisonReadReasonTotal = 0u;
  checkedOutsideExportAdd(
      outsidePoisonReadReasonTotal,
      gpuSkin.productionOutsideAdmissionRejectWithPoison[
          outsidePoisonReadReason]);
  checkedOutsideExportAdd(
      outsidePoisonReadReasonTotal,
      gpuSkin.productionOutsideAdmissionRejectWithPoison[
          outsidePoisonRevalidationReason]);
  checkedOutsideExportAdd(
      outsidePoisonReadReasonTotal,
      gpuSkin.productionOutsideAdmissionRejectWithPoison[
          outsideIndependentRevalidationReason]);
  uint64_t outsidePoisonScanTerminalTotal = 0u;
  checkedOutsideExportAdd(
      outsidePoisonScanTerminalTotal,
      gpuSkin.productionOutsidePoisonNoOverlapAdmissions);
  checkedOutsideExportAdd(
      outsidePoisonScanTerminalTotal,
      gpuSkin.productionOutsidePoisonOverlapRejects);
  checkedOutsideExportAdd(
      outsidePoisonScanTerminalTotal,
      gpuSkin.productionOutsidePoisonReadFailRejects);
  uint64_t outsideTelemetryBatchedAddTerm = 0u;
  checkedOutsideExportAdd(
      outsideTelemetryBatchedAddTerm,
      gpuSkin.productionOutsideAdmissionAttemptTotal);
  checkedOutsideExportAdd(
      outsideTelemetryBatchedAddTerm,
      gpuSkin.productionOutsideAdmissionCancellations);
  checkedOutsideExportAdd(
      outsideTelemetryBatchedAddTerm,
      gpuSkin.productionOutsideAdmissionLifecycleExcluded);
  checkedOutsideExportAdd(
      outsideTelemetryBatchedAddTerm,
      gpuSkin.productionOutsideAdmissionTrackedResolvedInside);
  checkedOutsideExportAdd(
      outsideTelemetryBatchedAddTerm,
      gpuSkin.productionOutsideAdmissionUntrackedResolvedOutside);
  const bool outsidePoisonScanClosed =
      outsideAdmissionExportArithmeticClean &&
      gpuSkin.productionOutsidePoisonScanAttempts ==
          outsidePoisonScanTerminalTotal;
  const bool outsidePoisonReasonClosed =
      outsideAdmissionExportArithmeticClean &&
      gpuSkin.productionOutsideAdmissionRejectNoPoison[
          outsidePoisonReadReason] == 0u &&
      gpuSkin.productionOutsideAdmissionRejectNoPoison[
          outsidePoisonOverlapReason] == 0u &&
      gpuSkin.productionOutsideAdmissionRejectNoPoison[
          outsidePoisonRevalidationReason] == 0u &&
      gpuSkin.productionOutsideAdmissionRejectWithPoison[
          outsidePoisonOverlapReason] ==
          gpuSkin.productionOutsidePoisonOverlapRejects &&
      outsidePoisonReadReasonTotal ==
          gpuSkin.productionOutsidePoisonReadFailRejects;
  const bool outsideAdmissionExactFieldsZero =
      gpuSkin.productionOutsideAdmissionAttemptTotal == 0u &&
      gpuSkin.productionOutsideAdmissionCancellations == 0u &&
      gpuSkin.productionOutsideAdmissionLifecycleExcluded == 0u &&
      gpuSkin.productionOutsideAdmissionTrackedResolvedInside == 0u &&
      gpuSkin.productionOutsideAdmissionUntrackedResolvedOutside == 0u &&
      gpuSkin.productionOutsideAdmissionTrackedResolvedOutside == 0u &&
      gpuSkin.productionOutsideAdmissionResolvedExpectedOutside == 0u;
  uint64_t outsideCoverBeginTotal = 0u;
  checkedOutsideExportAdd(
      outsideCoverBeginTotal,
      gpuSkin.productionOutsideCoverFlushBegins);
  checkedOutsideExportAdd(
      outsideCoverBeginTotal,
      gpuSkin.productionOutsideCoverSemanticBegins);
  checkedOutsideExportAdd(
      outsideCoverBeginTotal,
      gpuSkin.productionOutsideCoverIndependentBegins);
  uint64_t outsideCoveredOrDirectTotal = outsideCoverBeginTotal;
  checkedOutsideExportAdd(
      outsideCoveredOrDirectTotal,
      gpuSkin.productionOutsideNoPoisonDirectAttempts);
  const bool outsideCoverAcceptedClosed =
      outsideAdmissionExportArithmeticClean &&
      outsideCoveredOrDirectTotal ==
          gpuSkin.productionOutsideAdmissionAcceptedTotal;
  const bool outsideIndependentPinClosed =
      gpuSkin.productionOutsideIndependentPinBegins ==
          gpuSkin.productionOutsideIndependentPinEnds;
  const bool outsideCoverFieldsZero =
      outsideCoverBeginTotal == 0u &&
      gpuSkin.productionOutsideIndependentPinBegins == 0u &&
      gpuSkin.productionOutsideIndependentPinEnds == 0u;
  json << "      \"outsideAdmissionExact\": {\n";
  json << "        \"populationContract\": "
          "\"bypass-light-entry-dispatch-depth-zero-first-terminal-"
          "plus-final-boundary-v2\",\n";
  json << "        \"attemptContract\": "
          "\"accepted-plus-first-reject-disjoint\",\n";
  json << "        \"outsideClosureDenominator\": "
          "\"attempt-minus-cancellation-minus-lifecycleExcluded-minus-"
          "trackedResolvedInside-plus-untrackedResolvedOutside\",\n";
  json << "        \"availabilityContract\": "
          "\"render-thread-quiescent-published-live-ingress-no-exclusions\",\n";
  json << "        \"reasonOrder\": [";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadRejectReasonNames.size(); ++i) {
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadRejectReasonNames[i]
         << "\"";
  }
  json << "],\n";
  json << "        \"accepted\": {\"noPoison\": "
       << gpuSkin.productionOutsideAdmissionAcceptedNoPoison
       << ", \"withPoison\": "
       << gpuSkin.productionOutsideAdmissionAcceptedWithPoison
       << ", \"total\": "
       << gpuSkin.productionOutsideAdmissionAcceptedTotal << "},\n";
  json << "        \"noPoisonDirectOriginalAttempts\": "
       << gpuSkin.productionOutsideNoPoisonDirectAttempts << ",\n";
  json << "        \"cancellations\": "
       << gpuSkin.productionOutsideAdmissionCancellations
       << ", \"lifecycleExcluded\": "
       << gpuSkin.productionOutsideAdmissionLifecycleExcluded
       << ", \"trackedResolvedInside\": "
       << gpuSkin.productionOutsideAdmissionTrackedResolvedInside
       << ", \"untrackedResolvedOutside\": "
       << gpuSkin.productionOutsideAdmissionUntrackedResolvedOutside << ",\n";
  json << "        \"rejectNoPoison\": {";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadRejectReasonNames.size(); ++i) {
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadRejectReasonNames[i]
         << "\": "
         << gpuSkin.productionOutsideAdmissionRejectNoPoison[i];
  }
  json << "},\n";
  json << "        \"rejectWithPoison\": {";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadRejectReasonNames.size(); ++i) {
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadRejectReasonNames[i]
         << "\": "
         << gpuSkin.productionOutsideAdmissionRejectWithPoison[i];
  }
  json << "},\n";
  json << "        \"totals\": {\"rejectNoPoison\": "
       << gpuSkin.productionOutsideAdmissionRejectNoPoisonTotal
       << ", \"rejectWithPoison\": "
       << gpuSkin.productionOutsideAdmissionRejectWithPoisonTotal
       << ", \"reject\": "
       << gpuSkin.productionOutsideAdmissionRejectTotal
       << ", \"attempt\": "
       << gpuSkin.productionOutsideAdmissionAttemptTotal
       << ", \"trackedResolvedOutside\": "
       << gpuSkin.productionOutsideAdmissionTrackedResolvedOutside
       << ", \"resolvedExpectedOutside\": "
       << gpuSkin.productionOutsideAdmissionResolvedExpectedOutside
       << ", \"actualOutside\": " << gpuSkin.uploadsOutsideDispatch
       << "},\n";
  json << "        \"closure\": {\"snapshotAvailable\": "
       << (gpuSkin.productionOutsideAdmissionSnapshotAvailable
               ? "true" : "false")
       << ", \"unknownHardZero\": "
       << (gpuSkin.productionOutsideAdmissionUnknownHardZero
               ? "true" : "false")
       << ", \"acceptedResolutionClean\": "
       << (gpuSkin.productionOutsideAdmissionAcceptedResolutionClean
               ? "true" : "false")
       << ", \"outsideClean\": "
       << (gpuSkin.productionOutsideAdmissionOutsideClosureClean
               ? "true" : "false")
       << ", \"poisonScanClean\": "
       << (outsidePoisonScanClosed ? "true" : "false")
       << ", \"poisonReasonClean\": "
       << (outsidePoisonReasonClosed ? "true" : "false")
       << ", \"arithmeticClean\": "
       << (outsideAdmissionExportArithmeticClean ? "true" : "false")
       << ", \"zeroWhenFull\": "
       << (!gpuSkinConfig.fullDiagnostics || outsideAdmissionExactFieldsZero
               ? "true" : "false") << "},\n";
  json << "        \"telemetryBatchedAddTerm\": "
       << outsideTelemetryBatchedAddTerm
       << ",\n";
  json << "        \"unknownIndex\": " << outsideUnknownReason << "\n";
  json << "      },\n";
  json << "      \"outsideUploadCover\": {\n";
  json << "        \"contract\": "
          "\"accepted-fast-minus-direct-borrows-exact-flush-or-semantic-"
          "otherwise-independent-v2\",\n";
  json << "        \"begins\": {\"flush\": "
       << gpuSkin.productionOutsideCoverFlushBegins
       << ", \"semantic\": "
       << gpuSkin.productionOutsideCoverSemanticBegins
       << ", \"independent\": "
       << gpuSkin.productionOutsideCoverIndependentBegins
       << ", \"total\": " << outsideCoverBeginTotal << "},\n";
  json << "        \"independentPins\": {\"begins\": "
       << gpuSkin.productionOutsideIndependentPinBegins
       << ", \"ends\": "
       << gpuSkin.productionOutsideIndependentPinEnds << "},\n";
  json << "        \"acceptedTotal\": "
       << gpuSkin.productionOutsideAdmissionAcceptedTotal << ",\n";
  json << "        \"directOriginalAttempts\": "
       << gpuSkin.productionOutsideNoPoisonDirectAttempts << ",\n";
  json << "        \"closure\": {\"arithmeticClean\": "
       << (outsideAdmissionExportArithmeticClean ? "true" : "false")
       << ", \"acceptedClean\": "
       << (outsideCoverAcceptedClosed ? "true" : "false")
       << ", \"independentPinsClean\": "
       << (outsideIndependentPinClosed ? "true" : "false")
       << ", \"zeroWhenFull\": "
       << (!gpuSkinConfig.fullDiagnostics || outsideCoverFieldsZero
               ? "true" : "false") << "}\n";
  json << "      },\n";
  json << "      \"managerDispatch\": {\n";
  json << "        \"physicalScopes\": " << gpuSkin.dispatchScopes
       << ", \"physicalEnds\": " << gpuSkin.dispatchScopeEnds << ",\n";
  json << "        \"commonScopes\": " << gpuSkin.commonDispatchScopes
       << ", \"specialScopes\": " << gpuSkin.specialDispatchScopes
       << ", \"semanticScopes\": " << gpuSkin.semanticScopes << ",\n";
  json << "        \"eagerScopes\": "
       << gpuSkin.managerDispatchEagerScopes << ", \"lazyScopes\": "
       << gpuSkin.managerDispatchLazyScopes << ", \"neverScopes\": "
       << gpuSkin.managerDispatchNeverScopes << ", \"evidenceEagerScopes\": "
       << gpuSkin.managerDispatchEvidenceEagerScopes << ",\n";
  json << "        \"beginCallbacks\": "
       << gpuSkin.managerDispatchBeginCallbacks << ", \"endCallbacks\": "
       << gpuSkin.managerDispatchEndCallbacks << ", \"eagerBegins\": "
       << gpuSkin.managerDispatchEagerBegins << ",\n";
  json << "        \"lazyAdmissionAttempts\": "
       << gpuSkin.managerDispatchLazyAdmissionAttempts
       << ", \"lazyAdmissions\": "
       << gpuSkin.managerDispatchLazyAdmissions << ",\n";
  json << "        \"eagerAdmissionFailures\": "
       << gpuSkin.managerDispatchEagerAdmissionFailures
       << ", \"lazyAdmissionFailures\": "
       << gpuSkin.managerDispatchLazyAdmissionFailures
       << ", \"neverSafetyFailures\": "
       << gpuSkin.managerDispatchNeverSafetyFailures << ",\n";
  json << "        \"issuedEnds\": "
       << gpuSkin.managerDispatchIssuedEnds << ", \"noUploadEnds\": "
       << gpuSkin.managerDispatchNoUploadEnds << ", \"neverEnds\": "
       << gpuSkin.managerDispatchNeverEnds << ", \"failedEnds\": "
       << gpuSkin.managerDispatchFailedEnds << ",\n";
  json << "        \"skippedUploads\": "
       << gpuSkin.managerDispatchSkippedUploads << ", \"skippedDips\": "
       << gpuSkin.managerDispatchSkippedDips << ", \"skippedFanouts\": "
       << gpuSkin.managerDispatchSkippedFanouts << ",\n";
  json << "        \"telemetryFlushes\": "
       << gpuSkin.telemetryFlushes << ", \"telemetryBatchedAdds\": "
       << gpuSkin.telemetryBatchedAdds << ", \"telemetryDeltaPending\": "
       << gpuSkin.telemetryDeltaPending << ", \"telemetryDeltaFaulted\": "
       << (gpuSkin.telemetryDeltaFaulted ? "true" : "false") << "\n";
  json << "      },\n";
  json << "      \"nativeFanout\": {\"zero\": "
       << gpuSkin.uploadFanoutZero << ", \"one\": "
       << gpuSkin.uploadFanoutOne << ", \"many\": "
       << gpuSkin.uploadFanoutMany << ", \"max\": "
       << gpuSkin.maxUploadFanout << "}\n";
  json << "    },\n";
  json << "    \"nativeKernel\": {\n";
  json << "      \"originalUploadCalls\": " << gpuSkin.originalUploadCalls
       << ",\n";
  json << "      \"originalUploadBytes\": " << gpuSkin.originalUploadBytes
       << ",\n";
  json << "      \"bypassPreflightAttempts\": "
       << gpuSkin.bypassPreflightAttempts << ",\n";
  json << "      \"bypassAuthorizations\": "
       << gpuSkin.bypassAuthorizations << ",\n";
  json << "      \"kernelHookCalls\": " << gpuSkin.kernelHookCalls << ",\n";
  json << "      \"originalKernelCalls\": " << gpuSkin.originalKernelCalls
       << ",\n";
  json << "      \"originalKernelNormalReturns\": "
       << gpuSkin.originalKernelNormalReturns << ",\n";
  json << "      \"originalKernelNormalReturnRejects\": "
       << gpuSkin.originalKernelNormalReturnRejects << ",\n";
  json << "      \"cpuRewriteProofAttempts\": "
       << gpuSkin.cpuRewriteProofAttempts << ",\n";
  json << "      \"cpuRewriteProofExact\": "
       << gpuSkin.cpuRewriteProofExact << ",\n";
  json << "      \"cpuRewriteProofRejects\": "
       << gpuSkin.cpuRewriteProofRejects << ",\n";
  json << "      \"originalKernelBytes\": " << gpuSkin.originalKernelBytes
       << ",\n";
  json << "      \"bypassedKernelCalls\": " << gpuSkin.bypassedKernelCalls
       << ",\n";
  json << "      \"bypassedKernelBytes\": " << gpuSkin.bypassedKernelBytes
       << ",\n";
  json << "      \"nullMappedKernelFallbacks\": "
       << gpuSkin.nullMappedKernelFallbacks << ",\n";
  json << "      \"kernelPreflightRejects\": "
       << gpuSkin.kernelPreflightRejects << ",\n";
  json << "      \"postSkipMismatchFuses\": "
       << gpuSkin.postSkipMismatchFuses << ",\n";
  json << "      \"postSkipNativeFallback\": "
       << gpuSkin.postSkipNativeFallback << ",\n";
  json << "      \"duplicateKernelCalls\": "
       << gpuSkin.duplicateKernelCalls << ",\n";
  json << "      \"irreversibleKernelSuppressions\": "
       << gpuSkin.irreversibleKernelSuppressions << ",\n";
  json << "      \"pendingKernelAuthorizations\": "
       << gpuSkin.pendingKernelAuthorizations << "\n";
  json << "    },\n";
  json << "    \"safety\": {\n";
  json << "      \"poisonCreates\": " << gpuSkin.nativePoisonCreates
       << ",\n";
  json << "      \"poisonClears\": " << gpuSkin.nativePoisonClears
       << ",\n";
  json << "      \"poisonHits\": " << gpuSkin.nativePoisonHits << ",\n";
  json << "      \"poisonOverflows\": " << gpuSkin.nativePoisonOverflows
       << ",\n";
  json << "      \"poisonLeaks\": " << gpuSkin.nativePoisonLeaks << ",\n";
  json << "      \"poisonOutstanding\": "
       << gpuSkin.nativePoisonOutstanding << ",\n";
  json << "      \"retirementEventsPublished\": "
       << gpuSkin.nativeRetirementEventsPublished << ",\n";
  json << "      \"retirementEventsConsumed\": "
       << gpuSkin.nativeRetirementEventsConsumed << ",\n";
  json << "      \"retirementRangesCleared\": "
       << gpuSkin.nativeRetirementRangesCleared << ",\n";
  json << "      \"retirementQueueOverflows\": "
       << gpuSkin.nativeRetirementQueueOverflows << ",\n";
  json << "      \"indexTicketFailureMask\": "
       << gpuSkin.indexTicketFailureMask << ",\n";
  json << "      \"indexTicketAttempts\": " << gpuSkin.indexTicketAttempts
       << ",\n";
  json << "      \"indexTicketExact\": " << gpuSkin.indexTicketExact
       << ",\n";
  json << "      \"indexTicketSuppressed\": "
       << gpuSkin.indexTicketSuppressed << ",\n";
  json << "      \"indexTicketLeaks\": " << gpuSkin.indexTicketLeaks
       << "\n";
  json << "    },\n";
  json << "    \"drawCorrelation\": {\n";
  json << "      \"dips\": " << gpuSkin.dips << ",\n";
  json << "      \"outsideDispatchDips\": " << gpuSkin.outsideDispatchDips
       << ",\n";
  json << "      \"dispatchNoUploadDips\": "
       << gpuSkin.dispatchNoUploadDips << ",\n";
  json << "      \"correlatedDips\": " << gpuSkin.correlatedDips << ",\n";
  json << "      \"unmatchedDips\": " << gpuSkin.unmatchedDips << "\n";
  json << "    },\n";
  json << "    \"timing\": {\n";
  json << "      \"frequency\": " << gpuSkin.hotPathTimingFrequency << ",\n";
#define WRITE_GPU_SKIN_TIMING_JSON(label, member) \
  json << "      \"" label "\": {\"calls\": " \
       << gpuSkin.member##Calls << ", \"ticks\": " \
       << gpuSkin.member##Ticks << ", \"maxTicks\": " \
       << gpuSkin.member##MaxTicks << "},\n"
  WRITE_GPU_SKIN_TIMING_JSON("beginUpload", beginUploadTiming);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleCommon", beginSampleCommon);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleState", beginSampleState);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleExact", beginSampleExact);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleScopeRoute", beginSampleScopeRoute);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleStateRejectRoute", beginSampleStateRejectRoute);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleSkinRoute", beginSampleSkinRoute);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleSmallRoute", beginSampleSmallRoute);
  WRITE_GPU_SKIN_TIMING_JSON("beginSampleCandidateRoute", beginSampleCandidateRoute);
  WRITE_GPU_SKIN_TIMING_JSON("t2SampleGeoSnap", t2SampleGeoSnap);
  WRITE_GPU_SKIN_TIMING_JSON("t2SampleGeoHeader", t2SampleGeoHeader);
  WRITE_GPU_SKIN_TIMING_JSON("t2SamplePositionProof", t2SamplePositionProof);
  WRITE_GPU_SKIN_TIMING_JSON("t2SampleNormalProof", t2SampleNormalProof);
  WRITE_GPU_SKIN_TIMING_JSON("t2SampleGroupProof", t2SampleGroupProof);
  WRITE_GPU_SKIN_TIMING_JSON("t2SamplePaletteProof", t2SamplePaletteProof);
  WRITE_GPU_SKIN_TIMING_JSON("evaluateKernel", evaluateKernelTiming);
  WRITE_GPU_SKIN_TIMING_JSON("completeUpload", completeUploadTiming);
  WRITE_GPU_SKIN_TIMING_JSON("notifyNormal", notifyNormalTiming);
  WRITE_GPU_SKIN_TIMING_JSON("dip", dipTiming);
  WRITE_GPU_SKIN_TIMING_JSON("outerUpload", outerUploadTiming);
#undef WRITE_GPU_SKIN_TIMING_JSON
  json << "      \"productionLightSampled\": {\n";
  json << "        \"period\": "
       << gpuSkin.productionTimingSamplePeriod << ",\n";
  json << "        \"phase\": "
       << gpuSkin.productionTimingSamplePhase << ",\n";
  json << "        \"populationContract\": "
          "\"outer-kernel-event-independent_callback-stable-id-aligned\",\n";
  json << "        \"writerSnapshot\": {\"started\": "
       << gpuSkin.productionTimingWritesStarted
       << ", \"completed\": " << gpuSkin.productionTimingWritesCompleted
       << ", \"active\": " << gpuSkin.productionTimingWriters
       << ", \"pending\": "
       << (gpuSkin.productionTimingSnapshotPending ? "true" : "false")
       << "},\n";
#define WRITE_GPU_SKIN_SAMPLED_JSON(label, member) \
  json << "        \"" label "\": {\"calls\": " \
       << gpuSkin.member.calls << ", \"ticks\": " \
       << gpuSkin.member.ticks << ", \"maxTicks\": " \
       << gpuSkin.member.maxTicks << "},\n"
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerAdmissionAccepted", productionOuterAdmissionAcceptedTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerAdmissionRejected", productionOuterAdmissionRejectedTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFastInclusive", productionOuterFastInclusiveTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFastBody", productionOuterFastBodyTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFastComplete", productionOuterFastCompleteTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFastCancel", productionOuterFastCancelTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFallbackInclusive", productionOuterFallbackInclusiveTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFallbackBegin", productionOuterFallbackBeginTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFallbackBody", productionOuterFallbackBodyTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "outerFallbackComplete", productionOuterFallbackCompleteTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "kernelInclusive", productionKernelInclusiveTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "kernelEvaluate", productionKernelEvaluateTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "kernelOriginal", productionKernelOriginalTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "kernelNotify", productionKernelNotifyTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "flushRoot", productionFlushRootTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dispatchSemanticLookup", productionDispatchSemanticLookupTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dispatchBeginRoot", productionDispatchBeginRootTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dispatchEndRoot", productionDispatchEndRootTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "semanticInclusive", productionSemanticInclusiveTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "semanticOriginal", productionSemanticOriginalTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipDeviceRootOutside", productionDipDeviceRootOutsideTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipDeviceRootNoUpload", productionDipDeviceRootNoUploadTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipDeviceRootCorrelated", productionDipDeviceRootCorrelatedTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipBridgeOutside", productionDipBridgeOutsideTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipBridgeNoUpload", productionDipBridgeNoUploadTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipBridgeCorrelated", productionDipBridgeCorrelatedTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipResolveOutside", productionDipResolveOutsideTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipResolveNoUpload", productionDipResolveNoUploadTiming);
  WRITE_GPU_SKIN_SAMPLED_JSON(
      "dipResolveCorrelated", productionDipResolveCorrelatedTiming);
#undef WRITE_GPU_SKIN_SAMPLED_JSON
  uint64_t acceptedClassCalls = 0u;
  uint64_t acceptedClassTicks = 0u;
  uint64_t acceptedClassMaxTicks = 0u;
  uint64_t completeClassCalls = 0u;
  uint64_t completeClassTicks = 0u;
  uint64_t completeClassMaxTicks = 0u;
  json << "        \"outerAdmissionAcceptedByClass\": {";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadAdmissionClassNames.size(); ++i) {
    const auto& timing =
        gpuSkin.productionOuterAdmissionAcceptedClassTiming[i];
    acceptedClassCalls += timing.calls;
    acceptedClassTicks += timing.ticks;
    acceptedClassMaxTicks = std::max(acceptedClassMaxTicks, timing.maxTicks);
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadAdmissionClassNames[i]
         << "\": {\"calls\": " << timing.calls
         << ", \"ticks\": " << timing.ticks
         << ", \"maxTicks\": " << timing.maxTicks << "}";
  }
  json << "},\n";
  json << "        \"outerFastCompleteByClass\": {";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadAdmissionClassNames.size(); ++i) {
    const auto& timing =
        gpuSkin.productionOuterFastCompleteClassTiming[i];
    completeClassCalls += timing.calls;
    completeClassTicks += timing.ticks;
    completeClassMaxTicks = std::max(completeClassMaxTicks, timing.maxTicks);
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadAdmissionClassNames[i]
         << "\": {\"calls\": " << timing.calls
         << ", \"ticks\": " << timing.ticks
         << ", \"maxTicks\": " << timing.maxTicks << "}";
  }
  json << "},\n";
  uint64_t fallbackReasonCalls = 0u;
  uint64_t fallbackReasonTicks = 0u;
  uint64_t fallbackReasonMaxTicks = 0u;
  bool aliasFieldsZero = acceptedClassCalls == 0u &&
      acceptedClassTicks == 0u && acceptedClassMaxTicks == 0u &&
      completeClassCalls == 0u && completeClassTicks == 0u &&
      completeClassMaxTicks == 0u;
  json << "        \"outerFallbackByReason\": {";
  for (size_t i = 0u;
       i < gpu_skin::kNativeOutsideUploadRejectReasonNames.size(); ++i) {
    const auto& timing = gpuSkin.productionOuterFallbackReasonTiming[i];
    fallbackReasonCalls += timing.calls;
    fallbackReasonTicks += timing.ticks;
    fallbackReasonMaxTicks = std::max(
        fallbackReasonMaxTicks, timing.maxTicks);
    aliasFieldsZero = aliasFieldsZero && timing.calls == 0u &&
        timing.ticks == 0u && timing.maxTicks == 0u;
    if (i != 0u)
      json << ", ";
    json << "\"" << gpu_skin::kNativeOutsideUploadRejectReasonNames[i]
         << "\": {\"calls\": " << timing.calls
         << ", \"ticks\": " << timing.ticks
         << ", \"maxTicks\": " << timing.maxTicks << "}";
  }
  json << "},\n";
  json << "        \"outerAdmissionAliasClosure\": {"
          "\"contract\": "
          "\"partition-alias-inclusive-do-not-add-to-parent\", "
          "\"snapshotAvailable\": "
       << (gpuSkin.productionOuterAdmissionAliasSnapshotAvailable
               ? "true" : "false")
       << ", \"acceptedClassClean\": "
       << (gpuSkin.productionOuterAdmissionAcceptedClassClosureClean
               ? "true" : "false")
       << ", \"completeClassClean\": "
       << (gpuSkin.productionOuterFastCompleteClassClosureClean
               ? "true" : "false")
       << ", \"fallbackReasonClean\": "
       << (gpuSkin.productionOuterFallbackReasonClosureClean
               ? "true" : "false")
       << ", \"unknownHardZero\": "
       << (gpuSkin.productionOuterFallbackReasonUnknownHardZero
               ? "true" : "false")
       << ", \"zeroWhenFull\": "
       << (!gpuSkinConfig.fullDiagnostics || aliasFieldsZero
               ? "true" : "false") << "},\n";
  static constexpr const char* kGpuSkinCallbackNames[] = {
      "flush", "dispatchBegin", "dispatchEnd", "preflight",
      "cpuRewrite", "upload", "dip", "fanout"};
  json << "        \"bridgeCallbacks\": {\n";
  for (size_t i = 0u; i < gpu_skin::kNativeBridgeCallbackKindCount; ++i) {
    const auto& pinEnter = gpuSkin.productionCallbackPinEnterTiming[i];
    const auto& body = gpuSkin.productionCallbackBodyTiming[i];
    const auto& pinLeave = gpuSkin.productionCallbackPinLeaveTiming[i];
    json << "          \"" << kGpuSkinCallbackNames[i] << "\": {"
         << "\"pinEnter\": {\"calls\": " << pinEnter.calls
         << ", \"ticks\": " << pinEnter.ticks
         << ", \"maxTicks\": " << pinEnter.maxTicks << "}, "
         << "\"body\": {\"calls\": " << body.calls
         << ", \"ticks\": " << body.ticks
         << ", \"maxTicks\": " << body.maxTicks << "}, "
         << "\"pinLeave\": {\"calls\": " << pinLeave.calls
         << ", \"ticks\": " << pinLeave.ticks
         << ", \"maxTicks\": " << pinLeave.maxTicks << "}}"
         << (i + 1u == gpu_skin::kNativeBridgeCallbackKindCount
                 ? "\n" : ",\n");
  }
  json << "        }\n";
  json << "      },\n";
  json << "      \"originalKernel\": {\"calls\": "
       << gpuSkin.originalKernelTimingCalls << ", \"ticks\": "
       << gpuSkin.originalKernelTimingTicks << ", \"maxTicks\": "
       << gpuSkin.originalKernelTimingMaxTicks << "}\n";
  json << "    }\n";
  json << "  },\n";

  const auto writeIdentityDedup = [&](
      const char* label,
      const War3PerfMonitor::ExportSnapshot::IdentitySameFrameDedupStats&
          stats,
      bool trailingComma) {
    const uint64_t misses = stats.missMissingAlias +
        stats.missCrossFrame + stats.missNoBatchProof +
        stats.missIncomplete + stats.missInputMismatch +
        stats.missAliasConflict + stats.missRuntimeOwner;
    json << "    \"" << label << "\": {"
         << "\"attempts\": " << stats.attempts << ", "
         << "\"hits\": " << stats.hits << ", "
         << "\"missMissingAlias\": " << stats.missMissingAlias << ", "
         << "\"missCrossFrame\": " << stats.missCrossFrame << ", "
         << "\"missNoBatchProof\": " << stats.missNoBatchProof << ", "
         << "\"missIncomplete\": " << stats.missIncomplete << ", "
         << "\"missInputMismatch\": " << stats.missInputMismatch << ", "
         << "\"missAliasConflict\": " << stats.missAliasConflict << ", "
         << "\"missRuntimeOwner\": " << stats.missRuntimeOwner << ", "
         << "\"batchMarked\": " << stats.batchMarked << ", "
         << "\"closed\": "
         << (stats.attempts == stats.hits + misses ? "true" : "false")
         << "}" << (trailingComma ? ",\n" : "\n");
  };
  json << "  \"identitySameFrameDedup\": {\n";
  writeIdentityDedup(
      "model", snapshot.modelIdentitySameFrameDedup, true);
  writeIdentityDedup(
      "shadow", snapshot.shadowIdentitySameFrameDedup, false);
  json << "  },\n";

  const auto& shadowAgg = snapshot.shadowBudgetAggregate;
  const auto shadowMetadataSnapshot =
      shadow::ReadWar3ShadowAlphaTestPayloadCountersSnapshot();
  const uint64_t shadowMetadataRejectedByReasonCount =
      shadowMetadataSnapshot.metadataRejectedNoMaterialCount +
      shadowMetadataSnapshot.metadataRejectedOpaqueCount +
      shadowMetadataSnapshot.metadataRejectedNoUvCount +
      shadowMetadataSnapshot.metadataRejectedNoDiffuseCount +
      shadowMetadataSnapshot.metadataRejectedUploadCount +
      shadowMetadataSnapshot.metadataRejectedDuplicateCount;
  const double shadowFrames =
      static_cast<double>(std::max<uint64_t>(shadowAgg.framesObserved, 1u));
  json << "  \"shadowBudgetSummary\": {\n";
  json << "    \"framesObserved\": " << shadowAgg.framesObserved << ",\n";
  json << "    \"framesIncomplete\": " << shadowAgg.framesIncomplete << ",\n";
  json << "    \"framesBudgetExceeded\": " << shadowAgg.framesBudgetExceeded
       << ",\n";
  json << "    \"framesReuseLastComplete\": "
       << shadowAgg.framesReuseLastComplete << ",\n";
  json << "    \"framesRenderCurrentPartial\": "
       << shadowAgg.framesRenderCurrentPartial << ",\n";
  const double shadowReceiverAvgCasters =
      shadowAgg.shadowReceiverFrames != 0u
          ? static_cast<double>(shadowAgg.shadowReceiverReplayCasterCountTotal) /
                static_cast<double>(shadowAgg.shadowReceiverFrames)
          : 0.0;
  const double shadowReceiverAvgGeometryWork =
      shadowAgg.shadowReceiverFrames != 0u
          ? static_cast<double>(shadowAgg.shadowReceiverReplayGeometryWorkTotal) /
                static_cast<double>(shadowAgg.shadowReceiverFrames)
          : 0.0;
  json << "    \"shadowReceiverFrames\": "
       << shadowAgg.shadowReceiverFrames << ",\n";
  json << "    \"shadowReceiverReplayCasterCountAvg\": "
       << shadowReceiverAvgCasters << ",\n";
  json << "    \"shadowReceiverReplayCasterCountMax\": "
       << shadowAgg.shadowReceiverReplayCasterCountMax << ",\n";
  json << "    \"shadowReceiverReplayGeometryWorkAvg\": "
       << shadowReceiverAvgGeometryWork << ",\n";
  json << "    \"shadowReceiverReplayGeometryWorkMax\": "
       << shadowAgg.shadowReceiverReplayGeometryWorkMax << ",\n";
  json << "    \"shadowReceiverAdaptiveResolutionFrames\": "
       << shadowAgg.shadowReceiverAdaptiveResolutionFrames << ",\n";
  json << "    \"shadowReceiverRequestedResolutionLast\": "
       << shadowAgg.shadowReceiverRequestedResolutionLast << ",\n";
  json << "    \"shadowReceiverEffectiveResolutionLast\": "
       << shadowAgg.shadowReceiverEffectiveResolutionLast << ",\n";
  json << "    \"avgBudgetMb\": "
       << (static_cast<double>(shadowAgg.totalBudgetBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"avgUsedMb\": "
       << (static_cast<double>(shadowAgg.totalUsedBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"avgArenaMb\": "
       << (static_cast<double>(shadowAgg.totalArenaBytes) / shadowFrames /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxBudgetMb\": "
       << (static_cast<double>(shadowAgg.maxBudgetBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxUsedMb\": "
       << (static_cast<double>(shadowAgg.maxUsedBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"maxArenaMb\": "
       << (static_cast<double>(shadowAgg.maxArenaBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"arenaClaimCount\": " << shadowAgg.arenaClaimCount << ",\n";
  json << "    \"arenaDispatchClaims\": " << shadowAgg.arenaDispatchClaims
       << ",\n";
  json << "    \"arenaDispatchClaimMisses\": "
       << shadowAgg.arenaDispatchClaimMisses << ",\n";
  json << "    \"arenaCaptured\": " << shadowAgg.arenaCaptured << ",\n";
  json << "    \"legacyCaptured\": " << shadowAgg.legacyCaptured << ",\n";
  json << "    \"directRefCaptured\": " << shadowAgg.directRefCaptured << ",\n";
  json << "    \"arenaFallbackToFreeze\": "
       << shadowAgg.arenaFallbackToFreeze << ",\n";
  json << "    \"arenaDedup\": " << shadowAgg.arenaDedup << ",\n";
  json << "    \"vbLevelCacheHits\": " << shadowAgg.vbLevelCacheHits << ",\n";
  json << "    \"directRefRejectedUnstable\": "
       << shadowAgg.directRefRejectedUnstable << ",\n";
  json << "    \"arenaOverflowCount\": " << shadowAgg.arenaOverflowCount
       << ",\n";
  json << "    \"arenaEmitRejects\": " << shadowAgg.arenaEmitRejects << ",\n";
  json << "    \"arenaRebasedIndexed\": " << shadowAgg.arenaRebasedIndexed
       << ",\n";
  json << "    \"arenaRebasedNonIndexed\": "
       << shadowAgg.arenaRebasedNonIndexed << ",\n";
  json << "    \"arenaRejectUnsupported\": "
       << shadowAgg.arenaRejectUnsupported << ",\n";
  json << "    \"arenaRejectValidation\": "
       << shadowAgg.arenaRejectValidation << ",\n";
  json << "    \"arenaRejectUninitialized\": "
       << shadowAgg.arenaRejectUninitialized << ",\n";
  json << "    \"arenaRejectOverflow\": " << shadowAgg.arenaRejectOverflow
       << ",\n";
  json << "    \"skippedFreezeBudget\": " << shadowAgg.skippedFreezeBudget
       << ",\n";
  json << "    \"skippedPriorityBudget\": " << shadowAgg.skippedPriorityBudget
       << ",\n";
  json << "    \"skippedUpload\": " << shadowAgg.skippedUpload << ",\n";
  json << "    \"skippedCasterCap\": " << shadowAgg.skippedCasterCap << ",\n";
  json << "    \"skippedDistanceCull\": " << shadowAgg.skippedDistanceCull
       << ",\n";
  json << "    \"degradedAlphaBudget\": " << shadowAgg.degradedAlphaBudget
       << ",\n";
  json << "    \"reusedFreezeHits\": " << shadowAgg.reusedFreezeHits
       << ",\n";
  json << "    \"actualFreezeReuseHits\": " << shadowAgg.actualFreezeReuseHits
       << ",\n";
  json << "    \"uniqueGeometryCount\": " << shadowAgg.uniqueGeometryCount
       << ",\n";
  json << "    \"uniqueInstanceableGeometryCount\": "
       << shadowAgg.uniqueInstanceableGeometryCount << ",\n";
  json << "    \"duplicateGeometryInstances\": "
       << shadowAgg.duplicateGeometryInstances << ",\n";
  json << "    \"reuseEligibleDuplicates\": "
       << shadowAgg.reuseEligibleDuplicates << ",\n";
  json << "    \"potentialFreezeReuseHits\": "
       << shadowAgg.potentialFreezeReuseHits << ",\n";
  json << "    \"staticPersistentCount\": "
       << shadowAgg.staticPersistentCount << ",\n";
  json << "    \"dynamicPoseCount\": " << shadowAgg.dynamicPoseCount
       << ",\n";
  json << "    \"dynamicSkinnedOutputCount\": "
       << shadowAgg.dynamicSkinnedOutputCount << ",\n";
  json << "    \"fallbackDrawCount\": " << shadowAgg.fallbackDrawCount
       << ",\n";
  json << "    \"fallbackDrawCountTerrain\": "
       << shadowAgg.fallbackDrawCountTerrain << ",\n";
  json << "    \"fallbackDrawCountWorldObject\": "
       << shadowAgg.fallbackDrawCountWorldObject << ",\n";
  json << "    \"fallbackDrawCountUnitObject\": "
       << shadowAgg.fallbackDrawCountUnitObject << ",\n";
  json << "    \"objectFallbackDrawCount\": "
       << shadowAgg.objectFallbackDrawCount << ",\n";
  json << "    \"drawTimeVBCacheCaptureCount\": "
       << shadowAgg.drawTimeVBCacheCaptureCount << ",\n";
  json << "    \"drawTimeVBCacheConsumeHitCount\": "
       << shadowAgg.drawTimeVBCacheConsumeHitCount << ",\n";
  json << "    \"drawTimeVBCacheConsumeMissCount\": "
       << shadowAgg.drawTimeVBCacheConsumeMissCount << ",\n";
  json << "    \"drawTimeVBCacheRejectNoLayerContext\": "
       << shadowAgg.drawTimeVBCacheRejectNoLayerContext << ",\n";
  json << "    \"drawTimeVBCacheSameFrameDedupMiss\": "
       << shadowAgg.drawTimeVBCacheSameFrameDedupMiss << ",\n";
  json << "    \"semanticBridgeHit\": " << shadowAgg.semanticBridgeHit
       << ",\n";
  json << "    \"semanticBridgeMiss\": " << shadowAgg.semanticBridgeMiss
       << ",\n";
  json << "    \"semanticBridgeBypassed\": "
       << shadowAgg.semanticBridgeBypassed << ",\n";
  json << "    \"semanticSceneSubmitted\": "
       << shadowAgg.semanticSceneSubmitted << ",\n";
  json << "    \"semanticSceneSubmittedUnit\": "
       << shadowAgg.semanticSceneSubmittedUnit << ",\n";
  json << "    \"semanticSceneSubmittedSkinned\": "
       << shadowAgg.semanticSceneSubmittedSkinned << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedNonUnitResolvedCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnknownPacketKindCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnknownPacketKindCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedGroupNonZeroCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedGroupNonZeroCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedTransparentQueueCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedTransparentQueueCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedMissingUnitPtrCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedMissingUnitPtrCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount
       << ",\n";
  json << "    \"semanticSceneSubmittedBuilding\": "
       << shadowAgg.semanticSceneSubmittedBuilding << ",\n";
  json << "    \"semanticSceneSubmittedDestructible\": "
       << shadowAgg.semanticSceneSubmittedDestructible << ",\n";
  json << "    \"semanticSceneSubmittedCutout\": "
       << shadowAgg.semanticSceneSubmittedCutout << ",\n";
  json << "    \"semanticSceneSubmittedAlphaBlend\": "
       << shadowAgg.semanticSceneSubmittedAlphaBlend << ",\n";
  json << "    \"semanticSceneMaterialObservedCutoutCount\": "
       << shadowAgg.semanticSceneMaterialObservedCutoutCount << ",\n";
  json << "    \"semanticSceneMaterialObservedAlphaBlendCount\": "
       << shadowAgg.semanticSceneMaterialObservedAlphaBlendCount << ",\n";
  json << "    \"semanticSceneRejectedCutoutSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedCutoutSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedCutoutGeometry\": "
       << shadowAgg.semanticSceneRejectedCutoutGeometry << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendGeometry\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendGeometry << ",\n";
  json << "    \"semanticSceneRejectedCutoutVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedCutoutVisualPolicy << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendVisualPolicy << ",\n";
  json << "    \"semanticSceneMaterialLayerContractResolvedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractResolvedCount << ",\n";
  json << "    \"semanticSceneMaterialLayerContractFailedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractFailedCount << ",\n";
  json << "    \"semanticSceneMaterialBlendMode0Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode0Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode1Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode1Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode2PlusCount\": "
       << shadowAgg.semanticSceneMaterialBlendMode2PlusCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseAvailableCount\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseAvailableCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsSceneReadSuccessCount\": "
       << shadowAgg.semanticSceneFastAppendBoundsSceneReadSuccessCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe1Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe1Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe4Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe4Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe16Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe16Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaGt16Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaGt16Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaMaxMilli\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaMaxMilli << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshAttemptCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshAttemptCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshHitCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshHitCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshMissCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshMissCount << ",\n";
  json << "    \"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount\": "
       << shadowAgg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount << ",\n";
  json << "    \"semanticScenePaletteOverrideNoComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideNoComposeCount << ",\n";
  json << "    \"semanticScenePaletteOverrideWouldComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideWouldComposeCount << ",\n";
  json << "    \"semanticScenePalettePacketWorldComposeCount\": "
       << shadowAgg.semanticScenePalettePacketWorldComposeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupStableCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseAttemptCount\": "
       << shadowAgg.semanticSceneDrawTimePoseAttemptCount << ",\n";
  json << "    \"semanticSceneDrawTimePosePublishedCount\": "
       << shadowAgg.semanticSceneDrawTimePosePublishedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectUiOrEffectCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectUiOrEffectCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectVertexShaderCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectVertexShaderCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoVertexBlendCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoVertexBlendCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoContextCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoContextCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoRuntimeModelCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseDedupedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseDedupedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseChangedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseChangedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseStableCount\": "
       << shadowAgg.semanticSceneDrawTimePoseStableCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionChangedCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionChangedCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionStableCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionStableCount << ",\n";
  json << "    \"semanticSceneSkinnedDynamicIndexSliceCount\": "
       << shadowAgg.semanticSceneSkinnedDynamicIndexSliceCount << ",\n";
  json << "    \"semanticSceneSubmittedOwnedGroupSlots\": "
       << shadowAgg.semanticSceneSubmittedOwnedGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawContractKnownCount\": "
       << shadowAgg.semanticSceneCurrentDrawContractKnownCount << ",\n";
  json << "    \"semanticSceneCurrentDrawPaletteReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawPaletteReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawGroupSlotReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawGroupSlotReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoContract\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoContract << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoPalette\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoPalette << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoGroupSlots\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawMissStaleVisibleFrame\": "
       << shadowAgg.semanticSceneCurrentDrawMissStaleVisibleFrame << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyRejectedCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyRejectedCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCutoutCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCutoutCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyAlphaBlendCount\": "
       << shadowAgg.semanticSceneCanonicalReadyAlphaBlendCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCutoutCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCutoutCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyAlphaBlendCount\": "
       << shadowAgg.semanticSceneCanonicalReadyAlphaBlendCount << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoStableIdentity\": "
       << shadowAgg.semanticSceneCanonicalRejectNoStableIdentity << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoMesh\": "
       << shadowAgg.semanticSceneCanonicalRejectNoMesh << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoWorldTransform\": "
       << shadowAgg.semanticSceneCanonicalRejectNoWorldTransform << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoPalette\": "
       << shadowAgg.semanticSceneCanonicalRejectNoPalette << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoSlotContract\": "
       << shadowAgg.semanticSceneCanonicalRejectNoSlotContract << ",\n";
  json << "    \"semanticSceneCanonicalRejectStaleProducer\": "
       << shadowAgg.semanticSceneCanonicalRejectStaleProducer << ",\n";
  json << "    \"semanticSceneCanonicalRejectInvalidVertexIndex\": "
       << shadowAgg.semanticSceneCanonicalRejectInvalidVertexIndex << ",\n";
  json << "    \"semanticSceneCanonicalRejectExplicitBlendIncomplete\": "
       << shadowAgg.semanticSceneCanonicalRejectExplicitBlendIncomplete << ",\n";
  json << "    \"semanticSceneCanonicalRejectAfterReadyCount\": "
       << shadowAgg.semanticSceneCanonicalRejectAfterReadyCount << ",\n";
  json << "    \"semanticSceneSubmittedExplicitBlendContract\": "
       << shadowAgg.semanticSceneSubmittedExplicitBlendContract << ",\n";
  json << "    \"semanticSceneSubmittedSingleMatrixGroupSkinning\": "
       << shadowAgg.semanticSceneSubmittedSingleMatrixGroupSkinning << ",\n";
  json << "    \"semanticSceneSubmittedMultiGroupSlotSkinning\": "
       << shadowAgg.semanticSceneSubmittedMultiGroupSlotSkinning << ",\n";
  json << "    \"semanticSceneSkinnedMinUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMinUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedMaxUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMaxUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique1Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique1Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique2To4Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique2To4Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique5To8Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique5To8Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique9To16Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique9To16Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique17PlusCount\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique17PlusCount << ",\n";
  json << "    \"semanticSceneExplicitBlendUnavailableCurrentDraw\": "
       << shadowAgg.semanticSceneExplicitBlendUnavailableCurrentDraw << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackCount\": "
       << shadowAgg.semanticSceneSkinnedFullIndexFallbackCount << ",\n";
  json << "    \"semanticSceneSkinnedMissingVisibleIndexSliceRejectCount\": "
       << shadowAgg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount
       << ",\n";
  json << "    \"semanticSceneSubmittedFrameLocal\": "
       << shadowAgg.semanticSceneSubmittedFrameLocal << ",\n";
  json << "    \"semanticSceneSubmittedPersistent\": "
       << shadowAgg.semanticSceneSubmittedPersistent << ",\n";
  json << "    \"semanticScenePopulateAttemptCount\": "
       << shadowAgg.semanticScenePopulateAttemptCount << ",\n";
  json << "    \"semanticScenePopulateUnitsOnlyCount\": "
       << shadowAgg.semanticScenePopulateUnitsOnlyCount << ",\n";
  json << "    \"semanticScenePopulateLastReturnReason\": "
       << shadowAgg.semanticScenePopulateLastReturnReason << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishReadyDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishReadyDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryHitDelta << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeHitDelta
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawRecordCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawRecordCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltPacketCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount
       << ",\n";
  json << "    \"drawTimeSemanticProducerVisibleCandidateCount\": "
       << shadowAgg.drawTimeSemanticProducerVisibleCandidateCount << ",\n";
  json << "    \"drawTimeSemanticProducerFreshEntryCount\": "
       << shadowAgg.drawTimeSemanticProducerFreshEntryCount << ",\n";
  json << "    \"drawTimeSemanticProducerClaimedCount\": "
       << shadowAgg.drawTimeSemanticProducerClaimedCount << ",\n";
  json << "    \"drawTimeSemanticProducerSubmittedCount\": "
       << shadowAgg.drawTimeSemanticProducerSubmittedCount << ",\n";
  json << "    \"drawTimeSemanticProducerMissNoFreshEntryCount\": "
       << shadowAgg.drawTimeSemanticProducerMissNoFreshEntryCount << ",\n";
  json << "    \"drawTimeSemanticProducerFallbackCurrentDrawCount\": "
       << shadowAgg.drawTimeSemanticProducerFallbackCurrentDrawCount << ",\n";
  json << "    \"drawTimeSemanticProducerOwnedDirectGroupedSkipCount\": "
       << shadowAgg.drawTimeSemanticProducerOwnedDirectGroupedSkipCount
       << ",\n";
  json << "    \"drawTimeSemanticProducerLifecycleMergedCount\": "
       << shadowAgg.drawTimeSemanticProducerLifecycleMergedCount << ",\n";
  json << "    \"semanticSceneProducerClaimObserveMode\": "
       << shadowAgg.semanticSceneProducerClaimObserveMode << ",\n";
  json << "    \"semanticSceneProducerClaimExactKeyCount\": "
       << shadowAgg.semanticSceneProducerClaimExactKeyCount << ",\n";
  json << "    \"semanticSceneProducerClaimCandidateCount\": "
       << shadowAgg.semanticSceneProducerClaimCandidateCount << ",\n";
  json << "    \"semanticSceneProducerClaimCanonicalOwnedCount\": "
       << shadowAgg.semanticSceneProducerClaimCanonicalOwnedCount << ",\n";
  json << "    \"semanticSceneProducerClaimMissingKeyCount\": "
       << shadowAgg.semanticSceneProducerClaimMissingKeyCount << ",\n";
  json << "    \"semanticSceneProducerClaimUnresolvedCount\": "
       << shadowAgg.semanticSceneProducerClaimUnresolvedCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictPredictedCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictPredictedCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictMatchCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictMatchCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictFalsePositiveCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictFalsePositiveCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimStrictFalseNegativeCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictFalseNegativeCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalPredictedCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalPredictedCount << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalMatchCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalMatchCount << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalFalsePositiveCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalFalsePositiveCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalFalseNegativeCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalFalseNegativeCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimConsumeDeniedCount\": "
       << shadowAgg.semanticSceneProducerClaimConsumeDeniedCount << ",\n";

  // Phase 7.2: flicker diagnostics + reconciliation
  json << "    \"semanticSceneDirectLastRawRecordCount\": "
       << shadowAgg.semanticSceneDirectLastRawRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastEligibleRecordCount\": "
       << shadowAgg.semanticSceneDirectLastEligibleRecordCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableMode\": "
       << shadowAgg.semanticSceneCompactWorkTableMode << ",\n";
  json << "    \"semanticSceneCompactWorkTableCandidateCount\": "
       << shadowAgg.semanticSceneCompactWorkTableCandidateCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableSealedCount\": "
       << shadowAgg.semanticSceneCompactWorkTableSealedCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableConsumedCount\": "
       << shadowAgg.semanticSceneCompactWorkTableConsumedCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableFallbackCount\": "
       << shadowAgg.semanticSceneCompactWorkTableFallbackCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectStageCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectStageCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectFreshnessCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectFreshnessCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectPolicyCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectPolicyCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectFrameCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectFrameCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectIdentityCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectIdentityCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableMismatchCount\": "
       << shadowAgg.semanticSceneCompactWorkTableMismatchCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedRecordCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastUniqueObjectCount\": "
       << shadowAgg.semanticSceneDirectLastUniqueObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedObjectCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastRecordCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastRecordCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastScanCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastScanCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastMinGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMinGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastMaxGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMaxGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedIdentityHash\": "
       << shadowAgg.semanticSceneDirectLastSubmittedIdentityHash << ",\n";
  json << "    \"semanticSceneDirectIdentityChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentityChurnCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapHitCount\": "
       << shadowAgg.semanticSceneDirectRecordCapHitCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapTruncatedRecordCount\": "
       << shadowAgg.semanticSceneDirectRecordCapTruncatedRecordCount << ",\n";
  json << "    \"semanticSceneDirectScanCapHitCount\": "
       << shadowAgg.semanticSceneDirectScanCapHitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSubmitCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSubmitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSkipCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSkipCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapSkipObjectCount\": "
       << shadowAgg.semanticSceneDirectRecordCapSkipObjectCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapAppendFailCount\": "
       << shadowAgg.semanticSceneDirectRecordCapAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseActiveKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseActiveKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeasePrunedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeasePrunedKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseSubmittedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseSubmittedKeyCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillBudgetRecordCount\": "
       << shadowAgg.semanticSceneDirectStickyFillBudgetRecordCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillAppendedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillAppendedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillSubmittedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillSubmittedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillMissedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillMissedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRestoredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseUpdatedCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseUpdatedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseExpiredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedDynamicMeshCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseBudgetLimitCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseExpiredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseBudgetLimitCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreCompleteCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreIncompleteSkipCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartOmittedIncompleteCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount
       << ",\n";
  // Phase 7.25 core epoch planner 专属计数器。
  json << "    \"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochMissingPartCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochMissingPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount\": "
       << shadowAgg
              .semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount\": "
       << shadowAgg
              .semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseExpiredBackingOnlyCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount\": "
       << shadowAgg
              .semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestMissingRequiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestMissingRequiredPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestGraceUsedCount\": "
       << shadowAgg.semanticSceneShadowManifestGraceUsedCount << ",\n";
  json << "    \"semanticSceneShadowManifestTombstoneRetiredCount\": "
       << shadowAgg.semanticSceneShadowManifestTombstoneRetiredCount
       << ",\n";
  json << "    \"shadowMetadataClassifiedCount\": "
       << shadowMetadataSnapshot.metadataClassifiedCount << ",\n";
  json << "    \"shadowMetadataCapturedCount\": "
       << shadowMetadataSnapshot.metadataCapturedCount << ",\n";
  json << "    \"shadowMetadataAppliedCount\": "
       << shadowMetadataSnapshot.metadataAppliedCount << ",\n";
  json << "    \"shadowMetadataRejectedFrameCount\": "
       << shadowMetadataSnapshot.metadataRejectedFrameCount << ",\n";
  json << "    \"shadowMetadataRejectedGenerationCount\": "
       << shadowMetadataSnapshot.metadataRejectedGenerationCount << ",\n";
  json << "    \"shadowMetadataAmbiguousCount\": "
       << shadowMetadataSnapshot.metadataAmbiguousCount << ",\n";
  json << "    \"shadowMetadataRejectedNoMaterialCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoMaterialCount << ",\n";
  json << "    \"shadowMetadataRejectedOpaqueCount\": "
       << shadowMetadataSnapshot.metadataRejectedOpaqueCount << ",\n";
  json << "    \"shadowMetadataRejectedNoUvCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoUvCount << ",\n";
  json << "    \"shadowMetadataRejectedNoDiffuseCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoDiffuseCount << ",\n";
  json << "    \"shadowMetadataRejectedUploadCount\": "
       << shadowMetadataSnapshot.metadataRejectedUploadCount << ",\n";
  json << "    \"shadowMetadataRejectedDuplicateCount\": "
       << shadowMetadataSnapshot.metadataRejectedDuplicateCount << ",\n";
  json << "    \"shadowMetadataRejectedByReasonCount\": "
       << shadowMetadataRejectedByReasonCount << ",\n";
  json << "    \"shadowMetadataBlockerKnownRawcodeCount\": "
       << shadowMetadataSnapshot.blockerKnownRawcodeCount << ",\n";
  json << "    \"shadowMetadataBlockerWidgetIdentityCount\": "
       << shadowMetadataSnapshot.blockerWidgetIdentityCount << ",\n";
  json << "    \"shadowMetadataBlockerSmallFlatCount\": "
       << shadowMetadataSnapshot.blockerSmallFlatCount << ",\n";
  json << "    \"shadowMetadataBlockerBelowGroundCount\": "
       << shadowMetadataSnapshot.blockerBelowGroundCount << ",\n";
  json << "    \"shadowMetadataBlockerUnreadableCount\": "
       << shadowMetadataSnapshot.blockerUnreadableCount << ",\n";
  json << "    \"shadowMetadataBlockerFinalLeakCount\": "
       << shadowMetadataSnapshot.blockerFinalLeakCount << ",\n";
  // Phase 7.28：skinned palette content stability probe。
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceNoneCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteCountChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteAttributionSnapshotHitCount\": "
       << shadowAgg
              .semanticSceneDirectPaletteAttributionSnapshotHitCount
       << ",\n";
  json << "    \"semanticSceneDirectManifestObjectCount\": "
       << shadowAgg.semanticSceneDirectManifestObjectCount << ",\n";
  json << "    \"semanticSceneDirectManifestObservedPartCount\": "
       << shadowAgg.semanticSceneDirectManifestObservedPartCount << ",\n";
  json << "    \"semanticSceneDirectManifestShadowEligiblePartCount\": "
       << shadowAgg.semanticSceneDirectManifestShadowEligiblePartCount << ",\n";
  json << "    \"semanticSceneDirectObjectCompleteEligibleCount\": "
       << shadowAgg.semanticSceneDirectObjectCompleteEligibleCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByScanCapCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByScanCapCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAlphaPolicyCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByPacketBuildFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAppendFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedCompleteObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedCompleteObjectCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceAuthoritativeCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceAuthoritativeCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceFallbackLayerIndexCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceMissingCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceMissingCount << ",\n";
  json << "    \"semanticScenePreparedProbeAttemptCount\": "
       << shadowAgg.semanticScenePreparedProbeAttemptCount << ",\n";
  json << "    \"semanticScenePreparedProbeContextReadyCount\": "
       << shadowAgg.semanticScenePreparedProbeContextReadyCount << ",\n";
  json << "    \"semanticScenePreparedProbeBackingReadableCount\": "
       << shadowAgg.semanticScenePreparedProbeBackingReadableCount << ",\n";
  json << "    \"semanticScenePreparedSliceRecordedCount\": "
       << shadowAgg.semanticScenePreparedSliceRecordedCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryAttemptCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryAttemptCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryHitCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryHitCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryMissCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartCount\": "
       << shadowAgg.semanticSceneShadowManifestPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestStableObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestStableObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestNewObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestNewObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestFreshPartCount\": "
       << shadowAgg.semanticSceneShadowManifestFreshPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseablePartCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseablePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPoseStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestPoseStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestSliceStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestSliceStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestMultiSlicePartCount\": "
       << shadowAgg.semanticSceneShadowManifestMultiSlicePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPayload11CChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestPayload11CChurnCount << ",\n";
  json << "    \"semanticSceneShadowManifestRenderablePartChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestRenderablePartChurnCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseHitCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseHitCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseMissCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseNoRuntimeCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseNoRuntimeCount
       << ",\n";
  json << "    "
          "\"semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixHash\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneSubmittedObjectJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedObjectJaccardMilli << ",\n";
  json << "    \"semanticSceneSubmittedPartJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedPartJaccardMilli << ",\n";
  json << "    \"semanticSceneVisibleLookupPartLayerHitCount\": "
       << shadowAgg.semanticSceneVisibleLookupPartLayerHitCount << ",\n";
  json << "    \"semanticSceneVisibleLookupSingleFallbackCount\": "
       << shadowAgg.semanticSceneVisibleLookupSingleFallbackCount << ",\n";
  json << "    \"semanticSceneVisibleLookupMissCount\": "
       << shadowAgg.semanticSceneVisibleLookupMissCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingNotCheckedCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingNotCheckedCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingPassCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingPassCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailLookupMissCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailLookupMissCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonMainQueueCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneDirectPaletteHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStableGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectStableGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStream1PtrChurnCount\": "
       << shadowAgg.semanticSceneDirectStream1PtrChurnCount << ",\n";
  json << "    \"semanticSceneDirectGeometrySourceHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGeometrySourceHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectSameCasterComparisonCount\": "
       << shadowAgg.semanticSceneDirectSameCasterComparisonCount << ",\n";
  json << "    \"semanticSceneDirectIdentitySkippedChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentitySkippedChurnCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootDeltaSampleCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootDeltaSampleCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootMaxDeltaMilli\": "
       << shadowAgg.semanticSceneDirectPaletteRootMaxDeltaMilli << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyUnitPtrCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyUnitPtrCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyJHandleCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyJHandleCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRuntimeModelCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyWorldObjectCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyWorldObjectCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeySceneNodeCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeySceneNodeCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyModelMeshCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyModelMeshCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRenderablePartCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneLastAppendedGeometrySourceHash\": "
       << shadowAgg.semanticSceneLastAppendedGeometrySourceHash << ",\n";
  json << "    \"semanticSceneLastAppendedGeometryId\": "
       << shadowAgg.semanticSceneLastAppendedGeometryId << ",\n";
  json << "    \"semanticSceneShadowCastersCount\": "
       << shadowAgg.semanticSceneShadowCastersCount << ",\n";
  json << "    \"semanticSceneReplayDrawsCount\": "
       << shadowAgg.semanticSceneReplayDrawsCount << ",\n";
  json << "    \"semanticSceneShadowMapDrawnCasters\": "
       << shadowAgg.semanticSceneShadowMapDrawnCasters << ",\n";
  json << "    \"semanticSceneShadowMapCascadeCulledCount\": "
       << shadowAgg.semanticSceneShadowMapCascadeCulledCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsCullMode\": "
       << shadowAgg.semanticSceneTerrainBoundsCullMode << ",\n";
  json << "    \"semanticSceneTerrainBoundsCandidateCount\": "
       << shadowAgg.semanticSceneTerrainBoundsCandidateCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsProofAcceptedCount\": "
       << shadowAgg.semanticSceneTerrainBoundsProofAcceptedCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsFailVisibleCount\": "
       << shadowAgg.semanticSceneTerrainBoundsFailVisibleCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsWouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsWouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsAppliedCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsAppliedCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC0WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC0WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC1WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC1WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC2WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC2WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC3WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC3WouldCullCount << ",\n";
  json << "    \"semanticSceneObjectBoundsCandidateCount\": "
       << shadowAgg.semanticSceneObjectBoundsCandidateCount << ",\n";
  json << "    \"semanticSceneObjectBoundsProofAcceptedCount\": "
       << shadowAgg.semanticSceneObjectBoundsProofAcceptedCount << ",\n";
  json << "    \"semanticSceneObjectBoundsFailVisibleCount\": "
       << shadowAgg.semanticSceneObjectBoundsFailVisibleCount << ",\n";
  json << "    \"semanticSceneObjectBoundsWouldCullCount\": "
       << shadowAgg.semanticSceneObjectBoundsWouldCullCount << ",\n";
  json << "    \"semanticSceneObjectBoundsAppliedCullCount\": "
       << shadowAgg.semanticSceneObjectBoundsAppliedCullCount << ",\n";
  json << "    \"semanticSceneUnionCullMode\": "
       << shadowAgg.semanticSceneUnionCullMode << ",\n";
  json << "    \"semanticSceneUnionCullObserveFrameCount\": "
       << shadowAgg.semanticSceneUnionCullObserveFrameCount << ",\n";
  json << "    \"semanticSceneUnionCullCandidateCount\": "
       << shadowAgg.semanticSceneUnionCullCandidateCount << ",\n";
  json << "    \"semanticSceneUnionCullProofAcceptedCount\": "
       << shadowAgg.semanticSceneUnionCullProofAcceptedCount << ",\n";
  json << "    \"semanticSceneUnionCullFailVisibleCount\": "
       << shadowAgg.semanticSceneUnionCullFailVisibleCount << ",\n";
  json << "    \"semanticSceneUnionCullDynamicConservativeCount\": "
       << shadowAgg.semanticSceneUnionCullDynamicConservativeCount << ",\n";
  json << "    \"semanticSceneUnionCullUnknownOrStaleCount\": "
       << shadowAgg.semanticSceneUnionCullUnknownOrStaleCount << ",\n";
  json << "    \"semanticSceneUnionCullC2WouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullC2WouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullC3WouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullC3WouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullBothFarWouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullBothFarWouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullFalseNegativeCount\": "
       << shadowAgg.semanticSceneUnionCullFalseNegativeCount << ",\n";
  json << "    \"semanticSceneUnionCullFalsePositiveCount\": "
       << shadowAgg.semanticSceneUnionCullFalsePositiveCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedCasterCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedCasterCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedPreparedCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedPreparedCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidBufferCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidBufferCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidPipelineCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidPipelineCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedDrawnCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedDrawnCount << ",\n";
  json << "    \"semanticSceneShadowTaaActive\": "
       << shadowAgg.semanticSceneShadowTaaActive << ",\n";
  json << "    \"pointShadowPersistentFramesObserved\": "
       << shadowAgg.pointShadowPersistentFramesObserved << ",\n";
  json << "    \"pointShadowPersistentConfiguredModeLast\": "
       << shadowAgg.pointShadowPersistentLast.configuredMode << ",\n";
  json << "    \"pointShadowPersistentEffectiveModeLast\": "
       << shadowAgg.pointShadowPersistentLast.effectiveMode << ",\n";
  json << "    \"pointShadowPersistentLastBeginRejectReason\": "
       << shadowAgg.pointShadowPersistentLast.lastBeginRejectReason << ",\n";
  json << "    \"pointShadowPersistentWorkerCreatedLast\": "
       << shadowAgg.pointShadowPersistentLast.workerCreated << ",\n";
  json << "    \"pointShadowPersistentWorkerAvailableLast\": "
       << shadowAgg.pointShadowPersistentLast.workerAvailable << ",\n";
  json << "    \"pointShadowPersistentBeginAttemptsLast\": "
       << shadowAgg.pointShadowPersistentLast.beginAttempts << ",\n";
  json << "    \"pointShadowPersistentBeginEligibleLast\": "
       << shadowAgg.pointShadowPersistentLast.beginEligible << ",\n";
  json << "    \"pointShadowPersistentWorkerCreateCountLast\": "
       << shadowAgg.pointShadowPersistentLast.workerCreateCount << ",\n";
  json << "    \"pointShadowPersistentWorkerThreadStartsLast\": "
       << shadowAgg.pointShadowPersistentLast.workerThreadStarts << ",\n";
  json << "    \"pointShadowPersistentAcceptedLast\": "
       << shadowAgg.pointShadowPersistentLast.accepted << ",\n";
  json << "    \"pointShadowPersistentReadyLast\": "
       << shadowAgg.pointShadowPersistentLast.ready << ",\n";
  json << "    \"pointShadowPersistentDeadlineFallbackLast\": "
       << shadowAgg.pointShadowPersistentLast.deadlineFallback << ",\n";
  json << "    \"pointShadowPersistentRejectedFallbackLast\": "
       << shadowAgg.pointShadowPersistentLast.rejectedFallback << ",\n";
  json << "    \"pointShadowPersistentObserveMatchLast\": "
       << shadowAgg.pointShadowPersistentLast.observeMatch << ",\n";
  json << "    \"pointShadowPersistentMismatchLast\": "
       << shadowAgg.pointShadowPersistentLast.mismatch << ",\n";
  json << "    \"pointShadowPersistentConsumedLast\": "
       << shadowAgg.pointShadowPersistentLast.consumed << ",\n";
  json << "    \"pointShadowPersistentFailedLast\": "
       << shadowAgg.pointShadowPersistentLast.failed << ",\n";
  json << "    \"pointShadowPersistentBusyLast\": "
       << shadowAgg.pointShadowPersistentLast.busy << ",\n";
  json << "    \"shadowTaaFramesObserved\": "
       << shadowAgg.shadowTaaFramesObserved << ",\n";
  json << "    \"shadowTaaRuntimeModuleDisabledFrames\": "
       << shadowAgg.shadowTaaRuntimeModuleDisabledFrames << ",\n";
  json << "    \"shadowTaaRequestedDirectFrames\": "
       << shadowAgg.shadowTaaRequestedDirectFrames << ",\n";
  json << "    \"shadowTaaRequestedPrepassFrames\": "
       << shadowAgg.shadowTaaRequestedPrepassFrames << ",\n";
  json << "    \"shadowTaaRequestedTemporalFrames\": "
       << shadowAgg.shadowTaaRequestedTemporalFrames << ",\n";
  json << "    \"shadowTaaEffectiveDirectFrames\": "
       << shadowAgg.shadowTaaEffectiveDirectFrames << ",\n";
  json << "    \"shadowTaaEffectivePrepassFrames\": "
       << shadowAgg.shadowTaaEffectivePrepassFrames << ",\n";
  json << "    \"shadowTaaEffectiveTemporalFrames\": "
       << shadowAgg.shadowTaaEffectiveTemporalFrames << ",\n";
  json << "    \"shadowTaaBlockedSemanticDynamicFrames\": "
       << shadowAgg.shadowTaaBlockedSemanticDynamicFrames << ",\n";
  json << "    \"shadowTaaBlockedSunMotionFrames\": "
       << shadowAgg.shadowTaaBlockedSunMotionFrames << ",\n";
  json << "    \"shadowTaaBlockedCsmFallbackFrames\": "
       << shadowAgg.shadowTaaBlockedCsmFallbackFrames << ",\n";
  json << "    \"shadowTaaVisibilityExecutedFrames\": "
       << shadowAgg.shadowTaaVisibilityExecutedFrames << ",\n";
  json << "    \"shadowTaaMotionVectorExecutedFrames\": "
       << shadowAgg.shadowTaaMotionVectorExecutedFrames << ",\n";
  json << "    \"shadowTaaReceiverExecutedFrames\": "
       << shadowAgg.shadowTaaReceiverExecutedFrames << ",\n";
  json << "    \"shadowTaaHistoryWriteExecutedFrames\": "
       << shadowAgg.shadowTaaHistoryWriteExecutedFrames << ",\n";
  json << "    \"shadowTaaHistoryAdvancedFrames\": "
       << shadowAgg.shadowTaaHistoryAdvancedFrames << ",\n";
  json << "    \"shadowTaaHistoryAdvanceSkippedIncompleteFrames\": "
       << shadowAgg.shadowTaaHistoryAdvanceSkippedIncompleteFrames << ",\n";
  json << "    \"shadowTaaHistoryValidBeforeFrames\": "
       << shadowAgg.shadowTaaHistoryValidBeforeFrames << ",\n";
  json << "    \"shadowTaaHistoryValidAfterFrames\": "
       << shadowAgg.shadowTaaHistoryValidAfterFrames << ",\n";
  json << "    \"shadowTaaHistoryInvalidatedFrames\": "
       << shadowAgg.shadowTaaHistoryInvalidatedFrames << ",\n";
  json << "    \"shadowTaaHistoryInvalidationMaskLast\": "
       << shadowAgg.shadowTaaHistoryInvalidationMaskLast << ",\n";
  json << "    \"shadowTaaHistoryInvalidationReasonFrames\": [";
  for (size_t i = 0u;
       i < shadowAgg.shadowTaaHistoryInvalidationReasonFrames.size(); ++i) {
    if (i != 0u)
      json << ",";
    json << shadowAgg.shadowTaaHistoryInvalidationReasonFrames[i];
  }
  json << "],\n";
  json << "    \"stage13CaptureAttemptCount\": "
       << shadowAgg.stage13CaptureAttemptCount << ",\n";
  json << "    \"stage13CaptureRejectedNoDemandCount\": "
       << shadowAgg.stage13CaptureRejectedNoDemandCount << ",\n";
  json << "    \"stage13CaptureRejectedAfterBeforeUiCount\": "
       << shadowAgg.stage13CaptureRejectedAfterBeforeUiCount << ",\n";
  json << "    \"stage13CaptureConsideredCount\": "
       << shadowAgg.stage13CaptureConsideredCount << ",\n";
  json << "    \"stage13FreezeCopyBytes\": "
       << shadowAgg.stage13FreezeCopyBytes << ",\n";
  json << "    \"stage13CpuSnapshotCopyBytes\": "
       << shadowAgg.stage13CpuSnapshotCopyBytes << ",\n";
  json << "    \"stage13RetentionSnapshotBytes\": "
       << shadowAgg.stage13RetentionSnapshotBytes << ",\n";
  json << "    \"stage13ReplayDrawCount\": "
       << shadowAgg.stage13ReplayDrawCount << ",\n";
  json << "    \"shadowTaaRequestedModeLast\": "
       << shadowAgg.shadowTaaRequestedModeLast << ",\n";
  json << "    \"shadowTaaEffectiveModeLast\": "
       << shadowAgg.shadowTaaEffectiveModeLast << ",\n";
  json << "    \"shadowTaaShaderModeLast\": "
       << shadowAgg.shadowTaaShaderModeLast << ",\n";
  json << "    \"semanticSceneReceiverReuseShadowMap\": "
       << shadowAgg.semanticSceneReceiverReuseShadowMap << ",\n";
  json << "    \"semanticSceneReceiverInputValid\": "
       << shadowAgg.semanticSceneReceiverInputValid << ",\n";
  json << "    \"semanticSceneReceiverInputRejectReason\": "
       << shadowAgg.semanticSceneReceiverInputRejectReason << ",\n";
  json << "    \"semanticSceneReceiverNeedPass\": "
       << shadowAgg.semanticSceneReceiverNeedPass << ",\n";
  json << "    \"semanticSceneReceiverNeedShadowMap\": "
       << shadowAgg.semanticSceneReceiverNeedShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasCompleteShadowMap\": "
       << shadowAgg.semanticSceneReceiverHasCompleteShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasUsableDirectionalShadow\": "
       << shadowAgg.semanticSceneReceiverHasUsableDirectionalShadow << ",\n";
  json << "    \"semanticSceneReceiverActiveStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverActiveStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverUboStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverUboStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverDebugMode\": "
       << shadowAgg.semanticSceneReceiverDebugMode << ",\n";
  json << "    \"semanticSceneReceiverCsmCascadeCount\": "
       << shadowAgg.semanticSceneReceiverCsmCascadeCount << ",\n";
  json << "    \"semanticSceneReceiverZeroStrengthFrameCount\": "
       << shadowAgg.semanticSceneReceiverZeroStrengthFrameCount << ",\n";
  json << "    \"semanticSceneReceiverDrawnWithZeroStrengthCount\": "
       << shadowAgg.semanticSceneReceiverDrawnWithZeroStrengthCount << ",\n";
  json << "    \"semanticSceneReceiverNoCompleteShadowMapCount\": "
       << shadowAgg.semanticSceneReceiverNoCompleteShadowMapCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapImageCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapImageCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapSampleViewCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapSampleViewCount << ",\n";
  json << "    \"semanticSceneReceiverNoCandidateCsmCount\": "
       << shadowAgg.semanticSceneReceiverNoCandidateCsmCount << ",\n";
  json << "    \"semanticSceneReceiverCsmFallbackToLastGoodCount\": "
       << shadowAgg.semanticSceneReceiverCsmFallbackToLastGoodCount << ",\n";
  json << "    \"semanticSceneReceiverHoldInvalidCsmCount\": "
       << shadowAgg.semanticSceneReceiverHoldInvalidCsmCount << ",\n";
  json << "    \"semanticSceneReceiverHoldEmptyReplayCount\": "
       << shadowAgg.semanticSceneReceiverHoldEmptyReplayCount << ",\n";
  json << "    \"semanticSceneReceiverHoldIdentityChurnCount\": "
       << shadowAgg.semanticSceneReceiverHoldIdentityChurnCount << ",\n";
  json << "    \"semanticSceneReceiverReuseInvalidatedAfterEnsureCount\": "
       << shadowAgg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoResourcesCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoResourcesCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount
       << ",\n";
  json << "    \"semanticSceneReceiverViewportX\": "
       << shadowAgg.semanticSceneReceiverViewportX << ",\n";
  json << "    \"semanticSceneReceiverViewportY\": "
       << shadowAgg.semanticSceneReceiverViewportY << ",\n";
  json << "    \"semanticSceneReceiverViewportWidth\": "
       << shadowAgg.semanticSceneReceiverViewportWidth << ",\n";
  json << "    \"semanticSceneReceiverViewportHeight\": "
       << shadowAgg.semanticSceneReceiverViewportHeight << ",\n";
  json << "    \"semanticSceneSkippedUnitsOnlyFilter\": "
       << shadowAgg.semanticSceneSkippedUnitsOnlyFilter << ",\n";
  json << "    \"semanticSceneAcceptedExplicitResourceOwnerRigid\": "
       << shadowAgg.semanticSceneAcceptedExplicitResourceOwnerRigid << ",\n";
  json << "    \"semanticSceneRejectedNoVertex\": "
       << shadowAgg.semanticSceneRejectedNoVertex << ",\n";
  json << "    \"semanticSceneRejectedSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedGeometry\": "
       << shadowAgg.semanticSceneRejectedGeometry << ",\n";
  json << "    \"semanticSceneRejectedGeometryFrameLocal\": "
       << shadowAgg.semanticSceneRejectedGeometryFrameLocal << ",\n";
  json << "    \"semanticSceneRejectedGeometryPersistent\": "
       << shadowAgg.semanticSceneRejectedGeometryPersistent << ",\n";
  json << "    \"persistentRejectNoIdentity\": "
       << shadowAgg.persistentRejectNoIdentity << ",\n";
  json << "    \"persistentRejectUnsupportedMode\": "
       << shadowAgg.persistentRejectUnsupportedMode << ",\n";
  json << "    \"persistentRejectDynamicSource\": "
       << shadowAgg.persistentRejectDynamicSource << ",\n";
  json << "    \"persistentRejectAlphaBlend\": "
       << shadowAgg.persistentRejectAlphaBlend << ",\n";
  json << "    \"persistentRejectMissingStorage\": "
       << shadowAgg.persistentRejectMissingStorage << ",\n";
  json << "    \"persistentRejectCreateOrBudget\": "
       << shadowAgg.persistentRejectCreateOrBudget << ",\n";
  json << "    \"persistentRejectCapacity\": "
       << shadowAgg.persistentRejectCapacity << ",\n";
  json << "    \"persistentRejectPositionBufferCreate\": "
       << shadowAgg.persistentRejectPositionBufferCreate << ",\n";
  json << "    \"persistentRejectIndexBufferCreate\": "
       << shadowAgg.persistentRejectIndexBufferCreate << ",\n";
  json << "    \"persistentRejectBlendBufferCreate\": "
       << shadowAgg.persistentRejectBlendBufferCreate << ",\n";
  json << "    \"persistentRejectUvBufferCreate\": "
       << shadowAgg.persistentRejectUvBufferCreate << ",\n";
  json << "    \"persistentRejectRegistryInsert\": "
       << shadowAgg.persistentRejectRegistryInsert << ",\n";
  json << "    \"persistentRejectOther\": "
       << shadowAgg.persistentRejectOther << ",\n";
  json << "    \"persistentRejectCreateOrBudgetDetailedTotal\": "
       << (shadowAgg.persistentRejectCapacity +
           shadowAgg.persistentRejectPositionBufferCreate +
           shadowAgg.persistentRejectIndexBufferCreate +
           shadowAgg.persistentRejectBlendBufferCreate +
           shadowAgg.persistentRejectUvBufferCreate +
           shadowAgg.persistentRejectRegistryInsert +
           shadowAgg.persistentRejectOther)
       << ",\n";
  json << "    \"persistentDiagnosticsFramesObserved\": "
       << shadowAgg.persistentDiagnosticsFramesObserved << ",\n";
  json << "    \"persistentDiagnosticsCounterScope\": "
          "\"recordingWindowAdditive\",\n";
  json << "    \"persistentPoolGaugeScope\": "
          "\"lastObservedPresent\",\n";
  json << "    \"persistentCreateAttempts\": "
       << shadowAgg.persistentCreateAttempts << ",\n";
  json << "    \"persistentPoolBytesCap\": "
       << shadowAgg.persistentPoolBytesCapLast << ",\n";
  json << "    \"persistentPoolBytesUsed\": "
       << shadowAgg.persistentPoolBytesUsedLast << ",\n";
  json << "    \"persistentPoolBytesUsedMax\": "
       << shadowAgg.persistentPoolBytesUsedMax << ",\n";
  json << "    \"persistentPoolBytesUsedAverage\": "
       << (shadowAgg.persistentDiagnosticsFramesObserved != 0u
               ? static_cast<double>(
                     shadowAgg.persistentPoolBytesUsedGaugeSum) /
                     static_cast<double>(
                         shadowAgg.persistentDiagnosticsFramesObserved)
               : 0.0)
       << ",\n";
  json << "    \"persistentPoolBytesNeeded\": "
       << shadowAgg.persistentPoolBytesNeededLast << ",\n";
  json << "    \"persistentPoolBytesNeededMax\": "
       << shadowAgg.persistentPoolBytesNeededMax << ",\n";
  json << "    \"persistentPoolBytesNeededTotal\": "
       << shadowAgg.persistentPoolBytesNeededTotal << ",\n";
  json << "    \"persistentPoolBytesEvicted\": "
       << shadowAgg.persistentPoolBytesEvictedLast << ",\n";
  json << "    \"persistentPoolBytesEvictedDelta\": "
       << shadowAgg.persistentPoolBytesEvictedDelta << ",\n";
  json << "    \"persistentPoolLiveGeometryCount\": "
       << shadowAgg.persistentPoolLiveGeometryCountLast << ",\n";
  json << "    \"persistentPoolLiveGeometryCountMax\": "
       << shadowAgg.persistentPoolLiveGeometryCountMax << ",\n";
  json << "    \"persistentForceGcRequests\": "
       << shadowAgg.persistentForceGcRequests << ",\n";
  json << "    \"persistentForceGcNoBytesFreed\": "
       << shadowAgg.persistentForceGcNoBytesFreed << ",\n";
  json << "    \"persistentForceGcStillInsufficient\": "
       << shadowAgg.persistentForceGcStillInsufficient << ",\n";
  json << "    \"persistentForceGcBytesFreed\": "
       << shadowAgg.persistentForceGcBytesFreed << ",\n";
  json << "    \"persistentForceGcContract\": "
          "\"deprecated counters; incoming-allocation forced GC is disabled\",\n";
  json << "    \"persistentCapacityRejectAllCallers\": "
       << shadowAgg.persistentCapacityRejectAllCallers << ",\n";
  json << "    \"persistentCapacityFastReject\": "
       << shadowAgg.persistentCapacityFastReject << ",\n";
  json << "    \"persistentCapacityCounterContract\": "
          "\"rejectCapacity is ShadowCapture-only; capacityRejectAllCallers covers every caller; capacityFastReject is a ShadowCapture precheck subset and must not be added\",\n";
  json << "    \"persistentS1EarlyGaugeContract\": "
          "\"entry ownership gauges; logicalReferencedBytes is not unique or resident memory\",\n";
  json << "    \"persistentS1EarlyEntryCount\": "
       << shadowAgg.persistentS1EarlyEntryCountLast << ",\n";
  json << "    \"persistentS1EarlyEntryCountMax\": "
       << shadowAgg.persistentS1EarlyEntryCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentBackedCount\": "
       << shadowAgg.persistentS1EarlyPersistentBackedCountLast << ",\n";
  json << "    \"persistentS1EarlyPersistentBackedCountMax\": "
       << shadowAgg.persistentS1EarlyPersistentBackedCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexCount\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexCountLast << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexCountMax\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexMismatchFrames\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexMismatchFrames
       << ",\n";
  json << "    \"persistentS1EarlyFallbackBackedCount\": "
       << shadowAgg.persistentS1EarlyFallbackBackedCountLast << ",\n";
  json << "    \"persistentS1EarlyFallbackBackedCountMax\": "
       << shadowAgg.persistentS1EarlyFallbackBackedCountMax << ",\n";
  json << "    \"persistentS1EarlyLogicalReferencedBytes\": "
       << shadowAgg.persistentS1EarlyLogicalReferencedBytesLast << ",\n";
  json << "    \"persistentS1EarlyLogicalReferencedBytesMax\": "
       << shadowAgg.persistentS1EarlyLogicalReferencedBytesMax << ",\n";
  json << "    \"persistentS1EarlyReplayContract\": "
          "\"every accepted early-cache hit publishes exactly one canonical instance or fallback replay\",\n";
  json << "    \"persistentS1EarlyAcceptedHitCount\": "
       << shadowAgg.persistentS1EarlyAcceptedHitCount << ",\n";
  json << "    \"persistentS1EarlySourceMismatchEvictCount\": "
       << shadowAgg.persistentS1EarlySourceMismatchEvictCount << ",\n";
  json << "    \"persistentS1EarlyReplayPublishedCount\": "
       << shadowAgg.persistentS1EarlyReplayPublishedCount << ",\n";
  json << "    \"persistentS1EarlyReplayInstanceCount\": "
       << shadowAgg.persistentS1EarlyReplayInstanceCount << ",\n";
  json << "    \"persistentS1EarlyReplayFallbackCount\": "
       << shadowAgg.persistentS1EarlyReplayFallbackCount << ",\n";
  json << "    \"persistentS1EarlyReplayClosureMismatchFrames\": "
       << shadowAgg.persistentS1EarlyReplayClosureMismatchFrames << ",\n";
  json << "    \"semanticFallbackPruned\": "
       << shadowAgg.semanticFallbackPruned << ",\n";
  json << "    \"semanticFallbackPrunedByHandle\": "
       << shadowAgg.semanticFallbackPrunedByHandle << ",\n";
  json << "    \"semanticFallbackPrunedByWorldObjectEntry\": "
       << shadowAgg.semanticFallbackPrunedByWorldObjectEntry << ",\n";
  json << "    \"semanticFallbackPrunedBySceneNode\": "
       << shadowAgg.semanticFallbackPrunedBySceneNode << ",\n";
  json << "    \"semanticFallbackPrunedByRuntimeModel\": "
       << shadowAgg.semanticFallbackPrunedByRuntimeModel << ",\n";
  json << "    \"instancedGeometryGroups\": "
       << shadowAgg.instancedGeometryGroups << ",\n";
  json << "    \"instancedGeometryInstances\": "
       << shadowAgg.instancedGeometryInstances << ",\n";
  json << "    \"instancedGeometryDrawsSaved\": "
       << shadowAgg.instancedGeometryDrawsSaved << ",\n";
  json << "    \"reusedFreezeMb\": "
       << (static_cast<double>(shadowAgg.reusedFreezeBytes) / (1024.0 * 1024.0))
       << ",\n";
  json << "    \"actualFreezeReuseMb\": "
       << (static_cast<double>(shadowAgg.actualFreezeReuseBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"uniqueFreezeAcceptedMb\": "
       << (static_cast<double>(shadowAgg.uniqueFreezeAcceptedBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"duplicateFreezeBypassMb\": "
       << (static_cast<double>(shadowAgg.duplicateFreezeBypassBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"potentialFreezeReuseMb\": "
       << (static_cast<double>(shadowAgg.potentialFreezeReuseBytes) /
           (1024.0 * 1024.0))
       << ",\n";
  json << "    \"phases\": [\n";
  for (size_t i = 0; i < shadowAgg.phases.size(); ++i) {
    const auto& phase = shadowAgg.phases[i];
    if (i != 0)
      json << ",\n";
    json << "      {\"name\": \"" << phase.name << "\", \"requestedMb\": "
         << (static_cast<double>(phase.requestedBytes) / (1024.0 * 1024.0))
         << ", \"acceptedMb\": "
         << (static_cast<double>(phase.acceptedBytes) / (1024.0 * 1024.0))
         << ", \"rejectedMb\": "
         << (static_cast<double>(phase.rejectedBytes) / (1024.0 * 1024.0))
         << ", \"requests\": " << phase.requests
         << ", \"rejects\": " << phase.rejects << "}";
  }
  json << "\n    ]\n";
  json << "  },\n";

  std::vector<War3PerfMonitor::ShadowBudgetAggregate::PhaseAggregate>
      topShadowOffenders(shadowAgg.phases.begin(), shadowAgg.phases.end());
  std::sort(topShadowOffenders.begin(), topShadowOffenders.end(),
            [](const auto& a, const auto& b) {
              if (a.rejectedBytes != b.rejectedBytes)
                return a.rejectedBytes > b.rejectedBytes;
              return a.requestedBytes > b.requestedBytes;
            });
  json << "  \"topShadowOffenders\": [\n";
  for (size_t i = 0; i < topShadowOffenders.size(); ++i) {
    const auto& offender = topShadowOffenders[i];
    if (i != 0)
      json << ",\n";
    json << "    {\"name\": \"" << offender.name << "\", \"requestedMb\": "
         << (static_cast<double>(offender.requestedBytes) / (1024.0 * 1024.0))
         << ", \"rejectedMb\": "
         << (static_cast<double>(offender.rejectedBytes) / (1024.0 * 1024.0))
         << ", \"rejects\": " << offender.rejects << "}";
  }
  json << "\n  ],\n";

  json << "  \"shadowRuntimeV2Summary\": {\n";
  const auto runtimeSummary =
      dxvk::war3::render::QueryShadowRuntimeBridgeSummary();
  const auto semanticAugmentCache =
      dxvk::war3::render::QuerySemanticAugmentTlsCacheStats();
  json << "    \"staticPersistentCount\": "
       << shadowAgg.staticPersistentCount << ",\n";
  json << "    \"dynamicPoseCount\": " << shadowAgg.dynamicPoseCount
       << ",\n";
  json << "    \"dynamicSkinnedOutputCount\": "
       << shadowAgg.dynamicSkinnedOutputCount << ",\n";
  json << "    \"fallbackDrawCount\": " << shadowAgg.fallbackDrawCount
       << ",\n";
  json << "    \"fallbackDrawCountTerrain\": "
       << shadowAgg.fallbackDrawCountTerrain << ",\n";
  json << "    \"fallbackDrawCountWorldObject\": "
       << shadowAgg.fallbackDrawCountWorldObject << ",\n";
  json << "    \"fallbackDrawCountUnitObject\": "
       << shadowAgg.fallbackDrawCountUnitObject << ",\n";
  json << "    \"objectFallbackDrawCount\": "
       << shadowAgg.objectFallbackDrawCount << ",\n";
  json << "    \"drawTimeVBCacheCaptureCount\": "
       << shadowAgg.drawTimeVBCacheCaptureCount << ",\n";
  json << "    \"drawTimeVBCacheConsumeHitCount\": "
       << shadowAgg.drawTimeVBCacheConsumeHitCount << ",\n";
  json << "    \"drawTimeVBCacheConsumeMissCount\": "
       << shadowAgg.drawTimeVBCacheConsumeMissCount << ",\n";
  json << "    \"drawTimeVBCacheRejectNoLayerContext\": "
       << shadowAgg.drawTimeVBCacheRejectNoLayerContext << ",\n";
  json << "    \"drawTimeVBCacheSameFrameDedupMiss\": "
       << shadowAgg.drawTimeVBCacheSameFrameDedupMiss << ",\n";
  json << "    \"semanticBridgeHit\": " << shadowAgg.semanticBridgeHit
       << ",\n";
  json << "    \"semanticBridgeMiss\": " << shadowAgg.semanticBridgeMiss
       << ",\n";
  json << "    \"semanticBridgeBypassed\": "
       << shadowAgg.semanticBridgeBypassed << ",\n";
  json << "    \"semanticSceneSubmitted\": "
       << shadowAgg.semanticSceneSubmitted << ",\n";
  json << "    \"semanticSceneSubmittedUnit\": "
       << shadowAgg.semanticSceneSubmittedUnit << ",\n";
  json << "    \"semanticSceneSubmittedSkinned\": "
       << shadowAgg.semanticSceneSubmittedSkinned << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedNonUnitResolvedCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnknownPacketKindCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnknownPacketKindCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedGroupNonZeroCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedGroupNonZeroCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedTransparentQueueCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedTransparentQueueCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedMissingUnitPtrCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedMissingUnitPtrCount << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount
       << ",\n";
  json << "    \"semanticSceneSubmittedBuilding\": "
       << shadowAgg.semanticSceneSubmittedBuilding << ",\n";
  json << "    \"semanticSceneSubmittedDestructible\": "
       << shadowAgg.semanticSceneSubmittedDestructible << ",\n";
  json << "    \"semanticSceneSubmittedCutout\": "
       << shadowAgg.semanticSceneSubmittedCutout << ",\n";
  json << "    \"semanticSceneSubmittedAlphaBlend\": "
       << shadowAgg.semanticSceneSubmittedAlphaBlend << ",\n";
  json << "    \"semanticSceneMaterialObservedCutoutCount\": "
       << shadowAgg.semanticSceneMaterialObservedCutoutCount << ",\n";
  json << "    \"semanticSceneMaterialObservedAlphaBlendCount\": "
       << shadowAgg.semanticSceneMaterialObservedAlphaBlendCount << ",\n";
  json << "    \"semanticSceneRejectedCutoutSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedCutoutSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedCutoutGeometry\": "
       << shadowAgg.semanticSceneRejectedCutoutGeometry << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendGeometry\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendGeometry << ",\n";
  json << "    \"semanticSceneRejectedCutoutVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedCutoutVisualPolicy << ",\n";
  json << "    \"semanticSceneRejectedAlphaBlendVisualPolicy\": "
       << shadowAgg.semanticSceneRejectedAlphaBlendVisualPolicy << ",\n";
  json << "    \"semanticSceneMaterialLayerContractResolvedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractResolvedCount << ",\n";
  json << "    \"semanticSceneMaterialLayerContractFailedCount\": "
       << shadowAgg.semanticSceneMaterialLayerContractFailedCount << ",\n";
  json << "    \"semanticSceneMaterialBlendMode0Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode0Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode1Count\": "
       << shadowAgg.semanticSceneMaterialBlendMode1Count << ",\n";
  json << "    \"semanticSceneMaterialBlendMode2PlusCount\": "
       << shadowAgg.semanticSceneMaterialBlendMode2PlusCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawLayerIndexNonZeroCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawLayerIndexNonZeroCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseAvailableCount\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseAvailableCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsSceneReadSuccessCount\": "
       << shadowAgg.semanticSceneFastAppendBoundsSceneReadSuccessCount << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe1Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe1Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe4Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe4Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaLe16Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaLe16Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaGt16Count\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaGt16Count << ",\n";
  json << "    \"semanticSceneFastAppendBoundsPoseDeltaMaxMilli\": "
       << shadowAgg.semanticSceneFastAppendBoundsPoseDeltaMaxMilli << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshAttemptCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshAttemptCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshHitCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshHitCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshMissCount\": "
       << shadowAgg.semanticSceneLivePaletteRefreshMissCount << ",\n";
  json << "    \"semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount\": "
       << shadowAgg.semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount << ",\n";
  json << "    \"semanticScenePaletteOverrideNoComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideNoComposeCount << ",\n";
  json << "    \"semanticScenePaletteOverrideWouldComposeCount\": "
       << shadowAgg.semanticScenePaletteOverrideWouldComposeCount << ",\n";
  json << "    \"semanticScenePalettePacketWorldComposeCount\": "
       << shadowAgg.semanticScenePalettePacketWorldComposeCount << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastMatrixCount\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneLivePaletteRefreshLastMatrixHash\": "
       << runtimeSummary.semanticSceneLivePaletteRefreshLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionRawStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionRawStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupChangedCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupChangedCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionGroupStableCount\": "
       << shadowAgg.semanticSceneLivePaletteMotionGroupStableCount << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastPrevRawHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastPrevRawHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastRawHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastRawHash << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastPrevGroupHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastPrevGroupHash
       << ",\n";
  json << "    \"semanticSceneLivePaletteMotionLastGroupHash\": "
       << runtimeSummary.semanticSceneLivePaletteMotionLastGroupHash << ",\n";
  json << "    \"semanticSceneDrawTimePoseAttemptCount\": "
       << shadowAgg.semanticSceneDrawTimePoseAttemptCount << ",\n";
  json << "    \"semanticSceneDrawTimePosePublishedCount\": "
       << shadowAgg.semanticSceneDrawTimePosePublishedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectUiOrEffectCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectUiOrEffectCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectVertexShaderCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectVertexShaderCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoVertexBlendCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoVertexBlendCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoContextCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoContextCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseRejectNoRuntimeModelCount\": "
       << shadowAgg.semanticSceneDrawTimePoseRejectNoRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseDedupedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseDedupedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseChangedCount\": "
       << shadowAgg.semanticSceneDrawTimePoseChangedCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseStableCount\": "
       << shadowAgg.semanticSceneDrawTimePoseStableCount << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastRuntimeModelPtr << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastPrevHash\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastPrevHash << ",\n";
  json << "    \"semanticSceneDrawTimePoseLastHash\": "
       << runtimeSummary.semanticSceneDrawTimePoseLastHash << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionSampleCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionSampleCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionNewRuntimeCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionNewRuntimeCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionChangedCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionChangedCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionStableCount\": "
       << shadowAgg.semanticSceneSubmittedPaletteMotionStableCount << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastPrevHash\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastPrevHash
       << ",\n";
  json << "    \"semanticSceneSubmittedPaletteMotionLastHash\": "
       << runtimeSummary.semanticSceneSubmittedPaletteMotionLastHash << ",\n";
  json << "    \"semanticSceneSkinnedDynamicIndexSliceCount\": "
       << shadowAgg.semanticSceneSkinnedDynamicIndexSliceCount << ",\n";
  json << "    \"semanticSceneSubmittedOwnedGroupSlots\": "
       << shadowAgg.semanticSceneSubmittedOwnedGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawContractKnownCount\": "
       << shadowAgg.semanticSceneCurrentDrawContractKnownCount << ",\n";
  json << "    \"semanticSceneCurrentDrawPaletteReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawPaletteReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawGroupSlotReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawGroupSlotReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyCount << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoContract\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoContract << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoPalette\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoPalette << ",\n";
  json << "    \"semanticSceneCurrentDrawMissNoGroupSlots\": "
       << shadowAgg.semanticSceneCurrentDrawMissNoGroupSlots << ",\n";
  json << "    \"semanticSceneCurrentDrawMissStaleVisibleFrame\": "
       << shadowAgg.semanticSceneCurrentDrawMissStaleVisibleFrame << ",\n";
  json << "    \"semanticSceneCurrentDrawResolveReadyRejectedCount\": "
       << shadowAgg.semanticSceneCurrentDrawResolveReadyRejectedCount << ",\n";
  json << "    \"semanticSceneCanonicalReadyCount\": "
       << shadowAgg.semanticSceneCanonicalReadyCount << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoStableIdentity\": "
       << shadowAgg.semanticSceneCanonicalRejectNoStableIdentity << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoMesh\": "
       << shadowAgg.semanticSceneCanonicalRejectNoMesh << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoWorldTransform\": "
       << shadowAgg.semanticSceneCanonicalRejectNoWorldTransform << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoPalette\": "
       << shadowAgg.semanticSceneCanonicalRejectNoPalette << ",\n";
  json << "    \"semanticSceneCanonicalRejectNoSlotContract\": "
       << shadowAgg.semanticSceneCanonicalRejectNoSlotContract << ",\n";
  json << "    \"semanticSceneCanonicalRejectStaleProducer\": "
       << shadowAgg.semanticSceneCanonicalRejectStaleProducer << ",\n";
  json << "    \"semanticSceneCanonicalRejectInvalidVertexIndex\": "
       << shadowAgg.semanticSceneCanonicalRejectInvalidVertexIndex << ",\n";
  json << "    \"semanticSceneCanonicalRejectExplicitBlendIncomplete\": "
       << shadowAgg.semanticSceneCanonicalRejectExplicitBlendIncomplete << ",\n";
  json << "    \"semanticSceneCanonicalRejectAfterReadyCount\": "
       << shadowAgg.semanticSceneCanonicalRejectAfterReadyCount << ",\n";
  json << "    \"semanticSceneSubmittedExplicitBlendContract\": "
       << shadowAgg.semanticSceneSubmittedExplicitBlendContract << ",\n";
  json << "    \"semanticSceneSubmittedSingleMatrixGroupSkinning\": "
       << shadowAgg.semanticSceneSubmittedSingleMatrixGroupSkinning << ",\n";
  json << "    \"semanticSceneSubmittedMultiGroupSlotSkinning\": "
       << shadowAgg.semanticSceneSubmittedMultiGroupSlotSkinning << ",\n";
  json << "    \"semanticSceneSkinnedMinUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMinUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedMaxUniqueGroupSlots\": "
       << shadowAgg.semanticSceneSkinnedMaxUniqueGroupSlots << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique1Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique1Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique2To4Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique2To4Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique5To8Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique5To8Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique9To16Count\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique9To16Count << ",\n";
  json << "    \"semanticSceneSkinnedGroupSlotsUnique17PlusCount\": "
       << shadowAgg.semanticSceneSkinnedGroupSlotsUnique17PlusCount << ",\n";
  json << "    \"semanticSceneExplicitBlendUnavailableCurrentDraw\": "
       << shadowAgg.semanticSceneExplicitBlendUnavailableCurrentDraw << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackCount\": "
       << shadowAgg.semanticSceneSkinnedFullIndexFallbackCount << ",\n";
  json << "    \"semanticSceneSkinnedMissingVisibleIndexSliceRejectCount\": "
       << shadowAgg.semanticSceneSkinnedMissingVisibleIndexSliceRejectCount
       << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr\": "
       << runtimeSummary
              .semanticSceneSkinnedFullIndexFallbackLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneSkinnedFullIndexFallbackLastIndexCount\": "
       << runtimeSummary.semanticSceneSkinnedFullIndexFallbackLastIndexCount
       << ",\n";
  json << "    \"semanticSceneSubmittedFrameLocal\": "
       << shadowAgg.semanticSceneSubmittedFrameLocal << ",\n";
  json << "    \"semanticSceneSubmittedPersistent\": "
       << shadowAgg.semanticSceneSubmittedPersistent << ",\n";
  json << "    \"semanticScenePopulateAttemptCount\": "
       << shadowAgg.semanticScenePopulateAttemptCount << ",\n";
  json << "    \"semanticScenePopulateUnitsOnlyCount\": "
       << shadowAgg.semanticScenePopulateUnitsOnlyCount << ",\n";
  json << "    \"semanticScenePopulateLastReturnReason\": "
       << shadowAgg.semanticScenePopulateLastReturnReason << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerPublishReadyDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerPublishReadyDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerQueryHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerQueryHitDelta << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta\": "
       << shadowAgg
              .semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeAttemptDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeAttemptDelta
       << ",\n";
  json << "    \"semanticScenePopulateLastProducerGroupDecodeHitDelta\": "
       << shadowAgg.semanticScenePopulateLastProducerGroupDecodeHitDelta
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawRecordCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawRecordCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltPacketCount << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount\": "
       << shadowAgg.semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount
       << ",\n";
  json << "    \"semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount\": "
       << shadowAgg
              .semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount
       << ",\n";
  json << "    \"drawTimeSemanticProducerVisibleCandidateCount\": "
       << shadowAgg.drawTimeSemanticProducerVisibleCandidateCount << ",\n";
  json << "    \"drawTimeSemanticProducerFreshEntryCount\": "
       << shadowAgg.drawTimeSemanticProducerFreshEntryCount << ",\n";
  json << "    \"drawTimeSemanticProducerClaimedCount\": "
       << shadowAgg.drawTimeSemanticProducerClaimedCount << ",\n";
  json << "    \"drawTimeSemanticProducerSubmittedCount\": "
       << shadowAgg.drawTimeSemanticProducerSubmittedCount << ",\n";
  json << "    \"drawTimeSemanticProducerMissNoFreshEntryCount\": "
       << shadowAgg.drawTimeSemanticProducerMissNoFreshEntryCount << ",\n";
  json << "    \"drawTimeSemanticProducerFallbackCurrentDrawCount\": "
       << shadowAgg.drawTimeSemanticProducerFallbackCurrentDrawCount << ",\n";
  json << "    \"drawTimeSemanticProducerOwnedDirectGroupedSkipCount\": "
       << shadowAgg.drawTimeSemanticProducerOwnedDirectGroupedSkipCount
       << ",\n";
  json << "    \"drawTimeSemanticProducerLifecycleMergedCount\": "
       << shadowAgg.drawTimeSemanticProducerLifecycleMergedCount << ",\n";
  json << "    \"semanticSceneProducerClaimObserveMode\": "
       << shadowAgg.semanticSceneProducerClaimObserveMode << ",\n";
  json << "    \"semanticSceneProducerClaimExactKeyCount\": "
       << shadowAgg.semanticSceneProducerClaimExactKeyCount << ",\n";
  json << "    \"semanticSceneProducerClaimCandidateCount\": "
       << shadowAgg.semanticSceneProducerClaimCandidateCount << ",\n";
  json << "    \"semanticSceneProducerClaimCanonicalOwnedCount\": "
       << shadowAgg.semanticSceneProducerClaimCanonicalOwnedCount << ",\n";
  json << "    \"semanticSceneProducerClaimMissingKeyCount\": "
       << shadowAgg.semanticSceneProducerClaimMissingKeyCount << ",\n";
  json << "    \"semanticSceneProducerClaimUnresolvedCount\": "
       << shadowAgg.semanticSceneProducerClaimUnresolvedCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictPredictedCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictPredictedCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictMatchCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictMatchCount << ",\n";
  json << "    \"semanticSceneProducerClaimStrictFalsePositiveCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictFalsePositiveCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimStrictFalseNegativeCount\": "
       << shadowAgg.semanticSceneProducerClaimStrictFalseNegativeCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalPredictedCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalPredictedCount << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalMatchCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalMatchCount << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalFalsePositiveCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalFalsePositiveCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimLogicalFalseNegativeCount\": "
       << shadowAgg.semanticSceneProducerClaimLogicalFalseNegativeCount
       << ",\n";
  json << "    \"semanticSceneProducerClaimConsumeDeniedCount\": "
       << shadowAgg.semanticSceneProducerClaimConsumeDeniedCount << ",\n";

  // Phase 7.2: flicker diagnostics + reconciliation
  json << "    \"semanticSceneDirectLastRawRecordCount\": "
       << shadowAgg.semanticSceneDirectLastRawRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastEligibleRecordCount\": "
       << shadowAgg.semanticSceneDirectLastEligibleRecordCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableMode\": "
       << shadowAgg.semanticSceneCompactWorkTableMode << ",\n";
  json << "    \"semanticSceneCompactWorkTableCandidateCount\": "
       << shadowAgg.semanticSceneCompactWorkTableCandidateCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableSealedCount\": "
       << shadowAgg.semanticSceneCompactWorkTableSealedCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableConsumedCount\": "
       << shadowAgg.semanticSceneCompactWorkTableConsumedCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableFallbackCount\": "
       << shadowAgg.semanticSceneCompactWorkTableFallbackCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectStageCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectStageCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectFreshnessCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectFreshnessCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectPolicyCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectPolicyCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectFrameCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectFrameCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableRejectIdentityCount\": "
       << shadowAgg.semanticSceneCompactWorkTableRejectIdentityCount << ",\n";
  json << "    \"semanticSceneCompactWorkTableMismatchCount\": "
       << shadowAgg.semanticSceneCompactWorkTableMismatchCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedRecordCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedRecordCount << ",\n";
  json << "    \"semanticSceneDirectLastUniqueObjectCount\": "
       << shadowAgg.semanticSceneDirectLastUniqueObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedObjectCount\": "
       << shadowAgg.semanticSceneDirectLastSubmittedObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastRecordCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastRecordCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastScanCapPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectLastScanCapPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectLastMinGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMinGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastMaxGeosetsPerObject\": "
       << shadowAgg.semanticSceneDirectLastMaxGeosetsPerObject << ",\n";
  json << "    \"semanticSceneDirectLastSubmittedIdentityHash\": "
       << shadowAgg.semanticSceneDirectLastSubmittedIdentityHash << ",\n";
  json << "    \"semanticSceneDirectIdentityChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentityChurnCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapHitCount\": "
       << shadowAgg.semanticSceneDirectRecordCapHitCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapTruncatedRecordCount\": "
       << shadowAgg.semanticSceneDirectRecordCapTruncatedRecordCount << ",\n";
  json << "    \"semanticSceneDirectScanCapHitCount\": "
       << shadowAgg.semanticSceneDirectScanCapHitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSubmitCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSubmitCount << ",\n";
  json << "    \"semanticSceneDirectObjectGroupedSkipCount\": "
       << shadowAgg.semanticSceneDirectObjectGroupedSkipCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapSkipObjectCount\": "
       << shadowAgg.semanticSceneDirectRecordCapSkipObjectCount << ",\n";
  json << "    \"semanticSceneDirectRecordCapAppendFailCount\": "
       << shadowAgg.semanticSceneDirectRecordCapAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseActiveKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseActiveKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeasePrunedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeasePrunedKeyCount << ",\n";
  json << "    \"semanticSceneDirectSelectionLeaseSubmittedKeyCount\": "
       << shadowAgg.semanticSceneDirectSelectionLeaseSubmittedKeyCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillBudgetRecordCount\": "
       << shadowAgg.semanticSceneDirectStickyFillBudgetRecordCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillAppendedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillAppendedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillSubmittedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillSubmittedCount << ",\n";
  json << "    \"semanticSceneDirectStickyFillMissedCount\": "
       << shadowAgg.semanticSceneDirectStickyFillMissedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRestoredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseUpdatedCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseUpdatedCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseExpiredCount << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedDynamicMeshCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedDynamicMeshCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneDirectPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneDirectPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneDirectPartLeaseBudgetLimitCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseExpiredCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseExpiredCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseBudgetLimitCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseBudgetLimitCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreCompleteCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreIncompleteSkipCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCoreIncompleteSkipCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestPartOmittedIncompleteCoreCount\": "
       << shadowAgg.semanticSceneShadowManifestPartOmittedIncompleteCoreCount
       << ",\n";
  // Phase 7.25 core epoch planner 专属计数器。
  json << "    \"semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochMissingPartCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochMissingPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount\": "
       << shadowAgg
              .semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount\": "
       << shadowAgg
              .semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount\": "
       << shadowAgg
              .semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseExpiredBackingOnlyCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseExpiredBackingOnlyCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount\": "
       << shadowAgg
              .semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestMissingRequiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestMissingRequiredPartCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestGraceUsedCount\": "
       << shadowAgg.semanticSceneShadowManifestGraceUsedCount << ",\n";
  json << "    \"semanticSceneShadowManifestTombstoneRetiredCount\": "
       << shadowAgg.semanticSceneShadowManifestTombstoneRetiredCount
       << ",\n";
  json << "    \"shadowMetadataClassifiedCount\": "
       << shadowMetadataSnapshot.metadataClassifiedCount << ",\n";
  json << "    \"shadowMetadataCapturedCount\": "
       << shadowMetadataSnapshot.metadataCapturedCount << ",\n";
  json << "    \"shadowMetadataAppliedCount\": "
       << shadowMetadataSnapshot.metadataAppliedCount << ",\n";
  json << "    \"shadowMetadataRejectedFrameCount\": "
       << shadowMetadataSnapshot.metadataRejectedFrameCount << ",\n";
  json << "    \"shadowMetadataRejectedGenerationCount\": "
       << shadowMetadataSnapshot.metadataRejectedGenerationCount << ",\n";
  json << "    \"shadowMetadataAmbiguousCount\": "
       << shadowMetadataSnapshot.metadataAmbiguousCount << ",\n";
  json << "    \"shadowMetadataRejectedNoMaterialCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoMaterialCount << ",\n";
  json << "    \"shadowMetadataRejectedOpaqueCount\": "
       << shadowMetadataSnapshot.metadataRejectedOpaqueCount << ",\n";
  json << "    \"shadowMetadataRejectedNoUvCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoUvCount << ",\n";
  json << "    \"shadowMetadataRejectedNoDiffuseCount\": "
       << shadowMetadataSnapshot.metadataRejectedNoDiffuseCount << ",\n";
  json << "    \"shadowMetadataRejectedUploadCount\": "
       << shadowMetadataSnapshot.metadataRejectedUploadCount << ",\n";
  json << "    \"shadowMetadataRejectedDuplicateCount\": "
       << shadowMetadataSnapshot.metadataRejectedDuplicateCount << ",\n";
  json << "    \"shadowMetadataRejectedByReasonCount\": "
       << shadowMetadataRejectedByReasonCount << ",\n";
  json << "    \"shadowMetadataBlockerKnownRawcodeCount\": "
       << shadowMetadataSnapshot.blockerKnownRawcodeCount << ",\n";
  json << "    \"shadowMetadataBlockerWidgetIdentityCount\": "
       << shadowMetadataSnapshot.blockerWidgetIdentityCount << ",\n";
  json << "    \"shadowMetadataBlockerSmallFlatCount\": "
       << shadowMetadataSnapshot.blockerSmallFlatCount << ",\n";
  json << "    \"shadowMetadataBlockerBelowGroundCount\": "
       << shadowMetadataSnapshot.blockerBelowGroundCount << ",\n";
  json << "    \"shadowMetadataBlockerUnreadableCount\": "
       << shadowMetadataSnapshot.blockerUnreadableCount << ",\n";
  json << "    \"shadowMetadataBlockerFinalLeakCount\": "
       << shadowMetadataSnapshot.blockerFinalLeakCount << ",\n";
  // Phase 7.28：skinned palette content stability probe。
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceNoneCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteSourceNoneCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStablePartSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStablePartSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSourceChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSourceChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteCountChurnCount\": "
       << shadowAgg.semanticSceneSubmittedSkinnedPaletteCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount\": "
       << shadowAgg
              .semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteAttributionSnapshotHitCount\": "
       << shadowAgg
              .semanticSceneDirectPaletteAttributionSnapshotHitCount
       << ",\n";
  json << "    \"semanticSceneDirectManifestObjectCount\": "
       << shadowAgg.semanticSceneDirectManifestObjectCount << ",\n";
  json << "    \"semanticSceneDirectManifestObservedPartCount\": "
       << shadowAgg.semanticSceneDirectManifestObservedPartCount << ",\n";
  json << "    \"semanticSceneDirectManifestShadowEligiblePartCount\": "
       << shadowAgg.semanticSceneDirectManifestShadowEligiblePartCount << ",\n";
  json << "    \"semanticSceneDirectObjectCompleteEligibleCount\": "
       << shadowAgg.semanticSceneDirectObjectCompleteEligibleCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByScanCapCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByScanCapCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAlphaPolicyCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAlphaPolicyCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteBySliceUnresolvedCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteBySliceUnresolvedCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByPacketBuildFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByPacketBuildFailCount << ",\n";
  json << "    \"semanticSceneDirectObjectIncompleteByAppendFailCount\": "
       << shadowAgg.semanticSceneDirectObjectIncompleteByAppendFailCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedCompleteObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedCompleteObjectCount << ",\n";
  json << "    \"semanticSceneDirectSubmittedPartialObjectCount\": "
       << shadowAgg.semanticSceneDirectSubmittedPartialObjectCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceAuthoritativeCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceAuthoritativeCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceFallbackLayerIndexCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceFallbackLayerIndexCount << ",\n";
  json << "    \"semanticSceneDirectPreparedSliceMissingCount\": "
       << shadowAgg.semanticSceneDirectPreparedSliceMissingCount << ",\n";
  json << "    \"semanticScenePreparedProbeAttemptCount\": "
       << shadowAgg.semanticScenePreparedProbeAttemptCount << ",\n";
  json << "    \"semanticScenePreparedProbeContextReadyCount\": "
       << shadowAgg.semanticScenePreparedProbeContextReadyCount << ",\n";
  json << "    \"semanticScenePreparedProbeBackingReadableCount\": "
       << shadowAgg.semanticScenePreparedProbeBackingReadableCount << ",\n";
  json << "    \"semanticScenePreparedSliceRecordedCount\": "
       << shadowAgg.semanticScenePreparedSliceRecordedCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryAttemptCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryAttemptCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryHitCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryHitCount << ",\n";
  json << "    \"semanticScenePreparedSliceQueryMissCount\": "
       << shadowAgg.semanticScenePreparedSliceQueryMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestPartCount\": "
       << shadowAgg.semanticSceneShadowManifestPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestStableObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestStableObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestNewObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestNewObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredObjectCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredObjectCount << ",\n";
  json << "    \"semanticSceneShadowManifestFreshPartCount\": "
       << shadowAgg.semanticSceneShadowManifestFreshPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestLeaseablePartCount\": "
       << shadowAgg.semanticSceneShadowManifestLeaseablePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPoseStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestPoseStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestSliceStalePartCount\": "
       << shadowAgg.semanticSceneShadowManifestSliceStalePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestExpiredPartCount\": "
       << shadowAgg.semanticSceneShadowManifestExpiredPartCount << ",\n";
  json << "    \"semanticSceneShadowManifestMultiSlicePartCount\": "
       << shadowAgg.semanticSceneShadowManifestMultiSlicePartCount << ",\n";
  json << "    \"semanticSceneShadowManifestPayload11CChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestPayload11CChurnCount << ",\n";
  json << "    \"semanticSceneShadowManifestRenderablePartChurnCount\": "
       << shadowAgg.semanticSceneShadowManifestRenderablePartChurnCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseHitCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseHitCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseMissCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseMissCount << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseNoRuntimeCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseNoRuntimeCount
       << ",\n";
  json << "    "
          "\"semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount\": "
       << shadowAgg
              .semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixCount\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixCount
       << ",\n";
  json << "    \"semanticSceneShadowManifestCModelPoseLastMatrixHash\": "
       << shadowAgg.semanticSceneShadowManifestCModelPoseLastMatrixHash
       << ",\n";
  json << "    \"semanticSceneSubmittedObjectJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedObjectJaccardMilli << ",\n";
  json << "    \"semanticSceneSubmittedPartJaccardMilli\": "
       << shadowAgg.semanticSceneSubmittedPartJaccardMilli << ",\n";
  json << "    \"semanticSceneVisibleLookupPartLayerHitCount\": "
       << shadowAgg.semanticSceneVisibleLookupPartLayerHitCount << ",\n";
  json << "    \"semanticSceneVisibleLookupSingleFallbackCount\": "
       << shadowAgg.semanticSceneVisibleLookupSingleFallbackCount << ",\n";
  json << "    \"semanticSceneVisibleLookupMissCount\": "
       << shadowAgg.semanticSceneVisibleLookupMissCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingNotCheckedCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingNotCheckedCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingPassCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingPassCount << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNoRenderablePartCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailNoRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailLookupMissCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailLookupMissCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonMainQueueCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonMainQueueCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailNonWorldGroupCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailNonWorldGroupCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailIdentityMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailIdentityMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount\": "
       << shadowAgg
              .semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount\": "
       << shadowAgg.semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteHashChurnCount\": "
       << shadowAgg.semanticSceneDirectPaletteHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStableGroupHashChurnCount\": "
       << shadowAgg.semanticSceneDirectStableGroupHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectStream1PtrChurnCount\": "
       << shadowAgg.semanticSceneDirectStream1PtrChurnCount << ",\n";
  json << "    \"semanticSceneDirectGeometrySourceHashChurnCount\": "
       << shadowAgg.semanticSceneDirectGeometrySourceHashChurnCount << ",\n";
  json << "    \"semanticSceneDirectSameCasterComparisonCount\": "
       << shadowAgg.semanticSceneDirectSameCasterComparisonCount << ",\n";
  json << "    \"semanticSceneDirectIdentitySkippedChurnCount\": "
       << shadowAgg.semanticSceneDirectIdentitySkippedChurnCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootDeltaSampleCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootDeltaSampleCount << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedTinyDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedTinyDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedSmallDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedSmallDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedMediumDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedMediumDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootHashChangedLargeDeltaCount\": "
       << shadowAgg.semanticSceneDirectPaletteRootHashChangedLargeDeltaCount
       << ",\n";
  json << "    \"semanticSceneDirectPaletteRootMaxDeltaMilli\": "
       << shadowAgg.semanticSceneDirectPaletteRootMaxDeltaMilli << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyUnitPtrCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyUnitPtrCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyJHandleCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyJHandleCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRuntimeModelCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRuntimeModelCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyWorldObjectCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyWorldObjectCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeySceneNodeCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeySceneNodeCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyModelMeshCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyModelMeshCount << ",\n";
  json << "    \"semanticSceneDirectSelectionKeyRenderablePartCount\": "
       << shadowAgg.semanticSceneDirectSelectionKeyRenderablePartCount
       << ",\n";
  json << "    \"semanticSceneLastAppendedGeometrySourceHash\": "
       << shadowAgg.semanticSceneLastAppendedGeometrySourceHash << ",\n";
  json << "    \"semanticSceneLastAppendedGeometryId\": "
       << shadowAgg.semanticSceneLastAppendedGeometryId << ",\n";
  json << "    \"semanticSceneShadowCastersCount\": "
       << shadowAgg.semanticSceneShadowCastersCount << ",\n";
  json << "    \"semanticSceneReplayDrawsCount\": "
       << shadowAgg.semanticSceneReplayDrawsCount << ",\n";
  json << "    \"semanticSceneShadowMapDrawnCasters\": "
       << shadowAgg.semanticSceneShadowMapDrawnCasters << ",\n";
  json << "    \"semanticSceneShadowMapCascadeCulledCount\": "
       << shadowAgg.semanticSceneShadowMapCascadeCulledCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsCullMode\": "
       << shadowAgg.semanticSceneTerrainBoundsCullMode << ",\n";
  json << "    \"semanticSceneTerrainBoundsCandidateCount\": "
       << shadowAgg.semanticSceneTerrainBoundsCandidateCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsProofAcceptedCount\": "
       << shadowAgg.semanticSceneTerrainBoundsProofAcceptedCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsFailVisibleCount\": "
       << shadowAgg.semanticSceneTerrainBoundsFailVisibleCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsWouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsWouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsAppliedCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsAppliedCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC0WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC0WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC1WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC1WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC2WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC2WouldCullCount << ",\n";
  json << "    \"semanticSceneTerrainBoundsC3WouldCullCount\": "
       << shadowAgg.semanticSceneTerrainBoundsC3WouldCullCount << ",\n";
  json << "    \"semanticSceneObjectBoundsCandidateCount\": "
       << shadowAgg.semanticSceneObjectBoundsCandidateCount << ",\n";
  json << "    \"semanticSceneObjectBoundsProofAcceptedCount\": "
       << shadowAgg.semanticSceneObjectBoundsProofAcceptedCount << ",\n";
  json << "    \"semanticSceneObjectBoundsFailVisibleCount\": "
       << shadowAgg.semanticSceneObjectBoundsFailVisibleCount << ",\n";
  json << "    \"semanticSceneObjectBoundsWouldCullCount\": "
       << shadowAgg.semanticSceneObjectBoundsWouldCullCount << ",\n";
  json << "    \"semanticSceneObjectBoundsAppliedCullCount\": "
       << shadowAgg.semanticSceneObjectBoundsAppliedCullCount << ",\n";
  json << "    \"semanticSceneUnionCullMode\": "
       << shadowAgg.semanticSceneUnionCullMode << ",\n";
  json << "    \"semanticSceneUnionCullObserveFrameCount\": "
       << shadowAgg.semanticSceneUnionCullObserveFrameCount << ",\n";
  json << "    \"semanticSceneUnionCullCandidateCount\": "
       << shadowAgg.semanticSceneUnionCullCandidateCount << ",\n";
  json << "    \"semanticSceneUnionCullProofAcceptedCount\": "
       << shadowAgg.semanticSceneUnionCullProofAcceptedCount << ",\n";
  json << "    \"semanticSceneUnionCullFailVisibleCount\": "
       << shadowAgg.semanticSceneUnionCullFailVisibleCount << ",\n";
  json << "    \"semanticSceneUnionCullDynamicConservativeCount\": "
       << shadowAgg.semanticSceneUnionCullDynamicConservativeCount << ",\n";
  json << "    \"semanticSceneUnionCullUnknownOrStaleCount\": "
       << shadowAgg.semanticSceneUnionCullUnknownOrStaleCount << ",\n";
  json << "    \"semanticSceneUnionCullC2WouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullC2WouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullC3WouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullC3WouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullBothFarWouldCullCount\": "
       << shadowAgg.semanticSceneUnionCullBothFarWouldCullCount << ",\n";
  json << "    \"semanticSceneUnionCullFalseNegativeCount\": "
       << shadowAgg.semanticSceneUnionCullFalseNegativeCount << ",\n";
  json << "    \"semanticSceneUnionCullFalsePositiveCount\": "
       << shadowAgg.semanticSceneUnionCullFalsePositiveCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedCasterCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedCasterCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedPreparedCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedPreparedCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidBufferCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidBufferCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedInvalidPipelineCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedInvalidPipelineCount << ",\n";
  json << "    \"semanticSceneShadowMapSkinnedDrawnCount\": "
       << shadowAgg.semanticSceneShadowMapSkinnedDrawnCount << ",\n";
  json << "    \"semanticSceneShadowTaaActive\": "
       << shadowAgg.semanticSceneShadowTaaActive << ",\n";
  json << "    \"shadowTaaFramesObserved\": "
       << shadowAgg.shadowTaaFramesObserved << ",\n";
  json << "    \"shadowTaaRuntimeModuleDisabledFrames\": "
       << shadowAgg.shadowTaaRuntimeModuleDisabledFrames << ",\n";
  json << "    \"shadowTaaRequestedDirectFrames\": "
       << shadowAgg.shadowTaaRequestedDirectFrames << ",\n";
  json << "    \"shadowTaaRequestedPrepassFrames\": "
       << shadowAgg.shadowTaaRequestedPrepassFrames << ",\n";
  json << "    \"shadowTaaRequestedTemporalFrames\": "
       << shadowAgg.shadowTaaRequestedTemporalFrames << ",\n";
  json << "    \"shadowTaaEffectiveDirectFrames\": "
       << shadowAgg.shadowTaaEffectiveDirectFrames << ",\n";
  json << "    \"shadowTaaEffectivePrepassFrames\": "
       << shadowAgg.shadowTaaEffectivePrepassFrames << ",\n";
  json << "    \"shadowTaaEffectiveTemporalFrames\": "
       << shadowAgg.shadowTaaEffectiveTemporalFrames << ",\n";
  json << "    \"shadowTaaBlockedSemanticDynamicFrames\": "
       << shadowAgg.shadowTaaBlockedSemanticDynamicFrames << ",\n";
  json << "    \"shadowTaaBlockedSunMotionFrames\": "
       << shadowAgg.shadowTaaBlockedSunMotionFrames << ",\n";
  json << "    \"shadowTaaBlockedCsmFallbackFrames\": "
       << shadowAgg.shadowTaaBlockedCsmFallbackFrames << ",\n";
  json << "    \"shadowTaaVisibilityExecutedFrames\": "
       << shadowAgg.shadowTaaVisibilityExecutedFrames << ",\n";
  json << "    \"shadowTaaMotionVectorExecutedFrames\": "
       << shadowAgg.shadowTaaMotionVectorExecutedFrames << ",\n";
  json << "    \"shadowTaaReceiverExecutedFrames\": "
       << shadowAgg.shadowTaaReceiverExecutedFrames << ",\n";
  json << "    \"shadowTaaHistoryWriteExecutedFrames\": "
       << shadowAgg.shadowTaaHistoryWriteExecutedFrames << ",\n";
  json << "    \"shadowTaaHistoryAdvancedFrames\": "
       << shadowAgg.shadowTaaHistoryAdvancedFrames << ",\n";
  json << "    \"shadowTaaHistoryAdvanceSkippedIncompleteFrames\": "
       << shadowAgg.shadowTaaHistoryAdvanceSkippedIncompleteFrames << ",\n";
  json << "    \"shadowTaaHistoryValidBeforeFrames\": "
       << shadowAgg.shadowTaaHistoryValidBeforeFrames << ",\n";
  json << "    \"shadowTaaHistoryValidAfterFrames\": "
       << shadowAgg.shadowTaaHistoryValidAfterFrames << ",\n";
  json << "    \"shadowTaaHistoryInvalidatedFrames\": "
       << shadowAgg.shadowTaaHistoryInvalidatedFrames << ",\n";
  json << "    \"shadowTaaHistoryInvalidationMaskLast\": "
       << shadowAgg.shadowTaaHistoryInvalidationMaskLast << ",\n";
  json << "    \"shadowTaaHistoryInvalidationReasonFrames\": [";
  for (size_t i = 0u;
       i < shadowAgg.shadowTaaHistoryInvalidationReasonFrames.size(); ++i) {
    if (i != 0u)
      json << ",";
    json << shadowAgg.shadowTaaHistoryInvalidationReasonFrames[i];
  }
  json << "],\n";
  json << "    \"stage13CaptureAttemptCount\": "
       << shadowAgg.stage13CaptureAttemptCount << ",\n";
  json << "    \"stage13CaptureRejectedNoDemandCount\": "
       << shadowAgg.stage13CaptureRejectedNoDemandCount << ",\n";
  json << "    \"stage13CaptureRejectedAfterBeforeUiCount\": "
       << shadowAgg.stage13CaptureRejectedAfterBeforeUiCount << ",\n";
  json << "    \"stage13CaptureConsideredCount\": "
       << shadowAgg.stage13CaptureConsideredCount << ",\n";
  json << "    \"stage13FreezeCopyBytes\": "
       << shadowAgg.stage13FreezeCopyBytes << ",\n";
  json << "    \"stage13CpuSnapshotCopyBytes\": "
       << shadowAgg.stage13CpuSnapshotCopyBytes << ",\n";
  json << "    \"stage13RetentionSnapshotBytes\": "
       << shadowAgg.stage13RetentionSnapshotBytes << ",\n";
  json << "    \"stage13ReplayDrawCount\": "
       << shadowAgg.stage13ReplayDrawCount << ",\n";
  json << "    \"shadowTaaRequestedModeLast\": "
       << shadowAgg.shadowTaaRequestedModeLast << ",\n";
  json << "    \"shadowTaaEffectiveModeLast\": "
       << shadowAgg.shadowTaaEffectiveModeLast << ",\n";
  json << "    \"shadowTaaShaderModeLast\": "
       << shadowAgg.shadowTaaShaderModeLast << ",\n";
  json << "    \"semanticSceneReceiverReuseShadowMap\": "
       << shadowAgg.semanticSceneReceiverReuseShadowMap << ",\n";
  json << "    \"semanticSceneReceiverInputValid\": "
       << shadowAgg.semanticSceneReceiverInputValid << ",\n";
  json << "    \"semanticSceneReceiverInputRejectReason\": "
       << shadowAgg.semanticSceneReceiverInputRejectReason << ",\n";
  json << "    \"semanticSceneReceiverNeedPass\": "
       << shadowAgg.semanticSceneReceiverNeedPass << ",\n";
  json << "    \"semanticSceneReceiverNeedShadowMap\": "
       << shadowAgg.semanticSceneReceiverNeedShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasCompleteShadowMap\": "
       << shadowAgg.semanticSceneReceiverHasCompleteShadowMap << ",\n";
  json << "    \"semanticSceneReceiverHasUsableDirectionalShadow\": "
       << shadowAgg.semanticSceneReceiverHasUsableDirectionalShadow << ",\n";
  json << "    \"semanticSceneReceiverActiveStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverActiveStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverUboStrengthMilli\": "
       << shadowAgg.semanticSceneReceiverUboStrengthMilli << ",\n";
  json << "    \"semanticSceneReceiverDebugMode\": "
       << shadowAgg.semanticSceneReceiverDebugMode << ",\n";
  json << "    \"semanticSceneReceiverCsmCascadeCount\": "
       << shadowAgg.semanticSceneReceiverCsmCascadeCount << ",\n";
  json << "    \"semanticSceneReceiverZeroStrengthFrameCount\": "
       << shadowAgg.semanticSceneReceiverZeroStrengthFrameCount << ",\n";
  json << "    \"semanticSceneReceiverDrawnWithZeroStrengthCount\": "
       << shadowAgg.semanticSceneReceiverDrawnWithZeroStrengthCount << ",\n";
  json << "    \"semanticSceneReceiverNoCompleteShadowMapCount\": "
       << shadowAgg.semanticSceneReceiverNoCompleteShadowMapCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapImageCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapImageCount << ",\n";
  json << "    \"semanticSceneReceiverNoShadowMapSampleViewCount\": "
       << shadowAgg.semanticSceneReceiverNoShadowMapSampleViewCount << ",\n";
  json << "    \"semanticSceneReceiverNoCandidateCsmCount\": "
       << shadowAgg.semanticSceneReceiverNoCandidateCsmCount << ",\n";
  json << "    \"semanticSceneReceiverCsmFallbackToLastGoodCount\": "
       << shadowAgg.semanticSceneReceiverCsmFallbackToLastGoodCount << ",\n";
  json << "    \"semanticSceneReceiverHoldInvalidCsmCount\": "
       << shadowAgg.semanticSceneReceiverHoldInvalidCsmCount << ",\n";
  json << "    \"semanticSceneReceiverHoldEmptyReplayCount\": "
       << shadowAgg.semanticSceneReceiverHoldEmptyReplayCount << ",\n";
  json << "    \"semanticSceneReceiverHoldIdentityChurnCount\": "
       << shadowAgg.semanticSceneReceiverHoldIdentityChurnCount << ",\n";
  json << "    \"semanticSceneReceiverReuseInvalidatedAfterEnsureCount\": "
       << shadowAgg.semanticSceneReceiverReuseInvalidatedAfterEnsureCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoResourcesCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoResourcesCount
       << ",\n";
  json << "    \"semanticSceneShadowMapRenderSkippedNoMatrixBufferCount\": "
       << shadowAgg.semanticSceneShadowMapRenderSkippedNoMatrixBufferCount
       << ",\n";
  json << "    \"semanticSceneReceiverViewportX\": "
       << shadowAgg.semanticSceneReceiverViewportX << ",\n";
  json << "    \"semanticSceneReceiverViewportY\": "
       << shadowAgg.semanticSceneReceiverViewportY << ",\n";
  json << "    \"semanticSceneReceiverViewportWidth\": "
       << shadowAgg.semanticSceneReceiverViewportWidth << ",\n";
  json << "    \"semanticSceneReceiverViewportHeight\": "
       << shadowAgg.semanticSceneReceiverViewportHeight << ",\n";
  json << "    \"semanticSceneSkippedUnitsOnlyFilter\": "
       << shadowAgg.semanticSceneSkippedUnitsOnlyFilter << ",\n";
  json << "    \"semanticSceneAcceptedExplicitResourceOwnerRigid\": "
       << shadowAgg.semanticSceneAcceptedExplicitResourceOwnerRigid << ",\n";
  json << "    \"semanticSceneRejectedNoVertex\": "
       << shadowAgg.semanticSceneRejectedNoVertex << ",\n";
  json << "    \"semanticSceneRejectedSkinnedContract\": "
       << shadowAgg.semanticSceneRejectedSkinnedContract << ",\n";
  json << "    \"semanticSceneRejectedGeometry\": "
       << shadowAgg.semanticSceneRejectedGeometry << ",\n";
  json << "    \"semanticSceneRejectedGeometryFrameLocal\": "
       << shadowAgg.semanticSceneRejectedGeometryFrameLocal << ",\n";
  json << "    \"semanticSceneRejectedGeometryPersistent\": "
       << shadowAgg.semanticSceneRejectedGeometryPersistent << ",\n";
  json << "    \"persistentRejectNoIdentity\": "
       << shadowAgg.persistentRejectNoIdentity << ",\n";
  json << "    \"persistentRejectUnsupportedMode\": "
       << shadowAgg.persistentRejectUnsupportedMode << ",\n";
  json << "    \"persistentRejectDynamicSource\": "
       << shadowAgg.persistentRejectDynamicSource << ",\n";
  json << "    \"persistentRejectAlphaBlend\": "
       << shadowAgg.persistentRejectAlphaBlend << ",\n";
  json << "    \"persistentRejectMissingStorage\": "
       << shadowAgg.persistentRejectMissingStorage << ",\n";
  json << "    \"persistentRejectCreateOrBudget\": "
       << shadowAgg.persistentRejectCreateOrBudget << ",\n";
  json << "    \"persistentRejectCapacity\": "
       << shadowAgg.persistentRejectCapacity << ",\n";
  json << "    \"persistentRejectPositionBufferCreate\": "
       << shadowAgg.persistentRejectPositionBufferCreate << ",\n";
  json << "    \"persistentRejectIndexBufferCreate\": "
       << shadowAgg.persistentRejectIndexBufferCreate << ",\n";
  json << "    \"persistentRejectBlendBufferCreate\": "
       << shadowAgg.persistentRejectBlendBufferCreate << ",\n";
  json << "    \"persistentRejectUvBufferCreate\": "
       << shadowAgg.persistentRejectUvBufferCreate << ",\n";
  json << "    \"persistentRejectRegistryInsert\": "
       << shadowAgg.persistentRejectRegistryInsert << ",\n";
  json << "    \"persistentRejectOther\": "
       << shadowAgg.persistentRejectOther << ",\n";
  json << "    \"persistentRejectCreateOrBudgetDetailedTotal\": "
       << (shadowAgg.persistentRejectCapacity +
           shadowAgg.persistentRejectPositionBufferCreate +
           shadowAgg.persistentRejectIndexBufferCreate +
           shadowAgg.persistentRejectBlendBufferCreate +
           shadowAgg.persistentRejectUvBufferCreate +
           shadowAgg.persistentRejectRegistryInsert +
           shadowAgg.persistentRejectOther)
       << ",\n";
  json << "    \"persistentDiagnosticsFramesObserved\": "
       << shadowAgg.persistentDiagnosticsFramesObserved << ",\n";
  json << "    \"persistentCreateAttempts\": "
       << shadowAgg.persistentCreateAttempts << ",\n";
  json << "    \"persistentPoolBytesCap\": "
       << shadowAgg.persistentPoolBytesCapLast << ",\n";
  json << "    \"persistentPoolBytesUsed\": "
       << shadowAgg.persistentPoolBytesUsedLast << ",\n";
  json << "    \"persistentPoolBytesUsedMax\": "
       << shadowAgg.persistentPoolBytesUsedMax << ",\n";
  json << "    \"persistentPoolBytesUsedAverage\": "
       << (shadowAgg.persistentDiagnosticsFramesObserved != 0u
               ? static_cast<double>(
                     shadowAgg.persistentPoolBytesUsedGaugeSum) /
                     static_cast<double>(
                         shadowAgg.persistentDiagnosticsFramesObserved)
               : 0.0)
       << ",\n";
  json << "    \"persistentPoolBytesNeeded\": "
       << shadowAgg.persistentPoolBytesNeededLast << ",\n";
  json << "    \"persistentPoolBytesNeededMax\": "
       << shadowAgg.persistentPoolBytesNeededMax << ",\n";
  json << "    \"persistentPoolBytesNeededTotal\": "
       << shadowAgg.persistentPoolBytesNeededTotal << ",\n";
  json << "    \"persistentPoolBytesEvicted\": "
       << shadowAgg.persistentPoolBytesEvictedLast << ",\n";
  json << "    \"persistentPoolBytesEvictedDelta\": "
       << shadowAgg.persistentPoolBytesEvictedDelta << ",\n";
  json << "    \"persistentPoolLiveGeometryCount\": "
       << shadowAgg.persistentPoolLiveGeometryCountLast << ",\n";
  json << "    \"persistentPoolLiveGeometryCountMax\": "
       << shadowAgg.persistentPoolLiveGeometryCountMax << ",\n";
  json << "    \"persistentForceGcRequests\": "
       << shadowAgg.persistentForceGcRequests << ",\n";
  json << "    \"persistentForceGcNoBytesFreed\": "
       << shadowAgg.persistentForceGcNoBytesFreed << ",\n";
  json << "    \"persistentForceGcStillInsufficient\": "
       << shadowAgg.persistentForceGcStillInsufficient << ",\n";
  json << "    \"persistentForceGcBytesFreed\": "
       << shadowAgg.persistentForceGcBytesFreed << ",\n";
  json << "    \"persistentForceGcContract\": "
          "\"deprecated counters; incoming-allocation forced GC is disabled\",\n";
  json << "    \"persistentCapacityRejectAllCallers\": "
       << shadowAgg.persistentCapacityRejectAllCallers << ",\n";
  json << "    \"persistentCapacityFastReject\": "
       << shadowAgg.persistentCapacityFastReject << ",\n";
  json << "    \"persistentCapacityCounterContract\": "
          "\"rejectCapacity is ShadowCapture-only; capacityRejectAllCallers covers every caller; capacityFastReject is a ShadowCapture precheck subset and must not be added\",\n";
  json << "    \"persistentS1EarlyGaugeContract\": "
          "\"entry ownership gauges; logicalReferencedBytes is not unique or resident memory\",\n";
  json << "    \"persistentS1EarlyEntryCount\": "
       << shadowAgg.persistentS1EarlyEntryCountLast << ",\n";
  json << "    \"persistentS1EarlyEntryCountMax\": "
       << shadowAgg.persistentS1EarlyEntryCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentBackedCount\": "
       << shadowAgg.persistentS1EarlyPersistentBackedCountLast << ",\n";
  json << "    \"persistentS1EarlyPersistentBackedCountMax\": "
       << shadowAgg.persistentS1EarlyPersistentBackedCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexCount\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexCountLast << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexCountMax\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexCountMax << ",\n";
  json << "    \"persistentS1EarlyPersistentReverseIndexMismatchFrames\": "
       << shadowAgg.persistentS1EarlyPersistentReverseIndexMismatchFrames
       << ",\n";
  json << "    \"persistentS1EarlyFallbackBackedCount\": "
       << shadowAgg.persistentS1EarlyFallbackBackedCountLast << ",\n";
  json << "    \"persistentS1EarlyFallbackBackedCountMax\": "
       << shadowAgg.persistentS1EarlyFallbackBackedCountMax << ",\n";
  json << "    \"persistentS1EarlyLogicalReferencedBytes\": "
       << shadowAgg.persistentS1EarlyLogicalReferencedBytesLast << ",\n";
  json << "    \"persistentS1EarlyLogicalReferencedBytesMax\": "
       << shadowAgg.persistentS1EarlyLogicalReferencedBytesMax << ",\n";
  json << "    \"persistentS1EarlyReplayContract\": "
          "\"every accepted early-cache hit publishes exactly one canonical instance or fallback replay\",\n";
  json << "    \"persistentS1EarlyAcceptedHitCount\": "
       << shadowAgg.persistentS1EarlyAcceptedHitCount << ",\n";
  json << "    \"persistentS1EarlySourceMismatchEvictCount\": "
       << shadowAgg.persistentS1EarlySourceMismatchEvictCount << ",\n";
  json << "    \"persistentS1EarlyReplayPublishedCount\": "
       << shadowAgg.persistentS1EarlyReplayPublishedCount << ",\n";
  json << "    \"persistentS1EarlyReplayInstanceCount\": "
       << shadowAgg.persistentS1EarlyReplayInstanceCount << ",\n";
  json << "    \"persistentS1EarlyReplayFallbackCount\": "
       << shadowAgg.persistentS1EarlyReplayFallbackCount << ",\n";
  json << "    \"persistentS1EarlyReplayClosureMismatchFrames\": "
       << shadowAgg.persistentS1EarlyReplayClosureMismatchFrames << ",\n";
  json << "    \"semanticFallbackPruned\": "
       << shadowAgg.semanticFallbackPruned << ",\n";
  json << "    \"semanticFallbackPrunedByHandle\": "
       << shadowAgg.semanticFallbackPrunedByHandle << ",\n";
  json << "    \"semanticFallbackPrunedByWorldObjectEntry\": "
       << shadowAgg.semanticFallbackPrunedByWorldObjectEntry << ",\n";
  json << "    \"semanticFallbackPrunedBySceneNode\": "
       << shadowAgg.semanticFallbackPrunedBySceneNode << ",\n";
  json << "    \"semanticFallbackPrunedByRuntimeModel\": "
       << shadowAgg.semanticFallbackPrunedByRuntimeModel << ",\n";
  json << "    \"modelRegistryHit\": " << runtimeSummary.modelRegistryCount
       << ",\n";
  json << "    \"modelRegistryMiss\": 0,\n";
  json << "    \"modelLoadCount\": " << runtimeSummary.modelRegistryCount
       << ",\n";
  json << "    \"modelReuseCount\": " << runtimeSummary.instanceRegistryCount
       << ",\n";
  json << "    \"runtimeBoundCount\": " << runtimeSummary.runtimeBoundCount
       << ",\n";
  json << "    \"completeIdentityCount\": "
       << runtimeSummary.completeIdentityCount
       << ",\n";
  json << "    \"shadowRuntimeBoundCount\": "
       << runtimeSummary.shadowRuntimeBoundCount
       << ",\n";
  json << "    \"shadowIdentityCount\": " << runtimeSummary.shadowIdentityCount
       << ",\n";
  json << "    \"poseUpdateCount\": " << runtimeSummary.poseReadyCount
       << ",\n";
  json << "    \"poseCacheHit\": 0,\n";
  json << "    \"poseCacheMiss\": 0,\n";
  json << "    \"bonePaletteUpdates\": " << runtimeSummary.matrixPaletteCount
       << ",\n";
  json << "    \"shadowGeosetResourceCount\": "
       << runtimeSummary.shadowGeosetResourceCount << ",\n";
  json << "    \"shadowReadyGeosetCount\": "
       << runtimeSummary.shadowReadyGeosetCount << ",\n";
  json << "    \"shadowModelResourceCount\": "
       << runtimeSummary.shadowModelResourceCount << ",\n";
  json << "    \"shadowRuntimeModelCount\": "
       << runtimeSummary.shadowRuntimeModelCount << ",\n";
  json << "    \"visibleRenderableCount\": "
       << runtimeSummary.visibleRenderableCount << ",\n";
  json << "    \"visibleRenderableMainCount\": "
       << runtimeSummary.visibleRenderableMainCount << ",\n";
  json << "    \"visibleRenderableTransparentCount\": "
       << runtimeSummary.visibleRenderableTransparentCount << ",\n";
  json << "    \"semanticVisibleDirectUnitCandidateAccepted\": "
       << runtimeSummary.semanticVisibleDirectUnitCandidateAccepted << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNotUnitLike\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNotUnitLike << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedGroup\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedGroup << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoUnitPtr\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoUnitPtr << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoIdentity\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoIdentity << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoMesh\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoMesh << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedBuilding\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedBuilding << ",\n";
  json << "    \"semanticVisibleDirectUnitRejectedNoGeoset\": "
       << runtimeSummary.semanticVisibleDirectUnitRejectedNoGeoset << ",\n";
  // Phase 7.97 诊断：ManifestCopy 实际 scan/append 数量。
  json << "    \"semanticManifestCopyVisibleScanned\": "
       << runtimeSummary.semanticManifestCopyVisibleScanned << ",\n";
  json << "    \"semanticManifestCopyAppended\": "
       << runtimeSummary.semanticManifestCopyAppended << ",\n";
  json << "    \"semanticManifestCopyDeduplicatedSkipped\": "
       << runtimeSummary.semanticManifestCopyDeduplicatedSkipped << ",\n";
  json << "    \"semanticManifestCopyRejectedSkipped\": "
       << runtimeSummary.semanticManifestCopyRejectedSkipped << ",\n";
  json << "    \"semanticManifestCopySkipStableCount\": "
       << runtimeSummary.semanticManifestCopySkipStableCount << ",\n";
  json << "    \"semanticManifestCopyEnterCount\": "
       << runtimeSummary.semanticManifestCopyEnterCount << ",\n";
  json << "    \"semanticManifestCopyMaxScanned\": "
       << runtimeSummary.semanticManifestCopyMaxScanned << ",\n";
  json << "    \"semanticManifestCopyTotalScanned\": "
       << runtimeSummary.semanticManifestCopyTotalScanned << ",\n";
  json << "    \"semanticManifestCopyTotalChronoNs\": "
       << runtimeSummary.semanticManifestCopyTotalChronoNs << ",\n";
  json << "    \"semanticManifestCopyMaxChronoNs\": "
       << runtimeSummary.semanticManifestCopyMaxChronoNs << ",\n";
  json << "    \"semanticManifestResolveSourceCompleteSkipCount\": "
       << runtimeSummary.semanticManifestResolveSourceCompleteSkipCount
       << ",\n";
  json << "    \"semanticManifestResolveLegacyCacheHitCount\": "
       << runtimeSummary.semanticManifestResolveLegacyCacheHitCount << ",\n";
  json << "    \"semanticManifestResolveRawScanCount\": "
       << runtimeSummary.semanticManifestResolveRawScanCount << ",\n";
  json << "    \"semanticManifestResolveRawScanEntryVisitCount\": "
       << runtimeSummary.semanticManifestResolveRawScanEntryVisitCount
       << ",\n";
  json << "    \"semanticManifestResolveRawScanMissCount\": "
       << runtimeSummary.semanticManifestResolveRawScanMissCount << ",\n";
  json << "    \"semanticManifestResolveVerifierAttemptCount\": "
       << runtimeSummary.semanticManifestResolveVerifierAttemptCount
       << ",\n";
  json << "    \"semanticManifestResolveVerifierMismatchCount\": "
       << runtimeSummary.semanticManifestResolveVerifierMismatchCount
       << ",\n";
  json << "    \"semanticManifestResolveMaxRuntimeGeosetCount\": "
       << runtimeSummary.semanticManifestResolveMaxRuntimeGeosetCount
       << ",\n";
  json << "    \"semanticManifestModelResourceAttemptCount\": "
       << runtimeSummary.semanticManifestModelResourceAttemptCount << ",\n";
  json << "    \"semanticManifestModelResourceCacheHitCount\": "
       << runtimeSummary.semanticManifestModelResourceCacheHitCount << ",\n";
  json << "    \"semanticManifestModelResourceDeepResolveCount\": "
       << runtimeSummary.semanticManifestModelResourceDeepResolveCount
       << ",\n";
  json << "    \"semanticManifestModelResourceNullResultCount\": "
       << runtimeSummary.semanticManifestModelResourceNullResultCount
       << ",\n";
  json << "    \"semanticManifestModelResourceVerifierAttemptCount\": "
       << runtimeSummary.semanticManifestModelResourceVerifierAttemptCount
       << ",\n";
  json << "    \"semanticManifestModelResourceVerifierMismatchCount\": "
       << runtimeSummary.semanticManifestModelResourceVerifierMismatchCount
       << ",\n";
  json << "    \"semanticStaticCandidateCount\": "
       << runtimeSummary.semanticStaticCandidateCount << ",\n";
  json << "    \"semanticStaticCandidateBuildingCount\": "
       << runtimeSummary.semanticStaticCandidateBuildingCount << ",\n";
  json << "    \"semanticStaticCandidateDestructibleCount\": "
       << runtimeSummary.semanticStaticCandidateDestructibleCount << ",\n";
  json << "    \"semanticStaticCandidateMaybeDoodadOrEffectCount\": "
       << runtimeSummary.semanticStaticCandidateMaybeDoodadOrEffectCount
       << ",\n";
  json << "    \"semanticStaticCandidateWithStableIdentity\": "
       << runtimeSummary.semanticStaticCandidateWithStableIdentity << ",\n";
  json << "    \"semanticStaticCandidateWithMeshData\": "
       << runtimeSummary.semanticStaticCandidateWithMeshData << ",\n";
  json << "    \"semanticStaticCandidateWithRuntimeModel\": "
       << runtimeSummary.semanticStaticCandidateWithRuntimeModel << ",\n";
  json << "    \"semanticStaticCandidateWithModelResource\": "
       << runtimeSummary.semanticStaticCandidateWithModelResource << ",\n";
  json << "    \"semanticStaticCandidateWithResolvedGeoset\": "
       << runtimeSummary.semanticStaticCandidateWithResolvedGeoset << ",\n";
  json << "    \"semanticStaticCandidateRejectedUnitsOnlyFilter\": "
       << runtimeSummary.semanticStaticCandidateRejectedUnitsOnlyFilter
       << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoIdentity\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoIdentity << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoMeshData\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoMeshData << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoResource\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoResource << ",\n";
  json << "    \"semanticStaticCandidateRejectedNoGeoset\": "
       << runtimeSummary.semanticStaticCandidateRejectedNoGeoset << ",\n";
  json << "    \"semanticStaticCandidateRejectedNonCanonicalKind\": "
       << runtimeSummary.semanticStaticCandidateRejectedNonCanonicalKind
       << ",\n";
  json << "    \"shadowPoseReadyCount\": "
       << runtimeSummary.shadowPoseReadyCount << ",\n";
  json << "    \"upperLayerResolveAttempts\": "
       << runtimeSummary.upperLayerResolveAttempts << ",\n";
  json << "    \"upperLayerResolveVisibleMiss\": "
       << runtimeSummary.upperLayerResolveVisibleMiss << ",\n";
  json << "    \"upperLayerResolveVisibleUnresolvedGeoset\": "
       << runtimeSummary.upperLayerResolveVisibleUnresolvedGeoset << ",\n";
  json << "    \"upperLayerResolveGeosetMiss\": "
       << runtimeSummary.upperLayerResolveGeosetMiss << ",\n";
  json << "    \"upperLayerResolvePoseMiss\": "
       << runtimeSummary.upperLayerResolvePoseMiss << ",\n";
  json << "    \"upperLayerResolveRuntimeGroupPaletteMiss\": "
       << runtimeSummary.upperLayerResolveRuntimeGroupPaletteMiss << ",\n";
  json << "    \"upperLayerResolveAuthoritativeRigid\": "
       << runtimeSummary.upperLayerResolveAuthoritativeRigid << ",\n";
  json << "    \"upperLayerResolveAuthoritativeSkinned\": "
       << runtimeSummary.upperLayerResolveAuthoritativeSkinned << ",\n";
  json << "    \"upperLayerResolvedAuthoritativeItems\": "
       << runtimeSummary.upperLayerResolvedAuthoritativeItems << ",\n";
  json << "    \"upperLayerEmitted\": "
       << runtimeSummary.upperLayerEmitted << ",\n";
  json << "    \"upperLayerDuplicateOrSuppressed\": "
       << runtimeSummary.upperLayerDuplicateOrSuppressed << ",\n";
  json << "    \"semanticCoreManifestFrameSerial\": "
       << runtimeSummary.semanticCoreManifestFrameSerial << ",\n";
  json << "    \"semanticCoreFrameSerial\": "
       << runtimeSummary.semanticCoreFrameSerial << ",\n";
  json << "    \"semanticCoreFrameLag\": "
       << runtimeSummary.semanticCoreFrameLag << ",\n";
  json << "    \"semanticCoreFrameFresh\": "
       << (runtimeSummary.semanticCoreFrameFresh ? "true" : "false") << ",\n";
  json << "    \"semanticCoreConsidered\": "
       << runtimeSummary.semanticCoreConsidered << ",\n";
  json << "    \"semanticCoreResolved\": "
       << runtimeSummary.semanticCoreResolved << ",\n";
  json << "    \"semanticCoreRigidResolved\": "
       << runtimeSummary.semanticCoreRigidResolved << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidResolved\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidResolved
       << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidWorldTransformResolved\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidWorldTransformResolved
       << ",\n";
  json << "    \"semanticCoreExplicitResourceOwnerRigidNoMatrixPalette\": "
       << runtimeSummary.semanticCoreExplicitResourceOwnerRigidNoMatrixPalette
       << ",\n";
  json << "    \"semanticCoreSkinnedResolved\": "
       << runtimeSummary.semanticCoreSkinnedResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendAttempts\": "
       << runtimeSummary.semanticCoreExplicitBlendAttempts << ",\n";
  json << "    \"semanticCoreExplicitBlendAttemptWithSpanRemapTable\": "
       << runtimeSummary.semanticCoreExplicitBlendAttemptWithSpanRemapTable
       << ",\n";
  json << "    \"semanticCoreExplicitBlendResolved\": "
       << runtimeSummary.semanticCoreExplicitBlendResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendSpanRemapResolved\": "
       << runtimeSummary.semanticCoreExplicitBlendSpanRemapResolved << ",\n";
  json << "    \"semanticCoreExplicitBlendStrideSearchMiss\": "
       << runtimeSummary.semanticCoreExplicitBlendStrideSearchMiss << ",\n";
  json << "    \"semanticCoreExplicitBlendFinalDecodeMiss\": "
       << runtimeSummary.semanticCoreExplicitBlendFinalDecodeMiss << ",\n";
  json << "    \"semanticCoreCoreDrawPacketCount\": "
       << runtimeSummary.semanticCoreCoreDrawPacketCount << ",\n";
  json << "    \"semanticCoreUpperLayerResolvedItems\": "
       << runtimeSummary.semanticCoreUpperLayerResolvedItems << ",\n";
  json << "    \"semanticCoreSupplementalUpperLayerDrawPacketCount\": "
       << runtimeSummary.semanticCoreSupplementalUpperLayerDrawPacketCount
       << ",\n";
  json << "    \"semanticCoreDrawPacketCount\": "
       << runtimeSummary.semanticCoreDrawPacketCount << ",\n";
  json << "    \"semanticCoreSubmittedDrawCount\": "
       << runtimeSummary.semanticCoreSubmittedDrawCount << ",\n";
  json << "    \"semanticCoreSkippedNoIdentity\": "
       << runtimeSummary.semanticCoreSkippedNoIdentity << ",\n";
  json << "    \"semanticCoreSkippedNoResolvedGeoset\": "
       << runtimeSummary.semanticCoreSkippedNoResolvedGeoset << ",\n";
  json << "    \"semanticCoreSkippedNoGeoset\": "
       << runtimeSummary.semanticCoreSkippedNoGeoset << ",\n";
  json << "    \"semanticCoreSkippedResourceMiss\": "
       << runtimeSummary.semanticCoreSkippedResourceMiss << ",\n";
  json << "    \"semanticCoreSkippedResourceNotReady\": "
       << runtimeSummary.semanticCoreSkippedResourceNotReady << ",\n";
  json << "    \"semanticCoreSkippedNoPose\": "
       << runtimeSummary.semanticCoreSkippedNoPose << ",\n";
  json << "    \"semanticCoreSkippedNoPoseNoContext\": "
       << runtimeSummary.semanticCoreSkippedNoPoseNoContext << ",\n";
  json << "    \"semanticCoreSkippedNoPoseAnonymousSubpart\": "
       << runtimeSummary.semanticCoreSkippedNoPoseAnonymousSubpart << ",\n";
  json << "    \"semanticCoreSkippedNoPoseLookupMiss\": "
       << runtimeSummary.semanticCoreSkippedNoPoseLookupMiss << ",\n";
  json << "    \"semanticCoreSkippedNoRuntimeGroupPalette\": "
       << runtimeSummary.semanticCoreSkippedNoRuntimeGroupPalette << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByMeshPoseContext
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteRescueByResourceMatchedPose
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByRuntimeRoot
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByChildRuntime\": "
       << runtimeSummary.semanticCoreRuntimeGroupPaletteRescueByChildRuntime
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteRescueByDescendantRuntime
       << ",\n";
  json << "    \"semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed\": "
       << runtimeSummary
              .semanticCoreRuntimeGroupPaletteResourceMatchedPoseSuppressed
       << ",\n";
  json << "    \"animationSequenceCount\": 0,\n";
  json << "    \"avgModelResolveCpuMs\": 0.0,\n";
  json << "    \"avgPoseUpdateCpuMs\": 0.0,\n";
  json << "    \"avgSkinnedOutputCpuMs\": 0.0,\n";
  json << "    \"modelResourceBytes\": 0,\n";
  json << "    \"poseResourceBytes\": 0,\n";
  json << "    \"spriteFramePoseCount\": "
       << runtimeSummary.spriteFramePoseCount << ",\n";
  json << "    \"semanticAugmentTlsCacheEnabled\": "
       << boolJson(semanticAugmentCache.enabled) << ",\n";
  json << "    \"semanticAugmentTlsCacheTelemetryEnabled\": "
       << boolJson(semanticAugmentCache.telemetryEnabled) << ",\n";
  json << "    \"semanticAugmentTlsCacheCapacityPerRegistry\": "
       << semanticAugmentCache.capacityPerRegistry << ",\n";
  json << "    \"semanticAugmentTlsCacheStatsContract\": "
          "\"process-lifetime counters; enable explicitly; do not add to "
          "frame CPU scopes\",\n";
  json << "    \"semanticAugmentModelLookups\": "
       << semanticAugmentCache.modelLookups << ",\n";
  json << "    \"semanticAugmentModelHits\": "
       << semanticAugmentCache.modelHits << ",\n";
  json << "    \"semanticAugmentModelNegativeHits\": "
       << semanticAugmentCache.modelNegativeHits << ",\n";
  json << "    \"semanticAugmentModelMisses\": "
       << semanticAugmentCache.modelMisses << ",\n";
  json << "    \"semanticAugmentModelGenerationMismatches\": "
       << semanticAugmentCache.modelGenerationMismatches << ",\n";
  json << "    \"semanticAugmentModelCollisions\": "
       << semanticAugmentCache.modelCollisions << ",\n";
  json << "    \"semanticAugmentShadowLookups\": "
       << semanticAugmentCache.shadowLookups << ",\n";
  json << "    \"semanticAugmentShadowHits\": "
       << semanticAugmentCache.shadowHits << ",\n";
  json << "    \"semanticAugmentShadowNegativeHits\": "
       << semanticAugmentCache.shadowNegativeHits << ",\n";
  json << "    \"semanticAugmentShadowMisses\": "
       << semanticAugmentCache.shadowMisses << ",\n";
  json << "    \"semanticAugmentShadowGenerationMismatches\": "
       << semanticAugmentCache.shadowGenerationMismatches << ",\n";
  json << "    \"semanticAugmentShadowCollisions\": "
       << semanticAugmentCache.shadowCollisions << ",\n";
  json << "    \"semanticAugmentModelRegistryGeneration\": "
       << semanticAugmentCache.modelRegistryGeneration << ",\n";
  json << "    \"semanticAugmentShadowRegistryGeneration\": "
       << semanticAugmentCache.shadowRegistryGeneration << ",\n";
  json << "    \"frameSerial\": " << shadowAgg.framesObserved << ",\n";
  json << "    \"notes\": \"runtime-shadow-bridge-v1\"\n";
  json << "  },\n";

  const auto safeCopyVerifier =
      dxvk::war3::hooks::QueryPublishVisibleSafeCopyVerifierStats();
  json << "  \"publishVisibleSafeCopyVerifier\": {\n";
  json << "    \"productionEnabled\": "
       << boolJson(safeCopyVerifier.productionEnabled) << ",\n";
  json << "    \"verifierEnabled\": "
       << boolJson(safeCopyVerifier.enabled) << ",\n";
  json << "    \"assertOnMismatch\": "
       << boolJson(safeCopyVerifier.assertOnMismatch) << ",\n";
  json << "    \"samplePeriod\": "
       << safeCopyVerifier.samplePeriod << ",\n";
  json << "    \"statsContract\": "
          "\"process-lifetime sampled counters; ordinary TLS batches flush "
          "every 64 attempts and export flushes the current thread; any "
          "initial mismatch flushes immediately, so zero mismatch is not "
          "hidden by TLS residuals\",\n";
  json << "    \"attempts\": " << safeCopyVerifier.attempts << ",\n";
  json << "    \"scenePreset\": "
       << safeCopyVerifier.scenePreset << ",\n";
  json << "    \"sceneNullInput\": "
       << safeCopyVerifier.sceneNullInput << ",\n";
  json << "    \"copySuccess\": "
       << safeCopyVerifier.copySuccess << ",\n";
  json << "    \"copyFailure\": "
       << safeCopyVerifier.copyFailure << ",\n";
  json << "    \"candidateEarlyReturn\": "
       << safeCopyVerifier.candidateEarlyReturn << ",\n";
  json << "    \"legacyEarlyReturn\": "
       << safeCopyVerifier.legacyEarlyReturn << ",\n";
  json << "    \"rereadCount\": "
       << safeCopyVerifier.rereadCount << ",\n";
  json << "    \"initialMismatchCount\": "
       << safeCopyVerifier.initialMismatchCount << ",\n";
  json << "    \"stableMatchCount\": "
       << safeCopyVerifier.stableMismatchMaskCounts[
              dxvk::war3::hooks::PublishVisibleSafeCopyMismatchNone]
       << ",\n";
  json << "    \"stableMismatchCount\": "
       << safeCopyVerifier.stableMismatchCount << ",\n";
  json << "    \"unstable\": "
       << safeCopyVerifier.unstableCount << ",\n";
  json << "    \"initialMismatchMaskOr\": "
       << safeCopyVerifier.initialMismatchMaskOr << ",\n";
  json << "    \"stableMismatchMaskOr\": "
       << safeCopyVerifier.stableMismatchMaskOr << ",\n";
  json << "    \"mismatchMaskLegend\": "
          "{\"meshData\": 1, \"sceneNode\": 2, \"earlyReturn\": 4},\n";
  json << "    \"initialMismatchMaskCounts\": [";
  for (size_t i = 0u;
       i < safeCopyVerifier.initialMismatchMaskCounts.size(); ++i) {
    if (i != 0u)
      json << ", ";
    json << safeCopyVerifier.initialMismatchMaskCounts[i];
  }
  json << "],\n";
  json << "    \"stableMismatchMaskCounts\": [";
  for (size_t i = 0u;
       i < safeCopyVerifier.stableMismatchMaskCounts.size(); ++i) {
    if (i != 0u)
      json << ", ";
    json << safeCopyVerifier.stableMismatchMaskCounts[i];
  }
  json << "]\n";
  json << "  },\n";

  // These counters already exist in the semantic producers, but older reports
  // never surfaced them. They are process-lifetime accumulators (not report
  // window deltas), so export total and per-call cost without pretending they
  // are additive Present-frame scopes.
  struct SemanticPerfHotspotRow {
    const char* group;
    const char* name;
    uint64_t calls;
    uint64_t totalUs;
  };
  const SemanticPerfHotspotRow semanticPerfHotspots[] = {
      {"Core", "ModelHook", runtimeSummary.semanticModelHookCalls,
       runtimeSummary.semanticModelHookUs},
      {"Core", "PoseHook", runtimeSummary.semanticPoseHookCalls,
       runtimeSummary.semanticPoseHookUs},
      {"Core", "AttachmentHook", runtimeSummary.semanticAttachmentHookCalls,
       runtimeSummary.semanticAttachmentHookUs},
      {"Core", "FrameRegistryPublish",
       runtimeSummary.semanticFrameRegistryPublishCalls,
       runtimeSummary.semanticFrameRegistryPublishUs},
      {"Core", "ContractCapture", runtimeSummary.semanticContractCaptureCalls,
       runtimeSummary.semanticContractCaptureUs},
      {"Core", "ConsumerBuild", runtimeSummary.semanticConsumerBuildCalls,
       runtimeSummary.semanticConsumerBuildUs},
      {"Core", "SummaryRefreshRequest",
       runtimeSummary.semanticSummaryRefreshRequestCalls,
       runtimeSummary.semanticSummaryRefreshRequestUs},
      {"ShadowMetadata", "MetadataCapture",
       runtimeSummary.shadowMetadataCaptureFrameCalls,
       runtimeSummary.shadowMetadataCaptureUs},
      {"Model", "BuildChildPreScan",
       runtimeSummary.semanticModelBuildChildPreScanCalls,
       runtimeSummary.semanticModelBuildChildPreScanUs},
      {"Model", "RuntimeChildCollect",
       runtimeSummary.semanticModelRuntimeChildCollectCalls,
       runtimeSummary.semanticModelRuntimeChildCollectUs},
      {"Model", "RuntimeChildBootstrap",
       runtimeSummary.semanticModelRuntimeChildBootstrapCalls,
       runtimeSummary.semanticModelRuntimeChildBootstrapUs},
      {"Model", "RuntimeChildParentMap",
       runtimeSummary.semanticModelRuntimeChildParentMapCalls,
       runtimeSummary.semanticModelRuntimeChildParentMapUs},
      {"Model", "RuntimeChildOwnerPropagate",
       runtimeSummary.semanticModelRuntimeChildOwnerPropagateCalls,
       runtimeSummary.semanticModelRuntimeChildOwnerPropagateUs},
      {"Model", "PromoteRuntime",
       runtimeSummary.semanticModelPromoteRuntimeCalls,
       runtimeSummary.semanticModelPromoteRuntimeUs},
      {"Model", "SpriteHostBind",
       runtimeSummary.semanticModelSpriteHostBindCalls,
       runtimeSummary.semanticModelSpriteHostBindUs},
      {"Model", "RuntimeModelBinding",
       runtimeSummary.semanticModelRuntimeModelBindingCalls,
       runtimeSummary.semanticModelRuntimeModelBindingUs},
      {"Model", "GeosetResource",
       runtimeSummary.semanticModelGeosetResourceCalls,
       runtimeSummary.semanticModelGeosetResourceUs},
      {"Model", "RuntimeCtor", runtimeSummary.semanticModelRuntimeCtorCalls,
       runtimeSummary.semanticModelRuntimeCtorUs},
      {"Model", "RuntimeResolve",
       runtimeSummary.semanticModelRuntimeResolveCalls,
       runtimeSummary.semanticModelRuntimeResolveUs},
      {"Model", "RuntimeInitCopy",
       runtimeSummary.semanticModelRuntimeInitCopyCalls,
       runtimeSummary.semanticModelRuntimeInitCopyUs},
      {"Pose", "RuntimePose", runtimeSummary.semanticPoseRuntimePoseCalls,
       runtimeSummary.semanticPoseRuntimePoseUs},
      {"Pose", "RuntimePaletteTree",
       runtimeSummary.semanticPoseRuntimePaletteTreeCalls,
       runtimeSummary.semanticPoseRuntimePaletteTreeUs},
      {"Pose", "RuntimeMatrixPalette",
       runtimeSummary.semanticPoseRuntimeMatrixPaletteCalls,
       runtimeSummary.semanticPoseRuntimeMatrixPaletteUs},
      {"Pose", "SpriteFrameSourceIdentity",
       runtimeSummary.semanticPoseSpriteFrameSourceIdentityCalls,
       runtimeSummary.semanticPoseSpriteFrameSourceIdentityUs},
      {"Pose", "SpriteFramePose",
       runtimeSummary.semanticPoseSpriteFramePoseCalls,
       runtimeSummary.semanticPoseSpriteFramePoseUs},
      {"Pose", "RuntimeMatrixPublisher",
       runtimeSummary.semanticPoseRuntimeMatrixPublisherCalls,
       runtimeSummary.semanticPoseRuntimeMatrixPublisherUs},
      {"Pose", "SpriteAttachmentHit",
       runtimeSummary.semanticPoseSpriteAttachmentHitCalls,
       runtimeSummary.semanticPoseSpriteAttachmentHitUs},
      {"Pose", "SpriteTransformRead",
       runtimeSummary.semanticPoseSpriteTransformReadCalls,
       runtimeSummary.semanticPoseSpriteTransformReadUs},
      {"Pose", "SpriteIdentityLookup",
       runtimeSummary.semanticPoseSpriteIdentityLookupCalls,
       runtimeSummary.semanticPoseSpriteIdentityLookupUs},
      {"Pose", "SpriteBaseAlias",
       runtimeSummary.semanticPoseSpriteBaseAliasCalls,
       runtimeSummary.semanticPoseSpriteBaseAliasUs},
      {"Pose", "SpritePublishPose",
       runtimeSummary.semanticPoseSpritePublishPoseCalls,
       runtimeSummary.semanticPoseSpritePublishPoseUs},
      {"Pose", "SpritePaletteGate",
       runtimeSummary.semanticPoseSpritePaletteGateCalls,
       runtimeSummary.semanticPoseSpritePaletteGateUs},
      {"Attachment", "AttachedEffectInit",
       runtimeSummary.semanticAttachmentAttachedEffectInitCalls,
       runtimeSummary.semanticAttachmentAttachedEffectInitUs},
      {"Attachment", "AttachedEffectDirect",
       runtimeSummary.semanticAttachmentAttachedEffectDirectCalls,
       runtimeSummary.semanticAttachmentAttachedEffectDirectUs},
      {"Attachment", "AttachModelToPoint",
       runtimeSummary.semanticAttachmentAttachModelToPointCalls,
       runtimeSummary.semanticAttachmentAttachModelToPointUs},
      {"Attachment", "OverrideSharedPreset",
       runtimeSummary.semanticAttachmentOverrideSharedPresetCalls,
       runtimeSummary.semanticAttachmentOverrideSharedPresetUs},
      {"Attachment", "OverrideLocalPoint",
       runtimeSummary.semanticAttachmentOverrideLocalPointCalls,
       runtimeSummary.semanticAttachmentOverrideLocalPointUs},
      {"Attachment", "OverridePrimaryPreset",
       runtimeSummary.semanticAttachmentOverridePrimaryPresetCalls,
       runtimeSummary.semanticAttachmentOverridePrimaryPresetUs},
  };
  json << "  \"semanticPerfHotspots\": [\n";
  for (size_t i = 0; i < std::size(semanticPerfHotspots); ++i) {
    const auto& row = semanticPerfHotspots[i];
    if (i != 0u)
      json << ",\n";
    const double avgUs = row.calls != 0u
                             ? static_cast<double>(row.totalUs) /
                                   static_cast<double>(row.calls)
                             : 0.0;
    json << "    {\"group\": \"" << row.group << "\", \"name\": \""
         << row.name << "\", \"calls\": " << row.calls
         << ", \"totalUs\": " << row.totalUs << ", \"avgUsPerCall\": "
         << avgUs << "}";
  }
  json << "\n  ],\n";

  // 帧时间线（CPU/GPU）
  json << "  \"frameTimes\": [";
  bool first = true;
  for (const auto *f : frames) {
    if (!first)
      json << ",";
    json << f->totalCpuMs;
    first = false;
  }
  json << "],\n";
  json << "  \"frameTimesGpu\": [";
  first = true;
  for (const auto *f : frames) {
    if (!first)
      json << ",";
    if (f->hasGpuTiming)
      json << f->totalGpuMs;
    else
      json << "null";
    first = false;
  }
  json << "],\n";

  // Present-boundary workload gauges. Arrays keep a 3600-frame report compact
  // and can be joined to frameSeries/frameStateAnalysis by epoch.
  json << "  \"workloadSeriesColumns\": [\"epoch\", "
          "\"businessFrameSerial\", \"hasShadowBudget\", "
          "\"capturedDrawCount\", "
          "\"shadowMetadataCaptureCalls\", "
          "\"shadowMetadataCaptureUs\", "
          "\"terrainDoodadCaptureAttemptCount\", "
          "\"terrainDoodadCaptureAcceptedCount\", "
          "\"terrainDoodadDynamicSourceCount\", "
          "\"terrainDoodadWorldIdentityLikeCount\", "
          "\"terrainDoodadWorldNonIdentityCount\", "
          "\"terrainS1CaptureAttemptCount\", "
          "\"terrainS1CaptureAcceptedCount\", "
          "\"terrainS1WorldIdentityLikeCount\", "
          "\"terrainS1WorldNonIdentityCount\", "
          "\"terrainS1WorldNonFiniteCount\", "
          "\"terrainS1ForceIdentityWorldCount\", "
          "\"terrainS1WorldMatrixHash\", "
          "\"terrainS1WorldTranslationMilliMax\", "
          "\"stage13CaptureAttemptCount\", "
          "\"stage13CaptureRejectedNoDemandCount\", "
          "\"stage13CaptureRejectedAfterBeforeUiCount\", "
          "\"stage13CaptureConsideredCount\", "
          "\"stage13FreezeCopyBytes\", "
          "\"stage13CpuSnapshotCopyBytes\", "
          "\"stage13RetentionSnapshotBytes\", "
          "\"stage13ReplayDrawCount\", "
          "\"receiverCameraDeltaNano\", "
          "\"receiverSunDeltaNano\", "
          "\"receiverCsmDeltaNano\", "
          "\"receiverSnappedCenterDeltaTexelsNano\", "
          "\"receiverTexelSizeDeltaNano\", "
          "\"shadowMapRenderSerial\", "
          "\"uniqueGeometryCount\", "
          "\"staticPersistentCount\", \"dynamicPoseCount\", "
          "\"dynamicSkinnedOutputCount\", \"fallbackDrawCount\", "
          "\"drawTimeVBCacheCaptureCount\", "
          "\"drawTimeVBCacheConsumeHitCount\", "
          "\"drawTimeVBCacheConsumeMissCount\", "
          "\"drawTimeVBCacheRejectNoLayerContext\", "
          "\"drawTimeVBCacheRejectContractLookup\", "
          "\"drawTimeVBCacheRejectContractFreshness\", "
          "\"drawTimeVBCacheRejectContractStage\", "
          "\"drawTimeVBCacheRejectContractRenderFrame\", "
          "\"drawTimeVBCacheRejectContractInstance\", "
          "\"drawTimeVBCacheRejectContractSlice\", "
          "\"drawTimeVBCacheSameFrameDedupMiss\", "
          "\"semanticSceneSubmitted\", \"semanticSceneSubmittedSkinned\", "
          "\"skippedCasterCap\", \"skippedDistanceCull\", "
          "\"hasShadowReceiver\", \"replayCasterCount\", "
          "\"replayGeometryWork\", \"hasShadowTaaTelemetry\", "
          "\"shadowTaaRuntimeModuleEnabled\", "
          "\"shadowTaaRequestedMode\", \"shadowTaaEffectiveMode\", "
          "\"shadowTaaShaderMode\", "
          "\"shadowTaaBlockedSemanticDynamic\", "
          "\"shadowTaaBlockedSunMotion\", "
          "\"shadowTaaBlockedCsmFallback\", "
          "\"shadowTaaVisibilityExecuted\", "
          "\"shadowTaaMotionVectorExecuted\", "
          "\"shadowTaaReceiverExecuted\", "
          "\"shadowTaaHistoryWriteExecuted\", "
          "\"shadowTaaHistoryAdvanced\", "
          "\"shadowTaaHistoryAdvanceSkippedIncomplete\", "
          "\"shadowTaaHistoryValidBefore\", "
          "\"shadowTaaHistoryValidAfter\", "
          "\"shadowTaaHistoryInvalidationMask\", "
          "\"terrainDoodadPreparedCount\", \"terrainS1PreparedCount\", "
          "\"terrainDoodadCascade0DrawnCount\", "
          "\"terrainDoodadCascade1DrawnCount\", "
          "\"terrainDoodadCascade2DrawnCount\", "
          "\"terrainDoodadCascade3DrawnCount\", "
          "\"terrainS1Cascade0DrawnCount\", "
          "\"terrainS1Cascade1DrawnCount\", "
          "\"terrainS1Cascade2DrawnCount\", "
          "\"terrainS1Cascade3DrawnCount\", "
          "\"requestedShadowResolution\", "
          "\"effectiveShadowResolution\", \"reusedLastCompleteShadowMap\", "
          "\"renderedCurrentPartialShadowMap\", "
          "\"hasPersistentGeometry\", \"rejectCapacity\", "
          "\"rejectPositionBufferCreate\", \"rejectIndexBufferCreate\", "
          "\"rejectBlendBufferCreate\", \"rejectUvBufferCreate\", "
          "\"rejectRegistryInsert\", \"rejectOther\", \"createAttempts\", "
          "\"bytesNeededTotal\", \"bytesNeededMax\", \"bytesNeededLast\", "
          "\"forceGcRequests\", \"forceGcNoBytesFreed\", "
          "\"forceGcStillInsufficient\", \"forceGcBytesFreed\", "
          "\"capacityRejectAllCallers\", \"capacityFastReject\", "
          "\"expiryTokensPopped\", \"expiryTokensRequeued\", "
          "\"expiryStaleTokens\", \"expiryAgeEvictions\", "
          "\"expiryQueueSize\", "
          "\"poolBytesCap\", \"poolBytesUsed\", \"poolBytesEvicted\", "
          "\"poolBytesEvictedDelta\", \"liveGeometryCount\", "
          "\"s1EarlyEntryCount\", \"s1EarlyPersistentBackedCount\", "
          "\"s1EarlyPersistentReverseIndexCount\", "
          "\"s1EarlyFallbackBackedCount\", "
          "\"s1EarlyLogicalReferencedBytes\", "
          "\"s1EarlyAcceptedHitCount\", "
          "\"s1EarlyReplayPublishedCount\", "
          "\"s1EarlyReplayInstanceCount\", "
          "\"s1EarlyReplayFallbackCount\", "
          "\"s1EarlySourceMismatchEvictCount\", "
          "\"s1EarlyReplayClosureMismatch\"],\n";
  json << "  \"workloadSeries\": [\n";
  first = true;
  for (const auto* f : frames) {
    const auto& w = f->workload;
    if (!first)
      json << ",\n";
    first = false;
    json << "    [" << f->frameEpoch << ", "
         << w.businessFrameSerial << ", "
         << (w.hasShadowBudget ? 1 : 0) << ", "
         << w.capturedDrawCount << ", "
         << w.shadowMetadataCaptureCalls << ", "
         << w.shadowMetadataCaptureUs << ", "
         << w.terrainDoodadCaptureAttemptCount << ", "
         << w.terrainDoodadCaptureAcceptedCount << ", "
         << w.terrainDoodadDynamicSourceCount << ", "
         << w.terrainDoodadWorldIdentityLikeCount << ", "
         << w.terrainDoodadWorldNonIdentityCount << ", "
         << w.terrainS1CaptureAttemptCount << ", "
         << w.terrainS1CaptureAcceptedCount << ", "
         << w.terrainS1WorldIdentityLikeCount << ", "
         << w.terrainS1WorldNonIdentityCount << ", "
         << w.terrainS1WorldNonFiniteCount << ", "
         << w.terrainS1ForceIdentityWorldCount << ", "
         << w.terrainS1WorldMatrixHash << ", "
         << w.terrainS1WorldTranslationMilliMax << ", "
         << w.stage13CaptureAttemptCount << ", "
         << w.stage13CaptureRejectedNoDemandCount << ", "
         << w.stage13CaptureRejectedAfterBeforeUiCount << ", "
         << w.stage13CaptureConsideredCount << ", "
         << w.stage13FreezeCopyBytes << ", "
         << w.stage13CpuSnapshotCopyBytes << ", "
         << w.stage13RetentionSnapshotBytes << ", "
         << w.stage13ReplayDrawCount << ", "
         << w.receiverCameraDeltaNano << ", "
         << w.receiverSunDeltaNano << ", "
         << w.receiverCsmDeltaNano << ", "
         << w.receiverSnappedCenterDeltaTexelsNano << ", "
         << w.receiverTexelSizeDeltaNano << ", "
         << w.shadowMapRenderSerial << ", "
         << w.uniqueGeometryCount << ", "
         << w.staticPersistentCount << ", " << w.dynamicPoseCount << ", "
         << w.dynamicSkinnedOutputCount << ", " << w.fallbackDrawCount
         << ", " << w.drawTimeVBCacheCaptureCount << ", "
         << w.drawTimeVBCacheConsumeHitCount << ", "
         << w.drawTimeVBCacheConsumeMissCount << ", "
         << w.drawTimeVBCacheRejectNoLayerContext << ", "
         << w.drawTimeVBCacheRejectContractLookup << ", "
         << w.drawTimeVBCacheRejectContractFreshness << ", "
         << w.drawTimeVBCacheRejectContractStage << ", "
         << w.drawTimeVBCacheRejectContractRenderFrame << ", "
         << w.drawTimeVBCacheRejectContractInstance << ", "
         << w.drawTimeVBCacheRejectContractSlice << ", "
         << w.drawTimeVBCacheSameFrameDedupMiss << ", "
         << w.semanticSceneSubmitted << ", "
         << w.semanticSceneSubmittedSkinned << ", " << w.skippedCasterCap
         << ", " << w.skippedDistanceCull << ", "
         << (w.hasShadowReceiver ? 1 : 0) << ", " << w.replayCasterCount
         << ", " << w.replayGeometryWork << ", "
         << (w.hasShadowTaaTelemetry ? 1 : 0) << ", "
         << w.shadowTaaRuntimeModuleEnabled << ", "
         << w.shadowTaaRequestedMode << ", "
         << w.shadowTaaEffectiveMode << ", "
         << w.shadowTaaShaderMode << ", "
         << w.shadowTaaBlockedSemanticDynamic << ", "
         << w.shadowTaaBlockedSunMotion << ", "
         << w.shadowTaaBlockedCsmFallback << ", "
         << w.shadowTaaVisibilityExecuted << ", "
         << w.shadowTaaMotionVectorExecuted << ", "
         << w.shadowTaaReceiverExecuted << ", "
         << w.shadowTaaHistoryWriteExecuted << ", "
         << w.shadowTaaHistoryAdvanced << ", "
         << w.shadowTaaHistoryAdvanceSkippedIncomplete << ", "
         << w.shadowTaaHistoryValidBefore << ", "
         << w.shadowTaaHistoryValidAfter << ", "
         << w.shadowTaaHistoryInvalidationMask << ", "
         << w.terrainDoodadPreparedCount << ", "
         << w.terrainS1PreparedCount << ", "
         << w.terrainDoodadCascade0DrawnCount << ", "
         << w.terrainDoodadCascade1DrawnCount << ", "
         << w.terrainDoodadCascade2DrawnCount << ", "
         << w.terrainDoodadCascade3DrawnCount << ", "
         << w.terrainS1Cascade0DrawnCount << ", "
         << w.terrainS1Cascade1DrawnCount << ", "
         << w.terrainS1Cascade2DrawnCount << ", "
         << w.terrainS1Cascade3DrawnCount << ", "
         << w.requestedShadowResolution << ", "
         << w.effectiveShadowResolution << ", "
         << (w.reusedLastCompleteShadowMap ? 1 : 0) << ", "
         << (w.renderedCurrentPartialShadowMap ? 1 : 0) << ", "
         << (w.hasPersistentGeometry ? 1 : 0) << ", "
         << w.rejectCapacity << ", " << w.rejectPositionBufferCreate << ", "
         << w.rejectIndexBufferCreate << ", "
         << w.rejectBlendBufferCreate << ", " << w.rejectUvBufferCreate
         << ", " << w.rejectRegistryInsert << ", " << w.rejectOther << ", "
         << w.createAttempts << ", " << w.bytesNeededTotal << ", "
         << w.bytesNeededMax << ", " << w.bytesNeededLast << ", "
         << w.forceGcRequests << ", " << w.forceGcNoBytesFreed << ", "
         << w.forceGcStillInsufficient << ", " << w.forceGcBytesFreed << ", "
         << w.capacityRejectAllCallers << ", " << w.capacityFastReject << ", "
         << w.expiryTokensPopped << ", " << w.expiryTokensRequeued << ", "
         << w.expiryStaleTokens << ", " << w.expiryAgeEvictions << ", "
         << w.expiryQueueSize << ", "
         << w.bytesCap << ", " << w.bytesUsed << ", " << w.bytesEvicted
         << ", " << w.bytesEvictedDelta << ", " << w.liveGeometryCount
         << ", " << w.s1EarlyEntryCount << ", "
         << w.s1EarlyPersistentBackedCount << ", "
         << w.s1EarlyPersistentReverseIndexCount << ", "
         << w.s1EarlyFallbackBackedCount << ", "
         << w.s1EarlyLogicalReferencedBytes << ", "
         << w.s1EarlyAcceptedHitCount << ", "
         << w.s1EarlyReplayPublishedCount << ", "
         << w.s1EarlyReplayInstanceCount << ", "
         << w.s1EarlyReplayFallbackCount << ", "
         << w.s1EarlySourceMismatchEvictCount << ", "
         << (w.s1EarlyReplayClosureMismatch ? 1 : 0) << "]";
  }
  json << "\n  ],\n";

  // 聚合分段数据（注意：分层 scope 为“包含式”统计，父节点包含子节点耗时）
  struct SectionAggregate {
    std::string name;
    std::string path;
    std::string parentPath;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    double maxCpuMs = 0.0;
    double maxGpuMs = 0.0;
    uint64_t calls = 0;
  };
  std::unordered_map<std::string, SectionAggregate> sectionTotals;
  struct ThreadSectionAggregate : SectionAggregate {
    uint32_t threadId = 0;
  };
  std::unordered_map<uint64_t, ThreadSectionAggregate> threadSectionTotals;
  for (const auto *f : frames) {
    for (const auto &s : f->sections) {
      if (s.id >= snapshot.sections.size())
        continue;
      const auto &meta = snapshot.sections[s.id];
      std::string key = meta.path.empty() ? meta.name : meta.path;
      auto &agg = sectionTotals[key];
      if (agg.path.empty()) {
        agg.name = meta.name;
        agg.path = key;
        agg.parentPath = meta.parentPath;
      }
      agg.totalCpuMs += s.cpuMs;
      agg.totalGpuMs += s.gpuMs;
      agg.maxCpuMs = std::max(agg.maxCpuMs, s.maxCpuMs);
      agg.maxGpuMs = std::max(agg.maxGpuMs, s.maxGpuMs);
      agg.calls += s.callCount;

      const uint64_t threadKey = (uint64_t(s.threadId) << 32u) | s.id;
      auto& threadAgg = threadSectionTotals[threadKey];
      if (threadAgg.path.empty()) {
        threadAgg.name = meta.name;
        threadAgg.path = key;
        threadAgg.parentPath = meta.parentPath;
        threadAgg.threadId = s.threadId;
      }
      threadAgg.totalCpuMs += s.cpuMs;
      threadAgg.totalGpuMs += s.gpuMs;
      threadAgg.maxCpuMs = std::max(threadAgg.maxCpuMs, s.maxCpuMs);
      threadAgg.maxGpuMs = std::max(threadAgg.maxGpuMs, s.maxGpuMs);
      threadAgg.calls += s.callCount;
    }
  }

  // Offline fast/slow cohort attribution. Nothing below executes while a game
  // frame or Hook is running; it consumes only the immutable export snapshot.
  struct CohortPathAggregate {
    std::string name;
    std::string path;
    std::string parentPath;
    double inclusiveCpuMs = 0.0;
    double gpuMs = 0.0;
    uint64_t calls = 0u;
    uint32_t gpuSamples = 0u;
  };
  std::unordered_map<std::string, CohortPathAggregate> fastPathTotals;
  std::unordered_map<std::string, CohortPathAggregate> slowPathTotals;
  std::unordered_map<std::string, CohortPathAggregate> fastOverlayTotals;
  std::unordered_map<std::string, CohortPathAggregate> slowOverlayTotals;
  std::unordered_map<std::string, CohortPathAggregate> stableFastPathTotals;
  std::unordered_map<std::string, CohortPathAggregate> stableSlowPathTotals;
  std::unordered_map<std::string, CohortPathAggregate> stableFastOverlayTotals;
  std::unordered_map<std::string, CohortPathAggregate> stableSlowOverlayTotals;
  auto excludeFromFrameStateAttribution = [&](const std::string& path) {
    return isNonAdditiveOverlaySectionPath(path) ||
           isUncoveredFrameWallSectionPath(path) ||
           path == "EngineCycle" || startsWith(path, "EngineCycle/") ||
           startsWith(path, "War3MainLoop/Pump/Cycle") ||
           startsWith(path, "War3MainLoop/Engine/WaitGate/Cycle");
  };
  auto collectFrameStateAttribution =
      [&](const std::vector<int8_t>& states,
          std::unordered_map<std::string, CohortPathAggregate>& fastTotals,
          std::unordered_map<std::string, CohortPathAggregate>& slowTotals,
          std::unordered_map<std::string, CohortPathAggregate>&
              fastOverlay,
          std::unordered_map<std::string, CohortPathAggregate>&
              slowOverlay) {
        const size_t count = std::min(states.size(), frames.size());
        for (size_t frameOrdinal = 0u; frameOrdinal < count;
             ++frameOrdinal) {
          const int8_t state = states[frameOrdinal];
          if (state != 0 && state != 1)
            continue;
          auto& cohortTotals = state == 0 ? fastTotals : slowTotals;
          auto& cohortOverlays = state == 0 ? fastOverlay : slowOverlay;
          std::unordered_set<std::string> sampledGpuPaths;
          std::unordered_set<std::string> sampledGpuOverlays;
          for (const auto& timing : frames[frameOrdinal]->sections) {
            if (timing.id >= snapshot.sections.size())
              continue;
            const auto& meta = snapshot.sections[timing.id];
            const std::string path =
                meta.path.empty() ? meta.name : meta.path;
            // DetachedPhaseWall is a non-CPU phase budget overlay. It must
            // never appear in either the additive CPU cohort or the
            // orthogonal CPU overlay table merely because FramePipeline is
            // classified as non-additive elsewhere.
            if (isUncoveredFrameWallSectionPath(path) ||
                startsWith(path, "FramePipeline/DetachedPhaseWall/") ||
                path == "EngineCycle" ||
                startsWith(path, "EngineCycle/") ||
                startsWith(path, "War3MainLoop/Pump/Cycle") ||
                startsWith(path,
                           "War3MainLoop/Engine/WaitGate/Cycle")) {
              continue;
            }
            const bool overlay = isNonAdditiveOverlaySectionPath(path);
            if (!overlay && excludeFromFrameStateAttribution(path))
              continue;
            auto& totals = overlay ? cohortOverlays : cohortTotals;
            auto& aggregate = totals[path];
            if (aggregate.path.empty()) {
              aggregate.name = meta.name;
              aggregate.path = path;
              aggregate.parentPath = meta.parentPath;
            }
            aggregate.inclusiveCpuMs += timing.cpuMs;
            aggregate.calls += timing.callCount;
            if (timing.gpuCount != 0u) {
              aggregate.gpuMs += timing.gpuMs;
              (overlay ? sampledGpuOverlays : sampledGpuPaths).insert(path);
            }
          }
          for (const std::string& path : sampledGpuPaths) {
            auto it = cohortTotals.find(path);
            if (it != cohortTotals.end())
              ++it->second.gpuSamples;
          }
          for (const std::string& path : sampledGpuOverlays) {
            auto it = cohortOverlays.find(path);
            if (it != cohortOverlays.end())
              ++it->second.gpuSamples;
          }
        }
      };
  if (frameState.valid) {
    collectFrameStateAttribution(
        frameState.stateByFrame, fastPathTotals, slowPathTotals,
        fastOverlayTotals, slowOverlayTotals);
  }
  if (frameState.rollingStableValid) {
    collectFrameStateAttribution(
        frameState.rollingStableStateByFrame, stableFastPathTotals,
        stableSlowPathTotals, stableFastOverlayTotals,
        stableSlowOverlayTotals);
  }

  struct FrameStatePathRow {
    std::string name;
    std::string path;
    std::string parentPath;
    double fastInclusiveMs = 0.0;
    double slowInclusiveMs = 0.0;
    double fastSelfMs = 0.0;
    double slowSelfMs = 0.0;
    double fastCallsPerFrame = 0.0;
    double slowCallsPerFrame = 0.0;
    double fastGpuMs = 0.0;
    double slowGpuMs = 0.0;
    uint32_t fastGpuSamples = 0u;
    uint32_t slowGpuSamples = 0u;
  };
  struct FrameStateOverlayRow {
    std::string name;
    std::string path;
    std::string parentPath;
    double fastInclusiveMs = 0.0;
    double slowInclusiveMs = 0.0;
    double fastCallsPerFrame = 0.0;
    double slowCallsPerFrame = 0.0;
    double fastGpuMs = 0.0;
    double slowGpuMs = 0.0;
    uint32_t fastGpuSamples = 0u;
    uint32_t slowGpuSamples = 0u;
  };
  auto cohortChildSums =
      [](const std::unordered_map<std::string, CohortPathAggregate>& totals) {
        std::unordered_map<std::string, double> childSums;
        for (const auto& kv : totals) {
          if (!kv.second.parentPath.empty())
            childSums[kv.second.parentPath] += kv.second.inclusiveCpuMs;
        }
        return childSums;
      };
  auto buildPathRows =
      [&](const std::unordered_map<std::string, CohortPathAggregate>&
              fastTotals,
          const std::unordered_map<std::string, CohortPathAggregate>&
              slowTotals,
          uint32_t fastFrames,
          uint32_t slowFrames) {
        const auto fastChildSums = cohortChildSums(fastTotals);
        const auto slowChildSums = cohortChildSums(slowTotals);
        std::unordered_set<std::string> paths;
        paths.reserve(fastTotals.size() + slowTotals.size());
        for (const auto& kv : fastTotals)
          paths.insert(kv.first);
        for (const auto& kv : slowTotals)
          paths.insert(kv.first);

        std::vector<FrameStatePathRow> rows;
        rows.reserve(paths.size());
        const double fastDenominator = static_cast<double>(fastFrames);
        const double slowDenominator = static_cast<double>(slowFrames);
        for (const std::string& path : paths) {
          const auto fastIt = fastTotals.find(path);
          const auto slowIt = slowTotals.find(path);
          FrameStatePathRow row;
          if (fastIt != fastTotals.end()) {
            row.name = fastIt->second.name;
            row.path = fastIt->second.path;
            row.parentPath = fastIt->second.parentPath;
            if (fastDenominator > 0.0) {
              row.fastInclusiveMs =
                  fastIt->second.inclusiveCpuMs / fastDenominator;
              row.fastCallsPerFrame =
                  static_cast<double>(fastIt->second.calls) /
                  fastDenominator;
            }
            row.fastGpuSamples = fastIt->second.gpuSamples;
            if (row.fastGpuSamples != 0u) {
              row.fastGpuMs =
                  fastIt->second.gpuMs /
                  static_cast<double>(row.fastGpuSamples);
            }
            const auto childIt = fastChildSums.find(path);
            const double childTotal =
                childIt != fastChildSums.end() ? childIt->second : 0.0;
            row.fastSelfMs =
                fastDenominator > 0.0
                    ? std::max(0.0, fastIt->second.inclusiveCpuMs -
                                        childTotal) /
                          fastDenominator
                    : 0.0;
          }
          if (slowIt != slowTotals.end()) {
            if (row.path.empty()) {
              row.name = slowIt->second.name;
              row.path = slowIt->second.path;
              row.parentPath = slowIt->second.parentPath;
            }
            if (slowDenominator > 0.0) {
              row.slowInclusiveMs =
                  slowIt->second.inclusiveCpuMs / slowDenominator;
              row.slowCallsPerFrame =
                  static_cast<double>(slowIt->second.calls) /
                  slowDenominator;
            }
            row.slowGpuSamples = slowIt->second.gpuSamples;
            if (row.slowGpuSamples != 0u) {
              row.slowGpuMs =
                  slowIt->second.gpuMs /
                  static_cast<double>(row.slowGpuSamples);
            }
            const auto childIt = slowChildSums.find(path);
            const double childTotal =
                childIt != slowChildSums.end() ? childIt->second : 0.0;
            row.slowSelfMs =
                slowDenominator > 0.0
                    ? std::max(0.0, slowIt->second.inclusiveCpuMs -
                                        childTotal) /
                          slowDenominator
                    : 0.0;
          }
          rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(),
                  [](const FrameStatePathRow& a,
                     const FrameStatePathRow& b) {
                    const double aSelfDelta =
                        std::abs(a.slowSelfMs - a.fastSelfMs);
                    const double bSelfDelta =
                        std::abs(b.slowSelfMs - b.fastSelfMs);
                    if (aSelfDelta != bSelfDelta)
                      return aSelfDelta > bSelfDelta;
                    return std::abs(a.slowInclusiveMs -
                                    a.fastInclusiveMs) >
                           std::abs(b.slowInclusiveMs -
                                    b.fastInclusiveMs);
                  });
        return rows;
      };
  auto buildOverlayRows =
      [](const std::unordered_map<std::string, CohortPathAggregate>&
             fastTotals,
         const std::unordered_map<std::string, CohortPathAggregate>&
             slowTotals,
         uint32_t fastFrames,
         uint32_t slowFrames) {
        std::unordered_set<std::string> paths;
        paths.reserve(fastTotals.size() + slowTotals.size());
        for (const auto& kv : fastTotals)
          paths.insert(kv.first);
        for (const auto& kv : slowTotals)
          paths.insert(kv.first);
        std::vector<FrameStateOverlayRow> rows;
        rows.reserve(paths.size());
        const double fastDenominator = static_cast<double>(fastFrames);
        const double slowDenominator = static_cast<double>(slowFrames);
        for (const std::string& path : paths) {
          const auto fastIt = fastTotals.find(path);
          const auto slowIt = slowTotals.find(path);
          FrameStateOverlayRow row;
          if (fastIt != fastTotals.end()) {
            row.name = fastIt->second.name;
            row.path = fastIt->second.path;
            row.parentPath = fastIt->second.parentPath;
            if (fastDenominator > 0.0) {
              row.fastInclusiveMs =
                  fastIt->second.inclusiveCpuMs / fastDenominator;
              row.fastCallsPerFrame =
                  static_cast<double>(fastIt->second.calls) /
                  fastDenominator;
            }
            row.fastGpuSamples = fastIt->second.gpuSamples;
            if (row.fastGpuSamples != 0u) {
              row.fastGpuMs =
                  fastIt->second.gpuMs /
                  static_cast<double>(row.fastGpuSamples);
            }
          }
          if (slowIt != slowTotals.end()) {
            if (row.path.empty()) {
              row.name = slowIt->second.name;
              row.path = slowIt->second.path;
              row.parentPath = slowIt->second.parentPath;
            }
            if (slowDenominator > 0.0) {
              row.slowInclusiveMs =
                  slowIt->second.inclusiveCpuMs / slowDenominator;
              row.slowCallsPerFrame =
                  static_cast<double>(slowIt->second.calls) /
                  slowDenominator;
            }
            row.slowGpuSamples = slowIt->second.gpuSamples;
            if (row.slowGpuSamples != 0u) {
              row.slowGpuMs =
                  slowIt->second.gpuMs /
                  static_cast<double>(row.slowGpuSamples);
            }
          }
          rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(),
                  [](const FrameStateOverlayRow& a,
                     const FrameStateOverlayRow& b) {
                    return std::abs(a.slowInclusiveMs -
                                    a.fastInclusiveMs) >
                           std::abs(b.slowInclusiveMs -
                                    b.fastInclusiveMs);
                  });
        return rows;
      };

  const auto frameStatePathRows =
      buildPathRows(fastPathTotals, slowPathTotals,
                    frameState.fast.frameCount, frameState.slow.frameCount);
  const auto frameStateOverlayRows =
      buildOverlayRows(fastOverlayTotals, slowOverlayTotals,
                       frameState.fast.frameCount,
                       frameState.slow.frameCount);
  const auto rollingStablePathRows =
      buildPathRows(stableFastPathTotals, stableSlowPathTotals,
                    frameState.rollingStableFast.frameCount,
                    frameState.rollingStableSlow.frameCount);
  const auto rollingStableOverlayRows =
      buildOverlayRows(stableFastOverlayTotals, stableSlowOverlayTotals,
                       frameState.rollingStableFast.frameCount,
                       frameState.rollingStableSlow.frameCount);

  auto writeFrameStateCohort =
      [&](const char* indent, const char* name,
          const FrameStateCohortSummary& cohort, bool trailingComma) {
        json << indent << "\"" << name << "\": {\"frameCount\": "
             << cohort.frameCount << ", \"avgFrameWallMs\": "
             << cohort.avgFrameWallMs << ", \"avgGpuMs\": ";
        if (cohort.gpuSamples != 0u)
          json << cohort.avgGpuMs;
        else
          json << "null";
        json << ", \"avgProcessCpuMs\": ";
        if (cohort.processCpuSamples != 0u)
          json << cohort.avgProcessCpuMs;
        else
          json << "null";
        json << ", \"avgMainCpuMs\": ";
        if (cohort.mainCpuSamples != 0u)
          json << cohort.avgMainCpuMs;
        else
          json << "null";
        json << ", \"avgWorkerCpuMs\": ";
        if (cohort.processCpuSamples != 0u)
          json << cohort.avgWorkerCpuMs;
        else
          json << "null";
        json << ", \"processCpuPerWallCore\": ";
        if (cohort.processCpuSamples != 0u)
          json << cohort.processCpuPerWallCore;
        else
          json << "null";
        json << ", \"gpuSamples\": " << cohort.gpuSamples
             << ", \"processCpuSamples\": " << cohort.processCpuSamples
             << ", \"mainCpuSamples\": " << cohort.mainCpuSamples << "}";
        if (trailingComma)
          json << ",";
        json << "\n";
      };
  const auto writeRelativeDeltaPct =
      [&](double fastValue, double slowValue) {
        if (fastValue > 1e-9) {
          json << (slowValue - fastValue) / fastValue * 100.0;
        } else {
          // A zero/missing fast-side denominator is undefined, not a measured
          // zero-percent delta. Preserve that distinction for the report UI.
          json << "null";
        }
      };
  const auto writeSampledRelativeDeltaPct =
      [&](double fastValue, double slowValue, uint32_t fastSamples,
          uint32_t slowSamples) {
        if (fastSamples != 0u && slowSamples != 0u &&
            fastValue > 1e-9) {
          json << (slowValue - fastValue) / fastValue * 100.0;
        } else {
          json << "null";
        }
      };
  const auto writeStateSeries =
      [&](const char* indent, const std::vector<int8_t>& states,
          bool trailingComma) {
        json << indent << "\"stateSeriesColumns\": [\"epoch\", \"state\"],\n";
        json << indent << "\"stateSeries\": [";
        for (size_t i = 0u; i < frames.size(); ++i) {
          if (i != 0u)
            json << ",";
          const int state =
              i < states.size() ? static_cast<int>(states[i]) : -1;
          json << "[" << frames[i]->frameEpoch << "," << state << "]";
        }
        json << "]";
        if (trailingComma)
          json << ",";
        json << "\n";
      };
  const auto writePathRows =
      [&](const char* indent, const std::vector<FrameStatePathRow>& rows,
          bool trailingComma) {
        json << indent << "\"paths\": [\n";
        for (size_t i = 0u; i < rows.size(); ++i) {
          const auto& row = rows[i];
          const double inclusiveDelta =
              row.slowInclusiveMs - row.fastInclusiveMs;
          const double selfDelta = row.slowSelfMs - row.fastSelfMs;
          const double callsDelta =
              row.slowCallsPerFrame - row.fastCallsPerFrame;
          if (i != 0u)
            json << ",\n";
          json << indent << "  {\"name\": \"" << row.name
               << "\", \"path\": \"" << row.path
               << "\", \"parentPath\": \"" << row.parentPath
               << "\", \"fastInclusiveMs\": " << row.fastInclusiveMs
               << ", \"slowInclusiveMs\": " << row.slowInclusiveMs
               << ", \"inclusiveDeltaMs\": " << inclusiveDelta
               << ", \"inclusiveDeltaPct\": ";
          writeRelativeDeltaPct(row.fastInclusiveMs, row.slowInclusiveMs);
          json << ", \"fastSelfMs\": " << row.fastSelfMs
               << ", \"slowSelfMs\": " << row.slowSelfMs
               << ", \"selfDeltaMs\": " << selfDelta
               << ", \"selfDeltaPct\": ";
          writeRelativeDeltaPct(row.fastSelfMs, row.slowSelfMs);
          json << ", \"fastCallsPerFrame\": " << row.fastCallsPerFrame
               << ", \"slowCallsPerFrame\": " << row.slowCallsPerFrame
               << ", \"callsPerFrameDelta\": " << callsDelta
               << ", \"fastGpuMs\": ";
          if (row.fastGpuSamples != 0u)
            json << row.fastGpuMs;
          else
            json << "null";
          json << ", \"slowGpuMs\": ";
          if (row.slowGpuSamples != 0u)
            json << row.slowGpuMs;
          else
            json << "null";
          json << ", \"gpuDeltaMs\": ";
          if (row.fastGpuSamples != 0u && row.slowGpuSamples != 0u)
            json << row.slowGpuMs - row.fastGpuMs;
          else
            json << "null";
          json << ", \"gpuDeltaPct\": ";
          writeSampledRelativeDeltaPct(
              row.fastGpuMs, row.slowGpuMs, row.fastGpuSamples,
              row.slowGpuSamples);
          json << ", \"fastGpuSamples\": " << row.fastGpuSamples
               << ", \"slowGpuSamples\": " << row.slowGpuSamples << "}";
        }
        json << "\n" << indent << "]";
        if (trailingComma)
          json << ",";
        json << "\n";
      };
  const auto writeOverlayRows =
      [&](const char* indent,
          const std::vector<FrameStateOverlayRow>& rows,
          bool trailingComma) {
        json << indent << "\"overlayPaths\": [\n";
        for (size_t i = 0u; i < rows.size(); ++i) {
          const auto& row = rows[i];
          const double inclusiveDelta =
              row.slowInclusiveMs - row.fastInclusiveMs;
          const double callsDelta =
              row.slowCallsPerFrame - row.fastCallsPerFrame;
          if (i != 0u)
            json << ",\n";
          json << indent << "  {\"name\": \"" << row.name
               << "\", \"path\": \"" << row.path
               << "\", \"parentPath\": \"" << row.parentPath
               << "\", \"additive\": false"
               << ", \"fastInclusiveMs\": " << row.fastInclusiveMs
               << ", \"slowInclusiveMs\": " << row.slowInclusiveMs
               << ", \"inclusiveDeltaMs\": " << inclusiveDelta
               << ", \"inclusiveDeltaPct\": ";
          writeRelativeDeltaPct(row.fastInclusiveMs, row.slowInclusiveMs);
          json << ", \"fastCallsPerFrame\": " << row.fastCallsPerFrame
               << ", \"slowCallsPerFrame\": " << row.slowCallsPerFrame
               << ", \"callsPerFrameDelta\": " << callsDelta
               << ", \"fastGpuMs\": ";
          if (row.fastGpuSamples != 0u)
            json << row.fastGpuMs;
          else
            json << "null";
          json << ", \"slowGpuMs\": ";
          if (row.slowGpuSamples != 0u)
            json << row.slowGpuMs;
          else
            json << "null";
          json << ", \"gpuDeltaMs\": ";
          if (row.fastGpuSamples != 0u && row.slowGpuSamples != 0u)
            json << row.slowGpuMs - row.fastGpuMs;
          else
            json << "null";
          json << ", \"gpuDeltaPct\": ";
          writeSampledRelativeDeltaPct(
              row.fastGpuMs, row.slowGpuMs, row.fastGpuSamples,
              row.slowGpuSamples);
          json << ", \"fastGpuSamples\": " << row.fastGpuSamples
               << ", \"slowGpuSamples\": " << row.slowGpuSamples << "}";
        }
        json << "\n" << indent << "]";
        if (trailingComma)
          json << ",";
        json << "\n";
      };
  json << "  \"frameStateAnalysis\": {\n";
  json << "    \"valid\": " << (frameState.valid ? "true" : "false")
       << ",\n";
  json << "    \"method\": \"winsorized-1d-kmeans-v1-export-only\",\n";
  json << "    \"contract\": \"instantaneous descriptive frameWall cohorts; "
          "not proof of bimodality, a discrete engine state, or causality; OS "
          "CPU and GPU are cohort correlates, not additive frame-wall "
          "children\",\n";
  json << "    \"winsorLowMs\": " << frameState.winsorLowMs
       << ", \"winsorHighMs\": " << frameState.winsorHighMs << ",\n";
  json << "    \"fastCenterMs\": " << frameState.fastCenterMs
       << ", \"slowCenterMs\": " << frameState.slowCenterMs
       << ", \"thresholdMs\": " << frameState.thresholdMs << ",\n";
  json << "    \"relativeFrameWallDeltaPct\": "
       << frameState.relativeFrameWallDeltaPct
       << ", \"relativeCentroidDeltaPct\": "
       << frameState.relativeCentroidDeltaPct
       << ", \"pooledWithinStddevMs\": "
       << frameState.pooledWithinStddevMs << ",\n";
  json << "    \"relativeGpuDeltaPct\": ";
  writeSampledRelativeDeltaPct(
      frameState.fast.avgGpuMs, frameState.slow.avgGpuMs,
      frameState.fast.gpuSamples, frameState.slow.gpuSamples);
  json << ", \"relativeProcessCpuDeltaPct\": ";
  writeSampledRelativeDeltaPct(
      frameState.fast.avgProcessCpuMs, frameState.slow.avgProcessCpuMs,
      frameState.fast.processCpuSamples,
      frameState.slow.processCpuSamples);
  json << ", \"relativeMainCpuDeltaPct\": ";
  writeSampledRelativeDeltaPct(
      frameState.fast.avgMainCpuMs, frameState.slow.avgMainCpuMs,
      frameState.fast.mainCpuSamples, frameState.slow.mainCpuSamples);
  json << ",\n";
  writeFrameStateCohort("    ", "fast", frameState.fast, true);
  writeFrameStateCohort("    ", "slow", frameState.slow, true);
  const auto writeRunSummary =
      [&](const char* name, const FrameStateRunSummary& runs,
          bool trailingComma) {
        json << "      \"" << name << "\": {\"runCount\": "
             << runs.runCount << ", \"transitionCount\": "
             << runs.transitionCount << ", \"maxFastRunFrames\": "
             << runs.maxFastRunFrames << ", \"maxSlowRunFrames\": "
             << runs.maxSlowRunFrames << ", \"maxFastRunWallMs\": "
             << runs.maxFastRunWallMs << ", \"maxSlowRunWallMs\": "
             << runs.maxSlowRunWallMs << "}";
        if (trailingComma)
          json << ",";
        json << "\n";
      };
  json << "    \"temporalPersistence\": {\n";
  json << "      \"contract\": \"instantaneous runs use raw cohort labels; "
          "rolling runs use about one second of frame-wall samples plus "
          "hysteresis; both are descriptive and do not prove an internal "
          "mode switch\",\n";
  writeRunSummary("instantaneous", frameState.instantaneousRuns, true);
  json << "      \"rollingWindowTargetMs\": "
       << frameState.rollingWindowTargetMs
       << ", \"rollingHysteresisHalfBandMs\": "
       << frameState.rollingHysteresisHalfBandMs
       << ", \"rollingMeanMinMs\": " << frameState.rollingMeanMinMs
       << ", \"rollingMeanMaxMs\": " << frameState.rollingMeanMaxMs
       << ",\n";
  writeRunSummary("rolling", frameState.rollingRuns, false);
  json << "    },\n";
  json << "    \"pathAttributionContract\": \"per-cohort per-frame scope "
          "wall; self=inclusive-direct-children; EngineCycle, orthogonal "
          "overlays and uncovered frame wall excluded\",\n";
  json << "    \"overlayAttributionContract\": \"non-additive orthogonal "
          "attribution; inclusive and calls only; must not be summed with "
          "paths, Self, coverage, or lanes\",\n";
  json << "    \"pathGpuContract\": \"per-path per-frame sum of completed "
          "timestamp pairs, averaged only across frames with at least one "
          "completed pair; null when a cohort side has no sample\",\n";
  writeStateSeries("    ", frameState.stateByFrame, true);
  json << "    \"rollingStateSeriesColumns\": [\"epoch\", \"state\", "
          "\"rollingMeanMs\"],\n";
  json << "    \"rollingStateSeries\": [";
  for (size_t i = 0u; i < frames.size(); ++i) {
    if (i != 0u)
      json << ",";
    const int state = i < frameState.rollingStateByFrame.size()
                          ? static_cast<int>(
                                frameState.rollingStateByFrame[i])
                          : -1;
    json << "[" << frames[i]->frameEpoch << "," << state << ",";
    if (i < frameState.rollingMeanByFrame.size() &&
        frameState.rollingMeanByFrame[i] >= 0.0) {
      json << frameState.rollingMeanByFrame[i];
    } else {
      json << "null";
    }
    json << "]";
  }
  json << "],\n";
  json << "    \"rollingStable\": {\n";
  json << "      \"valid\": "
       << (frameState.rollingStableValid ? "true" : "false") << ",\n";
  json << "      \"contract\": \"about-one-second rolling-state stable "
          "cores only; frames inside threshold plus-or-minus hysteresis band "
          "are excluded; each side requires at least 8 frames; descriptive, "
          "not causal\",\n";
  json << "      \"deadbandLowMs\": "
       << frameState.thresholdMs - frameState.rollingHysteresisHalfBandMs
       << ", \"deadbandHighMs\": "
       << frameState.thresholdMs + frameState.rollingHysteresisHalfBandMs
       << ",\n";
  json << "      \"relativeFrameWallDeltaPct\": ";
  if (frameState.rollingStableValid) {
    writeRelativeDeltaPct(frameState.rollingStableFast.avgFrameWallMs,
                          frameState.rollingStableSlow.avgFrameWallMs);
  } else {
    json << "null";
  }
  json << ", \"relativeGpuDeltaPct\": ";
  if (frameState.rollingStableValid) {
    writeSampledRelativeDeltaPct(
        frameState.rollingStableFast.avgGpuMs,
        frameState.rollingStableSlow.avgGpuMs,
        frameState.rollingStableFast.gpuSamples,
        frameState.rollingStableSlow.gpuSamples);
  } else {
    json << "null";
  }
  json << ", \"relativeProcessCpuDeltaPct\": ";
  if (frameState.rollingStableValid) {
    writeSampledRelativeDeltaPct(
        frameState.rollingStableFast.avgProcessCpuMs,
        frameState.rollingStableSlow.avgProcessCpuMs,
        frameState.rollingStableFast.processCpuSamples,
        frameState.rollingStableSlow.processCpuSamples);
  } else {
    json << "null";
  }
  json << ", \"relativeMainCpuDeltaPct\": ";
  if (frameState.rollingStableValid) {
    writeSampledRelativeDeltaPct(
        frameState.rollingStableFast.avgMainCpuMs,
        frameState.rollingStableSlow.avgMainCpuMs,
        frameState.rollingStableFast.mainCpuSamples,
        frameState.rollingStableSlow.mainCpuSamples);
  } else {
    json << "null";
  }
  json << ",\n";
  writeFrameStateCohort("      ", "fast",
                        frameState.rollingStableFast, true);
  writeFrameStateCohort("      ", "slow",
                        frameState.rollingStableSlow, true);
  json << "      \"pathAttributionContract\": \"stable-core per-frame "
          "additive scope wall; self=inclusive-direct-children; overlays "
          "excluded\",\n";
  json << "      \"overlayAttributionContract\": \"non-additive orthogonal "
          "attribution; inclusive and calls only; never sum\",\n";
  json << "      \"pathGpuContract\": \"per-path per-frame completed "
          "timestamp sums; missing is null, never zero-filled\",\n";
  writeStateSeries("      ", frameState.rollingStableStateByFrame, true);
  writePathRows("      ", rollingStablePathRows, true);
  writeOverlayRows("      ", rollingStableOverlayRows, false);
  json << "    },\n";
  writePathRows("    ", frameStatePathRows, true);
  writeOverlayRows("    ", frameStateOverlayRows, false);
  json << "  },\n";

  auto findSection = [&](const std::string &path) -> const SectionAggregate * {
    auto it = sectionTotals.find(path);
    if (it == sectionTotals.end())
      return nullptr;
    return &it->second;
  };

  // EngineCycle 是跨 WaitGate 的独立 clock domain，而非 Present frame 的
  // 子 scope。它只能用于节拍分析，绝不能参与 frame CPU coverage/hot tree。
  const std::string cycleRootPathEngine = "EngineCycle";
  const std::string cycleRootPathPump = "War3MainLoop/Pump/Cycle";
  const SectionAggregate *cycleRoot = findSection(cycleRootPathEngine);
  std::string cycleRootPath = cycleRootPathEngine;
  if (!cycleRoot) {
    cycleRoot = findSection(cycleRootPathPump);
    cycleRootPath = cycleRootPathPump;
  }
  const std::string cycleActivePath = cycleRootPath + "/Active";
  const std::string cycleIdlePath = cycleRootPath + "/Idle";
  const SectionAggregate *cycleActive = findSection(cycleActivePath);
  const SectionAggregate *cycleIdle = findSection(cycleIdlePath);
  const double cycleTotalCpu = cycleRoot ? cycleRoot->totalCpuMs : 0.0;
  const double cycleActiveCpu = cycleActive ? cycleActive->totalCpuMs : 0.0;
  const double cycleIdleCpu = cycleIdle ? cycleIdle->totalCpuMs : 0.0;
  const bool hasEngineCycle = cycleRoot != nullptr;
  const uint64_t cycleCalls = cycleRoot ? cycleRoot->calls : 0u;
  // EngineCycle can run more than once per Present. Report a true per-cycle
  // average here; callsPerFrame is exported separately.
  const double cycleAvgMs =
      cycleCalls > 0 ? cycleTotalCpu / static_cast<double>(cycleCalls) : 0.0;
  const double cycleActiveAvgMs =
      cycleCalls > 0 ? cycleActiveCpu / static_cast<double>(cycleCalls) : 0.0;
  const double cycleIdleAvgMs =
      cycleCalls > 0 ? cycleIdleCpu / static_cast<double>(cycleCalls) : 0.0;

  struct CyclePhaseRow {
    std::string name;
    std::string path;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double sharePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<CyclePhaseRow> cyclePhases;
  if (cycleRoot) {
    cyclePhases.reserve(16);
    for (const auto &kv : sectionTotals) {
      const auto &agg = kv.second;
      if (agg.parentPath != cycleRootPath)
        continue;
      CyclePhaseRow row;
      row.name = agg.name;
      row.path = agg.path;
      row.totalCpuMs = agg.totalCpuMs;
      row.avgCpuMs =
          cycleCalls > 0
              ? (agg.totalCpuMs / static_cast<double>(cycleCalls))
              : 0.0;
      row.sharePct =
          (cycleTotalCpu > 1e-6)
              ? std::clamp(agg.totalCpuMs / cycleTotalCpu * 100.0, 0.0, 100.0)
              : 0.0;
      row.callsPerFrame = frames.empty() ? 0.0
                                         : (static_cast<double>(agg.calls) /
                                            static_cast<double>(frames.size()));
      cyclePhases.push_back(std::move(row));
    }
    std::sort(cyclePhases.begin(), cyclePhases.end(),
              [](const CyclePhaseRow &a, const CyclePhaseRow &b) {
                return a.avgCpuMs > b.avgCpuMs;
              });
  }

  // 先生成独立 cycle overlay，再从 Present-frame 树中移除该 clock domain。
  // 保留真实 War3MainLoop/Engine/WaitGate，它是一次实际 blocked wall interval。
  for (auto it = sectionTotals.begin(); it != sectionTotals.end();) {
    // 同时兼容旧报告：旧版曾错误把 cycle 挂在 WaitGate/Pump 下。
    if (it->first == cycleRootPathEngine ||
        startsWith(it->first, "EngineCycle/") ||
        startsWith(it->first, "War3MainLoop/Engine/WaitGate/Cycle") ||
        startsWith(it->first, "War3MainLoop/Pump/Cycle")) {
      it = sectionTotals.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = threadSectionTotals.begin(); it != threadSectionTotals.end();) {
    const std::string& path = it->second.path;
    if (path == cycleRootPathEngine || startsWith(path, "EngineCycle/") ||
        startsWith(path, "War3MainLoop/Engine/WaitGate/Cycle") ||
        startsWith(path, "War3MainLoop/Pump/Cycle")) {
      it = threadSectionTotals.erase(it);
    } else {
      ++it;
    }
  }

  // 主循环阶段聚合（对路径后缀做规范化，避免嵌套路径导致同名阶段分裂）。
  struct MainLoopStagePattern {
    const char *suffix;
    const char *stageName;
  };
  static const MainLoopStagePattern kStagePatterns[] = {
      {"War3MainLoop/Dispatch/Case0", "Dispatch/Case0"},
      {"War3MainLoop/Dispatch/Case1", "Dispatch/Case1"},
      {"War3MainLoop/Dispatch/Case2", "Dispatch/Case2"},
      {"War3MainLoop/Dispatch/Case3", "Dispatch/Case3"},
      {"War3MainLoop/Dispatch/Case4", "Dispatch/Case4"},
      {"War3MainLoop/Dispatch/Case5", "Dispatch/Case5"},
      {"War3MainLoop/Dispatch/Case6", "Dispatch/Case6"},
      {"War3MainLoop/Dispatch/Case7", "Dispatch/Case7"},
      {"War3MainLoop/Dispatch/Case8", "Dispatch/Case8"},
      {"War3MainLoop/Dispatch/Case9", "Dispatch/Case9"},
      {"War3MainLoop/Dispatch/Case10", "Dispatch/Case10"},
      {"War3MainLoop/Dispatch/Case11", "Dispatch/Case11"},
      {"War3MainLoop/Dispatch/Case12", "Dispatch/Case12"},
      {"War3MainLoop/Dispatch/Case13", "Dispatch/Case13"},
      {"War3MainLoop/Dispatch/Case14", "Dispatch/Case14"},
      {"War3MainLoop/DispatchModule/StateFinalize",
       "DispatchModule/StateFinalize"},
      {"War3MainLoop/DispatchModule/LoadBlockType1_Single",
       "DispatchModule/LoadBlockType1_Single"},
      {"War3MainLoop/DispatchModule/LoadBlockType1_Batch",
       "DispatchModule/LoadBlockType1_Batch"},
      {"War3MainLoop/DispatchModule/LoadBlockType27",
       "DispatchModule/LoadBlockType27"},
      {"War3MainLoop/DispatchModule/LoadBlockType28",
       "DispatchModule/LoadBlockType28"},
      {"War3MainLoop/DispatchModule/MainCallbackGate",
       "DispatchModule/MainCallbackGate"},
      {"War3MainLoop/DispatchModule/LoadBlockType2_ResetSync",
       "DispatchModule/LoadBlockType2_ResetSync"},
      {"War3MainLoop/DispatchModule/LoadBlockType8_InputSet",
       "DispatchModule/LoadBlockType8_InputSet"},
      {"War3MainLoop/DispatchModule/LoadBlockType9_InputClear",
       "DispatchModule/LoadBlockType9_InputClear"},
      {"War3MainLoop/DispatchModule/LoadBlockType11_InputCompose",
       "DispatchModule/LoadBlockType11_InputCompose"},
      {"War3MainLoop/DispatchModule/LoadBlockType12",
       "DispatchModule/LoadBlockType12"},
      {"War3MainLoop/DispatchModule/LoadBlockType16_Command",
       "DispatchModule/LoadBlockType16_Command"},
      {"War3MainLoop/DispatchModule/LoadBlockType13_TimeState",
       "DispatchModule/LoadBlockType13_TimeState"},
      {"War3MainLoop/DispatchModule/LoadBlockType14_Finalize",
       "DispatchModule/LoadBlockType14_Finalize"},
      {"War3MainLoop/DispatchModule/MainCallbackGateAlt",
       "DispatchModule/MainCallbackGateAlt"},
      {"War3MainLoop/DispatchModule/Other", "DispatchModule/Other"},
      {"War3MainLoop/Dispatch/Game", "Dispatch/Game"},
      {"War3MainLoop/Dispatch/Input", "Dispatch/Input"},
      {"War3MainLoop/Dispatch/System", "Dispatch/System"},
      {"War3MainLoop/Dispatch/Other", "Dispatch/Other"},
      {"War3MainLoop/Engine/WaitGate", "Engine/WaitGate"},
      {"War3MainLoop/Engine/SleepGateInner", "Engine/SleepGateInner"},
      {"War3MainLoop/Engine/SleepGate", "Engine/SleepGate"},
      {"War3MainLoop/Engine/PrepareDispatch", "Engine/PrepareDispatch"},
      {"War3MainLoop/Engine/FinalizeDispatch", "Engine/FinalizeDispatch"},
      {"War3MainLoop/Engine/TickUpdate", "Engine/TickUpdate"},
      {"War3MainLoop/Engine/FinalizeWorker", "Engine/FinalizeWorker"},
      {"War3MainLoop/Engine/ComputeWakeDelta", "Engine/ComputeWakeDelta"},
      {"War3MainLoop/Engine/PrepareWait", "Engine/PrepareWait"},
      {"War3MainLoop/Engine/RunCallbacks", "Engine/RunCallbacks"},
      {"War3MainLoop/Engine/QueueFlush", "Engine/QueueFlush"},
      {"War3MainLoop/Engine/FinalizeTick", "Engine/FinalizeTick"},
      {"War3MainLoop/Engine/Reschedule", "Engine/Reschedule"},
      {"War3MainLoop/Engine/SelectWorker", "Engine/SelectWorker"},
      {"War3MainLoop/Engine/TlsPump", "Engine/TlsPump"},
      {"War3MainLoop/Dispatch", "Dispatch"},
      {"War3MainLoop/Callback", "Callback"},
      {"War3MainLoop/Pump", "Pump"},
  };

  struct MainLoopStageAggregate {
    std::string name;
    std::string suffix;
    double totalCpuMs = 0.0;
    uint64_t calls = 0;
  };
  std::unordered_map<std::string, MainLoopStageAggregate> mainLoopStageTotals;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    for (const auto &pattern : kStagePatterns) {
      if (!endsWith(agg.path, pattern.suffix))
        continue;
      auto &row = mainLoopStageTotals[pattern.stageName];
      if (row.name.empty()) {
        row.name = pattern.stageName;
        row.suffix = pattern.suffix;
      }
      row.totalCpuMs += agg.totalCpuMs;
      row.calls += agg.calls;
      break;
    }
  }

  struct MainLoopStageRow {
    std::string name;
    std::string suffix;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double shareInFramePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<MainLoopStageRow> mainLoopStages;
  mainLoopStages.reserve(mainLoopStageTotals.size());
  for (const auto &kv : mainLoopStageTotals) {
    const auto &src = kv.second;
    MainLoopStageRow row;
    row.name = src.name;
    row.suffix = src.suffix;
    row.totalCpuMs = src.totalCpuMs;
    row.avgCpuMs = frames.empty()
                       ? 0.0
                       : (src.totalCpuMs / static_cast<double>(frames.size()));
    row.shareInFramePct =
        (totalCpu > 1e-6)
            ? std::clamp(src.totalCpuMs / totalCpu * 100.0, 0.0, 100.0)
            : 0.0;
    row.callsPerFrame = frames.empty() ? 0.0
                                       : (static_cast<double>(src.calls) /
                                          static_cast<double>(frames.size()));
    mainLoopStages.push_back(std::move(row));
  }
  auto parseDispatchCaseNumber = [](const std::string& name) -> int {
    constexpr const char* kPrefix = "Dispatch/Case";
    if (!startsWith(name, kPrefix))
      return -1;
    const char* p = name.c_str() + std::strlen(kPrefix);
    if (*p == '\0')
      return -1;
    char* end = nullptr;
    long v = std::strtol(p, &end, 10);
    if (end == p || (end && *end != '\0'))
      return -1;
    if (v < 0 || v > 999)
      return -1;
    return static_cast<int>(v);
  };
  auto stageOrderKey = [&](const std::string& name) -> int {
    static const char* kOrdered[] = {
        "Engine/SelectWorker",     "Engine/PrepareWait",
        "Engine/WaitGate",         "Engine/SleepGate",
        "Engine/SleepGateInner",   "Engine/PrepareDispatch",
        "Engine/RunCallbacks",     "Pump",
        "Dispatch",                "Dispatch/Game",
        "Dispatch/Input",          "Dispatch/System",
        "Dispatch/Other",          "Engine/FinalizeDispatch",
        "Engine/QueueFlush",       "Engine/TickUpdate",
        "Engine/FinalizeTick",     "Engine/FinalizeWorker",
        "Engine/ComputeWakeDelta", "Engine/Reschedule",
        "Callback",                "Engine/TlsPump",
    };
    for (int i = 0; i < static_cast<int>(sizeof(kOrdered) / sizeof(kOrdered[0]));
         ++i) {
      if (name == kOrdered[i])
        return i;
    }
    if (startsWith(name, "Dispatch/Case")) {
      const int caseId = parseDispatchCaseNumber(name);
      return caseId >= 0 ? (100 + caseId) : 190;
    }
    if (startsWith(name, "DispatchModule/"))
      return 300;
    return 1000;
  };
  std::sort(mainLoopStages.begin(), mainLoopStages.end(),
            [&](const MainLoopStageRow &a, const MainLoopStageRow &b) {
              const int orderA = stageOrderKey(a.name);
              const int orderB = stageOrderKey(b.name);
              if (orderA != orderB)
                return orderA < orderB;
              if (orderA == 300)
                return a.name < b.name;
              return a.avgCpuMs > b.avgCpuMs;
            });

  struct MainLoopBreakdownRow {
    std::string name;
    std::string path;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double shareInParentPct = 0.0;
    double callsPerFrame = 0.0;
    std::string sourceTag;
  };

  auto collectMainLoopBreakdownRows =
      [&](const std::string &parentPath, const std::string &pathPrefix,
          const char *sourceTag) -> std::vector<MainLoopBreakdownRow> {
    std::vector<MainLoopBreakdownRow> rows;
    const SectionAggregate *parent = findSection(parentPath);
    const double parentTotalCpu = parent ? parent->totalCpuMs : 0.0;
    for (const auto &kv : sectionTotals) {
      const auto &agg = kv.second;
      if (!startsWith(agg.path, pathPrefix.c_str()))
        continue;
      if (agg.path == parentPath)
        continue;
      MainLoopBreakdownRow row;
      row.name = agg.name;
      row.path = agg.path;
      row.totalCpuMs = agg.totalCpuMs;
      row.avgCpuMs = frames.empty()
                         ? 0.0
                         : (agg.totalCpuMs / static_cast<double>(frames.size()));
      row.shareInParentPct =
          (parentTotalCpu > 1e-6)
              ? std::clamp(agg.totalCpuMs / parentTotalCpu * 100.0, 0.0, 100.0)
              : 0.0;
      row.callsPerFrame =
          frames.empty() ? 0.0
                         : (static_cast<double>(agg.calls) /
                            static_cast<double>(frames.size()));
      row.sourceTag = sourceTag ? sourceTag : "";
      rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(),
              [](const MainLoopBreakdownRow &a, const MainLoopBreakdownRow &b) {
                return a.avgCpuMs > b.avgCpuMs;
              });
    return rows;
  };

  const auto dispatchCaseRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Dispatch", "War3MainLoop/Dispatch/Case", "HookMeasured");
  const auto dispatchModuleRows =
      collectMainLoopBreakdownRows("War3MainLoop/DispatchModule",
                                   "War3MainLoop/DispatchModule/",
                                   "CaseMapped");
  const auto tickUpdateSubRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Engine/TickUpdate/Sub",
      "War3MainLoop/Engine/TickUpdate/Sub/", "HookMeasured");
  const auto runCallbacksCallerRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Engine/RunCallbacks",
      "War3MainLoop/Engine/RunCallbacks/Caller_", "HookMeasured");
  const auto functionBreakdownRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Function", "War3MainLoop/Function/", "HookMeasured");
  const auto dispatchCaseFunctionRows = collectMainLoopBreakdownRows(
      "War3MainLoop/Function/DispatchCaseFunctions",
      "War3MainLoop/Function/DispatchCaseFunctions/", "CaseMapped");

  // 口径说明（变量名里的 Cpu 是历史命名，SectionTiming 实际是墙钟）：
  // - allSectionSum：所有节点（含父子重叠）的墙钟累积，可能大于 frame wall；
  // - allThreadRootSectionSum：所有线程 additive 根节点（parentPath 为空）
  //   的墙钟累积。不同线程可并行，因此它只能用于 root 内相对占比诊断；
  // - allThreadRootIdleWaitSum/allThreadRootActiveSum：上述全线程根节点按标签
  //   拆分，同样不是 Present frame-wall 预算。
  //
  // 只有下面单独计算的 mainThreadRootSectionSum 才能与 Present frame wall
  // 做覆盖率预算。进程/线程 Get*Times 是 OS CPU 时钟，不能与这些 scope
  // 墙钟相加或直接相减后称为“CPU hotspot”。
  double allSectionSum = 0.0;
  double allThreadRootSectionSum = 0.0;
  double allThreadRootIdleWaitSum = 0.0;
  double allThreadRootActiveSum = 0.0;
  for (const auto &kv : sectionTotals) {
    const bool overlay = isNonAdditiveOverlaySectionPath(kv.second.path);
    if (!overlay)
      allSectionSum += kv.second.totalCpuMs;
    if (!overlay && kv.second.parentPath.empty()) {
      allThreadRootSectionSum += kv.second.totalCpuMs;
      if (isIdleWaitSectionPath(kv.second.path))
        allThreadRootIdleWaitSum += kv.second.totalCpuMs;
      else
        allThreadRootActiveSum += kv.second.totalCpuMs;
    }
  }

  // 单独计算主线程 additive root wall。它为“主线程未归属 CPU”提供一个
  // 可证明的区间，而不是把全进程 worker CPU 混进 Present wall 缺口。
  double mainThreadRootSectionSum = 0.0;
  std::unordered_set<std::string> mainThreadAdditiveRootPaths;
  if (snapshot.mainThreadId != 0u) {
    for (const auto& kv : threadSectionTotals) {
      const auto& agg = kv.second;
      if (agg.threadId != snapshot.mainThreadId ||
          isNonAdditiveOverlaySectionPath(agg.path) ||
          isUncoveredFrameWallSectionPath(agg.path) ||
          !agg.parentPath.empty()) {
        continue;
      }
      mainThreadRootSectionSum += agg.totalCpuMs;
      mainThreadAdditiveRootPaths.insert(agg.path);
    }
  }
  uint32_t mainThreadRootWallOverlapFrames = 0u;
  if (!mainThreadAdditiveRootPaths.empty()) {
    for (const auto* frame : frames) {
      double frameMainRootWallMs = 0.0;
      for (const auto& timing : frame->sections) {
        if (timing.threadId != snapshot.mainThreadId ||
            timing.id >= snapshot.sections.size()) {
          continue;
        }
        const auto& meta = snapshot.sections[timing.id];
        if (meta.parentPath.empty() &&
            !isNonAdditiveOverlaySectionPath(meta.path) &&
            !isUncoveredFrameWallSectionPath(meta.path) &&
            mainThreadAdditiveRootPaths.find(meta.path) !=
            mainThreadAdditiveRootPaths.end()) {
          frameMainRootWallMs += timing.cpuMs;
        }
      }
      // 同一线程的真实 TLS 顶层 scope 理应互斥。保留 1us/0.1% 舍入余量；
      // 一旦违反，不再发布基于“帧墙钟 - main root wall”的 CPU 上界。
      const double toleranceMs =
          std::max(0.001, frame->totalCpuMs * 0.001);
      if (frameMainRootWallMs > frame->totalCpuMs + toleranceMs)
        ++mainThreadRootWallOverlapFrames;
    }
  }

  // 旧 active/idle 字段保留兼容，但不再作为 authoritative coverage 口径。
  const double idleWaitClampedTotal =
      std::clamp(allThreadRootIdleWaitSum, 0.0, totalCpu);
  const double trackedActiveClampedTotal =
      std::clamp(allThreadRootActiveSum, 0.0, totalCpu);
  const double activeCpuTotalRaw =
      std::max(0.0, totalCpu - idleWaitClampedTotal);
  const double activeCpuTotal =
      std::max(activeCpuTotalRaw, trackedActiveClampedTotal);
  // Present frame wall 是主线程的串行时钟域。全线程 roots 可能互相并行，
  // 因而 authoritative gap 必须只扣除主线程 additive root wall。
  const double uncoveredFrameWallTotal =
      std::max(0.0, totalCpu - mainThreadRootSectionSum);
  const bool idleActiveOverlapLikely =
      (allThreadRootIdleWaitSum + allThreadRootActiveSum) >
      (totalCpu * 1.05);
  const double avgAllThreadAdditiveRootWallMsDiagnostic =
      frames.empty() ? 0.0
                     : (allThreadRootSectionSum /
                        static_cast<double>(frames.size()));
  const double avgAllThreadActiveRootWallMsDiagnostic =
      frames.empty() ? 0.0
                     : (allThreadRootActiveSum /
                        static_cast<double>(frames.size()));
  const double avgAllThreadIdleRootWallMsDiagnostic =
      frames.empty() ? 0.0
                     : (allThreadRootIdleWaitSum /
                        static_cast<double>(frames.size()));
  // 历史字段保留旧的“全线程 root wall”数值，仅用于旧解析器。
  const double avgTrackedRootCpuMs =
      avgAllThreadAdditiveRootWallMsDiagnostic;
  const double avgTrackedMainThreadRootWallMs =
      frames.empty()
          ? 0.0
          : (mainThreadRootSectionSum /
             static_cast<double>(frames.size()));
  const double avgTrackedActiveCpuMs =
      frames.empty()
          ? 0.0
          : (trackedActiveClampedTotal / static_cast<double>(frames.size()));
  const double avgIdleWaitCpuMs =
      frames.empty()
          ? 0.0
          : (idleWaitClampedTotal / static_cast<double>(frames.size()));
  const double avgUntrackedCpuMs =
      frames.empty()
          ? 0.0
          : (uncoveredFrameWallTotal /
             static_cast<double>(frames.size()));
  const double avgUntrackedActiveCpuMs =
      avgUntrackedCpuMs;
  const double avgActiveFrameMs =
      frames.empty() ? 0.0
                     : (activeCpuTotal / static_cast<double>(frames.size()));
  const double frameWallScopeCoveragePct =
      (totalCpu > 1e-6)
          ? std::clamp(mainThreadRootSectionSum / totalCpu * 100.0,
                       0.0, 100.0)
          : 0.0;
  // Legacy aliases. “cpuCoverage” historically meant additive scope-wall
  // coverage, not sampled OS CPU coverage.
  const double cpuCoveragePct = frameWallScopeCoveragePct;
  const double cpuCoverageWithIdlePct = frameWallScopeCoveragePct;
  const double cpuInclusivePct =
      (totalCpu > 1e-6) ? (allSectionSum / totalCpu * 100.0) : 0.0;
  const bool mainThreadCpuBoundsAvailable =
      !frames.empty() &&
      mainThreadCpuSamples == frames.size() &&
      snapshot.mainThreadId != 0u;
  const bool mainThreadCpuUpperBoundAvailable =
      mainThreadCpuBoundsAvailable &&
      mainThreadRootWallOverlapFrames == 0u;
  const double mainThreadUnattributedCpuLowerBoundMs =
      mainThreadCpuBoundsAvailable
          ? std::max(0.0, avgMainThreadCpuMs -
                              avgTrackedMainThreadRootWallMs)
          : 0.0;
  const double avgMainThreadOutsideRootWallMs =
      std::max(0.0, avgCpu - avgTrackedMainThreadRootWallMs);
  const double mainThreadUnattributedCpuUpperBoundMs =
      mainThreadCpuUpperBoundAvailable
          ? std::min(avgMainThreadCpuMs, avgMainThreadOutsideRootWallMs)
          : 0.0;

  if (uncoveredFrameWallTotal > 0.0) {
    SectionAggregate &uncovered =
        sectionTotals["Other/UncoveredFrameWall"];
    uncovered.name =
        "Main-root-uncovered frame wall (NOT CPU)";
    uncovered.path = "Other/UncoveredFrameWall";
    uncovered.parentPath.clear();
    uncovered.totalCpuMs += uncoveredFrameWallTotal;
    uncovered.calls += static_cast<uint64_t>(frames.size());
  }

  // 计算 self 时间（exclusive）：self = inclusive - children(inclusive)。
  std::unordered_map<std::string, double> childCpuSums;
  std::unordered_map<std::string, double> childGpuSums;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    if (!agg.parentPath.empty()) {
      childCpuSums[agg.parentPath] += agg.totalCpuMs;
      childGpuSums[agg.parentPath] += agg.totalGpuMs;
    }
  }

  // 严格语义关系树：基于 self CPU 聚合，避免包含式统计导致父子重复计时。
  struct SemanticNodeAggregate {
    std::string name;
    std::string path;
    std::string parentPath;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    uint64_t calls = 0;
    bool hasIdleLeaf = false;
    bool hasActiveLeaf = false;
  };

  std::unordered_map<std::string, SemanticNodeAggregate> semanticNodes;
  auto ensureSemanticNode = [&](const std::string& path) -> SemanticNodeAggregate& {
    auto it = semanticNodes.find(path);
    if (it != semanticNodes.end())
      return it->second;

    SemanticNodeAggregate node = {};
    node.path = path;
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
      node.name = path;
      node.parentPath.clear();
    } else {
      node.name = path.substr(slash + 1);
      node.parentPath = path.substr(0, slash);
    }
    auto inserted = semanticNodes.emplace(path, std::move(node));
    return inserted.first->second;
  };

  constexpr const char* kSemanticRootPath = "Semantic";
  constexpr const char* kSemanticMainLoopPath = "Semantic/MainLoop";
  constexpr const char* kSemanticOutsideMainLoopPath =
      "Semantic/OutsideMainLoop";
  auto classifySemanticLeaf = [&](const std::string& path,
                                  bool idleWaitSection) -> std::string {
    // Cycle 指标属于“节拍包络”，与 Render/Logic 统计存在重叠。
    // 为了 strict 语义闭合关系，语义树内不纳入 Cycle 路径。
    if (path.find("/Cycle/") != std::string::npos || endsWith(path, "/Cycle"))
      return {};

    // 未覆盖帧墙钟是预算差值，不是函数节点，不纳入 strict 语义树分母。
    if (isUncoveredFrameWallSectionPath(path))
      return {};

    if (startsWith(path, "JassVM/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/JassVM";

    // 显式资源 trace（CurrentDraw/registry augment）不应被归进一个不可见的
    // OutsideMainLoop 泛桶；detail 模式下保留其叶名以便定位数据层开销。
    if (startsWith(path, "Semantic/CurrentDraw/") ||
        startsWith(path, "Semantic/AugmentShadowContext")) {
      constexpr std::string_view kSemanticPrefix = "Semantic/";
      return std::string(kSemanticOutsideMainLoopPath) + "/Resource/" +
             path.substr(kSemanticPrefix.size());
    }

    if (startsWith(path, "War3MainLoop/Dispatch/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/DispatchModule/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/DispatchCaseFunctions/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/sub_6F05A310"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";
    if (startsWith(path, "War3MainLoop/Function/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Engine";
    if (path == "War3MainLoop/Dispatch" ||
        endsWith(path, "War3MainLoop/Dispatch"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Dispatch";

    if (startsWith(path, "War3MainLoop/Callback") ||
        endsWith(path, "War3MainLoop/Callback"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Callback";
    if (startsWith(path, "War3MainLoop/Pump") ||
        endsWith(path, "War3MainLoop/Pump"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Pump";

    if (idleWaitSection)
      return std::string(kSemanticMainLoopPath) + "/Idle/WaitApi";

    if (startsWith(path, "War3MainLoop/Engine/"))
      return std::string(kSemanticMainLoopPath) + "/Logic/Engine";

    if (startsWith(path, "Draw") || startsWith(path, "Present") ||
        startsWith(path, "ShaderPack") || startsWith(path, "PostFX") ||
        startsWith(path, "AA") || startsWith(path, "SSAO")) {
      return std::string(kSemanticMainLoopPath) + "/Render/Pipeline";
    }
    if (startsWith(path, "Hook_Ui"))
      return std::string(kSemanticMainLoopPath) + "/Render/UI";
    if (startsWith(path, "Hook_WorldDispatch") ||
        startsWith(path, "Hook_WorldObjects_RenderGroup") ||
        startsWith(path, "Hook_Dispatch_") || startsWith(path, "SceneCollector")) {
      return std::string(kSemanticMainLoopPath) + "/Render/World";
    }
    if (startsWith(path, "Hook_FlushAndReset") ||
        startsWith(path, "Hook_FlushSortedItems") || startsWith(path, "FQ_")) {
      return std::string(kSemanticMainLoopPath) + "/Render/RenderQueue";
    }
    if (startsWith(path, "Shadow") || startsWith(path, "Outline") ||
        path.find("/Shadow") != std::string::npos ||
        path.find("/Outline") != std::string::npos) {
      return std::string(kSemanticMainLoopPath) + "/Render/Shadow";
    }

    if (startsWith(path, "War3MainLoop/") || startsWith(path, "Hook_") ||
        startsWith(path, "FQ_")) {
      return std::string(kSemanticMainLoopPath) + "/Unknown/OtherTracked";
    }
    return std::string(kSemanticOutsideMainLoopPath) + "/Tracked";
  };

  auto addSemanticLeaf = [&](const std::string& leafPath, double cpuMs, double gpuMs,
                             uint64_t calls, bool idleLeaf) {
    if (leafPath.empty())
      return;
    std::string path = leafPath;
    while (!path.empty()) {
      auto& node = ensureSemanticNode(path);
      node.totalCpuMs += cpuMs;
      node.totalGpuMs += gpuMs;
      node.calls += calls;
      if (idleLeaf)
        node.hasIdleLeaf = true;
      else
        node.hasActiveLeaf = true;

      if (node.parentPath.empty())
        break;
      path = node.parentPath;
    }
  };

  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    if (isNonAdditiveOverlaySectionPath(agg.path))
      continue;
    const double selfCpu = std::max(0.0, agg.totalCpuMs - childCpuSums[agg.path]);
    const double selfGpu = std::max(0.0, agg.totalGpuMs - childGpuSums[agg.path]);
    if (selfCpu <= 1e-9 && selfGpu <= 1e-9)
      continue;

    const bool idleLeaf = isIdleWaitSectionPath(agg.path);
    const std::string semanticLeafPath = classifySemanticLeaf(agg.path, idleLeaf);
    addSemanticLeaf(semanticLeafPath, selfCpu, selfGpu, agg.calls, idleLeaf);
  }

  // 计算语义树 self 值与导出列表。
  std::unordered_map<std::string, double> semanticChildCpuSums;
  std::unordered_map<std::string, double> semanticChildGpuSums;
  for (const auto& kv : semanticNodes) {
    const auto& node = kv.second;
    if (!node.parentPath.empty()) {
      semanticChildCpuSums[node.parentPath] += node.totalCpuMs;
      semanticChildGpuSums[node.parentPath] += node.totalGpuMs;
    }
  }

  struct SemanticExportRow {
    std::string name;
    std::string path;
    std::string parentPath;
    bool isIdleWait = false;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double totalSelfCpuMs = 0.0;
    double avgSelfCpuMs = 0.0;
    double totalGpuMs = 0.0;
    double avgGpuMs = 0.0;
    double totalSelfGpuMs = 0.0;
    double avgSelfGpuMs = 0.0;
    double avgCpuPerCycleMs = 0.0;
    double semanticSharePct = 0.0;
    uint64_t calls = 0;
    double callsPerFrame = 0.0;
    double framePct = 0.0;
  };

  const auto semanticRootIt = semanticNodes.find(kSemanticRootPath);
  const double semanticRootTotal =
      (semanticRootIt != semanticNodes.end()) ? semanticRootIt->second.totalCpuMs
                                              : 0.0;
  const double cycleCountTotal =
      (hasEngineCycle && cycleCalls > 0)
          ? static_cast<double>(cycleCalls)
          : static_cast<double>(frames.size());

  std::vector<SemanticExportRow> semanticRows;
  semanticRows.reserve(semanticNodes.size());
  for (const auto& kv : semanticNodes) {
    const auto& node = kv.second;
    SemanticExportRow row = {};
    row.name = node.name;
    row.path = node.path;
    row.parentPath = node.parentPath;
    row.isIdleWait = node.hasIdleLeaf && !node.hasActiveLeaf;
    row.totalCpuMs = node.totalCpuMs;
    row.avgCpuMs =
        frames.empty() ? 0.0 : (node.totalCpuMs / static_cast<double>(frames.size()));
    row.totalSelfCpuMs =
        std::max(0.0, node.totalCpuMs - semanticChildCpuSums[node.path]);
    row.avgSelfCpuMs =
        frames.empty() ? 0.0 : (row.totalSelfCpuMs / static_cast<double>(frames.size()));
    row.totalGpuMs = node.totalGpuMs;
    row.avgGpuMs =
        frames.empty() ? 0.0 : (node.totalGpuMs / static_cast<double>(frames.size()));
    row.totalSelfGpuMs =
        std::max(0.0, node.totalGpuMs - semanticChildGpuSums[node.path]);
    row.avgSelfGpuMs =
        frames.empty() ? 0.0 : (row.totalSelfGpuMs / static_cast<double>(frames.size()));
    row.avgCpuPerCycleMs =
        (cycleCountTotal > 1e-6) ? (node.totalCpuMs / cycleCountTotal) : row.avgCpuMs;
    row.semanticSharePct =
        (semanticRootTotal > 1e-6)
            ? std::clamp(node.totalCpuMs / semanticRootTotal * 100.0, 0.0, 100.0)
            : 0.0;

    const bool hasSemanticChildren =
        (semanticChildCpuSums[node.path] > 1e-9) ||
        (semanticChildGpuSums[node.path] > 1e-9);
    row.calls = node.calls;
    row.callsPerFrame =
        (frames.empty() || hasSemanticChildren)
            ? 0.0
            : (static_cast<double>(node.calls) / static_cast<double>(frames.size()));
    row.framePct =
        (totalCpu > 1e-6) ? std::clamp(node.totalCpuMs / totalCpu * 100.0, 0.0, 100.0) : 0.0;
    semanticRows.push_back(std::move(row));
  }

  std::sort(semanticRows.begin(), semanticRows.end(),
            [](const SemanticExportRow& a, const SemanticExportRow& b) {
              if (a.path == b.path)
                return a.avgCpuMs > b.avgCpuMs;
              return a.path < b.path;
            });

  // Legacy: historical all-thread root-wall aggregate. Cross-thread roots may
  // overlap, so this field is not a Present frame-wall coverage numerator.
  json << "  \"avgTrackedRootCpuMs\": " << avgTrackedRootCpuMs << ",\n";
  json << "  \"avgAllThreadAdditiveRootWallMsDiagnostic\": "
       << avgAllThreadAdditiveRootWallMsDiagnostic << ",\n";
  json << "  \"allThreadRootWallDiagnostic\": {\n";
  json << "    \"contract\": "
          "\"sum of additive root scope wall intervals across all threads; "
          "parallel roots may overlap; diagnostic only, never a Present "
          "frame-wall budget\",\n";
  json << "    \"legacyAliases\": "
          "\"avgTrackedRootCpuMs, avgTrackedActiveCpuMs and "
          "avgIdleWaitCpuMs use this non-budget clock domain\",\n";
  json << "    \"avgRootWallMs\": "
       << avgAllThreadAdditiveRootWallMsDiagnostic << ",\n";
  json << "    \"avgActiveRootWallMs\": "
       << avgAllThreadActiveRootWallMsDiagnostic << ",\n";
  json << "    \"avgIdleRootWallMs\": "
       << avgAllThreadIdleRootWallMsDiagnostic << "\n";
  json << "  },\n";
  json << "  \"avgTrackedActiveCpuMs\": " << avgTrackedActiveCpuMs << ",\n";
  json << "  \"avgIdleWaitCpuMs\": " << avgIdleWaitCpuMs << ",\n";
  json << "  \"avgUntrackedCpuMs\": " << avgUntrackedCpuMs << ",\n";
  json << "  \"avgUntrackedActiveCpuMs\": " << avgUntrackedActiveCpuMs << ",\n";
  // schema v9 authoritative wall-gap 命名。下方三个 *Cpu* 字段仅为旧解析器
  // 保留，数值仍是 frame wall，绝不能用于 CPU 热点排序。
  json << "  \"avgTrackedAdditiveRootWallMs\": "
       << avgTrackedMainThreadRootWallMs << ",\n";
  json << "  \"avgUncoveredFrameWallMs\": " << avgUntrackedCpuMs << ",\n";
  json << "  \"frameWallScopeCoveragePct\": "
       << frameWallScopeCoveragePct << ",\n";
  json << "  \"avgUnattributedCpuMs\": " << avgUntrackedActiveCpuMs << ",\n";
  json << "  \"legacyWallGapAliases\": {\n";
  json << "    \"contract\": "
          "\"deprecated field names below contain uncovered frame wall, "
          "not CPU\",\n";
  json << "    \"avgUntrackedCpuMs\": " << avgUntrackedCpuMs << ",\n";
  json << "    \"avgUntrackedActiveCpuMs\": "
       << avgUntrackedActiveCpuMs << ",\n";
  json << "    \"avgUnattributedCpuMs\": "
       << avgUntrackedActiveCpuMs << "\n";
  json << "  },\n";
  json << "  \"coverageWarning\": "
       << ((avgUntrackedCpuMs > 2.0 ||
            mainThreadRootWallOverlapFrames != 0u) ? "true" : "false")
       << ",\n";
  json << "  \"frameWallCoverageWarning\": "
       << ((avgUntrackedCpuMs > 2.0 ||
            mainThreadRootWallOverlapFrames != 0u) ? "true" : "false")
       << ",\n";
  json << "  \"activeFrameTimeMs\": " << avgActiveFrameMs << ",\n";
  json << "  \"idleActiveOverlapLikely\": "
       << (idleActiveOverlapLikely ? "true" : "false") << ",\n";
  json << "  \"cpuCoveragePct\": " << cpuCoveragePct << ",\n";
  json << "  \"cpuCoverageWithIdlePct\": " << cpuCoverageWithIdlePct << ",\n";
  json << "  \"cpuInclusivePct\": " << cpuInclusivePct << ",\n";
  json << "  \"frameWallAttribution\": {\n";
  json << "    \"contract\": "
          "\"Present frame wall budget subtracts only mutually-exclusive "
          "main-thread additive root scope wall intervals; uncovered wall is "
          "NOT CPU and worker CPU is parallel\",\n";
  json << "    \"frameWallMs\": " << avgCpu << ",\n";
  json << "    \"trackedAdditiveRootWallMs\": "
       << avgTrackedMainThreadRootWallMs << ",\n";
  json << "    \"uncoveredFrameWallMs\": " << avgUntrackedCpuMs << ",\n";
  json << "    \"scopeCoveragePct\": " << frameWallScopeCoveragePct << ",\n";
  json << "    \"mainThreadCpuBoundsAvailable\": "
       << (mainThreadCpuBoundsAvailable ? "true" : "false") << ",\n";
  json << "    \"mainThreadCpuSampleCount\": "
       << mainThreadCpuSamples << ",\n";
  json << "    \"frameCount\": " << frames.size() << ",\n";
  json << "    \"mainThreadCpuUpperBoundAvailable\": "
       << (mainThreadCpuUpperBoundAvailable ? "true" : "false") << ",\n";
  json << "    \"mainThreadRootWallOverlapFrames\": "
       << mainThreadRootWallOverlapFrames << ",\n";
  json << "    \"mainThreadCpuMs\": " << avgMainThreadCpuMs << ",\n";
  json << "    \"trackedMainThreadRootWallMs\": "
       << avgTrackedMainThreadRootWallMs << ",\n";
  json << "    \"mainThreadOutsideRootWallMs\": "
       << avgMainThreadOutsideRootWallMs << ",\n";
  json << "    \"unattributedMainThreadCpuLowerBoundMs\": "
       << mainThreadUnattributedCpuLowerBoundMs << ",\n";
  json << "    \"unattributedMainThreadCpuUpperBoundMs\": "
       << mainThreadUnattributedCpuUpperBoundMs << ",\n";
  json << "    \"upperBoundContract\": "
          "\"min(main-thread CPU, frame wall minus mutually-exclusive "
          "main-thread root wall); unavailable unless every report frame has "
          "a main-thread OS CPU sample or if root overlap is observed\",\n";
  json << "    \"workerThreadsCpuMs\": " << avgWorkerThreadsCpuMs << ",\n";
  json << "    \"workerCpuAttribution\": "
          "\"unavailable-without-per-thread-OS-CPU-probes\"\n";
  json << "  },\n";
  // Legacy object: retain keys, but make the lower-bound nature explicit in
  // the adjacent contract instead of presenting worker CPU as a measured gap.
  json << "  \"mainLoopUnknownAttribution\": {\n";
  json << "    \"contract\": "
          "\"legacy keys; mainThreadGapMs is a conservative CPU lower "
          "bound; workerGapMs is total worker CPU, not a gap\",\n";
  json << "    \"mainThreadTrackedMs\": "
       << avgTrackedMainThreadRootWallMs << ",\n";
  json << "    \"mainThreadGapMs\": "
       << mainThreadUnattributedCpuLowerBoundMs << ",\n";
  json << "    \"workerGapMs\": " << avgWorkerThreadsCpuMs << ",\n";
  json << "    \"outsideMainLoopMs\": 0.0\n";
  json << "  },\n";
  json << "  \"mainLoopCycle\": {\n";
  json << "    \"present\": " << (hasEngineCycle ? "true" : "false") << ",\n";
  json << "    \"avgCycleMs\": " << cycleAvgMs << ",\n";
  json << "    \"avgActiveMs\": " << cycleActiveAvgMs << ",\n";
  json << "    \"avgIdleMs\": " << cycleIdleAvgMs << ",\n";
  json << "    \"cyclesPerFrame\": "
       << (frames.empty() ? 0.0
                          : static_cast<double>(cycleCalls) /
                                static_cast<double>(frames.size()))
       << ",\n";
  json << "    \"phases\": [\n";
  first = true;
  for (const auto &row : cyclePhases) {
    if (!first)
      json << ",\n";
    json << "      {\"name\": \"" << row.name << "\", \"path\": \"" << row.path
         << "\", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"sharePct\": " << row.sharePct
         << ", \"callsPerFrame\": " << row.callsPerFrame << "}";
    first = false;
  }
  json << "\n    ]\n";
  json << "  },\n";
  json << "  \"mainLoopStages\": [\n";
  first = true;
  for (const auto &row : mainLoopStages) {
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name << "\", \"suffix\": \""
         << row.suffix << "\", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"shareInFramePct\": " << row.shareInFramePct
         << ", \"callsPerFrame\": " << row.callsPerFrame << "}";
    first = false;
  }
  json << "\n  ],\n";

  auto emitMainLoopBreakdownRows =
      [&](const char *fieldName, const std::vector<MainLoopBreakdownRow> &rows) {
        json << "  \"" << fieldName << "\": [\n";
        bool localFirst = true;
        for (const auto &row : rows) {
          if (!localFirst)
            json << ",\n";
          json << "    {\"name\": \"" << row.name << "\", \"path\": \""
               << row.path << "\", \"totalCpuMs\": " << row.totalCpuMs
               << ", \"avgCpuMs\": " << row.avgCpuMs
               << ", \"shareInParentPct\": " << row.shareInParentPct
               << ", \"callsPerFrame\": " << row.callsPerFrame
               << ", \"sourceTag\": \"" << row.sourceTag << "\"}";
          localFirst = false;
        }
        json << "\n  ],\n";
      };

  emitMainLoopBreakdownRows("mainLoopDispatchCases", dispatchCaseRows);
  emitMainLoopBreakdownRows("mainLoopDispatchModules", dispatchModuleRows);
  emitMainLoopBreakdownRows("mainLoopTickUpdateSub", tickUpdateSubRows);
  emitMainLoopBreakdownRows("mainLoopRunCallbacksCallers",
                            runCallbacksCallerRows);
  emitMainLoopBreakdownRows("mainLoopFunctionBreakdown", functionBreakdownRows);
  emitMainLoopBreakdownRows("mainLoopDispatchCaseFunctions",
                            dispatchCaseFunctionRows);

  json << "  \"semanticSections\": [\n";
  first = true;
  for (const auto& row : semanticRows) {
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name
         << "\", \"path\": \"" << row.path
         << "\", \"parentPath\": \"" << row.parentPath
         << "\", \"isIdleWait\": " << (row.isIdleWait ? "true" : "false")
         << ", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"totalSelfCpuMs\": " << row.totalSelfCpuMs
         << ", \"avgSelfCpuMs\": " << row.avgSelfCpuMs
         << ", \"totalGpuMs\": " << row.totalGpuMs
         << ", \"avgGpuMs\": " << row.avgGpuMs
         << ", \"totalSelfGpuMs\": " << row.totalSelfGpuMs
         << ", \"avgSelfGpuMs\": " << row.avgSelfGpuMs
         << ", \"avgCpuPerCycleMs\": " << row.avgCpuPerCycleMs
         << ", \"semanticSharePct\": " << row.semanticSharePct
         << ", \"calls\": " << row.calls
         << ", \"callsPerFrame\": " << row.callsPerFrame
         << ", \"framePct\": " << row.framePct
         << "}";
    first = false;
  }
  json << "\n  ],\n";

  json << "  \"semanticRootSections\": [\n";
  first = true;
  for (const auto& row : semanticRows) {
    if (row.parentPath != kSemanticRootPath)
      continue;
    if (!first)
      json << ",\n";
    json << "    {\"name\": \"" << row.name
         << "\", \"path\": \"" << row.path
         << "\", \"isIdleWait\": " << (row.isIdleWait ? "true" : "false")
         << ", \"totalCpuMs\": " << row.totalCpuMs
         << ", \"avgCpuMs\": " << row.avgCpuMs
         << ", \"trackedPct\": " << row.semanticSharePct
         << "}";
    first = false;
  }
  json << "\n  ],\n";

  // Static hook catalog is a deliberately partial migration pilot. It is
  // exported independently from timing sections and the legacy installed
  // inventory, so disabled/unsafe entries never masquerade as zero-cost
  // hotspots. Registration/state updates happen only during hook install.
  const auto hookCatalogRecords = hooks::SnapshotHookCatalogRecords();
  json << "  \"hookCatalogCoverage\": {\n";
  json << "    \"complete\": false,\n";
  json << "    \"mode\": \"partial-pilot\",\n";
  json << "    \"catalogedCount\": " << hookCatalogRecords.size() << ",\n";
  json << "    \"scope\": "
          "\"RenderPerf default-off WorldPrepare/RenderQueue/Transparent "
          "diagnostics only\",\n";
  json << "    \"migrationContract\": "
          "\"catalogedCount is not the total hook count; legacy "
          "hookInventory remains authoritative for unmigrated installed "
          "domains\"\n";
  json << "  },\n";
  json << "  \"hookCatalog\": [\n";
  first = true;
  for (const auto& record : hookCatalogRecords) {
    if (!first)
      json << ",\n";
    json << "    {\"id\": \"0x" << std::hex << record.id << std::dec
         << "\", \"domain\": \"" << record.domain
         << "\", \"hookName\": \"" << record.hookName
         << "\", \"kind\": \"" << record.kind
         << "\", \"targetRva\": \"0x" << std::hex << record.targetRva
         << "\", \"target\": \"0x" << record.target
         << "\", \"detour\": \"0x" << record.detour
         << "\", \"trampoline\": \"0x" << record.trampoline << std::dec
         << "\", \"timingRoot\": \"" << record.timingRoot
         << "\", \"nativeMode\": \"" << record.nativeMode
         << "\", \"customPhase\": \"" << record.customPhase
         << "\", \"safetyClass\": \"" << record.safetyClass
         << "\", \"activationGate\": {\"compileConfig\": \""
         << record.compileConfig << "\", \"environment\": \""
         << record.environment << "\", \"minPerfLevel\": "
         << record.minPerfLevel << ", \"runtimeModules\": \""
         << record.runtimeModules << "\", \"owner\": \"" << record.owner
         << "\", \"displayExpression\": \"" << record.displayExpression
         << "\"}, \"status\": \"" << record.status
         << "\", \"lastAttemptState\": \"" << record.lastAttemptState
         << "\", \"installed\": "
         << (record.installed ? "true" : "false")
         << ", \"attemptCount\": " << record.attemptCount
         << ", \"minHookStatus\": " << record.minHookStatus
         << ", \"reason\": \"" << record.reason << "\"}";
    first = false;
  }
  json << "\n  ],\n";

  // Legacy installed inventory remains byte-for-byte schema compatible for
  // unmigrated domains and existing parsers.
  const auto hookInstallRecords = hooks::SnapshotHookInstallRecords();
  json << "  \"hookInventory\": [\n";
  first = true;
  for (const auto& record : hookInstallRecords) {
    if (!first)
      json << ",\n";
    json << "    {\"domain\": \"" << record.domain
         << "\", \"hookName\": \"" << record.hookName
         << "\", \"state\": \"" << record.state
         << "\", \"target\": \"0x" << std::hex << record.target
         << "\", \"detour\": \"0x" << record.detour
         << "\", \"trampoline\": \"0x" << record.trampoline
         << std::dec << "\", \"installed\": "
         << (record.installed ? "true" : "false") << "}";
    first = false;
  }
  json << "\n  ],\n";

  json << "  \"sectionsTrackedPctContract\": "
          "\"trackedPct is relative to the all-thread additive root-wall "
          "diagnostic sum; cross-thread roots may overlap; it is not "
          "Present frame-wall coverage\",\n";
  json << "  \"sections\": [\n";
  first = true;
  for (const auto &kv : sectionTotals) {
    if (!first)
      json << ",\n";
    const auto &agg = kv.second;
    double avgCpu = frames.empty() ? 0.0 : agg.totalCpuMs / frames.size();
    double avgGpu = frames.empty() ? 0.0 : agg.totalGpuMs / frames.size();
    const double selfCpu =
        std::max(0.0, agg.totalCpuMs - childCpuSums[agg.path]);
    const double selfGpu =
        std::max(0.0, agg.totalGpuMs - childGpuSums[agg.path]);
    const double avgSelfCpu = frames.empty() ? 0.0 : selfCpu / frames.size();
    const double avgSelfGpu = frames.empty() ? 0.0 : selfGpu / frames.size();
    const double callsPerFrame = frames.empty()
                                     ? 0.0
                                     : static_cast<double>(agg.calls) /
                                           static_cast<double>(frames.size());
    const double trackedPct =
        (!isUncoveredFrameWallSectionPath(agg.path) &&
         allThreadRootSectionSum > 1e-6)
            ? std::clamp(agg.totalCpuMs / allThreadRootSectionSum * 100.0,
                         0.0, 100.0)
            : 0.0;
    json << "    {\"name\": \"" << agg.name << "\", \"path\": \"" << agg.path
         << "\", \"parentPath\": \"" << agg.parentPath << "\", \"isIdleWait\": "
         << (isIdleWaitSectionPath(agg.path) ? "true" : "false")
         << ", \"isOverlay\": "
         << (isNonAdditiveOverlaySectionPath(agg.path) ? "true" : "false")
         << ", \"isWallGap\": "
         << (isUncoveredFrameWallSectionPath(agg.path) ? "true" : "false")
         << ", \"totalCpuMs\": " << agg.totalCpuMs
         << ", \"avgCpuMs\": " << avgCpu << ", \"totalSelfCpuMs\": " << selfCpu
         << ", \"avgSelfCpuMs\": " << avgSelfCpu
         << ", \"totalGpuMs\": " << agg.totalGpuMs
         << ", \"avgGpuMs\": " << avgGpu << ", \"totalSelfGpuMs\": " << selfGpu
         << ", \"avgSelfGpuMs\": " << avgSelfGpu
         << ", \"maxCpuMs\": " << agg.maxCpuMs
         << ", \"maxGpuMs\": " << agg.maxGpuMs
         << ", \"calls\": " << agg.calls
         << ", \"callsPerFrame\": " << callsPerFrame
         << ", \"trackedPct\": " << trackedPct << "}";
    first = false;
  }
  json << "\n  ],\n";

  // 线程感知调用树。每条记录只能在同一 threadId 内做 parent/self closure；
  // 进程 CPU、worker CPU 与 Present wall 是独立口径，前端明确禁止相加。
  json << "  \"threadSections\": [\n";
  first = true;
  for (const auto& kv : threadSectionTotals) {
    const auto& agg = kv.second;
    if (!first)
      json << ",\n";
    const double avgCpu = frames.empty() ? 0.0 : agg.totalCpuMs / frames.size();
    const double avgGpu = frames.empty() ? 0.0 : agg.totalGpuMs / frames.size();
    const double callsPerFrame = frames.empty() ? 0.0 :
        static_cast<double>(agg.calls) / frames.size();
    json << "    {\"threadId\": " << agg.threadId
         << ", \"name\": \"" << agg.name
         << "\", \"path\": \"" << agg.path
         << "\", \"parentPath\": \"" << agg.parentPath
         << "\", \"isIdleWait\": "
         << (isIdleWaitSectionPath(agg.path) ? "true" : "false")
         << ", \"isOverlay\": "
         << (isNonAdditiveOverlaySectionPath(agg.path) ? "true" : "false")
         << ", \"totalCpuMs\": " << agg.totalCpuMs
         << ", \"avgCpuMs\": " << avgCpu
         << ", \"totalGpuMs\": " << agg.totalGpuMs
         << ", \"avgGpuMs\": " << avgGpu
         << ", \"maxCpuMs\": " << agg.maxCpuMs
         << ", \"maxGpuMs\": " << agg.maxGpuMs
         << ", \"calls\": " << agg.calls
         << ", \"callsPerFrame\": " << callsPerFrame << "}";
    first = false;
  }
  json << "\n  ],\n";

  // 父子关系边：用于前端展示函数关系与调用层级热度。
  struct SectionEdge {
    std::string parentPath;
    std::string parentName;
    std::string childPath;
    std::string childName;
    double totalCpuMs = 0.0;
    double avgCpuMs = 0.0;
    double parentAvgCpuMs = 0.0;
    double sharePct = 0.0;
    double callsPerFrame = 0.0;
  };
  std::vector<SectionEdge> edges;
  edges.reserve(sectionTotals.size());
  for (const auto &kv : sectionTotals) {
    const auto &child = kv.second;
    if (child.parentPath.empty())
      continue;
    auto parentIt = sectionTotals.find(child.parentPath);
    if (parentIt == sectionTotals.end())
      continue;

    const auto &parent = parentIt->second;
    SectionEdge edge;
    edge.parentPath = parent.path;
    edge.parentName = parent.name;
    edge.childPath = child.path;
    edge.childName = child.name;
    edge.totalCpuMs = child.totalCpuMs;
    edge.avgCpuMs =
        frames.empty()
            ? 0.0
            : (child.totalCpuMs / static_cast<double>(frames.size()));
    edge.parentAvgCpuMs =
        frames.empty()
            ? 0.0
            : (parent.totalCpuMs / static_cast<double>(frames.size()));
    edge.sharePct =
        (parent.totalCpuMs > 1e-6)
            ? std::clamp(child.totalCpuMs / parent.totalCpuMs * 100.0, 0.0,
                         100.0)
            : 0.0;
    edge.callsPerFrame = frames.empty() ? 0.0
                                        : (static_cast<double>(child.calls) /
                                           static_cast<double>(frames.size()));
    edges.push_back(std::move(edge));
  }
  std::sort(edges.begin(), edges.end(),
            [](const SectionEdge &a, const SectionEdge &b) {
              return a.avgCpuMs > b.avgCpuMs;
            });

  json << "  \"sectionEdges\": [\n";
  first = true;
  for (const auto &edge : edges) {
    if (!first)
      json << ",\n";
    json << "    {\"parentPath\": \"" << edge.parentPath
         << "\", \"parentName\": \"" << edge.parentName
         << "\", \"childPath\": \"" << edge.childPath << "\", \"childName\": \""
         << edge.childName << "\", \"totalCpuMs\": " << edge.totalCpuMs
         << ", \"avgCpuMs\": " << edge.avgCpuMs
         << ", \"parentAvgCpuMs\": " << edge.parentAvgCpuMs
         << ", \"sharePct\": " << edge.sharePct
         << ", \"callsPerFrame\": " << edge.callsPerFrame << "}";
    first = false;
  }
  json << "\n  ],\n";

  // 根节点分布：用于总览图（模块占比）。
  json << "  \"rootSectionsContract\": "
          "\"trackedPct is relative share within the all-thread additive "
          "root-wall diagnostic sum; cross-thread roots may overlap; not "
          "frame-wall coverage\",\n";
  json << "  \"rootSections\": [\n";
  first = true;
  for (const auto &kv : sectionTotals) {
    const auto &agg = kv.second;
    if (!agg.parentPath.empty() ||
        isUncoveredFrameWallSectionPath(agg.path) ||
        isNonAdditiveOverlaySectionPath(agg.path))
      continue;
    if (!first)
      json << ",\n";
    const double avgCpu = frames.empty() ? 0.0 : agg.totalCpuMs / frames.size();
    const double trackedPct =
        (allThreadRootSectionSum > 1e-6)
            ? std::clamp(agg.totalCpuMs / allThreadRootSectionSum * 100.0,
                         0.0, 100.0)
            : 0.0;
    json << "    {\"name\": \"" << agg.name << "\", \"path\": \"" << agg.path
         << "\", \"isIdleWait\": "
         << (isIdleWaitSectionPath(agg.path) ? "true" : "false")
         << ", \"totalCpuMs\": " << agg.totalCpuMs
         << ", \"avgCpuMs\": " << avgCpu << ", \"trackedPct\": " << trackedPct
         << "}";
    first = false;
  }
  json << "\n  ],\n";

  // ------------------------------------------------------------------
  // schema v9：线程泳道 + 帧级时间序列 + section 百分位。
  // 注意 worker 线程每帧可能 spawn 新 OS 线程（threadId 不同），因此泳道
  // 必须按"类别"归并而不是按原始 threadId 展开。
  // ------------------------------------------------------------------
  {
    const DWORD mainTid = snapshot.mainThreadId;
    uint32_t worldRenderThreadId = 0;
    double worldRenderThreadCpuMs = 0.0;
    for (const auto& kv : threadSectionTotals) {
      const auto& agg = kv.second;
      if (agg.path == "Hook_WorldRenderScene" &&
          agg.totalCpuMs > worldRenderThreadCpuMs) {
        worldRenderThreadCpuMs = agg.totalCpuMs;
        worldRenderThreadId = agg.threadId;
      }
    }
    const bool mainThreadMatchesWorldRender =
        mainTid != 0 && worldRenderThreadId != 0 &&
        mainTid == worldRenderThreadId;
    json << "  \"mainThreadProbeDiagnostics\": {\"trackedThreadId\": "
         << mainTid << ", \"worldRenderThreadId\": "
         << worldRenderThreadId << ", \"match\": "
         << (mainThreadMatchesWorldRender ? "true" : "false")
         << "},\n";
    auto classifyLane = [&](uint32_t tid, const std::string &path) -> int {
      if (mainTid != 0 && tid == mainTid)
        return 0; // MainThread
      if (path.rfind("PointShadow/", 0) == 0)
        return 2; // Worker
      if (path.rfind("War3Pipeline/", 0) == 0 || path.rfind("Shadow", 0) == 0 ||
          path.rfind("Draw", 0) == 0 || path.rfind("Present", 0) == 0 ||
          path.rfind("PostFX", 0) == 0 || path.rfind("SSAO", 0) == 0)
        return 1; // CS（命令流线程）
      return 3;   // Other
    };
    json << "  \"threadLanes\": [\"MainThread\", \"CS\", \"Worker\", "
            "\"Other\"],\n";
    json << "  \"threadLaneContract\": "
            "\"additive-process-cpu-budget-v3; main=GetThreadTimes exact; "
            "CS/Other=scope-wall-weighted estimates normalized to "
            "process-minus-main; Worker=unclassified CPU residual; lanes "
            "must not be used as frame-wall attribution\",\n";
    json << "  \"frameSeriesColumns\": [\"epoch\", \"frameWallMs\", "
            "\"gpuMs\", \"mainCpuMs\", \"csCpuMs\", \"workerCpuMs\", "
            "\"otherCpuMs\", \"processCpuMs\"],\n";

    // 帧级时间序列：[epoch, frameWall, gpu, main, cs, worker, other,
    // processCpu]。四条 CPU lane 必须严格闭合到 processCpu，不能与
    // frameWall（墙钟）或 GPU 队列时间相加。
    // lane 口径（2026-07-22 修正版）：
    // - Main：系统线程 CPU（GetThreadTimes 精确值），不用 scope 估算；
    // - CS/Other：非主线程按"线程局部顶层 section"（parentPath 不在同线程
    //   path 集合中）求和，避免 inclusive 父子双计，再按顶层 path 前缀归类；
    // - Worker：系统 workerCPU 减 CS 的余量（worker 探针本身含 CS 线程）。
    json << "  \"frameSeries\": [\n";
    bool firstFrame = true;
    for (const auto *f : frames) {
      // 按线程聚合本帧 section。
      std::unordered_map<uint32_t, std::unordered_map<std::string, double>>
          perThread;
      std::unordered_map<uint32_t, std::unordered_map<std::string, std::string>>
          perThreadParent;
      for (const auto &s : f->sections) {
        if (s.id >= snapshot.sections.size())
          continue;
        const auto &sec = snapshot.sections[s.id];
        if (isNonAdditiveOverlaySectionPath(sec.path))
          continue;
        perThread[s.threadId][sec.path] += s.cpuMs;
        perThreadParent[s.threadId][sec.path] = sec.parentPath;
      }
      double csMs = 0.0;
      double observedWorkerMs = 0.0;
      double otherMs = 0.0;
      for (const auto &tkv : perThread) {
        const uint32_t tid = tkv.first;
        if (mainTid != 0 && tid == mainTid)
          continue; // 主线程由系统 CPU 覆盖
        // 线程局部顶层：parentPath 不在本线程的 path 集合中（或为空）。
        double topSum = 0.0;
        std::string topPathForClassify;
        double topMax = 0.0;
        for (const auto &pkv : tkv.second) {
          const std::string &path = pkv.first;
          const std::string &parent = perThreadParent[tid][path];
          const bool isTop = parent.empty() ||
                             tkv.second.find(parent) == tkv.second.end();
          if (!isTop)
            continue;
          topSum += pkv.second;
          if (pkv.second > topMax) {
            topMax = pkv.second;
            topPathForClassify = path;
          }
        }
        if (topSum <= 0.0)
          continue;
        const int laneIdx = classifyLane(tid, topPathForClassify);
        if (laneIdx == 1)
          csMs += topSum;
        else if (laneIdx == 2)
          observedWorkerMs += topSum;
        else if (laneIdx == 3)
          otherMs += topSum;
        else
          otherMs += topSum;
      }
      const double mainMs = f->hasMainThreadCpu ? f->mainThreadCpuMs : 0.0;
      double workerMs = observedWorkerMs;
      double processMs = mainMs + csMs + workerMs + otherMs;
      if (f->hasProcessCpu) {
        const double nonMainBudget = f->hasMainThreadCpu
            ? std::max(0.0, f->workerThreadsCpuMs)
            : std::max(0.0, f->processCpuMs);
        const double observedNonMain = csMs + observedWorkerMs + otherMs;
        if (observedNonMain > nonMainBudget && observedNonMain > 1e-9) {
          // Scope 是墙钟包含式数据；若等待或线程重叠使观测和超过系统 CPU
          // 预算，按类别同比缩放，保持 lane 与 GetProcessTimes 严格闭合。
          const double scale = nonMainBudget / observedNonMain;
          csMs *= scale;
          workerMs *= scale;
          otherMs *= scale;
        } else {
          // 未被 scope 覆盖的非主线程 CPU 保守归入 Worker/Unknown residual。
          workerMs += nonMainBudget - observedNonMain;
        }
        processMs = f->processCpuMs;
      }
      if (!firstFrame)
        json << ",\n";
      firstFrame = false;
      json << "    [" << f->frameEpoch << ", " << f->totalCpuMs << ", ";
      if (f->hasGpuTiming)
        json << f->totalGpuMs;
      else
        json << "null";
      json << ", " << mainMs << ", " << csMs << ", " << workerMs << ", "
           << otherMs << ", " << processMs << "]";
    }
    json << "\n  ],\n";

    // Fixed-width per-frame stage series. This is derived entirely from the
    // archived SectionTiming records on the export worker: it adds no QPC,
    // scope, lock, or FrameSnapshot field to a running frame. These channels
    // are independent inclusive/overlay observations and are intentionally
    // not an additive frame decomposition.
    enum class StageSeriesThreadPolicy : uint8_t {
      MainSum,
      NonMainMax,
      AnySum,
    };
    enum class StageSeriesMetric : uint8_t {
      CpuWall,
      GpuTimestamp,
    };
    struct StageSeriesColumnDef {
      const char* key;
      const char* label;
      const char* kind;
      const char* threadDomain;
      const char* aggregation;
      StageSeriesThreadPolicy threadPolicy;
      StageSeriesMetric metric;
      bool overlay;
    };
    constexpr size_t kStageSeriesColumnCount = 16u;
    const std::array<StageSeriesColumnDef, kStageSeriesColumnCount>
        stageColumnDefs = {{
            {"worldPrepareMainInclMs", "WorldFramePrepare",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"renderSceneMainInclMs", "WorldRenderScene",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"flushSortedMainInclMs", "FlushSortedItems",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"populateMainInclMs", "SemanticScene Populate",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"shadowCaptureMainOverlayMs", "ShadowCapture",
             "nonAdditiveOverlayWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, true},
            {"shadowMainCsInclMs", "Shadow/Main CPU",
             "inclusiveScopeWall", "nonMainThreadMax",
             "maxAcrossNonMainThreads",
             StageSeriesThreadPolicy::NonMainMax,
             StageSeriesMetric::CpuWall, false},
            {"shadowMainGpuMs", "Shadow/Main GPU",
             "gpuTimestamp", "gpuQueue",
             "maxAcrossNonMainThreadSamples",
             StageSeriesThreadPolicy::NonMainMax,
             StageSeriesMetric::GpuTimestamp, false},
            {"presentTransitionMainInclMs",
             "PresentEx pre-swapchain transition",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"presentEntryPreEndFrameMainInclMs",
             "Swapchain Present entry before endFrame",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"engineQueueFlushMainInclMs", "Engine QueueFlush main",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"engineQueueFlushNonMainMaxMs",
             "Engine QueueFlush non-main max",
             "crossThreadScopeWallMax", "nonMainThreadMax",
             "maxAcrossNonMainThreads",
             StageSeriesThreadPolicy::NonMainMax,
             StageSeriesMetric::CpuWall, false},
            {"engineWaitGateMainInclMs", "Engine WaitGate main",
             "inclusiveScopeWall", "mainThread",
             "sumMatchedContexts", StageSeriesThreadPolicy::MainSum,
             StageSeriesMetric::CpuWall, false},
            {"engineWaitGateNonMainMaxMs",
             "Engine WaitGate non-main max",
             "crossThreadScopeWallMax", "nonMainThreadMax",
             "maxAcrossNonMainThreads",
             StageSeriesThreadPolicy::NonMainMax,
             StageSeriesMetric::CpuWall, false},
            {"presentToFirstWorldPhaseWallMs",
             "Present to first world boundary",
             "detachedPhaseWallOverlay", "detachedClockDomain",
             "sumSyntheticSamples", StageSeriesThreadPolicy::AnySum,
             StageSeriesMetric::CpuWall, true},
            {"firstToSecondWorldPhaseWallMs",
             "First to second world boundary",
             "detachedPhaseWallOverlay", "detachedClockDomain",
             "sumSyntheticSamples", StageSeriesThreadPolicy::AnySum,
             StageSeriesMetric::CpuWall, true},
            {"lastWorldToPresentEntryPhaseWallMs",
             "Last world boundary to Present entry",
             "detachedPhaseWallOverlay", "detachedClockDomain",
             "sumSyntheticSamples", StageSeriesThreadPolicy::AnySum,
             StageSeriesMetric::CpuWall, true},
        }};

    auto stageColumnMatches =
        [&](size_t column, const SectionStats& section) -> bool {
      switch (column) {
        case 0u:
          return section.path == "Hook_WorldFramePrepare";
        case 1u:
          return section.path == "Hook_WorldRenderScene";
        case 2u:
          // One logical FlushSortedItems hook can appear below multiple
          // dynamic parents. Summing its exact leaf name captures all calls
          // without adding any ancestor inclusive time.
          return section.name == "Hook_FlushSortedItems";
        case 3u:
          // This leaf currently has one producer
          // (War3TryPopulateSemanticShadowScene). Export matchedPaths below so
          // any future same-name collision is visible rather than silent.
          return section.name == "Populate";
        case 4u:
          return section.path == "ShadowCapture";
        case 5u:
        case 6u:
          return section.path == "Shadow/Main";
        case 7u:
          return section.path == "DXVK_D3D9_PresentEx";
        case 8u:
          return section.path == "D3D9Swapchain/PresentEntry";
        case 9u:
        case 10u:
          return section.path == "War3MainLoop/Engine/QueueFlush";
        case 11u:
        case 12u:
          return section.path == "War3MainLoop/Engine/WaitGate";
        case 13u:
          return section.path ==
              "FramePipeline/DetachedPhaseWall/"
              "PresentToFirstWorldBoundaryPhaseWall";
        case 14u:
          return section.path ==
              "FramePipeline/DetachedPhaseWall/"
              "FirstToSecondWorldBoundaryPhaseWall";
        case 15u:
          return section.path ==
                     "FramePipeline/DetachedPhaseWall/"
                     "FirstWorldBoundaryToPresentEntryPhaseWall" ||
                 section.path ==
                     "FramePipeline/DetachedPhaseWall/"
                     "SecondWorldBoundaryToPresentEntryPhaseWall";
        default:
          return false;
      }
    };

    std::vector<uint32_t> stageBindingMasks(snapshot.sections.size(), 0u);
    std::array<std::vector<std::string>, kStageSeriesColumnCount>
        stageMatchedPaths;
    for (size_t sectionId = 0u; sectionId < snapshot.sections.size();
         ++sectionId) {
      const auto& section = snapshot.sections[sectionId];
      uint32_t mask = 0u;
      for (size_t column = 0u; column < kStageSeriesColumnCount; ++column) {
        if (!stageColumnMatches(column, section))
          continue;
        mask |= uint32_t(1u) << column;
        auto& paths = stageMatchedPaths[column];
        if (std::find(paths.begin(), paths.end(), section.path) == paths.end())
          paths.push_back(section.path);
      }
      stageBindingMasks[sectionId] = mask;
    }

    struct StageSeriesFrameRow {
      uint64_t epoch = 0u;
      std::array<double, kStageSeriesColumnCount> values = {};
      std::array<bool, kStageSeriesColumnCount> hasValue = {};
    };
    std::vector<StageSeriesFrameRow> stageRows;
    stageRows.reserve(frames.size());
    std::array<bool, kStageSeriesColumnCount> stageDomainObserved = {};
    std::array<std::vector<uint32_t>, kStageSeriesColumnCount>
        stageThreadIdsSeen;
    const auto noteStageThread = [&](size_t column, uint32_t tid) {
      auto& tids = stageThreadIdsSeen[column];
      if (std::find(tids.begin(), tids.end(), tid) == tids.end())
        tids.push_back(tid);
    };

    for (const auto* frame : frames) {
      StageSeriesFrameRow row;
      row.epoch = frame->frameEpoch;
      for (const auto& timing : frame->sections) {
        if (timing.id >= stageBindingMasks.size())
          continue;
        const uint32_t mask = stageBindingMasks[timing.id];
        if (mask == 0u)
          continue;

        for (size_t column = 0u; column < kStageSeriesColumnCount;
             ++column) {
          if ((mask & (uint32_t(1u) << column)) == 0u)
            continue;
          const auto& def = stageColumnDefs[column];
          bool threadAccepted = false;
          switch (def.threadPolicy) {
            case StageSeriesThreadPolicy::MainSum:
              threadAccepted =
                  mainTid != 0u && timing.threadId == mainTid;
              break;
            case StageSeriesThreadPolicy::NonMainMax:
              threadAccepted =
                  mainTid != 0u && timing.threadId != mainTid;
              break;
            case StageSeriesThreadPolicy::AnySum:
              threadAccepted = true;
              break;
          }
          if (!threadAccepted)
            continue;
          if (def.metric == StageSeriesMetric::GpuTimestamp &&
              timing.gpuCount == 0u) {
            continue;
          }
          noteStageThread(column, timing.threadId);

          const double value =
              def.metric == StageSeriesMetric::GpuTimestamp
                  ? timing.gpuMs : timing.cpuMs;
          if (def.threadPolicy == StageSeriesThreadPolicy::NonMainMax) {
            row.values[column] = row.hasValue[column]
                ? std::max(row.values[column], value) : value;
          } else {
            row.values[column] += value;
          }
          row.hasValue[column] = true;
          stageDomainObserved[column] = true;
        }
      }
      stageRows.push_back(std::move(row));
    }

    std::array<bool, kStageSeriesColumnCount> stageColumnAvailable = {};
    for (size_t column = 0u; column < kStageSeriesColumnCount; ++column) {
      stageColumnAvailable[column] =
          !stageMatchedPaths[column].empty() &&
          stageDomainObserved[column];
      std::sort(stageMatchedPaths[column].begin(),
                stageMatchedPaths[column].end());
      std::sort(stageThreadIdsSeen[column].begin(),
                stageThreadIdsSeen[column].end());
    }

    json << "  \"stageSeriesColumns\": [\"epoch\"";
    for (const auto& def : stageColumnDefs)
      json << ", \"" << def.key << "\"";
    json << "],\n";
    json << "  \"stageSeriesContract\": {\n";
    json << "    \"unit\": \"ms\",\n";
    json << "    \"alignment\": \"rows share selected-frame order and epoch "
            "with frameSeries\",\n";
    json << "    \"additivity\": \"independent inclusive or overlay "
            "observations; never sum or stack columns and never derive a "
            "frame residual from them\",\n";
    json << "    \"threading\": \"main and non-main scope walls are separate; "
            "non-main columns use the maximum single-thread wall rather than "
            "a cross-thread sum\",\n";
    json << "    \"missing\": \"null means unavailable in this report, or no "
            "completed GPU timestamp for that frame; zero means an available "
            "CPU/phase channel had no matching call in that frame\",\n";
    json << "    \"presentCoverage\": \"PresentTransition and "
            "PresentEntryPreEndFrame end before PresentImage and "
            "SyncFrameLatency; neither is total Present time\"\n";
    json << "  },\n";
    json << "  \"stageSeriesMetadata\": [\n";
    for (size_t column = 0u; column < kStageSeriesColumnCount; ++column) {
      if (column != 0u)
        json << ",\n";
      const auto& def = stageColumnDefs[column];
      json << "    {\"column\": \"" << def.key
           << "\", \"label\": \"" << def.label
           << "\", \"kind\": \"" << def.kind
           << "\", \"threadDomain\": \"" << def.threadDomain
           << "\", \"aggregation\": \"" << def.aggregation
           << "\", \"isOverlay\": " << (def.overlay ? "true" : "false")
           << ", \"available\": "
           << (stageColumnAvailable[column] ? "true" : "false")
           << ", \"matchedPaths\": [";
      for (size_t i = 0u; i < stageMatchedPaths[column].size(); ++i) {
        if (i != 0u)
          json << ", ";
        json << "\"" << stageMatchedPaths[column][i] << "\"";
      }
      json << "], \"threadIdsSeen\": [";
      for (size_t i = 0u; i < stageThreadIdsSeen[column].size(); ++i) {
        if (i != 0u)
          json << ", ";
        json << stageThreadIdsSeen[column][i];
      }
      json << "]}";
    }
    json << "\n  ],\n";
    json << "  \"stageSeries\": [\n";
    for (size_t frameOrdinal = 0u; frameOrdinal < stageRows.size();
         ++frameOrdinal) {
      if (frameOrdinal != 0u)
        json << ",\n";
      const auto& row = stageRows[frameOrdinal];
      json << "    [" << row.epoch;
      for (size_t column = 0u; column < kStageSeriesColumnCount; ++column) {
        json << ", ";
        if (!stageColumnAvailable[column] ||
            (stageColumnDefs[column].metric ==
                 StageSeriesMetric::GpuTimestamp &&
             !row.hasValue[column])) {
          json << "null";
        } else {
          json << row.values[column];
        }
      }
      json << "]";
    }
    json << "\n  ],\n";

    // section 百分位：按帧聚合（跨线程求和），从帧历史取经验 P50/P95。
    std::unordered_map<uint32_t, std::vector<double>> perSectionFrames;
    for (const auto *f : frames) {
      std::unordered_map<uint32_t, double> perFrame;
      for (const auto &s : f->sections)
        perFrame[s.id] += s.cpuMs;
      for (const auto &kv : perFrame)
        perSectionFrames[kv.first].push_back(kv.second);
    }
    auto percentileOf = [](std::vector<double> &values, double q) {
      if (values.empty())
        return 0.0;
      std::sort(values.begin(), values.end());
      const double pos = q * static_cast<double>(values.size() - 1);
      const size_t lo = static_cast<size_t>(pos);
      const size_t hi = std::min(lo + 1, values.size() - 1);
      const double frac = pos - static_cast<double>(lo);
      return values[lo] * (1.0 - frac) + values[hi] * frac;
    };
    json << "  \"sectionPercentiles\": {\n";
    bool firstPct = true;
    for (auto &kv : perSectionFrames) {
      if (kv.first >= snapshot.sections.size())
        continue;
      if (!firstPct)
        json << ",\n";
      firstPct = false;
      const double p50 = percentileOf(kv.second, 0.50);
      const double p95 = percentileOf(kv.second, 0.95);
      json << "    \"" << snapshot.sections[kv.first].path
           << "\": {\"p50\": " << p50 << ", \"p95\": " << p95 << "}";
    }
    json << "\n  }\n";
  }

  json << "}";
  return json.str();
}

void War3PerfMonitor::exportHtmlReport(const std::string &outputPath) {
  if (m_shutdownStarted.load(std::memory_order_acquire))
    return;

  flushCurrentThreadCpuDeltas();

  ExportJob job = {};
  job.finalPath = resolveExportPath(outputPath);
  job.windowSec = resolveReportWindowSec();
  job.isAuto = outputPath == "war3_perf_report_auto.html";
  {
    std::lock_guard lock(m_mutex);
    job.snapshot = captureExportSnapshotLocked();
    war3dbg::Print("DXVK War3Perf: exportHtmlReport called, frames=%zu\n",
                   job.snapshot.frameHistory.size());
  }

  enqueueExportJob(std::move(job));
}

void War3PerfMonitor::processExportJob(const ExportJob &job) {
  war3dbg::Print("DXVK War3Perf: Writing to %s\n", job.finalPath.c_str());

  std::string jsonData = generateJsonDataFromSnapshot(job.snapshot, job.windowSec);
  std::ostringstream html;
  html << kWar3PerfReportHtmlHead;
  html << jsonData;
  html << kWar3PerfReportHtmlTail;

  // 原子写：先写 .tmp，再 MoveFileEx 替换，失败时保留旧报告。
  const std::string tmpPath = job.finalPath + ".tmp";
  bool written = false;
  {
    std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
    if (file.is_open()) {
      file << html.str();
      file.flush();
      written = file.good();
    }
  }
  if (written) {
    if (MoveFileExA(tmpPath.c_str(), job.finalPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      war3dbg::Print("DXVK War3Perf: Report exported to %s\n",
                     job.finalPath.c_str());
    } else {
      war3dbg::Print("DXVK War3Perf: Failed to move report into place %s (err=%lu)\n",
                     job.finalPath.c_str(), (unsigned long)GetLastError());
      DeleteFileA(tmpPath.c_str());
    }
  } else {
    war3dbg::Print("DXVK War3Perf: Failed to write report temp file %s (old file kept)\n",
                   tmpPath.c_str());
    DeleteFileA(tmpPath.c_str());
  }
}

void War3PerfMonitor::cleanupTempFolder() {
  const std::string baseDir = getWarVkBaseDir();
  if (baseDir.empty())
    return;
  const std::string tempDir = baseDir + "Temp\\";

  WIN32_FIND_DATAA ffd = {};
  HANDLE hFind = FindFirstFileA((tempDir + "*").c_str(), &ffd);
  if (hFind == INVALID_HANDLE_VALUE)
    return;

  do {
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    const std::string filePath = tempDir + ffd.cFileName;
    DeleteFileA(filePath.c_str());
  } while (FindNextFileA(hFind, &ffd));
  FindClose(hFind);
}

} // namespace dxvk::war3
