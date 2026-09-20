from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# 2026-09-17 活跃路径纯计数（上级 Q-B 条件批准，无条件编译）。
# 四项计数只证明"路径到达 / 提交数量"，不构成来源归属或对象级关联的证据。
# 本门禁钉死：字段名、四处自增位置（含 skinned 条件与所在函数）、11 个出口/字段、
# 无进程全局原子，以及新计数附近注释不得出现超出上述范围的宣称。

DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SCENE_H = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
BRIDGE_H = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.h").read_text(encoding="utf-8")
BRIDGE = (ROOT / "src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp").read_text(encoding="utf-8")
HUB_H = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.h").read_text(encoding="utf-8")
HUB = (ROOT / "src/d3d9/war3/tools/war3_diagnostics_hub.cpp").read_text(encoding="utf-8")
CP = (ROOT / "src/d3d9/war3/tools/war3_control_plane.cpp").read_text(encoding="utf-8")
PM_H = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h").read_text(encoding="utf-8")
PM = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
PALETTE_SELECTION = (
    ROOT / "src/d3d9/war3/render/war3_skin_palette_selection.h").read_text(encoding="utf-8")

F_ENTRY = "semanticSceneAppendEntrySkinnedCount"
F_GATE_REJECT = "semanticSceneCanonicalGateRejectSkinnedCount"
F_DT_PRODUCER = "semanticSceneDrawTimeProducerSubmittedSkinnedCount"
F_DT_FASTAPPEND = "semanticSceneDirectCurrentDrawSubmittedSkinnedCount"
FIELDS = [F_ENTRY, F_GATE_REJECT, F_DT_PRODUCER, F_DT_FASTAPPEND]

NL = "\n"
# ---- 分组 A：字段名 + 11 个出口/字段（照 PaletteSourceChurnCount 先例）----
for f in FIELDS:
    # L1 当帧字段：d3d9_war3_scene.h（uint32_t，与 palette 分类桶同源，非进程全局原子）
    assert f"uint32_t {f} = 0;" in SCENE_H, f
    # L2 bridge summary 结构体字段
    assert f"uint64_t {f} = 0;" in BRIDGE_H, f
    # L3 bridge summary 填充（g_shadowSceneStats -> summary）
    assert f in BRIDGE and "g_shadowSceneStats" in BRIDGE, f
    # L4 diagnostics hub summary 字段
    assert f"uint64_t {f} = 0;" in HUB_H, f
    # L5 hub summary 填充（bridgeSummary -> summary）
    assert f in HUB and "bridgeSummary" in HUB, f
    # L6 hub JSON 键值对
    assert f'{{"{f}"' in HUB, f
    # L7 control plane JSON 键值对
    assert f'{{"{f}"' in CP, f
    # L8 perf monitor agg 字段
    assert f"uint64_t {f} = 0;" in PM_H, f
    # L9 perf monitor agg 累加
    assert f"agg.{f} +=" in PM, f
    # L10/L11 perf monitor 两个 JSON 写手
    assert PM.count(f'\\"{f}\\"') == 2, (f, PM.count(f'\\"{f}\\"'))
assert "runtimeSummary" in PM

# ---- 分组 B：四处自增位置（禁止只钉名字）----

# B1 (1)：append 入口，策略门之后、canonical 构建之前，skinned-only。
BLOCK_ENTRY = NL.join([
    "  if (skinned) {",
    f"    m_war3Scene.shadowStats.{F_ENTRY}++;",
    "  }",
])
assert DEVICE.count(f"{F_ENTRY}++") == 1, F_ENTRY
assert DEVICE.count(BLOCK_ENTRY) == 1, ("entry block", F_ENTRY)
entry_idx = DEVICE.index(f"{F_ENTRY}++")
assert entry_idx > DEVICE.rindex("ShadowProducerPolicyAllows(", 0, entry_idx)
assert DEVICE.count("BuildCanonicalShadowDrawItem(") == 1
assert entry_idx < DEVICE.index("BuildCanonicalShadowDrawItem(")
# (1) 紧跟 const bool skinned = packet.path == ...::Skinned;
assert entry_idx - DEVICE.rindex("const bool skinned =", 0, entry_idx) < 400

# B2 (4-b)：canonical 就绪门拒绝分支内，skinned-only，与 resolve 状态无关。
BLOCK_GATE = NL.join([
    "    if (skinned) {",
    f"      m_war3Scene.shadowStats.{F_GATE_REJECT}++;",
    "    }",
])
assert DEVICE.count(f"{F_GATE_REJECT}++") == 1, F_GATE_REJECT
assert DEVICE.count(BLOCK_GATE) == 1, ("gate block", F_GATE_REJECT)
GATE_ANCHOR = "if (!canonicalItem.readyForShadowConsumer()) {"
assert DEVICE.count(GATE_ANCHOR) == 1, GATE_ANCHOR
gate_idx = DEVICE.index(GATE_ANCHOR)
reject_idx = DEVICE.index(f"{F_GATE_REJECT}++")
assert gate_idx < reject_idx
gate_seg = DEVICE[gate_idx:gate_idx + 800]
assert BLOCK_GATE in gate_seg
# 计数是无条件 skinned-only：出现在 currentDrawResolveStatus == 判断之前。
assert gate_seg.index(f"{F_GATE_REJECT}++") < gate_seg.index("currentDrawResolveStatus ==")
assert "return false;" in gate_seg

# B3 (6-a)：War3TryPopulateDrawTimeSemanticProducer 内，与 semanticSceneSubmittedSkinned++ 同点。
producer_start = DEVICE.index(
    "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer(")
producer_end = DEVICE.index(
    "void D3D9DeviceEx::War3CollectRetiredShadowSessions(", producer_start)
assert DEVICE.count(f"{F_DT_PRODUCER}++") == 1, F_DT_PRODUCER
dt_idx = DEVICE.index(f"{F_DT_PRODUCER}++")
assert producer_start < dt_idx < producer_end, F_DT_PRODUCER
dt_skinned = DEVICE.rindex(
    "m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;", producer_start, dt_idx)
assert dt_idx - dt_skinned < 400, F_DT_PRODUCER
# 同一 Unit 分支条件（沿用既有语义，不独立证明蒙皮）。
assert DEVICE.rindex("ObjectKind::Unit", producer_start, dt_idx) > dt_idx - 400

# B4 (6-b)：War3TryPopulateDirectCurrentDrawGrouped 的 fast-append 发布段。
grouped_start = DEVICE.index(
    "uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(")
grouped_end = DEVICE.index(
    "uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(", grouped_start)
assert DEVICE.count(f"{F_DT_FASTAPPEND}++") == 1, F_DT_FASTAPPEND
fa_idx = DEVICE.index(f"{F_DT_FASTAPPEND}++")
assert grouped_start < fa_idx < grouped_end, F_DT_FASTAPPEND
fa_skinned = DEVICE.rindex(
    "m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;", grouped_start, fa_idx)
assert fa_idx - fa_skinned < 400, F_DT_FASTAPPEND
assert fa_idx < DEVICE.index(
    "return finishFastAppend(FastAppendOutcome::Success, true);", grouped_start)

# ---- 分组 C：禁止进程全局原子（四项必须是 device 私有的当帧 uint32_t）----
for f in FIELDS:
    assert f"std::atomic<uint32_t> {f}" not in DEVICE, f
    assert f"std::atomic<uint64_t> {f}" not in DEVICE, f
    assert f"std::atomic<uint32_t> {f}" not in SCENE_H, f
    assert f"std::atomic<uint64_t> {f}" not in SCENE_H, f
    assert f"uint32_t {f} = 0;" in SCENE_H, f
    assert f"uint64_t {f} = 0;" not in SCENE_H, f
    assert f"m_war3Scene.shadowStats.{f}++" in DEVICE, f
    start = 0
    while True:
        i = DEVICE.find(f, start)
        if i < 0:
            break
        assert "std::atomic" not in DEVICE[max(0, i - 240):i], (f, i)
        start = i + 1

# ---- 分组 D：注释不得把四项写成超出"路径到达 / 提交数量"的宣称 ----
CLAIM_TERMS = ("替代来源", "同对象恢复")
AFFIRMATIVE_CLAIMS = (
    "完成替代来源", "证明替代来源", "支持替代来源",
    "完成同对象恢复", "证明同对象恢复", "支持同对象恢复",
)
NEGATIONS = ("不", "非", "未", "禁止", "否决", "不得", "不能")


def windows(text, needle, radius=420):
    out = []
    start = 0
    while True:
        i = text.find(needle, start)
        if i < 0:
            break
        out.append(text[max(0, i - radius):i + radius])
        start = i + 1
    return out


for src_name, text in (("d3d9_device.cpp", DEVICE), ("d3d9_war3_scene.h", SCENE_H)):
    for f in FIELDS:
        for w in windows(text, f):
            for tpl in AFFIRMATIVE_CLAIMS:
                assert tpl not in w, (src_name, f, tpl)
            for line in w.splitlines():
                if any(term in line for term in CLAIM_TERMS):
                    assert any(neg in line for neg in NEGATIONS), (src_name, f, line)

# ---- 分组 E：既有合同未动（回归护栏）----
assert "#define WARVK_SKIN_PALETTE_CONTRACT_DEFAULT 0" in PALETTE_SELECTION
# 既有 8 桶仍在原位；新四项不得混入 palette 来源桶命名族。
assert SCENE_H.count("semanticSceneSubmittedSkinnedPaletteSource") == 8
for f in FIELDS:
    assert "PaletteSource" not in f, f
# 上级明确否决：不得落未经证明的 palette 来源桶（第二条 draw-time 路径只落纯分母）。
for text in (DEVICE, SCENE_H, BRIDGE_H, BRIDGE, HUB_H, HUB, PM_H, PM, CP):
    assert "semanticSceneDrawTimeSubmittedSkinnedPaletteSource" not in text
# 既有 semanticSceneSubmittedSkinned 的 3 个自增点未被移动/删除（可逐帧对账）。
assert DEVICE.count("m_war3Scene.shadowStats.semanticSceneSubmittedSkinned++;") == 3
# M2-5：taxonomy 发射块（含 skinned 条件与诊断门本身）已逐字节迁往
# src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp。断言语义不变：
# 该门仍以 skinned && getter 的形式守在整个发射块之前、且全仓唯一；device.cpp
# 只经 War3EmitSemanticPaletteTaxonomy(...) 调用它（M1/M2-1/M2-3 同型的
# "跟随迁移改锚、断言不削弱"）。
TAXONOMY = (
    ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp"
).read_text(encoding="utf-8")
assert TAXONOMY.count(
    "if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {") == 1
assert "if (skinned && War3SemanticPaletteDiagnosticsRuntime()) {" not in DEVICE
assert DEVICE.count("War3EmitSemanticPaletteTaxonomy(") == 1

# ---- 分组 F：计数 / 重置 / 发布口径一致 ----
assert "m_war3Scene = War3FrameScene{};" in DEVICE
assert "dxvk::war3::render::NoteShadowSceneStats(m_war3Scene.shadowStats);" in DEVICE
assert "War3ShadowCaptureStats merged = stats;" in BRIDGE
assert "g_shadowSceneStats = merged;" in BRIDGE
capture_start = SCENE_H.index("struct War3ShadowCaptureStats {")
frame_start = SCENE_H.index("struct War3FrameScene {")
for f in FIELDS:
    i = SCENE_H.index(f"uint32_t {f} = 0;")
    assert capture_start < i < frame_start, f

print("active path palette instrumentation export static checks passed")
