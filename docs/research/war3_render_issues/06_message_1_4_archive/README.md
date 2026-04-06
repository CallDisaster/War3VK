# 06 - 留言1~4夜间专项归档

## 归档目的
本页用于固化本轮夜间专项（留言1~4）的研究闭环，确保后续迭代不再重复踩坑：
1. 每条留言的目标、实现、验证证据可回溯；
2. 关键地址、关键 Hook 与配置开关集中可查；
3. 遗留风险与下一步动作明确，不依赖口头记忆。

## 留言1：JASS VM 性能研究
### 目标
- 弄清 JASS 主循环是否是 CPU 大头；
- 建立“可观测”而不是“猜测”的优化基础。

### 实现
- AddressBook 补齐：
  - `executeJassFunctionInternal = 0x7F2D92`
  - `jassInterpreterMainLoop = 0x7F1A20`
- Hook 新增：
  - `Hook_ExecuteJassFunction`
  - `Hook_ExecuteJassFunctionInternal`
  - `Hook_JassInterpreterMainLoop`
- 统计能力：
  - completed / timeout / paused / native_error / stack_error / arith_error
  - 预算策略（override / adaptive）

### 代码落点
- `src/d3d9/war3/hooks/war3_hook_jass.cpp`
- `src/d3d9/war3/hooks/war3_hook_address_book.h`
- `src/d3d9/war3/hooks/war3_hook_address_book.cpp`
- `src/d3d9/war3/core/war3_internal_test_config.h`

### 产出
- 已能回答“JASS 是否 timeout 驱动卡顿”；
- JASS 不再是 `Other/Untracked` 的黑箱。

---

## 留言2：局部合并提交（可生效）
### 目标
- 在不重写全队列的前提下，先拿到可见收益。

### 实现
- `Dispatch Local Merge`：同 `renderablePart` 连续 dispatch 时复用 `ExecBatch Begin/End` 上下文；
- 在 `FlushAndReset` 帧尾强制收口，避免跨帧污染。

### 代码落点
- `src/d3d9/war3/hooks/war3_hook_render.cpp`
- `src/d3d9/war3/hooks/war3_hook_render.h`
- `src/d3d9/war3/hooks/war3_hook_lifecycle.cpp`

### 产出
- 保持渲染语义不变；
- 降低 `Dispatch_*` 热路径桥接开销。

---

## 留言3：非合批优化路线评估并落地
### 目标
- 找到“除了合并批次提交”之外还能稳定压 CPU 的路径。

### 实现
- 新增 `Dispatch Tag/Stage` 线程本地缓存；
- Common/Special + reimpl 回调统一走缓存查询；
- 帧尾同步清空缓存。

### 代码落点
- `src/d3d9/war3/hooks/war3_hook_render.cpp`
- `src/d3d9/war3/core/war3_internal_test_config.h`

### 产出
- 减少 `GetTagStage` 高频重复探测；
- 与 Local Merge 形成“并行增益”。

---

## 留言4：静态阴影根因收敛与实现
### 目标
- 解决“关闭阴影后静态建筑/可破坏物贴花仍渲染”的老问题；
- 明确避免只在末端 `ListA/ListB` 粗拦截。

### 关键结论（IDA）
- 静态阴影核心上游写入入口是：`TerrainShadow_RegisterImageEntry(0x713250)`；
- 主要来源：
  - `TerrainShadow_ToggleStaticStampFromObject(0x74DB30)`
  - `TerrainShadow_ToggleEmitterStamp(0x74DF50)`
- `CWorld_TerrainShadow_Dispatch(stage14)` 仍会直调 `RenderListB(type=4)`，因此末端拦截只能兜底。

### 实现
- 新增 `RegisterImageEntry` Hook，并按返回地址判定来源进行拦截：
  - `mode>=1` 阻断 static/emitter stamp 来源注册；
- 保留 ListB/WriteEntry 作为诊断兜底，不再作为主策略。

### 代码落点
- `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
- `src/d3d9/war3/hooks/war3_hook_shadow.h`
- `src/d3d9/d3d9_war3_hook.cpp`
- `src/d3d9/war3/hooks/war3_hook_address_book.h`
- `src/d3d9/war3/hooks/war3_hook_address_book.cpp`
- `src/d3d9/war3/core/war3_internal_test_config.h`

### 产出
- 从“渲染末端截流”升级为“注册上游截流”；
- 更容易精准控制，不易误伤雾/边界类条目。

---

## 统一验证清单（后续回归直接照此执行）
1. 编译：`ninja -C build32`。
2. 场景验证：
   - 建筑/可破坏物静态阴影是否消失；
   - 雾/边界/选择圈是否正常。
3. 日志验证：
   - `DispatchLocalMerge ... reusePct`；
   - `DispatchTagStageCache ... hitPct`；
   - `RegisterImage stats ... static/emitter/blocked`；
   - `Jass MainLoop stats ... timeoutPct`。
4. 性能验证：
   - `Hook_Dispatch_Common/Special` 平均 CPU 是否下降；
   - `Other/Untracked` 是否继续收敛。

## 踩坑清单（必须规避）
1. 不要只在 `ListA/ListB` 末端做粗拦截：`stage14 -> ListB(type=4)` 直调链会绕过部分开关，必须以前置注册拦截为主。
2. 不要把 `LastRenderHandle` 直接用于描边精确匹配：应保持 `strictBatchHandle`（描边）与 `lookupHandle`（回查）分离，避免“全体描边”回归。
3. 不要把合批优化和语义改写混在一起：`Dispatch Local Merge` 只能做上下文复用，不能破坏原 `layer/state` 切换语义。
4. 不要在 JASS 预算策略里默认激进 override：必须先看 `MainLoop timeoutPct`，再按场景启用 adaptive/override。

## 证据索引（复盘时优先看）
1. 代码入口：
   - `src/d3d9/war3/hooks/war3_hook_jass.cpp`
   - `src/d3d9/war3/hooks/war3_hook_render.cpp`
   - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
2. 地址簿：
   - `src/d3d9/war3/hooks/war3_hook_address_book.h`
   - `src/d3d9/war3/hooks/war3_hook_address_book.cpp`
3. 配置开关：
   - `src/d3d9/war3/core/war3_internal_test_config.h`
4. 日志关键字：
   - `DXVK War3Hook: DispatchLocalMerge`
   - `DXVK War3Hook: DispatchTagStageCache`
   - `DXVK War3Shadow: RegisterImage`
   - `DXVK War3Jass: MainLoop`

## 遗留风险与后续计划
1. `RegisterImageEntry` 来源拦截仍需多地图样本验证（尤其自定义地图特效）。
2. 若存在漏网条目，再补“来源 + key + callbackRVA”三维规则。
3. 合批下一阶段优先做 `ExecBatch` 对象缓存命中率优化，再评估全队列接管。
