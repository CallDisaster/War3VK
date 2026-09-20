# 2026-09-17 — 线程修复实施设计（B 方案 + 进度快照发布；待落地，未改源码）

> 依据：上级（codex 01a02e0b）四条裁定——
> ① 驳回「数据竞争只导致拒绝、因此仍安全」；② 线程修复优先 **B**，但只把 MCP 开关改 false 不够，
> `IsHotSemanticBuildWaitPayload` 仍能隐式放行 drain，**检查必须设在真正推进构建的边界并覆盖所有入口**；
> ③（见对象级工单）恢复定义须含实际提交与绘制；④ **B 还必须包含安全的进度快照发布**。

## 0. 已核实的现状（file:line，以函数名/唯一子串为锚）

1. **构建推进的唯一真实边界是 `ShadowValidationRuntime::ensureLatestFrameBuilt()`**：
   - `drainPendingBuildForControlPlane(...)`（core.cpp:9448）在 :9466 调 `ensureLatestFrameBuilt()`；
   - `runObserveValidation()`（:9685）在 :9686 调它；
   - 控制面侧有三处 `DrainSemanticBuildFromControlPlaneIfAllowed` 调用（control_plane.cpp:4521/4527/4539），
     门控为 `allowControlPlaneSemanticDrain || IsHotSemanticBuildWaitPayload`（:4356）。
2. **推进体在锁外执行**：`ensureLatestFrameBuilt` 用 `std::unique_lock` 完成状态准备后被 `}`（:9535）释放，
   随后在 :9548 调 `m_core.buildFrameChunk(...)`，并**在锁外**写
   `buildWork->nextRecordIndex`（:9548）、`chunkCount++`（:9553）、`totalBuildDurationUs +=`（:9554）。
3. **读侧在锁内读同一批字段**：`snapshot()`（:9722-9732，`shared_lock`）读
   `m_buildWork->stats`、`m_buildWork->frame.draws.size()`、`m_buildWork->totalBuildDurationUs`；
   `buildStateSnapshot()`（:9734-9756）读 `m_buildWork->nextRecordIndex/chunkCount/totalBuildDurationUs`。
   ⇒ **只给读者加锁无法保护不使用同一把锁的写者**（上级原话），这正是必须一并修的点。
4. **既有线程身份设施**：`GetMainLoopThreadId()`（war3_hook_lifecycle.h:53 / .cpp:3155，
   `std::atomic<DWORD> g_mainLoopThreadId`），既有同款判定先例：
   `war3_native_capture.cpp:174 !GetMainLoopThreadId() || GetCurrentThreadId() != GetMainLoopThreadId()`。

## 1. 修复 1：在推进边界拒绝非主循环线程（覆盖所有入口）

**改动点**：`ensureLatestFrameBuilt()` 开头（在 `requestLatestFrameBuild()` 之后、任何状态改动之前）：

```cpp
// 2026-09-17 上级裁定：构建推进只允许发生在主循环/渲染线程。
// 控制面 drain 在管道分离线程上调用本函数（drainPendingBuildForControlPlane -> 此处），
// 而 A5 读的槽位缓存是非原子、进度字段也在锁外更新 ⇒ 非主循环线程一律只请求、不推进。
const DWORD mainLoopThreadId = dxvk::war3::hooks::GetMainLoopThreadId();
const DWORD currentThreadId = ::GetCurrentThreadId();
if (mainLoopThreadId == 0u || currentThreadId != mainLoopThreadId) {
  g_semanticBuildOffThreadRefusedCount.fetch_add(1u, std::memory_order_relaxed);
  return;
}
```

- 该检查放在**推进边界**，因此同时覆盖：`allowControlPlaneSemanticDrain`、
  `IsHotSemanticBuildWaitPayload`（后者是 :4356 的 OR 分支）、以及任何未来新增的调用者。
- **MCP 开关改动不是修复**（上级明确）：仍建议把 `AutoTest/war3_autotest_mcp.py` 的 4 处
  `allowControlPlaneSemanticDrain: True` 改 false，作为**减少无效请求**的卫生措施，但安全由边界检查保证。
- 计数 `semanticBuildOffThreadRefusedCount` 走既有导出链（bridge/hub/control-plane/perf），
  **与 A5 的拒绝计数不同分母**（上级要求：线程见证不得与 RejectedStale 混算）。
- 边界语义：`ensureLatestFrameBuilt()` 原本兼有「请求最新帧」与「推进分块」两种作用；
  被拒时**只保留请求语义**（`requestLatestFrameBuild()` 已在最前面执行），因此不是 fail-open。

## 2. 修复 2：安全的进度快照发布（替代锁外改写 + 锁内读）

**问题**：见 §0.2/§0.3。三种可选做法与取舍：

| 方案 | 做法 | 取舍 |
| --- | --- | --- |
| A 全程持锁 | 把 `buildFrameChunk` 放进 `unique_lock` 作用域 | 会让控制面/诊断快照在每块构建期间阻塞（当前设计刻意避免，见 :9543-9545 注释）⇒ **不采用** |
| **B 进度发布（建议）** | 每块完成后在**短临界区**内发布一份**不可变进度快照**；读侧只读该快照 | 不改变构建并发性；写侧一致；读侧不再触碰 live 字段 |
| C 进度字段原子化 | `nextRecordIndex/chunkCount/totalBuildDurationUs` 改 `std::atomic<uint64_t>` | 三个标量可行，但 `frame.draws`/`stats` 仍需锁 ⇒ 只能部分解决，作为 B 的补充 |

**建议 B 的具体形态**：

```cpp
struct ShadowValidationBuildProgress {   // 只在锁内发布/读取
  uint64_t nextRecordIndex = 0u;
  uint64_t recordCount = 0u;
  uint64_t chunkCount = 0u;
  uint64_t totalBuildDurationUs = 0u;
  uint64_t drawCount = 0u;
  ShadowValidationFrameStats stats = {};
};
std::shared_ptr<const ShadowValidationBuildProgress> m_publishedBuildProgress;
```

- 构建侧：每完成一块（:9553 之后）用**一次短 `unique_lock`** 构造并发布新的 `m_publishedBuildProgress`
  （复制 `stats`、`frame.draws.size()`、三个标量），随后立刻释放——**不把锁持有到构建过程里**。
- 读侧：`snapshot()` 与 `buildStateSnapshot()` 改为在 `shared_lock` 内读 `m_publishedBuildProgress`，
  **不再访问** `m_buildWork->*` 的 live 字段。
- 终态：构建完成时（:9633 一带）发布最终进度，与 `m_lastStats`/`m_lastFrame` 的既有发布保持同序。
- 清理：`m_buildWork.reset()` 的各处（:9404/:9647/:9699）同时 `m_publishedBuildProgress.reset()`，
  避免读侧看到已废弃进度。

## 3. 修复 3：把「拒绝原因」呈现到控制面响应（可选但建议）

- `CanDrainSemanticBuildFromControlPlane` 增加返回原因（`offThreadRefused` / `notInGame` / …），
  使控制面响应能区分「不允许 drain」与「drain 了但没进展」；
  否则调用者会把「线程拒绝」误读为「没有待构建内容」。

## 4. 验收（必须逐条可执行）

1. **线程拒绝可见**：经管道以 `allowControlPlaneSemanticDrain: true` 请求一次 summary，
   断言（a）`semanticBuildOffThreadRefusedCount` +1、（b）`buildCurrentRecordIndex` **不前进**、
   （c）响应里出现拒绝原因。
2. **隐式入口覆盖**：用 `IsHotSemanticBuildWaitPayload` 能命中的请求形态重复第 1 条（不得只测显式开关）。
3. **进度发布无竞争**：宿主机并发测试（一个线程反复调 `snapshot()`/`buildStateSnapshot()`，
   另一个线程反复推进分块）断言读数单调不回退、`drawCount` 不出现撕裂（如 `size()` 与元素一致），
   并在 TSAN 下（若可用）无报告。
4. **回归**：`ninja -C build32 -j4` exit 0、`ninja -n` no work、meson 全过、静态全量通过。
5. **口径**：即便全部通过，报告只能写「已观察到主循环线程唯一推进 + 进度发布一致」，
   **不得**写「已建立内存序 / 已消除所有竞争」（上级对线程证明的措辞要求同样适用）。

## 5. 与实机的关系

- 本修复**不改变渲染/阴影语义**（只拒绝非主循环线程推进 + 改变读侧数据来源）。
- 落地后：A5 的帧证明前提才第一次成立；但仍然**不证明槽位所有权**（对象级工单 §3 的两条路线）。
- 因此实机申请里的「（i）构建消费线程」一项，只有在修复落地后才可能写成「已观察到同线程」。

## 6. 明确不做

- 不在此修复里放宽/收紧任何 palette 判定；不引入负缓存；
- 不把 `IsHotSemanticBuildWaitPayload` 删掉（只让它无法绕过边界检查）；
- 不宣称线程问题闭合——按上级要求，B 必须与进度发布一起完成才算闭合。

---

## 9. 实施状态（2026-09-17 更新，含上级追加要求）

| 上级要求 | 落实 |
| --- | --- |
| 所有入口必须经过线程身份检查；**直接调用底层方法也不能绕过** | 门设在**两处**：`ensureLatestFrameBuilt()`（入口）与 `ensureFrameBuiltForContract()`（真正的推进入口）；后者被拒时单独计数 `g_semanticBuildDirectAdvanceRefusedCount` |
| **所有者未建立时只能排队/明确拒绝，不能消费** | 原稿「id==0 放行并计数」**已被推翻**：现改为 `OwnerUnestablished` ⇒ 计数并 return（不推进） |
| 所有者身份**不能由「第一个请求线程」认领** | 判定用 hook 生命周期的 `GetMainLoopThreadId()`（既有设施，`war3_native_capture.cpp:174` 同款先例）；不是首个调用者 |
| 同时修复进度快照的并发读写 | **已实现**：新增 `ShadowValidationBuildProgress` 不可变摘要 + `publishBuildProgressLocked()`（短 `unique_lock`，每块发布一次）+ 各 `m_buildWork.reset()` 处同步清空；`snapshot()`/`buildStateSnapshot()` 只读该摘要，函数体内**不再触达 `m_buildWork`**。未把锁持进 `buildFrameChunk` |
| 补实际生产边界测试 | 判定函数抽为轻量头 `war3_shadow_build_thread_gate.h`（inline 纯函数），新增宿主机边界测试覆盖：所有者未建立、正常所有者推进、管道线程 drain、隐式 hot-wait、直接调用底层入口、未知/错误所有者、Reset 后未重建 |
| 读写者关系仍须核清（封住控制面消费 ≠ 证明所有 palette writer 同线程） | **待办**：见 §10 的写者审计；单独把 `valid/frameTag` 改原子不足以证明整段一致 |

措辞修正（上级明确）：
- 旧三轮只能写「该驱动未请求已发现的 drain 路径」，**不得**升级为「全进程无竞争/数据未受影响」。
- 即便本修复全部落地，措辞上限仍是「已观察到主循环线程唯一推进 + 进度发布一致」，
  **不得**写「已建立内存序 / 已消除所有竞争」。

## 10. 待办：palette 槽位缓存写者审计（上级要求核清）

需枚举 `s_slotBlendedPaletteCache` 的**全部写入者**及其线程，回答「是否所有 writer 都在同一所有者线程」；
若存在其它写者，B 方案尚未闭合，需要共同的同步协议（而不是只把 `valid/frameTag` 改原子）。
审计结果写入本文或独立文档，并在实机报告里作为「（i）构建消费线程」的先决条件之一。

## 11. 构建纪律（上级要求）

- 后续构建：**Below Normal 优先级、exact DLL 目标、最多 `-j2`**；
  本轮此前发生的 `-j4` 已如实记录，不作为后续扩大并行度的依据。

### 10.1 审计结果（2026-09-17 主线程执行，只读，全部集中在 war3_model_hook.cpp）

缓存本体（声明见 `war3_model_hook.cpp:286-305`）：`static std::array<BlendedPaletteEntry, 65536>`，
成员 `matrix/frameTag/writeSerial/valid`，注释自述「查询和写入都是 O(1) 数组访问，**没有锁和分配**」。

| 写入者 | 位置 | 触发来源（线程） |
| --- | --- | --- |
| `CaptureBlendedPaletteSlotRange` 逐槽写入 `frameTag/writeSerial/valid/matrix` | `war3_model_hook.cpp:2400-2406` | 由 `:2699`（绑定捕获）与 `:8020`（运行时矩阵写入 hook，`nodePtr/destMatrixPtr` 路径）调用 ⇒ Hook 侧，即 **主循环/渲染所有者线程** |
| `ResetMapSession()` 批量 `entry.valid = false` | `:10337-10338` | Present 安全点的地图切换 ⇒ **渲染所有者线程** |
| 静态初始化 `= {}` | `:303-304` | 进程加载 ⇒ 无并发 |

**结论**：grep 全树（`s_slotBlendedPaletteCache` 共 9 处命中，全部在本文件）**未发现**来自控制面管道线程、
DXVK 工作线程或点阴影 worker 的写入者；已知写者均属主循环/Present 所有者线程族。
因此就**该缓存**而言，B 方案的写者前提在代码结构上是成立的——但按上级口径，
措辞仍只能写「已观察到写者均属所有者线程族」，不得写「已消除竞争」。

**顺带发现（对对象级证据有用）**：写入时已维护单调写序号 `s_slotBlendedPaletteWriteSerial`
（`:2397` 为每槽分配 `baseSerial + i`，并写入 `entry.writeSerial`），但**全树没有任何读者消费 `writeSerial`**
（grep 仅命中声明与写入两处）。这意味着「读前后校验写序号是否稳定」是一个**已存在但未被使用**的廉价一致性检查，
可作为对象级路线 B 的「可用 write serial」基础（上级在对象级裁定中也要求记录可用 write serial）。
注意：它只能检测「读期间是否发生过写入」，**不能**证明矩阵与整段数据一致，也不能替代所有权证明。
