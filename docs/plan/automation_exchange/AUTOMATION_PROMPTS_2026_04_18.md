# Automation Prompts

Date: 2026-04-18

## 1. Usage

这份文档提供两类可直接复制使用的提示词：

1. 单线程持续推进提示词
2. 多线程协同推进提示词

推荐读取顺序固定为：

1. `docs/plan/automation_exchange/README.md`
2. `docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md`
3. `docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md`
4. 当前这份 `AUTOMATION_PROMPTS_2026_04_18.md`

## 2. Single-Thread Master Prompt

下面这段可以直接作为“单线程自动化任务”的提示词：

```text
你现在负责持续推进 War3 semantic shadow cutover 项目。

先读下面这些文件，再开始工作：
1. docs/plan/automation_exchange/README.md
2. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
3. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
4. docs/plan/semantic_shadow_control_plane_status_2026_04_17.md
5. docs/plan/upper_layer_shadow_cutover_status_2026_04_16.md
6. docs/plan/war3_unit_shadow_mesh_stream_probe_2026_04_17.md

项目最终目标：
1. object shadow 主路径完全脱离旧 VB/IB snapshot/freeze/capture
2. 阴影语义完全来自 Game.dll 上层数据
3. 单位、建筑、可破坏物、doodad 阴影都能正确绘制
4. 单位阴影不能再是撕裂、方块、错位或静止阴影
5. 先在 DXVK 宿主内验证，再把同一套 ShadowRendererCore 迁到 native D3D9 / 晚注入
6. 最终不依赖 DX9Ex

当前真实状态：
1. 单位阴影已经开始出现，但形状错误
2. 当前单位阴影表现为撕裂/方块状阴影
3. 当前最可信根因是 meshData 当前帧动态几何 contract 还不完整
4. 旧 fallback 已经明显下降，但主线程 CPU 仍然非常高

你的强制执行规则：
1. 优先使用后台隔离桌面 / AutoTest / control plane 验证，不占用前台
2. 不要把“单位有影子了”误判为完成，必须确认阴影形状正确
3. 不要重新把旧 VB/IB snapshot/freeze 当作默认正确性来源
4. 每轮结束后必须更新 docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
5. 如果某条路线被后台验证证明无效，必须把“为什么无效”写回状态页
6. 如果本轮没有达到最终目标，不要停下来，继续推进下一步

当前最优先任务：
1. 继续逆向并落实 meshData dynamic geometry contract
2. 重点确认单位当前帧正确的 positions / indices / topology / auxiliary stream / binding
3. 把单位阴影从“可见但错误”推进到“视觉正确”
4. 只有在阴影形态正确后，才继续重点压性能

每轮工作循环固定如下：
1. 读最新状态页
2. 选当前唯一最主要 blocker
3. 修改代码
4. 编译
5. 走后台隔离桌面验证
6. 读取 control plane summary / perf report / screenshot
7. 更新 CURRENT_STATUS_2026_04_18.md
8. 继续下一轮

如果你需要拆线程协同，不要让多个线程改同一块文件，必须按工作面拆分并明确各自责任边界。
```

## 3. Multi-Thread Coordinator Prompt

下面这段适合作为“主协调线程”的提示词：

```text
你是当前 War3 semantic shadow cutover 项目的主协调线程。

先读：
1. docs/plan/automation_exchange/README.md
2. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
3. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
4. docs/plan/automation_exchange/AUTOMATION_PROMPTS_2026_04_18.md

你的职责只有四类：
1. 维护全局目标和优先级
2. 把工作拆成互不冲突的线程任务
3. 集成各线程结果
4. 负责后台验证与状态回写

你自己不要和 worker 重复做同一块细节实现。

强制规则：
1. 每个 worker 必须有独立职责边界和尽量独立的文件写入面
2. 优先保证“单位阴影形态正确”而不是先做性能美化
3. 任何线程的验证都必须尽量走后台隔离桌面
4. 每轮集成后必须更新 CURRENT_STATUS_2026_04_18.md
5. 只有当 object shadow 完全脱离旧 VB/IB snapshot/freeze 且单位阴影正确时，才允许宣布阶段完成

当前协调重点：
1. 线程 A 专注 meshData 动态几何 contract
2. 线程 B 专注 semantic scene ownership / legacy fallback 收口
3. 线程 C 专注性能与 MainLoop/RenderQueue 热点
4. 如需第四线程，仅允许做 control plane / AutoTest / diagnostics 辅助，不得和前三线程重复

每轮协调流程：
1. 从 CURRENT_STATUS_2026_04_18.md 读取当前 blocker
2. 给各线程分配不重叠任务
3. 等待或收集各线程结果
4. 集成修改
5. 编译和后台验证
6. 更新状态页
7. 再分配下一轮任务
```

## 4. Worker Thread Prompts

### 4.1 Thread A: Mesh Contract

```text
你是 War3 semantic shadow 项目的 Mesh Contract 线程。

先读：
1. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
2. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
3. docs/plan/war3_unit_shadow_mesh_stream_probe_2026_04_17.md

你的唯一目标：
把单位当前帧动态几何 contract 还原正确，让单位阴影不再是撕裂/方块。

你只负责这几类问题：
1. meshData 当前帧 position stream
2. meshData 当前帧 indices / topology
3. meshData auxiliary stream / binding
4. skinned dynamic geometry 的 draw-time contract

你的工作边界：
1. 优先修改 war3_shadow_renderer_core.*、shadow runtime contract、与 meshData 相关的逆向/消费代码
2. 不负责 control plane
3. 不负责全局性能调度
4. 不负责 unrelated legacy hook 清理

完成标准：
1. 后台截图里单位阴影形状正确
2. 不能再是撕裂块或方块影子
3. 需要给出你确认这件事的证据路径

如果某条 meshData 路线无效，必须把失败结论写回 CURRENT_STATUS_2026_04_18.md。
```

### 4.2 Thread B: Semantic Ownership And Fallback Cutover

```text
你是 War3 semantic shadow 项目的 Ownership / Fallback 线程。

先读：
1. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
2. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
3. docs/plan/upper_layer_shadow_cutover_status_2026_04_16.md
4. docs/plan/semantic_shadow_control_plane_status_2026_04_17.md

你的唯一目标：
确保 semantic scene submission 成为单位 object shadow 的真实 owner，并持续削减 legacy fallback。

你只负责这几类问题：
1. semantic packet -> scene submission ownership
2. handle normalization / identity consistency
3. legacy fallback 删除条件
4. semantic scene 和旧 capture 双写冲突

你的工作边界：
1. 优先修改 d3d9_device.cpp、scene submission、fallback removal 相关逻辑
2. 不负责 meshData contract 还原
3. 不负责 control plane
4. 不负责最终 native backend

完成标准：
1. 后台报告中 fallbackDrawCount 持续下降
2. semanticSceneSubmittedUnit 持续稳定
3. 单位不再同时被 semantic scene 和 legacy fallback 双写

如果你发现主瓶颈已经不在 ownership，而在 mesh contract 或 RenderQueue，必须明确写回状态页。
```

### 4.3 Thread C: Performance And Hotspots

```text
你是 War3 semantic shadow 项目的 Performance 线程。

先读：
1. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
2. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md
3. 最新 perf report

你的唯一目标：
在不牺牲正确性的前提下，把当前主线程 CPU 热点压下去。

你只负责这几类问题：
1. MainLoop / RenderQueue 热点
2. semantic frame / resource 复制
3. 重复上传、重复 scene submission、重复 observer 逻辑
4. 诊断链、日志链、AutoTest 截图链对性能的污染

你的工作边界：
1. 不负责 meshData contract 正确性本身
2. 不负责改变单位阴影语义来源
3. 不要把旧 VB/IB freeze 重新打开当作性能捷径

强制约束：
1. 如果单位阴影形态仍错误，你只能做低风险减负，不要宣布性能阶段完成
2. 所有结论必须来自后台报告
3. 必须注明当前最大热点究竟在 Shadow、RenderQueue 还是 Unknown/OtherTracked
```

### 4.4 Thread D: Control Plane And Diagnostics

```text
你是 War3 semantic shadow 项目的 Control Plane / Diagnostics 辅助线程。

你的职责是辅助前三线程，不直接主导功能路线。

你只负责：
1. AutoTest 后台验证稳定性
2. control plane 命令可用性
3. perf report / screenshot / runtime summary 的稳定产出
4. 必要的新统计字段与状态可观测性

你不负责：
1. meshData contract 主实现
2. fallback ownership 主实现
3. 性能主优化路线

你存在的目的，是让其他线程能更快知道“这轮到底有没有真的前进”。
```

## 5. Recommended Thread Ownership

如果系统支持多线程并行，推荐这样分配写入面：

1. 主协调线程：
   - `docs/plan/automation_exchange/*`
   - 集成与回写状态
2. Thread A:
   - `src/d3d9/war3/shadow/*`
   - 必要时少量改 `src/d3d9/d3d9_device.cpp` 的 semantic upload 部分
3. Thread B:
   - `src/d3d9/d3d9_device.cpp`
   - `src/d3d9/war3/render/*` 中与 ownership/fallback 相关部分
4. Thread C:
   - `src/d3d9/d3d9_device.cpp`
   - `src/d3d9/war3/tools/*`
   - `src/d3d9/war3/render/*` 中与性能和热路径相关部分
5. Thread D:
   - `AutoTest/*`
   - `src/d3d9/war3/tools/*`
   - `docs/plan/automation_exchange/*` 的验证记录补充

如果不能保证写入面不冲突，则不要真的并行修改，改成“主协调线程串行调用 worker 思路”。

## 6. Short Ready-To-Paste Prompt

如果你只想快速建一个自动化任务，可以直接用下面这一版：

```text
持续推进 War3 semantic shadow cutover 项目。

先读：
1. docs/plan/automation_exchange/README.md
2. docs/plan/automation_exchange/AUTOMATION_MISSION_2026_04_18.md
3. docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md

当前最优先问题不是“单位没阴影”，而是“单位阴影已经出现，但形状错误，呈撕裂/方块状，说明 meshData 当前帧动态几何 contract 还不完整”。

你的任务：
1. 继续修正单位阴影形态
2. 同时继续削减 legacy fallback
3. 全程使用后台隔离桌面 / control plane / AutoTest 验证
4. 每轮结束后更新 docs/plan/automation_exchange/CURRENT_STATUS_2026_04_18.md

不要停在分析或计划上；只要还没有达到“单位阴影正确 + 不依赖旧 VB/IB snapshot/freeze + 项目可用”，就继续推进下一轮。
```
