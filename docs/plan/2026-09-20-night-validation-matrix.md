# 2026-09-20 夜班 validation-matrix（b08 校对版）：冻结清单、业务帧域与恢复门矩阵

状态：设计/离线矩阵，未执行。本文件只由 architecture-b08 将 b02 文档对齐当前源码；
不授予编译、部署、游戏或 GPU lease。恢复门阈值、固定路线、2560×1440 解隔离桌面约束
和恢复事务均保留。`visualRecoveryProven` 始终为 false。

## 1. 冻结运行清单（唯一输入许可）

`--apply` 必须显式传入 `--frozen-manifest <json>`；runner 不得读取当前 live 后自行批准。
历史 `HISTORICAL_PINS` 只读保留，不继承 identity 许可。缺少或漂移即 fail-closed。

清单必须固定：

- `schema=2`、`runId`、`scenario`、`profile=full_default`、`matrix`。
- `candidate`、`liveRecovery`、`map`、`game`、`exe` 五项精确 `sha256/size`。
- `resolution={2560,1440}`、`isolatedDesktop=true`、`globalInputUsed=false`。
- `env` 至少包含 internal test API、exit test、background/pause、perf monitor/level、
  async screenshot、4000 帧、384 MiB cap、`full_default`、scenario、OBS capture off、
  `DXVK_WAR3_STAGE11_BUDGET_CENSUS=1`、`DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE` 显式 0/1，
  以及全部重型取证开关 `0`。
- `route` 精确等于固定路线：20 s 基线、6 个偏移各 10 s、回原地、80 s relief、
  `angleOfAttack=335`、`duration=1.5`。

## 2. 固定运行变量

| 项 | 当前固定值 |
| --- | --- |
| 候选 | `build32/src/d3d9/d3d9.dll`；精确 SHA-256/size 必须来自本次源码构建后父任务冻结的 manifest，不得沿用文档内旧值 |
| 历史旧值 | `68176AE78808CA2329EB9513502D805426401FC91101134E11C71C104F3C5378` / 36,386,190 B 是 `source-freeze-0338` 时期的 source-stale 基线，不是本批次可部署候选，不得复用为 candidate/liveRecovery |
| 地图 | `E:/Work/Warcraft III/Maps/(4)生与死v1.28读档bug修复.w3x`，以冻结清单 SHA/size 为准 |
| 运行配置 | `full_default`；重型帧/图像/输入/历史取证全关；`DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB=384` |
| 环境 | 非交互隔离桌面；客户区 2560×1440；零全局输入、零桌面切换、零强制终止、零预算提高、零 caster 裁剪 |
| 相机/路线 | 由冻结清单 `route` 唯一决定 |
| 进程 | 启动前 zero-processes；事务只绑定本进程 exact HANDLE、PID、创建时刻、canonical EXE |

## 3. OFF/ON 唯一变量

两轮都必须开轻量页账户 census：

```text
DXVK_WAR3_STAGE11_BUDGET_CENSUS=1
```

仅切换 range 摘要候选：

```text
OFF: DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE=0
ON:  DXVK_WAR3_STAGE11_UPLOAD_INDEX_RANGE=1
```

## 4. 每轮执行顺序（拿到 lease 后）

1. 父任务冻结并交付 manifest；runner 校验 candidate/liveRecovery/map/game/exe、route、
   resolution、profile、matrix、全部 env，任一漂移 fail-closed。
2. 零进程预检、候选/地图/Game/EXE 身份复算、2 GiB 磁盘余量。
3. 创建 own isolated desktop，启动
   `war3.exe -window -loadfile Maps\(4)生与死v1.28读档bug修复.w3x`；保存 `binding.json`。
4. ready 前检：读取 owner runtime status，缺少 module/runtime/render 关键字段必须标记
   `status-incomplete`，不能因默认 false 或默认 ready 通过。pre-ready 诊断不得要求
   perf-history anchor，也不得计入五个 phase marker；真实 ready 门不得放宽。
5. 仅当 launch 是 own non-interactive isolated desktop 时，才允许 bounded HWND-scoped
   `SPACE` pulse；非隔离必须禁止。pulse 结果写入 `ready-pulses.json`，失败可见。
   超时只调用一次 `wait_for_game_ready`，不重试、不扩 timeout。
6. ready 硬条件：`module.state=Running`、`jassReady`、`runtimeReady`、`gameStarted`、
   `inGameRenderReady` 同时成立；菜单不算 ready。
7. 保存五个 phase markers：`sample-start`、`pressure-start`、`pressure-end`、
   `relief-start`、`relief-end`。每个 marker 必须有同一 `runId`/`pid`、有限正 `wallUnix`、
   `readiness`，并以 producer-owned perf 域绑定：
   `frameDomain=workload.businessFrameSerial`、
   `businessFrameSerial`、`perfFrameEpoch`、`producerAccumulationEpoch`。
   scene `readiness.frameNumber` 只属于 readiness 上下文，不是业务帧锚点。
8. 执行固定路线，在 `normal-start`、pressure 两点、`relief-end` 做内部帧捕获。
9. 复制本轮 HTML 报告、`phaseMarkers.json`、`binding.json`；按 frozen manifest 的精确
   identity 恢复 live，确认 zero processes、无新 dump/GPU 事件。
10. 两轮 OFF/ON 均完成后调用离线比较器；单轮不得自称恢复通过。

## 5. 业务帧域、producer marker 与报告归属

### 5.1 当前已完成 contract（validation-b05 + architecture-b07 source review）

- producer 在 `runtimeStatus.perf` 暴露 `frameAnchorValid`、`businessFrameSerial`、
  `perfFrameEpoch`、`producerAccumulationEpoch`。
- `queryPublishedPerfState()` 以 last completed `m_frameHistory.back()` 为业务帧锚点；
  它取一次 `War3PerfMonitor::m_mutex`，O(1) 但 **不是零成本**。该锁只用于有界 status
  query，不进入 per-frame render 路径。
- phase marker 不得使用 scene `frameNumber`；不得用 `epoch` 猜 offset。
- `phaseMarkers` 跨报告按 `businessFrameSerial` 归属；不同 PID/runId 的 marker 不得混合。
- analyzer 使用 `(start,end]`：`pressure-start + 1` 到 `pressure-end`，
  `relief-start + 1` 到 `relief-end`。

### 5.2 待 validation-b06 完成项（本文件不声称已落地）

以下由 validation-b06 的 runner/test 写入完成并回报后再更新：

- phase acquisition 必须使用 exact PID 的命名管道 `get_runtime_status`，要求 transport/ok
  均真；证据路径不得回退到 stale `runtime_status.json`。
- `pressure-end -> camera.apply -> relief-start` 之间若没有 completed Present，允许 bounded
  polling 至下一个 completed `businessFrameSerial` + `perfFrameEpoch`，同一
  `producerAccumulationEpoch`；max 5 s、100 ms 间隔、失败即 fail；不得无限 sleep 或放宽
  strictly-increasing marker contract。
- pre-ready 诊断与真实五 phase 分离，transport/ready 失败处理按 b06 工单执行。

### 5.3 报告窗口与累计域

- 报告窗口按自身 `businessFrameSerial` 范围归属 pre/post/straddle；重叠窗口本身不自动
  禁止 cumulative delta，但必须先做 ordered union/dedup、重叠冲突拒绝和 gap 保留。
- 只有 `war3_perf_monitor.cpp` 中明确为累计加法的字段可跨报告做差；`*Last` gauge 只读
  当窗/末帧，禁止减法。
- 累计 delta 只在同一 `producerAccumulationEpoch` 内允许；reset/epoch 改变即 fail 或
  uncovered。
- 即使累计 counter 全为零，也必须提供非零、一致、有效的 `producerAccumulationEpoch`；
  否则只能 uncovered，不能 pass。
- 非零 cumulative post-relief 增量在 exact completed-cut proof 之前保持
  `post_relief_cut_proof_missing` uncovered；`producerSealFrameSerialLast` 是 export aggregate
  last observation，不是 completed history cut，不得当 cut 使用。
- 缺失 report meta env、map/resolution、backbuffer/rect 证据时明确列为未覆盖或拒绝认证，
  不能静默通过。

## 6. 恢复门（预先固定合同）

阈值保留不变：

| 合同 | 值 |
| --- | ---: |
| 最少 relief 帧 | 60 |
| 末段持续有效帧 | 60 |
| relief 内允许的最大单次无影连续区间 | 6 帧 |
| relief 内最少 fresh complete render serial advance | 8 |
| 逐帧 有效/无效 counter | `invalidRuns`、`validFrames`、`freshCompleteFrames`、`receiverActiveFrames` |

逐帧有效定义按当前 analyzer 实际 conjunction：

```text
receiver_ok =
  shadowTaaReceiverExecuted == 1
  && hasShadowReceiver == 1
  && replayCasterCount > 0

valid =
  renderedCurrentPartialShadowMap == 0
  && skippedCasterCap == 0
  && receiver_ok
  && (fresh || reusedLastCompleteShadowMap == 1)

fresh = shadowMapRenderSerial > 前一 pressure/relief 行 serial
```

`freshCompleteFrames` 额外要求 `fresh`、`partial=0`、`receiver_ok`、`skippedCasterCap=0`。
`renderedCurrentPartialShadowMap=1` 直接无效；`skippedCasterCap != 0`、`replayCasterCount<=0`、
`shadowTaaReceiverExecuted!=1`、`hasShadowReceiver!=1` 均使该帧无效。

逐帧完整性代理依据：

- `shadowMapRenderSerial` 只在真实 shadow map render 成功后前进
  （`d3d9_war3_shadow.cpp`/`d3d9_war3_scene.h`）。
- `reusedLastCompleteShadowMap` 表示当帧复用上一完整图；只有 `partial=0` 且
  `receiver_ok` 时才可替代 fresh。
- `renderedCurrentPartialShadowMap` 表示当帧部分图；出现即不可认证。
- `semanticSceneReceiverHasCompleteShadowMap` 是聚合 max，不能当逐帧完整性。

还应报告：无影连续区间列表、末段持续覆盖、post-relief `framesIncomplete`/
`framesProducerIncomplete`/`producerRequiredCasterOmissionCount`/
`producerAllocationFailureCount`/`drawTimeSnapshotPageCapacityRejectCount` 增量、census 4 cut
完整性与最后 cut 后新增拒绝。所有既有 failure/无权 pass 语义保持。

## 7. 可比较性与认证边界

- OFF/ON 必须固定 map/profile/resolution/route/matrix/candidate；否则 fail。
- 两轮 wall-clock 帧数不保证相等；逐样本 population 对比只能是统计参考，不能声称逐对象
  同场景配对。
- 帧数不等或窗口不匹配 -> `uncovered/uncomparable`，不得补成 pass。
- caster min/mean 不得下降，`skippedCasterCap` 不得增加，零 caster 不得出现；减少 caster
  不能作为通过。
- 即使统计门通过，`visualRecoveryProven` 始终为 false；没有合法对象/图像/像素证据时不
  认证视觉恢复。

## 8. 离线判定命令

```powershell
py AutoTest\analyze_night_pressure_recovery.py `
  --run-dir AutoTest\artifacts\night-shift-20260920\<off-run> `
  --run-dir AutoTest\artifacts\night-shift-20260920\<on-run> `
  --require-census
```

缺 frozen `binding.json`、phase markers、census、合成完整性字段或报告证据任一未覆盖，
结论保持 fail/uncovered。

## 9. 下一 lease 的编译前检与目标（本文件不授权 lease）

前检：zero war3/ninja/meson/compiler processes；HEAD/worktree 未漂移；frozen manifest
只读冻结；build32 配置与源码身份记录；不部署、不启动游戏。使用 BelowNormal、`-j2`。

从 `src/d3d9/meson.build` 读取的显式目标，共九项工件：

1. `d3d9_dll`（shared library，产物 `d3d9.dll`）
2. `war3_runtime_group_palette_kernel_test`
3. `war3_runtime_group_palette_kernel_diff_test`
4. `war3_stage11_budget_census_test`
5. `war3_stage11_lifetime_page_policy_test`
6. `war3_union_consumer_visibility_test`
7. `war3_shadow_culling_counterexamples_test`
8. `war3_draw_time_snapshot_lifetime_test`
9. `war3_index_upload_summary_test`

其中 2–9 共八个 CPU-runnable 目标必须从当前源码 freshly build；不得复用
old fullsuite binaries。先前 b02 记录的 missing targets（lifetime page policy、culling
counterexamples）现已全部在 Meson 注册。本文件不创建新 lease，也不执行任何目标。

## 10. 明确未覆盖

- 本轮不编译、不部署、不启动游戏、不 GPU/视觉；b07 只完成 readonly producer review 与
  静态 guard，validation-b06 的 runner 变更和测试仍待回报。
- 不能证明 384 MiB 物理显存 OOM、GPU last-use、页回收安全、BF938 玩家现场恢复。
- 不能证明 bounded pulse 真能解决 interface_r2 进图失败；它只是待检候选，旧失败原因未证。
- `68176AE...` source-stale 旧基线不可部署、不可用作 candidate/liveRecovery。
- 统计/合成夹具不构成产品接受；视觉恢复必须另走独立像素/玩家门。