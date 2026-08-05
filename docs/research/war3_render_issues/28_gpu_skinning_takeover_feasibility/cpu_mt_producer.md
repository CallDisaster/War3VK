# DXVK-owned 多线程 CPU 蒙皮 producer 设计与离线门

日期：2026-07-15  
状态：独立核心与纯 Python 合同已完成第三轮安全收口，已经纳入 Meson 并通过 x86
build-only；尚未接入 native hook/controller，也未部署或运行 Warcraft III。

## 1. 范围与结论

当前逆向证据已经足以独立实现普通 `skinMode == 1` geoset 的最后一级纯 CPU
蒙皮数学：每顶点用一个 `uint8_t groupSlot` 选择当前帧 48-byte 3x4 group
palette 矩阵，position 使用 translation，normal 不使用 translation、不 normalize，最后按
FVF 0..5 写出 24/28/32/36/40/44-byte interleaved vertex。

这不等于已经还原整个 `CWorld`、全部子类或全部成员函数。当前 producer 也不需要这项
前提；它只替换已经被寄存器级 ASM 闭合的逐顶点末端。新的 Game.dll hook 接线、资源
owner 生命周期或扩大 native bypass 语义前，仍必须回到 IDA ASM 逐项证明，伪代码不能单独
授权。

新增实现：

- `src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.h`
- `src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp`
- `AutoTest/analyze_cpu_skin_mt_offline.py`

`src/d3d9/meson.build` 已纳入该独立核心；native bridge、manager、device 和 hook 仍未接线。
只有唯一 Test Conductor 才能完成后续 build/deploy/War3 验收。

最新离线合同为
`AutoTest/artifacts/cpu_skin_mt_offline_v3_20260715_213404`。最新 build-only 为
`AutoTest/artifacts/gpu_skin_cpu_mt_round3_build_only_j2_v1_20260715_213652`：增量
`2/2`、warning `0`，最终 DLL SHA256
`337930500A6598AAB69B50790EF5B6BB0B9A8A3D10F4D5C6E940B2BBF0143B78`。链接后纯 kernel
反汇编为 `mulps/addps = 12/10`、FMA/x87 `0/0`。这些只证明代码和构建闭合，不证明运行收益。

## 2. 为什么不能“一次 native call 一个线程任务”

最新隔离 ABBA 只能用于归因，不能作为正式 FPS：

- artifact：`AutoTest/artifacts/gpu_skin_perf_isolated_ab_20260715_133103/summary.json`；
- disabled/bypass frame：`11.4635/14.2640 ms`，增量 `+2.8005 ms/frame`；
- disabled/bypass main：`9.4265/12.7975 ms`，增量 `+3.3710 ms/frame`；
- GPU：`1.424/1.412 ms`，基本不变；
- `3058` 帧内 original native kernel `1,111,353` calls；prime-period all-route sample
  平均 `0.597056 us/call`，但它不是 bucket-specific 小模型 SSE 时间。

即使把这个 all-route sample 当成每次调用上界，全部 original kernel 也只有约
`0.216985 ms/frame`，只占当前 `+2.8005 ms` bypass 增量的 `7.748%`。因此 CPU-MT
不能偿还当前 takeover 的主要管理损失；主线仍必须继续删除/合并 manager、bridge 和
submission 固定成本。

纯参数模型中，仅 descriptor、enqueue、completion poll 的单任务固定成本已经约为
sampled native mean 的 `92%`；若再加入 batch setup、单独 upload 记录而尚未计算任何
顶点或复制任何字节，约为 native mean 的近九倍。因此：

- tiny job 保留 Game.dll SSE；
- medium job 只允许 flush-entry 粗批、存在足够 async lead 且实测 crossover 为正时进入
  CPU-MT；
- large job 仍优先 GPU；
- hook-local synchronous fork/join 只作为大 CPU fallback 的低风险对照，不用于小 job。

第二版成本模型还显式计入 synchronous owned-staging→mapped-output 的 owner copy；这条
安全复制只会抬高 Phase A crossover，不允许用旧的 direct-mapped 假设宣传收益。

## 3. 输入与数学合同

### 3.1 immutable static snapshot

`CpuSkinMtStaticSnapshot::Create` 只在资源创建或 generation/content 变化时打包一次
position、normal、group slot、UV 和可选 diffuse。worker 永不保存 Game.dll 裸指针，也
不访问 D3D9/Vulkan 对象。

输入门固定如下：

| 输入 | null | stride 0 | 策略 |
|---|---:|---:|---|
| position | 拒绝 | 拒绝 | 必须覆盖全部顶点，每项 12 bytes |
| normal | 拒绝 | 拒绝 | 当前核心明确 fail-closed，不隐式合成 normal |
| group slot | 拒绝 | 拒绝 | 每项 1 byte，创建时一次性证明最大 slot 小于 group count |
| UV0/UV1 | 声明该层时拒绝 | 声明该层时拒绝 | 每项 8 bytes；只按 snapshot 声明层数固定 |
| diffuse/extra | 允许 | 仅严格 null+size0+stride0 | 空时 odd FVF 合成 `0xffffffff`；非空时一次性原样固定 |

额外硬门：map/device/content/layout generation 均非零，顶点数 `1..16384`，matrix group
数 `1..256`，UV 层数 `0..2`，所有 size/offset 使用 checked arithmetic。

### 3.2 frozen kernel

`CpuSkinMtFrozenKernel::Create` 在 render thread 的权威窗口内只复制当前 palette 和小
descriptor；大静态流通过 `shared_ptr<const CpuSkinMtStaticSnapshot>` 固定。每条 palette
精确为 `matrixGroupCount * 48` bytes。

创建时还冻结 render lane 当前 MXCSR 的 DAZ、异常 mask、rounding 与 FTZ 控制位；sticky
exception status 不进入 key，也不会跨线程传播。六类 SSE 异常没有全部 mask 时直接拒绝。
每段 `runRange` 用 RAII 临时安装冻结控制位并恢复调用线程完整 MXCSR；async submit、
synchronous fork/join 入口与 Ready lease 发布前还会在 owner thread 再次 exact 核验，
rounding/FTZ/DAZ 发生漂移只能回退原生 kernel。

`runRange` 是纯 CPU、无排队入口，可让多个线程写 caller 提供 output 的互斥顶点范围。
它支持 FVF 0..5 以便 parity；正式 producer 的 async/sync route 仍保守限制为
`0/2/4`。运算顺序与现有 precise compute shader一致：

```text
x = ((m0 * x + m3 * y) + m6 * z) + m9
y = ((m1 * x + m4 * y) + m7 * z) + m10
z = ((m2 * x + m5 * y) + m8 * z) + m11
normal 使用同样的前三列，但没有 m9/m10/m11
```

实现使用显式 SSE mul/add；没有 FMA、reassociation、inverse-transpose 或 normalize。

## 4. persistent producer 合同

`War3CpuSkinMtProducer` 由 D3D9 owner 显式拥有，不是 process-static singleton：

- 使用 persistent、joinable `dxvk::thread`；线程名为 `war3-cpu-skin-N`；
- worker 只执行纯数学，不调用 Game.dll、D3D9、DXVK context 或 Vulkan；
- public `CpuSkinMtFrozenKernel::runRange` 仍独立安装/恢复 MXCSR；producer 内部则每个 coarse
  task/job 只安装一次冻结控制位，在该 scope 内继续每 64 顶点做 cancel/generation 检查，避免
  把 `stmxcsr/ldmxcsr` 和完整 RAII 重复到每个取消片段；内部入口仍逐片验证当前控制位 exact；
- position 与 normal 共用一次 3 列 SSE materialization；每个 synchronous coarse task 或
  async job 内连续相同 `groupSlot` 跨 64-vertex cancel fragment 复用同一份 48-byte matrix，
  只有 slot transition 或进入新的 task/job 才重新载入。每个输出分量的 mul/add 括号、position
  translation、normal 无 translation、无 normalize/FMA 的语义均不变；
- bounded task queue、pending batch、owned bytes、per-batch pinned static 和 output bytes
  都有硬门；跨 resource-cache/pending-batch 的全局 pinned-static 预算尚待集成层补齐；
- async task 以一组完整 jobs 为粒度，不把单顶点或单 native call 变成 task；
- owner 可执行有限 `assist`，但不会等待其他 worker；
- 每个 job 独立持有 token、epoch、consumer descriptor、state、cancel 和 output slice；
- CPU output lease 只证明 staging bytes ready，不授权 native bypass，也不是现有 GPU
  `OutputLease`。

job 终态用 CAS 单次结算，`cancelJob` 与 worker 的 `Ready` 竞争不能互相覆盖。batch cancel
会立即封闭尚未 terminal 的 job；已经发布的 CPU staging 也因 batch cancelled 而不可再由
lease 消费。

### 4.1 reset、shutdown 与迟写

async reset 推进非零 generation、drain queued tasks，并使 running job 在最多
`cancelCheckPeriodVertices`（配置会硬夹到 `1..64`，默认 64）后观察失效。旧 snapshot、palette 和 CPU output 由
任务的 shared ownership 保留到自然结算；旧 lease 因 generation mismatch 立即失效。

`runSynchronous` 的 producer-owned staging lease 有更强合同：

1. 每个 queued range 在 reset/shutdown drain 时标记 cancelled 并 decrement
   `remainingTasks`；
2. 每个 running range 至少每 64 顶点检查 generation/stopping，退出时也 decrement 并
   notify；
3. render thread 在 `remainingTasks == 0` 前绝不返回；
4. worker 和 owner assist 始终只写自有 staging；全部任务结算、generation 与 MXCSR 再次
   exact 后，producer 只发布拥有该 staging 的 `CpuSkinMtSynchronousOutput`，API 不接收也
   不触碰 caller output；
5. native 接入层只能在 lease 有效时，由同一 render owner 在 Game.dll caller-owned SEH
   transaction 中执行最终 copy，并对 normal/exception/reset 显式结算；producer 的 DWARF2
   栈上不再持有任何可能被 mapped-VB fault 跳过析构的大对象；
6. 因此 native commit 之前 caller mapped VB 保持未触碰，producer 返回后也不存在 worker
   迟写；staging 的全局 owned-byte reservation 一直跟随 lease，不能无账累积；
7. shutdown 还会唤醒并 join 所有 persistent worker，绝不 detach。

最终 owner copy 必须由接入层放在 Game.dll `0x6F0EEB7B..0x6F0EEB8A` 的 caller-owned
SEH 区间；producer API 已完全移除外部 destination 参数，不尝试用 MinGW DWARF2 C++ RAII
跨 Win32 access violation。这样即使 mapped VB 提交故障，也不会遗失 producer 栈上的
staging/kernel/static ownership；native sidecar 必须在异常结算或下一 transaction/reset 时
显式释放 lease。

`runSynchronous` 必须由创建 producer 的 owner thread 调用；在同一个 owner thread 上，
reset 不可能与该阻塞调用重入并发。跨线程销毁一个仍在执行成员函数的 C++ 对象不属于合法
生命周期，集成层必须先 quiesce owner。`reset()` 的 wrong-thread 返回值固定为 0，
`shutdown()` 返回 false，析构 wrong-thread 则 fail-hard；调用者不能把拒绝误当成已 join。

## 5. 两种接线路线

### 5.1 Phase A：hook-local synchronous fork/join

目标是最小化资源语义变化：当前 native CPU kernel 已经 Lock 后，冻结本次调用的全部
静态输入和 palette；worker/owner 把互斥 range 写入自有 staging，在原函数 return/Unlock
之前等待全部 range并取得 lease，最后由独立 native transaction 在原生 SEH 中提交到
mapped VB。producer 不持有 destination。

优点：不跨 native 输入寿命、不新建跨帧 output authority、仍走原 VB/Unlock/DIP。缺点：
barrier 固定成本无法隐藏，且每帧冻结大静态输入会直接否决收益。因此真正接线前必须把
snapshot 绑定到独立资源 owner；hook-local 只能复制本帧 palette/descriptor。

Phase A 的额外 native 门：

- 在同一个 ASM 证明窗口冻结所有 scratch source、count/stride/format、palette 和 mapped
  destination；
- 证明被替换 kernel 的 normal-return side effects，包括 scratch pointer advance/return
  状态，不被后续 native 代码观察；否则继续调用原 kernel；
- 任一 SafeCopy、epoch、format、slot、output coverage 或 generation 证明失败均走原 CPU；
- native ASM 最终审查进一步收窄：Phase A 首轮只能考虑原生 SSE format 2 exact gate；
  generic 0/4 会推进六个 GX source globals，当前纯 helper 不能替代这些 side effects；
- 顶点数必须达到实测同步 crossover；当前参数模型不构成授权。

### 5.2 Phase B：Common 粗批提前生产，native kernel 原位提交

只读合同复核否决了“在 manager `onUpload` 建任务”的早期设想。推荐 Phase B 不扩展 GPU
callback ABI 9，也不把 CPU 输出送入 GPU output arena：

1. Common exact-negative seal 只负责确认该 dispatch 不含 GPU-skin candidate，并为独立
   CPU-MT sidecar 提供 dispatch 级粗批边界；
2. 首轮只学习精确
   `(renderablePart, layer, geosetData, source-layout, format, upload-ordinal)` 序列；后续只有
   template、map/device/content/layout generation、源指针/stride/count 全部 exact 时才提前提交；
3. 在 palette 已绑定、首个 upload 尚未发生的最早已证地址一次提交多个完整 job；worker
   只写 producer-owned staging，绝不访问 Game.dll、D3D9 或 mapped VB；
4. 每个 native outer 只做 O(1) ordinal/token 绑定。shape/source/palette/MXCSR/generation
   任一漂移会取消该 job及剩余 batch，不能把旧模板当授权；
5. 唯一合法最终替换点仍是 `0x6F0EDDC0` kernel detour。只有 `readyBeforeKernel` 的 lease
   才能在原 caller SEH 范围内复制到真实 mapped dynamic VB；未 Ready 立即调用 original
   exactly once，帧路径不 wait/join；
6. outer 的 globals、ensure/Lock/ring/Unlock、FVF/stream publication 和后续所有原生消费者
   保持 exactly once。成功提交的是普通 native VB bytes，因此不新增 GPU consumer、poison、
   index ticket 或 manager ledger；
7. 首发只允许 native SSE format 2。format 0/4 的 generic kernel 会推进 GX source scratch
   globals，未证明等价前必须保留原生；如果 palette 只能到首个 outer 才取得，首 job也直接
   fallback，只提前生产后续 job。

CPU-MT controller 应由 device lifecycle owner 独立持有，并与 manager 共用 reset/quiescence
边界但不进入 manager mutex/map 热路。static snapshot cache、batch pinned static、palette、
staging、metadata 和仍存活的 retired generation 必须共享一个跨 epoch 全局预算；reset 只失效，
真实 shared owner 析构前不能提前归还额度。

### 5.3 为什么否决 manager `onUpload`

bridge 的 `onUpload` 位于 outer 和原 CPU kernel 已经完成之后，无法替换当前调用；manager
`noteNativeUpload` 随后还要持 mutex 做 pairing、map、layout 和 palette proof。把 scheduler
接在这里既太晚，又会重新引入约 `51.62` 次/frame CPU-only upload 的正路径固定税，而全部
原 kernel 的回收天花板只有约 `0.217 ms/frame`。因此该回调最多保留 `1/127` sampled
telemetry，不能作为 CPU-MT admission 或 scheduler。

### 5.4 staging lease 不是授权

`CpuSkinMtOutputLease` 只证明某份 owned staging 已按冻结 descriptor 计算完成。它不能自行：

- 证明当前 live palette、static generation、ordinal/token、format、MXCSR 仍相等；
- 承诺可以跳过 native kernel；
- 写 mapped VB、分配 GPU lease或退休任何 GPU buffer；
- 在 reset、异常或 template mismatch 后继续被消费。

独立 native sidecar 必须在 kernel 入口重新验证全部 proof，先把含 shared ownership 的 lease
移入 TLS/native transaction，再在 caller-owned SEH 中提交。copy fault 不得被 C++ RAII
吞掉；Game.dll handler继续原流程，sidecar由 normal/exception/下一 transaction/reset 显式结算。

## 6. 纯 Python 离线结果

`python -m py_compile AutoTest/analyze_cpu_skin_mt_offline.py` 通过；直接运行脚本也通过，且
明确记录 `launchPerformed=false`、`deployPerformed=false`、`autoTestPerformed=false`、
`childProcessesCreated=false`。

合同结果：

- FVF 0..5 共 6 种，stride 24/28/32/36/40/44 全部闭合；
- chunk 1/7/64/127/1024 对完整 reference 逐字节一致；
- odd FVF 空 extra 的 white default 与 group-slot OOB 拒绝通过；
- 53 个 medium jobs 被稳定合成 8 个完整-job tasks，覆盖精确一次；
- 16384 顶点按 1024 切成 16 个互斥 range，覆盖精确一次；
- generation 推进后 running job 的旧 lease 不可发布。
- cancel 先完成 `Queued->CpuFallback` 时 worker 不能覆盖回 Running；Ready 先完成时
  `cancelJob` 返回 false 且不遗留矛盾 cancel flag，两条确定性 interleaving 均通过。
- MXCSR sticky status 被排除、rounding mismatch 与未 mask exception 被拒绝；
- synchronous worker 只写 owned staging，caller output 在 owner commit 前保持 sentinel，
  commit 后与 reference 逐字节一致；
- cancel check 的 `-7/0/1/63/64/65/4096` 边界精确夹到 `1..64`。

成本输出是参数敏感性模型，不是 Python benchmark，更不是 War3 FPS。除明确引用的 runtime
sample 外，所有微秒常量都只是 crossover 参数；正式阈值必须由 C++ 运行证据决定。

## 7. 仍需 IDA/运行证据的边界

2026-07-15 的优先 native 证据包已完成：
[cpu_mt_phase_b_native_evidence.md](cpu_mt_phase_b_native_evidence.md)。它用真实 x86 ASM、
xref/SEH、原始字节和当前 sidecar/ABI 源码确认：source/palette 与 output Lock identity 必须分段
冻结；worker staging copy/owner join 必须在 kernel detour 内、`0x6F0EEB8A` 前完成；现有
diagnostics-only Lock/Unlock sidecar 与 ABI 9 原样没有 submit/acquire/commit/abort 合同，不能
直接承担 Phase B。`onUpload` 在本次 native kernel 之后，不能作为当前 upload 的 producer。

因此下列第 2 项的“介入点”已经闭合，但 producer integration、static owner 与 runtime
crossover 仍未闭合：

当前 pure kernel 和 producer 生命周期不需要继续打开 IDA。以下动作发生前仍需要 Game.dll
ASM 复核；如果 IDA 暂时关闭，可以先做 DXVK 侧静态审查和离线测试，不能据此扩大接管：

1. **static owner key**：把 geoset 静态流固定到哪个独立资源 owner，以及其销毁、map reset、
   layout mutation 与 content generation 的精确关系；
2. **Phase A/Phase B native join**：已确认只能在 `0x6F0EDDC0` kernel detour 提交到
   `mappedDst`，不能替换 `0x6F0EEA50` outer；Phase B 必须在更早的 flush assembly 提交 worker，
   再以 exact token 在 detour non-blocking acquire。现有 ABI 9 仍缺这层接线；Phase A 首轮的
   format-2 exact gate、owner commit 和 normal-return 诊断亦未实现，generic 0/4 保持原生；
3. **null normal**：native 确有 null-normal default，但当前核心选择拒绝；若要支持，必须再次
   读取对应 ASM 并做 parity，不能猜默认向量；
4. **odd FVF extra**：0xffffffff 与真实 extra pointer 的各 path/stage 使用频率和精确来源；
5. **新的 CWorld 语义声明**：本设计没有、也不需要声称已经完整恢复 `CWorld` 子类层次和
   全部成员函数。任何依赖新字段/虚函数的优化都要单独逆向。

以下问题主要需要运行取证而不是更多 IDA：bucket-specific native SSE 时间、实际 C++ MT
vertex throughput、flush lead 分布、batch upload 固定成本、owner assist 比例、reset/resize/
第二进程生命周期，以及 medium crossover。

## 8. Test Conductor 后续门

本文件所有者没有执行 build/deploy/War3。集成时按以下顺序交给唯一 Test Conductor：

1. 独立静态复核 header/cpp 的 race、generation、32-bit size、shutdown/join；
2. 接入层补 static snapshot resource cache 的 mutation/reset invalidation、全局 pinned-static
   预算和 device quiescence 提前 shutdown，再决定 Meson/owner 接线；
3. 先做 build-only，并反汇编确认纯 kernel 仍为预期 SSE `mulps/addps`，没有 FMA、
   fast-math reassociation 或 x87 路径；
4. C++ 纯内存 parity：6 FVF、null/stride/OOB、chunk、MXCSR、staging commit、
   cancel-vs-ready、reset/shutdown；
5. isolated correctness：只在隔离桌面验证同一姿态字节、fallback、resize/reset/relaunch，
   不把 isolated frame advance 判为 FPS；
6. 用实际 bucket 数据求 tiny/native、medium/CPU-MT、large/GPU crossover；
7. 用户明确让出前台后，最后才进入 foreground dual_perf 发布护栏。
