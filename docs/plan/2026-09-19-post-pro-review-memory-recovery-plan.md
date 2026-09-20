# Pro 审核后修订计划：先恢复完整阴影，再收拢资源寿命，最后迁移位数

日期：2026-09-19。状态：**审查采纳与执行计划；本 checkpoint 未修改生产代码、未构建、未部署或运行游戏**。

后续实施状态见[同日 P0/P1a 首批离线 checkpoint](../agent-history/2026-09-19-memory-recovery-p0-p1a-checkpoint.md)；
上句及 §6 是本计划起草时的历史边界，不再代表当前工作树未修改。未完成 Water 和原版模型自动点光同样排除出 v1.22。
随后[一次隔离 canary](../agent-history/2026-09-19-pressure-canary-and-merge-hold.md)未通过进图 ready，已恢复清场；P4 压力恢复仍未覆盖，用户的条件合并授权尚未满足。
其后 BF938 玩家回归已实际触发容量拒绝及反复全场景丢影，P4 未通过；后续[预算解决方案研究](../research/2026-09-19-stage11-budget-solution-research.md)补充源码覆盖矩阵、15 项离线模型与 P1b/P2/P3 的最小落地合同，不代表新修复或 GPU 验收。

> 后续用户范围裁定（同日）：**v1.22 不推出 64 位更新**。本版只执行 P0–P4 的基础收尾和阴影恢复；
> E3 CPU 取证外置、可选 GPU 外置、R0–R2 渲染宿主均延后，保留设计与实验，不作为当前实施支线或发布前置。
> 下文相关技术评价仍可用于后续版本，但不构成本版实施许可。

## 0. 输入、版本与本轮核验

用户提供完整审核：`C:/Users/Administrator/Desktop/WarVK-memory-x64-independent-review-20260919.md`，73,109 B，730 个物理行；SHA-256：
`9BD1626FC0026D3219BF3A7FCEF2C9013A22F839B4F5D1CA6630F7CDCF54CAFC`。
全文已读。其结论、建议和操作指令均作为待审材料，不构成自动执行授权。

报告审查的 r2 ZIP SHA 为 `CE7204CBB0727BE38231838A43EADA96D5350076785D865A6941535380D43165`。
本轮对包内索引覆盖的 `src/`、`subprojects/`、`tools/render_host/` **1,963 个文件**逐 SHA 复核，当前 B 树没有变化。
B 树 HEAD 为 `ae890542d766470d1703f5bea7f5b73636039733`；开工 dirty=859。工作树未提交，不能只用 HEAD 代表被审源码。

### 主线程独立验证，不只采信报告

1. 源码确认：未知索引域采用完整 bounded VB；页内顺序推进，无自由区间分配；GC 仅删除 Page.use_count()==1 的整页；容量拒绝在 GPU createBuffer 前。
2. 源码确认：captureComplete 先清、frameSerial/活跃状态先刷新、position 可先成功，再在 IB 失败分支退出；ownedGpuBytes 在成功尾部更新。确认有失败所有权/记账风险，但未证明现场各页正由它占住。
3. 源码确认：GPU-skin settlement 显式清 Buffer/capacity 时未同步清对应 Page 字段。它有前置条件；主样本 GPU-skin 未启用，不能认作此次主因。
4. 源码确认：producer 两组重复 JSON emitter 与 profile 可扩容反例仍存在。
5. 源码确认：当前 IB 扫描受 HOST_CACHED 和同代可读 span 约束。注释记录过扫描 write-combined 映射造成高耗时；本轮未复测耗时，禁止把该保护简单删掉作为优化。
6. 原始 inputs.json 使用递归 duplicate-key 拒绝读取，按生产 gather 格式独立解码 inputs.bin：224 batches / 10,026 draw metadata；position sourceBytes 全为 524,288，index domain 均 unknown；2,238 份 Copied gather 全部满足 valid=1、reserved=0、有符号 BaseVertex 地址关系和源范围界；其中 2,199 份与已采 IB 逐索引一致，另 39 份不能作 IB 对照证明；7,788 份未复制数据不当作零几何。
7. 例子 batch4731/frame63735/draw2：part=1245587604，jHandle=1165615，105 索引，BaseVertex=1823，实际顶点 1823..1874 共 52 个，stride=32，有效连续范围 1,664 B；源范围 524,288 B，诊断 gather 输出 5,040 B。这是范围放大见证，不是每 draw 独占 512 KiB 或整池浪费比例。

原始输入身份：manifest `1EDC4C27DE7B46A6FF9E1B904A12A430321570B93BD35261C5EE19178E1F46A0`；
binary `BC9BDFCB7425D8BC4E4CEA5B65D3BA73A46447568320484BCD28356C02A7D5BD`。
两者位于已冻结 r2 包 `evidence/current_run/`，本轮不回写。

Pro 的 24 页/6 KiB 锚点模型、Linux 编译结果及其他补充脚本仅由报告描述；此次未收到完整补充 harness，**不登记为主线程复现通过**。
Linux LP64/Windows 专用 API 的测试编译差异也不直接算 Windows 产品缺陷。

## 1. 结论裁定

### 采纳

- 直接失效链：应用快照容量拒绝 → 必需输入不完整 → ProducerIncomplete 拒绝整帧 CSM 更新 → 旧完整图保留 8 帧 → 无可用完整方向图。不能再以“只漏几条 caster”排除全屏失影。
- 384 MiB 是 active page capacity，不是视野模型净大小，更不是此次 vkAllocateMemory 物理显存 OOM 的证明。used 不等于 live；活动页账不包含全部旧 epoch/命令保留/驱动池。
- 已有条目复用和整页回收，不能说“每帧必然新增”或“从来不回收”。混合寿命死区是可信机制，但现场比例仍未知。
- E3 CPU 取证外置在技术上值得后续评估，且必须与真正 64 位渲染器分开；按用户裁定不在 v1.22 推进产品接入。不把部分 shadow pass 双进程外置作为主修复路线。

### 条件采纳

- 小索引域缩小快照：必须从正式数据来源拿到同 generation 的证明，不允许把诊断 gather、D3D Min/Num 提示或旧 min/max 直接当生产凭据。
- backing 清理：先证明该分支放弃的是哪份资源；Page/Buffer/pin/alias/范围/账一起处理，不无条件删去所有失败 entry。已排队 CS 与在途 GPU 仍按原机制退休。
- 寿命分组、页轮转、区间回收、压实：先由 census 判断收益和成本，再选最小方案。不是一次全部实现；不把 use_count==1 改成页内 used 清零。
- 64 位主渲染边界以 D3D9 语义代理→64 位 DXVK/WarVK 消费为长期候选方向；先证明窗口/Present/Reset 可行，不把报告给出的 128B wire 直接冻结成产品协议。

### 不接受的捷径

不增加 384 MiB 验收预算、不靠 LAA 替换、不减少合法 caster/画质、不延长旧阴影保留掩盖失败、不移除完整性门；不重新逐 draw 扫描非缓存映射或增加同步读回；不强制释放在途资源；不以 CPU ACK 冒充 GPU 完成。
审核报告澄清了机制，**没有证明根因全部查清、已修复、稳定或已有性能收益**。

## 2. 新顺序与长期计划关系

当前最高优先级从“继续扩大模块迁移”改为：

```text
P0 可信报告/预算配置
   ↓
P1 backing 所有权与失败记账 + 有界页/切片基线
   ↓
P2 可信索引域 → 精确捕获范围
   ↓
P3 按实测剩余原因选择压力回收方案
   ↓
P4 组合高压、解除压力后恢复、性能与视觉验收

1.22 之后的候选路线（当前延期）：E3 CPU 取证产品化 → 净收益验收
1.22 之后的长期路线（当前延期）：R0 窗口/语义原型 → R1 D3D9 覆盖 → R2 WarVK 场景迁移
```

M1/M2 已迁出的纯函数、共享生命周期和协议保留；M3/M4、后端收口按本次资源链顺带推进，不另开全库拆分。
基础 JSON/profile 修正先做；资源 census 与修复各自冻结差异，避免把仪表效应和资源行为混在一个无法对照的补丁中。
读取研究不增加实现百分比：[原进度基线](2026-09-19-longterm-progress-and-foundation-closeout.md)仍约实现 60% / 闭合 40%，64 位继续单列。

## 3. 主线工作包与通过条件

### P0：完成之前答应的基础收尾（小范围）

- producer 仅删同一 shadowBudgetSummary 对象第二组 Cutout/AlphaBlend 输出；不动 aggregation、键名与数值。
- 完整生产报告交给严格 root parser：零值、非零值、相同值重复/不同值重复反例都覆盖；旧 invalid 报告永不追认。
- profile overrides 不得高于**所选 profile**的 pre/post 默认值；保留两档默认，不用全局最大值代替只缩小条件。对无值、非法、边界、溢出、带尾随字符及超过 profile 值明确拒绝/保留默认策略。
- 配置清册补新增生产 knob，测试探针单列；实际生效配置在诊断合同可见，不产生无文档隐藏开关。
- 通过：生产函数测试 + 完整导出往返 + 相关旧回归；本工作包不更改渲染数据和分配算法。

### P1a：失败 backing 与账分离（局部职责收口）

- 先冻结字段关联表：Buffer、allocation pin、Page、offset/range/capacity、alias、资格 proof、owned 字节分别是谁的职责。
- 正确占用账随获取/替换/解除更新，不等待整个 draw 成功；lastAttempt、lastSuccessfulCapture、缓存访问与发布资格明确分开。
- 窄化统一的 backing-transition helper，覆盖实际发生的状态转换；不要新造只转发调用的 Manager。
- indexed→nonindexed/独立 UV→position alias 等保留策略写明。失败可留合法可复用 backing，但不得冒充有效当前输入、误刷新成功寿命或隐藏占用。
- 不改变 key、caster 准入、配额、GPU 完成条件；不把“回退失败”改成绘制错误几何。
- 回归：position 成功后 IB 失败重复 100 次；显式 GPU-skin settlement 失败携 Page；UV 分合、独立 IB 废弃；pending CS/GPU 仍能使用旧 backing；同 identity 反复重试不产生无界不可见保留。

### P1b：有界 census，先回答钱花在哪里

分两层交付，避免又造一套庞大取证系统：

1. **CPU Page/Slice 所有权层**：创建、替换、解除、alias、失败 entry、active/retired、lastAttempt/lastSuccess、范围回退原因；逐页 capacity/used/tail，按范围并集去重。
2. **底层保留层**：通过既有 CS 批次/提交/完成追踪关联 Buffer/allocation；缺证只记 pending/unknown，不能把“无 Page owner”写成可安全复用空间。

新增 x86 元数据和出口缓冲总硬预算 ≤4 MiB（具体 sizeof 在 32 位编译测量，表/索引/对齐全计入）；默认关闭，不扩大图像环，不保持诊断用 shared_ptr/Rc，不新增每 draw 同步读回。
关门时无新增表/worker/采样扫描/OS 查询/格式化；必要生产正确性记账单独计成本。

- arm 时核对象与切片规模；不足时分页 + cut sequence/delta 或拒绝“完整”模式，不能截断后称闭合。
- current-required/cache-only/pending-only/safe-dead/padding/tail 互斥，active/retired 为标签；未知不可塞进 safe-dead。active 页账与物理 allocation 账分层。
- 成功与失败帧统一有效 gauge，给出 valid/sample frame；Last=0 与窗口 Max=1 不再被误作同一时点状态。
- 有界采集目标：正常→触发→停止相机→至少两个既有 GC 周期及相关完成；时间上限 20 秒、输出上限 32 MiB 作为初始合同，未满足就 incomplete，不自动延长/重试。
- 通过：别名/增容/退役/丢事件/表满/跨 cut 修改的生产交接测试；页分区闭合；关闭/开启成本测量；一次可定位持有者的有限运行。物理层缺数据时只关闭已证实的子问题。

### P2：减少已证实的大范围捕获，而不是搬运更多内存

先审计已有上传、Lock/Unlock、DrawIndexedPrimitiveUP 与 CPU-readable span 路径，复用 allocation/content generation；避免第三份重复身份系统。

- 在 CPU 输入仍权威且可低成本读取的写入/上传边界建立**范围级索引摘要**，消费只查证；不能假设所有映射都是普通缓存内存。
- key 至少绑定实际源身份/分配与内容代际/索引类型/所覆盖字节域；局部更新、DISCARD、NOOVERWRITE、GPU 写入、重命名与 Reset 都有失效规则。
- 不建立无界完整 IB 镜像；摘要内存、更新扫描、重复 draw 命中及 p95/p99 主线程成本一并预算。
- 只在正式已有范围选择入口使用证明；未知/过期保持原安全路径，不能为了省内存扩大准入。
- 覆盖 UINT16/UINT32、有符号 BaseVertex、stream offset、首索引、stride/UV、零/溢出、多个 layer/instance、同地址重用；保留非索引语义。
- 通过：原小 draw/大 VB 样本缩到正确连续范围（最终对齐容量单列）；最终读到的 position/IB/UV 与旧路径逐字节等价；坏 generation 拒绝摘要；无逐 draw 非缓存扫描、无新增 GPU→CPU 同步等待。

### P3：按 census 选剩余分配修复，而不是先造通用 GPU 堆

选择顺序：无用/失败 backing 的安全保留与淘汰 → 静态/动态/大 position 与小 IB/UV 的寿命/流量分组 → 动态页轮转 → 必要时才做区间回收/压实。
全局预算统一；不能硬分池造成一池空、一池拒绝；不删除实例/layer/world/pose 语义去凑共享。

- 为报告 24 页每页 256B 锚点的模型建立仓库内可复现生产策略回归；这是新待办，不追认外部 harness 已入库。
- 任何区间重用必须同时闭合 CPU owner、未执行/未提交 CS、全部相关 GPU 使用；不能只等一个 fence 或固定 N 帧。
- 压实若需要额外 scratch，也计入预算；满池无空间不能假设能搬。真实 DXVK backing 的 usage/同步由专项合同核验。
- 若唯一合法工作集本就超预算，再单独设计保持全部合法 caster 的流式/分批方案；不承诺只做 GC 一定够。

### P4：用户可用候选的门，不靠文档打分

每项行为改变独立构建冻结，最后组合验证；新哈希不继承旧实机结论。构建按 Below Normal、exact DLL、最多 -j2；本计划不自动授权构建/部署。

- 同地图、路线、画质、有效配置与诊断 profile，记录 EXE/Game/DLL/map 身份及真实 LAA，不修改 EXE 掩盖成本。
- 非交互隔离、零全局输入，目标 2560×1440；客户区/backbuffer/receiver viewport 分列，达不到须如实标记和单独确认替代，不能外推旧 1902×963。
- 正常视角→低视角→不同地点重复→回原地→压力解除；再覆盖单独的长时/Reset/换图矩阵。
- 阴影完整性、合法 caster、图像恢复和原始输入正确都要通过；不能用关闭效果/少画对象抵消内存。
- 首个定向运行采用一次已冻结路线，不以无界重试掩盖失败；若未触发预定压力条件只能未覆盖。
- 账户随解除压力趋向有解释的稳定值，不能将所有缓存必须归零作为伪验收；active/retired/backing 与基线差异可解释，资源异常与 GPU incident 为零。
- 主线程/GPU/Present 延迟 p50/p95/p99、上传字节、分配/复制/锁、32 位 VA/private/最大连续空闲及 heap budget 分列；无前台同条件对照则不宣称 FPS 收益。
- 通过这些门后才生成玩家候选；最终视觉与长期门通过后，才考虑稳定日志和发布。

## 4. 64 位后续版本备忘（v1.22 排除，不执行）

### E3：后续版本候选，只外置 CPU 取证

复用 E1/E2 事件协议、有限 ingress 与 host history；产品入口到后台 worker 必须异步有界，绝不把 lab 的阻塞 pipe/析构直接接到 render thread 或 DllMain。

- 先完成 owned I/O 生命周期：前台超时可返回，worker 继续持有 OVERLAPPED、buffer 和 handle 至真实完成；显式 Shutdown 完成前不得卸载其代码。故障时停止取证而不是卡住主帧。
- 建议初始预算：入口 1024 事件、4×64 KiB slot；数据面 ≤1.5 MiB、x86 增量 private 目标 ≤4 MiB；线程 stack VA 另列。这些是待测目标，不是收益承诺。
- 同 profile 对比本地/外置净 VA/private/最大连续空闲区；移除本地历史后再算净收益。必须包括正常、host 缺席/崩溃、慢消费、写盘失败、停止、Reset、退出。
- 诊断可有具名 loss；不改变渲染数据、不阻塞游戏。未连 host 时清楚选择停取证或受限本地模式，不能静默保留大历史再声称省内存。
- 该支线不移走 Stage11 池、彩色 GPU 历史、输入 staging，也不是本次阴影恢复的前置条件。

### 真正渲染宿主：长期 R0 → R1 → R2

- **R0**：独立测试程序先验证 32 位窗口消息与 64 位 Present/Reset/query/create 的最小语义环；早测 Alt-Tab、resize、全屏、host 故障和窗口互等，不先投入全部资源迁移。
- **R1**：D3D9 COM identity、Lock/Unlock、DISCARD/NOOVERWRITE、Create 错误、stateblock、UP、Query、读回、9/9Ex、多个 swapchain 的兼容和生命周期；必需命令不能丢，控制消息有独立额度。
- **R2**：Warcraft Hook/JAPI 留 32 位，按值身份/几何/姿态发送；64 位统一最终选择、DXVK 资源与渲染，不保留双份最终决策或全量 32 位镜像。
- Accepted / UploadReleased / GPUCompleted / Presented 四种事实分开；资源域及 connection/map/device generation 分开。新 render wire 与 RH0/RH1/WVE1 不混认。
- 双 GPU 进程共享 shadow pass 暂缓；可丢失 GPU 取证出口仅在另行窄原型需要时研究，不能让主 GPU 队列等待可能永不 signal 的 helper。

## 5. 技术规范核验（本轮已重新查阅一手资料）

- [Khronos vkFreeMemory](https://docs.vulkan.org/refpages/latest/refpages/source/vkFreeMemory.html)：相关提交必须完成。应用到 P1/P3：清 Page 引用不等于可覆盖仍在使用的底层区间，继续保留 CS/GPU 生命周期证明。
- [Microsoft CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)：取消请求不代表 I/O 已完成，完成前不可重用/释放 OVERLAPPED。应用到 E3：worker 持有并异步排空，而非删等待制造 UAF。
- [Khronos Win32 surface](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateWin32SurfaceKHR.html)：相关 WSI 调用可能同步发窗口消息。应用到 R0：窗口线程与 host 不可形成同步互等；不能只凭 HWND 参数认定跨进程兼容。

外部报告的其他规范在对应实现开工时逐项核验；本轮未假称重新验证全部 N1–N22。

## 6. 本轮完成与第一步

本轮完成：全文审查、版本一致性复核、关键源码路径审查、原始 gather/IB 独立复算、三项规范核验及本计划。
**未实施 P0–P4/E3/R0，未运行新的 C++/Meson/GPU/游戏测试。**

下一实施批次从 **P0 的 producer 重复键 + profile 只缩小**开始，各自独立差异和生产回归；随后 P1 的所有权字段表与失败序列测试，再落最小 census。
先用可复现的失败输入约束实现，再写修复；不先请求用户反复录新的大包。
资源策略修改前保留旧基线，受审 ZIP 与用户原始证据不改；不自动提交 Git、部署或晋升稳定。

## 7. Arena / GPU 缓存 / CPU 内存的口径澄清（同日源码核验）

- **ShadowArena 是 GPU 分配器，不是 CPU 模型仓库**：`war3/memory/war3_shadow_arena.cpp:288` 请求 DEVICE_LOCAL，模块不 map 页数据。它为需要冻结的帧级输入提供范围，按实际 completion 轮转，不是经过固定三帧就能覆盖。
- **本次 384 MiB 失败是另一套 Stage11SnapshotPage 池**：`d3d9_device.cpp:23767–23783` 同样创建 DEVICE_LOCAL buffer；页与跨帧 draw-time cache entry 关联。不能把它与帧级 ShadowArena 的 resident/used 或上限合并为同一个数字。
- **常驻 GPU 模型资源**在已证明不可变、身份/代际/范围和实例变换适配的路径可共享，减少快照；但缓存模型原始几何并不自动替代当次变形、临时 UP、附件、材质层、UV 与原生动态流。未知路径不能拿旧模型数据冒充最终 draw。
- `d3d9_device.h:2630` 附近说明原动态 VB 会被后续 draw 复用覆盖，保留 Buffer 句柄本身不冻结字节；因此需要已证明的存储租约或 GPU 快照。动态 GPU→GPU copy 不等于先回读到 CPU 再上传；另有 `stableSourceBytes` 路径需要 CPU-owned 上传暂存，须分账。
- **CPU 侧确有模型 payload**：`war3/model/war3_model_resource_cache.h` 的 positions/normals/UV/indices/group/matrix arrays 是真实 vector，不只是指针表；当前发布为不可变记录供解析、验证、GPU package 上传等消费者使用。哪些可释放/压缩/只留摘要必须按消费者审计，不因“上传过一次”直接删除。
- CPU 身份/代际/命令/引用记账、D3D9 合同要求的系统内存副本、上传和取证/截图 staging 是另外几类；DEVICE_LOCAL 申请不证明驱动无 CPU 开销，也不代表 GPU 字节等量占用应用 CPU 堆。
- [Vulkan memory flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html) 区分 DEVICE_LOCAL 与 HOST_VISIBLE，两者不必互斥；[D3D9 managed resources](https://learn.microsoft.com/en-us/windows/win32/direct3d9/managing-resources) 有系统/设备副本语义，但不能据此假设本项目所有资源都属于该类。

本轮未测全局 cache hit rate、CPU/GPU 实际字节比例或逐持有者大小。v1.22 的方向是让可信静态资源尽量共享，动态输入只冻结必要范围，缩短无用保留，CPU 只留有消费者的必要数据；不是直接关闭 Arena，也不是声称所有 CPU 副本都合理。
