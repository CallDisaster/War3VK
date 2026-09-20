# 2026-09-17 — S2 收窄版：共享纯计算内核设计（只共享计算，不合并来源选择链）

> **状态（2026-09-17 08:1x 更新）**：已实施并入库（内核 `war3_runtime_group_palette_kernel.h`
> 318 行（含 2 处空视图守卫）+ 测试 **1225+ 行 T1-T17** + core/upper 薄适配层 + meson 目标）。
> 门禁：ninja no-work、meson **73/73**、静态 **243/243**（含新增单一来源门禁）、内核 **T1-T17 全过**。
> 等价性证据口径更正：抽取后内核改用视图结构体，**逐行 52/52 已不适用**；有效证据是
> 差分模糊（上一轮：4 步 30 万随机输入 0 mismatch、core 风格 5 步 30 万输入 0 mismatch，
> 但该 harness **未入库、当前不可复现**，已列为待补测试）。
> **未部署、未实机**；画面/性能未验证。DLL 35,995,922 / `BFA4C463…`。
> §5.1 的"实机结算前不落地"前置**已完成**（实机事务 03:2x 结算并恢复现场）。
> 一处与 §3.3 伪代码的偏离：适配层保留原有 maxSlot 内联扫描与来源链之后的去重循环
> （内核内部自行重算 scan），以保持 engine-direct 快路径的惰性时序与 `logFailure` 字面不变。

> 上级 Q5 裁定（2026-09-17 凌晨，codex 线程 01a02e0b，记录于
> `docs/plan/2026-09-16-merge-execution-ledger.md` 第 296-331 行）：
> **条件批准收窄版 S2 —— 只共享纯计算内核 + 薄适配 + 生产函数测试，不合并来源选择链；
> 实机事务期间暂停 B 树源码/构建修改。**
>
> 本文是**纯设计文档**：不改任何 `src/`、不构建、不跑 git、不部署。
> 文内所有行号/签名/调用关系均在 2026-09-17 01:30（+08:00）前后逐条对照 B 树源码核实，
> 基线文件 SHA-256 见第 6.2 节；**行号会随改动漂移，故每处事实同时给出函数名**。

---

## 1. 事实核对：两个同名实现的完整现状

### 1.1 位置、可见性与签名

| 项 | 入口 2（shadow-core 版） | 入口 3（upper-layer 版） |
| --- | --- | --- |
| 文件 | `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp` | `src/d3d9/war3/render/war3_upper_layer_shadow.cpp` |
| 定义行 | **6509-7072**（函数 `TryBuildRuntimeGroupPalette`） | **98-242**（同名函数） |
| 命名空间 | `dxvk::war3::shadow` 内匿名命名空间（1689-7090）→ **TU 内部符号** | `dxvk::war3::render` 内匿名命名空间（12-244）→ **TU 内部符号** |
| 头文件声明 | 无 | 无（两文件均未在任何头文件声明） |
| 签名 | `bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord&, const ShadowRenderableRecord&, const ShadowPoseRecord&, const ShadowPoseStore&, std::vector<Matrix4>& outPalette, uint32_t& outMaxVertexGroupSlot, bool& outUsesAveraging, RuntimeGroupPaletteMissDetail* = nullptr)` | `bool TryBuildRuntimeGroupPalette(const model::ShadowGeosetResourceRecord&, const model::PoseRecord&, std::vector<Matrix4>& outPalette, uint32_t& outMaxVertexGroupSlot, bool& outUsesAveraging)` |
| 参数个数 | 8（含可选诊断出参） | 5 |
| 输出 | palette + maxSlot + averaging + **miss 明细** | palette + maxSlot + averaging |

两者**不是重载**（参数个数/类型均不同），是两条独立演化的链。`HashMatrixPalette`、
`RuntimeVertexGroupSlotCount`、`RuntimeGroupPaletteMissDetail` 等辅助符号也都各自定义在
各自 TU 的匿名命名空间内，因此今天不存在任何跨这两处的共享符号。

### 1.2 输入记录类型差异（决定了内核不能用记录类型当参数）

| 语义 | shadow-core 版输入 `ShadowModelResourceRecord` | upper-layer 版输入 `model::ShadowGeosetResourceRecord` |
| --- | --- | --- |
| 定义 | `war3_shadow_runtime_contract.h:78-108` | `model/war3_model_resource_cache.h:37-115` |
| 顶点 group 槽位 | `std::vector<uint8_t> vertexGroupIndices`（:88） | `std::vector<uint8_t> vertexGroupIndices`（:55） |
| 槽位计数来源 | **无独立字段**，由 `RuntimeVertexGroupSlotCount()` 派生（见 D4） | `uint32_t vertexGroupCount`（:54） |
| 组大小表 | `std::vector<uint32_t> matrixGroupSizes`（:90） | `std::vector<uint32_t> matrixGroupSizes`（:74） |
| 组计数来源 | **无独立字段**，取 `matrixGroupSizes.size()` | `uint32_t matrixGroupCount`（:73） |
| 矩阵索引表 | `std::vector<uint32_t> matrixIndices`（:91） | `std::vector<uint32_t> matrixIndices`（:78） |
| `hasSkinningData()` | :104-107，函数体 `return !vertexGroupIndices.empty() && (!matrixGroupSizes.empty() || !matrixIndices.empty());` | :111-114，**函数体逐字符相同** |

姿态输入：`ShadowPoseRecord`（`war3_shadow_runtime_contract.h:178-188`）与
`model::PoseRecord`（`model/war3_model_registry.h:162-188`）是两个不同的结构，但计算只用到
同名的 `uint32_t matrixCount` 与 `std::vector<Matrix4> matrixPalette`（同类型、同名字）。
**这使"按视图结构而不是按记录类型传参"成为自然选择**（见 3.2）。

### 1.3 调用点完整清册（逐点核对）

**入口 2（shadow-core 版）：共 6 处语句，全部位于
`ShadowRendererCore::resolveRecord`（定义于 7427）这一个函数体内。**

| # | 行号 | 角色 | 传入的 renderable/pose | 失败后行为 |
| --- | --- | --- | --- | --- |
| 1 | **8276-8279** | 主调用 | `resolvedRenderable` + `pose` | 进入救援链 |
| 2 | **8296-8299** | 救援：mesh pose context | 改 `runtimeModelPtr` 的 renderable 副本 + `meshPose` | 继续下一救援 |
| 3 | **8338-8342** | 救援：resource-matched pose（非 semantic 路径） | 副本 + 扫描到的 `candidatePose`（上限 `kMaxResourcePoseScan=48`，8312） | `continue` |
| 4 | **8389-8394** | 救援：runtime-model root pose | 副本 + `candidatePose` | `continue` |
| 5 | **8473-8477** | 救援：descendant runtime pose | 副本 + `candidatePose` | `continue` |
| 6 | **8519-8522** | child-runtime pose 提交 | `resolvedRenderable`（已改 runtimeModelPtr）+ `childPose` | 两条分支都只做 `pose = std::move(childPose)` |

6 处的 `outPalette/outMaxVertexGroupSlot/outUsesAveraging/paletteMissDetail` 都是**同一组局部变量**，
调用前显式 `clear()/0/false` 重置；`paletteMissDetail`（声明于 8215）被后续
`logRuntimeGroupPaletteSkip`（8234-8260）消费。

**入口 3（upper-layer 版）：共 1 处，位于
`UpperLayerShadowRegistry::resolve`（定义于 261）内的 372-380 行。**

```
372:   if (out.skinned && out.hasPosePalette) {
373:     out.hasRuntimeGroupPalette = TryBuildRuntimeGroupPalette(
374:         out.geoset, out.pose, out.runtimeGroupPalette,
375:         out.maxVertexGroupSlot, out.matrixGroupsUseAveraging);
376:     if (!out.hasRuntimeGroupPalette) { ... resolveRuntimeGroupPaletteMiss++ ... }
```

上层消费者（S2 **不触碰**，仅用于说明"不得改变失败语义"）：
- `d3d9_device.cpp:47771 tryCaptureUpperLayerShadow` 调 `UpperLayerShadowRegistry::instance().resolve(semantic, upperItem)`（47777），
  随后用 `HasAuthoritativeRigidPath()/HasAuthoritativeSkinnedPath()`（`war3_upper_layer_shadow.h:25-33`）判定；
- `war3_shadow_renderer_core.cpp:9564-9584` 用 `snapshotResolvedItems()` + `TryConvertUpperLayerResolvedItem`（6475）把上层结果转成 `ShadowDrawPacket`。

**当前可达性（重要，但不构成删除理由）**：
`war3_internal_test_config.h:770` `kUpperLayerShadowConsumerEnabled = false`；
`war3_upper_layer_shadow.cpp:267-268` 在 `resolve` 入口即早退；
`war3_shadow_renderer_core.cpp:9558` 是 `if constexpr (... )`，整块编译期消除；
`d3d9_device.cpp:47772` 亦被同一常量短路。⇒ **入口 3 今天编译但生产不可达**。
按 Q5 与用户纪律，这**不授权删除**：它仍是观察链与未来切换目标，且删除会改变 stats/JSON 面。

### 1.4 两实现的结构对照（区域级）

| 区域 | shadow-core 版 | upper-layer 版 |
| --- | --- | --- |
| 输出重置 | 6518-6522（含 `*outMissDetail = {}`） | 103-105 |
| 前置校验 | 6524-6542（三处 `NoteRuntimeGroupPaletteMiss`） | 107-114（三处 `return false`） |
| group 槽位计数 | 6536 `RuntimeVertexGroupSlotCount(resource)`（6412-6425） | 111-112 `min(geoset.vertexGroupCount, vertexGroupIndices.size())` |
| 最大槽位扫描 | 6544-6548 | 116-118 |
| **来源选择链** | **6552-6619 `tryEngineDirectPosePalette`** | **不存在** |
| 唯一槽位去重 | 6640-6649（位于来源链**之后**，可被成功路径跳过） | 120-129（紧接最大槽位扫描） |
| 失败诊断 lambda | 6651-6914 `logFailure`（含 root/l1c head 探针） | 不存在 |
| 4 个重映射 lambda | 6916-6974 | 131-189 |
| 第 5 个 lambda | **6976-6992 `buildUniformPosePalette`** | **不存在** |
| fallback 组合 | 6994-7006 `tryFallbacks`（5 步 + FallbacksFailed 记录 + logFailure） | 无独立函数；内联 4 步表达式 |
| 分组平均主路径 | 7008-7071 | 191-241 |
| 收尾校验 | 7060-7069，上界 `vertexGroupSlotCount` | 234-239，上界 `vertexGroupCount` |

---

## 2. 同 / 异片段清单

### 2.1 真正逐语句相同 / 等价的纯计算片段

**核对方法（可复现）**：对 shadow-core 6916-6992 与 upper-layer 131-189 取正文，
去掉空白与注释，把记录名统一（`resource.` / `geoset.` → 同一占位符），再做**保序逐行贪心匹配**：

- upper-layer 侧 **52 行非空行全部匹配**；
- shadow-core 侧 67 行非空行中 **52 行匹配**，未匹配的 15 行**恰好就是 `buildUniformPosePalette` 那一个 lambda**（6976-6992）。

即：**除记录名与那一个 core 独有 lambda 外，四段重映射 lambda 逐语句相同。**

| 片段 | shadow-core 行 | upper-layer 行 | 等价性 |
| --- | --- | --- | --- |
| `buildDirectMatrixRemap` | 6916-6932 | 131-147 | **逐语句相同**（仅 `resource.matrixIndices` ↔ `geoset.matrixIndices`） |
| `buildSparseMatrixRemap` | 6934-6950 | 149-165 | **逐语句相同**（同上） |
| `buildDirectPosePalette` | 6952-6962 | 167-177 | **逐语句相同** |
| `buildSparsePosePalette` | 6964-6974 | 179-189 | **逐语句相同** |
| 分组平均主路径 | 7013-7058 | 197-232 | **逐语句相同**（前缀和；`groupSize == 0` 与 `groupBase + groupSize` 越界；`matrixIndex >= pose.matrixCount \|\| matrixIndex >= pose.matrixPalette.size()`；`Matrix4 accum(0.0f)` 累加；以及 `if (groupSize > 1u) outUsesAveraging = true; outPalette[group] = groupSize == 1u ? accum : (accum / float(groupSize));`） |
| `running > matrixIndices.size()` 总量校验 | 7019-7025 | 204-206 | 条件表达式相同（core 额外记 `InvalidGroupTable`） |
| 收尾 group 槽位范围校验 | 7060-7069 | 234-239 | 条件语义相同（`slot >= groupCount`），**迭代上界不同**（见 D4） |
| 最大 group 槽位扫描 | 6544-6548 | 116-118 | 循环体相同（`(std::max)(outMaxVertexGroupSlot, uint32_t(groupSlot))`），**上界不同**（见 D4） |
| 唯一槽位去重（`std::array<bool,256> seenGroupSlots` + `reserve(maxSlot+1)`） | 6640-6649 | 120-129 | 循环体相同；仅循环变量类型 `size_t` vs `uint32_t`；**执行时机不同**（core 在来源链之后，upper 在其之前），但去重结果与后续用法相同 |
| 输出初值 | 6518-6520 | 103-105 | 相同（`clear()` / `0u` / `false`） |
| 前置量校验 | 6524-6535 | 107-110 | 相同（`hasSkinningData()`；`matrixPalette.empty() \|\| matrixCount == 0`） |
| averaging 语义 | 7054-7057 | 228-231 | 相同：**只有 `groupSize > 1` 才置位**；`groupSize == 1` 时直接取该矩阵（不做除法） |
| 输出尺寸语义 | 7027 / 7008 | 208 / 191 | 平均路径 `palette.size() == groupCount`；fallback 路径为 `maxSlot+1`（sparse 两条路径对未写槽位填 `Matrix4(0.0f)`） |

**结论：§2.1 的 13 个片段构成一个完整的、行为可逐位验证的纯计算内核；其中 4 个 lambda 是
"逐语句相同"级别的重复，不是"逻辑相似"。**

### 2.2 本质不同的片段（S2 明确不合并）

| ID | 差异 | 位置 | 为什么不能合并 |
| --- | --- | --- | --- |
| **D1** | **来源选择链（core 独有）** | 6552-6619 `tryEngineDirectPosePalette`：① producer 快照 `QueryRenderablePartPaletteSnapshot(renderable.renderablePart, requiredCount, &outPalette, nullptr, nullptr)`（6561-6564；声明 `war3_model_hook.h:507`，实现 `war3_model_hook.cpp:9285`）；② `SafeReadU32Fast(renderablePart, RenderablePartFieldOffsets::StagePresetSpanBaseIndex)`（6576-6581）；③ `FindOrUpdatePaletteSlotCache`（6584-6585）+ `0xFFFFFFFF` / `>= 0x3A98` 门（6587）；④ `GetModuleHandleA("Game.dll")` → `kGlobalPaletteBufferRva` → 解引用 → `IsReadableRange` → `DecodeRuntimePoseMatrix48`（6590-6615）。upper-layer 版**无任何内存读取**（已核实该文件不含 `SafeRead`/`GetModuleHandle`/`IsReadableRange`/`DecodeRuntimePoseMatrix48` 调用） | 这是**生产来源选择与失败语义本身**；Q5 明令不合并。它决定"未知来源能否被当作已验证数据"，正是 S1/S3/S4 的对象 |
| **D2** | **第 5 条 fallback `buildUniformPosePalette`（core 独有）** | 6976-6992：`paletteCount = max(maxSlot+1, matrixGroupSizes.size())`，`assign(paletteCount, matrixPalette.front())`（广播根矩阵） | **行为差异**：在 `VertexGroupOutOfRange` / `InvalidGroupTable` 等失败路径上，core 仍可能靠广播根矩阵成功出图，upper-layer 直接返回 false。合并会让上层多出（或失去）投影 → 违反"合法对象必须有替代路径" |
| **D3** | **失败诊断面（core 独有）** | `RuntimeGroupPaletteMissReason`（2677-2686）、`RuntimeGroupPaletteMissDetail`（2688-2696）、`NoteRuntimeGroupPaletteMiss`（2698-2715）、`AccumulateRuntimeGroupPaletteMissStats`（2717+）、`logFailure`（6651-6914，含 CModel/mesh/l1c head 探针与 32/2048 限频） | upper-layer 只有 `resolveRuntimeGroupPaletteMiss` 计数。合并必然改变 stats/日志面与 JSON。另：core 的 `poses`（`ShadowPoseStore&`）参数**只被 `logFailure` 使用**（6724-6726、6780-6781），**不参与计算** |
| **D4** | **槽位计数派生不同** | core：`RuntimeVertexGroupSlotCount(resource)`（6412-6425）= `min(vertexGroupIndices.size(), vertexCount != 0 ? vertexCount : positions.size()/3, 128*1024)`；upper：`min(geoset.vertexGroupCount, geoset.vertexGroupIndices.size())`（111-112） | **上界不同**：core 额外按 `vertexCount` 截断并硬顶 128Ki，upper 只看 `vertexGroupCount`。统一任何一个都会改变"哪些槽位被扫描到"，进而改变 maxSlot / uniqueSlots / 越界判定 |
| **D5** | **groupCount 派生不同** | core：`uint32_t groupCount = uint32_t(resource.matrixGroupSizes.size())`（7008）；upper：`min<uint32_t>(geoset.matrixGroupCount, uint32_t(geoset.matrixGroupSizes.size()))`（191-192） | 当 `matrixGroupCount` 与表长不一致时（缓存未补齐/旧记录）两者行为不同；统一成 min 会让 core 少读组 → 改变平均结果 |
| **D6** | **来源侧计数上限门（core 独有）** | 6553-6555 `requiredCount = maxSlot + 1; if (requiredCount == 0u \|\| requiredCount > 256u) return false;` | 属 D1 路径；upper 无此约束 |
| **D7** | 输入记录类型 / 坐标系 / 有效期上下文 | `ShadowModelResourceRecord` vs `model::ShadowGeosetResourceRecord`；两者均由各自调用方在同一帧内解析（core：`resolveRecord` 从 `ShadowFrameManifest`；upper：`resolve` 从 `VisibleRenderableRegistry` / `ShadowModelResourceCache` / `PoseRegistry`） | 纯计算不涉及 space 变换；但"输入从哪来、多新鲜"的差异必须留在适配层 |
| **D8** | 参数集合差异 | core 传 `ShadowRenderableRecord`（用于 D1）与 `ShadowPoseStore`（用于 D3） | 内核不得引入任何指针来源参数 |

### 2.3 任务清单里候选的逐条核对结论（含一处纠正）

- **矩阵重映射**：✔ §2.1，4 段**逐语句相同**（可整段抽出）。
- **group size 校验**：✔ §2.1，`groupSize == 0u`、`(groupBase + groupSize) > matrixIndices.size()`、
  `running > matrixIndices.size()` 三处条件表达式相同。
- **数量校验**：◐ **部分相同**。相同的是 `matrixIndex >= pose.matrixCount`、
  `matrixIndex >= pose.matrixPalette.size()`、`matrixIndices` 容量门、收尾 `slot >= groupCount`；
  **不相同的是两个计数派生（D4/D5）与 `requiredCount > 256u`（D6）**，这些必须由参数量化。
- **hash 计算**：✘ **纠正：两个 `TryBuildRuntimeGroupPalette` 函数体内都没有任何 hash 计算。**
  实际存在的 hash 是 TU 级自由函数：
  - `war3_shadow_renderer_core.cpp:835-844` `HashMatrixPalette(const std::vector<Matrix4>&)`（逐字节 FNV，初值 `1469598103934665603ull`，乘子 `1099511628211ull`）；
  - `war3/model/war3_model_hook.cpp:927-936` **同名同体**的第二份；
  - `d3d9_device.cpp:8398-8415` `War3SemanticHashMatrixPalette` 是**另一套方案**（`bit::fnv1a_iter` 逐 word + 计入 `matrixCount`）。
  ⇒ 它们是真实重复，但**不在 S2 两函数体内**，本轮**不纳入**（见 §5）。
  调用方在转换期自行算 hash（如 6502-6505、8053、8588、8620），属调用点行为，S2 不动。

---

## 3. 共享纯计算内核设计

### 3.1 放置位置建议

**首选：新增 `src/d3d9/war3/render/war3_runtime_group_palette_kernel.h`（header-only）。**

理由（均为既有先例，不是新发明）：
1. `war3/render/` 已经是"纯内核 + 独立可测目标"的家：
   `war3_current_draw_group_slot_summary.h`（85 行，header-only，`dxvk::war3::render` + `noexcept` 纯函数）、
   `war3_immutable_group_slot_binding.h`、`war3_shadow_palette_storage.h`。
   meson 里 `war3_current_draw_group_slot_summary_test`（`src/d3d9/meson.build:485-497`）与
   `war3_immutable_group_slot_binding_test`（:518-530）**只编译 test.cpp**，证明 header-only 纯内核
   可直接被测试目标消费。
2. 两侧调用者分属 `shadow/` 与 `render/`，放进任一侧都会制造"谁是主实现"的错觉；`render/`
   是上层语义数据的既有归属地（`war3_upper_layer_shadow.cpp` 就在此）。
3. 依赖面最小：只需 `<algorithm>`、`<array>`、`<cstddef>`、`<cstdint>`、`<vector>` 与
   `dxvk::Matrix4`（`src/util/util_matrix.h:7`）。**不得** include 任一记录类型头、
   不得 include `war3_model_hook.h` / d3d9 设备头 / 任何 `SafeRead*`。

**备选（若希望有独立符号与编译单元）**：`war3_runtime_group_palette_kernel.h` + `.cpp`，
并把 `.cpp` 同时加入 `src/d3d9/meson.build` 的 `d3d9_src`（如 :127 的
`'war3/render/war3_point_shadow_cpu_plan.cpp'`）与新测试目标（如 :1103-1115 的
`war3_point_shadow_cpu_plan_test`）。**本文推荐 header-only**：内核无状态、无平台依赖，
可避免新增 DLL 符号面。

### 3.2 函数签名草案

```cpp
// src/d3d9/war3/render/war3_runtime_group_palette_kernel.h
#pragma once
#include "../../../util/util_matrix.h"      // dxvk::Matrix4
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk::war3::render {

// 只读视图：由各自的薄适配层从自己的记录类型填充，内核不认识任何一种记录。
// 所有权/有效期仍属调用方；内核不保存、不跨帧、不做任何内存读取。
struct RuntimeGroupPaletteInput {
  const uint8_t*  vertexGroupIndices   = nullptr;  // 槽位数组
  size_t          vertexGroupSlotCount = 0u;       // 由适配层决定（core: 128Ki/vertexCount clamp；upper: min(vertexGroupCount,size)）
  const uint32_t* matrixGroupSizes     = nullptr;
  uint32_t        groupCount           = 0u;       // 由适配层决定（core: size()；upper: min(matrixGroupCount,size)）
  const uint32_t* matrixIndices        = nullptr;
  size_t          matrixIndexCount     = 0u;
  const Matrix4*  posePalette          = nullptr;  // pose.matrixPalette.data()
  size_t          posePaletteSize      = 0u;       // pose.matrixPalette.size()
  uint32_t        poseMatrixCount      = 0u;       // pose.matrixCount
};

// 与 core 的 RuntimeGroupPaletteMissReason 取值一一对应，但独立定义，
// 避免内核依赖 shadow/ 的 TU 内部类型。
enum class RuntimeGroupPaletteKernelMiss : uint32_t {
  None = 0, NoSkinningData, NoPosePalette, NoVertexGroups,
  InvalidGroupTable, MatrixIndexOutOfRange, VertexGroupOutOfRange, FallbacksFailed,
};

struct RuntimeGroupPaletteKernelDetail {
  RuntimeGroupPaletteKernelMiss reason = RuntimeGroupPaletteKernelMiss::None;
  uint32_t group = UINT32_MAX;
  uint32_t matrixIndex = UINT32_MAX;
  uint32_t poseCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t maxVertexGroupSlot = 0u;
  uint32_t matrixIndexCount = 0u;
};

// 行为差异必须用参数表达，不得用"取 min"抹平：
enum class RuntimeGroupPaletteFallbackSet : uint8_t {
  MatrixAndPoseRemap       = 0,  // 4 步：direct / sparse / directPose / sparsePose（upper-layer 现状）
  MatrixPoseAndUniformRoot = 1,  // 5 步：+ 广播根矩阵                       （shadow-core 现状）
};

struct RuntimeGroupPaletteOutput {
  std::vector<Matrix4> palette;
  uint32_t maxVertexGroupSlot = 0u;
  bool     usesAveraging = false;
};

// 片段 1（可分离）：等价于两侧相同的"最大槽位扫描 + 唯一槽位去重"。
// 单独暴露是为了让 core 保持"去重发生在来源链之后"的惰性时序。
struct RuntimeGroupPaletteSlotScan {
  uint32_t maxVertexGroupSlot = 0u;
  std::vector<uint32_t> uniqueGroupSlots;   // 出现顺序、已去重
};
RuntimeGroupPaletteSlotScan ScanRuntimeGroupPaletteSlots(
    const uint8_t* vertexGroupIndices, size_t vertexGroupSlotCount);

// 片段 2：前置校验 + （4/5 步）fallback + 分组等权平均。
// 前置校验与 miss 语义严格按 §2.1 的行结构实现，不引入任何新的 clamp。
bool TryBuildRuntimeGroupPaletteKernel(
    const RuntimeGroupPaletteInput& in,
    RuntimeGroupPaletteFallbackSet fallbackSet,
    RuntimeGroupPaletteOutput& out,
    RuntimeGroupPaletteKernelDetail* outDetail = nullptr);

} // namespace dxvk::war3::render
```

**内核实现必须遵守的三条硬规则：**
1. 复刻 §2.1 的行结构（含 `Matrix4 accum(0.0f)` 起点、`groupSize == 1u ? accum : accum / float(groupSize)`、
   `outUsesAveraging` 只在 `groupSize > 1u` 置位、fallback 表达式**短路顺序**不变）；
2. 所有计数上界来自 `RuntimeGroupPaletteInput`，**内核内部不做任何 `min()` 收窄**；
3. 任何"来源"概念（renderablePart、slot index、arena、producer、Selection）都不得进入内核类型。

### 3.3 两个调用点的薄适配层伪代码

**（A）shadow-core 版（`war3_shadow_renderer_core.cpp`，现 6509-7072）**

```cpp
bool TryBuildRuntimeGroupPalette(const ShadowModelResourceRecord& resource,
                                 const ShadowRenderableRecord& renderable,
                                 const ShadowPoseRecord& pose,
                                 const ShadowPoseStore& poses,          // 仅供 logFailure
                                 std::vector<Matrix4>& outPalette,
                                 uint32_t& outMaxVertexGroupSlot,
                                 bool& outUsesAveraging,
                                 RuntimeGroupPaletteMissDetail* outMissDetail) {
  // 步骤 1：输出重置（保持 6518-6522 逐行不变）
  outPalette.clear(); outMaxVertexGroupSlot = 0u; outUsesAveraging = false;
  if (outMissDetail) *outMissDetail = {};

  // 步骤 2：前置校验（保持 6524-6542 的顺序与 reason）
  if (!resource.hasSkinningData())  return Note(NoSkinningData, ...);
  if (pose.matrixPalette.empty() || pose.matrixCount == 0u) return Note(NoPosePalette, ...);
  const size_t slotCount = RuntimeVertexGroupSlotCount(resource);   // 保持 128Ki/vertexCount clamp
  if (slotCount == 0u) return Note(NoVertexGroups, ...);

  // 步骤 3：最大槽位扫描（等价片段，用内核）
  const auto scan = render::ScanRuntimeGroupPaletteSlots(
      resource.vertexGroupIndices.data(), slotCount);
  outMaxVertexGroupSlot = scan.maxVertexGroupSlot;

  // 步骤 4：来源选择链 —— 原样保留，绝不进内核
  if (tryEngineDirectPosePalette()) {                 // 6552-6619 一行不改
    Note(None, ...);                                  // 6623-6628
    return true;
  }
  Note(None, ...);                                    // 6631-6635
  if (!resource.hasSkinningData()) return false;      // 6637-6638（保持）

  // 步骤 5：纯计算交内核，fallbackSet 固定 5 步
  render::RuntimeGroupPaletteInput in = {};
  in.vertexGroupIndices   = resource.vertexGroupIndices.data();
  in.vertexGroupSlotCount = slotCount;
  in.matrixGroupSizes     = resource.matrixGroupSizes.data();
  in.groupCount           = uint32_t(resource.matrixGroupSizes.size());   // 保持 core 派生
  in.matrixIndices        = resource.matrixIndices.data();
  in.matrixIndexCount     = resource.matrixIndices.size();
  in.posePalette          = pose.matrixPalette.data();
  in.posePaletteSize      = pose.matrixPalette.size();
  in.poseMatrixCount      = pose.matrixCount;

  render::RuntimeGroupPaletteOutput kernelOut = {};
  render::RuntimeGroupPaletteKernelDetail detail = {};
  if (!render::TryBuildRuntimeGroupPaletteKernel(
          in, render::RuntimeGroupPaletteFallbackSet::MatrixPoseAndUniformRoot,
          kernelOut, &detail)) {
    MapKernelDetailToMissDetail(detail, pose, resource,
                                outMaxVertexGroupSlot, outMissDetail);
    logFailure();                                     // 6651-6914 一行不改
    return false;
  }
  outPalette            = std::move(kernelOut.palette);
  outMaxVertexGroupSlot = kernelOut.maxVertexGroupSlot;
  outUsesAveraging      = kernelOut.usesAveraging;
  return true;
}
```

**保持不变的关键点**：6 处调用点（8276/8296/8338/8389/8473/8519）源码**无需修改**——
适配层保留原签名、原输出出参、原 miss 明细语义；调用点重置与救援分支结构完全不变。

**（B）upper-layer 版（`war3_upper_layer_shadow.cpp`，现 98-242）**

```cpp
bool TryBuildRuntimeGroupPalette(const model::ShadowGeosetResourceRecord &geoset,
                                 const model::PoseRecord &pose,
                                 std::vector<Matrix4> &outPalette,
                                 uint32_t &outMaxVertexGroupSlot,
                                 bool &outUsesAveraging) {
  outPalette.clear(); outMaxVertexGroupSlot = 0u; outUsesAveraging = false;

  if (!geoset.hasSkinningData()) return false;                            // 107-108
  if (pose.matrixPalette.empty() || pose.matrixCount == 0) return false;  // 109-110

  render::RuntimeGroupPaletteInput in = {};
  in.vertexGroupIndices = geoset.vertexGroupIndices.data();
  in.vertexGroupSlotCount = std::min<uint32_t>(                           // 111-112 原样
      geoset.vertexGroupCount, uint32_t(geoset.vertexGroupIndices.size()));
  if (in.vertexGroupSlotCount == 0u) return false;                        // 113-114
  in.matrixGroupSizes = geoset.matrixGroupSizes.data();
  in.groupCount = std::min<uint32_t>(                                     // 191-192 原样
      geoset.matrixGroupCount, uint32_t(geoset.matrixGroupSizes.size()));
  in.matrixIndices    = geoset.matrixIndices.data();
  in.matrixIndexCount = geoset.matrixIndices.size();
  in.posePalette      = pose.matrixPalette.data();
  in.posePaletteSize  = pose.matrixPalette.size();
  in.poseMatrixCount  = pose.matrixCount;

  render::RuntimeGroupPaletteOutput out = {};
  if (!render::TryBuildRuntimeGroupPaletteKernel(
          in, render::RuntimeGroupPaletteFallbackSet::MatrixAndPoseRemap,
          out, nullptr)) {                        // 无 detail：保持 resolve 的 stats 面
    return false;
  }
  outPalette            = std::move(out.palette);
  outMaxVertexGroupSlot = out.maxVertexGroupSlot;
  outUsesAveraging      = out.usesAveraging;
  return true;
}
```
调用点（372-380）**一行不改**。

### 3.4 用户约束 → 设计映射（逐条）

| 约束 | 设计上的落实 |
| --- | --- |
| 保持各调用者现有**来源顺序** | 内核**没有任何来源**；core 的 producer 快照 → `+0x08` 槽位 → 记忆槽位 → Game.dll arena 顺序与各自 return 点原样保留在适配层步骤 4 |
| 保持**失败语义** | 4 步 vs 5 步由 `RuntimeGroupPaletteFallbackSet` 表达；core 的 `FallbacksFailed` + `logFailure` 保留；upper 仍只返回 false 并自增 `resolveRuntimeGroupPaletteMiss` |
| 保持**坐标空间** | 内核只做 palette 重映射/等权平均，不引入任何 space 变换或矩阵分解（与现状一致，均为模型局部姿态矩阵副本） |
| 保持**有效期** | 内核只用传入的 `posePalette` 指针视图，不缓存、不持有、不跨帧；有效期仍由调用方既有的同帧 `ShadowPoseRecord` / `PoseRecord` 约定负责 |
| **不新增 legacy 读取** | 内核 0 处内存读取；适配层不新增任何 `SafeRead` / arena / slot 读取；既有读取既不增也不删 |
| **不把未知来源贴成已验证 Selection** | 内核与两个适配层都**不产出** `render::skin::Selection`，不调用 `Usable()/CanReplace()`；本设计不触碰 `war3_skin_palette_selection.h` |

### 3.5 必须拒绝的设计（反例）

- ✘ 把两侧 `slotCount` 统一成 `min(...)`"以免参数太多" → 改变 core 的扫描上界（D4）。
- ✘ 给 upper-layer 补上第 5 步 uniform 广播"反正更鲁棒" → 改变失败语义，可能让原本被拒的 geoset 出图。
- ✘ 给 upper-layer 加 miss detail / 日志 → 改变 `resolve` 的 stats 与 JSON 面。
- ✘ 把 core 的 `tryEngineDirectPosePalette` 搬进内核"顺便复用" → 直接违反 Q5。
- ✘ 在内核里新增 `outUsesAveraging = true` 默认值，或让 fallback builder 置位/重置该标志
  → 与主路径的置位语义冲突（会改变"主路径已置位后被 fallback 承接"的值）。

---

## 4. 生产函数级测试清单

### 4.1 归口（meson 既有体系）

新目标，仿 `war3_shadow_palette_storage_test`（`src/d3d9/meson.build:565-581`）与
`war3_current_draw_group_slot_summary_test`（:482-497）：

```meson
# 共享纯计算内核：两端重映射 / group 校验 / 等权平均的行为合同。
# 与来源选择链无关（S2 不合并来源），因此不链接任何 d3d9 / 游戏状态源。
war3_runtime_group_palette_kernel_test = executable(
  'war3_runtime_group_palette_kernel_test',
  [
    'war3/render/tests/war3_runtime_group_palette_kernel_test.cpp',
    '../util/util_matrix.cpp',   # 必需：Matrix4 的 + += == [] 均 out-of-line（util_matrix.h:48-64）
  ],
  include_directories : [ dxvk_include_path ],
  install             : false,
)
test(
  'war3_runtime_group_palette_kernel',
  war3_runtime_group_palette_kernel_test,
  timeout : 30,
)
```

- 测试文件：`src/d3d9/war3/render/tests/war3_runtime_group_palette_kernel_test.cpp`；
- 风格：沿用 `war3_shadow_palette_storage_test.cpp:13-17` 的 `bool require(bool, const char*)`
  + `main` 返回非 0（**不是 pytest、不是字符串静态检查**）；
- 位置：紧随 `war3_shadow_palette_storage` 目标之后（现 581 行后），与 palette 段同区；
- 台账现值 72 个 meson 目标（`docs/plan/2026-09-16-merge-execution-ledger.md:272`），
  本设计预计变为 73；**本轮未运行任何构建/测试，故不宣称门禁结果**；
- **不**把新用例塞进 `war3_shadow_palette_storage`（那是上传扩尾语义，职责不同）；
  **不**在 `AutoTest/` 增加字符串静态检查——`test_direct_pose_payload_on_demand_static.py`
  之类只能锚定字面量，无法证明行为，S2 的验收必须落在可运行的函数级目标上。

### 4.2 测试点清单（输入 → 期望）

1. **T1 分组等权平均**：`groupCount=2, matrixGroupSizes={3,1}, matrixIndices={0,1,2,0}`，
   pose 取 3 个互不相同的可辨识矩阵 → `palette[0] == (p0+p1+p2)/3.0f`（**逐位**比较，
   不是 epsilon）、`palette[1] == p0`、`usesAveraging == true`。
2. **T2 单元素组不做除法**：`matrixGroupSizes={1,1}` → `palette[0]==p0`、
   `palette[1]==p1` 逐位相等（守卫 `groupSize == 1u ? accum : accum / float(groupSize)` 两个分支）。
3. **T3 `usesAveraging` 只由 `groupSize>1` 置位**：全为 1 的组 → `false`；
   任一组 >1 → `true`。**修正（2026-09-17）**：`usesAveraging` 只由主路径的
`groupSize > 1` 置位；fallback builder **既不置位也不重置**它，因此"主路径已置位后被 fallback
救回"时仍为 `true`（T16 覆盖，与抽取前一致）。
4. **T4 输出尺寸 / 填充**：
   - 平均路径 `palette.size() == groupCount`；
   - `buildSparseMatrixRemap` / `buildSparsePosePalette` 路径 `size() == maxSlot+1`
     且**未写槽位为 `Matrix4(0.0f)`**（逐元素断言零矩阵，含未覆盖的尾部）。
5. **T5 `maxVertexGroupSlot` 扫描**：槽位序列 `{0,5,5,2}` → 5；`{255}` → 255；
   `slotCount` 只读前缀（见 T8）。
6. **T6 失败路径逐条（每条的 reason 与返回 false 都要断言）**：
   - `groupCount == 0` → 走 fallback 集合；
   - `running > matrixIndexCount` → `InvalidGroupTable`；
   - 某组 `groupSize == 0` → `InvalidGroupTable`（附 `group` 序号）；
   - `groupBase + groupSize > matrixIndexCount` → `InvalidGroupTable`；
   - `matrixIndex >= poseMatrixCount` → `MatrixIndexOutOfRange`（附 `matrixIndex`）；
   - `matrixIndex < poseMatrixCount` 但 `>= posePaletteSize` → `MatrixIndexOutOfRange`
     （**两个条件必须分别构造**，守卫 `||` 两侧）；
   - 某顶点槽位 `>= groupCount` → `VertexGroupOutOfRange`（附 `groupSlot`）；
   - 4 / 5 步全部失败 → `FallbacksFailed`。
7. **T7 fallback 集合差异（D2 的行为守卫）**：构造"只有 uniform 广播能成功"的输入
   （所有组的 `matrixIndex` 越界但 `matrixPalette` 非空）→
   `MatrixPoseAndUniformRoot` 返回 **true**，且 `palette` 全等于 `matrixPalette.front()`、
   `size() == max(maxSlot+1, matrixGroupSizes.size())`；
   `MatrixAndPoseRemap` 返回 **false**。**这条就是"不合并失败语义"的证据。**
8. **T8 计数派生差异（D4/D5 的行为守卫）**：同一份 `vertexGroupIndices`，
   分别用 `slotCount = size` 与 `slotCount = min(size, vertexCount)` 调用 →
   断言 maxSlot 与去重结果随之不同（把"第 `slotCount` 个元素"设为 255 用于区分）；
   同理 `groupCount = size` 与 `groupCount = min(matrixGroupCount, size)` 用未补齐的表区分。
   **该测试的目的是阻止未来"顺手统一 clamp"。**
9. **T9 fallback 顺序可观测**：构造"只有 direct 成立"/"只有 sparse 成立"/"只有 directPose 成立"/
   "只有 sparsePose 成立"的四组输入，用矩阵值识别命中的分支（不看内部标志位）。
10. **T10 退化与异常输入**：空 `posePalette`、`poseMatrixCount == 0`、`slotCount == 0`、
    全 `nullptr` 视图、`matrixIndices` 空但 `matrixGroupSizes` 非空、
    `uniqueGroupSlots.size() > matrixIndexCount`、`maxSlot` 极大而 `matrixIndexCount` 极小
    （越界必须失败而不是截断）→ 全部要求返回 false 且不越界写（用 `std::vector` 大小断言）。
11. **T11 确定性 / 无副作用**：同一输入连续调用两次，输出逐位相同；
    失败调用后的输出状态与"该失败路径的历史行为"一致（fallback 表达式整体重写 `palette`）。
12. **T12 规模边界**：`groupCount=1, slotCount=1`；256 个不同槽位（`seenGroupSlots` 全满）；
    `matrixIndex == poseMatrixCount - 1` 恰好合法。
13. **T13 等价性回归**：用表驱动用例锁定 §2.1 中 4 段 lambda 的语义
    （direct 表顺序敏感、sparse 表稀疏填充顺序敏感），保证抽取后与抽取前同值。

---

## 5. 暂不涉及边界（明确列出）

本轮（S2 收窄版）**不做**、也不在本设计中预留实现：

1. **S3**：三个 slot 读取器（`QueryBlendedPaletteBySlotIndex` / `…Exact` / `…BestEffort`）收敛，
   生产只允许 Exact —— 另立设计。
2. **S4**：合同 OFF 的 legacy arena / slot 读取降级为**默认关**的诊断开关 —— 另立设计；
   本轮**不新增也不删除**任何 legacy 读取。
3. **registry domain 隔离（P3 批次 4）与 alias 迁移（批次 5）** —— 与"资源生命周期收口"同向但不同层。
4. **Stage13 content-persistent 常驻几何（批次 6）** —— 前置占用/回收验证未过。
5. **`d3d9_device.cpp` 语义职责大迁移（M1-M4）** —— 独立方案
   （`docs/plan/2026-09-16-device-semantic-responsibility-migration.md`）。
6. **完整版 S2**：让两处入口产出并用 `render::skin::Selection` / `Usable()` / `CanReplace()`
   裁决 —— **已被 Q5 否决**，本设计不触碰 `war3_skin_palette_selection.h`。
7. **不合并、不删除、不重命名**任一 `TryBuildRuntimeGroupPalette` 实现；不改 6+1 处调用点结构。
8. **不改** `kUpperLayerShadowConsumerEnabled` / `kUpperLayerShadowConsumerObserveOnly`
   默认值（`war3_internal_test_config.h:770/776`），也不新开 env 开关。
9. **`HashMatrixPalette` 家族**（`war3_shadow_renderer_core.cpp:835`、
   `war3/model/war3_model_hook.cpp:927`、`d3d9_device.cpp:8398` 的 `War3SemanticHashMatrixPalette`）
   —— 真实重复，但**不在两函数体内**，本轮仅登记、不抽取。
10. **不改** `meson.build` 现有目标语义、不改 build32 配置、不构建、不部署、不 git。

### 5.1 落地前置与门禁（供后续执行会话，本轮不执行）

- **前置（已完成）**：Q5 要求"实机事务期间暂停 B 树源码/构建修改"→ 本设计原定**在实机结算前不落地**；
  实机事务已于 2026-09-17 03:2x 结算（现场已恢复原 DLL），随后落地实施。
  （`docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md` 冻结候选
  35,984,240 bytes / `8CECC495…` 完成后才解禁）。
- **落地门禁（沿用台账流程）**：`build32_safe` exit 0 → `ninja -C build32 -n` no-work →
  `meson test` 全过（预期 72 → 73）→ AutoTest 静态全量不退化 → 更新 DEVELOPMENT_CHANGELOG。
  任何 DLL 变化都使冻结候选失效，须重新冻结 / 重新走实机授权。
- **行为等价证明**：抽取后应能用 §2.1 的规范化比对方法（去空白 + 记录名统一 + 保序逐行匹配）
  对"新内核 + 适配层"重放，仍应得到 52/52 匹配；两处调用点的 JSON 计数与画面属**实机**范畴，
  不能被静态 / 单元测试替代。

---

## 6. 附：核对办法与基线哈希

### 6.1 同/异清单的核对办法（可复现）

1. 取 `war3_shadow_renderer_core.cpp` 的 6916-6992 与 `war3_upper_layer_shadow.cpp` 的 131-189；
2. 每行去首尾空白、压缩内部空白、剔除空行与注释行；
3. 把记录名统一（`resource.` 与 `geoset.` 都替换为同一占位符）；
4. 在 upper 序列上对 core 序列做**保序贪心逐行匹配**（单调推进指针）；
5. 结果：core 非空 67 行中 52 行匹配；upper 非空 52 行**全部**匹配；
   core 未匹配的 15 行恰为 `buildUniformPosePalette`（6976-6992）；
6. 主路径 7008-7071 vs 191-241 同法核对：差异仅为 D3（miss 记录）、D4（上界）、D5（groupCount 派生），
   以及不可见的类型宽度（`size_t` vs `uint32_t` 循环变量、`uint32_t(groupSlot)` 显式转换）。

### 6.2 核对基线（2026-09-17 01:35 +08:00，B 树）

| 文件 | 行数 | SHA-256 |
| --- | --- | --- |
| `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp` | 9749 | `4C968870F571D9F1F216BAB0418C2FDC3DC3E49C311D08F76D07B57CB90743FD` |
| `src/d3d9/war3/render/war3_upper_layer_shadow.cpp` | 455 | `61EB0C8FFDF24F36016B1651A7C677BE0C47E805680C5278F0CE18BD950517A9` |
| `src/d3d9/war3/render/war3_upper_layer_shadow.h` | 98 | `5B4A5960ABD9FC4AEF0034964EF8BB81071B75D4D0FB26C2D75D04F302738B29` |
| `src/d3d9/war3/shadow/war3_shadow_runtime_contract.h` | 516 | `23FB75E9906F14F9725A22C0A5C3B6AD7D9CD6CA053E2A2A49D8D090D2A9EC6B` |
| `src/d3d9/war3/model/war3_model_resource_cache.h` | 468 | `AD0E311CA8BCA50F6A670C09ED829D99B6E051E13ED43D10E700990A5D2FD2F9` |
| `src/d3d9/war3/model/war3_model_registry.h` | 590 | `3442271D9CC3D8A0666467DB73514252207089EB574FBBE15878146375C388B2` |
| `src/d3d9/meson.build` | 1294 | `D366C61D09F1D28C7F839D633DE78C21DEFDFD36EAE8C7604ABD0C3FA0E1FF23` |
| `src/d3d9/war3/core/war3_internal_test_config.h` | 1699 | `79EE9E08425E0223ABA5FB25C4A1C7FB4D86BD7D74074D5E6DD001C64E13A815` |

（表内行数为 `Get-Content` 实测；行号引用一律以函数名为主锚点，避免行号漂移导致误读。）

### 6.3 本文引用的关键符号（行号 + 函数名双锚点）

- `TryBuildRuntimeGroupPalette`：`war3_shadow_renderer_core.cpp:6509`、`war3_upper_layer_shadow.cpp:98`
- `tryEngineDirectPosePalette`：`war3_shadow_renderer_core.cpp:6552`（lambda）
- `buildUniformPosePalette`：`war3_shadow_renderer_core.cpp:6976`（lambda）
- `tryFallbacks`：`war3_shadow_renderer_core.cpp:6994`（lambda）
- `RuntimeVertexGroupSlotCount`：`war3_shadow_renderer_core.cpp:6412`
- `HashMatrixPalette`：`war3_shadow_renderer_core.cpp:835`、`war3_model_hook.cpp:927`
- `NoteRuntimeGroupPaletteMiss` / `RuntimeGroupPaletteMissDetail`：`war3_shadow_renderer_core.cpp:2698 / 2688`
- `ShadowRendererCore::resolveRecord`：`war3_shadow_renderer_core.cpp:7427`（6 处调用点）
- `UpperLayerShadowRegistry::resolve`：`war3_upper_layer_shadow.cpp:261`（1 处调用点 373）
- `TryConvertUpperLayerResolvedItem`：`war3_shadow_renderer_core.cpp:6475`
- `QueryRenderablePartPaletteSnapshot`：声明 `war3_model_hook.h:507`，实现 `war3_model_hook.cpp:9285`
- `FindOrUpdatePaletteSlotCache`：`war3_shadow_renderer_core.cpp`（调用点 6584）
- 纯内核先例：`war3_current_draw_group_slot_summary.h:10-54`、`war3_point_shadow_cpu_plan.h:11`
- 测试先例：`src/d3d9/meson.build:482-497 / 565-581 / 1101-1115`，
  `war3/render/tests/war3_shadow_palette_storage_test.cpp:13-17`

---

## 7. 结论

1. 两个同名 `TryBuildRuntimeGroupPalette` **确实共享一段逐语句相同的纯计算内核**
   （§2.1 的 13 个片段，其中 4 段 lambda 达到保序逐行 52/52 匹配）；
2. 但它们**不共享来源选择链、不共享失败诊断面、不共享计数派生**（D1-D8），
   故 Q5 的"只共享计算、不合并来源"在技术上成立，且是唯一安全解；
3. 本设计给出可执行的签名草案、放置位置、两处薄适配层伪代码，以及
   **可运行的生产函数级测试清单（13 组，归口到新 meson 目标 `war3_runtime_group_palette_kernel`）**；
4. 落地前置是实机事务结算；本轮只产出本文档，**未改 `src/`、未构建、未执行 git**。
