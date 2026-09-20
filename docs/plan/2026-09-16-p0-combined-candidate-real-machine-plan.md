# 2026-09-16 — P0 组合候选：实机验证方案（2026-09-17 凌晨按上级裁定修订冻结）

> 用途：把用户裁定的三项证明变成可执行、可判定的实机/隔离验证。
> 纪律：**静态通过不算证明**；**不得以"拒绝计数增加"代替替代路径证明**；
> 计数器是进程累计值，一律看**分段增量**；未命中目标路径记"未覆盖"，
> 缺数据记"观察结果"，不判完整验收通过。

## 0. 授权链与冻结身份

- 授权：用户（2026-09-17 凌晨）指示将上级审核提交 codex 线程
  `01a02e0b-1d1e-7762-b40b-63a00bbb3449` 裁决；该线程已给出
  **Q1 条件批准 / Q2 否决旧 DLL 基线 / Q3 批准同 DLL 环境变量覆盖 /
  Q4 方向(b) / Q5 收窄批准 S2**（裁定全文已记入台账）。
- **冻结候选**：`build32/src/d3d9/d3d9.dll` = **35,984,240 bytes** /
  SHA-256 `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE`
  （F855 诊断配置构建；结论必须注明该配置，不等于发布候选验收）。
- **测试现场**：仅 `E:\Work\War3`。开跑前实测其 `d3d9.dll`（2026-09-17 02:0x）=
  35,486,415 bytes / `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`
  （裁定记录的 `74CC676B…C6414F6` 尾串有误，**以实测为准**；备份于
  `C:\Windows\Temp\warvk_p0\backup\d3d9.dll.orig` 同哈希），
  完整备份原文件；**结束必须恢复原文件并核验零残留**，
  不得套用历史 055F 恢复源。**绝不触碰玩家目录 `E:\Work\Warcraft III`（F855）**。
- 实机事务期间（部署到恢复结算完成）：**暂停 B 树一切源码/构建修改**（Q5 条件 4）。
- 身份变化（候选哈希、现场哈希、Game.dll、地图文件任一不符）即停止。

## 1. 环境变量矩阵（已冻结，无"视需要"）

| 变量 | 第一轮（合同 ON） | 第二轮（合同 OFF） | 说明 |
| --- | --- | --- | --- |
| `DXVK_WAR3_SKIN_PALETTE_CONTRACT` | `1` | `0` | 进程级覆盖编译期默认（war3_skin_palette_selection.h:29 已核实；静态缓存 ⇒ **每轮 fresh process，不自动重试**） |
| `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT` | `1`（显式） | `1`（显式） | 替代路径核心来源，验证期间不得关闭 |
| `DXVK_WAR3_FRAME_TIMELINE` | `0` | `0` | 裁定：本轮关闭大容量采集 |
| `DXVK_WAR3_DATA_COLLECTION_TREE` | `0` | `0` | 同上 |
| `DXVK_WAR3_FRAME_EVIDENCE` | `0` | `0` | 同上（有界轻量诊断） |

### 1.1 M1 试跑结果与矩阵修正（2026-09-17 02:1x，pilot 轮）

- **pilot 轮（合同 ON + M1）完整通过**：三段各 600.0s、deviceLost=false、无新增 GPU
  incident、现场 DLL 未被改、相机/视野已恢复、92 份 perf HTML、驱动 exit 0。
- **但 M1 无法产生 P0 证据**（主线程源码核实 + 只读数据诊断）：
  1. **palette 来源分类 7 个计数器结构性恒 0**——整个分类块在
     `if (skinned && War3SemanticPaletteDiagnosticsRuntime())`
     （d3d9_device.cpp:23318），而该运行时门默认 **0**（同文件 2590-2598）。
     ⇒ 不加 `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1`，"替代路径/恢复"没有仪表。
  2. **Gap A 计数器恒 0 是结构性的**：合同编译默认 = 1（meson
     `warvk_skin_palette_contract_candidate=true` ⇒ `-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1`），
     `War3TryBuildLiveRuntimeGroupPalette` 在 8585-8603 提前 return，
     `resolvePaletteSlotIndex`/`useCachedEntry`（8691/8695 自增）整段不可达。
     ⇒ pilot 的 `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1` 是 **no-op**，必须显式 =0 才有 OFF 轮。
  3. **语义 core 整轮未构建**（coreFrameSerial/coreResolved 全程 0、
     BuildRequestPending 全程 true），core 唯一进程内消费点是
     `war3_renderer.cpp:385 runObserveValidation()`，其门默认 **false**
     （`DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD`，gate:69-71）。
     ⇒ 不打开它，Gap B（shadow-core 蒙皮路径）不可能有证据。
  4. pilot 快照里的标量是 **当帧值**（`g_shadowSceneStats` 每帧整体替换，
     bridge.cpp:4217-4568），不是累计值——分段增量必须用同一序列器的多次快照，
     且不得把单帧 0 当"整轮未发生"。
- **矩阵 M2（修正后正式轮使用；与 M1 仅差 4 个"激活/诊断"变量）**：
  在 M1 基础上增加 `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS=1`（来源分类仪表；
  热路径加 512 项查找表 ⇒ 性能单列）、`DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD=1`
  （激活 core 构建 ⇒ 改变语义/性能，单列）、
  `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE=1`
  （注册表发布次序 ⇒ 可能改变 world.valid/画面，单列）。
  已核实**不需要**设的（源码证明无效）：`..._BOOTSTRAP_CATCHUP`（gate:62-67 ×
  常量 false）、`..._TAIL_FALLBACK`（gate:80-84 × false）、`..._ENDFRAME_FLUSH`
  （gate:74-78 × false）、`..._SCENE_SUBMISSION`/`..._PRE_READY`/`..._PREVIEW`
  （默认已 true）、`DXVK_WAR3_SKIN_PALETTE_CONTRACT=1`（编译默认）。
- 正式轮安排：**R2 = 合同 ON + M2（M2 的安全/恢复门）→ R3 = 合同 OFF + M2**
  （Gap A 唯一可达配置）。两轮矩阵除合同外完全相同 ⇒ ON/OFF 比较有效。
  pilot(M1,ON) 只作为管线验证与 M1 基线，不参与 M2 的 ON/OFF 判定。

顺序：**先合同 ON 轮，安全与恢复门通过后才允许合同 OFF 轮**。
合同 OFF 轮用于覆盖 Gap A 路径；合同 ON 轮证明"合同 ON 未受影响"。
注意（裁定纠正）：合同 OFF **不会**撤回已写进源码的 Gap A/B 修复，它不是修复回退开关；
诊断 DLL 的 OFF 运行也不等于正式发布配置验收。

## 2. 场景脚本（三段，缺一不可，参数已冻结）

地图：`Maps\(4)生与死v1.28读档bug修复.w3x`（生与死，AutoTest 既有高压图）。
执行面：AutoTest 隔离桌面 runner / war3_autotest_mcp 的"生与死低视角巡航"能力
（冷启动进图、全图视野、5x5 蛇形低视角、首次设备错误立即止损）；**指向 E:\Work\War3
测试现场的接线在执行记录中逐条核实**，默认指向玩家目录的配置一律改掉。

1. **段 1 高压低视角（10 分钟）**：低俯角锁定最低档，5x5 蛇形巡航高压区。
2. **段 2 往返移动（10 分钟）**：镜头在单位群/建筑群与空地之间按 20 秒周期往返，
   制造 part/payload 身份漂移与槽位复用窗口。
3. **段 3 压力解除后恢复（10 分钟）**：停止巡航静止观察，验证被拒对象**重新投影**
   而非持续遗漏。阴影消失本身是待观察现象，**不得提前退出漏掉本段**。

每段边界各取一次报告快照（计数器是进程累计值，**判定只用分段增量**）。

## 3. 采集字段与证明口径（按上级裁定修正）

既有来源分类 + Gap A/B 新计数器（全已于 2026-09-17 凌晨导出，守卫测试
`test_palette_slot_cache_counters_export_static.py`）：
- 来源分类：`…SourceNoneCount` / `…DrawTimeCapturedCount` / `…SubmitTimeGlobalSlotCount` /
  `…SubmitTimeBlendedCacheCount` / `…SubmitTimePublishedRegistryCount` /
  `…SubmitTimeCModelFallbackCount` / `…OwnedPartSnapshotCount` / **`…SourceChurnCount`**
- Gap A：`semanticSceneSkinnedPaletteSlotCacheDeviceServedAfterConfirmCount` / `…DeviceRejectedStaleCount`
- Gap B：`semanticSceneSkinnedPaletteSlotCacheShadowCoreServedAfterConfirmCount` /
  `…ShadowCoreRejectedStaleCount` / `…ShadowCoreProducerSnapshotFallbackCount`

**裁定修正的证明口径**：
- 五个新计数器是进程累计值 → 一律取**分段增量**；累计上升不等于故障持续积累。
- `SourceChurn` 下降**不**单独证明矩阵正确；快照命中增加**不**单独证明被拒对象得到替代。
- 三项证明必须关联到**同一对象/part、帧与代际、实际来源、矩阵证据及后续投影**；
  关联不上的记"未覆盖"，不判通过。

**Gap B 已知局限（裁定发现，本轮结论的上限）**：core.cpp:799-805 的复核取回了
`boundGroupCount` 但**未参与判定**，也未校验帧新鲜度；producer 快照优先调用未接收
frameTag。底层查询返回的是记录的绑定/快照，**不是当前原生 arena 槽位所有权证明**。
⇒ 本轮最多宣称"观察结果与路径覆盖"，**不能宣称"完全排除了错误矩阵"**。
补强（groupCount 参与判定 + 帧校验）列入实机结算后的下一候选，会改变 DLL 哈希，
**不在本轮冻结候选内修改**。

## 4. 判定矩阵（修正版）

| 项 | 判定（全部基于分段增量 + 同对象关联） | 反例/否决 |
| --- | --- | --- |
| ① 错误矩阵不再被使用 | 未经 producer 确认的 GlobalSlot/BlendedCache 读取增量为 0；且能按对象关联：上一候选中出现 churn 的对象本轮来源为 confirmed/snapshot/registry | 仅 RejectedStale 上升而 churn 不降；或无法做同对象关联（记"未覆盖"） |
| ② 合法对象仍有替代路径 | 被拒对象（RejectedStale 增量）中能关联到后续 OwnedPartSnapshot/PublishedRegistry/DrawTimeCaptured 命中的比例显著；SourceNone 增量不同幅上升 | SourceNone 同幅上升 = 把拒绝当修复 |
| ③ 失败后可恢复 | 段 2 被拒对象在段 2 后期/段 3 重新出现 confirmed/captured/snapshot 命中；SourceNone 不逐帧单调累积 | 永久黑名单/单调遗漏 |
| 反例门 | 段 1+2 正常单位/建筑/桥梁/装饰物投影正常（无整体阴影丢失且来源计数健康） | 画面阴影整体减少而无替代路径证据 |

## 5. 基线与宣称限制（Q2 裁定）

- **否决**把 `1C314090…`（已含 Gap B）当作完整修复前基线；不批准临时翻旧 DLL 凑基线。
- 本轮允许**无修复前基线的运行观察**，但禁止宣称：性能收益数值、相对修复前消除多少错误、
  "ON/OFF = 修复前/修复后"。
- 若后续需要因果对照，另行冻结"仅目标修复不同"的成对版本。

## 6. 执行条件（冻结）

1. 非交互隔离桌面、零全局输入、无并发编译（B 树构建冻结）。
   **分辨率偏差（2026-09-17 02:0x 实测记录，必须随结论一并声明）**：
   冻结值 2560×1440 在本机当前会话不可达——物理/GPU 模式虽为 2560×1440，
   但会话桌面由 Oray 虚拟显示驱动提供为 **2048×1152**（AllScreens 实测），
   且 AutoTest 的 ensure_war3_video_baseline 只改游戏注册表、不改桌面模式。
   处置：两轮均使用会话实际分辨率 2048×1152，**不得**把游戏注册表设成 2560×1440
   造成窗口与桌面不一致；该偏差写入执行记录与结论头部。
   影响边界：两轮内部一致 ⇒ on/off 与段间比较仍有效；
   **不得**与历史 2560×1440 数据横比，也不得据此声称绝对性能。
2. 每轮 fresh process；不自动重试、不补跑。
3. 停止条件：崩溃、device lost、恢复流程失败、身份核验不符——立即停止并保留证据。
4. 恢复结算：停止后恢复原 `d3d9.dll` 并哈希核验；确认测试现场零残留；
   写执行记录（各段增量、停止/通过、证据路径）。
5. 不得为了"通过"提高预算、删合法对象或放宽门槛。

## 7. 结论纪律

- 结论必须区分：接线完成 / 静态合同 / 画面行为（观察结果 / 未覆盖 / 通过）/ 性能。
- 禁止用语：以"已修复阴影消失"描述仅通过静态与计数的状态。
- 玩家现场保护：不得自动替换玩家在用 DLL；本轮全程不碰 `E:\Work\Warcraft III`。
