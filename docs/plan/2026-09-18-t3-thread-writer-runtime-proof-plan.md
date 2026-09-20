# 2026-09-18 — T3 构建线程 / 写者关系运行时证明方案

> **状态：未实施 / 待上级批准。**
> 本文是证据**方案**，不是实施记录：不改任何源码、不构建、不跑测试、不做 git 操作。
> 依据 `docs/plan/2026-09-18-relay-m2-t2t3-workorder.md` §3 产出；所有 file:line 均为
> **2026-09-18 当前快照**（B 树 `dxvk-v1.22-integration-20260914`，HEAD `ae89054`，
> 工作树含未提交改动），以函数名 / 唯一子串为锚，**行号会漂移**。
> 措辞上限按上级（codex `01a02e0b`）2026-09-17 03:05 裁定：未实机候选**不得**写
> 运行时肯定表述；写者 / 调用链审计只能表述为「**未发现其他写入路径**」。

## 1. 背景与现状

### 1.1 现状：只有源码级审计 + 静态判定，缺运行时见证

- 写者审计（`docs/plan/2026-09-17-thread-fix-implementation-design.md` §10.1）口径为
  「**未发现其他写入路径**」，且**只覆盖 `s_slotBlendedPaletteCache`**；
  其它与渲染线程共享的非原子状态**未审计**（冻结记录 §4 同口径）。
- 2026-09-17 03:05 上级裁定（见 `2026-09-17-p0-thread-relationship-proof.md` 置顶、
  `2026-09-17-candidate-freeze-record.md` C-1/C-2/C-3）：未实机不得写
  「已观察到所有者线程唯一推进＋进度发布一致」；统一措辞为
  「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。生产生命周期与运行时覆盖待验证。**」
- 「fail-closed 所以安全」**已被上级驳回**：无锁读可能读到**碰巧满足等式的旧值**
  （A5 通过却用了错误所有者数据）；C++ 数据竞争属 UB，一旦发生任何结果都不再有保证。

### 1.2 off-thread drain 分支不是理论风险

- 控制面 drain：`war3_control_plane.cpp` → `DrainSemanticBuildFromControlPlaneIfAllowed`
  （:4466）在 `AllowsControlPlaneSemanticDrain`（:4361-4364，`allowControlPlaneSemanticDrain`
  或 hot-wait payload）通过后调用
  `ShadowValidationRuntime::drainPendingBuildForControlPlane`
  （`war3_shadow_renderer_core.cpp:9641`），其循环内调 `ensureLatestFrameBuilt()`（:9659）——
  即在**命名管道分离线程**（`war3_control_plane.cpp:5150` `std::thread(HandlePipeClient, pipe).detach()`；
  线程文档 §1 记作 :5136，2026-09-18 快照已漂移）上走到构建推进边界。推进本体是 `m_core.buildFrameChunk`（:9755）。
  2026-09-17 线程文档 §2/§3 与工单 §4.2 已论证：该路径与游戏线程的 palette 写入 hook
  并发时，A5 对非原子槽位缓存的普通读构成 **C++ 数据竞争（UB）**。
- 触发面实证：`AutoTest/war3_autotest_mcp.py` **4 处**
  `get_shadow_runtime_summary` 携带 `"allowControlPlaneSemanticDrain": True`
  （:8659 / :8746 / :9159 / :9599）⇒ 标准 AutoTest 工具链会武装该分支。
- 当前快照的**拒绝面已落地**（线程修复方案 B 第一、二部分）：
  - 唯一消费权限检查 `ShadowValidationRuntime::consumePermissionGranted(bool)`
    （:9533-9543），规则只此一处，两处推进入口共用：
    `ensureLatestFrameBuilt()`（:9578，`directEntry=false`）与
    `ensureFrameBuiltForContract()`（:9687，`directEntry=true`）。
  - 所有者门实现：`war3_shadow_build_lifecycle.h` → `ShadowBuildLifecycle::consumeAllowed`
    （:33-50）：所有者未建立 / 非所有者一律拒绝推进，只保留安全请求。
  - 三个所有者门拒绝计数：
    `g_semanticBuildOffThreadRefusedCount`（:797）、
    `g_semanticBuildOwnerUnestablishedRefusedCount`（:800）、
    `g_semanticBuildDirectAdvanceRefusedCount`（:803）。
    （严格说只有第一个字面是 off-thread；三者合称所有者门拒绝计数。）
  - **按值进度快照**：`publishBuildProgressLocked`（:9545-9566）以
    `ShadowBuildProgressValues` 按值发布、无可变别名、（work, generation）成对守卫，
    旧代际 / Reset 后旧块无法回写（`war3_shadow_build_lifecycle.h` `*IfCurrent` 一族）。
- 三个拒绝计数**在当前快照均已接入导出链**（2026-09-18 逐项核读）：
  bridge summary（`war3_shadow_runtime_bridge.h:909-914` +
  `war3_shadow_runtime_bridge.cpp:6767-6771`）→ diagnostics hub
  （`war3_diagnostics_hub.h:549-551` + `.cpp:2051-2056` + JSON `:3274-3279`）→
  control plane JSON（`war3_control_plane.cpp:2337-2342`）→
  perf monitor JSON（`war3_perf_monitor.cpp:7718-7723`）。
  （注：`2026-09-17-candidate-freeze-record.md` §4 称后两个计数「尚未接入导出链」，与其
  §2 门禁行「各 7 处链路」表述不一致；以当前代码为准——三者均已导出。）

### 1.3 缺口：拒绝面 ≠ 正向见证

拒绝计数只能证明「非所有者推进**被拒绝过 / 未被观察到**」，不能证明
「**推进只发生在所有者线程**」这一正向命题——缺逐轮运行时样本。且按既有纪律，
在 `allowControlPlaneSemanticDrain` 生效的运行里，**A5 结论一律不采纳为证据**
（工单 §4.3-7、线程文档 §6-C）；该运行下 palette 路径「帧同源」判定不可信。

## 2. 待证命题

| 命题 | 内容 | 现状 |
| --- | --- | --- |
| **Q1** | 构建**消费线程**逐轮是谁（`buildFrameChunk` 每次实际推进所在线程） | 拒绝面已可证（§1.2 三计数全链导出）；**缺正向「推进只发生在所有者线程」的运行时样本** |
| **Q2** | palette 槽位缓存（`s_slotBlendedPaletteCache`）**全部写者的线程 id 集合** | 只有源码审计「未发现其他写入路径」；无运行时集合 |
| **Q3（措辞上限）** | 结论只写「**已观察到同线程 / 所有者线程唯一推进**」 | **永不**升级为「已建立内存序 / 已消除竞争 / 已证明无数据竞争」（上级 03:05 裁定 + 工单 §4.3-6） |

## 3. 候选证据手段对比

| # | 手段 | 覆盖命题 | 成本 / 改动 | 局限 |
| --- | --- | --- | --- | --- |
| M1 | **per-entry 原子写者戳**：`BlendedPaletteEntry`（`war3_model_hook.cpp:295-300`）增加 `std::atomic<uint32_t> writerThreadId`，65536 槽 × 4 B = **+256 KB**；条目写入集中在 `CaptureBlendedPaletteSlotRange` 的循环一处（:2400-2406），两个调用方（:2699 `CaptureRuntimeGroupPaletteBindings`、:8020 `Hook_RuntimeMatrixWrite`）共用 ⇒ **一个 store 点覆盖两条调用路径**（relaxed）；`ResetMapSession`（:10327）随既有清表点（:10337-10338）一并失效 | Q2（逐槽写者） | +256 KB 常驻；1 字段 + 1（-2）个 store 点 + Reset 清理 | 「最后写者」语义：同槽多线程往返写时只能看到最近一次；不改准入、不参与判定 |
| M2 | **最后写者 + 位掩码**：进程级 `std::atomic<uint32_t> lastWriterThreadId` + `std::atomic<uint64_t> writerThreadIdMask`（线程 id 到 bit 的映射表） | Q2（集合级） | 字节级内存 | 多线程往返写使「最后写者」**失真**；位掩码保留集合但失去逐槽归属；只能与 M1 二选一作主证据 |
| M3 | **消费线程记录**：对象级事件 wire 的 `bits[12]` **已具备**（`war3_palette_object_evidence_sink.cpp` → `EncodePaletteObjectEvent`，:88 `event.bits[12] = GetCurrentThreadId()`） | Q1（拒绝路径的消费线程） | 零改动（已落地） | 只在对象级事件（子门开、拒绝/替代/入队/绘制路径）携带；**A5 通过点本身无事件**，不能单独回答 Q1 |
| M4 | **推进点正向见证**：两处推进入口在 `consumePermissionGranted` 通过后记录当场线程 id（有界：逐帧最后推进者 + 推进线程 id 集合 / 原始列表） | Q1（正向） | 1 处有界记录（core） | 观察手段，只能保证「出现第三个线程即可见」（§6） |
| M5 | **写者 id 上 wire**（二选一，**涉 wire/label 登记，待上级裁定**，见 §8）：<br>（a）填 `bits[13]`——当前转换器**从不写** `bits[13]`（`Event event{}` 零初始化 ⇒ 恒 0；冻结 wire 注释 `[13]=0`，sink :19）；读方 `AutoTest/analyze_palette_object_evidence.py` 的 `RESERVED_BITS`（:228）含 13 且硬校验 `require(bits[index]==0)`（:385-386）⇒ 填它属 **wire 变更**，需登记；先例存在：版本 2 已把 `data[3]`（windowSegment，裁定⑧）与 `words32[15]`（identityProofKind，裁定⑦）从保留位转正（sink :78-79 / :90-92；analyzer :93-97、:314-332）。<br>（b）新 label `palette-writer/v1`（复用 `Kind::ShadowState`，**不改既有 wire**；同类先例 = 裁定 2 批准的 `palette-object/v1`） | Q2 可导出 / 可对拍 | （a）改读方 + wire 登记；（b）新 label 登记 + 读方新增 | 二选一涉登记表与 golden，**本方案不擅自选定** |
| M6 | **mismatch 专用计数**：`g_paletteSlotCacheThreadAffinityMismatchCount`——A5 **通过点**比对条目 `writerThreadId` 与 `GetCurrentThreadId()`，不等即 +1；与 `RejectedStale` **不同分母**，不得并入（上级 Q-C ③）；导出沿既有 7 处链先例（§1.2 链路清单；模板 `AutoTest/test_palette_slot_cache_counters_export_static.py`：访问器 → bridge 字段+填充 → hub 字段+填充+JSON → control plane JSON → perf monitor JSON） | Q1∩Q2（消费-写者关系） | 1 计数 + 7 处链 + 静态门禁 | mismatch==0 仍只是「未观察到异线程消费」 |
| M7 | **宿主并发单测**：一线程写槽位（带戳）、另一线程经同一查询路径读取，断言**见证字段记账正确**（戳值集合、掩码、mismatch 计数随场景增减） | 手段正确性 | 1 个测试目标 | **不冒充无撕裂证明**：单测验证的是见证机制记账，不证明生产路径无数据竞争 |
| M8 | **逐轮原始线程 id 列表**：实机报告逐轮列出写者集合、消费 / 推进线程集合、`GetMainLoopThreadId()`（`war3_hook_lifecycle.h:53` / `.cpp:3155`）原始值 | Q1/Q2/Q3 报告口径 | 报告口径，写入实机申请书 | 出现**第三个线程必须可见**；不得只报「相等 / 不等」结论 |

**推荐组合（待批准）**：M1（主）+ M4 + M6 + M7 + M8；M5 待裁定后择一；M2 仅在
上级否决 +256 KB 时作降级备选。

## 4. 最小改动集（待批准）

> 不改任何准入 / 判定语义；戳与计数**不参与** A0-A5 判定、不改变拒绝分布。
> 估计改动面：**2-3 个源文件 + 2 个测试 + 4 个导出口（对应 6 个文件）**。
> 常量 / 计数 / env 名为建议值，实施前一次性冻结。

1. **写者戳（M1）**：`BlendedPaletteEntry` +`std::atomic<uint32_t> writerThreadId`；
   store 点在 `CaptureBlendedPaletteSlotRange` 条目循环（relaxed）；
   `ResetMapSession` 清表点一并失效（与 `entry.valid=false` 同处）。
   - 热路径成本：`Hook_RuntimeMatrixWrite` 正常 13K-30K 次/帧、极端 50K-100K 次/帧
     （上限 `kMaxCallsPerFrame = 20000u`，:7960）；每次条目写一次 relaxed 原子 store，
     与既有 `entry.frameTag / writeSerial` 写同点；**µs 级成本须实测**（关闭 / 开启两组，
     K3 同类纪律），不得只声明理论量级。
2. **mismatch 计数 + 导出（M6）**：`g_paletteSlotCacheThreadAffinityMismatchCount` +
   A5 通过点比对 + 7 处导出链（§1.2 清单）+ 静态导出门禁（以
   `test_palette_slot_cache_counters_export_static.py` 为模板新增 / 扩展）。
3. **写者 id 上 wire（M5，二选一）**：填 `bits[13]`（wire 变更登记）**或**新 label
   `palette-writer/v1`（不改既有 wire）——**待上级裁定项**，见 §8。
4. **静态导出门禁 + 宿主记账测试（M7）**：钉死 7 处链、分母隔离（不得并入
   `RejectedStale`）、戳字段不参判定；宿主并发单测断言见证记账正确（不冒充无撕裂证明）。
5. **逐轮原始 id 列表口径（M8 + M4）**：写入实机申请书——每轮列写者 id 集合、
   推进 / 消费线程 id 集合、主循环线程 id 原始值；`allowControlPlaneSemanticDrain`
   武装的轮次必须单列。

### 4.1 与 T2 的记录点合并（建议）

T2（`docs/plan/2026-09-18-t2-slot-ownership-minimal-evidence-plan.md` §4.1）的
写者见证记录点与本方案 M1 的 store 点**同址**（`CaptureBlendedPaletteSlotRange`）。
建议合并为**一个**记录点设计（同一次写同时落 `writerThreadId` 戳与 T2 的有界 TLS 记录），
但两份证明的**判定口径独立**：所有权（T2）≠ 线程关系（T3）。合并与否属实施细节，
待上级裁定。

## 5. 验收标准

| 项 | 标准 |
| --- | --- |
| 正向通过 | 整轮实机 `mismatch == 0` **且**观测到的写者线程 id 集合 ⊆ {消费 / 推进线程 id 集合}；逐轮列**原始 id 列表**（不得只报结论） |
| 措辞上限 | 通过也只写「**已观察到同线程 / 所有者线程唯一推进**」；**永不**写「已建立内存序 / 已消除竞争 / 无数据竞争」 |
| drain 轮次 | `allowControlPlaneSemanticDrain` 生效的运行里，**A5 结论一律不采纳为证据**（既有纪律，本方案不改变）；该情形轮次单列 |
| mismatch ≠ 0 | A5 相关「帧同源」结论**立即 fail-visible 标记不成立**；**唯一可采纳**证据退回对象级原始观测记录（`palette-object/v1`，工单 §4.4）；后续修复另立候选（线程文档 §6 的 A/B），不在本方案 |
| 门禁 | 静态导出门禁 + 宿主记账测试 + 既有全量静态 / `meson test` / `ninja -n` no-work；实机轮次按上级 Q-D 先申请后跑 |

## 6. 不可证边界

1. **观察不能证明「不存在第三个线程」**：本方案只能保证「出现第三个线程即可见」
   （逐轮原始列表）；任何轮次之外的情形不可外推。
2. **`drainPendingBuildForControlPlane` 的其它共享非原子读写待单独审计**
   （线程文档 §8 遗留）：本方案只覆盖 palette 槽位缓存的写者与构建推进线程，
   不审计 drain 路径其余共享状态。
3. **32 位注入路径无 TSan**：数据竞争只能依靠本方案的见证字段与计数观察，
   没有工具级竞争检测兜底；宿主并发单测只验证见证记账正确（M7）。
4. **写者审计口径永远只是「未发现其他写入路径」**：源码审计（§1.1）不因本方案的
   运行时观察而升级为「已证明无其他写者」；未审计的共享非原子状态不因 mismatch==0
   而变安全。
5. **mismatch==0 ≠ 无竞争**：戳与比对本身是观察手段；「已观察到同线程」永不升级为
   「已消除竞争」（Q3）。
6. **拒绝计数增长不证明正确性**：三个所有者门拒绝计数只是拒绝面证据（不同分母，
   不得相加成「总拒绝」）；本方案的正向命题只由 M4/M6/M8 的运行时样本支持。

## 7. 与 T2 的关系

- **同址记录点**：T2 的写者见证（`writerIdentity` / `writerSource` / 区间）与本方案的
  写者线程戳都在 `CaptureBlendedPaletteSlotRange`；建议合并为一个记录点（§4.1）。
- **判定口径独立**：T2 回答「arena 字节属于谁」（所有权，五层 ③）；
  本方案回答「谁在写 / 谁在消费 / 是否同线程」（线程关系，A5 的**证据前提**）。
  任一者不通过不自动否决另一者的证据；但 **T2 记录的 A5 通过样本在本方案闭合前
  不得单独升级为证据**（T2 文档 §6.7）。
- 两者**共同阻塞**三项判定之判定 ①（「错误矩阵不再被使用」），见 T2 文档 §7。

## 8. 待上级裁定项

| # | 事项 | 选项 | 备注 |
| --- | --- | --- | --- |
| D1 | 写者 id 上 wire（M5） | （a）填 `bits[13]`：需 wire 变更登记 + 读方 `RESERVED_BITS` 收缩 + golden / 解析测试同步；版本 2 的 `data[3]` / `words32[15]` 转正为先例。<br>（b）新 label `palette-writer/v1`：不改既有 wire / 读方硬校验；需 label 登记 + 新读方 | 本方案不擅自选定；两者互斥 |
| D2 | M1（per-entry 戳，+256 KB）vs M2（最后写者 + 位掩码，降级备选） | 推荐 M1；M2 仅在上级否决内存时启用 | M2 的「最后写者」在多线程往返写下失真（§3） |
| D3 | 与 T2 合并为同一记录点 | 建议合并；判定口径仍独立 | §4.1 |
| D4 | 戳 / 计数是否常驻（无 env 门） | 建议常驻（不参与判定）；若上级要求诊断门则按 RecorderConfiguration 先例进入配置面 | 常驻意味着 +256 KB 与热路径 store 不可关；成本实测（§4-1）是批准前提 |

## 9. 明确不做（本方案范围外）

- 不改 `consumePermissionGranted` / `consumeAllowed` 的门规则；不新增 / 放松任何准入。
- 不把 A5 逐槽读改为原子 / 加锁（线程文档方案 A 属另一候选，改变拒绝分布需独立批准）。
- 不改 AutoTest MCP 的 `allowControlPlaneSemanticDrain` 现状（既有裁定：安全由边界门保证）。
- 不审计 drain 路径其它共享非原子状态（§6.2，另案）。
- 不构建、不部署、不启动游戏、不做 git 写操作；实机轮次须另行申请（上级 Q-D）。
- 永不把本方案结果写成「已建立内存序 / 已消除竞争 / 无数据竞争」（Q3）。

## 10. 交叉引用

- T2：`docs/plan/2026-09-18-t2-slot-ownership-minimal-evidence-plan.md`（同址记录点；所有权证明）。
- 工单：`docs/plan/2026-09-18-relay-m2-t2t3-workorder.md` §3。
- 线程关系证明（方案 A/B/C、03:05 裁定、fail-closed 驳回）：`docs/plan/2026-09-17-p0-thread-relationship-proof.md`。
- 对象级证据工单（§4.3 写者见证雏形、§4.4 mismatch 处置、裁定 2 的 label 先例）：`docs/plan/2026-09-17-p0-object-level-evidence-workorder.md`。
- 线程修复实施设计（修复 1/2 已落地、修复 3 未做）：`docs/plan/2026-09-17-thread-fix-implementation-design.md`。
- 措辞收窄 C-1/C-2/C-3 与拒绝面导出链：`docs/plan/2026-09-17-candidate-freeze-record.md`。
