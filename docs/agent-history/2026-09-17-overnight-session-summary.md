# 2026-09-17 整晚工作总结（P0 实机 + 两条候选入库；长期整理与优化进展）

> 主线程执行；子代理并行承担可切分工作（DeepSeek V4.1 Flash）。
> 纪律：未 commit、未发布、未把任何候选当稳定版；玩家目录全程未触碰；测试现场已恢复原状。
> 结论严格区分：**接线完成 / 静态合同 / 门禁通过 / 实机观察 / 画面通过**。

## 0. 一句话状态

P0 组合候选完成**实机三轮**（含合同 ON/OFF）并已恢复现场，拿到第一批真实计数；
据此确认两个结构性阻塞（Gap B 不可度量、来源分类仪表在合同 ON 下不可达）；
**Gap B 补强**与 **S2 共享纯计算内核**两条候选已入库且门禁全绿；Gap B 根因已定位到
一处谓词并出工单。**三项画面证明仍为"观察/未覆盖"——不得记作阴影消失已修复。**

## 1. 产物身份（必须分清）

| 身份 | bytes | SHA-256 | 状态 |
| --- | ---: | --- | --- |
| **实机验证过的冻结候选** | 35,984,240 | `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE` | 实机三轮；**已撤下现场** |
| Gap B 补强候选（中间态） | 35,994,449 | `05BEBA32ACAAD704FC863C3535B22125E2C290CEF99DE0D61800781C48CF5E6C` | 被 S2 覆盖 |
| **当前源码态候选（Gap B 补强 + S2）** | 35,996,016 | `F03A84E3303E4FE552855459868196460AAC87035BBA796160C3914B2FEE55D0` | 门禁全绿；**未部署、未实机、性能/画面未测** |
| 测试现场原 DLL（已恢复并核验） | 35,486,415 | `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73` | 备份完好 |
| 玩家目录 DLL | 35,884,733 | `F855A67C61A82686EF2CA4C4CE3E688C1BB7966004DDE014D82B4779EDDA39DD` | **全程未触碰** |

## 2. P0 实机三轮（凌晨 02:13–03:19）

- 设计：M1 试跑 → 修正为 **M2**（+ `PALETTE_DIAGNOSTICS=1`、`SHADOW_ENDFRAME_BUILD=1`、
  `PUBLISH_REGISTRIES_BEFORE_SCENE=1`）→ R2(合同 ON)+R3(合同 OFF) 成对，除合同外矩阵相同。
- 执行：三段各 600s；三轮 `deviceLost=false`、无 GPU incident、`restoreOk` 全 true、
  现场 DLL 未被改、91–92 份 perf 报告/轮、驱动 exit 0。
- **结果**：
  - **Gap A 复核在生产路径真实工作**：合同 OFF 轮
    `DeviceRejectedStaleCount` 31,881 → 88,926 → 91,039（Δ21=57,045 / Δ32=2,113，累计原子）。
  - `DeviceServedAfterConfirmCount` 全程 0 ⇒ 没有一次"合法快路径命中"⇒ **不得据此宣布通过**。
  - 来源分类键为**当帧值**（`g_shadowSceneStats` 每帧整体替换）⇒ 其"Δ"无判定意义。
  - Gap B 三轮全 0。
- **判定**：① 错误矩阵不再被使用 = **观察结果（部分）**；② 合法对象仍有替代路径 = **未覆盖**；
  ③ 失败后可恢复 = **未覆盖（仅趋势）**；反例门（画面）= **未覆盖**。
- 详见 `docs/plan/2026-09-17-p0-real-machine-execution-record.md`。

## 3. 本轮暴露的两个结构性阻塞（本晚最有价值的发现）

1. **Gap B 不可度量**：语义 core 构建完成，但 manifest 记录**全部**栽在四键资源查找
   （`considered=2 / skippedResourceMiss=2 / resolved=0`）。根因已定位：key3/key4 结构性为空
   （`m_byRuntimeModel`/`m_byModelResource` 唯一写入者未运行，实机 count 恒 0），
   真正的第一刀是 key1/key2，因为 manifest 的 runtime geoset 每帧从**活体内存**解析、
   从不来自缓存；三个能补 index/modelResourcePtr 的调用者被**编译期常量**关掉（非合同、非 env）。
   **无纯 env 解**；已出工单（见 §6）。
2. **palette 来源分类仪表在合同 ON 下结构性不可达**：分类块位于 append 函数 canonical 就绪门
   （`d3d9_device.cpp` 23166-23172）之后，ON 下该门对全部包返回 false（`CanonicalReadyCount` 恒 0），
   其后整段（含分类块与 24882/25917 提交计数）为死代码；而真实 skinned 提交来自 draw-time 路径
   **且完全没有来源归属** ⇒ 要证明"合法对象仍有替代路径"，必须在活跃路径补仪表。

## 4. 本晚入库的两条候选

### 4.1 Gap B 补强（主线程实施，P0 相邻）

- `FindOrUpdatePaletteSlotCache` 增加 `requiredPaletteCount`；调用点转发并补
  `[slot, slot+requiredCount)` 区间上界。
- 记忆槽位供出改为 **A0-A5 全链**：绑定命中 → 槽位域合法 → 与记忆一致 →
  **groupCount 覆盖所需矩阵数** → 绑定帧不旧 → **逐槽区间帧同源（missing==0 && min==max==绑定帧）**；
  任一失败仍聚合拒绝 + `return 0xFFFFFFFFu`，**不写状态（无负缓存）**。
- 快照路径 **S0-S3**：上限 64、`size() == requiredCount`（原 `>=`）、快照帧必须为当前帧。
- 新增 6 个本地计数（导出 5 个）沿 bridge/hub/control-plane/perf 四出口接线；容差
  `kPaletteSlotCacheMaxFrameTagDelta = 0u`（严格同帧）。
- **边界**：未做设计 §4.2 的 S4 ⇒ **不得宣称"快照路径已排除 FROZEN 旧槽位"**。
- 门禁：build32 exit 0 / `ninja -C build32 -n` no work / meson 72/72 / 静态 242/242。

### 4.2 S2 共享纯计算内核（子代理实施 + 主线程独立复核）

- 新增 `war3_runtime_group_palette_kernel.h`（312 行）与生产函数级测试
  `war3_runtime_group_palette_kernel_test.cpp`（908 行，**T1-T13 全过**）；
  `war3_shadow_renderer_core.cpp` 9778 行（-88）、`war3_upper_layer_shadow.cpp` 358 行（-98）
  改为薄适配层；meson 新增目标（72→73）。
- 硬边界遵守：内核上界全部来自输入、内部 0 处 `min()` 收窄、0 处内存读取、无
  renderablePart/slot/arena/producer 概念；**6+1 个调用点、两函数签名、来源链、logFailure、
  miss reason 逐字未改**。
- 等价性证据：30 万随机输入差分（4 步与 5 步两变体）**mismatches=0**；差分还独立证明
  适配层三条前置校验必须保留（不建模时 4399/300000 差异）。
- 主线程独立复核：`ninja -n` no work、内核可执行文件 all T1-T13 passed、meson **73/73**、
  静态全量 **242/242**、DLL 哈希与报告一致。
- **未部署、未实机**；性能与画面未测 ⇒ 不得宣称行为不变（尽管差分支持"计算等价"）。

## 5. 长期整理与优化进展（按工作流）

| 工作流 | 状态 | 未完成边界 |
| --- | --- | --- |
| 两树统一 A→B（P1-P11） | P4/P5/P8/P11 完成；P1/P2(0,2,3)/P3/P9/P10 部分；P6 暂缓；批次1 不执行 | P7 审计工具链、P3 的 40 处 native frame-sync 探针未移植 |
| 统一数据选择入口 S1-S4 | S1=P0（实机已做，未通过）；**S2 已实施入库**；S3/S4 未立项 | S2 未实机；S3/S4 无设计 |
| 语义职责迁出 device.cpp M1-M4 | **迁出量仍为 0**（门禁 37 符号 found=37/added=0/removed=0） | 四步均未开始；未因本轮两候选而推进 |
| 运行时开关三分类 | 清册完成（412 项 A156/B152/C19/待85）；Q4 已裁定方向(b) | A 类口径未按 Q4(b) 回写 |
| 固定门禁 | 2 个结构型 + 12+ 个合同型；本轮新增/加强 3 个静态断言面 | 迁移等价门禁无对象；S2 未加"迁移等价"门 |
| P0 palette 两处缝隙 | Gap A 完成+实机（部分证据）；Gap B 初版+补强入库 | 三项画面证明未通过 |
| P2 批次 0-7 | 0/2/3 完成；4/5/6/7 未开始 | 批次4 是批次6 硬前置 |
| 资源生命周期收口 | 既有度量面在位 | 批次4/5/6 未开始；Stage13 占用/回收零数据 |
| 诊断与证据链 | P5 六切片完成；5+5 计数器运行时/接线可见 | 深层 11 hook 默认无数据；Gap B 计数仍未实机 |

## 6. 下一步（已写成工单/记录，供接手）

1. **Gap B 根因修复工单**：`docs/plan/2026-09-17-gapb-resource-key-fix-workorder.md`
   —— 先做 **P0 纯 env trace**（`DXVK_WAR3_SEMANTIC_SHADOW_TRACE=1` +
   `..._CONTRACT_CAPTURE_PERIOD=1`，零渲染语义风险）把 key1/key2 定死；再上 **P1 一处谓词**
   （放宽 `IsContractUnitCandidate`），验收用 `semanticCoreResolved>0` /
   `SkippedResourceMiss→0` / `SubmittedDrawCount>0` + Arena/deviceLost + ABBA；
   manifest 只有 1-4 条是**上游 starvation**，与 P1 无因果关系，**不得混验收**。
   P1 改默认阴影行为，是否允许直接落地需上级裁定。
2. **活跃路径补仪表**（draw-time 提交 25917/29772 + append 门 23166 前），否则"合法对象有
   替代路径"永远不可度量。
3. 组合候选实机复跑（新 DLL 冻结后），用"同对象/part/帧"关联口径重判三项证明。
4. M1 语义职责迁出（纯判定）——第一个可交付的迁出步骤（当前仍为 0）。
5. 开关清册按 Q4(b) 回写口径；S3/S4 立项。

## 7. 纪律与纠正（防止复述旧错误）

1. **快照标量是当帧值不是累计值**；只有本轮新增的原子计数是进程累计。
2. 交接文档多条已过期（Gap A 进度、静态测试数、清册身份、"63 个玩家文档命中"应为 **0**）。
3. 来源分类 7 计数器默认不可见，且合同 ON 下即使打开也不可见。
4. 实机事务期间冻结源码（本轮遵守）；事务结束后才动源码，且**先备份再改**（本轮对
   core.cpp/upper/meson 做了文件级备份，无 git 写）。
5. "静态通过 ≠ 修复"；本轮所有候选均**未实机**，报告不得外推。

---

# 附：夜间后半段（04:00–08:15）新增事实与更正

> 本节由主线程在收尾时补写，**取代文中较早的中间数字**；上文正文保留为当时快照。

## A. 最终候选身份（取代正文 §1）

| 身份 | bytes | SHA-256 | 状态 |
| --- | ---: | --- | --- |
| **当前源码态候选（Gap B 补强 + S2 + 对抗性复核修复）** | 35,995,922 | `BFA4C4636DDEEAEA7935EF228AFD127D0CF7DDFBF67F4CCD0C971DABD22894F0` | 门禁全绿；**未部署、未实机、未 commit** |
| 中间态（仅 Gap B 补强 / +S2 初版） | 35,994,449 / 35,996,016 | `05BEBA32…` / `F03A84E3…` | 已被覆盖 |
| 实机过的冻结候选 | 35,984,240 | `8CECC495…` | 已从现场撤下 |

## B. 最终门禁（主线程统一复跑）

`ninja -C build32` exit 0；`ninja -C build32 -n` **no work to do**；meson test **73/73**；
AutoTest 静态全量 **243/243**；内核可执行 **all T1-T17 passed**。
S2 内核头 **318 行**、内核测试 **1,225 行 / T1-T17**；palette-slot 相关静态 **20 个**（新增 S2 单一来源门禁）。
新增门禁文件：`AutoTest/test_runtime_group_palette_kernel_single_source_static.py`
（内核纯度、重复实现消除、D2 差异不得合并、来源链留在适配层、两处空视图守卫）。

## C. 独立对抗性复核（今天质量上的一次关键收益）

由独立只读子代理完成（以 2026-09-15 artifact 为"抽取前"基线）。结论：
**S2 未发现行为不等价**；**Gap B 的 A0-A5 只覆盖"记忆槽位"一条供出路径**。

- **过度宣称（已更正）**：`FindOrUpdatePaletteSlotCache` 的
  (a) 命中且 `+0x08` 合法 ⇒ 覆写记忆并直接供出、
  (b) 未命中且 `+0x08` 合法 ⇒ 首见插入并直接供出，
  两条**仍为未证兜底**（有意保留，与 2026-09-16 架构复审口径一致）。
  设计 §8① 与静态测试注释原写"未经帧证明的 arena 读不可达"——**错误，已改正**；
  后续任何报告不得复述该措辞。
- **已修 4 处**：① 快照陈旧计数加 `currentReadable && snapshotFrameTag != 0u` 门（该计数是决定
  是否放宽 kDelta 的唯一依据，不得混入"不可证明"）；② 槽位域判定改**溢出安全**形式
  （`requiredPaletteCount <= 0x3A98u && boundSlotIndex <= 0x3A98u - requiredPaletteCount`）；
  ③ 内核补 `matrixGroupSizes`/`matrixIndices` 两处空视图守卫（后者修复前严格组合实测
  `0xC0000005` 访问违例）；④ 快照上限 64 增加两侧单源一致性静态断言。
- **待办（P3，需新证据）**：A5 的**槽位所有权**缺口（FROZEN 携带 + 槽位被重分配给别的 part 时
  A0-A5 可全过）需实机 trace 或 per-slot owner 见证；A5 读非原子槽位缓存的线程亲和性待记录。

## D. 离线分析器 v2（外部工具，不进树）

`C:\Windows\Temp\warvk_p0\warvk_p0_analyze_v2.py`，95,280 B / 1,887 行 /
SHA-256 `2252F2CDC5B8240FA5AF96DEBF952E6FC99A9690C474C34B9BEAD352D0C70C75`，
`--self-test` **31/31 PASS**，**不写任何文件**，**永不输出"通过"**。核心改进：
自动区分"进程累计原子（Δ 有效）"与"当帧值（Δ 拒绝，改用每轮 max/非零占比）"；
判定数值锁定 `samples.jsonl` 的 627 键 runtime_status 全量快照；ON/OFF 同矩阵配对并核验
（只差 `DXVK_WAR3_SKIN_PALETTE_CONTRACT`）；每项判定附"缺什么证据才可判通过"。

**它纠正了一个事实错误**：P0 执行记录原写会话分辨率 2048×1152，与产物不符；
实测 **客户区 1902×963**（窗口外框 1920×1010、运行时 receiver viewport 1902×722、
截图 IHDR 1902×963 三方印证）。执行记录与本文已更正；对 2560×1440 与 2048×1152 **双向**不得外推。

其 ON/OFF 判定摘要（与主线程 v1 分析一致）：① 观察结果（部分）；② 未覆盖；③ 未覆盖（仅趋势）；
反例门 未覆盖。累计键 OFF 段末 30,961→87,582→91,034（Δ21=56,621 / Δ32=3,452）；
当帧值 OFF：GlobalSlot max=35（22.9% 非零）、DrawTimeCaptured max=4（5.7%）、Churn max=7；
ON 轮上述来源全 0；`ServedAfterConfirm` 与 `SourceNone` 两轮全 0。

## E. 三份新工单（详见 §6 与各自文档）

1. `2026-09-17-gapb-resource-key-fix-workorder.md`：先零风险 trace，再改一处谓词。
2. `2026-09-17-active-path-palette-instrumentation-workorder.md`：全量 187 处不建议，
   先做 **MVP-1（4 字段）**；draw-time 来源归属因 `War3DrawTimeVBEntry` 无 palette 字段
   必须由既有证据派生（标"待验证"）。
3. 清册 Q4(b) 口径回写（836 数据行未动、0 行被标记污染，已独立核验）。

## F. 收尾纪律

- 未 commit、未发布、未部署；三条候选一律标注"未实机"。
- 实机现场已恢复原 DLL（`74CC676B…DB73`，哈希核验一致），玩家目录全程未触碰。
- 需上级裁定的事项已提交 codex 线程 `01a02e0b-1d1e-7762-b40b-63a00bbb3449`
  （messageId `01a0acac-2752-7b02-bafc-b049180a911b`）。
