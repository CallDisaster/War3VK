# 批次 4：真实蒙皮选择入口的路径核实与准入规则 — 2026-09-18

> 依据外部独立复审批次 4：「先明确实际构建是否开启严格 palette 契约，然后围绕真实选择入口，验证正常输入、slot 重用、矩阵数量不足和对象／地图切换」。
> **本文只含"读码级"结论；运行期正常/异常对照尚未完成**（见 §6）。未提交 / 未部署 / 未晋升稳定。

## 1. 严格契约是否启用 —— 已确认（可复核）

`meson_options.txt` 中 `warvk_skin_palette_contract_candidate` 默认 `false`，但 **`build32` 实际配置值 = `True`**（`build32/meson-info/intro-buildoptions.json`，已随检查点 c0 存档）。

## 2. 决定性事实：R1 修复的代码在**本构建中不可达**

`src/d3d9/war3/semantic/war3_live_palette_selection.cpp` 是**互斥双路**：

```cpp
if (dxvk::war3::render::skin::ContractEnabled()) {          // :236  <- 我们构建 = true
  dxvk::war3::render::skin::Selection selected;
  if (!QueryOwnedRenderablePartPaletteSnapshot(...))        // :241
    return false;                                          // :244
  ... return true;                                         // :253
}                                                          // —— 到此必然已返回

auto resolvePaletteSlotIndex = [&](void* partPtr) ...       // :256  <- legacy 路径
  // 含 R1 修复的冷缓存数量检查；只有契约**关闭**时才可能执行
```

⇒ `ContractEnabled()==true` 时函数一定在 `:244` 或 `:253` 返回；**`:256` 起的整段 legacy 路径（含 R1 修复）在本构建中是死代码**。
外部复审当时即指出「严格路径会提前走另一套拥有者快照逻辑，不涉及这个复现」—— **该判断经读码证实正确**。

### 2.0 严格路径的第二道前置也是开的

```cpp
static inline bool RenderablePartPaletteSnapshotEnabled() {          // war3_model_hook.cpp:2413
  static const bool enabled =
      GetEnvBoolCached("DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT", true);   // 默认 true
  return enabled;
}
```

⇒ 本构建同时满足 `ContractEnabled()==true`（meson 选项）与 `RenderablePartPaletteSnapshotEnabled()==true`（默认值），
因此严格路径不仅**可选**，而是**默认生效**；除非显式设置环境变量关闭。

### 2.1 对 R1 的重新定位（不是撤回）

- R1 修复本身**正确且必要**：该选项的**默认值是 false**，任何按默认构建（或将来关闭契约）的产物都会执行该路径；
- 但在**我们当前部署的构建**里它不产生运行期影响 ⇒ **不能**用它解释或修复任何现场现象；
- ⇒ 「严格路径是否也有等价缺口」才是本构建的关键问题，见 §3。

## 3. 本构建实际走的严格准入路径 —— 逐条回答四个问题

入口：`QueryOwnedRenderablePartPaletteSnapshot()`（`src/d3d9/war3/model/war3_model_hook.cpp:9384-9408`）

| 复审的四个问题 | 读码结论 |
| --- | --- |
| ① **选中了什么数据** | 从 renderablePart 绑定单元 `(part>>4) % kRenderablePartPaletteBindingCacheSize` 取 `entry.sealedSelection` + `entry.palette`；来源被限定为 `Source::OwnedPartSnapshot`（`OwnedSnapshotMatches` 第一项即要求它） |
| ② **为什么允许使用** | 必须**同时**满足：`RenderablePartPaletteSnapshotEnabled()`；`runtimeModel`/`part` 非空；`0 < required <= 64`；slot 与 frameTag 当场可读；`OwnedSnapshotMatches(...)`（source=OwnedPartSnapshot、space=World、domain=VertexGroups、model/part/ownerEpoch 逐项相等、`publicationTicket!=0`、`hash!=0`、`s.slot==currentSlot && currentSlot<0x3a98`、`s.frameTag==currentFrame`、**`GroupRange(required, actualGroupCount)`**）；`count == selection.actualGroupCount && count <= 64`；绑定单元非忙（`TryCell` 非阻塞） |
| ③ **提交时会不会被换掉** | **会重读并作废**：拷贝出 `required` 个矩阵后，重读 `part+offset` 的 slot 与 frameTag，并要求 `afterSlot==currentSlot && afterFrame==currentFrame && epoch 未变`；任一不符 ⇒ `out.clear(); return false;`（TOCTOU 自检，失败即整体拒绝，不留半份数据） |
| ④ **正常对象会不会被误伤** | **从准入规则无法排除该风险，需要运行期对照** —— 该路径要求 `publicationTicket!=0`、`hash!=0`、`currentSlot<0x3a98`、`required<=64`，任一不成立即该对象**无 palette**（fail-closed）。这些前提下「正常对象被误伤」与「不合规对象被正确拒绝」在读码层面**不可区分** |

### 3.1 数量规则（回答"是否所有入口都受检查"）

```cpp
inline bool GroupRange(uint32_t required,uint32_t actual) noexcept {   // war3_skin_palette_selection.h:34
  return required && actual && actual <= 256 && required <= actual;     // <- 数量规则
}
```

| 路径 | 热缓存 | 冷缓存首次查询 |
| --- | --- | --- |
| **严格路径（本构建实际执行）** | 受 `GroupRange` 检查 | 受 `GroupRange` 检查（同一函数，无分叉） |
| legacy 路径（仅契约关闭时） | 受 `producerGroupCount >= requiredPaletteCount` 检查 | **原先缺失** ⇒ 已由 **R1** 补上（本构建不可达） |

⇒ 对「是否所有入口都受这项检查」的完整答复：**本构建走的严格路径全部受检；legacy 路径的冷缓存缺口已修，但只影响按默认构建/关闭契约的产物。**

## 4. 对批次 4 计划的影响

复审原意是"围绕真实选择入口验证正常/异常对照"。读码结果表明**本构建的真实入口就是严格路径**，因此：

- 验证对象应从 legacy `resolvePaletteSlotIndex` **改为** `QueryOwnedRenderablePartPaletteSnapshot` 的准入与 TOCTOU；
- 需构造的对照：**正常**（ticket/hash/epoch/slot/frame/count 全合规）／**数量不足**（`required > actualGroupCount`）／**提交中途被换**（slot 或 frameTag 在拷贝后改变）／**对象或地图切换**（epoch 改变）；
- 「选中什么数据、为何允许、是否被换掉」三问已由 §3 读码回答；**只有第 ④ 问必须靠运行期对照**。

## 5. 已有可用的测试基础设施

`src/d3d9/war3/render/tests/war3_live_palette_selection_test.cpp` 内**已有**该准入函数的替身实现（`:2839`），上述四类对照可在**宿主测试**内构造，无需实机；另有 legacy 参考实现 `war3_live_palette_selection_chain_legacy_reference.inc`（`:253` 同样调用该准入函数）供差分。

## 6. 未完成（不得含糊）

- **运行期正常/异常对照未做**：四类对照尚未实现为可执行测试，**第 ④ 问仍无证据**；
- **全门禁未复跑**（全量静态 / meson 全量 / Win32 runnable 全量 / TDR / ABBA / 玩家前台门）；
- 批次 3 的 FirstSight 链**仍无实机非零导出**；
- 未提交、未部署；现场 DLL 仍是 `F275545B…`（`519AFA69…` 备份在位）；未改用户视频设置。

## 7. 边界声明

本文全部结论来自**读码**（含精确行号与函数体），**不含**运行期实测；不得据此宣称"蒙皮选择路径已验证"。