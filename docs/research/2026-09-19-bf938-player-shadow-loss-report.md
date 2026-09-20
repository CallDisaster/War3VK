# BF938 玩家回归：全场景阴影消失、移动后恢复并再次消失

日期：2026-09-19。性质：玩家报告与当前源码的离线调查；不是新修复、构建或实机验收。

## 1. 结论与验收裁定

**BF938 的 P0/P1a 首批候选没有通过压力下阴影持续显示/恢复门，不得合并、发布或称问题已修好。**

玩家明确补充：“全场景阴影消失，我移动了一会镜头之后阴影回复，然后去到某些场地又会消失。”
因此本轮不是“永远无法恢复”的证明，而是重复进入资源压力坏态、短暂恢复、再度丢失。
画面描述来自玩家；HTML 的渲染序列与计数提供独立的时序支持，不冒充像素审查。

本轮证据把直接触发机制收敛为：Stage11 快照池容量拒绝持续发生，必需输入不完整，
完整性门拒绝更新整个方向阴影图，旧图有限保留窗口耗尽后失效。
**仍未证明具体哪组对象/切片持有了多少页，不能把页内死区、静态驻留或重复大范围副本的占比编成实测结论。**

## 2. 输入、身份与离线复核

- 输入目录：`E:/Work/Warcraft III/WarVK/Log/`。
- 主报告：`war3_perf_report_2026_09_19_20_56_59.html`，3,547,076 B，SHA-256
  `FD04C62DDCA33FC691656E693C04715980BA4818E914FFB1050320A2750A6A96`。
- 另读同次录制的 8 份自动报告（20:54:30 至 20:56:50，每 20 秒一份）。
- 九份 meta 均绑定 36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`、
  schema 9、`full_default`。不是旧 DLL 误测。
- 严格递归 duplicate-key/nonfinite/root 解析通过。原报告不改，不用宽松解析器追认旧无效报告。
- 33,243 原始 workload 行减去 18,029 重叠行，得到连续 epoch **1..15,214**；
  重叠 workload 和 frameWallMs 完全一致，无冲突/缺口。菜单引导到世界帧期间主线程权威可变化，
  不把 mainThreadId 当进程会话 ID；连续序号、完全一致的重叠内容和累计计数提供本次拼接依据。
- 九份原文件 size/SHA 与完整回执在
  `AutoTest/artifacts/player-bf938-205659-20260919/summary.json`；去重序列在同目录
  `deduplicated-series.json`。`analyze.py` 生成，`verify.py` 独立重读原始报告复核 **59 项通过**，
  `verification.json` 留证。输出采用 CreateNew，原文件前后哈希不变。

元数据未绑定地图 SHA、实测分辨率、全部环境开关，故本次不外推为严格同场景性能 AB。
最后 4000 帧约 104 FPS 不能用来宣称收益：缺阴影本身已减少渲染工作。

## 3. 去重后的时序

下表以 perf epoch 为帧号。时间为对应帧的 frameWallMs 累加，不是根据文件名反推的绝对时间。
不把 serial=0 的缺失/占位样本擅自补成有效渲染事件。

| 帧区间 | 实测现象 | 时长/限制 |
| --- | --- | --- |
| 7043..14219 | 非零 shadowMapRenderSerial 始终 2837；doodad/S1 prepared 全 0 | 累计 76.895273 s；其中 8 行 serial=0，故不称每行均有有效 serial |
| **9110..14219** | **5110 行全部 serial=2837，prepared 全 0；捕获与 receiver 每行仍有活动** | **52.270132 s，无 serial=0 样本** |
| 14220..14346 | serial 2838→2964，prepared 恢复 | 127 帧；中间 14 行 serial=0，不逐行推定像素状态 |
| 14347..14612 | serial=2964，prepared 全 0 | 266 帧 / 2.558848 s |
| 14613..14905 | serial 2965→3257，prepared 恢复 | 293 帧；中间 21 行 serial=0 |
| 14906..15214 | serial=3257，prepared 全 0，持续到录制末尾 | 309 帧 / 2.970510 s |

52.27 秒的完整坏态区间内，doodad capture accepted 每帧 129..400、replayCasterCount 215..528、
semanticSceneSubmitted 3..27，shadowTaaReceiverExecuted 每行为 1。
这些只证明仍有捕获、输入列表与接收端活动，**不证明这些 caster 被画进有效阴影图**。
renderShadowMap 的完整性检查在 prepared 计数循环之前；prepared=0 是早退的下游结果，
不能据此断言“静态世界几何从未进入 replayDraws”。

本轮后段确实新增了 420 个渲染序列号（2837→3257），与玩家“移动后恢复、再到某处消失”一致。
本报告的 camera delta 仪表全零不能用来否定玩家移动相机，未证明该观察器本轮已启用。

## 4. 容量压力有实据，但不能称物理显存 OOM

以下是录制累计值，**不是最后 4000 帧专属，不得把九份重叠报告累计值相加**。

| 项 | 最后报告 |
| --- | ---: |
| 快照池 resident 有效发布值 | 384 MiB |
| drawTimeSnapshotPageCapacityRejectCount | 353,592 |
| producerAllocationFailureCount | 353,592 |
| drawTimeSnapshotPageAllocationFailureCount | 0 |
| framesProducerIncomplete | 7,841 |
| producerRequiredCasterOmissionCount | 286,043 |
| semanticSceneReceiverNoCompleteShadowMapCount | 7,794 |
| page create / reclaimed 累计见证 | 55 / 31 |
| indexed unknown-range fallback 次数 | 827,956 |

20:55:30、20:55:50、20:56:10、20:56:30 四份报告中，成功 suballocation bytes 累计值
**恒为 863,311,872 B**；容量拒绝却从 878 增至 224,855（新增 223,977 次）。
即这 60 秒报告间隔没有成功申请新的快照切片；已有 backing 的复用仍可能继续，不能写“所有捕获都停止”。
20:56:50 累计成功分配重新增加到 863,838,464 B，末报告为 895,889,408 B。

容量检查在 GPU `createBuffer` 前面，命中的是插件为 Stage11SnapshotPage 设置的预算。
本轮没有记录到 page GPU allocation failure；这不是对系统 GPU incident/dump 的全面检查，
也不是“显卡只有 384 MiB 可用”的结论。Stage11 池不是 ShadowArena，更不是当前视野模型的净有效字节量。

## 5. 当前代码解释与诊断字段陷阱

### 5.1 整页保留、尾部追加

`src/d3d9/d3d9_device.cpp` 的 `War3AllocateStage11Snapshot`（约 23686）以 256B 对齐，
遍历所有页，在各页 `used` 尾部追加；不能在页内回收已失去用途的切片空洞。
`War3CollectUnusedStage11SnapshotPages`（约 23822）仅在 Page `shared_ptr` 的 pool 引用为唯一引用时
解除整页。一个小切片仍被缓存 entry 持有，就足以使整页继续计入 resident。

这是“可见模型不多但池满”的可行结构性机制，不是本轮死区占比的测量。
回收 31 页说明并非 GC 永远不执行。Present 中业务帧序号仍递增（约 33734），
cache GC 每 60 帧检查，动态成功寿命 16 帧；静态工作集保护及 inactive 64MiB 目标是另一层策略，
不能将其误解成快照池总 resident 上限。现有日志不足以精确区分静态、动态、失败重试、retired 持有者。

Page 无 CPU owner 不等价于 GPU 可立即覆盖；后续优化必须保留 DXVK backing/pin/CS/GPU last-use 权限。

### 5.2 范围放大仍走正式路径

同文件约 42241：无法安全取得索引范围时保留完整有界 VB，而不是信任未证明的 Min/Num。
本轮该分支累计 827,956 次；这证明尚未闭合 P2，不等于每次都新分配或每次都复制 512KiB。
旧 F2A7 输入中“52 顶点/1664B 却对应 512KiB 源域”的实例仍只能作为旧样本，不能套成本次每个 draw。

### 5.3 全屏丢影而非少数对象省略

`src/d3d9/d3d9_war3_shadow.cpp`：

- `validateShadowProducerCompleteness`（约 3527）拒绝必需输入缺失的候选；
- `renderShadowMap`（约 3862）在 clear/draw、prepared 统计之前因失败返回；
- 成功完整图补充 8 帧恢复额度（约 11199）；拒绝候选耗尽额度后清除 completeness（约 11227..11258）。

因此少量乃至持续的输入分配失败也可以触发**整个方向阴影图不可用**，包括看起来仍在 replay 列表里的动态单位。
不应通过放宽完整性门、把缺 caster 的图称完整、无限沿用陈旧图或关闭 caster 来“修复”。
当前计数支持该直接链，但无逐页事件，不足以把所有坏帧的每一次具体失败与对象逐一绑定。

### 5.4 两类字段不能当即时状态

`war3_perf_monitor.cpp` 约 2328 对 semanticSceneReceiverHasCompleteShadowMap 做 **max**。
报告为 1 仅表示聚合期间曾经有过完整图，不表示坏态帧仍有完整图；
同类 receiver usable 标志亦应核查聚合语义，不能拼成当前帧状态。

resident/reclaimed 是 collect 或 allocation 时发布的 gauge；used 只在成功分配后写。
帧 stats 重置后若未在当帧采样，Last 可能为 0。
本轮 UsedBytesLast=0、个别报告 ResidentBytesLast=0 **均不是池空或资源已释放的证明**。
P1b 应统一在所有者安全点发布 sampleFrame/valid 与互相一致的账户，不用 Max 冒充 Last。

## 6. 下一步范围与发布阻塞

沿既有 P1b/P2/P3 顺序继续，不重启一套重型录屏、不提高预算、不并入 64 位：

1. 默认关闭、硬预算内的 Page/Slice 持有者账户：range union 去重、alias、active/retired、失败 entry、
   lastAttempt/lastSuccess，以及池 used/dead/tail 与底层 pending/unknown 分开。零诊断 Rc/shared_ptr 保留。
2. 先补统一采样有效性，消除“0 字节却超预算”和窗口 max/当前帧混用；坏态/恢复各取一次有界账户即可，
   不要求玩家继续盲录相同内容。
3. 正式索引范围摘要和 proof 复用减少已证明的过大快照；不在每个 draw 强行同步读取 device-local IB。
4. 按账户再选寿命分组/释放/页轮转，证明完整合法 caster 留存、压力解除后恢复，且 GPU last-use 不放宽。
5. 新候选独立冻结后才进行对照。玩家 BF938 已给出失败反馈，不将旧离线 87/265 追认为恢复通过。

本 checkpoint：59 项报告分析复核通过，仅新增离线派生产物与调查文档/入口日志；
没有 C++/Shader 改动，没有 Ninja/构建/部署/启动或关闭游戏，没有 merge/commit/push。
P0 完整真实报告严格解析得到新运行证据；P1a 对压力恢复**仍不足**。

## 7. 玩家补充：大地图、当前镜头对象少于高压图

玩家认为可能把其他区域数据放进快照并未清理。本次定向源码核对区分三件事：

1. **整图提前加载没有得到证明。** 这套 draw-time 快照入口由 indexed/nonindexed/UP 绘制捕获调用
   `War3TryCaptureShadowCaster`，不是在该分配器中遍历地图对象。此结论只覆盖 Stage11 池的已查路径，
   不外推为游戏原生剔除精确或所有上层 registry 都没有全图数据。
2. **访问过的区域可留在缓存，这是明确策略。** `generationBackedStaticCandidate` 无动态单位/姿态、
   非 GPU-skin 直接来源等条件成立后，最终 entry 标为静态。成功捕获刷新访问帧；静态离开当前视野
   不立即释放，闲置上限为 108000 帧，另受 inactive 逻辑切片容量 64MiB 的 LRU 目标约束。
   最近 120 帧访问的静态工作集受保护；GC 每 60 帧执行。不是“无清理”，更不是“64MiB 限制全池”。
   这些是当前源码策略；本轮每类 entry 的数量/字节量尚未实测。
3. **一个 draw 也可能携带过大的源域。** 无可信索引范围时 `vRangeStart=0`、
   `vRangeCount=totalVerts`。共享源缓冲可能包含其他 draw 的顶点或暂时不用的容量；
   不能因当前 draw 很小就把完整捕获域当作该物体净几何量。未取得本轮源字节，不能断言里面是哪个区域。

因此“当前画面对象较少”并不能排除池压力；不同场地的源缓冲范围、独有实例、历史访问集合与页内寿命混合
比屏幕上计数更相关。**移动过程中的历史积累是有机制支持的假设，不是已证实的整图泄漏。**
不能据此把所有屏外对象立即清掉：屏外物体仍可能向可见区域投影，且未完成的 GPU 使用必须保留。

P1b 的持有者账户需明确按最后成功捕获/访问年龄区分 current-required、近期/cache-only、retired/pending，
统计旧区域条目保留的切片容量与其独占/共享钉住的整页容量；与同次分配的范围回退原因联查。
不能只再加一个“缓存总数”或把几个未知 domain 相加。这是原 P1b 范围细化，没有新增产品开关或准入修改。

本次只读核对调用点、静态分类、成功寿命、GC 与整域 fallback；更新此文和开发日志，未构建/部署或改生产源码。

## 8. 术语澄清：缓存没有记进帧级 ShadowArena 的 384MiB 账

384MiB 本轮限制的是 **War3Stage11SnapshotPage 池**，不是 `ShadowArena`，也不是全部 WarVK GPU 内存。
`war3_shadow_arena.h` 的帧级分配器有独立的完成序号轮转/预算；`war3_stage11_snapshot_page_policy.h`
定义 Stage11 池自己的 resident cap。模型 CPU 数据缓存、常驻 GPU geometry 等又属于其他账户。

这里所谓 draw-time cache 不是仅存对象索引的 CPU 表：`War3DrawTimeVBEntry::positionSnapshotPage`
等字段直接持有 Stage11 池中的页，实际 position/IB/UV GPU 副本就在该页内。
因此缓存跨帧保留的不是“另一个独立缓存池中的副本”，而是原 Stage11 页的引用。
就这处计账而言不是将缓存再次加到 Arena，而是缓存使同一快照页继续 resident。
其他链路是否有额外几何副本仍须分账户测量，不能由此宣称全系统无重复。

当前实现的实际边界是“长期/较长期静态 draw-time 缓存与短寿命捕获共用一个页分配池”，
不是“384MiB 只用于当前视角、本帧完成后全部释放”。其他池有余额也不会自动解除这一局部预算拒绝。
下一步应按寿命与实际 ownership 拆账/组织页，仍受统一资源约束；不能把保留副本不记账、
将一个固定预算简单拆成互相不能借用的若干池，或跳过 GPU 最后使用证明。
