"""2026-09-18 独立复审批次 3：**正常观察链**（wire 版本 3）的静态协议门禁。

只读源码锚点；不启动/聚焦/结束游戏。

复审要求：写方 / 解析器 / 测试**同步**变更，且
  · 不得伪造 Rejected；
  · 不得只让 NoteEnqueued 自动插表后放宽解析器直到它通过；
  · 不得把未知身份升级为已证明；
旧版本（1/2）含义必须保持。

本门禁把上述约束钉成结构不变式，并**同时**覆盖写方与读方 —— 单侧改动会在此立刻失败。
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

EVIDENCE_H = (ROOT / "src/d3d9/war3/tools/war3_palette_object_evidence.h").read_text(encoding="utf-8")
FRAME_EVIDENCE = (ROOT / "src/d3d9/war3/tools/war3_frame_evidence.cpp").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
PARSER = (ROOT / "AutoTest/analyze_palette_object_evidence.py").read_text(encoding="utf-8")
HOST_TEST = (ROOT / "src/d3d9/war3/render/tests/war3_palette_object_evidence_test.cpp").read_text(encoding="utf-8")

# ---- 1. 写方：新阶段 + 语义秩 + 唯一建条目入口 ----
assert "FirstSight = 5u," in EVIDENCE_H, "阶段枚举必须新增 FirstSight=5"
assert "static uint32_t StageRank(PaletteObjectStage stage)" in EVIDENCE_H
# 顺序必须按语义秩而非枚举数值（否则首见之后的阶段会被误判 orderViolation）
assert "const uint32_t s = StageRank(stage);" in EVIDENCE_H
assert "const uint32_t s = static_cast<uint32_t>(stage);" not in EVIDENCE_H, (
    "MarkStage 不得再用枚举数值比较顺序")
# 首见是唯一为「从未被拒绝的对象」建条目的入口
assert "void NoteFirstSight(const PaletteObjectKey& key," in EVIDENCE_H
assert "m_counters.firstSightInserted++;" in EVIDENCE_H
assert "m_counters.firstSightEmitted++;" in EVIDENCE_H
# 首见**不得**伪装成拒绝：不得写 Rejected 阶段 / 不得发 TableFull 终态
_first_sight = EVIDENCE_H[EVIDENCE_H.index("void NoteFirstSight("):]
_first_sight = _first_sight[:_first_sight.index("void NoteServed(")]
assert "PaletteObjectStage::Rejected" not in _first_sight, "首见不得伪造 Rejected 阶段"
assert "PaletteObjectTerminal::TableFull" not in _first_sight, "首见不得宣告 TableFull 终态"
assert "PaletteObjectRejectReason::NotChecked" in _first_sight

# ---- 2. 写方：终态不得为首见链谎报 Drawn ----
assert "record.stage = e.sawFirstSight ? StageOfRank(e.maxStage)" in EVIDENCE_H
assert "static PaletteObjectStage StageOfRank(uint32_t rank)" in EVIDENCE_H
# 旧链（无首见）必须保持冻结形状
assert ": PaletteObjectStage::Drawn;" in EVIDENCE_H

# ---- 3. 写方：**恒定 v4**（2026-09-18 阶段 C）----
# 原断言要求「版本由 firstSightUsed() 决定」，那是**动态版本**：同一个二进制产生两种块形状，
# 而版本本应是**契约**而不是内容摘要。现改为断言恒定 v4，并**显式禁止**动态选择回归。
assert 'result["version"]=4;' in FRAME_EVIDENCE, (
    "阶段 C：块版本必须**恒定 v4**（含未使用观察链的导出）")
assert 'firstSightUsed() ? 3 : 2' not in FRAME_EVIDENCE, (
    "阶段 C：按 firstSightUsed() 动态选择 2/3 必须已删除（版本是契约，不是内容摘要）")
# 2026-09-18 Astra 六项裁定之一：**移除强制该函数存在的文本门禁**。
# 动态选版本的用法已删除（见上面的断言），只读查询本身的正当性由**真实使用**保证：
# 宿主用例 `war3_palette_object_evidence_test.cpp` 用它区分两类窗口（Require(rec.firstSightUsed()) /
# Require(!rec.firstSightUsed())）⇒ 文本卡存在性会阻止合理的重构，属于过时保护。
# ---- 4. 生产采集点：发首见，不再冒充 ServedCandidate ----
# 只约束**生产采集块**：语义 append 块（War3TryAppendSemanticShadowPacket）内另有合法的
# NoteServed 用法，不得被这条规则误伤。
_PROD = DEVICE[DEVICE.index("NotePaletteObjectProductionNoteCalled();"):]
_PROD = _PROD[:_PROD.index("NoteEnqueued(")]
assert "NoteFirstSight(" in _PROD, "生产采集点必须发首见"
assert "NoteServed(" not in _PROD, (
    "生产路径本无 selectedPalette；发 ServedCandidate 等于冒充拒绝恢复链")

# ---- 5. 读方：版本 3 支持 + 旧版本 fail-visible + 链首规则 ----
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION=3" in PARSER
assert "5:'FirstSight'" in PARSER, "解析器阶段表必须登记 FirstSight"
assert "firstSightStageUnderLegacyVersion" in PARSER, (
    "旧版本出现首见阶段必须具名判错，不得静默接受")
assert "if i_reject is None and i_firstsight is None:" in PARSER, (
    "版本 3 首见链没有拒绝事实，缺 Rejected 不得再判为截断")
assert "bothRejectedAndFirstSightHead" in PARSER, "两条链必须互斥"
assert "servedCandidateBeforeFirstSight" in PARSER
# 不得为了通过而放宽："必须有 ServedCandidate" 的旧规则仍在
assert "servedCandidateBeforeRejected" in PARSER

# ---- 6. 宿主测试：两类链都有集成见证 ----
assert "Case24FirstSightObservationChain" in HOST_TEST
assert 'RunCase("24 first-sight observation chain vs reject-recovery chain"' in HOST_TEST
assert "PaletteObjectStage::FirstSight" in HOST_TEST
assert "a normal chain must NEVER be reported as Recovered" in HOST_TEST
assert "unknown identity must stay NoIdentityProof (never upgraded)" in HOST_TEST
assert "a genuinely certified key keeps InstanceLifecycleIdentityProof" in HOST_TEST
assert "recovery chain keeps the frozen stage=Drawn terminal shape" in HOST_TEST


# ---- 7. 跨语言版本注册一致性（2026-09-18 更正后新增）----
# 这是能提前抓出"写方发 v3、读方只注册 {1,2}"那类缺口的检查。
# 教训：批次 3 的门禁只断言了 Python 侧常量**存在**，没有把它和 C++ 写方**绑起来**，
# 于是"写方发一个读方不认识的版本"这种致命不一致在全绿套件下存活了下来。
FRAME_READER = (ROOT / "AutoTest/analyze_frame_evidence.py").read_text(encoding="utf-8")

# (a) C++ 写方能发出的版本必须与 Python 读方登记的版本一致。
# 阶段 C：写方恒定 v4 ⇒ 读方必须**登记 v4**，否则整份导出会被拒。
assert 'result["version"]=4;' in FRAME_EVIDENCE, \
    "写方的版本表达式变了；本一致性检查需要同步"
assert "PALETTE_OBJECT_CHAIN_TYPED_VERSION" in FRAME_READER, \
    "读方必须登记 v4（链型 + ObservationClosed），否则写方发出的 v4 会被整份拒绝"
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION=3" in FRAME_READER, \
    "读方必须登记版本 3，否则写方发出的 v3 会被整份拒绝"
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION:" in FRAME_READER, \
    "版本 3 必须进入 PALETTE_OBJECT_BLOCK_FIELDS（仅定义常量不够）"

# (b) 只定义常量、不登记形状，是最容易犯的错 —— 显式禁止这种半截状态。
# 注意：不要按第一个 "}" 切片 —— 那会截在 frozenset 内部（我第一版就是这么写错的）。
_start = FRAME_READER.index("PALETTE_OBJECT_BLOCK_FIELDS={")
assert "PALETTE_OBJECT_FIRST_SIGHT_VERSION:" in FRAME_READER[_start:_start + 400], \
    "PALETTE_OBJECT_BLOCK_FIELDS 必须包含 v3 条目（只定义常量、不登记形状 = 半截状态）"

# (c) v3 才允许的首见计数器，必须在解析器侧按版本校验（不能一视同仁）。
PARSER_CODE = (ROOT / "AutoTest/analyze_palette_object_evidence.py").read_text(encoding="utf-8")
assert "FIRST_SIGHT_COUNTER_FIELDS" in PARSER_CODE, \
    "首见计数器必须有自己的集合常量"
assert "if version>=PALETTE_OBJECT_FIRST_SIGHT_VERSION else COUNTER_FIELDS" in PARSER_CODE, \
    "counters 校验必须按版本分支，否则 v2 会被错误接受或 v3 会被错误拒绝"

# (d) 必须存在一条"把 v3 喂给两个读方并断言接受"的端到端用例。
HOST_ANALYZER_TEST = (ROOT / "AutoTest/test_palette_object_evidence_analysis_static.py").read_text(encoding="utf-8")
assert "test_explicit_version_3_is_registered_and_accepted_by_both_readers" in HOST_ANALYZER_TEST, \
    "必须有 v3 端到端正向用例（只测常量的门禁抓不到这类缺口）"
assert "format_version=3" in HOST_ANALYZER_TEST

print("first-sight static: PASS (+ cross-language version-registration consistency)")
