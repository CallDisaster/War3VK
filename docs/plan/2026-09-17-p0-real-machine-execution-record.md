# 2026-09-17 — P0 组合候选实机执行记录（三轮胎、已恢复现场、未部署为稳定版）

> 用途：记录 2026-09-17 凌晨 P0 组合候选实机观察的**原始事实、判定与边界**。
> 纪律：静态/接线 ≠ 画面；计数器分段增量；缺对象级关联一律记"未覆盖"；
> 本记录**不宣称**"阴影消失已修复"或三项证明通过。

## 0. 冻结身份与现场结算

| 项 | 值 |
| --- | --- |
| 候选 DLL | 35,984,240 B / SHA-256 `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE`（F855 诊断配置：`warvk_skin_palette_contract_candidate=true`、`warvk_internal_frame_recorder=true`） |
| 测试现场 | `E:\Work\War3\d3d9.dll`，仅此一处 |
| 部署前现场 DLL | 35,486,415 B / `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`（备份于 `C:\Windows\Temp\warvk_p0\backup\d3d9.dll.orig`，哈希一致） |
| Game.dll | 13,187,048 B / `E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A` |
| 地图 | `Maps\(4)生与死v1.28读档bug修复.w3x` 62,290,145 B / `548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F` |
| **现场结算** | **已恢复原 DLL 并核验哈希一致**；玩家目录 `E:\Work\Warcraft III` 全程未触碰（现仍 F855A67C…）；无残留进程；无多余 d3d9*.dll |
| 分辨率偏差（**已按实测更正**） | 冻结值 2560×1440 不可达。实测（v2 分析器从 driver_result.json 客户区 + 截图 IHDR + 运行时视口三方印证）：**客户区 1902×963**（窗口外框 1920×1010，运行时 receiver viewport 1902×722）。此前记录写的"2048×1152"**与产物不符，已更正**；三轮内部一致 ⇒ on/off 与段间比较有效，但**对 2560×1440 与 2048×1152 都不得外推** |

## 1. 三轮设计与执行结果

| 轮 | 矩阵 | 合同 | 结果 |
| --- | --- | --- | --- |
| round1_contract_on | M1（6 变量） | ON | 管线验证通过；P0 目标计数全 0（结构性） |
| round2_on_m2 | M2 = M1 + PALETTE_DIAGNOSTICS=1 + SHADOW_ENDFRAME_BUILD=1 + PUBLISH_REGISTRIES_BEFORE_SCENE=1 | ON | 完整通过；P0 目标计数仍全 0（canonical 门在 ON 下 100% 拒绝） |
| round3_off_m2 | M2（同上） | **OFF** | 完整通过；**Gap A 计数复活**，分类仪表部分复活 |

三轮共同的成功指标：三段 observedSec 均 ≈600.0s（误差 <0.1s）、deviceLost=false、
stopReason 空、newGpuIncidents=[]、newGpuEvents=0、`dllUnchanged=true`、
restoreOk={camera,visibility,stopped}=true、每轮 91–92 份 perf HTML、驱动 exit 0。
产物：`C:\Windows\Temp\warvk_p0\round{1_contract_on,2_on_m2,3_off_m2}\`。

## 2. 关键实测数字（只引用可复算的原值）

### 2.1 Gap A（device 侧记忆槽位复核）在合同 OFF 轮复活

| 键 | seg1 | seg2 | seg3 | Δ(2-1) | Δ(3-2) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `semanticSceneSkinnedPaletteSlotCacheDeviceRejectedStaleCount` | 31,881 | 88,926 | 91,039 | **57,045** | **2,113** |
| `semanticSceneSkinnedPaletteSlotCacheDeviceServedAfterConfirmCount` | 0 | 0 | 0 | 0 | 0 |

- 该键是 process-cumulative 原子计数（单调增）⇒ 分段增量有效。
- 结论：**合同 OFF 的生产路径确实反复走到"记忆槽位不可信"的分支，复核机制不是空转**；
  即旧 fail-open（producer miss 直接供出记忆槽位）在实机路径上是被命中的。
- 恢复段（seg3）拒绝增量从 57,045 降到 2,113 ⇒ 与"压力解除后拒绝减少"方向一致，
  但见 §3 第③项：**无同对象关联，不得当作"失败后可恢复"的证明**。
- `ServedAfterConfirm` 全程 0 ⇒ 本轮**没有一次**"producer 确认后供出合法快路径"的命中。

### 2.2 来源分类（判定"替代路径"的仪表）在 OFF 轮部分复活，但为**当帧值**

| 键 | seg1 | seg2 | seg3 |
| --- | ---: | ---: | ---: |
| `…SourceSubmitTimeGlobalSlotCount`（危险读者①） | 6 | 0 | 0 |
| `…SourceDrawTimeCapturedCount` | 0 | 0 | 0 |
| `…SourceNoneCount` | 0 | 0 | 0 |
| `…SourceOwnedPartSnapshotCount` | 0 | 0 | 0 |
| `…SourceSubmitTimePublishedRegistryCount` | 0 | 0 | 0 |
| `…SourceChurnCount` | 0 | 0 | 0 |

**口径警告（必须随结论声明）**：这些字段位于 `m_war3Scene.shadowStats`，每帧被整体
替换（`war3_shadow_runtime_bridge.cpp:4217-4568`）⇒ 边界快照是**当帧值**，
其"Δ"不表示累计增量。上表只说明：采样瞬间未观察到这些来源的非零值，
**不可**据此推断整轮计数为零（样本内 max 见 §2.4）。

### 2.3 Gap B（shadow-core）全轮不可度量

`ShadowCoreServedAfterConfirm / RejectedStale / ProducerSnapshotFallback` 三轮全程 0。
原因：语义 core 在 M2 下**确实构建完成**（`semanticCoreFrameSerial == ManifestFrameSerial`、
`publishRevisionLag=0`、`buildInProgress/Pending=0`），但 `considered=2 / skippedNoGeoset=2 /
skippedResourceMiss=2 / resolved=0` ⇒ draws 为空 ⇒ `submittedDrawCount=0`；
shadow-core 蒙皮 caster 路径整轮未运行。资源查找失败点在
`war3_shadow_renderer_core.cpp:7553-7561`（四键全 miss），属**代码问题，非 env 可解**。

### 2.4 样本内极值（3300+ 采样/轮，含 runtime_status.json 627 键全量）

| 键 | round1(M1,ON) | round2(M2,ON) | round3(M2,OFF) |
| --- | ---: | ---: | ---: |
| `semanticSceneSubmitted` | max 178 | max 340 | max ~340 |
| `semanticSceneSubmittedSkinned` | max 52 | max 336 | max ~40 |
| `…SourceSubmitTimeGlobalSlotCount` | 0/842 | 0/1092 | max 16（564/1177 样本非零） |
| `…SourceNoneCount` | 0 | 0 | 0 |
| `…PaletteSourceChurnCount` | 0 | 0 | 0 |
| `…DeviceRejectedStaleCount` | 0 | 0 | 样本点 max **91,034**（v2 分析器全样本重算；本节早期写的 33,819 是 phase1 窗口内的样本最大值，**统计版本不统一，以 91,034 为准**） |
| `semanticCoreResolved` | 0 | 0 | 0 |

⇒ 合同 ON（M1/M2）下 canonical 就绪门在 `d3d9_device.cpp:23166-23172` 对全部包返回 false
（`CanonicalReadyCount` 全程 0），使 23172 之后整段（含 23318 分类块与 24882/25917 提交计数）
成为结构性死代码；**合同 OFF 打开该门**，仪表才可能出数（已由 R3 证实）。

## 3. 三项证明判定（严格按方案 §4 修正版）

| 项 | 判定 | 依据 | 缺什么 |
| --- | --- | --- | --- |
| ① | **上级裁定措辞（08:2x）**：\"已观察到记忆槽位复核拒绝路径；'错误矩阵不再被使用'尚未证明\"。`RejectedStale` 含绑定缺失/数量不足等拒绝，**不能全部解释为"确认发现陈旧矩阵"** | Gap A 复核在 OFF 轮大量拒绝（Δ21=56,621，v2 分析器口径）；采样点 `SourceNoneCount`=0 | 无对象级关联；ON 轮门关闭导致无对照仪表 |
| ② 合法对象仍有替代路径 | **未覆盖** | `ServedAfterConfirm`=0（无合法快路径命中）、`OwnedPartSnapshot/PublishedRegistry/DrawTimeCaptured` 采样瞬时均为 0 | 缺同对象替代来源归属；不得用"拒绝数增加"代替 |
| ③ 失败后可恢复 | **未覆盖（仅有趋势）** | 恢复段拒绝增量显著下降（57,045 → 2,113） | 无同一被拒对象在后续帧重新命中的证据；下降也可能只是内容/视角变化 |
| 反例门（画面） | **未覆盖** | replay/casters 每帧 400–1,093 持续存在，shadow map 多数采样执行 | 缺画面级/对象级证据；本记录不做视觉判定 |

**总判定（含上级 08:2x 裁定）**：本轮为"运行观察 + 路径覆盖"，不足以宣布 P0 修复通过。
另须随结论声明：
1. 0.5s 采样给出的是**采样点最大值/占比**，不是完整逐帧统计；分段增量只对进程累计原子有效。
2. 三轮均为 `forced=true` 的**受控终止**；驱动 exit 0 不代表游戏自然退出已验收。
3. `CanonicalReadyCount=0` 只支持"**该运行未观察到就绪**"，**不得**定性为所有输入下的结构性死代码。
4. 报告不得复述"未经帧证明的 arena 读不可达"（该说法已被对抗性复核否定，见整晚总结 §5）。

## 4. 本轮暴露的、需要下一候选解决的问题（按优先级）

1. **Gap B 不可度量**：语义 core 的 manifest 记录在 `core.cpp:7519-7561` 四键资源查找全 miss
   ⇒ resolved=0。必须先修资源键匹配（代码），否则 Gap B 的三计数器永远是 0。
2. **来源分类仪表挂在合同 ON 下不可达的路径上**：分类块在 append 函数的 canonical 就绪门之后；
   ON 下该门 100% 拒绝 ⇒ 生产（合同 ON 是编译默认）路径上**没有**来源/替代路径仪表。
   若要"合法对象有替代路径"可度量，需在活跃路径补仪表：
   ① 23166 前加 skinned-only "append 被 canonical 门拒绝"计数；
   ② 阶段脉冲（22067/22338/22691/23099/23318）；
   ③ **最关键**：25917（`War3ActivateDrawTimeCacheEntry`）与 29772（draw-time fast append）
   才是真实 skinned 提交来源，需镜像 23318 的 8 桶来源计数。
   （风险极低：relaxed 原子 + 复用既有导出链。）
3. **无修复前基线**（上级裁定 Q2 否决旧 DLL 当基线）⇒ 本记录不做任何"改善幅度"宣称。
4. 合同 OFF 是既有回退开关，画面语义需截图人工核对；live palette rebuild 量很大
   （`submitLiveRebuildAttempt=408,617 / Hit=0 / Miss=408,617`）⇒ 性能必须单列。

## 5. 证据路径清单

- 轮次产物：`C:\Windows\Temp\warvk_p0\{round1_contract_on,round2_on_m2,round3_off_m2}\`
  （`run_plan.json` 含已核验 env 矩阵；`driver_result.json` 含 effectiveWar3Environment、
  restoreOk、dllUnchanged、perfSeries；`boundary_0N_*.json`/`shadow_summary_0N_*.json`
  为段边界全量快照；`samples.jsonl` 为 0.5s 采样；`counter_deltas.json/csv` 为驱动侧增量表；
  `perf_json/` + `perf_index.json` 为 91–92 份 perf 报告抽取）
- 分析输入：`C:\Windows\Temp\warvk_p0\m2_pair\round{1,2}\segment{1,2,3}.json`
  （round1=R2/ON，round2=R3/OFF，均为驱动原生 segment 文件）
- 分析器：`C:\Windows\Temp\warvk_p0\warvk_p0_analyze.py`
  （SHA-256 `A64C1F4DA0E1FB908CDD66D0009639046531D49B017B33D460C70C2C586EADFE`）
- 驱动：`C:\Windows\Temp\warvk_p0\warvk_p0_phase_driver.py`
  （1,883 行；R2/R3 用此定稿版本；R1 pilot 用的是同源早期版本，故 R1 由
  `prepare_segments.py` 从 boundary 生成 segment 文件）
- 现场备份：`C:\Windows\Temp\warvk_p0\backup\d3d9.dll.orig`
