# GPU 蒙皮 P1-P4 多 Agent 执行计划

> 日期：2026-07-10
> 状态：本文保留 2026-07-10 的 P1-P4 原始执行波次；截至 2026-07-20，旧 compute/bypass、
> VS-A、VS-B0 与独立 VS-B1 普通图正确性门已有隔离证据，性能主线见下方增量。takeover/skip
> 仍默认关闭；VS-B1 lifecycle 的最后一笔 sampled reset 分类修复尚未部署复跑。
> 技术基线：`docs/research/war3_render_issues/28_gpu_skinning_takeover_feasibility/README.md`

> 2026-07-19 增量：旧 compute/bypass 虽已通过正确性门，但 ABBA 为约
> `+2.35 ms/frame` CPU 负增量，性能主线已转向 fixed-function VS-in-draw hybrid。真实
> consumer-fenced input lease 与 VS-A ubershader prelude 已在显式路线隔离通过。显式 VS-B0
> `vertex_shader_input_only` 又完成了“省略 compute output/dispatch、Main/Shadow 直接消费 input
> lease、但保留 CPU kernel 与 P4 零权限”的独立中间门；普通图、lifecycle 与高压图均 PASS。
> Special/非目标 layout 已有正覆盖，transparent 自动图计数仍为 0，只具备静态 fail-open 证明。
> 下一门是独立 VS-B1 kernel bypass preflight，不能直接把 VS-B0 升权。本文 P1-P4 波次继续作为
> 旧路线的正确性与回退基线，不再代表当前性能实现顺序。

> 2026-07-20 增量：独立显式 `vertex_shader_bypass`（VS-B1）已接通 kernel 前 exact input
> capability、WVS2 Main prelude 与 Shadow static-atlas direct input。普通隔离 crash-gate PASS：
> Main/Shadow `22,055/22,055`，CPU kernel 跳过 `22,055` 次 / `415,749,920 B`，compute output/job
> 省略 `23,359` 次 / `440,855,616 B`，poison/index/ledger 全闭合。Outline 在 kernel 前明确拒绝，
> transparent 与非目标格式继续旧路径。lifecycle 的窗口、reset 与第二进程 B1 数据已经闭合，但
> runner 仍因 dip-fast sampled reset 冷窗口误分类严格 FAIL；第二笔重分类修复已 build 为
> `1975BB89...E151`，因用户启动 World Editor 未部署/复跑。下一步不是扩大白名单，而是先完成该
> lifecycle 门，再做高压/Special/透明覆盖和 isolated A/B；foreground dual_perf 仍需用户授权。

## 1. 总目标

在保持 War3 1.27a 动画树和本帧 group palette 生产不变的前提下，将最终逐顶点 CPU skin 与动态 VB 写入迁移到 DXVK GPU compute，并让同一份本帧 post-skin output 被主画面、CSM、点阴影与描边共享。

执行范围：

- P1：CPU/GPU 双跑与差分验证；
- P2：阴影优先消费 GPU output；
- P3：主画面 one-shot stream override；
- P4：在严格 gate 下跳过原 CPU skin/upload。

第一轮明确不做：

- GPU 动画树、轨道插值或骨骼层级求值；
- 粒子、ribbon、UI、地形等非普通 `CGeosetData` producer；
- 每 draw compute dispatch；
- 移除逐 draw CPU fallback；
- 顺带重构点光、体积光或 CSM 算法。

## 2. 协作硬规则

### 2.1 并发上限

- 主线程之外最多同时运行 **3 个子 Agent**；
- 内存压力升高时立即降到 2 个；
- 完成且没有立即 follow-up 的 Agent 立刻 close，避免占用并发槽和上下文；
- 不让两个 Agent 同时修改同一个高冲突文件。

### 2.2 唯一 AutoTest 所有者

任一时刻只能存在一个 `Test Conductor`：

- 只有它允许运行 `ninja -C build32` / `build32_safe.cmd`；
- 只有它允许部署 `E:\Work\War3\d3d9.dll`；
- 只有它允许启动/停止 Warcraft III、运行 AutoTest、截图和读取正式性能报告；
- 其他 Agent 禁止启动游戏、部署 DLL 或运行 AutoTest；
- 主线程也不与它并发启动测试；
- 每轮测试使用唯一 artifact 前缀，并在结果中记录 DLL hash、环境变量和地图。

建议新增进程级/文件级测试锁；即使调度失误，第二个测试进程也应直接拒绝启动。

### 2.3 ASM 逆向要求

凡涉及以下内容，Agent 必须使用 `gpt-5.6-sol + ultra`：

- Game.dll render/GxDevice/RenderQueue 语义；
- hook ABI、寄存器参数、栈参数、SEH/prologue；
- `0x6F0EEA50` bypass 副作用；
- palette slot、FVF、ring/base vertex；
- crash root cause 与 native lifetime。

交付要求：

- 必须阅读 IDA disassembly/ASM；
- 伪代码只能辅助，不得作为唯一证据；
- 输出 VA/RVA、关键指令、寄存器/栈参数和调用者/被调者；
- 明确区分 `Confirmed / Inference / Unknown`；
- 对已有文档结论做一致性审计。

### 2.4 工作区与文件所有权

- 所有 Agent 都必须先读 `AGENTS.md` 与 GPU skin 专项文档；
- 工作区有大量未提交改动，不得回退他人改动；
- worker 只修改分配给它的文件；
- 中央高冲突文件由单一 owner 修改；
- `meson.build`、`AGENTS.md` 和主计划文档默认由主线程整合；
- worker 不得擅自格式化无关文件。

## 3. Agent 角色池

### R：Native Reverse Auditor

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-sol`, `ultra` |
| 类型 | explorer/read-only；必要时只改逆向文档与 IDA 注释 |
| 职责 | ASM 锁定 hook ABI、调用配对、必要副作用、异常路径和回退条件 |
| 禁止 | 不写 GPU/DXVK 生产代码，不运行 AutoTest |

首轮任务：

1. 对 `0x6F0E35B0 / 0x6F0EEA50 / 0x6F0EDDC0 / 0x6F0EEC20 / 0x6F0EE9F0` 做最终 ASM 契约表；
2. 确认一个 upload 对应多少 DIP、是否存在多 draw 复用；
3. 确认 skip outer 时必须模拟的所有字段和函数；
4. 确认 format `0/2/4` 覆盖率假设及 `1/3/5` 入口；
5. 给 Native Integration Agent 一份可直接编码的 gate/fallback 清单。

后续 follow-up：

- 审核 P3 stream override patch；
- P4 前再次逐指令审核 bypass；
- crash 时只做 ASM 归因，不与实现 Agent 抢写文件。

### C：Compute Kernel Owner

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-sol`, `xhigh` |
| 类型 | worker |
| 文件所有权 | `war3_gpu_skin_compute.*`、`war3_gpu_skin.comp` |
| 职责 | SPIR-V shader、DxvkSpirvShader 构造、descriptor contract、dispatch 与 hazard 跟踪 |

要求：

- 输入按 raw float/uint 布局，避免 `mat4/std430` 转置歧义；
- group slot 按 packed byte 解包；
- 完全复刻 native 3x4 position/normal 公式；
- normal 不 normalize，不 inverse-transpose；
- output 使用精确 FVF byte offsets；
- shader binding metadata 正确声明 read/write；
- compute 必须由 flush 级 batch 调用，不提供 per-draw production API。

后续 follow-up：

- P1 mismatch 修复；
- P2 output reuse；
- P4 job packing/dispatch 优化。

### M：GPU Resource/Lifetime Owner

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-terra`, `high` |
| 类型 | worker |
| 文件所有权 | `war3_gpu_skin_resources.*`、`war3_gpu_skin_types.h` |
| 职责 | static geoset cache、palette/job upload ring、device-local output arena、generation/fence 回收 |

要求：

- key 至少包含 `CGeosetData* + contentHash/revision`；
- 地图卸载、device reset、资源释放时失效；
- 首次资源 miss 只能回退 CPU，不在 draw 热点同步重建大资源；
- output buffer usage 同时包含 STORAGE 与 VERTEX；
- 不用固定三帧轮转冒充 fence correctness；
- 所有跨 CS/GPU 使用的资源由 `Rc<>` 持有。

后续 follow-up：

- arena overflow/fragmentation 优化；
- 静态资源异步上传；
- 跨 pass output lease。

### N：Native/D3D Integration Owner

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-sol`, `ultra` |
| 类型 | worker |
| 文件所有权 | `war3_hook_render.cpp`、`war3_hook_address_book.*`、`d3d9_device.cpp/.h` 中 GPU skin 专区 |
| 启动条件 | R 的首轮 ASM 契约完成并由主线程签收 |
| 职责 | flush batch 发布、native upload token、DIP matching、stream/base override、fallback |

要求：

- P1 只观测/双跑，绝不跳过原函数；
- P3 在 `PrepareDraw` 后直接绑定 output slice；
- 主 draw 与 geometry outline 共用 output；
- 函数退出后重新置 `VertexBuffers` dirty；
- 所有 early return、debug skip、auto-instancing、index split 都必须清理或拒绝 pending contract；
- `War3TryCaptureShadowCasterDrawIndexed` 必须能消费 GPU output，不能继续读取旧 D3D9 stream；
- P4 bypass 只能在 R 审核通过后启用。

### D：Diagnostics/Reference Owner

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-terra`, `high` |
| 类型 | worker |
| 文件所有权 | 新 GPU skin diagnostics/reference 模块；perf/control-plane 的窄字段 |
| 职责 | CPU reference kernel、差分统计、token/coverage/mismatch counters、dump 格式 |

核心指标：

- eligible/upload/pair/dispatch/consume/fallback counts；
- format/stride/UV selector histogram；
- native upload -> DIP 距离与一对多分布；
- CPU/GPU position/normal maxAbs/maxRel/RMS；
- UV/unchanged bytes bitwise mismatch；
- stale palette/frameTag、slot ownership、resource generation miss；
- stream restore、pending leak、base vertex mismatch；
- GPU output 被 main/shadow/outline 消费的次数。

### T：Test Conductor

| 项 | 内容 |
|---|---|
| 模型/强度 | `gpt-5.6-terra`, `high`；纯机械复跑可降 `medium` |
| 类型 | worker，唯一测试所有者 |
| 文件所有权 | `AutoTest/gpu_skin_*` 与本轮 artifacts |
| 职责 | 串行 build/deploy/AutoTest、截图、性能与 crash 证据 |

测试地图：

- 低压：`E:\Work\War3\Maps\ShadowTest\光影测试.w3x`；
- 高压：`E:\Work\War3\Maps\ShadowTest\光影测试-高压.w3x`；
- GPU/CPU parity 最好再固定一个少量典型单位、可重复相机与动画的专项 case。

### Q：Independent Patch Reviewer

| 项 | 内容 |
|---|---|
| 模型/强度 | 普通 DXVK patch 用 `gpt-5.6-sol/high`；涉及 native 语义提升到 `ultra` |
| 类型 | explorer/read-only |
| 职责 | 在测试前检查状态泄漏、barrier、资源寿命、错误 fallback 与未覆盖 early return |

Q 不与代码 owner 同时修改文件，只给 findings。

## 4. 执行波次

## Wave 1：P1 基础并行

同时开启 3 个 Agent：

1. **R / ultra**：最终 ASM 契约；
2. **C / xhigh**：compute shader 与 pipeline 独立模块；
3. **M / high**：资源 cache 与 output arena 独立模块。

主线程此时负责：

- 固化公共 C++ contract；
- 审查三方接口是否能无环依赖；
- 不重复做 R/C/M 的工作；
- 准备中央 meson 与 feature flag 接线。

Agent 槽轮转：

- R 完成后立即审阅并保存其 Agent ID；若契约明确，先 close R 腾出实现槽给 N，待 N 形成 hook 草案且有槽位空闲时再 resume 同一个 R 做审核；
- C 完成后，若无 mismatch 调试需求则 close，槽位转给 D；
- M 完成后，槽位转给 Q 做资源寿命审查。

## Wave 2：P1 集成与差分

并行角色：

1. **N / ultra**：只做 observer/token/flush batch 接线，原 CPU path 保持；
2. **D / high**：CPU reference 与差分 counters；
3. **R 或 Q / ultra/high**：只读审查 N/C/M 的契约一致性。

集成后才开启 T：

- 其他 Agent 停止 build/test；
- T 串行执行 build、部署、专项 parity、低压、高压；
- mismatch 时先按来源派回原 owner，不由 T 修生产代码。

## Wave 3：P2 阴影消费

角色：

1. **Shadow Consumer Worker / xhigh**：只改 GPU output -> `War3FrameScene/ShadowCasterDraw` bridge；
2. **C / high follow-up**：优化 output slice 复用与 shadow 输入；
3. **Q / high**：审查 path blocker、alpha、S1、静态阴影和 fallback 不回退。

T 独占测试：

- CPU main + GPU shadow A/B；
- 动画移动时同帧 pose；
- CSM silhouette 对 draw-time VB oracle；
- 路径阻断器、uberSplat、S1 地形、建筑静态阴影回归；
- dual_perf 前台测试。

## Wave 4：P3 主画面 override

角色：

1. **N / ultra follow-up**：stream 0 + `vertexOffset` override；
2. **R / ultra**：针对实际 patch 做 ASM/副作用审计；
3. **Q / high**：D3D9/DXVK 状态污染与 early-return 审查。

P3 仍保留原 CPU upload，只改变正式 draw 的消费 buffer。这样即使画面失败，也能立刻对比 CPU post-skin oracle，不把 bypass 副作用与 GPU 数学混在一起。

## Wave 5：P4 跳过 CPU skin/upload

角色：

1. **R / ultra**：再次确认 bypass checklist；
2. **N / ultra**：只对严格 supported draw 模拟必要副作用并跳过原函数；
3. **Performance/QA / high**：统计 CPU skin call、VB upload bytes、主线程与 GPU 变化。

覆盖率按 gate 逐步扩大：

```text
Stage 11
-> skinMode == 1
-> vertexCount/source pointers match CGeosetData
-> groupStride == 1
-> full live palette
-> static resource ready
-> supported FVF/UV selector
-> exact native-upload/DIP token match
-> no index split / unsupported recursion
```

任一条件失败即原函数 fallback。

## 5. P1-P4 验收门

### P1

- feature off 零行为变化、零新增每 draw 重路径；
- supported draw 的 native upload/DIP 配对错误为 0；
- position/normal 数值误差满足预设阈值；
- UV/非变换字段 bitwise 一致；
- 没有 stale palette、越界 slot 或资源 generation 混用；
- 高低压均不崩溃。

建议初始误差门：

- position/normal `maxAbs <= 2e-5 * max(1, abs(reference))`；
- 若 FMA/SSE 顺序导致极少数超限，必须先输出分布再讨论放宽，不能直接提高阈值。

### P2

- GPU shadow silhouette 与 draw-time post-skin VB oracle 一致；
- 动态 pose 不冻结、不晚一帧；
- path blocker、S1 terrain、uberSplat、静态阴影策略不回退；
- CSM/point/outline 不复制同一 output 多份；
- dual_perf 不出现不可解释回退。

### P3

- CPU main 与 GPU main 截图/序列差分通过；
- geometry outline 正确；
- 后续非 skin draw 不受 stream 污染；
- debug skip、auto-instancing、空 draw、index split 均不会留下 pending binding；
- shadow capture 明确消费 GPU output，而不是旧 ring。

### P4

- 原 CPU skin/upload 调用对 supported draw 实际下降；
- fallback draw 保持原行为；
- `NumVertices/FVF/BaseVertexIndex` 无 mismatch；
- 无 map unload/device reset 崩溃；
- 前台 dual_perf 给出 CPU/GPU/帧率变化，不能只报 FPS；
- 达不到收益时允许保留 P3 共享 output 架构而不强推 P4 全覆盖。

## 6. Agent 完成后的续派规则

### 6.1 优先复用同一 Agent

Agent 完成后，主线程先快速审查：

1. diff 是否只在 owner 文件；
2. contract 是否满足上游依赖；
3. 是否有未处理 fallback/early return；
4. 是否需要原领域上下文继续工作。

若下一任务仍属于同一领域且可以立即开始，使用 `send_input` 续派，而不是新建 Agent：

- R：逆向契约 -> patch 审核 -> crash ASM；
- C：compute kernel -> mismatch 修正 -> batch packing；
- M：arena -> overflow/fence -> reset lifetime；
- N：P1 token -> P3 override -> P4 bypass；
- T：P1 matrix -> P2 matrix -> P3/P4 regression。

这样保留热上下文，减少重复读库时间。

若同域后续必须等待其他 Agent 的产物，则保存 Agent ID 后 close；依赖满足时使用 `resume_agent` 恢复原上下文，再发送增量任务。这样既不长期占用三个并发槽，也不丢失已经完成的逆向或实现上下文。

### 6.2 何时关闭并换 Agent

满足任一条件则关闭：

- 下一任务跨领域；
- 当前 Agent 已给出结论且没有即时依赖；
- Agent 连续两次偏离文件所有权；
- 需要独立复审而不是原作者自审；
- 上下文过重或内存压力明显。

### 6.3 Backlog 补位顺序

空出槽位时按以下顺序补位：

1. 当前 phase 的阻塞依赖；
2. 独立 reviewer；
3. diagnostics/reference；
4. 性能优化；
5. 文档/清理。

不在 correctness gate 通过前派发“顺便优化”和大范围重构。

### 6.4 每次派发的任务包

主线程给任何 Agent 的指令都按下面的固定字段组织：

```text
Role / model / reasoning:
Goal:
Evidence already established:
Files owned (write allowlist):
Files read-only:
Forbidden actions:
Required ASM evidence (native tasks only):
Deliverables:
Verification that this Agent may run:
Phase gate:
Likely follow-up after completion:
```

代码 Agent 的任务包必须提醒“工作区还有其他 Agent，不得回退他人改动”；测试权限一律写明，除 T 外均为“只允许静态检查，不得 build/deploy/运行 War3 或 AutoTest”。任务返回后，主线程先审查 diff 和证据，再决定续派、恢复旧 Agent、交给 reviewer，或进入唯一 T 的串行测试窗口。

## 7. 主线程职责

Ultra 主线程不承担所有机械编码，而负责：

- 保持架构和阶段边界；
- 选择 Agent、强度和文件所有权；
- 审核所有结果后再接入共享主线；
- 处理 `d3d9_device.cpp`、frame graph 和跨模块的最终契约；
- 决定是否通过 phase gate；
- 只在子任务不可安全拆分时亲自实现；
- 每完成一个 Agent 结果就立即补位，不让并行槽空闲；
- 控制测试串行化和回归红线。

## 8. 下一轮计划模式的第一批指令

进入计划模式后，第一批只启动 Wave 1：

1. R：`sol/ultra`，只读 ASM 最终契约；
2. C：`sol/xhigh`，compute shader/pipeline 独立模块；
3. M：`terra/high`，资源与 lifetime 独立模块。

主线程同步建立公共 contract 和 feature gate。R 返回并签收后，立刻用空槽启动 N；C 返回后启动 D；P1 代码完成并经 Q 审查后，最后才启动唯一 T。

这套调度避免了两个已知失败模式：

- 多个 Agent 同时修改 `d3d9_device.cpp`；
- 多个 Agent 同时部署 DLL/运行 War3 导致 AutoTest 撞车。
