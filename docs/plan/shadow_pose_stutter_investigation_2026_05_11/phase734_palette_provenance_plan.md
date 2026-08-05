# Phase 7.34 线 A 实施计划：Palette Provenance 仲裁

## 调研结论

### 现状链路（当前代码）

```
0x12E600 CGeosetData_BuildGroupBlendedPalette
    └─> Hook_RuntimeMatrixWrite (war3_model_hook.cpp)
        └─> s_slotBlendedPaletteCache[slotIndex..]  (per-slot, frameTag/writeSerial)

0x12FDC0 CModel_CopyPoseMatrixRangeFromStack   <-- authoritative
    └─> Hook_RuntimeMatrixRangeCopy (war3_model_hook.cpp:7220)
        └─> RecordRuntimeMatrixPaletteFromRangeCopy(..., publishPalette=FALSE)  <-- 关键：不发布
            ├─> TryReadRuntimeMatrixPaletteFromRangeCopy → 读 a2/a3 段
            └─> g_runtimeMatrixRangeCopy*Count 只做诊断  ❌ 从不写 PoseRegistry

仲裁端（war3_current_draw_contract.cpp:1215-1253）
    1. QueryBlendedPaletteBySlotIndexExact(slotIdx, count, frameTag)
       → s_slotBlendedPaletteCache 查 0x12E600 结果
       → 命中：provenance = TrustedBlendedWriter
    2. else: paletteSource = record.paletteAddress (raw arena)
       → provenance = RawGlobalArena   ❌ 计划禁区：不能默认胜出
```

### 三个核心 BUG

**Bug 1（raw arena 保底）**：`war3_current_draw_contract.cpp:1250-1253`
```cpp
if (paletteSource == nullptr) {
  paletteSource = reinterpret_cast<const uint8_t*>(record.paletteAddress);
  provenance = PaletteProvenance::RawGlobalArena;
}
```
trusted miss 时直接回退 raw arena —— 违反计划第一条。

**Bug 2（零填充蒙混）**：`war3_current_draw_contract.cpp:1230-1245`
```cpp
const uint32_t trustedCount = std::min<uint32_t>(
    uint32_t(trustedPalette.size()), record.capturedPaletteCount);
for (uint32_t i = 0u; i < trustedCount; ++i) { ... }
// trustedPaletteBytes[trustedCount..capturedPaletteCount) 保持 0 字节
paletteSource = trustedPaletteBytes.data();
```
trusted palette size 小于 expected 时，剩余矩阵是**零字节**，下游按 expected count 读到零矩阵。

**Bug 3（`QueryBlendedPaletteBySlotIndexExact` 允许 partial）**：`war3_model_hook.cpp:7798-7820`
- 遇到 `!entry.valid` 或 frameTag 差 > 1 时 `break`（非 fail）
- 函数名叫 Exact 但实际是 "best effort"
- 这是 Bug 2 的根源 —— partial 能通过就是因为 Query 先放水

**Bug 4（range-copy 不发布）**：`war3_model_hook.cpp:7248, 7250`
```cpp
RecordRuntimeMatrixPaletteFromRangeCopy(runtimeModel, a2, a3, false, false);
                                                               ^^^^^^
                                                               publishPalette=false
```
`0x12FDC0` authoritative 数据从未通过 PoseRegistry 发布给仲裁端。

## 本轮修改范围（最小侵入，可回滚）

### 修改 1：`QueryBlendedPaletteBySlotIndexExact` 恢复严格语义
**文件**：`src/d3d9/war3/model/war3_model_hook.cpp`（第 7788-7820 行）

**原逻辑**：遇到 invalid / frameTag 漂移 → break（返回 partial）
**新逻辑**：
- 默认严格：遇到 invalid / frameTag 漂移 → 整体 return false（不再 partial）
- 保留旧宽松行为作为可选：新增辅助函数 `QueryBlendedPaletteBySlotIndexBestEffort` 供诊断查看
- 新增 counter：
  - `g_queryBlendedPaletteExactHit`
  - `g_queryBlendedPaletteRejectedSlotOverflow`
  - `g_queryBlendedPaletteRejectedInvalidEntry`
  - `g_queryBlendedPaletteRejectedFrameTagMismatch`
  - `g_queryBlendedPaletteRejectedShortResult`

### 修改 2：仲裁端禁止 raw arena 默认胜出 + 拒绝 partial 填充
**文件**：`src/d3d9/war3/render/war3_current_draw_contract.cpp`（第 1215-1253 行附近）

**新仲裁顺序**：
1. `QueryBlendedPaletteBySlotIndexExact` exact 命中（`size == capturedPaletteCount`）→ `provenance = TrustedBlendedWriter`
2. `QueryRuntimeMatrixPaletteByRuntimeModel`（新增，来自 `0x12FDC0` PoseRegistry）→ `provenance = RangeCopyPoseRebuild`
3. `QueryBlendedPaletteBySlotIndexBestEffort` 且 `size == capturedPaletteCount` → 保留 `TrustedBlendedWriter`（不再接受 size < expected）
4. **拒绝**：既不 exact 命中也拿不到 range-copy 的 skinned record，publishGlobal 直接丢弃，不再写 raw arena snapshot

**关键**：
- 不再把 `record.paletteAddress` 直接当 `RawGlobalArena` snapshot 发布
- `PaletteProvenance::RawGlobalArena` 只保留在 counter 里做诊断
- 新增 counter：`paletteRejectedNoTrustedSourceCount`, `paletteAcceptedFromRangeCopyCount`

### 修改 3：`Hook_RuntimeMatrixRangeCopy` 发布到 PoseRegistry
**文件**：`src/d3d9/war3/model/war3_model_hook.cpp`（第 7220-7260 行）

**改动**：把 `publishPalette=false` 改为通过新开关 `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH`（默认 1 = 开启发布）。
- 开启时：走 `PublishRuntimeMatrixPalette` → 写 `PoseRegistry::recordMatrixPalette`
- 保留原 "preferRuntimePoseUpdate" 逻辑作为可选覆盖（环境变量关闭）
- 新增 counter：
  - `g_runtimeMatrixRangeCopyPublishedToPoseRegistryCount`
  - `g_runtimeMatrixRangeCopySuppressedByLaterUpdateCount`

### 修改 4：仲裁端查询 PoseRegistry
**文件**：`src/d3d9/war3/render/war3_current_draw_contract.cpp`

通过 `record.renderablePart → runtimeModel` 的解析，调用 `PoseRegistry::findByRuntimeModel(...)` 拿到 matrixPalette。
- 需要确认：`CurrentDrawContractRecord` 中是否已经有 runtimeModel 字段或可以从 renderablePart 反查？
- 如果没有现成反查，本轮先不做修改 4，只做修改 1-3 + 修改 2 的 exact 门槛。

## 实施顺序与回滚点

| 步骤 | 修改 | 风险 | 回滚方式 |
|------|------|------|---------|
| A1 | 修改 1：Query 严格化 + 新 counter | 可能让 trusted hit 下降（因为之前的 partial hit 现在 fail）| counter 观察 |
| A2 | 修改 2：仲裁端拒绝 raw arena + partial | skinned submitted 可能掉坑，但符合计划 | 环境变量 `DXVK_WAR3_PALETTE_ARBITRATION_STRICT` 回滚 |
| A3 | 修改 3：range-copy 发布到 PoseRegistry | 覆盖 RuntimePoseUpdate 的 stable segment 风险 | `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH=0` 回滚 |
| A4 | （可选）修改 4：仲裁端查 PoseRegistry | 需要 runtimeModel 反查基础设施 | 本轮可不做 |

每步落地后必须：
1. `ninja -C build32` 通过
2. AutoTest `hot_shadow_poll`：记录 `paletteProvenance*`/`paletteCaptureTrustedSource*`/`runtimeMatrixRangeCopy*` counter
3. 用户视觉复核

## Acceptance（与计划对齐）

- ✅ `paletteProvenanceRawGlobalArenaCount = 0`（或仅诊断）
- ✅ `paletteRejectedNoTrustedSourceCount > 0` 表示过滤在工作
- ✅ 严格 exact hit 或 range-copy 成为主来源
- ✅ 视觉：pose 不再"停顿一会再追帧"
- ⚠️ 如果 exact hit 严格化后主线直接断：必须补 range-copy publish（修改 3）


---

## Phase 7.34 A1+A2 执行结果（2026-05-11 21:20）

### 落地代码
1. ✅ `war3_model_hook.cpp::QueryBlendedPaletteBySlotIndexExact`
   - partial 情形整体 return false + outPalette.clear()
   - 新增 5 个诊断 counter：exactHit / slotOverflow / invalidEntry / frameTagMismatch / shortResult
2. ✅ `war3_model_hook.cpp::QueryBlendedPaletteBySlotIndexBestEffort`（新增，仅诊断）
   - 保留旧宽松行为作为观察通道
   - 新增 counter：bestEffortHit
3. ✅ `war3_model_hook.h` 同步声明 + 严格语义注释
4. ✅ `war3_current_draw_contract.cpp` 仲裁块：
   - Exact 命中要求 `size == capturedPaletteCount`（双重防御）
   - 移除 partial 零填充路径
   - `DXVK_WAR3_PALETTE_ARBITRATION_STRICT=1`（默认）下 trusted miss 整条丢弃
   - 新增 counter：`g_paletteRejectedNoTrustedSourceCount`
   - 兼容回滚：`DXVK_WAR3_PALETTE_ARBITRATION_STRICT=0` 恢复旧 raw arena 行为
5. ✅ `war3_current_draw_contract.h` summary 新增 `paletteRejectedNoTrustedSourceCount` 字段

### 编译
- `ninja -C build32` 通过（仅既有 warning）
- DLL 部署到 `E:\Work\War3\`（25164289 bytes，mtime 21:18）

### 首轮 AutoTest（hot_shadow_poll，隔离桌面）
- `semanticScenePopulateLastReturnReason = 9`（成功）
- `ReceiverHoldEmptyReplayCount = 0`
- `ReceiverReuseShadowMap = 0`
- `ShadowCastersCount = 33`
- `ShadowMapDrawnCasters = 117`
- `SubmittedSkinnedPaletteSourceDrawTimeCapturedCount = 33`（100%）
- `SubmittedSkinnedPaletteSourceNoneCount = 0`（无 source=None 退化）
- `PaletteCaptureTrustedSourceHitCount = 6529`（vs Miss = 1978，hit rate ~77%）

### 胜利点
1. **raw arena 不再默认胜出**：所有 ready publish 都经过严格 trusted 验证
2. **partial 零填充彻底消除**：Query 层和仲裁层双重防御
3. **阴影管线未退化**：ShadowCasters/DrawnCasters 正常
4. **严格拒绝路径可观测**：counter 接入完成（但 control-plane 暴露待补）

### 残余问题（需要修改 3）
- `semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount = 7`
- `semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount = 7`
- 即便 100% trusted 命中，仍有 live→live 大 delta
- 根因推测：`0x12E600` 作为 late group-blend writer，某些场景下可能存在跨对象污染
- **解决方向**：修改 3 — 把 `0x12FDC0` publishPalette 改为 true，让 range-copy 写入 PoseRegistry，
  作为对账 oracle（不直接替换 trusted，而是给 trusted 做完整性校验）

### 下一步
- A3: `Hook_RuntimeMatrixRangeCopy` → `PublishRuntimeMatrixPalette(..., publishPalette=true)`
- A4 (可选): 仲裁端查 PoseRegistry 作为 RangeCopyPoseRebuild 来源

### 用户视觉验收
- 等用户前台进游戏观察：
  - 大门闪烁是否进一步减轻
  - Caster 阴影稳定性（是否有"停顿→追帧"消失）
  - alpha-test 方形卡片仍存在是**预期**（线 B 未做）
