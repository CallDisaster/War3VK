# 2026-09-17 — P0 对象级证据：事件—生产函数—局部字段—身份/帧域—缺失处理 对应表（冻结稿）

> 依据：上级（codex 01a02e0b）03:05 裁定第 2 节 Step 1 顺序：
> ① 先核实实际提交/绘制点及身份来源（已完成：`2026-09-17-p0-object-evidence-collection-points-verification.md`）；
> ② **冻结一张简短的「事件—生产函数—局部字段—身份/帧域—缺失处理」对应表**；
> ③ 再实施默认关闭的记录器/状态机/解析器；④ 离线测容量/淘汰/分配/开关成本后，才能冻结实机候选。
>
> 本文即第②步产物。**它只描述「打算在哪里记录什么」，不构成任何已实现或已证明的结论。**

## 0. 口径约定（先于表格）

- **不得**把「取到 palette」当成「阴影恢复」（裁定 3 / K2）。
- **不得**把区间重叠当成「错误矩阵被消费」（裁定 5）。
- `writeSerial` 只表示**插件侧槽位缓存的写入动作序号**，**不得**升级为 native arena 所有权或字节一致性证明（上级 03:05 明确）。
- 生命周期身份必须能说明「同一个对象实例」；**每帧变化的 publication ticket 不是稳定对象身份**（上级 03:05 明确）。
- 提交/绘制必须与**具体对象 + 消费帧**对应，不得用全局计数代替。
- 字段缺失时的处理只有三种合法写法：`identityWeak`（身份弱）/ `epochUnknown`（代际未知）/ `frameUnknown`（帧域未知），
  并**必须单列比例**；不得用 0 冒充「已知且为零」。

## 1. 事件类型与生产函数（全部为**拟新增**采集点，源码中当前不存在对象级事件）

| 事件 | 含义 | 生产函数与位置（锚点） | 该点当时可得的局部字段 | 身份域 | 帧域 | 缺失处理 |
| --- | --- | --- | --- | --- | --- | --- |
| **R**（拒绝） | A5 链拒绝，含具名原因 | `ShadowValidationRuntime`/`FindOrUpdatePaletteSlotCache`（`war3_shadow_renderer_core.cpp:794`）；R0..R3 具名原因在 `:886-899`，调用方 `:6722` 只见 `0xFFFFFFFF` | `renderablePart`（唯一身份）、`currentSlotIndex`、`requiredPaletteCount`；原因/允许槽位/绑定帧**需新增 out 参数** | **弱**：只有 `renderablePart`；无 `runtimeModelPtr`/`jHandle`/`rawcode`；无 `sessionGeneration`/`deviceEpoch` | **无**：函数内当前无任何帧号（`lastUpdateFrame` 实为 hit 计数 `:817/:910`） | `identityWeak` 必标；`epochUnknown` 必标；`frameUnknown` 必标 → 因此 R 事件**单独不足以**完成跨帧恢复验收 |
| **F**（被接住） | 具名替代来源供出 palette | ② `TryBuildRuntimeGroupPalette`（`war3_shadow_renderer_core.cpp:6619`，参数含 `resource`）与 ① 的命中分支 | `resource.mapEpoch`、`resource.immutableModelGeneration`、命中键/允许槽位 | 中：模型侧 `immutableModelGeneration + mapEpoch` 可得；**不是** part 级身份 | 部分：仅 `record.frameSerial`（且 `war3_shadow_runtime_contract.cpp:4253-4254` 会改写 manifest 帧号 ⇒ **不得冒充 manifestFrameSerial**） | 同上三项；`manifestFrameSerial` 记 `frameUnknown` |
| **S**（提交） | canonical packet 被 append 进本帧 caster 列表（**append 本身即提交事实**） | `D3D9DeviceEx::War3TryAppendSemanticShadowPacket` caster 段：`d3d9_device.cpp:24049` `draw={}`、`:24050/24051` mapEpoch/deviceEpoch、`:24052` `shadowRenderablePart`、`:24062` `inputSkinSelection`、`:24385/24386` rawcode/jHandle、`:24641` `shadowCasters.emplace_back` | **最全**：`renderablePart` + `runtimeModelPtr`(视 canonical 来源) + `rawcode` + `jHandle` + `inputSkinSelection` + `mapEpoch` + `deviceEpoch`(device 成员 `d3d9_device.h:1907`) | **强**（本表内最强），但仍需 `sessionGeneration` 才够跨图隔离 | `current`（`:24056`）与 `native frameTag`（`:24062`）可得；`manifest.frameSerial` 该点**不可得** | `manifestFrameSerial` 记 `frameUnknown`（或后续透传）；`:24224` 存在清空 `inputSkinSelection` 的 native-snapshot 分支 ⇒ **取 Selection 前必须判空** |
| **C**（剔除） | 该 caster 被具名原因剔除 | `War3ShadowReceiverPass::renderShadowMap`（`d3d9_war3_shadow.cpp:3713`）→ `evaluateBoundsPolicy` `:4314/:4460`；写 mask `:4530`；`cascadeVisible` `:4532`；剔除分支 `:4795` | `draw.renderablePart(:401)`、`rawcode(:396)`、`jHandle(:397)`、`inputSkinSelection(:407)`、`mapEpoch/deviceEpoch(:262-263)`、`boundsProvenance(:437)`、`boundsIdentityProven(:441)`、**具名 rejectReason** | 强（同 S 的字段集；来自 draw 列表） | 同上：`manifest.frameSerial` 不可得 | 默认对象级 cull **只 Observe**（`:4305-4307`）⇒ **不得**把 would-cull 写成实际剔除；`:4556` 属 Observation-only 段 |
| **D**（绘制） | 该 caster 真的发出 draw | 同文件真实 draw 点：`:5190` `cmdDrawIndexed` / `:5194` `cmdDraw` / `:5222` `drawnCasters++`（自检另发现 `:5497/:5500`、`:8579/:8583` 站点） | 同 C 的字段集 | 强 | 同上 | 需按 caster 身份关联到具体 draw 调用点；**现有导出只有无对象关联的聚合**（`d3d9_war3_scene.h:1705-1708`） |
| **T**（终态） | `Recovered` / `WindowExpired` / `ObjectGone` / `TableFull` / `EventLost` / `Unclosed` | 记录器状态机（拟新增，宿主可测） | 对象键 + 终态原因 + 该对象的阶段计数 | 沿用该对象键 | 沿用记录中的帧域 | 六类终态**不得互相冒充**；证据不足 ⇒ `Unclosed` |

## 2. 五层证明各自到哪一步（与工单 §5.6 对齐）

| 层 | 本表能否证明 | 说明 |
| --- | --- | --- |
| ① 缓存标签 | 可记录（R/F 的 frameTag 与 slot 区间） | 标签相等**不等于**所有者正确 |
| ② 实际读取的 arena 字节 | **不能**（R 只读 slot 缓存；F 读 native arena 但无字节归属证明） | 需另行设计，不属本轮 |
| ③ 对象所有权 | **不能**：`skin::Selection::slotAllocationGeneration` 恒 0（`war3_skin_palette_selection.h:26`） | 路线 A 未获批准（裁定 5） |
| ④ 提交 | **可**：S 事件与对象键对应 | 但 `manifest.frameSerial` 缺失 ⇒ 跨帧对齐需另证 |
| ⑤ 绘制 | **可**：D 事件与对象键对应 | 同上；且默认 cull 只 Observe |

## 3. 必须先补的字段（实施门槛，逐条对应上级裁定）

1. **R 的具名原因 out 参数**（`FindOrUpdatePaletteSlotCache`）：R0..R3 枚举 + `boundSlotIndex` / `boundGroupCount` / `boundFrameTag` /
   `slotRangeMin/Max/Missing` / `currentPaletteFrameTag`；不改判定、不放松准入。
2. **`manifest.frameSerial` 透传**：至少让 R/F 的采集点能记录「本帧对应的 manifest 帧号」，并**与 `current`/`native frameTag` 分列**。
3. **`deviceEpoch` 策略**：shadow-core 内不可得 ⇒ 记 0 + `epochUnknown` 并**单列比例**（既有先例 `d3d9_device.cpp:23158`）。
4. **`sessionGeneration`**：跨图隔离必需；若既有契约无该字段，需新增（不得复用 publication ticket）。
5. **S 的载荷**：写入 `skin::Selection`（判空后）与 device epoch；不新增结构体/不新增哈希（裁定 4）。

## 4. 预算与缺链（按上级 03:05 修正）

- **总计上限 4096 条/会话**（R + F + S + C + D + T 全部计数在内）；
  **512 条终态从该总额中预留，不额外增加**。
- 配额**不能**保证共享环里的旧证据不被淘汰 ⇒ 记录器必须输出**丢失/淘汰/饱和计数**，
  分析器必须能判定「链缺失」并把该对象判为 `Unclosed`（不得当成成功或确认故障）。
- 关闭 / 开启成本必须**离线实测**（含关闭时零开销的可测口径）。

## 5. 明确不做

- 不放宽 `IsContractUnitCandidate` / `IsVisibleDirectGeosetUnitCandidate`；
- 不新增矩阵扫描哈希（裁定 4）；不新增 JAPI（裁定 7）；
- 不改 `AutoTest/test_recorder_event_wire_golden.py` 的 wire 布局（裁定 2）；
- 不把 `writeSerial` 升级为所有权/字节一致性证明；
- **不在 core 一条路径上装完仪表就跑实机**：必须先证明采集点覆盖目标活跃路径（当前 core 路径已知 `resolved=0`）。

---

## 6. 2026-09-17 03:58 上级就地修正（必须遵守，覆盖上文相应表述）

1. **S 是 CPU caster 候选入队，不是 GPU 提交完成**；**D 是绘制命令已记录，不是 GPU 执行或像素正确性证明**。
   三者分列：`shadowCasters.emplace_back`（进入 CPU 候选列表）/ `cmdDraw|Indexed`（绘制命令已记录）/ GPU 完成与画面（需另行证据）。
2. **字段更多不等于强身份**：S/C/D 在没有可靠**对象实例生命周期证明**时，**同样**必须保留弱身份限制
   （即 §1 表中 S/C/D 的「身份域=强」须读作「字段最全，但仍受弱身份约束」）。
3. **必须取最终来源**：native override 清空 `inputSkinSelection` 后，应**记录空值及其原因**，
   **不得补回**此前尝试的 Selection；移动前保存必要 POD、入队成功后发事件，**不读 move 后对象**。
4. `sessionGeneration` 可复用既有采集会话/地图隔离机制；**不允许**借此新造「原生对象代际」并赋予权威。
5. 总预算仍为 **4096 条、其中终态最多 512 条**；未知身份/帧域/淘汰/链缺失必须保持可见。
