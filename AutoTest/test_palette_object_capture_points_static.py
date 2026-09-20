"""生产侧对象级证据（palette-object/v1）采集点的**静态**契约。

只读源码锚点，不启动/聚焦/结束游戏，也不触碰 AutoTest MCP 控制面。
本文件钉死四件事（2026-09-17 上级裁定 Step 1③）：

1. **子门短路位置**：三个采集点的第一条语句都是
   `PaletteObjectEvidenceEnabled()`，键/帧构造与查表都排在它之后；
2. **R 的 POD 出参**：`FindOrUpdatePaletteSlotCache` 新增一个 POD 出参、默认 nullptr、
   调用方在子门关闭时传 nullptr，且**只在 out != nullptr 时填充**；
   具名原因由生产分类函数同时驱动"聚合细分计数分支"与"POD 里的原因"；
3. **S 的顺序**：POD 在 `shadowCasters.emplace_back` **之前**取出，
   `NoteEnqueued` 在 emplace_back **之后**发出（move 之后不得再读 draw）；
   native draw-time override 清空 `draw.inputSkinSelection` 的既有语句保持原样；
4. **D 的语义边界**：`NoteDrawn` 只挂在 `renderShadowMap` 真实绘制命令（cmdDraw*）之后，
   且本帧只在一个级联记录一次；剔除侧不产生对象级事件。
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CORE = (ROOT / "src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp").read_text(encoding="utf-8")
CAPTURE_H = (ROOT / "src/d3d9/war3/tools/war3_palette_object_capture.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
SCENE_H = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")

# ---- 1. R：POD 出参 + 子门关闭时传 nullptr + 只在 out != nullptr 时填充 ----
assert "PaletteSlotRecheckEvidence*" in CORE
assert "outRecheckEvidence = nullptr) {" in CORE
assert "paletteObjectEvidenceOn ? &recheckEvidence : nullptr" in CORE
assert "if (outRecheckEvidence != nullptr) {" in CORE
assert "dxvk::war3::tools::evidence::PublishPaletteSlotRecheckEvidence(" in CORE
# 聚合细分计数的分支选择与 POD 里的原因来自同一个生产分类函数。
assert "ClassifyPaletteSlotRecheckReject(" in CORE
for counter in ("g_paletteSlotCacheBindingMissRejectCount",
                "g_paletteSlotCacheGroupShortRejectCount",
                "g_paletteSlotCacheBindingFrameStaleRejectCount",
                "g_paletteSlotCacheSlotRangeStaleRejectCount"):
    assert counter in CORE, counter
# 原 if/else 分支被等价的 switch 取代后，四个细分计数都必须仍在。
assert "case PaletteObjectRejectReason::R0:" in CORE
assert "case PaletteObjectRejectReason::R1:" in CORE
assert "case PaletteObjectRejectReason::R2:" in CORE

# R 点：子门判定出现在取值/构造键/发事件之前。
r_guard = CORE.index("dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled();")
r_notify_gate = CORE.index("ShouldNotifyPaletteSlotReject(")
r_make_key = CORE.index("MakePaletteObjectKey(")
r_note_reject = CORE.index("PaletteObjectRecorder().NoteReject(")
assert r_guard < r_notify_gate < r_make_key < r_note_reject
# 未执行的检查用 ShouldNotify 门挡住（NotChecked 不得冒充 R0）。
assert "namedRejectReason" in CORE

# ---- 2. 采集支持头：三项纪律的单一来源 ----
assert "ClassifyPaletteSlotRecheckReject" in CAPTURE_H
classify_body = CAPTURE_H.split("ClassifyPaletteSlotRecheckReject(", 1)[1]
classify_body = classify_body.split("// 生产填充", 1)[0]
assert classify_body.index("PaletteObjectRejectReason::R0") < \
    classify_body.index("PaletteObjectRejectReason::R1") < \
    classify_body.index("PaletteObjectRejectReason::R2") < \
    classify_body.index("PaletteObjectRejectReason::R3")
assert "if (out != nullptr)" in CAPTURE_H
assert "if (!evidence.recheckPerformed || evidence.producerConfirmed)" in CAPTURE_H
assert "evidence.rejectReason == PaletteObjectRejectReason::NotChecked" in CAPTURE_H
assert "evidence.rejectReason == PaletteObjectRejectReason::Unknown" in CAPTURE_H
# 键口径：deviceEpoch 记 0 + epochUnknown；lifecycleIdentity 不得伪造。
assert "key.deviceEpoch = 0u;" in CAPTURE_H
assert "key.epochUnknown = true;" in CAPTURE_H
assert "key.lifecycleIdentity = 0u;" in CAPTURE_H
assert "key.identityWeak = true;" in CAPTURE_H
# 帧域四项分列：manifest 一律 unknown，native 由调用方给"当场已读到"的值。
assert "frames.renderFrame = renderFrame;" in CAPTURE_H
assert "frames.recordFrameSerial = recordFrameSerial;" in CAPTURE_H
assert "frames.nativeFrameTag = nativeKnown ? nativeFrameTag : 0u;" in CAPTURE_H
assert "frames.manifestUnknown = true;" in CAPTURE_H
assert "frames.nativeUnknown = !nativeKnown;" in CAPTURE_H

# ---- 3. S：move 之前取 POD、emplace_back 之后发事件 ----
assert "draw.shadowRuntimeModelPtr = packet.renderable.runtimeModelPtr;" in DEVICE
assert "draw.shadowRecordFrameSerial = packet.renderable.frameSerial;" in DEVICE
assert "void* shadowRuntimeModelPtr = nullptr;" in SCENE_H
assert "uint64_t shadowRecordFrameSerial = 0u;" in SCENE_H
s_guard = DEVICE.index("dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled();")
s_make_key = DEVICE.index("MakePaletteObjectKey(")
s_emplace = DEVICE.index("m_war3Scene.shadowCasters.emplace_back(std::move(draw));")
s_note_enqueued = DEVICE.index("paletteObjectRecorder.NoteEnqueued(")
assert s_guard < s_make_key < s_emplace < s_note_enqueued, (s_guard, s_make_key, s_emplace, s_note_enqueued)
# 入队事件必须带上"来源"（记录器 API 的 source 只能由 NoteServed 带入）。
s_note_served = DEVICE.index("paletteObjectRecorder.NoteServed(")
assert s_make_key < s_note_served < s_note_enqueued
# native override 清空 Selection 的既有语句不得被改动（不得补回早先尝试值）。
assert "draw.inputSkinSelection = {}; // Semantic palette was not consumed by this native snapshot." in DEVICE
# native override 标志是**记录器参数**（= drawTimeVBOverrideApplied），不新增本地命名。
# 2026-09-19：S 侧 NoteEnqueued 的 selectionCleared 实参已统一为
# draw.paletteDiagnostics.verdict.overrideCleared（同一份按值描述），意图不变：
# 入队事实必须携带「本次该 caster 的 selection 是否被 native override 清空」。
assert "draw.paletteDiagnostics.verdict.overrideCleared);" in DEVICE
assert "bool selectionClearedByNativeOverride" not in DEVICE
# S 只是"候选入队"：不得写成 GPU 提交完成。
assert "GPU 提交完成" in DEVICE
assert "paletteObjectEnqueueReady" in DEVICE

# ---- 4. D：只在真实绘制命令之后、且本帧只在一个级联记录一次 ----
assert "war3::tools::evidence::PaletteObjectEvidenceEnabled();" in SHADOW
d_guard = SHADOW.index("war3::tools::evidence::PaletteObjectEvidenceEnabled();")
d_make_key = SHADOW.index("war3::tools::evidence::MakePaletteObjectKey(")
d_note_drawn = SHADOW.index("PaletteObjectRecorder().NoteDrawn(")
assert d_guard < d_make_key < d_note_drawn
assert "if (paletteObjectEvidenceOn && c == paletteObjectDrawnCascade) {" in SHADOW
# 绘制命令先记录，事件后发（NoteDrawn 在 cmdDraw/cmdDrawIndexed 之后）。
d_cmd_indexed = SHADOW.index("ctx->cmdDrawIndexed(draw.indexCount, 1, draw.firstIndex,")
d_cmd = SHADOW.index("ctx->cmdDraw(draw.vertexCount, 1, draw.firstVertex, 0);", d_cmd_indexed)
assert d_cmd_indexed < d_cmd < d_note_drawn, (d_cmd_indexed, d_cmd, d_note_drawn)
# 剔除侧（cull 观察）不产生对象级事件：本文件只允许一次 NoteDrawn 调用点。
assert SHADOW.count("PaletteObjectRecorder().NoteDrawn(") == 1
assert "would-cull" in SHADOW

print("palette object capture point static checks passed")
