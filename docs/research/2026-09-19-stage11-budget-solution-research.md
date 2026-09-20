# Stage11 预算超限：修复方向、离线反例与最小落地合同

日期：2026-09-19。范围：B 主树 BF938 后续研究，**非生产修复、非新候选、非实机验收**。
用户要求继续研究预算超限如何解决；本轮只读生产源码、查阅一手规范、新增独立 Python 模型/测试及文档。
未 Ninja/编译/部署/游戏/提交/合并。v1.22 仍不纳入 x64、未完成 Water 或自动模型灯。

## 1. 当前结论，不再重复错误归因

实机直接链已在[玩家调查](2026-09-19-bf938-player-shadow-loss-report.md)固定：
Stage11 应用池容量拒绝 → 必需输入不完整 → 整张方向图拒绝更新 → 8 帧旧图保留耗尽 → 全场景无影。
它不是帧级 ShadowArena 满，也不是当前屏幕可见模型确实需要 384MiB，更不直接证明 GPU 物理 OOM。
已有 P1a 对失败寿命/引用账的修正不足以通过玩家恢复门。

建议主线保持 **P1b 精确小账 → P2 可信范围减量 → 根据账户选择 P3 页组织/压力回收 → P4 玩家恢复**。
不把本轮模型的改善作为直接跳过现场账户、一次性重写 allocator 的理由。

## 2. 新的源码核对结果

| 位置（本次源码锚点） | 查实事实 | 对方案的约束 |
| --- | --- | --- |
| d3d9_device.cpp:23686 / 23822 | 尾部追加；仅 Page CPU 引用归池独占才解除整页 | 当前不能重新使用页内洞；`used` 不是 live |
| d3d9_device.cpp:43120 / 42916 | capacity 足够可继续用同 entry backing；可信静态 source proof 相同可跳过 copy | 不能声称每 draw/每帧必定新分配；不要再造重复 cache |
| d3d9_device.cpp:42241 | 未证明索引范围时完整 bounded VB 捕获 | 小物体可以产生大副本，正式范围证明是高收益候选 |
| d3d9_device.cpp:41915..42022 | 常规索引扫描已有 HOST_CACHED、NeedsReadback、FlushBuffer 和同代 span 检查 | 不删除非缓存映射防护；mapping 字节与 REAL 上传顺序必须配对 |
| d3d9_common_buffer.h:149..185 | resource identity / map allocation / content 三类 generation 已存在 | 复用身份系统，不能新增第二套竞争“真值” |
| d3d9_device.cpp:35598 | writable Lock 时内容代际已经推进，不是 Unlock 后才推进 | generation 匹配仍不表示写入已完成；摘要必须有成功 Unlock/可消费状态 |
| d3d9_device.cpp:36194 / 32316 | 普通上传与 DIP-UP 已有 CPU→UP 拷贝入口 | 可研究在权威 CPU 输入/已有拷贝边界产生范围凭据；不能靠事后回读 GPU |
| gpu_skin/war3_gpu_skin_native_bridge.cpp:13738 | index ticket 已校验实际字节/min/max，但仅启用 native bridge 且 CurrentBypassedIndexUpload 存在才走 | 不是通用常开服务；不可开启实验或复制其扫描来假装 P2 已有覆盖 |
| d3d9_device.cpp:2931 / 2997 | direct-static/direct-upload 仍 dev-only；注释明确资源代际不能证明子对象源内容不可变 | 不将零复制开关默认打开作为减量修复，否则可能重新引入撕裂 |
| d3d9_device.h:2936 | retired session 保留 drawTimeVbCache；旧 Buffer/CS/GPU 可继续保活 backing | active 页预算不是全部物理驻留，必须分开账户 |
| war3_resource_residency_census.h | 已有资源 census 主要是 D3D resource/host backing 与 GPU-skin 池 | 不等价于 Stage11 Page/Slice 所有权表，可复用输出体系，不能冒充已有所需数据 |

### 特别排除一条“看起来最快”的捷径

不能因为显卡里已有原生 VB，就只保存它的句柄/代际然后不复制。原生后续写入可能改同一存储；
资源活着与字节被冻结是两回事。恢复阴影不能以重新制造错误顶点/撕裂为代价。
共享同代不可变 backing 可以作为后续优化，但本轮未证明原生动态环满足该条件。

## 3. 离线反例：已执行，不是假定现场比例

新增模型：`AutoTest/analyze_stage11_snapshot_budget_model.py`。
测试：`AutoTest/test_stage11_snapshot_budget_model.py`，**15/15 通过**，含固定种子 4000 次操作的账户闭合检查。
结果：`AutoTest/artifacts/stage11-budget-research-20260919/policy-model.json`，CreateNew 写入。
结果绑定当前 policy/lifetime/cache policy/device/common-buffer 五份源码 SHA；常量 256B/16MiB/384MiB 再核。

**这是 Python 策略镜像，不是编译/调用生产 C++ 分配器，更不是 GPU 证明或玩家实测。**
模型不含生产每帧 create 上限、TTL、真实线程、driver allocation 或 capture 分类覆盖；不能登记为生产门禁通过。

### 3.1 少量长期切片钉住大池

输入：每轮一个 256B 长期切片、一个 16MiB−256B 短期切片；短期输入假定 GPU 已完成后解除 CPU 引用。
重复 24 轮，随后请求 512KiB。两种策略保留同样的全部长期输入，不靠删除 caster。

| 策略 | 24 轮后长期有效切片 | 24 轮后 active resident | 下一次 512KiB |
| --- | ---: | ---: | --- |
| 当前式混页/尾部追加/整页解除 | 6KiB | 384MiB | 拒绝 |
| 同一总预算、按寿命选择相容页 | 6KiB | 16MiB | 成功，active 变 32MiB |

这复现了此前外部报告的“少量锚点可钉住整池”机制，现在仓库内有可重复的独立模型。
**不代表玩家现场只有 6KiB 活数据。** 分组模型还发生更多创建/回收（24 轮后创建 25/回收 24，混页创建 24/回收 0），
所以不能从 resident 下降宣称主线程成本改善。实作需保持 create 门、验证抖动与完成后的页复用策略。

把混页预算模拟提高到 768MiB，只是让相同模式在 48 轮后再次满；不是根本解法。

### 3.2 范围放大的量级

构造 400 个仍需保留的 draw，每个 105 个 UINT16 索引，position 分别取 512KiB 或已证明的 52×32B。
包括 256B 对齐和每 draw 的 210B IB：

| 输入模式 | aligned CPU-owned 切片总量 | active page capacity |
| --- | ---: | ---: |
| 全域 position | 209,817,600 B | 208MiB |
| 精确连续 position 域 | 819,200 B | 16MiB |

52 顶点例子来自先前旧证据；将其复制为 400 个是**人工 workload**，不是本轮 draw 分布，不能据此报净节省百分比。
它说明应该优先少捕获不需要的字节，而不是仅换一个容量更大的容器。

### 3.3 必须保留的反例

- 固定拆成 static 64MiB / dynamic 320MiB：其余为空时 65MiB 静态请求仍失败；共享预算则能通过。
  因此不能把两种寿命分组误写成两份不可互借硬额度。
- 25×16MiB **全部真实必需且仍活跃**时，384MiB 预算无论如何分组都不足。届时需要进一步减量/可信共享，
  或独立设计完整场景的分批方案；不能保证只改 GC 一定解决所有地图。
- CPU owner 已解除但假定 GPU completion 未到时：active 可为 0，retained backing 仍为 16MiB。
  新分配另一页后 modeled backing 可达 32MiB；旧页的地址从不重用。证明账户口径差异，不代表真实驱动占用测量。

## 4. 第一刀：小型页账户，而不是再做一套大取证器

### 采样合同（待实现）

- 渲染所有者 Present 安全点，一次一致性 cut；禁止控制面线程直接遍历 live cache/page。
- 默认关，总新增 x86 元数据/出口缓冲 ≤4MiB；复用既有诊断配置/发布流程，不复制图像/VB/IB，不保持 Rc/shared_ptr。
- 先检查待处理规模再采集；超预算/跨 cut 变动/未知 owner 都显式 incomplete，不截断后报“完整”。
- 启用时取正常、首次容量拒绝后的 owner 安全点、两个既有 GC 周期之后、恢复后这几个有界样本；
  不承诺能在一次运行覆盖所有状态，也不为补齐自动延长原定窗口。
- 失败热路径只置请求/累加具名标量；不要在每次 35 万级拒绝分支遍历全缓存。
- 同次发布 valid/sampleFrame/map/device/capture serial、resident/used/tail；不能把未采样 0 伪装成空池。

### 要回答的具体问题

每页记录 id/capacity/used，与 entry 的 position/index/UV 的范围并集对账；alias 只算一次。
输出 current-required、cache-only、failed-retained、retired/pending 标签及 lastAttempt/lastSuccess/lastAccess 年龄，
按所有权优先级对重叠范围分区；**CPU-unowned 不叫 GPU-safe-dead**，缺完成证明保持 unknown。

页级等式：`capacity = CPU引用范围并集 + 已分配但无CPU引用范围 + 对齐/间隙 + tail`（分区定义冻结后实现）。
物理 backing 另账，以 allocation identity 去重，CS 未提交/已提交未完成与 retired 分列。
旧区域只依据已知对象身份/最后访问年龄描述，不靠猜测世界位置或屏外就授权驱逐。

索引 unknown 同时拆成 **GPU-authored/readback、无可信代际、UP无匹配source合同、非HOST_CACHED、越界/缺span** 等
真实失败分支。只有这样才能决定 P2 应先覆盖哪一种 producer；827,956 这个总数本身不能回答。

## 5. 第二刀：可信范围减量的正式入口

不要删除 HOST_CACHED 门，也不要直接信任旧 MinVertexIndex/NumVertices 提示。
候选凭据应绑定：资源 identity、allocation/content generation、index type、完整 index byte range、
生产完成状态、map/device，以及 min/max；消费继续验证 signed BaseVertex、stream offset、stride 与所有边界。
**摘要可以是覆盖查询范围的保守上界，不一定每次是最紧值，但绝不能漏掉实际索引。**

优先级与覆盖限制：

1. 已有可安全读的 index span：在同内容代际复用已计算摘要，避免逐 draw 重复扫描。
   不宣称它解决当前大量 unknown；这一类当前本来已有精确扫描。
2. DIP-UP/已有 CPU 上传边界：利用仍权威的 CPU 数据一次产生 draw-local 范围，绑定原始拷贝和最终上传 slice。
   不在返回后保存调用者指针；不把 raw UP 冒充有 D3D commonResource identity 的持久 buffer。
   若需 scratch，必须硬上限并单列复制成本，不能新增完整无界 IB 镜像。
3. 普通 Lock/Unlock：写锁一开始就撤销旧摘要，成功最终 Unlock 后才允许发表；
   DISCARD 整 buffer 失效，NOOVERWRITE 仅是对已用数据不覆盖的调用方承诺，不是本项目可忽略所有失效的理由。
   首批可整 generation 失效，覆盖差异先测；以后才考虑正确维护局部区间摘要。
4. GPU-authored/unproven/WC-only 路径：保持现有安全 fallback。不为图省事增加同步 GPU→CPU 回读，
   不通过开启 GPU-skin/原生上传实验来制造覆盖。若它占主导，另审原生写入可证明边界或异步后续代际方案。

已有 GPU-skin index ticket 只能在其完整合同确实成立时复用，不能因它存有 actualMin/Max 就越过其 scope。
这也是“最大量未知来源是哪类”必须先有有界分桶的原因。

## 6. 第三刀：由测量选择页组织，避免大改 GPU 堆

### 首选候选：寿命相容选页，统一预算

在原 allocator 单一入口加入 allocation lifetime hint，初期区分长期与短期/未知；
字段只影响选哪页，不授权输入有效性/资源重用。复用已完成的静态分类，分类不可靠时保守归类并统计。
不移动现有切片、不更改 Buffer/offset/pin 合同；对新分配选择相容页，共享原总上限。
只有当 census 显示明显混页死区时才实施/开门；它不是现场已证实有效的修复。

成本门：页创建峰值/总数、create throttle、主线程 p95/p99、retired backing 峰值，不能只看 active resident。
页轮转/复用必须额外证明所有 CPU、待执行 CS、全部 GPU 使用已结束，不能按“过两三帧”或 Page.use_count 独占重置游标。

### 可选后续，不与第一刀混成一个补丁

- 压力驱逐：只解除已证明 cache-only 且可重新取得的引用，保留 current-required 与在途使用。
  不用“屏外”代替“当前不需要”，避免驱逐投向可见区域的 caster。整页释放收益应优于纯 entry 年龄猜测。
- Bundle 预留：已有帧 Arena 的 Begin/Commit/Rollback 是可参考的职责模式，不能直接替代 Stage11 页所有权。
  若测到 position 成功/IB或UV失败的部分保留占主导，再设计“先验证/预留整套，后发 GPU copy”的原子捕获。
  已 emit copy 的区域不能简单回滚 used；不在本轮实现此跨热路径改造。
- 压实最后考虑：当前 Stage11 页 usage 没有 TRANSFER_SRC，不能直接把它们当搬家源。
  还需新 descriptor/offset 交接、额外 scratch 预算、barrier 与全部消费者完成证明；满池时不能假设凭空有搬迁空间。

## 7. 一手规范到本项目的映射

- [Khronos vkFreeMemory](https://docs.vulkan.org/refpages/latest/refpages/source/vkFreeMemory.html)：提交中引用内存的使用结束后才可释放；
  当前 Page owner 与实际 CS/GPU backing 不同层，不能把 CPU 引用计数当覆盖授权。
- [Khronos vkCmdCopyBuffer](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBuffer.html)：源/目标 usage、范围、不重叠与 render-pass 外操作均有约束；
  对应 Stage11 压实不是补一个 copy 就能上线。
- [Microsoft D3DLOCK](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlock)：buffer DISCARD 作用于整个 buffer；NOOVERWRITE 有特定使用承诺；
  对应 P2 摘要失效与可消费时机必须保留，而非仅检查指针一致。
- [Microsoft DrawIndexedPrimitive](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitive)：索引与 Min/Num 相对 BaseVertex，合法调用不得引用声明范围外顶点；
  本项目在实际路径已记录过不可靠提示，规范前提不能替代本地来源验证。
- [Microsoft DrawIndexedPrimitiveUP](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup)：输入为调用者内存，调用返回后无需保留；
  对应在现有拷贝边界产生自有摘要而非异步保留外部指针。

以上资料本轮实际查阅；没有以外部建议直接修改 GPU 同步/准入。

## 8. 实施顺序与验收

| 独立批次 | 交付 | 不得冒充的结论 |
| --- | --- | --- |
| P1b | 有效采样页账户 + unknown 原因分桶；生产交接/别名/容量/关闭开销测试 | 有数据不等于已回收 |
| P2 | 覆盖占主导且可证明的 index producer；tight position/UV/index 配对 | 旧样本缩小不等于全场景节省 |
| P3 | 按账户选寿命分组或已证实 cache-only 解除；独立行为差异 | CPU owner 归零不等于 GPU 可覆盖 |
| P4 | 同图多点低视角→回原地→解除压力，保持全部合法 caster；冷/热/长时与视觉门 | 模型/静态/编译通过不是玩家通过 |

本轮 15 项 Python 模型通过，只完成研究工具与具体工单。DLL 仍为 BF938 / 36,359,521 B，未重编译。
下一实际实现从 P1b 小账开始；不要继续扩大取证图像环，不要求玩家反复录相同信息。
成功标准是同预算下阴影持续完整且压力后可恢复，并控制 CPU/GPU 尾延迟和 backing 峰值，不是计数器“没有错误”或少画对象。
