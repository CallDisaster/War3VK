#pragma once

#include <cstdint>

// 2026-09-19 上级裁定（零新增原生读取的诊断修复批次）：
// 一个字段同时承担「发生的动作」「来源与本帧新鲜度证明」「数据字段」三种信息，是不成立的。
// 这里把它们分开：
//   · DrawAction        —— **只在真正执行了该选择/替换/清空的分支**赋值；
//   · FrameEvidence     —— **复用既有检查**本来就已经算出的结果（本次不新增任何原生读取），
//                          并记录它**覆盖的是哪一份 Selection 身份**；
//   · NativeFrameVerdict—— 由**唯一**的 Verdict() 规则解释，S 点与 D 点都必须消费它，
//                          任何一点都不得再自行推导（否则两轨会漂移）。
// 没有证据 ⇒ Unknown。Unknown **既不是**「已证明陈旧」，**也不是**「已经回退」。
// 边界：本文件只提供诊断描述与解释，**不得**反过来决定 palette 准入、几何选择、回退或阴影生成。

namespace dxvk::war3::render::palette_object {

// 实际发生的动作。Unknown 表示「此处没有发生任何选择/替换/清空」。
enum class DrawAction : uint32_t {
  Unknown = 0u,
  LiveNativeSelected = 1u,      // 生效的 palette 确实来自本帧 live native 刷新
  PacketFallbackSelected = 2u,  // selectedPalette 确实被替换成 packet 的 palette
  StaleRefreshObserved = 3u,    // 某项既有检查**实际执行并观测到**该选择已不新鲜
  NativeOverrideCleared = 4u,   // native draw-time override 确实清空了该选择
};

// 证据来自哪一项**已经存在**的检查（本批次不新增检查、不新增读取）。
enum class FrameEvidenceSource : uint32_t {
  None = 0u,
  CapturedPaletteCurrentFrame = 1u,  // device.cpp: capturedPaletteCurrentFrameProven
  ProducerPaletteCurrentFrame = 2u,  // device.cpp: producerPaletteCurrentFrameProven
  SkinPaletteSelectionCurrent = 3u,  // model::IsSkinPaletteSelectionCurrent
};

// 一份 Selection 的小型身份：证据只对它负责；换了不相容的 Selection 就不能沿用。
struct SelectionIdentity {
  uint32_t source = 0u;
  uint32_t slot = UINT32_MAX;
  uint32_t minFrameTag = 0u;
  uint32_t maxFrameTag = 0u;

  friend bool operator==(const SelectionIdentity& a, const SelectionIdentity& b) {
    return a.source == b.source && a.slot == b.slot &&
           a.minFrameTag == b.minFrameTag && a.maxFrameTag == b.maxFrameTag;
  }
  friend bool operator!=(const SelectionIdentity& a, const SelectionIdentity& b) {
    return !(a == b);
  }
};

// 按值携带的帧证据：检查**是否执行**、执行时的结果，以及它覆盖的身份。
struct FrameEvidence {
  FrameEvidenceSource source = FrameEvidenceSource::None;
  bool checkExecuted = false;  // false ⇒ 从未执行 ⇒ 未知（既不是陈旧，也不是已证明）
  bool checkPassed = false;    // 仅在 checkExecuted 为真时有意义
  // 2026-09-19 二次裁定：检查**当时观测到的当前帧标签**。它必须来自既有的
  // QueryCurrentPaletteFrameTag 调用结果（**不新增任何原生读取**）。
  // 把它带上，是为了让 KnownCurrent 的含义等于「真的比较过」：
  // 只把一个布尔实参写成 true 并不足以伪造本帧证明，还必须标签相符。
  uint32_t observedFrameTag = 0u;
  SelectionIdentity covered = {};
};

enum class NativeFrameVerdict : uint32_t {
  Unknown = 0u,       // 无证据，或证据并不描述这份 Selection
  KnownCurrent = 1u,  // 已证明是本帧的 native 选择
  KnownStale = 2u,    // 某项检查已观测到它陈旧
};

// 诊断描述本体（S 点与 D 点消费的**同一份**东西）。
struct Diagnostics {
  DrawAction action = DrawAction::Unknown;
  bool overrideCleared = false;
  FrameEvidence evidence = {};
  SelectionIdentity identity = {};  // 本描述所关于的那份 Selection
};

// --- 生产交接（hand-off）辅助：顺序即生产调用顺序，宿主测试直接执行这段真实逻辑 ---

inline void NoteLiveNativeSelected(Diagnostics& d, const SelectionIdentity& selection) {
  d.action = DrawAction::LiveNativeSelected;
  d.identity = selection;
}

inline void NotePacketFallbackSelected(Diagnostics& d, const SelectionIdentity& selection) {
  d.action = DrawAction::PacketFallbackSelected;
  d.identity = selection;
  // 回退是**动作**，不是帧证据：它不产生 native 证明。
  d.evidence = {};
}

inline void NoteStaleObserved(Diagnostics& d) {
  d.action = DrawAction::StaleRefreshObserved;
}

// 只在既有检查**原本就已经执行**的位置调用，且只传它当时算出的结果。
inline void NoteFrameEvidence(Diagnostics& d, FrameEvidenceSource source, bool passed,
                              uint32_t observedFrameTag,
                              const SelectionIdentity& covered) {
  d.evidence.source = source;
  d.evidence.checkExecuted = true;
  d.evidence.checkPassed = passed;
  d.evidence.observedFrameTag = observedFrameTag;
  d.evidence.covered = covered;
}

// 选择被替换：若已有证据并不描述新选择，则**作废**（A 的证明不得被 B 继承）。
inline void NoteSelectionReplaced(Diagnostics& d, const SelectionIdentity& newSelection) {
  if (d.evidence.covered != newSelection) {
    d.evidence = {};
  }
  d.identity = newSelection;
}

inline void NoteOverrideCleared(Diagnostics& d) {
  d.action = DrawAction::NativeOverrideCleared;
  d.overrideCleared = true;
  // 被清空 ⇒ 没有可描述的 Selection，也没有可沿用的证据。
  d.evidence = {};
  d.identity = {};
}

// **唯一**的解释规则。S 点、D 点都只能经由此函数得出结论。
inline NativeFrameVerdict Verdict(DrawAction action, bool overrideCleared,
                                  const FrameEvidence& evidence,
                                  const SelectionIdentity& described) {
  if (overrideCleared) {
    return NativeFrameVerdict::Unknown;  // 已清空：没有东西可描述
  }
  if (evidence.checkExecuted && !evidence.checkPassed &&
      evidence.source == FrameEvidenceSource::SkinPaletteSelectionCurrent) {
    return NativeFrameVerdict::KnownStale;  // 检查**执行了**且观测到陈旧
  }
  const bool evidenceDescribesThisSelection =
      evidence.checkExecuted && evidence.checkPassed && evidence.covered == described;
  // 只有**做了本帧比较**的证据才能给出「本帧已证明」：
  // 「皮肤选择仍当前」即使通过，也只是必要不充分（它不比较 native 当前帧标签），
  // 因此它只能在上面的 KnownStale 分支起作用，绝不能升级成 KnownCurrent。
  const bool evidenceIsCurrentFrameProof =
      evidence.source == FrameEvidenceSource::CapturedPaletteCurrentFrame ||
      evidence.source == FrameEvidenceSource::ProducerPaletteCurrentFrame;
  // KnownCurrent ⟺ 动作是 live 选择 ∧ 本帧比较类证据已执行且通过 ∧ 证据描述的就是这份选择
  //              ∧ 证据当时观测到的当前帧标签**非零** ∧ 它与被描述选择的标签**相等**。
  // 最后两条把「真的比较过」变成硬条件：
  //   · 只把布尔实参写成 true（放宽类改写）不足以伪造本帧证明；
  //   · 也保证 KnownCurrent 不会与被导出的 frameTag 脱钩（导出标签必然非零且等于观测值）。
  // 2026-09-19 二次裁定：身份必须是**单帧**身份（min == max）。
  // 生产所有构造点都写 {source, slot, frameTag, frameTag}；若出现 min != max，
  // 说明这份身份不是我们约定的形状（例如被替换点写入），一律不予证明。
  if (described.minFrameTag != described.maxFrameTag) {
    return NativeFrameVerdict::Unknown;
  }
  const bool evidenceComparesThisFrame =
      evidence.observedFrameTag != 0u && described.minFrameTag == evidence.observedFrameTag;
  if (action == DrawAction::LiveNativeSelected && evidenceIsCurrentFrameProof &&
      evidenceDescribesThisSelection && evidenceComparesThisFrame) {
    return NativeFrameVerdict::KnownCurrent;
  }
  // 其它一切（无证据 / 证据不匹配 / 未执行 / 未证明 / packet 回退）一律未知。
  // 特别地：**没有满足 LiveNative 不等于发生了 packet 回退**。
  return NativeFrameVerdict::Unknown;
}

inline NativeFrameVerdict Verdict(const Diagnostics& d) {
  return Verdict(d.action, d.overrideCleared, d.evidence, d.identity);
}

// --- 2026-09-19 二次裁定（路径 i）：把「接线」里可测的部分搬进本头 ---
// 独立验证者证明：只把头内的 Verdict 做对没有用，生产**接线**（检查怎么组合、导出帧怎么推）
// 若留在 device.cpp 里就完全没有机制覆盖（B1/B3 都是一行改写）。
// 因此把这两段也搬进来，生产与宿主测试执行**同一段代码**；device.cpp 只负责把
// **既有**的原始值传进来（仍然零新增原生读取）。

// 捕获调色板的本帧证明：**组合**规则在这里，不再散落在 device.cpp。
inline void NoteCapturedPaletteCurrentFrameEvidence(Diagnostics& d, bool provenanceTrusted,
                                                    bool currentTagReadable,
                                                    uint32_t observedFrameTag,
                                                    uint32_t minFrameTag,
                                                    uint32_t maxFrameTag, uint32_t slot) {
  const bool passed = provenanceTrusted && minFrameTag != 0u && minFrameTag == maxFrameTag &&
                      currentTagReadable && observedFrameTag == minFrameTag;
  NoteFrameEvidence(d, FrameEvidenceSource::CapturedPaletteCurrentFrame, passed, observedFrameTag,
                    SelectionIdentity{0u, slot, minFrameTag, maxFrameTag});
}

// 被导出的本帧字段：**由同一份描述推导**，因此不可能与证明脱钩。
struct ExportedNativeFrame {
  bool known = false;
  uint32_t frameTag = 0u;
};
inline ExportedNativeFrame ExportedNativeFrameFor(const Diagnostics& d) {
  ExportedNativeFrame out{};
  if (Verdict(d) == NativeFrameVerdict::KnownCurrent) {
    out.known = true;
    out.frameTag = d.identity.minFrameTag;
  }
  // 未证明 ⇒ 导出标签一律 0 且 unknown。绝不出现 (tag=0 ∧ known=true) 或 (tag!=0 ∧ known=false)。
  return out;
}

} // namespace dxvk::war3::render::palette_object
