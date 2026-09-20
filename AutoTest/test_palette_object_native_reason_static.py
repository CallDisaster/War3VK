#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""对象级 palette 原因：**接口与执行**检查（2026-09-19 上级裁定后重写）。

裁定要点（本文件据此重写）：
  · 一个 cause 枚举同时承载「动作/来源与新鲜度证明/数据字段」是不成立的 ⇒ 已拆成
    DrawAction + FrameEvidence(+SelectionIdentity) + 唯一解释规则 Verdict；
  · 帧证据必须**随其所属 Selection 身份按值传递**，未执行/不匹配一律 Unknown；
  · S 点与 D 点消费**同一份**描述与**同一个**规则；
  · 本批次**零新增原生内存读取** ⇒ 用计数钉住既有检查调用点数量；
  · 语义契约改由**执行型宿主测试**承担（war3_palette_object_diagnostics_test）。

⚠️ 本文件**不是**语义验收依据：它只识别特定文本/结构形状。验证者已实测出多类
   可绕过的文本改写（把语句搬进字符串/#if 0/#define、用函数调用给 live 事实赋值等）。
   真正的行为保证来自上面的执行型测试与实机证据，而不是这里。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8", errors="replace")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8", errors="replace")
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8", errors="replace")
HEADER = ROOT / "src/d3d9/war3/render/war3_palette_object_diagnostics.h"
HEADER_TEXT = HEADER.read_text(encoding="utf-8", errors="replace")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8", errors="replace")

failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def strip_noncode(text):
    # 行/块注释、字符串与字符字面量、以及 #if 0 区域与其余预处理行。
    # 2026-09-19：修掉验证者指出的缺陷 —— 旧实现用 DOTALL 的 .*$ 收尾，
    # 会把 #endif 之后的**有效源码**一并吞掉（可被用作绕过）。这里改为**配平计数**扫描。
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", text)
    out, depth = [], 0
    for line in text.splitlines():
        stripped = line.strip()
        if depth > 0:
            if stripped.startswith("#if"):
                depth += 1
            elif stripped.startswith("#endif"):
                depth -= 1
            out.append("")
            continue
        if re.match(r"^#\s*if\s+0\b", stripped):
            depth = 1
            out.append("")
            continue
        if stripped.startswith("#"):
            out.append("")
            continue
        out.append(line)
    return "\n".join(out)


CODE_DEVICE = strip_noncode(DEVICE)
CODE_SHADOW = strip_noncode(SHADOW)

# 1) 共享诊断头必须被两点都包含（同一份描述 + 同一个规则）。
# 共享头经 d3d9_war3_scene.h 间接包含；这里同时要求：该场景头确实包含它，
# 且两点确实在**共享命名空间**里使用同一份类型与规则。
check("war3_palette_object_diagnostics.h" in SCENE,
      "E: d3d9_war3_scene.h must include the shared diagnostics header")
check(SCENE.count("palette_object::Diagnostics verdict") == 1,
      "E: the by-value diagnostics description must be part of the payload struct")
for name, text in (("device", CODE_DEVICE), ("shadow", CODE_SHADOW)):
    check("palette_object::" in text,
          "E: %s must use the shared palette-object diagnostics types/rules" % name)

# 2) D 点不得再由 frameTag 反推（packet 回退时它带的是 packet 的非零 tag）。
check("draw.paletteDiagnostics.frameTag != 0u" not in CODE_SHADOW,
      "E/D: the D point must not re-derive nativeKnown from frameTag")
check("draw.paletteDiagnostics.frameTag == 0u" not in CODE_SHADOW,
      "E/D: the D point must not treat 'not live native' as 'cleared by override'")

# 3) D 点必须消费**同一个**规则（nativeKnown 与 selectionCleared 两处）。
# D 点：nativeKnown 经共享 Verdict；「被清空」直接读同一份描述的 overrideCleared。
check(CODE_SHADOW.count("palette_object::Verdict(draw.paletteDiagnostics.verdict)") >= 1 and
      "draw.paletteDiagnostics.verdict.overrideCleared" in CODE_SHADOW,
      "E/D: the D point must consume the shared description (Verdict + overrideCleared)")

# 4) 载荷必须**整份**按值携带描述（动作/证据/身份），而不是只带一个枚举。
check("draw.paletteDiagnostics.verdict = paletteObjectDiagnostics;" in CODE_DEVICE,
      "E: the payload must carry the whole by-value diagnostics description")

# 5) S 点必须消费**同一个**规则，不得再自行推导。
_s_point = CODE_DEVICE.find("const bool paletteObjectNativeKnown =")
# 允许两种等价写法：判定与比较同行，或判定提前一行、再与 KnownCurrent 比较。
_seg = CODE_DEVICE[max(0, _s_point - 400):_s_point + 400] if _s_point >= 0 else ""
# 2026-09-19 二次裁定（路径 i）：S 点现在消费共享头里的 ExportedNativeFrameFor（内部即 Verdict），
# 使导出字段与证明由**同一段代码**推导。
check(_s_point >= 0 and "ExportedNativeFrameFor(" in _seg and "Verdict(d)" in HEADER_TEXT,
      "E: the S point must derive nativeKnown from the shared description (ExportedNativeFrameFor)")
check("paletteObjectSelectionFromLiveNative" not in CODE_DEVICE,
      "E: the old single-boolean derivation must be gone (it made S and D diverge)")

# 6) 旧的过载枚举必须消失（动作/证据/身份已分离）。
for name, text in (("device", CODE_DEVICE), ("shadow", CODE_SHADOW), ("scene", SCENE)):
    check("PaletteObjectDrawCause" not in text,
          "E: the overloaded PaletteObjectDrawCause enum must be gone (%s)" % name)

# 7) 动作必须记录在**真正执行**它的分支。
check("NoteLiveNativeSelected(" in CODE_DEVICE,
      "E: the live-native action must be recorded where the live selection is established")
_fb_note = CODE_DEVICE.find("NotePacketFallbackSelected(")
_fb_assign = CODE_DEVICE.find("selectedPalette = packet.paletteSelection;")
check(_fb_note >= 0 and _fb_assign >= 0 and _fb_note > _fb_assign,
      "E: the packet-fallback action must be recorded at/after the real packet replacement")
_stale = CODE_DEVICE.find("IsSkinPaletteSelectionCurrent(selectedPalette)")
_stale_end = CODE_DEVICE.find("\n  }", _stale) if _stale >= 0 else -1
check(_stale >= 0 and "NoteStaleObserved(" in CODE_DEVICE[_stale:_stale_end],
      "E: the stale action must be recorded inside the freshness branch that actually ran")
_clear = CODE_DEVICE.find("draw.paletteDiagnostics = {};")
# 注意：清空语句自身带花括号，不能拿右花括号当窗口右界（会立刻截断）。
check(_clear >= 0 and "NoteOverrideCleared(" in CODE_DEVICE[_clear:_clear + 600],
      "E: the override-clear action must be recorded in the real clear branch")

# 8) 帧证据只能在**既有检查**本来就已经执行的位置取用。
# 2026-09-19 二次裁定（路径 i）：捕获调色板的证据**组合**已搬进共享头，
# device 侧只剩「陈旧检查」一处直接调用；两处都必须在**既有检查**的位置。
_ev = CODE_DEVICE.count("NoteFrameEvidence(")
_shared_ev = CODE_DEVICE.count("NoteCapturedPaletteCurrentFrameEvidence(")
_stale_ev = CODE_DEVICE.find("NoteFrameEvidence(")
_stale_chk = CODE_DEVICE.find("IsSkinPaletteSelectionCurrent(selectedPalette)")
check(_ev == 1 and _shared_ev == 1 and _stale_chk >= 0 and
      0 <= _stale_ev - _stale_chk < 700,
      "E: frame evidence must be taken only at existing check sites (direct=%d shared=%d)"
      % (_ev, _shared_ev))
# 说明：生产者侧 producerPaletteCurrentFrameProven 目前**没有**可携带的诊断载荷，
# 因此本轮未接线（登记为后续），不作为通过条件。

# 9) 零新增原生内存读取：钉住既有检查调用点数量（本批次不得增加）。
check(CODE_DEVICE.count("QueryCurrentPaletteFrameTag(") == 3,
      "E: zero new native reads - QueryCurrentPaletteFrameTag call sites changed")
check(CODE_DEVICE.count("IsSkinPaletteSelectionCurrent(") == 1,
      "E: zero new native reads - IsSkinPaletteSelectionCurrent call sites changed")

# 9b) 2026-09-19 回归断言（独立验证者证伪）：陈旧分支必须保留 fail-closed 清空。
#     我在改写时曾误删它 ⇒ 陈旧 caster 会带着旧 Selection 参与准入（canonical_draw 的 Usable）。
_stale_blk = CODE_DEVICE[_stale:_stale_end] if _stale >= 0 and _stale_end > 0 else ""
check("selectedPalette = {};" in _stale_blk,
      "E: the freshness branch must keep the fail-closed 'selectedPalette = {};' (admission!)")
# 9c) 帧证据必须带上**既有的**当前帧标签（观测值），否则 Boolean 实参可被一个 token 放宽。
# 2026-09-19 二次裁定（路径 i）：检查的**组合**已搬进共享头（可被宿主测试执行）；
# device 只传既有原始值。这里断言：接线点确实调用共享组合函数，且仍复用**既有**读取结果。
_ev_idx = CODE_DEVICE.find("NoteCapturedPaletteCurrentFrameEvidence(")
_ev_blk = CODE_DEVICE[_ev_idx:_ev_idx + 700] if _ev_idx >= 0 else ""
check(_ev_idx >= 0 and "QueryCurrentPaletteFrameTag(" in _ev_blk and
      "currentPaletteFrameTag" in _ev_blk,
      "E: the captured-palette wiring must call the shared composition with the existing read")
check("observedFrameTag" in (HEADER.read_text(encoding='utf-8', errors='replace')) and
      "evidenceComparesThisFrame" in (HEADER.read_text(encoding='utf-8', errors='replace')),
      "E: the shared rule must require the observed tag to equal the described selection's tag")
# 10) 语义契约由**执行型测试**承担：文件存在、六类用例齐全、且已在 meson 注册。
_TEST = ROOT / "src/d3d9/war3/render/tests/war3_palette_object_diagnostics_test.cpp"
check(_TEST.exists(), "E: the executing host test must exist (semantic contract carrier)")
if _TEST.exists():
    _t = _TEST.read_text(encoding="utf-8", errors="replace")
    for _case in ("case1:", "case2:", "case3:", "case4:", "case5:", "case6:"):
        check(_case in _t, "E: the executing test must cover %s" % _case)
    check("ENUM: combos=" in _t, "E: the executing test must enumerate the truth table")
    check("NoteSelectionReplaced(" in _t,
          "E: the executing test must exercise the real replacement hand-off")
check("war3_palette_object_diagnostics_test" in MESON,
      "E: the executing test must be registered in meson (it is the contract, not this scanner)")

if failures:
    for message in failures:
        print("FAILURE: %s" % message)
    sys.exit(1)
print("palette object native-reason static checks passed")
