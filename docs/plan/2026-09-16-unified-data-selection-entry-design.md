# 2026-09-16 — 统一数据选择入口：现状清册与统一化设计（主线程产出）

> 目标项之一。用户裁定：统一数据选择入口、限制旧路径绕行、资源生命周期收口
> 是长期重构的核心；**不得用百分比概括**，按交付项判断。
> 本文是设计文档，**不代表任何代码已改动**。

## 1. 为什么必须统一（事实清册）

B 今天有**五个**调色板/蒙皮数据选择入口，证明强度与失败语义彼此不同：

| # | 入口 | 位置 | 输入 | 输出 | 证明强度 |
| --- | --- | --- | --- | --- | --- |
| 1 | `War3TryBuildLiveRuntimeGroupPalette` | `d3d9_device.cpp:8501` | resource, runtimeModel, renderablePart, frameSerial | palette + `War3SemanticPaletteSource` + slot + frameTag | **合同 ON 时最强**（`QueryOwnedRenderablePartPaletteSnapshot`，失败即 return）；合同 OFF 时退化为 slot/arena/pose 混合 |
| 2 | `TryBuildRuntimeGroupPalette` | `war3_shadow_renderer_core.cpp:6509` | ShadowModelResource/ShadowRenderable/ShadowPose/ShadowPoseStore | palette + maxVertexGroupSlot + MissDetail | 中（P0 Gap B 前会无条件信任记忆槽位） |
| 3 | `TryBuildRuntimeGroupPalette`（**同名不同签名**） | `war3_upper_layer_shadow.cpp:98` | ShadowGeosetResourceRecord + PoseRecord | palette + maxVertexGroupSlot | 弱/未知（独立实现，未见合同层） |
| 4 | producer query 家族 | `war3_model_hook.cpp:9010/9110/9166/9240/9285/9384` | part / slot | palette 或绑定 | 三个 slot 读取器严格度不同：`QueryBlendedPaletteBySlotIndex`、`…Exact`、`…BestEffort` |
| 5 | 合同层裁决 | `war3_skin_palette_selection.h` | `Selection` | `Usable` / `CanReplace` | **最强且唯一显式**（Source/Space/Domain/ownerEpoch/publicationTicket/hash） |

问题：
- 入口 1 只在**合同 ON** 时使用合同层；合同 OFF（普通/Release 构建的生产路径）走 legacy 混合，
  且其中"记忆槽位"曾可无条件供出（P0 正在修）。
- 入口 2/3 是**同名双实现**，各自演化，无法用一份合同约束。
- 入口 4 的三个 slot 读取器严格度不同，调用方需要知道该用哪个——这正是"旧路径绕行"的来源。
- 诊断 taxonomy（`semanticSceneSubmittedSkinnedPaletteSource*`）只覆盖入口 1 的路径。

## 2. 已有的统一原语（不必新造）

`war3_skin_palette_selection.h` 已定义：
- `enum class Source { Unknown, CapturedWriter, CapturedRawArena, OwnedPartSnapshot, … }`
- `enum class Space { Unknown, World, ModelLocal }`、`enum class Domain { Unknown, VertexGroups, Bones }`
- `struct Selection { runtimeModel, part, slot, frameTag, ownerEpoch, publicationTicket, captureSerial, hash, source, space, domain }`
- `ContractEnabled()`、`Usable(Selection, part, required, hash)`（拒绝未证明来源）、
  `CanReplace(old, next)`（单调发布规则）

**结论：统一化的目标形态不是发明新抽象，而是让所有入口产出并用同一个 `Selection` 裁决。**

## 3. 目标形态（提案，未实施）

单一入口（示意签名，最终以实施 PR 为准）：

```
bool War3SelectGroupPalette(const PaletteSelectInput& in,   // resource/renderable/pose/frameSerial/part/runtimeModel
                            PaletteSelectOutput& out);       // palette + maxVertexGroupSlot + Selection + 拒绝原因
```

裁决规则（全部 fail-closed）：
1. 合同 ON：只用 `QueryOwnedRenderablePartPaletteSnapshot`，`Usable()` 不通过即拒绝，**无 legacy 回退**（B 现状已如此）。
2. 合同 OFF：按证明强度降序尝试，且每一步都必须**携带可验证来源**：
   producer part snapshot → producer 绑定复核过的 slot（P0 Gap A/B 后的状态）→
   已发布 PoseRegistry（`Space::World` 且 `canReplace` 通过）→ CModel 回退（仅在调用方显式允许时）。
3. 记忆值（slot 缓存、last pose）**永远不能单独构成 World-space 权威**；只能作为"待复核候选"。
4. 拒绝必须可见：返回统一拒绝原因 + 计入来源 taxonomy（不得静默回退）。

诊断统一：所有入口写同一组来源/拒绝计数（现有 `semanticSceneSubmittedSkinnedPaletteSource*` +
`SourceChurnCount`，再补"经确认/陈旧被拒/快照接管"），JSON 键统一。

## 4. 分阶段迁移（每步独立可验收、可回退）

| 步 | 内容 | 验收 | 回退 |
| --- | --- | --- | --- |
| S1（进行中） | **P0 Gap A/B**：消除"未复核记忆槽位"读取 | 静态合同 + 实机三证明（错误矩阵不再用 / 合法对象有替代路径 / 可恢复） | 关闭对应 env 回到旧行为 |
| S2 | 让入口 2 与入口 3 共用同一实现（先统一签名，再合并逻辑），输出 `Selection` | 两处调用点行为等价（同场景像素/计数对照）；消除同名双实现 | 保留旧 3 号实现于诊断开关后 |
| S3 | 三个 slot 读取器收敛：明确 `Exact` 为生产、其余仅诊断；调用方不得再自选严格度 | 静态门禁禁止生产路径调用 BestEffort/裸版本 | 逐个文件回退 |
| S4 | 合同 OFF 的 legacy arena/slot 读取降级为**显式诊断开关（默认关）** | 默认构建无可达 legacy arena 读取；实机确认阴影不回退 | 开关打开即恢复现状 |

**顺序理由**：S1 先把最危险的"未证明即使用"堵住；S2/S3 消除重复实现（否则统一没有对象）；
S4 才是真正的"限制旧路径绕行"。**任何一步都不得让正常单位/建筑/桥梁/装饰物失去投影路径。**

## 5. 必须保持的不变量（用户此前明确）

- 对象身份（part/runtimeModel 同一性）、**当前帧**、**坐标空间**（World vs ModelLocal）、**来源**；
- map/device epoch 隔离；
- 证明失败 fail-closed（整份 candidate 不发布），但**合法对象必须有替代路径**；
- 不得用"拒绝计数增加"证明正确性；
- 不得把记忆值当作所有权。

## 6. 与当前工作的关系

- P0（Gap A/B）= S1，是眼前发布问题，优先级最高。
- P2 批次 4/5（registry domain 隔离、alias 迁移）属"资源生命周期收口"，与本文同向但不同层。
- P2 批次 6（Stage13 常驻几何）必须先过占用/回收验证，**不得**因本文目标而提前。

## 7. 明确不做

- 不在本文档阶段改任何代码；不新造与 `Selection` 平行的抽象；
- 不删除 legacy 路径的**诊断**可用性（只限制其生产可达性）；
- 不用"入口数量减少"当作完成度证明——必须同时证明画面行为未回退。
