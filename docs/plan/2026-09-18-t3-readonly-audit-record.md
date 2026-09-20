# T3 只读审计记录（2026-09-18 主线程；**只读，未实施任何修复**）

> 性质：本文件把 `2026-09-18-overnight-progress-log.md` 中跨约 18 轮的 T3 只读审计结论**合并成单一交付件**。
> 纪律：全程**未改任何源码**；树保持冻结（ninja -C build32 -n = no work）；现场 DLL `A0A51AF2…` 未替换。
> 措辞上限：本文件**不声称** T3 已闭合、**不声称**运行时安全；所有"已验"均标注证据类型（读码 / ripgrep / 宿主测试执行）。

## 1. 审计对象与结论一句话

**对象**：`s_slotBlendedPaletteCache`（生产者侧调色板槽位缓存）与 T3 指出的**控制面 drain 分离线程**并发风险。

**一句话结论**：**T3 担心的那条具体机制（drain 线程推进构建/触碰槽位缓存）在代码结构上已被拦断**，且该闸门语义**有执行证据**；剩余未闭合仅为（i）其它间接路径、(ii) 实机运行期印证、(iii) 其它共享非原子状态的范围。

## 2. 缓存访问面（ripgrep，src 全量）

- **访问点 9 处，全部在单一 TU**：src/d3d9/war3/model/war3_model_hook.cpp；
- **写面 3 处（读函数体确证）**：

| 行 | 形态 | 性质 |
| --- | --- | --- |
| :304 | `s_slotBlendedPaletteCache = {};` | 重置 |
| :2401 | `entry.frameTag / writeSerial / valid / matrix = …` | **逐帧生产者写**（在 `CaptureBlendedPaletteSlotRange`，:2369） |
| :10337 | `for (auto& entry : …) entry.valid = false;` | **跨地图失效**（位于纪元发布之后） |

- **读面 6 处（枚举已对账：1+2+1+2 = 6）**：

| 函数 | 行 | 读点 | 生产调用者 |
| --- | --- | --- | --- |
| `QueryBlendedPaletteBySlotIndex` | :9010 | :9026 | **有**：war3_live_palette_selection.cpp:512 |
| `ValidateBlendedPaletteBySlotIndexExact` | :9073 | :9085 | 仅被 `…Exact` 调用 |
| `QueryBlendedPaletteBySlotIndexExact` | :9110 | :9123、:9153 | **无** |
| `QueryBlendedPaletteBySlotIndexBestEffort` | :9166 | :9179、:9220 | **无** |

- 缓存规模：`kSlotBlendedPaletteCacheSize = 65536`（:302-303）。

## 3. 发布/读取协议（读码）

- **写方**（:2396-2406）：先天 `s_slotBlendedPaletteWriteSerial.fetch_add(count, memory_order_relaxed)` 取序号，**再**写**非原子**字段 `frameTag` / `writeSerial` / `valid` / `matrix`；
- **读方**（:9023-9039）三重一致性检查：`entry.valid` → `entry.frameTag` 与当帧 `TryReadCurrentPaletteFrameTag` 一致 → **逐条目 `writeSerial` 跨连续槽位严格递增**；
- **全局序列号 `s_slotBlendedPaletteWriteSerial` 只写不读**（全树仅 2 处：声明 :305 与 :2397 的 fetch_add）⇒ **今日不构成读侧校验**；读方使用的是**逐条目** `writeSerial`；
- 另有**一种**缓存的 `writeSerial` **是原子**（:323 定义 / :2568 store）⇒ 两种缓存、两种原子性，不可混述。

## 4. 并发风险的两端（读码）

- **写端**：`CaptureBlendedPaletteSlotRange`（war3_model_hook.cpp:2369）——**游戏 hook 抓取路径**（从 Game.dll 侧 srcBase 解码 48 字节姿态）；
- **读端**：`QueryBlendedPaletteBySlotIndex`（war3_model_hook.cpp:9010）由 **`War3TryBuildLiveRuntimeGroupPalette`**（war3_live_palette_selection.cpp:173）在 :512 调用 ⇒ **提交时（submit-time）选择链**；
- `War3TryBuildLiveRuntimeGroupPalette` 在**生产代码中恰 5 个调用点，全在 d3d9_device.cpp**：:6634、**:20418**（前置注释 :20397-20404：EveryFrame OR LagExceedsThreshold，**默认每帧**）、:20813、:26228、:26308；
- 承载 drain 的 src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp 对 `war3_live_palette_selection|War3TryBuildLiveRuntimeGroupPalette` **命中 0**（直接调用层面）。

## 5. **关键：线程闸门（2026-09-17「方案 B 第一部分」已在树中）**

war3_shadow_renderer_core.cpp:9568-9579：

    void ShadowValidationRuntime::ensureLatestFrameBuilt() {
      requestLatestFrameBuild();
      // 2026-09-17 上级裁定（线程修复方案 B 第一部分）：构建推进只允许发生在所有者线程。
      // 控制面 drain（…）在命名管道的分离线程上调用本函数；而 palette 槽位缓存是非原子读、
      // 构建进度字段也在锁外更新 ⇒ 非所有者/所有者未建立一律只保留请求语义，不推进任何分块。
      // 该检查位于推进边界，因此同时覆盖 allowControlPlaneSemanticDrain 与 IsHotSemanticBuildWaitPayload。
      // 所有者身份取自 hook 生命周期观测到的主循环线程，不是第一个请求线程。
      if (!consumePermissionGranted(false))
        return;

`consumePermissionGranted`（:9533-9543）：**唯一一份**消费权限检查，两处推进入口共用（:9578 directEntry=false、:9687 directEntry=true）；所有者 = hooks::GetMainLoopThreadId()，调用者 = ::GetCurrentThreadId()；**三类拒绝计数**（offThreadRefused / ownerUnestablishedRefused / directAdvanceRefused）。

### 执行证据（宿主测试真跑，非仅读码）

- build32/src/d3d9/war3_shadow_build_thread_gate_test.exe ⇒ **EXIT=0 / SUMMARY 9/9 PASS**，其中：
  - **C3 pipe-thread-drain owner=0x1A2B current=0x00FF -> NotOwner** ← **即 T3 场景本身**；
  - C4 implicit-hot-wait -> NotOwner、C5 direct-lower-entry -> NotOwner（第二入口）、C6 bogus-owner -> NotOwner、C2/C7 owner -> Allow、C1/C7 owner-unestablished -> OwnerUnestablished；
- build32/src/d3d9/war3_shadow_build_lifecycle_test.exe ⇒ **EXIT=0 / SUMMARY 187/187 PASS**（含 C2.non-owner-entry-off-thread-count actual=1 expected=1、C8 发布压测 publishes=200001/reads=200000）。

## 6. 无调用者的三个函数（死代码候选，**未清理**）

- `QueryBlendedPaletteBySlotIndexExact`（:9110）、`QueryBlendedPaletteBySlotIndexBestEffort`（:9166）、`ValidateBlendedPaletteBySlotIndexExact`（:9073）在 src 内**无任何调用者**；
- 它们在 AutoTest/data_collection_entrypoints.json 中以 **WARVK_DATA_SCOPE 插桩点清单条目**形式出现，由 AutoTest/test_data_collection_static.py 核对 ⇒ **登记 ≠ 调用**，**不构成运行期调用者**；
- ⇒ 与 **A9/A10 同类**；清理需同步改清单 + 该静态测试（机制与 A9/A10 的 FROZEN/锚点同型）。**本夜不动**。

## 7. 未闭合项（交下一班）

| # | 未闭合项 | 需要的证据 |
| --- | --- | --- |
| (i) | 是否存在**其它间接**路径（函数指针/导出符号/跨模块转发）读写该缓存 | 全仓调用图/符号分析 |
| (ii) | **实机运行期**：所有者确为主循环线程；drain 被拒计数出现 | **实机**（依赖 ①/② 的"能进图"前置）；可观测量 = 上述三类拒绝计数 |
| (iii) | T3 点名的"其它共享非原子状态"是否也被同类门覆盖 | 逐项读码（本夜未查） |

## 8. 本夜为此做的自我纠正中的 3 条（诚信留痕）

1. 「发布协议 = relaxed 序列号 + 非原子载荷」→ **不完整**：存在另一种原子 writeSerial 缓存，且读侧**确实有**单调性校验；撤回"读方无法排除撕裂"的泛化说法；
2. 用 `Select-String -Path` 配合双层通配符得 0 命中 ⇒ **假阴性**（通配符只当一层目录）⇒ 改用 ripgrep 复核；
3. 「data_collection_entrypoints.json 是运行期消费者」⇒ **错误**（实为静态测试清单）⇒ 已更正。

## 9. 红线

- 本文件**不授权**任何代码改动；T3 的修复已存在（2026-09-17），**不要**在本夜之后再引入第二套守卫；
- 不得把宿主测试通过表述为"实机无竞态"；不得把本文件当"T3 已闭合"；
- 现场 DLL 仍为 `A0A51AF2…`，未替换、未部署、未提交。

## 7.1 (iii) 在生产者模块内**已闭合**：可变共享静态量只有 2 个

实测 `src/d3d9/war3/model/war3_model_hook.cpp` 的**文件级 `static`** 共 **22** 个：**7 个原子**、其余 15 个为 `constexpr` 常量或 `static inline` 函数（**非状态**）。

**排除常量与函数后，可变共享静态量仅 2 个，且都非原子**：

| 行 | 对象 | 规模 | 备注 |
| --- | --- | --- | --- |
| :303 | `s_slotBlendedPaletteCache` | 65536 条目 | **T3 审计对象**（本文件 §2-§5） |
| :339 | `s_renderablePartPaletteBindings` | 8192 条目 | 另一个生产者缓存；**其条目含原子发布字**（:10344-10345 `renderablePart.store(0u, release)` / `paletteWriteSerial.store(0u, release)`） |

**7 个原子共享量**（全部为序列号/纪元/缓存指针）：`s_slotBlendedPaletteWriteSerial`(:305)、`s_renderablePartPaletteBindingSerial`(:342)、`s_renderablePartPaletteSnapshotSerial`(:343)、`s_skinPalettePublicationTicket`(:344)、`s_skinPaletteOwnerEpoch`(:345)、`s_cachedGlobalPaletteBufBase`(:346)、`s_runtimeModelValidationSessionGeneration`(:351)。

### 由此得到的两点（都可支撑后续决策）

1. **"其它共享非原子状态"的范围在该模块内是有限的**：只有 2 个数组；其中 `s_renderablePartPaletteBindings` **已有原子发布字**先例 ⇒ 若将来讨论加固，**树内已有可参照的原子发布模式**（不必从零设计）；
2. 该模式也解释了为何 T3 的对象值得单独看：**同模块的兄弟缓存用原子发布字，而槽位缓存用非原子载荷字段 + 只写不读的全局序列号** —— 这是**设计不一致**，而不是"全模块都没做保护"。

> 仍未查：其它模块（render/shadow/bridge）的共享非原子状态；(i) 的间接路径。**不声称 T3 闭合**。

## 7.2 (iii) 扩展到读侧模块与 drain 所在模块：**结论更强**

| 模块 | 文件级 `static` | 原子 | 可变非原子 | 判定 |
| --- | --- | --- | --- | --- |
| 生产者 `war3_model_hook.cpp` | 22 | 7 | **2**（两个数组，§7.1） | T3 对象所在 |
| **读侧 `war3_live_palette_selection.cpp`** | **0** | 0 | 0 | **完全无文件级共享状态**（M2 迁移后的选择链是纯函数式） |
| **drain 宿主 `war3_shadow_renderer_core.cpp`** | 20 | **15** | 3（见下） | **无非共享非原子状态** |

drain 宿主那 3 个"非原子"实测为：

- `:773` `static thread_local PaletteSlotCacheEntry s_paletteSlotCache[…]` ⇒ **`thread_local`**（每线程各一份，**构造上不构成数据竞争**）；
- `:774` `static thread_local size_t s_paletteSlotCacheIndex = 0;` ⇒ **`thread_local`**；
- `:816` `static uint32_t FindOrUpdatePaletteSlotCache(…)` ⇒ **函数，不是状态**。

### 因此 (iii) 在 T3 路径上的三个模块内**基本闭合**

1. **读侧模块零文件级状态** ⇒ 读路径不引入新的共享可变状态；
2. **drain 宿主模块的共享可变状态全是原子（15）或 `thread_local`（2）** ⇒ 该模块**没有**共享非原子状态；
3. **唯一不符合该纪律的就是 T3 对象本身**（`s_slotBlendedPaletteCache`：非原子载荷 + 只写不读的全局序列号），而同模块兄弟缓存已有原子发布字先例。

⇒ 综合 §5 的闸门（分离线程在推进边界被拒 + 执行证据 9/9）与本节，**T3 的剩余风险面已收敛到**：(i) 间接调用路径；(ii) 实机运行期印证；以及**本审计未覆盖的形态**——**类成员/堆共享状态**（见下）。

### **本审计的覆盖边界（必须明示）**

- 本节只统计**文件级 `static`**；**未覆盖**：类的**成员变量**、堆分配后被多线程共享的对象、`m_mutex` 等锁保护下的状态、以及其它模块（bridge/hooks/native/gpu_skin 等）；
- 因此**不能**据此宣称"全仓无其它共享非原子状态"，只能宣称"**T3 路径上的三个模块的文件级共享状态已枚举完毕**"。

## 5.1 闸门的**完整契约**（`war3_shadow_build_lifecycle.h`，读码补齐）

- **来源裁定**（`:9-14`）：**2026-09-17 上级裁定（codex `01a02e0b`）**——「9 个测试实际只调用线程判定函数，没有调用真实推进/Reset 边界」⇒ 要求补**生产共用**的推进/发布生命周期测试（覆盖未知所有者、两种入口、正常推进、取消/Reset、旧工作不得重新发布、并发摘要读取）；因此把「**所有者门 + 按值进度发布 + 工作代际守卫**」收进一个组件，使**运行时两处推进入口与宿主测试共用同一份实现**（测试覆盖真实边界，而非复制品）。
- **自身不加锁**（`:16`）：「本组件自身不加锁（并发保护由调用方的 mutex 提供，与运行时一致）；不引入堆分配。」
- **判定为纯函数**：`consumeAllowed` 委托 `DecideShadowBuildAdvance(ownerThreadId, currentThreadId)`（`war3_shadow_build_thread_gate.h`）⇒ 这正是 `build_thread_gate_test` 那 **9 个用例**所覆盖的对象。
- **两类入口**：`directEntry=true` 表示来自真正的推进入口（`ensureFrameBuiltForContract`），**另计一份拒绝数**，"用于证明『直接调用底层方法也无法绕过』"。

### ⚠️ 报告约束（我必须遵守，也请后续 agent 遵守）

> `:21` 原文：「入口分类与拒绝原因**不是天然互斥的统计维度**，不得未经证明相加成『总拒绝』。」

- ⇒ **不得**把 `offThreadRefused + ownerUnestablishedRefused + directAdvanceRefused` 相加成"总拒绝数"；
- 与测试输出一致：`C1.entry-classes-are-not-additive PASS`（该用例正是把这条约束固化成断言）；
- 本审计记录全文**未**做此类相加（自查确认）。

### 闸门链条（自上而下，四层）

| 层 | 物件 | 证据 |
| --- | --- | --- |
| 1 纯判定 | `DecideShadowBuildAdvance`（thread_gate 头） | `build_thread_gate_test` **9/9 PASS**（含 `C3 pipe-thread-drain -> NotOwner`） |
| 2 生命周期封装 + 计数 | `ShadowBuildLifecycle::consumeAllowed` | `build_lifecycle_test` **187/187 PASS** |
| 3 运行时入口 | `ShadowValidationRuntime::consumePermissionGranted`（两入口共用） | 读码（:9533-9543、:9578、:9687） |
| 4 推进边界调用点 | `ensureLatestFrameBuilt()` :9578 / `ensureFrameBuiltForContract` :9687 | 读码 + `:9571-9577` 注释 |

## 7.3 (i) 静态层面闭合：三个关键函数**无取地址** ⇒ 无函数指针间接调用

ripgrep `src/` 全量，检查是否存在对关键函数的**取地址**（函数指针/回调注册的前置条件）：

| 模式 | 命中 |
| --- | --- |
| `&QueryBlendedPaletteBySlotIndex` | **0** |
| `&War3TryBuildLiveRuntimeGroupPalette` | **0** |
| `&CaptureBlendedPaletteSlotRange` | **0** |

结合已知直接调用点：

- **读端**：`QueryBlendedPaletteBySlotIndex` 的唯一生产调用点是 `war3_live_palette_selection.cpp:512`（**直接调用**）；
- **枢纽**：`War3TryBuildLiveRuntimeGroupPalette` 的 5 个生产调用点全在 `d3d9_device.cpp`（**直接调用**）；
- **写端**：`CaptureBlendedPaletteSlotRange`（hook 抓取侧）；
- 另经全仓检索确认：`…Exact` / `…BestEffort` / `Validate…Exact` 在 `src` 内**无调用者**，其 AutoTest 出现仅为 `WARVK_DATA_SCOPE` 清单条目（§6）。

⇒ **就"函数指针 / 回调注册"这一类间接机制而言，(i) 在静态层面闭合**：三条关键函数的调用关系**全部是直接调用**。

### 仍然未覆盖（不得越界）

- **DLL 导出面**：这些函数位于内部命名空间，理论上不予导出，但**本轮未核对导出表**（未检查 `.def` / 导出符号清单）；
- **跨模块转发**：未做全仓调用图（本步是定向 grep，不是调用图分析）；
- ⇒ (i) 表述应为"**可静态枚举的间接机制中，函数指针已被排除；导出面未核**"，**不得**写成"(i) 已闭合"。

## 7.4 (i) **导出面也已闭合**：`d3d9.def` 不含这些内部函数

`src/d3d9/d3d9.def` 是本 DLL 的**权威导出清单**（共 46 行），其全部导出为：

- D3D9 入口：`Direct3DCreate9` / `Direct3DCreate9Ex` / `Direct3DShaderValidatorCreate9` / `PSGP*` / `D3DPERF_*` / `DebugSet*` / `Direct3D9EnableMaximizedWindowedModeShim`；
- 注解注册：`DXVK_RegisterAnnotation` / `DXVK_UnRegisterAnnotation`；
- 初始化：`Initialize` / `WarVK_Initialize`；
- **JAPI 视觉 API**：`WarVKVisualV1_*`（约 20 项）。

ripgrep 该文件对 `QueryBlendedPalette…` / `War3TryBuildLiveRuntimeGroupPalette` / `CaptureBlendedPalette…` ⇒ **命中 0**。

### ⚠️ 一条容易误读的观察（记录以免后续被曲解）

- 直接对 `build32/…/d3d9.dll` 做**字节子串**检索，这三个函数名**都能命中**（True）；
- **这不代表导出**：`WARVK_DATA_SCOPE(...)` 等插桩会把**作用域/函数名以字符串形式**编进二进制（这也正是 `data_collection_entrypoints.json` 能把"名字↔行号"对应起来的原因）；
- ⇒ **判断导出必须看 `.def`/导出表，不能用字符串存在性**。

### (i) 的最终表述（静态层面）

| 机制 | 状态 | 依据 |
| --- | --- | --- |
| 函数指针 / 回调注册 | **已排除** | `&fn` 命中 0（§7.3） |
| DLL 导出面 | **已排除** | `.def` 命中 0（本节） |
| 直接跨模块调用 | **已枚举** | 读端 `selection.cpp:512`；枢纽 5 处在 `device.cpp` |
| 其它模块内无调用者 | **已确认** | `…Exact` / `…BestEffort` / `Validate…Exact`（§6） |

⇒ **(i) 在静态可枚举范围内闭合**。仍未证明的只剩"本审计未设想到的机制"与**运行时行为**（后者属 (ii)，需实机）。**仍不声称 T3 整体闭合。**

## 6.1 ⚠️ **§6 的更正（同夜 09:36）**：`Validate…` **不是**无调用者

- `CopyBlendedPaletteBytesBySlotIndexExact`（`.cpp:9134`，声明 `.h:469`）**在 `:9146` 调用 `ValidateBlendedPaletteBySlotIndexExact`**，且该函数**被 `war3_current_draw_contract.cpp:2365` 真实调用** ⇒ 这是一条**存活的生产链**：
  `PublishCurrentDrawContract` → `CopyBlendedPaletteBytesBySlotIndexExact` → `ValidateBlendedPaletteBySlotIndexExact` → `PackWar3PaletteMatrix3x4`；
- 因此 §6 把 `ValidateBlendedPaletteBySlotIndexExact` 列为"无调用者"**有误**；**它属活代码**；
- **仍然无调用者的只剩**：`QueryBlendedPaletteBySlotIndexExact`（`:9110`）与 `QueryBlendedPaletteBySlotIndexBestEffort`（`:9166`）；
- 证据来源：`AutoTest/test_current_draw_trusted_palette_direct_pack_static.py`（39 行，断言 `Copy…` 体内 validate 先于 pack、且 publish 体内调用 `Copy…`）；
- **教训**：定向 grep 会漏掉中间层函数（本次即漏掉 `Copy…`）⇒ 任何"无调用者"结论都必须用**调用图或门禁反查**复核。
