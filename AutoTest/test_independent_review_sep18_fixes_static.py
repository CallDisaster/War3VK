"""2026-09-18 独立复审三项修复的**静态**回归门禁（R1/R2/R3）。

只读源码锚点；不启动/聚焦/结束游戏，也不触碰 AutoTest MCP 控制面。

本文件把"复审已复现的缺口"钉成"改动后不得再出现"的结构不变式：

R1 冷/热缓存数量检查同规则
    每一个 `queryProducerBindingSlot(&producerGroupCount, &producerFrameTag)` 调用点之后，
    都必须出现与 `requiredPaletteCount` 的数量比较。复审复现：冷缓存首次查询曾接受
    groupCount 不足的 producer slot（需 9 组、producer 只报 8 组 ⇒ 仍返回 slot 42），
    而同一对象热缓存命中时被拒绝。

R2 生产采集点的 nativeKnown 必须为 false
    该调用点**拿不到** native 帧 ⇒ 必须记 unknown。`MakePaletteObjectFrames` 的
    第 4 参是 `nativeKnown`；传 true 且 tag=0 会把"未知"写成"已知且等于 0"。

R3 诊断到达计数器必须原子化且受子门约束
    这些计数器由渲染线程写、控制面客户线程读；原为普通 `uint64_t` 且无共同锁
    （记录器内部锁不保护它们）。且至少两处计数位于子门判断之前 ⇒ 关闭诊断时仍在写。
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SLOTS = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(encoding="utf-8")
CAPTURE_H = (ROOT / "src/d3d9/war3/tools/war3_palette_object_capture.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SINK = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp").read_text(encoding="utf-8")

# ---- R1：每个 producer 绑定查询之后都必须有 requiredPaletteCount 数量比较 ----
QUERY = "queryProducerBindingSlot(&producerGroupCount, &producerFrameTag);"
assert SLOTS.count(QUERY) == 2, "producer 绑定查询点数量变化，本门禁需同步"
for _idx, chunk in enumerate(SLOTS.split(QUERY)[1:], start=1):
    window = chunk[:1400]
    assert "requiredPaletteCount" in window, (
        f"第 {_idx} 个 queryProducerBindingSlot 调用点之后未见 requiredPaletteCount 数量检查"
    )
# 2026-09-18 批次 4（步骤①）：数量规则已抽为**单一实现** `ProducerGroupCountCovers`，
# 冷/热缓存两处都必须调用它（相同规则只维护一份）。
assert SLOTS.count("dxvk::war3::render::skin::ProducerGroupCountCovers(") == 2, \
    "冷缓存与热缓存都必须使用同一数量规则实现"
assert "producerGroupCount < requiredPaletteCount" not in SLOTS, \
    "不得再出现各写一份的字面比较"
assert "producerGroupCount >= requiredPaletteCount" not in SLOTS, \
    "不得再出现各写一份的字面比较"
assert "g_devicePaletteSlotCacheRejectedStaleCount.fetch_add(" in SLOTS
assert "inline bool ProducerGroupCountCovers(uint32_t required,uint32_t producerGroupCount) noexcept {" \
    in (ROOT / "src/d3d9/war3/render/war3_skin_palette_selection.h").read_text(encoding="utf-8")

# ---- R2：生产采集点 nativeKnown=false；辅助函数语义不变 ----
ANCHOR = "uint64_t(currentRenderFrameIndex), uint64_t(manifestFrame),"
assert ANCHOR in DEVICE
_tail = DEVICE[DEVICE.index(ANCHOR):DEVICE.index(ANCHOR) + 420]
assert "0u, false," in _tail, "生产采集点必须传 nativeKnown=false"
# 2026-09-18 阶段 C（K3）：该站点还必须**按值携带 attemptSerial**（否则实机判序仍是终身单调）。
assert "uint64_t(manifestFrame));" in _tail, \
    "首见采集点必须按值携带 attemptSerial（K3）：用当地 manifest 记录帧作尝试号"
assert "0u, true" not in _tail, "生产采集点不得再传 nativeKnown=true（无论其后还跟几个实参）"
assert "frames.nativeUnknown = !nativeKnown;" in CAPTURE_H
assert "frames.nativeFrameTag = nativeKnown ? nativeFrameTag : 0u;" in CAPTURE_H

# ---- R3a：诊断计数器为原子，且不再有裸 ++ ----
# 2026-09-18 P0-5（复审 C2）：新增两个**具名计数**（g_paletteObjectDroppedNoSession /
# g_paletteObjectEncodeFailed），用于把发射路径的两条早退从「无声 return」改为可计数 ⇒
# 计数从 5 个变 7 个。**这个数字锁是故意的**：将来再加计数器必须回到这里更新并说明用途。
# 2026-09-18 P0-6（复审的锁审计）：旧的 `SINK.count('.load(std::memory_order_relaxed)') == 7` 是**脆弱文本锁**：
# 复审实测「把某个具名访问器改成 `return 0u;` 同时随便加一处无关的 relaxed load」⇒ 计数仍是 7 ⇒ **通过**（替换攻击）；
# 而「注释里写出该字面量」或「`.load( std::memory_order_relaxed )` 加空格」都会**假失败**。
# ⇒ 改为**按名锁**：每个具名计数器必须恰好 1 处声明、1 处 relaxed 自增、1 处 relaxed 读取（且只应在它自己的访问器里），
# 并必须存在对应的具名访问器。这样「计数被写死在别处」或「访问器被改名/掏空」都会**具名失败**。
_SINK_COUNTERS = ("g_paletteObjectEnqueueBlockReached",
                  "g_paletteObjectAppendEntered",
                  "g_paletteObjectProductionInsertReached",
                  "g_paletteObjectProductionNoteCalled",
                  "g_paletteObjectResetOrClear",
                  "g_paletteObjectDroppedNoSession",
                  "g_paletteObjectEncodeFailed")
for _name in _SINK_COUNTERS:
    assert SINK.count(f"std::atomic<uint64_t> {_name}{{0u}};") == 1, \
        ("R3a: 具名计数器必须恰好声明一次且为原子", _name)
    # 允许多个自增点（例如 g_paletteObjectResetOrClear 有 4 处），但**每一处都必须 relaxed**：
    # 「计数被写死在别处」或「偷偷换成非原子自增」都会让本断言失败。
    _relaxed_adds = SINK.count(f"{_name}.fetch_add(1u, std::memory_order_relaxed)")
    _all_adds = SINK.count(f"{_name}.fetch_add(1u")
    assert _all_adds >= 1 and _all_adds == _relaxed_adds, \
        ("R3a: 具名计数器的**每一处**自增都必须是 relaxed 原子自增", _name, _all_adds, _relaxed_adds)
    assert SINK.count(f"{_name}.load(std::memory_order_relaxed)") == 1, \
        ("R3a: 具名计数器必须恰好有一处 relaxed 读取（只应在它自己的访问器里）", _name)
    _accessor = _name[len("g_"):]
    _accessor = _accessor[0].upper() + _accessor[1:] + "Count"
    assert f"uint64_t {_accessor}() noexcept {{" in SINK, \
        ("R3a: 具名计数器必须有具名访问器", _name, _accessor)
    # 2026-09-18 P0-6（复审 0224f6c8 的反例 2）：仅数 `.load(...)` 的出现次数会被**替换攻击**绕过 ——
    # 「访问器改成 `return 0u;`，再把唯一那处 relaxed load 搬到一个永不被调用的函数」仍然 PASS。
    # ⇒ 必须校验 load 的**归属**：该 relaxed load 必须出现在**这个具名访问器自己的函数体**里，
    # 且访问器体必须是「单行 return <name>.load(relaxed);」这一形状（掏空/改名/读别的计数器都会失败）。
    _acc_sig = f"uint64_t {_accessor}() noexcept {{"
    _acc_start = SINK.index(_acc_sig)
    _acc_end = SINK.index("}", _acc_start)
    _acc_body = SINK[_acc_start:_acc_end]
    assert f"{_name}.load(std::memory_order_relaxed)" in _acc_body, \
        ("R3a: 该具名计数器的 relaxed load 必须**位于它自己的访问器体内**", _name, _accessor)
    assert "return 0u;" not in _acc_body, \
        ("R3a: 访问器不得被掏空（return 0u;）", _name, _accessor)
assert "++g_paletteObject" not in SINK, "仍存在对诊断计数器的非原子自增"
assert "#include <atomic>" in SINK

# ---- R3b：两个调用点必须在子门判断之内 ----
assert ("if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled())\n"
        "    dxvk::war3::tools::evidence::NotePaletteObjectAppendEntered();") in DEVICE, (
    "append 入口计数必须在子门判断之内")
assert ("if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()) {\n"
        "      dxvk::war3::tools::evidence::NotePaletteObjectProductionInsertReached();") in DEVICE, (
    "生产插入点计数必须在子门判断之内")
# 被移入子门的调用不得再出现在子门之前
assert DEVICE.index("NotePaletteObjectProductionInsertReached()") > DEVICE.index(
    "if (dxvk::war3::tools::evidence::PaletteObjectEvidenceEnabled()) {\n"
    "      dxvk::war3::tools::evidence::NotePaletteObjectProductionInsertReached();")

print("independent review sep18 fixes static: PASS (R1 cold/warm count rule, R2 nativeKnown=false, R3 atomic+gated)")
