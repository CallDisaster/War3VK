// 2026-09-19 上级裁定：M3 从「锁文本」转为「执行这些语义」。
// 本测试**直接调用生产使用的 inline 交接函数**（war3_palette_object_diagnostics.h），
// 按生产顺序执行「选材诊断赋值 → 选择被替换 → 载荷拷贝 → S/D 消费」，
// 覆盖裁定要求的六个用例。纯 CPU，不加载游戏、不读原生内存。
#include "war3/render/war3_palette_object_diagnostics.h"
#include "war3/render/war3_skin_palette_selection.h"  // fail-closed 屏障（Usable）

#include <cstdint>
#include <cstdio>

using namespace dxvk::war3::render::palette_object;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool ok, const char* what) {
  if (ok) {
    g_passed++;
  } else {
    g_failed++;
    std::printf("FAIL: %s\n", what);
  }
}

// 生产中的按值诊断载荷（draw.paletteDiagnostics）。
struct Payload {
  Diagnostics verdict;
};

// 捕获调色板的身份构造（与 device.cpp 中证据点/选择点使用同一形式 ⇒ 身份可比）。
static SelectionIdentity Captured(uint32_t slot, uint32_t minTag, uint32_t maxTag) {
  return SelectionIdentity{0u, slot, minTag, maxTag};
}

int main() {
  // 用例 1：ready=true、tag=0 ⇒ S/D 不分叉；且**没有发生 fallback 就不记录 fallback**。
  {
    Diagnostics device;
    Payload shadow{};
    NoteLiveNativeSelected(device, Captured(7u, 0u, 0u));
    NoteFrameEvidence(device, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 0u, 0u));
    shadow.verdict = device;  // 载荷拷贝
    check(device.action == DrawAction::LiveNativeSelected,
          "case1: the action is still 'live native selected' (no fallback happened)");
    check(Verdict(device) == NativeFrameVerdict::Unknown,
          "case1: ready=true with tag=0 has no native proof => Unknown");
    // （原「副本与原件同 Verdict」是恒真断言，已删除；改用下面的替换性质。）
    // 非恒真性质：替换之后，**之前取下的副本**不得跟着变（S/D 消费的是当前描述）。
    const Diagnostics copiedEarlier = shadow.verdict;
    NoteSelectionReplaced(device, Captured(99u, 1u, 1u));
    check(Verdict(copiedEarlier) == NativeFrameVerdict::Unknown &&
              Verdict(device) == NativeFrameVerdict::Unknown,
          "case1: a replacement must not be followed by an earlier copy, and both are Unknown");
  }

  // 用例 2：packet 回退且 tag 非零 ⇒ 不得仅凭标签非零升级为本帧 live 证明。
  {
    Diagnostics device;
    Payload shadow{};
    NotePacketFallbackSelected(device, SelectionIdentity{2u, UINT32_MAX, 0x2Au, 0x2Au});
    shadow.verdict = device;
    check(device.action == DrawAction::PacketFallbackSelected,
          "case2: the fallback is recorded as the action it is");
    check(Verdict(shadow.verdict) == NativeFrameVerdict::Unknown,
          "case2: a nonzero packet tag never becomes a native frame proof");
  }

  // 用例 3：未检查 / 证据不匹配 / 已证明 —— 三种情况不得混为一谈。
  {
    Diagnostics unchecked;
    NoteLiveNativeSelected(unchecked, Captured(7u, 5u, 5u));
    check(Verdict(unchecked) == NativeFrameVerdict::Unknown, "case3: unchecked => Unknown");

    Diagnostics mismatched;
    NoteLiveNativeSelected(mismatched, Captured(7u, 5u, 5u));
    NoteFrameEvidence(mismatched, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(9u, 5u, 5u));  // 证据覆盖的是**另一份**选择
    check(Verdict(mismatched) == NativeFrameVerdict::Unknown,
          "case3: evidence covering another selection => Unknown");

    Diagnostics proven;
    NoteLiveNativeSelected(proven, Captured(7u, 5u, 5u));
    NoteFrameEvidence(proven, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 5u, 5u));
    check(Verdict(proven) == NativeFrameVerdict::KnownCurrent,
          "case3: a matching passed check => KnownCurrent");
  }

  // 用例 4：候选 A 有证明，随后换成 B ⇒ B 不得继承 A 的证明（走真实替换交接）。
  {
    Diagnostics device;
    Payload shadow{};
    NoteLiveNativeSelected(device, Captured(7u, 5u, 5u));
    NoteFrameEvidence(device, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 5u, 5u));
    check(Verdict(device) == NativeFrameVerdict::KnownCurrent, "case4: A is proven before the swap");
    NoteSelectionReplaced(device, Captured(11u, 9u, 9u));  // 换成 B
    shadow.verdict = device;
    check(Verdict(shadow.verdict) == NativeFrameVerdict::Unknown,
          "case4: B must not inherit A's proof");
  }

  // 用例 5：合法候选取得有效证明 ⇒ 正向路径确实可达（不得靠一律 Unknown 跑绿）。
  {
    Diagnostics device;
    NoteLiveNativeSelected(device, Captured(3u, 0x2Au, 0x2Au));
    NoteFrameEvidence(device, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 0x2Au,
                      Captured(3u, 0x2Au, 0x2Au));
    check(Verdict(device) == NativeFrameVerdict::KnownCurrent,
          "case5: the positive path is reachable with a valid proof");
  }

  // 用例 6：真正的 override 清空 vs 其它路径的默认载荷。
  {
    Diagnostics cleared;
    NoteLiveNativeSelected(cleared, Captured(7u, 5u, 5u));
    NoteFrameEvidence(cleared, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 5u, 5u));
    NoteOverrideCleared(cleared);
    check(cleared.overrideCleared && cleared.action == DrawAction::NativeOverrideCleared,
          "case6: the real clear branch records the clear");
    check(Verdict(cleared) == NativeFrameVerdict::Unknown,
          "case6: a cleared selection proves nothing");

    Diagnostics untouched;  // 其余 append 路径：默认载荷
    check(untouched.action == DrawAction::Unknown && !untouched.overrideCleared &&
              Verdict(untouched) == NativeFrameVerdict::Unknown,
          "case6: the default payload stays Unknown and must not infer a clear");
  }

  // 附加：陈旧只在**检查实际执行**时才成立；且「皮肤选择仍当前」本身不是本帧证明。
  {
    Diagnostics stale;
    NoteLiveNativeSelected(stale, Captured(7u, 5u, 5u));
    NoteStaleObserved(stale);
    NoteFrameEvidence(stale, FrameEvidenceSource::SkinPaletteSelectionCurrent, false, 0u,
                      Captured(7u, 5u, 5u));
    check(Verdict(stale) == NativeFrameVerdict::KnownStale,
          "extra: an executed freshness check that failed => KnownStale");

    Diagnostics notExecuted;
    NoteLiveNativeSelected(notExecuted, Captured(7u, 5u, 5u));
    check(Verdict(notExecuted) == NativeFrameVerdict::Unknown,
          "extra: an unexecuted freshness check => Unknown (not stale)");

    Diagnostics skinOnly;
    NoteLiveNativeSelected(skinOnly, Captured(7u, 5u, 5u));
    NoteFrameEvidence(skinOnly, FrameEvidenceSource::SkinPaletteSelectionCurrent, true, 5u,
                      Captured(7u, 5u, 5u));
    check(Verdict(skinOnly) == NativeFrameVerdict::Unknown,
          "extra: 'skin selection is current' is not by itself a current-frame proof");
  }


  // 缺口补齐（独立验证者 2026-09-19 指出）：
  //   (i) 放宽类改写：只把 passed 写成 true、但**没有**观测到的当前帧标签 ⇒ 不得 KnownCurrent。
  {
    Diagnostics forged;
    NoteLiveNativeSelected(forged, Captured(7u, 5u, 5u));
    NoteFrameEvidence(forged, FrameEvidenceSource::CapturedPaletteCurrentFrame, /*passed=*/true,
                      /*observedFrameTag=*/0u, Captured(7u, 5u, 5u));
    check(Verdict(forged) == NativeFrameVerdict::Unknown,
          "gap(i): passed=true without an observed current-frame tag is not a proof");
  }
  //   (ii) 观测到的当前帧标签与被描述选择**不相等** ⇒ Unknown（真的比较过才算证明）。
  {
    Diagnostics mismatchedTag;
    NoteLiveNativeSelected(mismatchedTag, Captured(7u, 5u, 5u));
    NoteFrameEvidence(mismatchedTag, FrameEvidenceSource::CapturedPaletteCurrentFrame, true,
                      /*observedFrameTag=*/0x2Au, Captured(7u, 5u, 5u));
    check(Verdict(mismatchedTag) == NativeFrameVerdict::Unknown,
          "gap(ii): the observed current-frame tag must equal the described selection's tag");
  }
  //   (iii) 生产者侧来源同样必须携带观测标签（正向可达）。
  {
    Diagnostics producer;
    NoteLiveNativeSelected(producer, Captured(4u, 0x11u, 0x11u));
    NoteFrameEvidence(producer, FrameEvidenceSource::ProducerPaletteCurrentFrame, true, 0x11u,
                      Captured(4u, 0x11u, 0x11u));
    check(Verdict(producer) == NativeFrameVerdict::KnownCurrent,
          "gap(iii): ProducerPaletteCurrentFrame with a matching tag is a proof");
  }
  //   (iv) 本帧比较类检查**执行了但未通过** ⇒ Unknown（不是陈旧，也不是证明）。
  {
    Diagnostics failedCheck;
    NoteLiveNativeSelected(failedCheck, Captured(7u, 5u, 5u));
    NoteFrameEvidence(failedCheck, FrameEvidenceSource::CapturedPaletteCurrentFrame, false, 5u,
                      Captured(7u, 5u, 5u));
    check(Verdict(failedCheck) == NativeFrameVerdict::Unknown,
          "gap(iv): an executed-but-failed current-frame check => Unknown");
  }
  //   (v) packet 回退必须作废既有证明（回退是动作，不产生本帧证明）。
  {
    Diagnostics fallbackAfterProof;
    NoteLiveNativeSelected(fallbackAfterProof, Captured(7u, 5u, 5u));
    NoteFrameEvidence(fallbackAfterProof, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 5u, 5u));
    check(Verdict(fallbackAfterProof) == NativeFrameVerdict::KnownCurrent,
          "gap(v): the proof holds before the fallback");
    NotePacketFallbackSelected(fallbackAfterProof, SelectionIdentity{2u, UINT32_MAX, 0x2Au, 0x2Au});
    check(Verdict(fallbackAfterProof) == NativeFrameVerdict::Unknown,
          "gap(v): a packet fallback must void an earlier proof (nonzero packet tag never proves)");
  }
  //   (vi) 同身份替换 ⇒ 证据保留（NoteSelectionReplaced 的保留分支）。
  {
    Diagnostics sameIdentity;
    NoteLiveNativeSelected(sameIdentity, Captured(7u, 5u, 5u));
    NoteFrameEvidence(sameIdentity, FrameEvidenceSource::CapturedPaletteCurrentFrame, true, 5u,
                      Captured(7u, 5u, 5u));
    NoteSelectionReplaced(sameIdentity, Captured(7u, 5u, 5u));
    check(Verdict(sameIdentity) == NativeFrameVerdict::KnownCurrent,
          "gap(vi): replacing with the same identity keeps the matching evidence");
  }

  // 全空间枚举（独立验证者第二轮指出旧版是**缩减空间**：covered.source 固定、min==max、
  // 身份相等只用 1 个 bool；且 combos==8640 只验循环边界）。这里覆盖 source/slot/min/max 全组合。
  {
    int combos = 0, relaxViolations = 0, decoupleViolations = 0, knownCurrent = 0;
    int exportViolations = 0;
    const DrawAction actions[] = {DrawAction::Unknown, DrawAction::LiveNativeSelected,
                                  DrawAction::PacketFallbackSelected,
                                  DrawAction::StaleRefreshObserved,
                                  DrawAction::NativeOverrideCleared};
    const FrameEvidenceSource sources[] = {
        FrameEvidenceSource::None, FrameEvidenceSource::CapturedPaletteCurrentFrame,
        FrameEvidenceSource::ProducerPaletteCurrentFrame,
        FrameEvidenceSource::SkinPaletteSelectionCurrent};
    const uint32_t tags[] = {0u, 5u, 0x2Au};
    const uint32_t slots[] = {1u, 2u};
    const bool flags[] = {false, true};
    for (DrawAction action : actions) {
      for (bool overrideCleared : flags) {
        for (FrameEvidenceSource source : sources) {
          for (bool executed : flags) {
            for (bool passed : flags) {
              for (uint32_t observed : tags) {
                for (uint32_t coveredSource : slots) {
                  for (uint32_t coveredSlot : slots) {
                    for (uint32_t coveredMin : tags) {
                      for (uint32_t coveredMax : tags) {
                        for (uint32_t describedSource : slots) {
                          for (uint32_t describedSlot : slots) {
                            for (uint32_t describedMin : tags) {
                              for (uint32_t describedMax : tags) {
                                Diagnostics d;
                                d.action = action;
                                d.overrideCleared = overrideCleared;
                                d.evidence.source = source;
                                d.evidence.checkExecuted = executed;
                                d.evidence.checkPassed = passed;
                                d.evidence.observedFrameTag = observed;
                                d.evidence.covered = SelectionIdentity{
                                    coveredSource, coveredSlot, coveredMin, coveredMax};
                                d.identity = SelectionIdentity{
                                    describedSource, describedSlot, describedMin, describedMax};
                                combos++;
                                // 导出不变量（生产接线用的正是这个函数）：
                                //   known ⇒ tag != 0 ∧ tag == described.minFrameTag
                                //   !known ⇒ tag == 0（绝不出现 tag=0 ∧ known=true，或 tag!=0 ∧ known=false）
                                {
                                  const ExportedNativeFrame ex = ExportedNativeFrameFor(d);
                                  if ((ex.known && (ex.frameTag == 0u || ex.frameTag != describedMin)) ||
                                      (!ex.known && ex.frameTag != 0u)) {
                                    exportViolations++;
                                  }
                                }
                                if (Verdict(d) != NativeFrameVerdict::KnownCurrent) continue;
                                knownCurrent++;
                                const bool isCurrentFrameProof =
                                    source == FrameEvidenceSource::CapturedPaletteCurrentFrame ||
                                    source == FrameEvidenceSource::ProducerPaletteCurrentFrame;
                                const bool relaxed =
                                    action == DrawAction::LiveNativeSelected && !overrideCleared &&
                                    executed && passed && isCurrentFrameProof &&
                                    d.evidence.covered == d.identity &&
                                    describedMin == describedMax && describedMin != 0u &&
                                    observed == describedMin;
                                if (!relaxed) relaxViolations++;
                                if (describedMin == 0u) decoupleViolations++;
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    std::printf("ENUM: combos=%d knownCurrent=%d relaxViolations=%d decoupleViolations=%d exportViolations=%d\n",
                combos, knownCurrent, relaxViolations, decoupleViolations, exportViolations);
    check(combos == 622080, "enum: the truth table must cover the FULL space (not the reduced 8640 one)");
    check(knownCurrent > 0, "enum: the positive path must be reachable inside the space");
    check(relaxViolations == 0, "enum: KnownCurrent must never be reached by a relaxed rule");
    check(decoupleViolations == 0,
          "enum: KnownCurrent must imply a nonzero described frame tag (no export decoupling)");
    check(exportViolations == 0,
          "enum: the exported native frame must never decouple from the proof");
  }

  // fail-closed 屏障（可执行）：`selectedPalette = {};` 之所以是屏障，是因为空选择
  // 一定不 Usable；而陈旧选择仍然 Usable（Usable 不查新鲜度）⇒ 该清空不可省。
  // 说明：这验证的是**屏障语义**，不是「分支确实调用了它」（那需要编译 device.cpp）。
  {
    using dxvk::war3::render::skin::Selection;
    using dxvk::war3::render::skin::Source;
    using dxvk::war3::render::skin::Space;
    using dxvk::war3::render::skin::Domain;
    Selection empty{};
    check(!dxvk::war3::render::skin::Usable(empty, 1u, 1u, 1u),
          "barrier: an empty selection must be unusable (that is why the clear is fail-closed)");
    // 陈旧选择（frameTag 是旧值）仍然 Usable —— Usable 不查新鲜度。
    Selection stale{};
    stale.source = Source::CapturedWriter;
    stale.space = Space::World;
    stale.domain = Domain::VertexGroups;
    stale.part = 1u;
    stale.hash = 1u;
    stale.captureSerial = 1u;
    stale.meshPayload = 1u;
    stale.frameTag = 0x2Au;
    stale.actualGroupCount = 1u;
    check(dxvk::war3::render::skin::Usable(stale, 1u, 1u, 1u),
          "barrier: a stale selection is still Usable - the clear is the only fail-closed step");
    check(!dxvk::war3::render::skin::Usable(stale, 2u, 1u, 1u),
          "barrier: a mismatching part is rejected (Usable does check part/hash/domain/space)");
  }

  // 生产接线函数直测（独立验证者第二轮：B3 写死 provenance、B3B4 放宽观测标签、H2 身份 min≠max）。
  // 这些函数**就是** device.cpp 现在调用的那段代码 ⇒ 对它们的测试即对接线的语义测试。
  {
    // 全绿：provenance 可信、当前标签可读、观测==min、min==max、min!=0，且描述身份与之相符。
    Diagnostics d;
    d.action = DrawAction::LiveNativeSelected;
    d.identity = SelectionIdentity{0u, 7u, 0x2Au, 0x2Au};
    NoteCapturedPaletteCurrentFrameEvidence(d, /*provenanceTrusted=*/true,
                                            /*currentTagReadable=*/true, /*observed=*/0x2Au,
                                            /*min=*/0x2Au, /*max=*/0x2Au, /*slot=*/7u);
    check(Verdict(d) == NativeFrameVerdict::KnownCurrent, "wire: the full-match path is KnownCurrent");
    const ExportedNativeFrame exGood = ExportedNativeFrameFor(d);
    check(exGood.known && exGood.frameTag == 0x2Au,
          "wire: a proven description exports known=true with the proven tag");

    // B3 类比：把 provenance 放宽成 true 也无效 —— 只要观测标签不成立就不通过。
    Diagnostics b3;
    b3.action = DrawAction::LiveNativeSelected;
    b3.identity = SelectionIdentity{0u, 7u, 0x2Au, 0x2Au};
    NoteCapturedPaletteCurrentFrameEvidence(b3, true, /*currentTagReadable=*/false, 0u, 0x2Au, 0x2Au, 7u);
    check(Verdict(b3) == NativeFrameVerdict::Unknown,
          "wire(B3): a widened provenance flag cannot prove a frame without a readable current tag");
    check(!ExportedNativeFrameFor(b3).known && ExportedNativeFrameFor(b3).frameTag == 0u,
          "wire(B3): and the export stays (tag=0, unknown)");

    // B3B4 类比：让观测标签在不可读时回退到 min ⇒ 仍然不通过（因为 currentTagReadable 为假）。
    Diagnostics b3b4;
    b3b4.action = DrawAction::LiveNativeSelected;
    b3b4.identity = SelectionIdentity{0u, 7u, 0x2Au, 0x2Au};
    NoteCapturedPaletteCurrentFrameEvidence(b3b4, true, false, /*observed=*/0x2Au, 0x2Au, 0x2Au, 7u);
    check(Verdict(b3b4) == NativeFrameVerdict::Unknown,
          "wire(B3B4): falling back to min must not substitute for reading the current tag");

    // H2 类比：身份 min != max（非单帧）⇒ 不通过。
    Diagnostics h2;
    h2.action = DrawAction::LiveNativeSelected;
    h2.identity = SelectionIdentity{0u, 7u, 0x2Au, 0x2Au};
    NoteCapturedPaletteCurrentFrameEvidence(h2, true, true, 0x2Au, /*min=*/0x2Au, /*max=*/0x2Bu, 7u);
    check(Verdict(h2) == NativeFrameVerdict::Unknown,
          "wire(H2): a non-single-frame identity must not satisfy the proof");

    // 观测标签与 min 不相等 ⇒ 不通过。
    Diagnostics mism;
    mism.action = DrawAction::LiveNativeSelected;
    mism.identity = SelectionIdentity{0u, 7u, 0x2Au, 0x2Au};
    NoteCapturedPaletteCurrentFrameEvidence(mism, true, true, /*observed=*/5u, 0x2Au, 0x2Au, 7u);
    check(Verdict(mism) == NativeFrameVerdict::Unknown,
          "wire: an observed tag different from the selection's tag is not a proof");
  }
  std::printf("SUMMARY: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
