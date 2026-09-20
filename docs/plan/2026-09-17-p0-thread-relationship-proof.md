# 2026-09-17 — P0 线程关系证明（Gap B / A5 非原子槽位缓存读的前提）

> 用途：回答“shadow-core palette 路径与 palette 写入 hook 是否同线程”。
> 上级（codex 01a02e0b）与 Gap B 设计 §9 都要求该证明作为 A5 帧证明成立的前提。
> **本文只做只读证明与方案，未改任何源码**（源码改动需上级裁定）。

## 2026-09-17 上级裁定与本次修订说明（置顶）

- **依据**：上级（codex `01a02e0b`）2026-09-17 03:05 裁定——未实机候选**不得**写
  「已观察到所有者线程唯一推进＋进度发布一致」；写者 / 调用链审计只能表述为
  「**未发现其他写入路径**」，**不是**运行时线程见证。
- **本次改了什么**（逐条锚点）：

| # | 位置 | 旧表述 | 处置 |
| --- | --- | --- | --- |
| T-1 | §0.1 / §2 末 | 「渲染线程消费：palette 路径在渲染线程，**A5 读安全**」「**这条路径是安全的**」 | **已收窄**：只能写「源码调用链上该路径与写者不并发」「该分支未发现其他写入路径」（**源码审计结论，不是运行时线程见证，也不是运行时安全结论**）；旧表述保留删除线 |
| T-2 | §1 线程清单表 | 逐条 file:line 的线程 / 触达判定 | **口径标注**：属**源码调用链审计**，结论上限为「**未发现其他写入路径**」；**不是**运行时线程见证，也不等于"生产生命周期已验证" |
| T-3 | §6 修复方案 B / §7 验收 | 建议在 drain 入口拒绝非渲染线程消费 | 方案本身不变（仍是**建议、未实施**）；但**不得**在实施前写成"已消除竞争" |

- **本次不改**：任何源码 / 测试 / `meson`、任何阈值、任何旧运行证据数值；本文仍是**只读证明与方案稿**。
- **仍成立的核心结论（未被推翻）**：off-thread drain 存在 ⇒ A5 的通过结论在
  `allowControlPlaneSemanticDrain` 生效时**不能作为证据**；修复落地前**不得**宣称「A5 前提（同线程）已成立」。
- **保留可追溯**：被驳回 / 收窄的旧表述一律保留 ~~删除线~~ 或加「已被上级驳回」标注，**不静默删除**。

## 0. 结论（先行）

1. **默认不是同线程保证，而是“取决于谁来消费语义构建”。**
   - 渲染线程消费（scene submit / EndFrame 小步推进）：~~palette 路径在渲染线程，A5 读安全~~ —— **已按上级 2026-09-17 03:05 收窄**：源码调用链上该路径与写者不并发（**源码所有者门加固**结论，措辞上限「**未发现其他写入路径**」），但**不是**运行时安全结论，**生产生命周期与运行时覆盖待验证**。
   - **控制面 drain 消费：palette 路径在命名管道的分离线程上执行**，与游戏线程的 palette 写入 hook 并发
     ⇒ A5 对槽位缓存 valid/frameTag 的普通读与写者构成**数据竞争**。
2. **该分支不是假设**：AutoTest MCP 实际以 allowControlPlaneSemanticDrain: True +
   semanticBuildDrainMaxChunks: 32 发起请求（见 §4）⇒ **AutoTest 路径会走 off-thread drain**。
3. ~~方向上 fail-closed（读到垃圾 tag 只会让 A5 拒绝并落替代路径）~~ —— **该推断已被上级驳回**：
   无锁读可能读到**碰巧满足等式的旧值**（于是 A5 通过，却用了错误的所有者数据），
   而且一旦发生 UB，任何结果都不再有保证。因此**只要 off-thread drain 可能发生，
   A5 的通过结论就完全不能作为证据**，不是「方向安全、只是不可信」。
   （上级原话：不能说「数据竞争只会导致拒绝，因此仍然安全」。）
   且会让“槽位新鲜度”结论不可信 ⇒ **修复前任何报告不得宣称“A5 前提（同线程）已成立”**。
4. 本次实机三轮**未触发**该分支（驱动未发送该标志，见 §5）；但这不改变结论，
   因为标准 AutoTest 路径会触发。

## 1. 线程清单（生产代码，逐条 file:line）

> **口径（2026-09-17 上级 03:05 裁定）**：本节是**源码调用链审计**，结论上限为
> 「**未发现其他写入路径**」；**不是**运行时线程见证，也不构成"不存在竞争"或"生产生命周期已验证"。

| 线程 | 位置 | 是否触达 palette 槽位缓存 |
| --- | --- | --- |
| 游戏主/渲染线程 | WC3 主循环（hook 挂在 Game.dll 函数上，由该线程调用） | **写者**：palette 绑定/槽位写入 |
| 早期加载引导线程 | src/d3d9/d3d9_main.cpp:153 ::CreateThread(..., WarVkDirectLoadBootstrapThread, ...) | 否（只做 DLL 直载引导） |
| **控制面管道线程（每连接一个，detach）** | war3/tools/war3_control_plane.cpp:5136 std::thread(HandlePipeClient, pipe).detach(); | **可读**：见 §3 的 drain 分支 |
| 点阴影 prepare worker | d3d9_war3_shadow.cpp:7118 m_pointShadowPrepareFuture = std::async(...) | **已核实：否**（worker 只消费拷贝出的 POD：paletteHashes/frameSerial/pose 计数 + draws，见 d3d9_war3_shadow.cpp:7103-7124；replayDraws 由 scope-exit wait guard 保护；不触碰 s_slotBlendedPaletteCache 或绑定表） |
| DXVK 内部线程 | dxvk_queue.h:218-219（submit/finish）、dxvk_presenter.cpp:933（frame thread）、dxvk_shader_cache.cpp:86（writer）、dxvk_pipemanager.cpp:93（workers）、dxvk_fence.cpp:90 | 否（DXVK 自有资源路径） |

生产代码中除上表外没有其它线程创建点；std::thread 的其余用法全部在测试里（war3/gpu_skin/tests/*）。

## 2. palette 路径与“谁来消费构建”

- A5 读点：QueryBlendedPaletteFrameTagRange（war3/model/war3_model_hook.cpp:9201-9238），
  循环内取 s_slotBlendedPaletteCache[slotIndex + i] 并读 entry.valid / entry.frameTag
  ——**无任何锁、无 TryCell、无原子**（§3 给出对照）。
- 该函数只被 A5 使用（war3_shadow_renderer_core.cpp 的 FindOrUpdatePaletteSlotCache 判定链），
  而该链在 TryBuildRuntimeGroupPalette 内，由 ShadowRendererCore::buildFrameChunk 逐记录调用。
- 构建的**请求方**可以是控制面（war3_shadow_runtime_bridge.cpp:5254 的 refreshSemanticFrameIfStale）；
  该分支**只发请求、不消费**，代码注释明确写了原因（war3_shadow_runtime_bridge.cpp:5313-5317）：
  > “control-plane 不能同步替 render thread 消费 semantic build；否则 pipe 请求会把
  >  buildFrameChunk 压到控制线程上……真正的消费必须发生在 scene submit / EndFrame 的
  >  render-thread 小步推进里”
  并调用 validationRuntime.requestLatestFrameBuild()（:5318）。~~**这条路径是安全的。**~~ —— **已按上级 2026-09-17 03:05 收窄**：只能写「该分支**未发现其他写入路径**、源码调用链上仅发请求不消费」（**源码审计结论，不是运行时线程见证**）。
- **但另有一条路径会消费**：DrainSemanticBuildFromControlPlaneIfAllowed
  （war3/tools/war3_control_plane.cpp:4452）在满足 CanDrainSemanticBuildFromControlPlane(snapshot, payload) 时
  调用 validationRuntime.drainPendingBuildForControlPlane(maxChunks, maxBudgetUs, recordCeiling)（:4474）
  ——**在调用者线程（即管道分离线程）上执行构建分块**。
  其门控是 allowControlPlaneSemanticDrain（:4348，payload 默认 false）或 IsHotSemanticBuildWaitPayload，
  另加游戏状态条件（:4440-4449）。

## 3. 危害分析

| 读点 | 是否有守卫 | 跨线程后果 |
| --- | --- | --- |
| QueryBlendedPaletteFrameTagRange（A5 逐槽帧） | **无** | 与写者并发 ⇒ 数据竞争；可能读到撕裂/陈旧 tag |
| QueryRenderablePartPaletteSnapshot（S0-S3 快照） | **有**：TryCell + busy/seqlock（设计 §4.2 已核） | 已 fail-closed，不会撕裂 |
| QueryCurrentPaletteFrameTag | 普通 SafeReadU32Fast（单 u32 读） | 良性（单字读） |
| QueryRenderablePartPaletteSlot（绑定） | 原子字段（groupCount 等为 std::atomic） | 良性 |

方向性（**已按上级驳回改写**）：A5 的最终判决是「三个 tag 必须相等且等于绑定帧」。
原稿写的「垃圾/撕裂值只会让等式失败 ⇒ 拒绝 ⇒ 安全」**不成立**：无锁读可能读到**恰好满足等式的旧值**，
此时 A5 会通过并供出不属于本对象的槽位；UB 之后更无任何保证。正确表述是：
⇒ 拒绝 ⇒ 落替代路径（fail-closed）。因此**不会**把错误矩阵当正确矩阵用；
但它使“帧内新鲜度”这一证据的**可信度**下降，并与“槽位所有权未证明”叠加。

## 4. 触发面（为什么不是理论风险）

标准工具链就会开：AutoTest/war3_autotest_mcp.py 在 4 处发起 command=get_shadow_runtime_summary 时带上
allowControlPlaneSemanticDrain: True 与 semanticBuildDrainMaxChunks: 32
（例：L8655/L8659/L8661、L8746/L8748、L9159/L9162、L9595 附近）。
⇒ 任何经该 MCP 的 AutoTest 取证/自检，其语义构建都由**管道线程**消费。
本项目既有大量 renderThreadId != ::GetCurrentThreadId() 检查（d3d9_device.cpp），说明
“渲染线程身份”是既有概念；palette 槽位缓存路径没有使用该检查。

## 5. 本次实机三轮的实际情形

- C:/Windows/Temp/warvk_p0/warvk_p0_phase_driver.py **未发送** allowControlPlaneSemanticDrain、
  requestSemanticFrameBuild、refreshSemanticFrameIfStale（三者在驱动里 0 命中）；它只读 get_shadow_runtime_summary。
  ⇒ **三轮的构建消费都在渲染线程**，故本轮数据没有产生该竞争的直接证据。
  **上级裁定修正**：只能写「该驱动未请求已发现的 drain 路径」，**不得**升级为「全进程无竞争、数据肯定未受影响」（C++ 数据竞争属 UB，不能保证只会失败）。
- round2 使用 M2 的 ENDFRAME_BUILD=1 ⇒ 构建确实发生过（frameSerial==manifest、publishRevisionLag=0），在渲染线程发生。

## 6. 修复方案（三选一，建议 B）

**A. 给逐槽读加与快照同源的守卫（最彻底，改动面中等）**：让 QueryBlendedPaletteFrameTagRange 复用
快照查询已有的 TryCell/busy 协议，或把 BlendedPaletteEntry::valid/frameTag 改为原子。
风险：写者持锁期间该查询会失败 ⇒ A5 拒绝增多（fail-closed，但改变拒绝分布，需重新解释实机细分计数）；属行为改变，需上级裁定。

**B. 禁止非渲染线程消费构建（最小、与既有纪律一致，建议）**：在 DrainSemanticBuildFromControlPlaneIfAllowed
入口增加渲染线程身份校验（本代码库已有 GetCurrentThreadId() 与 renderThreadId 的既有模式）：
非渲染线程直接**拒绝 drain**，只允许发请求（与 refreshSemanticFrameIfStale 分支同语义）；
附带把 AutoTest MCP 的 allowControlPlaneSemanticDrain 改为 false（或去掉），直到修复落地。
风险：控制面 drain 相关自检会变成“只请求不推进”，需相应调整判定口径；**不改变任何渲染/阴影语义**。

**C. 仅记录约束（最低限度，必须做）**：在 A5 设计与本文写清“前提仅在渲染线程消费构建时成立”，
禁止在 off-thread drain 开启时宣称 A5 帧证明成立；并加静态测试钉住“drain 门控 + 逐槽读无守卫”现状，任何变更须同步更新本文。

## 7. 验收方式

1. 修复 B 落地后：注入一次“管道线程调用 drain”的请求，断言构建**未被消费**（buildCurrentRecordIndex 不前进）且返回原因可见。
2. 或修复 A 落地后：宿主机并发单测（一线程写槽位、一线程查询区间）断言无撕裂（返回失败而非半个 tag）。
3. 无论选哪条：实机报告必须**分别声明**“构建消费线程”与“槽位所有权”两项是否已证明。

## 8. 明确不做 / 待验证

- 本文不改任何源码；不放松任何验收口径。
- 点阴影 prepare worker 是否触达 palette 槽位缓存：**已核实为否**（§1 表内已给证据）。
- drainPendingBuildForControlPlane 是否还读写其它与渲染线程共享的非原子状态：**待验证**（建议单独审计）。
