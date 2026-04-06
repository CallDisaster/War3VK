#include "war3_queue_takeover_policy.h"

#include "../../d3d9_war3_debug.h"

#include "../core/war3_internal_test_config.h"

#include <algorithm>
#include <atomic>

namespace dxvk::war3::hooks {

struct ConservativeTakeoverStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> rejectDisabled{0};
  std::atomic<uint64_t> rejectNoOpaquePtr{0};
  std::atomic<uint64_t> rejectTransparent{0};
  std::atomic<uint64_t> rejectTransparentDisabled{0};
  std::atomic<uint64_t> rejectTransparentTooMany{0};
  std::atomic<uint64_t> rejectTransparentTakeoverRange{0};
  std::atomic<uint64_t> rejectTransparentPrereqMissing{0};
  std::atomic<uint64_t> rejectOpaqueTooSmall{0};
  std::atomic<uint64_t> rejectOpaqueTooSmallWithTransparent{0};
  std::atomic<uint64_t> rejectOpaqueTooLarge{0};
  std::atomic<uint64_t> lastLoggedCall{0};
};

static ConservativeTakeoverStats g_conservativeTakeoverStats;

static void MaybeLogConservativeTakeoverStats() {
  if constexpr (!dxvk::war3::internal::
                    kNativeQueueTakeoverConservativeStatsLogging) {
    return;
  }

  constexpr uint64_t kInterval =
      dxvk::war3::internal::kNativeQueueTakeoverConservativeStatsIntervalCalls;
  if (kInterval == 0)
    return;

  const uint64_t calls =
      g_conservativeTakeoverStats.calls.load(std::memory_order_relaxed);
  if (calls == 0 || (calls % kInterval) != 0)
    return;

  const uint64_t lastLogged =
      g_conservativeTakeoverStats.lastLoggedCall.exchange(
          calls, std::memory_order_relaxed);
  if (lastLogged == calls)
    return;

  const uint64_t accepted =
      g_conservativeTakeoverStats.accepted.load(std::memory_order_relaxed);
  const uint64_t rejectDisabled =
      g_conservativeTakeoverStats.rejectDisabled.load(
          std::memory_order_relaxed);
  const uint64_t rejectNoOpaquePtr =
      g_conservativeTakeoverStats.rejectNoOpaquePtr.load(
          std::memory_order_relaxed);
  const uint64_t rejectTransparent =
      g_conservativeTakeoverStats.rejectTransparent.load(
          std::memory_order_relaxed);
  const uint64_t rejectTransparentDisabled =
      g_conservativeTakeoverStats.rejectTransparentDisabled.load(
          std::memory_order_relaxed);
  const uint64_t rejectTransparentTooMany =
      g_conservativeTakeoverStats.rejectTransparentTooMany.load(
          std::memory_order_relaxed);
  const uint64_t rejectTransparentTakeoverRange =
      g_conservativeTakeoverStats.rejectTransparentTakeoverRange.load(
          std::memory_order_relaxed);
  const uint64_t rejectTransparentPrereqMissing =
      g_conservativeTakeoverStats.rejectTransparentPrereqMissing.load(
          std::memory_order_relaxed);
  const uint64_t rejectOpaqueTooSmall =
      g_conservativeTakeoverStats.rejectOpaqueTooSmall.load(
          std::memory_order_relaxed);
  const uint64_t rejectOpaqueTooSmallWithTransparent =
      g_conservativeTakeoverStats.rejectOpaqueTooSmallWithTransparent.load(
          std::memory_order_relaxed);
  const uint64_t rejectOpaqueTooLarge =
      g_conservativeTakeoverStats.rejectOpaqueTooLarge.load(
          std::memory_order_relaxed);
  const double acceptPct =
      calls > 0
          ? (100.0 * static_cast<double>(accepted) / static_cast<double>(calls))
          : 0.0;

  war3dbg::Print(
      "DXVK War3Hook: TakeoverConservative calls=%llu accepted=%llu (%.2f%%) "
      "reject{disabled=%llu noOpaquePtr=%llu transparent=%llu "
      "transparentDisabled=%llu transparentTooMany=%llu "
      "transparentTakeoverRange=%llu transparentPrereqMissing=%llu "
      "tooSmall=%llu tooSmallWithTrans=%llu tooLarge=%llu}\n",
      static_cast<unsigned long long>(calls),
      static_cast<unsigned long long>(accepted), acceptPct,
      static_cast<unsigned long long>(rejectDisabled),
      static_cast<unsigned long long>(rejectNoOpaquePtr),
      static_cast<unsigned long long>(rejectTransparent),
      static_cast<unsigned long long>(rejectTransparentDisabled),
      static_cast<unsigned long long>(rejectTransparentTooMany),
      static_cast<unsigned long long>(rejectTransparentTakeoverRange),
      static_cast<unsigned long long>(rejectTransparentPrereqMissing),
      static_cast<unsigned long long>(rejectOpaqueTooSmall),
      static_cast<unsigned long long>(rejectOpaqueTooSmallWithTransparent),
      static_cast<unsigned long long>(rejectOpaqueTooLarge));
}

bool HasTransparentTakeoverPrerequisites(const War3QueueTakeoverContext &ctx) {
  // 透明接管至少要满足：
  // 1) 透明队列地址完整；
  // 2) 透明分发函数(type0-4)全部可用。
  return ctx.transparentCountPtr && ctx.transparentArrayBasePtr &&
         ctx.transparentSortedPtrs && ctx.transparentDispatchType0 &&
         ctx.transparentDispatchType1 && ctx.transparentDispatchType2 &&
         ctx.transparentDispatchType3 && ctx.transparentDispatchType4;
}

bool ShouldUseConservativeQueueTakeover(const War3QueueTakeoverContext &ctx,
                                        uint32_t *outOpaqueCount,
                                        uint32_t *outTransparentCount) {
  if (outOpaqueCount)
    *outOpaqueCount = 0;
  if (outTransparentCount)
    *outTransparentCount = 0;

  g_conservativeTakeoverStats.calls.fetch_add(1, std::memory_order_relaxed);

  if constexpr (!dxvk::war3::internal::kNativeQueueTakeoverConservativeEnabled) {
    g_conservativeTakeoverStats.rejectDisabled.fetch_add(
        1, std::memory_order_relaxed);
    MaybeLogConservativeTakeoverStats();
    return false;
  }

  if (!ctx.opaqueCountPtr) {
    g_conservativeTakeoverStats.rejectNoOpaquePtr.fetch_add(
        1, std::memory_order_relaxed);
    MaybeLogConservativeTakeoverStats();
    return false;
  }

  const uint32_t opaqueCount = *ctx.opaqueCountPtr;
  const uint32_t transparentCount =
      ctx.transparentCountPtr ? *ctx.transparentCountPtr : 0u;

  uint32_t minOpaqueForTakeover =
      dxvk::war3::internal::kNativeQueueTakeoverConservativeMinOpaque;
  if constexpr (dxvk::war3::internal::
                    kNativeQueueTakeoverConservativeEnableSmallOpaqueNoTransparent) {
    if (transparentCount == 0u) {
      minOpaqueForTakeover = (std::min)(
          minOpaqueForTakeover,
          dxvk::war3::internal::
              kNativeQueueTakeoverConservativeMinOpaqueNoTransparent);
    }
  }

  uint32_t maxTransparentForTakeover =
      dxvk::war3::internal::
          kNativeQueueTakeoverConservativeMaxTransparentForTakeover;
  if (opaqueCount >=
      dxvk::war3::internal::
          kNativeQueueTakeoverConservativeHighOpaqueThreshold) {
    maxTransparentForTakeover = (std::max)(
        maxTransparentForTakeover,
        dxvk::war3::internal::
            kNativeQueueTakeoverConservativeMaxTransparentForTakeoverHighOpaque);
  }
  maxTransparentForTakeover =
      (std::min)(maxTransparentForTakeover,
                 dxvk::war3::internal::kNativeQueueTakeoverConservativeMaxTransparent);

  if (outOpaqueCount)
    *outOpaqueCount = opaqueCount;
  if (outTransparentCount)
    *outTransparentCount = transparentCount;

  if (transparentCount != 0u) {
    if constexpr (dxvk::war3::internal::
                      kNativeQueueTakeoverConservativeRequireNoTransparent) {
      g_conservativeTakeoverStats.rejectTransparent.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }

    if constexpr (!dxvk::war3::internal::
                      kNativeQueueTakeoverConservativeAllowTransparent) {
      g_conservativeTakeoverStats.rejectTransparentDisabled.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }

    if (transparentCount >
        dxvk::war3::internal::kNativeQueueTakeoverConservativeMaxTransparent) {
      g_conservativeTakeoverStats.rejectTransparentTooMany.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }

    if (transparentCount > maxTransparentForTakeover) {
      g_conservativeTakeoverStats.rejectTransparentTakeoverRange.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }

    if (!HasTransparentTakeoverPrerequisites(ctx)) {
      g_conservativeTakeoverStats.rejectTransparentPrereqMissing.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }

    if (opaqueCount <
        dxvk::war3::internal::
            kNativeQueueTakeoverConservativeMinOpaqueWhenTransparent) {
      g_conservativeTakeoverStats.rejectOpaqueTooSmallWithTransparent.fetch_add(
          1, std::memory_order_relaxed);
      MaybeLogConservativeTakeoverStats();
      return false;
    }
  }

  if (opaqueCount < minOpaqueForTakeover) {
    g_conservativeTakeoverStats.rejectOpaqueTooSmall.fetch_add(
        1, std::memory_order_relaxed);
    MaybeLogConservativeTakeoverStats();
    return false;
  }

  if (opaqueCount >
      dxvk::war3::internal::kNativeQueueTakeoverConservativeMaxOpaque) {
    g_conservativeTakeoverStats.rejectOpaqueTooLarge.fetch_add(
        1, std::memory_order_relaxed);
    MaybeLogConservativeTakeoverStats();
    return false;
  }

  g_conservativeTakeoverStats.accepted.fetch_add(1, std::memory_order_relaxed);
  MaybeLogConservativeTakeoverStats();
  return true;
}

War3QueueTakeoverDecision
EvaluateQueueTakeoverDecision(const War3QueueTakeoverContext &ctx) {
  War3QueueTakeoverDecision decision = {};
  constexpr bool kFullTakeoverEnabled =
      dxvk::war3::internal::kNativeQueueTakeoverEnabled;
  constexpr bool kConservativeTakeoverEnabled =
      dxvk::war3::internal::kNativeQueueTakeoverConservativeEnabled;

  if constexpr (!kFullTakeoverEnabled && !kConservativeTakeoverEnabled) {
    decision.reason = War3QueueTakeoverReason::Disabled;
    return decision;
  }

  decision.opaqueCount = ctx.opaqueCountPtr ? *ctx.opaqueCountPtr : 0u;
  decision.transparentCount =
      ctx.transparentCountPtr ? *ctx.transparentCountPtr : 0u;

  bool useFullTakeover = kFullTakeoverEnabled;
  if constexpr (kFullTakeoverEnabled) {
    uint32_t minOpaque = dxvk::war3::internal::kNativeQueueTakeoverFullMinOpaque;
    if (decision.transparentCount > 0u) {
      minOpaque = (std::max)(
          minOpaque,
          dxvk::war3::internal::kNativeQueueTakeoverFullMinOpaqueWhenTransparent);
    }
    if (decision.opaqueCount < minOpaque) {
      useFullTakeover = false;
      decision.reason = War3QueueTakeoverReason::OpaqueBelowFullMin;
    }
  }

  if (useFullTakeover && decision.transparentCount > 0u) {
    uint32_t maxTransparent =
        dxvk::war3::internal::kNativeQueueTakeoverFullMaxTransparent;
    if (decision.opaqueCount >=
        dxvk::war3::internal::kNativeQueueTakeoverFullHighOpaqueThreshold) {
      maxTransparent =
          (std::max)(maxTransparent,
                     dxvk::war3::internal::
                         kNativeQueueTakeoverFullMaxTransparentHighOpaque);
    }
    if (decision.transparentCount > maxTransparent) {
      useFullTakeover = false;
      decision.reason = War3QueueTakeoverReason::TransparentAboveFullRange;
    }
  }

  if (useFullTakeover && decision.transparentCount > 0u) {
    if constexpr (dxvk::war3::internal::
                      kNativeQueueTakeoverUseNativeTransparentFlush) {
      if (!ctx.hasOriginalFlushTransparent) {
        useFullTakeover = false;
        decision.reason = War3QueueTakeoverReason::NativeTransparentFlushMissing;
      }
    } else if (!HasTransparentTakeoverPrerequisites(ctx)) {
      useFullTakeover = false;
      decision.reason = War3QueueTakeoverReason::TransparentPrereqMissing;
    }
  }

  if (useFullTakeover) {
    decision.mode = War3QueueTakeoverMode::Full;
    decision.reason = War3QueueTakeoverReason::FullAccepted;
    return decision;
  }

  if (kConservativeTakeoverEnabled &&
      ShouldUseConservativeQueueTakeover(ctx, nullptr, nullptr)) {
    decision.mode = War3QueueTakeoverMode::Conservative;
    decision.reason = War3QueueTakeoverReason::ConservativeAccepted;
    return decision;
  }

  decision.mode = War3QueueTakeoverMode::Fallback;
  if (decision.reason == War3QueueTakeoverReason::Unknown)
    decision.reason = War3QueueTakeoverReason::ConservativeRejected;
  return decision;
}

} // namespace dxvk::war3::hooks

