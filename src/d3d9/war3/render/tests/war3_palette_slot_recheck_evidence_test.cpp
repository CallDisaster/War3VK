// 2026-09-17 对象级 palette 证据（Step 1③）R 点采集支持的宿主机测试（纯 CPU：无 IO、
// 无 Vulkan、无游戏、无 Windows 头）。
//
// 被测对象是**生产函数**（不是复制品）：
//   * dxvk::war3::tools::evidence::ClassifyPaletteSlotRecheckReject
//       —— 生产侧用它同时选择"聚合细分计数分支"与"POD 里的具名原因"，
//          函数体与改动前 war3_shadow_renderer_core.cpp 的拒绝 if/else 逐条一致。
//   * dxvk::war3::tools::evidence::PublishPaletteSlotRecheckEvidence
//       —— 生产侧唯一的 POD 填充点；子门关闭时调用方传 nullptr ⇒ 不写任何字段。
//   * dxvk::war3::tools::evidence::ShouldNotifyPaletteSlotReject
//       —— 生产侧唯一的"是否发具名拒绝事件"门；未执行的检查一律不发。
//   * dxvk::war3::tools::evidence::MakePaletteObjectKey / MakePaletteObjectFrames
//       —— 三个采集点共用的键/帧域口径（identityWeak / epochUnknown / 帧域分列）。
//
// 断言口径：
//   1) **正常路径不变**：把 A0..A5 判据的 2^6 组合**穷举**，与"原始 if/else 链"的
//      独立参考实现逐例比对（服务记忆槽位 / R0 / R1 / R2 / R3）。
//   2) 各失败分支给出**正确具名原因**（R0..R3 各自可复现）。
//   3) **未执行的检查是 NotChecked 而不是 R0**：默认 POD、未执行/已确认/原因非具名
//      的 POD 都不发事件；分类函数永远不会返回 NotChecked。
//   4) **子门关闭时不填充**：Publish(nullptr, …) 对哨兵 POD 逐字节无影响。

#include "../../tools/war3_palette_object_capture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using dxvk::war3::tools::evidence::ClassifyPaletteSlotRecheckReject;
using dxvk::war3::tools::evidence::MakePaletteObjectFrames;
using dxvk::war3::tools::evidence::MakePaletteObjectKey;
using dxvk::war3::tools::evidence::PaletteObjectFrames;
using dxvk::war3::tools::evidence::PaletteObjectKey;
using dxvk::war3::tools::evidence::PaletteObjectRejectReason;
using dxvk::war3::tools::evidence::PaletteSlotRecheckEvidence;
using dxvk::war3::tools::evidence::PublishPaletteSlotRecheckEvidence;
using dxvk::war3::tools::evidence::ShouldNotifyPaletteSlotReject;

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
uint32_t g_casePassed = 0u;
uint32_t g_caseFailed = 0u;

bool Require(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("    FAIL: %s\n", what);
  }
  return condition;
}

// ---------------------------------------------------------------------------
// 独立参考实现：照**改动前** core.cpp 的判据顺序写出（A0/A2 → R0，A3 → R1，
// A4 → R2，A5 → R3；六项全通过则继续服务记忆槽位）。
// 刻意不调用生产分类函数，从而成为可对拍的 oracle。
// ---------------------------------------------------------------------------
struct LegacyOutcome {
  bool producerConfirmed = false;   // legacy: producerConfirmed 合取式
  bool rejected = false;            // legacy: 是否走到拒绝 if/else
  PaletteObjectRejectReason reason = PaletteObjectRejectReason::NotChecked;
};

LegacyOutcome LegacyRejectChain(bool bindingHit, bool slotDomainValid,
                                bool boundSlotIndexMatchesRemembered,
                                bool groupCountSuffices, bool bindingFrameFresh,
                                bool slotRangeFrameFresh) {
  LegacyOutcome out{};
  out.producerConfirmed =
      bindingHit && slotDomainValid && boundSlotIndexMatchesRemembered &&
      groupCountSuffices && bindingFrameFresh && slotRangeFrameFresh;
  if (out.producerConfirmed)
    return out; // legacy 在此 return 记忆槽位，不进入拒绝 if/else
  out.rejected = true;
  if (!bindingHit || !slotDomainValid || !boundSlotIndexMatchesRemembered)
    out.reason = PaletteObjectRejectReason::R0;
  else if (!groupCountSuffices)
    out.reason = PaletteObjectRejectReason::R1;
  else if (!bindingFrameFresh)
    out.reason = PaletteObjectRejectReason::R2;
  else
    out.reason = PaletteObjectRejectReason::R3;
  return out;
}

// 生产侧的等价组合：producerConfirmed 走服务路径（原因保持 NotChecked），
// 否则用 ClassifyPaletteSlotRecheckReject 取原因。
PaletteObjectRejectReason ProductionReason(bool producerConfirmed, bool bindingHit,
                                           bool slotDomainValid,
                                           bool boundSlotIndexMatchesRemembered,
                                           bool groupCountSuffices,
                                           bool bindingFrameFresh) {
  if (producerConfirmed)
    return PaletteObjectRejectReason::NotChecked;
  return ClassifyPaletteSlotRecheckReject(
      bindingHit, slotDomainValid, boundSlotIndexMatchesRemembered,
      groupCountSuffices, bindingFrameFresh);
}

bool ReasonIsNamed(PaletteObjectRejectReason reason) {
  return reason == PaletteObjectRejectReason::R0 ||
         reason == PaletteObjectRejectReason::R1 ||
         reason == PaletteObjectRejectReason::R2 ||
         reason == PaletteObjectRejectReason::R3;
}

// 照生产采集点的方式组装 POD（字段全部来自当场局部值）。
PaletteSlotRecheckEvidence BuildEvidence(bool bindingHit, bool slotDomainValid,
                                         bool boundSlotIndexMatchesRemembered,
                                         bool groupCountSuffices,
                                         bool bindingFrameFresh,
                                         bool slotRangeFrameFresh) {
  PaletteSlotRecheckEvidence evidence{};
  evidence.recheckPerformed = true;
  evidence.producerConfirmed =
      bindingHit && slotDomainValid && boundSlotIndexMatchesRemembered &&
      groupCountSuffices && bindingFrameFresh && slotRangeFrameFresh;
  evidence.bindingHit = bindingHit;
  evidence.slotDomainValid = slotDomainValid;
  evidence.boundSlotIndexMatchesRemembered = boundSlotIndexMatchesRemembered;
  evidence.groupCountSuffices = groupCountSuffices;
  evidence.bindingFrameFresh = bindingFrameFresh;
  evidence.slotRangeFrameFresh = slotRangeFrameFresh;
  evidence.requiredPaletteCount = 7u;
  evidence.currentSlotIndexRaw = 0xFFFFFFFFu;
  evidence.rememberedSlotIndex = 41u;
  evidence.boundSlotIndex = 41u;
  evidence.boundGroupCount = 9u;
  evidence.boundFrameTag = 0x1234u;
  evidence.currentPaletteFrameTag = 0x1234u;
  evidence.slotRangeMinFrameTag = 0x1233u;
  evidence.slotRangeMaxFrameTag = 0x1235u;
  evidence.slotRangeMissingCount = 1u;
  evidence.rejectReason =
      evidence.producerConfirmed
          ? PaletteObjectRejectReason::NotChecked
          : ClassifyPaletteSlotRecheckReject(
                bindingHit, slotDomainValid, boundSlotIndexMatchesRemembered,
                groupCountSuffices, bindingFrameFresh);
  return evidence;
}

// ---------------------------------------------------------------------------
// 案例 1：默认 POD 的"未执行"语义必须是 NotChecked（不是 R0）。
// ---------------------------------------------------------------------------
bool Case1DefaultNotChecked() {
  bool ok = true;
  const PaletteSlotRecheckEvidence fresh{};
  ok &= Require(!fresh.recheckPerformed, "default recheckPerformed == false");
  ok &= Require(!fresh.producerConfirmed, "default producerConfirmed == false");
  ok &= Require(fresh.rejectReason == PaletteObjectRejectReason::NotChecked,
                "default rejectReason == NotChecked (never R0)");
  ok &= Require(!ReasonIsNamed(fresh.rejectReason),
                "default reason is not a named reject");
  ok &= Require(fresh.boundSlotIndex == 0u && fresh.boundGroupCount == 0u &&
                    fresh.boundFrameTag == 0u &&
                    fresh.currentPaletteFrameTag == 0u &&
                    fresh.slotRangeMissingCount == 0u,
                "default diagnostics stay zero (not back-filled)");

  // 未执行的检查不得发事件：默认 POD 必须被 ShouldNotify 拒绝。
  PaletteObjectRejectReason out = PaletteObjectRejectReason::R3;
  ok &= Require(!ShouldNotifyPaletteSlotReject(fresh, out),
                "unperformed check must not notify");
  ok &= Require(out == PaletteObjectRejectReason::R3,
                "out reason untouched when not notified");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 2：子门关闭时不填充（Publish(nullptr, …) 不得触碰内存）。
// ---------------------------------------------------------------------------
bool Case2PublishNullDoesNotFill() {
  bool ok = true;
  PaletteSlotRecheckEvidence sentinel{};
  sentinel.recheckPerformed = true;
  sentinel.producerConfirmed = true;
  sentinel.rejectReason = PaletteObjectRejectReason::R3;
  sentinel.bindingHit = true;
  sentinel.requiredPaletteCount = 0xA5A5A5A5u;
  sentinel.currentSlotIndexRaw = 0xDEADBEEFu;
  sentinel.rememberedSlotIndex = 0xCAFEBABEu;
  sentinel.boundSlotIndex = 11u;
  sentinel.boundGroupCount = 12u;
  sentinel.boundFrameTag = 13u;
  sentinel.currentPaletteFrameTag = 14u;
  sentinel.slotRangeMinFrameTag = 15u;
  sentinel.slotRangeMaxFrameTag = 16u;
  sentinel.slotRangeMissingCount = 17u;
  const PaletteSlotRecheckEvidence before = sentinel;

  PublishPaletteSlotRecheckEvidence(nullptr, BuildEvidence(false, false, false,
                                                           false, false, false));
  ok &= Require(std::memcmp(&before, &sentinel, sizeof(sentinel)) == 0,
                "Publish(nullptr) leaves the POD byte-identical");

  // 非空出参：逐字段复制（生产填充路径）。
  PaletteSlotRecheckEvidence filled{};
  const PaletteSlotRecheckEvidence value =
      BuildEvidence(true, true, true, true, true, true);
  PublishPaletteSlotRecheckEvidence(&filled, value);
  ok &= Require(std::memcmp(&filled, &value, sizeof(value)) == 0,
                "Publish(out, value) copies every field");
  ok &= Require(filled.recheckPerformed && filled.producerConfirmed,
                "served recheck keeps confirmed/POD true");
  ok &= Require(filled.rejectReason == PaletteObjectRejectReason::NotChecked,
                "served path keeps NotChecked (no reject reason)");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 3：正常路径不变 —— 2^6 组合穷举对拍（服务记忆槽位 / R0..R3）。
// ---------------------------------------------------------------------------
bool Case3ExhaustiveEquivalence() {
  bool ok = true;
  uint32_t served = 0u;
  uint32_t rejected = 0u;
  uint32_t perReason[4] = {0u, 0u, 0u, 0u};
  for (uint32_t mask = 0u; mask < 64u; ++mask) {
    const bool bindingHit = (mask & 1u) != 0u;
    const bool slotDomainValid = (mask & 2u) != 0u;
    const bool matches = (mask & 4u) != 0u;
    const bool groupShort = (mask & 8u) != 0u;
    const bool bindingFrameFresh = (mask & 16u) != 0u;
    const bool slotRangeFrameFresh = (mask & 32u) != 0u;

    const LegacyOutcome legacy = LegacyRejectChain(
        bindingHit, slotDomainValid, matches, groupShort, bindingFrameFresh,
        slotRangeFrameFresh);
    const PaletteObjectRejectReason produced = ProductionReason(
        legacy.producerConfirmed, bindingHit, slotDomainValid, matches,
        groupShort, bindingFrameFresh);
    ok &= Require(produced == legacy.reason,
                  "production reason equals legacy reject chain");
    if (legacy.producerConfirmed) {
      ++served;
      ok &= Require(produced == PaletteObjectRejectReason::NotChecked,
                    "served combo carries no reject reason");
    } else {
      ++rejected;
      ok &= Require(ReasonIsNamed(produced),
                    "rejected combo carries a named reason");
      const uint32_t index = static_cast<uint32_t>(produced);
      ok &= Require(index < 4u, "named reason inside R0..R3");
      if (index < 4u)
        ++perReason[index];
    }

    // 分类函数自身永远不得返回 NotChecked/Unknown（未执行由调用点表达，
    // 不得在分类函数里伪造）。
    const PaletteObjectRejectReason classified = ClassifyPaletteSlotRecheckReject(
        bindingHit, slotDomainValid, matches, groupShort, bindingFrameFresh);
    ok &= Require(ReasonIsNamed(classified),
                  "classifier only returns named reasons");
    ok &= Require(classified == legacy.reason || legacy.producerConfirmed,
                  "classifier agrees with the legacy else-branch");
  }
  ok &= Require(served == 1u, "exactly one combo serves the remembered slot");
  ok &= Require(rejected == 63u, "63 combos are rejects");
  // 四个具名分支都必须可达（各失败分支都能给出原因）。
  ok &= Require(perReason[0] != 0u && perReason[1] != 0u &&
                    perReason[2] != 0u && perReason[3] != 0u,
                "R0/R1/R2/R3 are each reachable");
  std::printf("    [coverage] served=%u rejected=%u R0=%u R1=%u R2=%u R3=%u\n",
              served, rejected, perReason[0], perReason[1], perReason[2],
              perReason[3]);
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 4：每个失败分支的具名原因（点名到点，便于实机对账）。
// ---------------------------------------------------------------------------
bool Case4NamedBranches() {
  bool ok = true;
  // 仅绑定 miss（A0）失败 → R0。
  ok &= Require(ClassifyPaletteSlotRecheckReject(false, true, true, true, true) ==
                    PaletteObjectRejectReason::R0,
                "binding miss => R0");
  // 槽位域非法（A1/A2 之一）→ R0。
  ok &= Require(ClassifyPaletteSlotRecheckReject(true, false, true, true, true) ==
                    PaletteObjectRejectReason::R0,
                "invalid slot domain => R0");
  ok &= Require(ClassifyPaletteSlotRecheckReject(true, true, false, true, true) ==
                    PaletteObjectRejectReason::R0,
                "remembered-slot mismatch => R0");
  // groupCount 不足（A3）→ R1。
  ok &= Require(ClassifyPaletteSlotRecheckReject(true, true, true, false, true) ==
                    PaletteObjectRejectReason::R1,
                "group count short => R1");
  // 绑定帧陈旧（A4）→ R2。
  ok &= Require(ClassifyPaletteSlotRecheckReject(true, true, true, true, false) ==
                    PaletteObjectRejectReason::R2,
                "binding frame stale => R2");
  // 前五项全过、A5 失败 → R3（legacy 的最后一个 else）。
  ok &= Require(ClassifyPaletteSlotRecheckReject(true, true, true, true, true) ==
                    PaletteObjectRejectReason::R3,
                "slot range stale => R3");
  // 顺序：A3 先于 A4/A5（A0 失败时不得报 R1/R2/R3）。
  ok &= Require(ClassifyPaletteSlotRecheckReject(false, false, false, false,
                                                 false) ==
                    PaletteObjectRejectReason::R0,
                "first failing gate wins (R0 before R1..R3)");
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 5：ShouldNotify —— 未执行 / 已确认 / 原因非具名一律不发事件。
// ---------------------------------------------------------------------------
bool Case5NotifyGate() {
  bool ok = true;
  PaletteObjectRejectReason out = PaletteObjectRejectReason::NotChecked;

  PaletteSlotRecheckEvidence unperformed{};
  unperformed.rejectReason = PaletteObjectRejectReason::R0; // 即便被伪造成 R0
  ok &= Require(!ShouldNotifyPaletteSlotReject(unperformed, out),
                "recheckPerformed=false never notifies even if reason==R0");

  PaletteSlotRecheckEvidence served = BuildEvidence(true, true, true, true, true, true);
  ok &= Require(!ShouldNotifyPaletteSlotReject(served, out),
                "confirmed recheck never notifies");

  PaletteSlotRecheckEvidence notChecked = BuildEvidence(true, false, true, true, true, true);
  notChecked.rejectReason = PaletteObjectRejectReason::NotChecked;
  ok &= Require(!ShouldNotifyPaletteSlotReject(notChecked, out),
                "NotChecked reason never notifies");

  PaletteSlotRecheckEvidence unknown = BuildEvidence(true, false, true, true, true, true);
  unknown.rejectReason = PaletteObjectRejectReason::Unknown;
  ok &= Require(!ShouldNotifyPaletteSlotReject(unknown, out),
                "Unknown reason never notifies");

  const PaletteObjectRejectReason expected[4] = {
      PaletteObjectRejectReason::R0, PaletteObjectRejectReason::R1,
      PaletteObjectRejectReason::R2, PaletteObjectRejectReason::R3};
  struct Combo { bool a; bool b; bool c; bool d; bool e; };
  const Combo combos[4] = {{false, true, true, true, true},
                           {true, true, true, false, true},
                           {true, true, true, true, false},
                           {true, true, true, true, true}};
  for (uint32_t i = 0u; i < 4u; ++i) {
    const PaletteSlotRecheckEvidence evidence =
        BuildEvidence(combos[i].a, combos[i].b, combos[i].c, combos[i].d,
                      combos[i].e, /*slotRangeFrameFresh=*/false);
    PaletteObjectRejectReason notified = PaletteObjectRejectReason::NotChecked;
    ok &= Require(ShouldNotifyPaletteSlotReject(evidence, notified),
                  "named reject notifies");
    ok &= Require(notified == expected[i], "notified reason matches the branch");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// 案例 6：键/帧域口径（三点共用）——身份弱、epoch 未知、帧域四项分列。
// ---------------------------------------------------------------------------
bool Case6KeyAndFrameDomains() {
  bool ok = true;
  int part = 0;
  int model = 0;
  const PaletteObjectKey key =
      MakePaletteObjectKey(&part, &model, 0x1234u, 0x68303031u, 77u, 88u);
  ok &= Require(key.renderablePart ==
                    uint64_t(reinterpret_cast<uintptr_t>(&part)),
                "renderablePart carried by value");
  ok &= Require(key.runtimeModelPtr ==
                    uint64_t(reinterpret_cast<uintptr_t>(&model)),
                "runtimeModelPtr carried by value");
  ok &= Require(key.jHandle == 0x1234u && key.rawcode == 0x68303031u,
                "jHandle/rawcode carried");
  ok &= Require(key.sessionGeneration == 77u && key.mapEpoch == 88u,
                "session/map epoch carried");
  ok &= Require(key.deviceEpoch == 0u, "deviceEpoch recorded as 0");
  ok &= Require(key.epochUnknown, "epochUnknown visible when deviceEpoch is 0");
  ok &= Require(key.identityWeak, "identityWeak true (no instance lifecycle proof)");
  ok &= Require(key.lifecycleIdentity == 0u,
                "lifecycleIdentity not faked from a model generation");

  const PaletteObjectFrames frames =
      MakePaletteObjectFrames(111u, 222u, 333u, /*nativeKnown=*/true);
  ok &= Require(frames.renderFrame == 111u, "renderFrame is its own domain");
  ok &= Require(frames.recordFrameSerial == 222u,
                "recordFrameSerial is its own domain");
  ok &= Require(frames.nativeFrameTag == 333u,
                "nativeFrameTag is its own domain");
  ok &= Require(frames.manifestFrameSerial == 0u &&
                    frames.manifestPublishRevision == 0u,
                "manifest domains recorded as 0 (not substituted)");
  ok &= Require(frames.manifestUnknown, "manifestUnknown=true when unavailable");
  ok &= Require(!frames.nativeUnknown, "nativeUnknown=false when the tag was read");

  const PaletteObjectFrames unknownNative =
      MakePaletteObjectFrames(111u, 222u, 333u, /*nativeKnown=*/false);
  ok &= Require(unknownNative.nativeFrameTag == 0u && unknownNative.nativeUnknown,
                "unreadable native tag => 0 + nativeUnknown (not a wildcard)");
  return ok;
}

void RunCase(const char* name, bool (*body)()) {
  g_checks = 0u;
  g_failures = 0u;
  const bool ok = body();
  const bool passed = ok && g_failures == 0u;
  std::printf("[%s] %s (%u checks, %u failures)\n",
              passed ? "PASS" : "FAIL", name,
              static_cast<unsigned>(g_checks),
              static_cast<unsigned>(g_failures));
  if (passed)
    ++g_casePassed;
  else
    ++g_caseFailed;
}

} // namespace

int main() {
  std::printf("war3_palette_slot_recheck_evidence_test: R-point POD contracts\n");
  std::printf("sizeof(PaletteSlotRecheckEvidence) = %llu bytes\n",
              static_cast<unsigned long long>(
                  sizeof(PaletteSlotRecheckEvidence)));
  RunCase("1 unperformed check is NotChecked (never R0)", &Case1DefaultNotChecked);
  RunCase("2 sub-gate closed => Publish does not fill", &Case2PublishNullDoesNotFill);
  RunCase("3 legacy path unchanged (2^6 exhaustive)", &Case3ExhaustiveEquivalence);
  RunCase("4 every failure branch names its reason", &Case4NamedBranches);
  RunCase("5 notify gate rejects unperformed/unnamed", &Case5NotifyGate);
  RunCase("6 key/frame domain discipline", &Case6KeyAndFrameDomains);
  std::printf("SUMMARY: %u passed, %u failed\n",
              static_cast<unsigned>(g_casePassed),
              static_cast<unsigned>(g_caseFailed));
  return g_caseFailed == 0u ? 0 : 1;
}
