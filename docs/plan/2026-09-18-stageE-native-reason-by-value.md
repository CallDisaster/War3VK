# 阶段 E · 原因字段按值携带（代码半）落地 — 2026-09-18

> 裁定条目：「把 D 点由 frameTag==0 反推的原因字段改为在实际清空/拒绝分支赋值并按值携带，
> 覆盖 :20321/:20452/:20865-20870/:21694-21697/:23299-23530 五个点位」。

## 1. 五个点位的实测性质（比裁定描述更具体）

  :20321-20324  初始选择（packet 权威 / currentDraw / 空 Selection）—— 不是清空分支
  :20452        selectedPalette = rebuildSelection       —— **替换**，不是清空
  :20865-20870  ★ 真正的两处事实来源：
                  :20865 if (!liveRuntimeGroupPaletteReady) selectedPalette = packet.paletteSelection;
                  :20868 if (skinned && ContractEnabled() &&
                  :20869     !IsSkinPaletteSelectionCurrent(selectedPalette))
                  :20870   selectedPalette = {};          —— **观测到陈旧 ⇒ 清空**
  :21694-21697  drawTimeVBOverrideApplied = true; draw.inputSkinSelection = {};
                —— **已经在真实分支赋值**（无需改动）
  :23299-23530  首见点，MakePaletteObjectFrames(..., 0u, false) 硬编码 false（不是反推）

**全文件只有一处反推**：

  :22137-22138  const bool paletteObjectNativeKnown =
                    !drawTimeVBOverrideApplied && selectedPalette.frameTag != 0u;

## 2. 为什么这是**真实缺陷**（不是洁癖）

`:20865` 在 live 刷新失败时把 `selectedPalette` 切到 **packet** 的选择；
packet 的 `frameTag` **可以非零**（它是 packet 自己的帧）。
于是 `:22138` 的 `frameTag != 0u` 会把**packet 的帧**谎报成「native 帧已知」❌。

而代码自己在 `:20862-20864` 就写明：「Strict mode additionally checks native frame
freshness below, so a failed live refresh cannot silently authorize an older packet's palette」
—— 即**来源身份**与**新鲜度**是两件必须分别携带的事实，不能从 tag 反推。

## 3. 改动（观测字段，不动准入/几何/优先级）

在**来源判定处**声明并赋值，在**新鲜度判定处**置假，在**判定处**使用：

  // :20865 之前
  bool paletteObjectSelectionFromLiveNative = liveRuntimeGroupPaletteReady;

  // :20868-20870（改为带花括号）
  if (skinned && ContractEnabled() &&
      !IsSkinPaletteSelectionCurrent(selectedPalette)) {
    paletteObjectSelectionFromLiveNative = false;   // 观测到陈旧 ⇒ 现场帧不可信
    selectedPalette = {};
  }

  // :22137-22138
  const bool paletteObjectNativeKnown =
      !drawTimeVBOverrideApplied && paletteObjectSelectionFromLiveNative;

⇒ nativeKnown ⟺ (1) 来源是**本帧现场**的 live 刷新 ∧ (2) 新鲜度**未**判陈旧
              ∧ (3) 未被 native draw-time override 清空。三件都按值携带。

**行为等价性说明（重要）**：在"陈旧 ⇒ frameTag 恰为 0"这一常见情形下新旧同值；
差异只出现在 **packet 回退且 packet tag 非零** 的情形 —— 那正是旧实现说谎的情形。

## 4. 静态锁（AutoTest/test_palette_object_native_reason_static.py，自动入门禁）

  1) 反推**代码形态**必须消失（注释保留说明是允许的）；
  2) 两个真实分支都必须赋值（来源判定处 / 新鲜度判定处）；
  3) 判定处必须消费**携带值**；
  4) 赋值点必须与新鲜度判定**同处**（偏移 < 400 字符）—— 防止"别处随手写"。

## 5. 载荷性（变异探针，具名失败）

  变异（把 frameTag 反推放回）:
    FAILURE: E: nativeKnown must not be re-derived from selectedPalette.frameTag
             (a packet fallback carries a NON-ZERO packet tag, so the derivation lies)

## 6. 状态

  STATIC = 263 脚本 / 0 失败    meson 85/0    no-work
  evidence 32/0                 wire CHECKS=1160 FAILURES=0
  DLL = B8AD9AFB24AAE1C8B2FE16AA64F2E3BCBB8C471524170EEA5C03FEBCEC85D978
  h d3d9_device.cpp = B228D8E1EE2839809655AEAB210CD22003BA2A0DD1116A92F35CC14E48785C63
  站点 = E:\Work\Warcraft III\d3d9.dll = F275545BAA65A015…（与基线逐字相同 ⇒ 未部署）
  git  = 无写操作

## 7. ⚠️ E 的**实机半**明确未做，且**不得擅自开始**

裁定对 E 还要求「白名单采集并保存完整启动环境」。这一半**未做**，原因有两条，**都是硬约束**：

  1. **AGENTS.md 明令**：构建或测试**不会**自动授权部署 DLL、覆盖 YDWE/Warcraft 文件、
     启动/关闭编辑器或游戏；此类操作需**用户明确请求** ⇒ 我**不**启动游戏；
  2. 裁定规定：两个 P0 完成前**不新增实机因果结论**、**不晋升稳定候选**；
     C 仍有一项（归属规则其余阶段形状）待裁定 ⇒ 即便能启动，也**不应**由本轮新增实机结论。

⇒ E 的实机半**等待用户明确授权的启动条件**，不是我在等裁定。

## 8. 不声称

- **不**声称本改动**改善了实机**（**无任何实机观测**）；
- **不**声称 packet 回退且 tag 非零的情形**在实机上出现过**（该情形由代码结构推出，未由数据支撑）；
- **不**声称 E 阶段完成（实机半未做）；**不**声称两个 P0 完成；
- 全局边界：一张表不代表一种事实，一条链结算不代表观察完整，
  观察完整不代表对象已证明，更不代表阴影已恢复。