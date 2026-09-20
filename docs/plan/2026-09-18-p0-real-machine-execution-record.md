# P0 实机对象级取证执行记录（2026-09-18，DLL 已替换后首次两轮采集）

## 0. 结论（一句话）

**有证据**：新候选 DLL 已成功加载并完成两轮完整采集，**对象级子门首次在游戏进程内为 ON**（这是相对 09-17 的实质进步）；但**对象级证据两次均为零输出**，且**当前无法从导出区分"未 arm"与"已 arm 但零发射"**（代码级原因见 §4）。判定：**未覆盖**（对象级链仍未取到证据）。

## 1. 部署与产物（事实）

| 项 | 值 |
| --- | --- |
| 现役 DLL | 36,283,128 B / `519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306` |
| 替换前现场（备份） | `E:\Work\Warcraft III\d3d9.dll.A0A51AF2_backup_20260918-092843` = 36,271,456 B / `A0A51AF2…`（字节精确） |
| 回退脚本 | `E:\Work\Warcraft III\回退到A0A51AF2.cmd` |
| 启动器 | `启动-取证-含对象级子门.cmd`（`DXVK_WAR3_FRAME_EVIDENCE=1` + `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`） |
| 高压导出 | `cpu-17616-1076560418617-1.json`（09:57:53，36,385,526 B） |
| 低压导出 | `cpu-23712-1077836818029-1.json`（10:00:04，32,901,023 B） |

## 2. 有证据的部分（两轮均通过）

| 指标 | 高压(09:57) | 低压(10:00) |
| --- | --- | --- |
| `effectiveConfiguration.frameEvidence` | **true** | **true** |
| **`effectiveConfiguration.paletteObjectEvidence`** | **true** | **true** |
| `effectiveConfiguration.skinPaletteContract` | true | true |
| `accepted`（CPU 边界事件） | **578,898** | **881,000** |
| `inputs.ok` | true | true |
| 导出可解析（头块读到） | 是 | 是 |

⇒ **DLL 加载成功、录制链完整、子门已在进程内生效**。

## 3. 对象级证据为零（问题）

| 指标 | 高压 | 低压 |
| --- | --- | --- |
| `paletteObject.watchCount` | **0** | **0** |
| `paletteObject.counters.emitted` | **0** | **0** |
| 全部 `dropped*` / `closed*` | 0 | 0 |
| 导出内 `palette-object/v1` wire 事件数 | **0** | **0** |

### 阴影管线本身是活的（排除"阴影没跑"）

（取自同刻 `war3_perf_report_2026_09_18_10_00_14.html`）

- `semanticSceneShadowCastersCount` = **169**；
- `semanticSceneReceiverHasCompleteShadowMap` = **1**；
- `drawTimeSemanticProducerSubmittedCount` = **109,877**；
- `drawTimeSemanticProducerOwnedDirectGroupedSkipCount` = **101,838**（防御性所有权门跳过）。

### 但"提交期 skinned palette"记账**全为 0**（重要线索）

- `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount` = **0**；
- `semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount` = **0**。

⇒ 对象级证据所依赖的"提交期 skinned 调色板"路径在该会话中**没有产生记账**。

## 4. 代码级：为什么头块无法区分两种情形（不得过度解读）

`war3_palette_object_evidence_sink.cpp:167-175`：

```cpp
void QueryPaletteObjectEvidenceHeader(PaletteObjectEvidenceHeader& out) noexcept {
  if (!g_paletteObjectArmed) {            // 未 arm -> 显式全 0
    out.watchCount = 0u;
    out.counters = PaletteObjectEvidence::Counters{};
    return;
  }
  g_paletteObjectEvidence.SnapshotCounters(out.counters, out.watchCount);  // 已 arm -> 真实计数
}
```

- **未 arm** ⇒ 头块全 0；**已 arm 但零发射** ⇒ 头块**同样**全 0；
- ⇒ **仅凭导出头块无法判定是哪种**。本记录**不声称**"未 arm"，也**不声称**"已 arm 无事件"。

### 相关的两道门（读码）

- `d3d9_device.cpp:22113`：`paletteObjectEvidenceOn` = 子门（已 ON ✓）；
- `d3d9_device.cpp:22122-22124`：`ActiveSession() != 0` 才会构造键并入队。而 `ActiveSession()`（`war3_frame_evidence.cpp:202`）在**证据环冻结时被置 0**（`:225`）⇒ 若对象级入队只发生在环冻结之后，就会全部落空。

## 5. 下一轮需要的数据（三选一即可判定）

1. **在游戏中查询（首选）**：需要能读到 `g_paletteObjectArmed` 与实时计数 —— 当前控制面**未暴露**该字段（已核实 `frame_evidence_control.py` 与 `war3_frame_evidence_control.h` 均无 `paletteObject`）⇒ 若要走这条路，需要**加一处只读诊断导出**（改码 + 重构建 + 重新部署）；
2. **确认地图/场景**：告诉我这两轮打的是哪张图、战斗中视野内是否有**带骨骼的动画单位**（英雄/普通单位），以及**阴影在画面上是否可见**；
3. **复现一次带 skinning 的场景**：用确定有大量动画单位的地图再采一轮，看 `semanticSceneSubmittedSkinnedPaletteSource*` 是否变为非 0。

## 6. 边界（不得越界）

- 本记录只写「有证据 / 未覆盖 / 仅趋势」，**不写"已解决"**；
- 「子门 ON」**不等于**「对象级证据已取到」；
- 本记录**不**作为 GPU 提交或像素正确性证据；生产链 `identityNotProven` 是**有意收紧**；
- 全程未在用户游玩期间部署/改动文件；替换动作经用户明确授权，且已完成字节精确备份。

---

## 7. **关键推进：歧义已消除 ⇒ 确定为「已 arm，但零发射」**（10:20，读码 + 产物交叉验证）

### 7.1 决定性证据

```cpp
// war3_frame_evidence.cpp:163-169
const RecorderConfiguration& recorderConfiguration() noexcept {
  static const auto config=ResolveRecorderConfiguration(... std::getenv("DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT"));
}                                    // function-local static：全进程**只解析一次**
```

1. 配置是**一次性静态**（`static const auto`），⇒ arm 时刻与 export 时刻读到的是**同一份**配置；
2. 导出头块 `effectiveConfiguration.paletteObjectEvidence = true` ⇒ arm 时刻子门也为 true；
3. `ArmPaletteObjectEvidence`（`sink.cpp:146-154`）在子门为 true 时**无条件**置 `g_paletteObjectArmed = true`；
4. 导出路径 `ok:true`；若 arm 未发生，`Control(export)` 会以 `no session` / `session mismatch` 失败（`war3_frame_evidence.cpp:289/293`）；
5. `DisarmPaletteObjectEvidence()` 的**唯一**调用点是 `:343` 的 `action=="discard"` 分支（已全量检索），本轮无 discard。

⇒ **`g_paletteObjectArmed == true`**。因此头块里的 `watchCount=0 / emitted=0` 是 `SnapshotCounters()` 返回的**真实零**，
**不是** `!armed` 时的显式掩码。

### 7.2 这修正了 §4 的表述

- §4 说"无法区分两种情况"在**代码层面**成立（两者输出相同），但**结合配置一次性 + arm 成功这两条外部事实**，现在可以**确定**是"已 arm、零发射"；
- ⇒ 问题不在开关，而在**发射准入**。

### 7.3 指向的具体机制（假设，待证）

发射准入在 `d3d9_device.cpp:22121-22124`：

```cpp
if (paletteObjectEvidenceOn) {                       // 已 ON ✓
  const uint64_t paletteObjectSession = ActiveSession();
  if (paletteObjectSession != 0u) { ...构造键... }    // <- 若恒为 0，则整段落空
}
paletteObjectEnqueueReady = true;                    // 只在上面 if 内设置
```

而 `ActiveSession()`（`war3_frame_evidence.cpp:202-206`）= `s->active.load()`，且 `active` 在**证据环进入 Frozen 时被置 0**（`:225`）。
⇒ 若 caster 追加发生在环已 Frozen（或 `active` 尚未建立）的时段，则**每次追加都被静默跳过**，与"169 个 caster、109,877 次语义提交、却零发射"完全吻合。

### 7.4 可零改码验证的预测（首选下一步）

若上述机制成立，则：**按 Ctrl+Shift+C 之后继续游玩 30–60 秒**（环处于 Triggered/Armed 且 `active` 非 0 的窗口内）应能产生**非零** `watchCount/emitted`。
⇒ 请再做一轮：进图 → 战斗 → **按一次 Ctrl+Shift+C** → **不退出，继续打 30–60 秒** → 再退出。

若这样仍为 0，则改为最小诊断构建（在头块暴露 `armed` 与 `ActiveSession()`），一次采集即可定案。

### 7.5 边界

- 本节仍**不**声称对象级链已通；判定维持 **未覆盖**；
- 本节**不**作为 GPU 提交/像素正确性证据；
- 未在用户游玩期间改动任何文件。
