# War3 Semantic Shadow 接手计划（Phase 7.34+）

> **来源**：用户 2026-05-11 晚上传入，要求作为下一阶段的行动纲领。
> **立场修正**：上一轮把"寄生式管线天然不可解"当最终结论是错误的。
> 真正的问题不是时序竞争，而是三条**独立**的数据正确性线没修完：
> palette provenance、alpha-test caster payload、destructible 身份。

## 三条独立修复线

### 线 A — Palette provenance 仲裁（第一刀，最优先）

**事实依据（IDA 验证）**：
- `0x6F12FDC0` = `CModel_CopyPoseMatrixRangeFromStack` = authoritative final-pose
  range-copy，写 `runtimeModel + 0x60`，count 来自 `+0x5C`。
- `0x6F12E600` = `CGeosetData_BuildGroupBlendedPalette` = per-geoset blended
  palette writer，不是单矩阵 writer。**适合做 RenderQueue slot oracle，不应独占主路线。**
- 当前 submit 端仍然优先吃 `DrawTimeCaptured`，即使它来自 raw global arena
  或 partial trusted cache → 产生 pose 停顿、追帧、错误矩阵。

**要改的事情**：
1. `RawGlobalArena` 对 skinned semantic caster **只保留诊断**，不作为 Ready palette。
2. `QueryBlendedPaletteBySlotIndexExact` 必须 exact count 才能发布 trusted snapshot；
   partial 结果必须标记并拒绝，不能用零填充剩余矩阵。
3. skinned palette 选择顺序固定为：
   - `exact TrustedBlendedWriter` →
   - `0x12FDC0 RangeCopyPoseRebuild` →
   - explicit safe fallback
   - **禁止 raw `Game.dll+0xBC6BD0` 默认胜出。**
4. `War3TryBuildLiveRuntimeGroupPalette` 改为优先 PoseRegistry/range-copy rebuild，
   再用 exact slot cache；global slot raw read 只做 probe。

**要扶正 `0x12FDC0` 为主 pose authority**：
- 保持 `Hook_RuntimeMatrixRangeCopy` 发布完整 final-pose palette。
- 用 model resource 的 `matrixGroupSizeVec / matrixIndexVec / vertexGroupIndexVec`
  重建 per-part runtime group palette。
- 新增 counter：
  - matrixCount histogram
  - range-copy rebuild hit/miss
  - source winner
  - partial trusted reject
  - zero-matrix reject

**`0x12E600` 的正确定位**：
- 保留为 per-part slot 对账 oracle；
- 使用固定 ring/array，不做热路径分配。

---

### 线 B — Alpha-test caster 链路补完（修树叶/羽毛方形）

**现状**：alpha-test shader 已存在，但 semantic draw path 仍然把 `draw.uvStride = 0`，
没有给 shadow caster 绑定 UV buffer → 树叶/羽毛继续方块。

**要改的事情**：
1. semantic/canonical path 必须把以下字段写进 `War3ShadowPersistentGeometry`
   和 `War3ShadowCasterDraw`：
   - alpha material
   - diffuse texture descriptor
   - sampler index
   - UV buffer / offset / stride / format
2. shadow caster pipeline 支持 UV vertex binding：
   - 当前最多只绑 position/blend
   - 需要支持 UV binding 2，或正确复用 binding 0/1
3. 严格闸门：
   - 只有 `alphaTestEnabled && diffuseTexture && uvFormat valid && uvStride != 0`
     才允许 alpha caster
   - 否则**拒绝**，不退化成透明/alpha 广撒 caster
4. 验收目标：树、羽毛、栅栏**不再投方形卡片阴影**。

**上轮错误**：Phase 7.33 曾把 `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER`
默认改为 0 试图让 cutout 直接进 shadow map，实测产生方形卡片。
**已回退为 1**（见 `d3d9_device.cpp::War3SemanticRejectUnsafeAlphaCasterRuntime`）。
在线 B 真正打通之前保持过滤开启。

---

### 线 C — Destructible 身份（单独修，不全局扩大 part key）

**要改的事情**：
1. 先修 `objectKind / rawcode / jHandle` 分类：
   - 让大门、建筑、destructible 不再落进 skinned unknown bucket。
2. `payload11C` 只在 `objectKind` 明确是 destructible 后作为 slice 维度使用：
   - **不全局加入 manifest part key**（上轮 Iter C 已证实 benchmark FPS 会崩）。
3. 验收：
   - 大门 open/close 期间 object key 稳定
   - part 数不爆炸
   - identity churn 不升高

---

## 性能与禁区

**绝对禁区（AGENTS.md + 本计划共同约束）**：
- ❌ 不用 receiver hold / reuse shadow map 作为最终修复
- ❌ 不用 shadow TAA 遮盖问题
- ❌ 不用上一帧 caster list fallback 作为最终修复
- ❌ 不做 VB/IB capture snapshot 回退
- ❌ 不继续调 grace window
- ❌ 不把 alpha/cutout/transparent caster 全开（Phase 7.33 已被证伪）
- ❌ 不把 `payload11C` 全局塞进 part key（Iter C 已被证伪）
- ❌ 不把 `0x12E600` batch capture 当最终答案（只是 oracle）

**性能修复的顺序**：
1. `Populate` 中 O(N) 扫描和高频 hash/map 压力确实该修
2. 但**必须在 palette/alpha correctness 之后做**
3. 不能用性能优化掩盖正确性缺陷

---

## 每轮验收动作（强制）

每次落地代码修改后必须完成：
1. `ninja -C build32` 通过
2. `AutoTest/phase720_hot_shadow_poll.py`（或等价 `run_quick_autotest + hot_shadow_probe`）
3. 记录 artifact 目录、关键 counters、视觉截图

---

## 验收标准（Acceptance）

- ✅ skinned 单位阴影**持续可见**，pose 不再"停顿一会再追帧"
- ✅ skinned submitted palette 中 `RawGlobalArena` 为 0 或仅诊断，不参与最终提交
- ✅ `RangeCopyPoseRebuild` 或 exact trusted writer 成为**主来源**
- ✅ partial trusted 被拒绝（不是零填充蒙混过关）
- ✅ 树叶、狮鹫羽毛等 alpha-test 模型**不再是方形阴影**
- ✅ destructible 大门动画不再因全局 key 膨胀或 slice 混淆闪烁
- ❌ 不引入禁区方案
- ❌ 不靠假稳定遮盖问题

---

## 接手的执行顺序

**第一刀必须是线 A（palette provenance 仲裁）：raw arena 不能再赢。**

后续顺序：
1. 线 A：palette provenance（本轮重点）
2. 线 B：alpha-test caster payload（视觉最明显）
3. 线 C：destructible 身份（闪烁残余）
4. 性能/O(N) 清理（在正确性达标后）

---

## 当前代码入口点（Phase 7.34 起点）

**palette provenance 相关**：
- `src/d3d9/war3/render/war3_current_draw_contract.cpp`
  - `RecordHasLocalPaletteSnapshot`（上轮 Phase 7.32 放宽了 captureSerial）
  - `QueryBlendedPaletteBySlotIndexExact`（需要加 partial 拒绝）
- `src/d3d9/war3/model/war3_model_hook.cpp`
  - `Hook_RuntimeMatrixWrite`（当前走 `0x12E600` batch capture）
  - 需要新增 `Hook_RuntimeMatrixRangeCopy`（`0x12FDC0`）作为主 authority
- `src/d3d9/d3d9_device.cpp`
  - `PopulateDirectSceneShadow`（palette source 选择仲裁点）
  - `War3TryBuildLiveRuntimeGroupPalette`

**alpha-test caster 相关**：
- `src/d3d9/d3d9_device.cpp::War3SemanticRejectUnsafeAlphaCasterRuntime`（闸门）
- `src/d3d9/d3d9_device.cpp::War3CurrentDrawRecordIsUnsafeAlphaCaster`
- `src/d3d9/war3/shadow/*.cpp`（caster pipeline + shader binding）
- `subprojects/war3fx/shaders/war3_shadow_caster_*.frag`（需要加 alpha test discard）

**destructible 相关**：
- `src/d3d9/war3/render/war3_visible_renderables.cpp`
  - `ShadowManifestPartKey`（Phase 5 已经在 destructible 上加了 payload11C）
  - `War3ResolveSemanticPacketObjectKindFast`（objectKind 解析）

---

## 上下文压缩防丢字段

如果本文件下次被压缩遗漏，关键信息：

1. **两个 RVA 必须记住**：
   - `0x6F12FDC0` = authoritative pose range-copy（主路线）
   - `0x6F12E600` = per-geoset blended palette writer（oracle）
2. **raw arena 必须降级为诊断**，不能再赢 palette 仲裁
3. **alpha test 必须真正实现**，不是关闭过滤
4. **destructible 不能全局扩 key**
5. **所有 receiver hold / reuse / TAA / VB-IB / grace window 都是禁区**
