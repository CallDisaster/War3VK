# 2026-09-12：从性能日志大头出发的源码复审

## 结论与证据边界

本轮是只读日志/源码审计及离线记录，不是优化实现、性能接受或稳定版晋升。
优先顺序是：原生调用边界归因 → ShadowCapture/PostGate 分解 → Populate 剩余工作与观测成本；
GPU 优化放在 CPU 归因之后。没有运行 Ninja、构建、runnable、游戏、部署或 GPU 验证。

当前主工作树 `dxvk` 为 `codex/native-shadow-stable-baseline-20260830`，HEAD
`88089cdf90f728e85b91bf45d75002348574b665`。开工 status 为 268 项，包含用户已有的
258 个 AutoTest tracked 删除；它不是最新性能日志的源码版本，不得用它直接归因该日志。
本轮只在主树新增本报告并追加 DEVELOPMENT_CHANGELOG；不恢复删除、不改变产品源码。

日志对应的只读参考树为同级 `dxvk-native-shadow-stage13-current-20260830`，HEAD
`a72c70058accd971ebe9cb6a09f7f7ec2f6b3512`。以下源码行号均属于这个参考树，除非另有说明。
其 `d3d9_device.cpp`、`d3d9_war3_shadow.cpp`、`war3_hook_render.cpp`、`war3_renderer.cpp` 与
`war3_shadow_generation_backed_stream.h` 的 working blob 均已核为对应 HEAD blob。
两树存在大量差异，不能跨树套用 hunk 或把参考树缺陷宣称为主树当前默认行为。

## 输入身份

- 检索的日志目录为 `E:/Work/War3/WarVK/Log` 和 `E:/Work/Warcraft III/WarVK/Log`；
  前者最新可用性能报告仍为 8 月 30 日，后者止于 8 月 15 日。没有据此声称扫描了整台机器。
- 本次主输入：`E:/Work/War3/WarVK/Log/war3_perf_report_auto_2026_08_30_21_03_25.html`。
  3,685,319 bytes，SHA-256
  `FD3B36DE9F91C0760E965EB1BD15B86C511E637C587EAD116E41C896E8086F81`。
- 报告 DLL 与参考树 build32 DLL：35,954,960 bytes，SHA-256
  `5C1A9E4AFA80C244E45CB4717E446E569660799F6397EE0BD16A4E065A723E4E`。
- 参考树 transaction：`AutoTest/artifacts/native_shadow_stage13_general_integrity_current_5c1a9e4a_v2_20260830.json`，
  798,420 bytes，SHA-256 `F38D05945EE91EAA8E952A7B8A36000CD632B6AD092137DCCE26F37D98B68217`。
- 对应 analysis：15,446 bytes，SHA-256
  `E78FF76970B00113D57E26939F71AA3DA57002506F2E766D06E2DCC99E0B00D5`。
  general-integrity 通过，但 `performanceClaimAuthorized=false`，产品接受未获授权。

报告为 4000 帧、129.185 秒报告窗口、full_default。平均帧时 31.896 ms，主线程 OS CPU
24.219 ms，GPU union 3.461 ms。worker CPU 合计 10.566 ms 是并行工作，不可加到主线程或帧时上。
这些是隔离诊断现场的数据，不是玩家前台 FPS，也不是开始优化以来的同条件收益比较。

## 热点排序：计时域不能混加

| 路径/边界 | 平均 ms | 解释及优先级 |
| --- | ---: | --- |
| Hook_WorldFramePrepare/NativeOriginal | 8.165 | 第一归因目标；尚未细分的原生调用边界 |
| Hook_WorldRenderScene/.../Hook_FlushSortedItems/NativeOriginal | 7.073 | 第二归因目标；函数名不证明全部耗时来自排序 |
| ShadowCapture/PostGate | 2.473 | 自有捕获链首要目标；占 ShadowCapture 4.319 ms 的 57.26% |
| ShadowCapture/Gates | 1.845 | 其中 DrawTimeCapture 0.787 ms；与 PostGate 同属捕获覆盖计时 |
| 根 Hook_FlushAndReset/NativeOriginal/Populate | 1.290 | self 0.761；DirectGrouped 0.529 |
| DirectGrouped/SnapshotPreselect、BuildEligible | 0.282 / 0.118 | 本次排序低于 Populate 未细分的 self |
| ShadowReceiver GPU leaf / ShadowMap GPU leaf | 1.804 / 1.109 | GPU 内部优先观察 receiver；不能与其父计时再累加 |

表中的排序队列完整路径是
`Hook_WorldRenderScene/NativeOriginal/Hook_FlushAndReset/NativeOriginal/Hook_FlushSortedItems/NativeOriginal`。
ShadowCapture 是独立覆盖计时，不能加到原生调用树形成“总开销”。原生 trampoline 内仍可能回调
WarVK/D3D9，因此前两个边界合计 15.238 ms 也不能直接称为“纯 Game.dll CPU”或全部可消除成本。
报告 `detailEnabled=false`，PERF_LEVEL=1，capture/gate/drawtime/shadow-phase 细分开关均关闭；
没有有效的 PostGate ResourceResolve/FreezeBuffers 子叶可供本轮归因。

root aggregate 为 22.399 ms，显示的 root 行合计 22.400 ms、self 行合计 22.391 ms。
三位小数累计误差可能参与这 0.009 ms 差额，但本轮没有放宽 ownership 合同或宣称精确闭合。
未覆盖 wall 9.497 ms 不是可直接归类的 CPU 热点；不能用它推导某个模块成本。

## 源码发现与后续候选

### 1. 先使用现有细分入口，拆开最大的原生调用边界

`war3_hook_render.cpp:2558` 的 WorldFramePrepare wrapper 与 `:4723` 的
CallOriginalFlushSortedItems 目前只能界定 trampoline 时间，不能证明内部真正的热点。
源码已具备 WorldPrepare 的 CameraBuildFrustum、TerrainShadowFlush、TerrainExtraPass、
ShadowProjectorFlush，以及 residual/core 的 FrameUpdateGate、UIFrameSync、CameraAdvance 等细分入口。
开关判断位于 `:2160/:2177/:2194`，细分 wrapper 从 `:3195` 起，安装从 `:5340` 起。

后续独立诊断可使用已有 `DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS`、RESIDUAL_HOOKS、CORE_HOOKS
及满足构建条件的 PERF_LEVEL>=2；不是现在就增加更多 observer，也不是按函数名跳过原生调用。
诊断开关会增加开销，细分结果只用于归因，收益必须另以同条件 stats-off 基线验证。

### 2. PostGate 是首要自有优化区域，但不能直接归咎于 hash/copy

`d3d9_device.cpp:52062` 起有 ResourceResolve，`:56106` 起有 FreezePlan，`:56696` 起是
SnapshotCopy，`:56721` 起是 commit，`:56747` 起是 CopyPublish/CommandEnqueue。
当前日志只能确认整个 PostGate 2.473 ms，不能把它全分配给其中任一阶段。

后续先取得 phase 时间、调用密度、copy bytes、命中/拒绝原因，再决定是否减少重复的资源解析、
内容证明或冻结工作。StableSource 先 CPU memcpy 到 snapshot、再 CS copyBuffer 是当前生命周期
保护的一部分，不是仅凭“复制两次”就能删掉的冗余；异步 CS 不得捕获临时裸指针。
Arena 平均约 5.011 MiB、峰值约 8.343 MiB且 overflow 为零，不支持先扩容作为主要优化。
不得复活已判退的 missing-only、exact-N、PreparedKey/hash hand-off 或负缓存路线。

### 3. 已确认的诊断缺陷：S1 generation 观测与 GC 混用两种时钟

参考树 `d3d9_device.cpp:53179` 使用
`AdvanceWar3ShadowGenerationObservationClock(..., sourceFrame+1)` 得到 observationFrame，
随后 `:53187` 将其写入 `lastSeenFrame`。该 helper（`war3_shadow_generation_backed_stream.h:130`）
仅在一次实际调用的 sourceFrame 变化时递增；没有 eligible observation 的源帧不会推进这个时钟。
但 GC（`d3d9_device.cpp:23595`）却用原始 `m_war3ShadowPersistentFrameSerial - lastSeenFrame`
判断过期。两者不是同一个时间域。

纯 Python 算术反例：源帧 5001/5002 的两次观测得到 observationFrame=1/2；GC 在源帧 5100、
maxAge=3600 时，真实 age=98，不应过期；当前混合时钟 age=5098，却会删除该条目。
这证明可能错误清理新观测，不证明它造成了本次报告中的所有容量拒绝，也不证明任何 FPS 收益。

这条路径受 `kDevelopmentShadowObserversEnabled` 限制，普通 Release 已编译移除。
主工作树旧版本也没有这套 S1 观测代码，因此问题只归属于本次参考候选，不可扩大成 Release 缺陷。
候选修复方向是将 GC 的 source-frame 年龄与 stability 的 observation-frame 连续性分开存储，
补无观测间隔、长菜单、reset、溢出等模型测试；本轮未实现修复或改变观测合同。

报告累计 eligible=305478、capacityReject=182112、First=122853、Changed=513、
Advanced/SameFrame/PromotionReady=0；表容量 16384。该表当前没有产品消费（`:53218`）。
这是 Dev 观测成本和观测有效性问题，优先修复计数可信度，不应扩大容量来制造收益。
这些累计值的 diagnosticsFramesObserved=5583，与报告 4000 帧不是同一计数窗口，不能直接除以4000。

### 4. Populate：已有优化必须保留，优先拆 self，而非重复做 BuildEligible 微优化

`d3d9_device.cpp:29306` 的非 Dev、非 FullTrace 分支已在 snapshot 前执行 canonical winner
过滤；`:29344` 的 directRecords 已使用 TLS vector 复用。Dev/full-trace 刻意保留更完整的记录。
因此当前 Dev SnapshotPreselect 0.282 ms 不可当作 Release 必须承担的相同成本，也不能重复建议
“先过滤再复制”作为全新优化。Populate self 0.761 ms 大于 BuildEligible 0.118 ms，下一步应先
把 Populate residual 工作细分，而非继续围绕一个较小子叶迭代。

### 5. CS/GPU：仅保留有测量入口的后续假设

`d3d9_war3_shadow.cpp:11266` 已一次构建/解析 replay，并共享给 directional/volume/point 消费者；
`:11275` 的作用域保护确保 point worker 在本地 storage 销毁前退出。CSM 已有 pipeline、descriptor、
VB 和 lifetime 去重，不建议把这些现有能力重新列为待实现收益。

仍可审计 `:11267` 两个局部 vector 与 `:1658` 的 clear/reserve/解析路径，测量是否值得只复用
allocation/scratch。不能跨帧保留资源有效性判断或 Rc，必须保持 worker-drain、重入、epoch 与 owner 边界。
此项尚无独立计时，优先级低于 PostGate；进一步 instancing/indirect 也只能在同几何/材质和实际
draw 密度证明后立项。本报告不把这些假设写成已确认收益。

## 本轮验证与下一阶段

- 以 Python 标准库对 raw `const data` 唯一根对象作递归 duplicate-key 拒绝解析；主输入通过。
  抽查的 13 份近期报告中只有 3 份语法通过，其余 10 份因重复 canonical-ready key 被拒绝。
  语法通过不等于事务有效：较早同 DLL 报告仍受其 invalid attribution 结论约束；不同候选报告
  也不能作为同条件 A/B。旧 invalid 证据没有重新接受、改写或删除。
- 已运行上述 S1 混合时钟算术反例；仅为源码模型，不是 C++ runnable 或实机复现。
- 尝试导入现有 ownership analyzer 时，Python 3.13 的 MCP 缺少 `mcp.server.fastmcp`；
  Python 3.11 无 MCP。未安装依赖、修改 analyzer 或绕过其检查，故 ownership analyzer **未通过运行**。
- 未执行产品测试、Ninja/dry-run、编译、部署或实机；历史测试数量不作为本轮新验证。
- 下一阶段先明确实现所在源码树，再单独安排同身份诊断矩阵：现有 native 细分 + PostGate 细分，
  独立确认 ReShade/第三方注入状态；诊断与 stats-off 性能运行分开，禁止混入冻结事务。
  仅在大头归因后选一个产品改动，用调用密度、完整性、同条件 ABBA、恢复与玩家视觉门验证。
  本报告不授予任何新的构建、实机、部署或稳定接受权限。
