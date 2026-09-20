# 交接文档：阴影消失问题 + 近期取证类问题（致 Astra）

生成时间：2026-09-19（本轮实测状态）
工作树：`E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`（linked worktree）

---

## 0. 结论速览（TL;DR）

1. **阴影消失是真的**，已由视觉审查确认：4 帧内**所有对象（含静态建筑/树木/栅栏）完全没有投射阴影**。
2. **逐帧序列已抓到转折点**：`terrainDoodadPreparedCount` 与 `terrainDoodadCascade0..3DrawnCount` 在**业务帧 64059 起永久归零**，直到窗口结束（65738）再未恢复；
   同期 `terrainDoodadCaptureAttemptCount` 仍约 262（**仍在捕获**）⇒ 静态世界几何在「捕获 → replay」之间被丢弃，且呈**粘滞**形态。
3. 下游计数器**自认为在正常工作**：`replayCasterCount` 272~431、`shadowTaaReceiverExecuted=1` 每帧、`semanticSceneReceiverHasCompleteShadowMap=1`、`effectiveShadowResolution` 恒 4096。
   ⇒ **断点在「捕获 → replay 构建」以及「最终合成」这两层**，现有聚合指标看不见，只有逐帧序列看得见。
4. **另有一个独立问题**：draw-time 快照几何池容量拒绝（`snapshot-alloc/v1` 4548 次 `ResidentCapacity`，常驻 384MiB/已用 372.6MiB=97%），约 6 个 caster/帧被 fail-closed 省略。**与「全场景无影」不是同一件事**。
5. 取证链路另有 4 个独立问题（导出失败/交换链重置/32 位地址空间/一轮只能导一次），详见第 3 节；其中地址空间问题已被玩家把 `War3.exe` 换成 **LAA** 绕过。

---

## 1. 环境与部署现状（务必按此核对）

```
站点（玩家测试用）  E:\Work\Warcraft III\
  d3d9.dll  SHA256 = F2A7A6FC8FFAD077CEE70D69A2F0B16B48AC514837D64BC7D66CE3804F2CC33D
  War3.exe  LAA = true（玩家自行替换，用于绕过 32 位地址空间门槛；站点原版为 LAA=false）
构建产物          <工作树>\build32\src\d3d9\d3d9.dll（与站点同哈希）
玩家自身树        E:\Work\War3\ —— 其 d3d9.dll 仍是 2026-09-17 的 A0A51AF2…，**未部署本轮任何构建**
备份/让路         站点目录内：d3d9.dll.parked_20260919-145909 / d3d9.dll.parked_20260919-143636 /
                  d3d9.dll.8697A86C_backup_20260919-143636 / d3d9.dll.BCF234C8_backup_20260919-120445 /
                  d3d9.dll.7120104E_backup_20260918-215341
磁盘              E: 可用约 58.7 GB（此前曾被日志占满，已清理 54.39 GB，见第 3.5 节）
当前进程          无 war3.exe 在运行
```

### 必须遵守的约束（延续既有裁定）
- **不使用 git 写操作**；
- **未经明确许可不触碰 `E:\Work\War3`**（玩家树）；对站点树操作前先记录进程与精确备份；
- 若 `War3.exe` 在运行 ⇒ 用 **rename-park 让路，绝不 kill**；
- 不改 palette 准入 / 几何选择 / 渲染优先级；不得以「关闭动态 caster / 减少正常阴影」充当修复；
- **候选、判退、纯证据与无效运行不得冒充稳定更新**；隔离桌面数据不得当作前台性能；
- 每个修复必须有**载荷探针**（撤销修复⇒具名失败）；改契约要证明符合新要求，等价重构要证明行为相同。

---

## 2. 主问题：阴影消失（重点）

### 2.1 症状（视觉证据，权威）
由视觉子代理（gemini-3.8-flash-high）审阅 4 张真实帧（1902×963，来自 15:20 那轮取证）：

> 四帧中动态投射阴影**全程完全缺失**：全屏所有区域、所有对象（**不论动态单位还是静态建筑/树木/栅栏**）均无投射阴影；无局部渲染、闪烁或断裂边缘。

⇒ 不是「个别 caster 被省略」，而是**全场景无影**。

### 2.2 逐帧序列证据（决定性）
报告：`E:\Work\Warcraft III\WarVK\Log\war3_perf_report_2026_09_19_15_41_40.html`
（`meta.dllSha256 = F2A7A6FC…`；`frameCount=3600`；`windowSec=40.18`；`meta.perfFrameEpoch=5528`；序列覆盖 epoch 1929→5528）

```
terrainDoodadPreparedCount / terrainDoodadCascade0..3DrawnCount（逐帧）：
  行 0..1778      约 241 → 228   正常，偶发单帧 0
  行 1779..1881   0             14 秒缺口
  行 1882..1919   261           短暂恢复
  行 1920..3599   0             ← 业务帧 64059 → 65738，直到窗口结束再未恢复

同期（一直正常）：
  terrainDoodadCaptureAttemptCount ≈ 262     （仍在捕获，未归零）
  replayCasterCount 272..431                 （动态 caster 仍在回放）
  effectiveShadowResolution 恒 4096
  shadowMapRenderSerial 持续推进 3159→4954
  shadowTaaReceiverExecuted=1 每帧 / semanticSceneReceiverHasCompleteShadowMap=1
```

### 2.3 代码定位（下一步的入口）
- 上述 prepared/drawn 计数在 **`src/d3d9/d3d9_war3_shadow.cpp:4243-4262`** 的 **`replayDraws` 准备循环**内递增（按 category/objectKind 分类）。
- 因此 **prepared=0 而 captureAttempt≈262** 的语义是：**doodad 已被捕获，但没有出现在 `replayDraws` 中** ⇒ 丢弃发生在**捕获 → replay 构建之间**。
- 「一旦归零就永不恢复」的形态提示**粘滞状态**（某配额/缓存/身份表耗尽后无回收），而不是逐帧抖动——需要沿捕获→replay 的每一道过滤（验证/身份/新鲜度/配额/blocker 分类）逐条核对。

### 2.4 已排除 / 尚未排除
| 结论 | 状态 |
| --- | --- |
| 「阴影链路整体停摆」 | ❌ 已排除：接收端每帧执行、阴影图渲染序列推进、回放 caster 数百个 |
| 「阴影图未被告知完整」 | ❌ 已排除（就 CPU 记账而言）：`HasCompleteShadowMap=1`、`ReceiverInputValid=1` |
| 「快照池容量耗尽导致全场景无影」 | ❌ **不成立**（我先前说法过强，已更正）：它只造成约 6 caster/帧 的省略 |
| 「静态世界 caster 在捕获→replay 之间被丢弃」 | ✅ **有逐帧实据**，且呈粘滞、不可自愈 |
| 「动态单位为何也无影」 | ⚠️ **未解释**：`replayCasterCount` 272~431 说明单位 caster 在回放里；需继续查 replay 内容有效性 / CSM 级联偏置 / 深度比较 / 最终合成 |

### 2.5 建议的下一步（按性价比排序）
1. **定位粘滞丢弃点**（最高优先）：从 `terrainDoodadCaptureAcceptedCount` 追到 `replayDraws` 的构建处，找出会永久粘住的过滤/配额条件；
2. **物理确认根因**：在坏态下强制清空/重建该缓存（诊断用，fail-closed 语义不变），观察静态阴影是否立即恢复；若恢复 ⇒ 根因确认；
3. 给「捕获→replay 丢弃」加**具名、限量的失败证据**（沿用既有 `snapshot-alloc/v1` 那种「只在失败时记录」的风格），使下一次采集能自证；
4. 复查动态单位路径：replay caster 是否**退化**（零面积/错误变换）、CSM 级联与偏置、深度比较方向/格式、最终合成强度；
5. 复现/对照手段：`DXVK_WAR3_PERF_AUTO_EXPORT_SEC=20`（`war3_perf_monitor.cpp:1085-1100`，每 20 秒自动落一份报告）⇒ 可拿到转折点前后的对照窗口。

---

## 3. 近期取证类问题点（与阴影问题独立）

### 3.1 `local-recorder-raw-export-failed; partial files retained`
- 机制：该字符串只是**导出阶段的前置标签**（`war3_frame_history.cpp:214-222`），真实原因原先被外层丢弃。
- 三条真实失败路径：ring 未 Frozen（`freeze before export`）、`CreateNew export failed`（写盘失败，最可能是空间不足）、`input provider never initialized`。
- 磁盘门槛：`RecorderDiskHeadroom = 6 GiB`（`war3_frame_recorder_config.h:28`）；默认档 `pre=256` ⇒ 帧图 ring 上限 **4 GiB**。
- **已做**：失败原因具名化（`(freeze-before-export)` / `(write-failed; check free space >= 6 GiB)` / `(input-provider)` / `(cpu-control-error)`）；
  新增**只缩小**的规模开关 `DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES`（4..256）与 `_POST_FRAMES`（1..16）。

### 3.2 `reset-or-owner-change`
- 机制：`m->cancelled` 置位后下一帧捕获即判失败；置位点是 `d3d9_swapchain.cpp:1130`（**交换链 Reset**）、`:1059`（源图像缺失）、`:1086`（拷贝提交失败）、`:1089`（捕获异常）、`war3_frame_history.cpp:144/162`（owner 替换/释放）。
- **已做**：取消原因具名化（`swapchain-reset` / `history-copy-submit-failed` / `history-source-image-missing` / `history-capture-exception`）；
  `AutoTest/test_frame_history_static.py` 按**意图不变**更新并加强（要求四处具名齐全）。
- **未做（缺口）**：自包含模式下一旦故障，冻结的 CPU 环**无法导出**（本地 worker 已停、外部控制需租约）⇒ 建议做 **salvage-on-fault**（进入 Fault 前先落盘冻结证据）。

### 3.3 `recorder-process-address-space-headroom`
- 机制：arm 阶段 fail-closed；`RecorderProcessHeadroom = 256 MiB`（`war3_frame_recorder_memory.h:8`），用 `GlobalMemoryStatusEx` 的 `ullTotalVirtual/ullAvailVirtual` 判定（`war3_frame_recorder_memory.cpp:10-13`）。32 位无 LAA ⇒ 2 GiB 上限。
- **已做**：失败信息带上实测数字（`required/availVirtual/availCommit/largestFree/totalVirtual`，`totalVirtual` 即 LAA 判据）；新增 `DXVK_WAR3_FRAME_EVIDENCE_VA_HEADROOM_MB`（**只下调**，下限 64、上限默认 256）。
- **玩家侧处置**：把站点 `War3.exe` 换成 **LAA=true**（站点原为 LAA=false）⇒ arm 不再报该错。
  ⚠️ 注意既有研究文档 `docs/research/2026-09-16-player-crash-pid13216-memory-admission.md:116` 的裁定：「**不修改 War3.exe 的 LAA 位来掩盖资源成本**」——玩家此举属**显式例外**，需在后续结论中如实登记，不得据此宣称资源成本问题已解决。

### 3.4 取证「一轮只能导一次」
- 现象：本地录制器模式下 owner 已存在/故障后无法再次 arm（`war3_frame_history.cpp:144`：已有 owner ⇒ 返回空并 cancel），导致**拿不到有影/无影对照**。
- 绕过：两次采集之间重启游戏；或改用外部监视器模式（`DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=0` + `AutoTest/frame_history_watch.py`，它自行认领租约并可重新 arm）。

### 3.5 环境事故（已处置）
- `E:\Work\War3\WarVK\Log` 曾占 **55.58 GB**（`.jsonl` 49.2 GB/226 个 + `.html` 6.37 GB/13,724 个）。
- 经玩家明确授权后**只删日志**：13,508 文件 / 54.39 GB，清单存 `E:\Work\deleted-war3-logs-20260919.txt`；保留最近 30 天报告、最近 7 天日志、`Crash\*.dmp`；**未触碰任何游戏文件/DLL/配置/地图**。
- 结果：`WarVK\Log` → 513 文件 / 1.60 GB；E: 5.95 GB → 约 60 GB。

---

## 4. 与本主题相关的既有能力（可复用，勿重复造）

### 4.1 64 位渲染宿主 RH0/RH1（`<工作树>\tools\render_host\`）
- **已实现且有测试**：`protocol` / `win32_transport`（命名管道 + SID DACL + 双向 PID 校验）/ `shared_slots` / `slot_ledger` / `producer_inbox` / `sample_envelope` / `recorder_ingress` / `recorder_event_wire` / `recorder_history_store` / `owned_child`（父子 Job kill-on-close）/ `README.md`。
- **本地实测（2026-09-19）**：`AutoTest/run_recorder_offload_e2_gate.py` **15/15 通过**，回执 `E:\Work\rh0-e2-verify-20260919-150245\receipt.json`；
  32 位父进程实测：`privateBytes 1.22MB → 5.11MB(峰值) → 1.22MB`、`usedVirtual 24.1→29.4→24.1 MB`、`lost=0`、`handles 107→107`。
- **状态**：README 首行即声明 *not linked into the shipping DLL*；`src/` 内**零客户端符号**；回执自述 `productIntegration=false / shippingDllBuilt=false / gameLaunched=false / gpuTested=false / gameMemoryBenefitMeasured=false`。
- 工具链已就位：`E:\Dev\Toolchains\warvk-rh0-llvm-mingw-20260616`（i686 / x86_64 clang++）。
- ⚠️ 注意：README 明确该 lab 传输为**阻塞式**，**不得**直接链进 DLL 或渲染线程；且它省下的是 CPU 事件环（约 5 MB 峰值），**不足以**单独解决 256 MiB 量级的 VA 缺口。

### 4.2 诊断批次（对象级 palette 证据，v4）
- 唯一规则 `Verdict(...)` + 描述式交接：全空间枚举 `combos=622080 relaxViolations=0 decoupleViolations=0 exportViolations=0`；执行型测试 39 passed / 0 failed。
- 已知边界：**规则可枚举证明，但生产接线不可由文本门禁保证**（验证者曾用 `true ||`、强制写字段、D 点 `|| true` 等在门禁 exit 0 下重造谎报）。

---

## 5. 当前门禁与构建状态

```
ninja -C build32 src/d3d9/d3d9.dll                     => exit 0
AutoTest/test_*_static.py                               => 264/0
ninja -C build32 test（meson）                          => Ok: 86  Fail: 0
war3_stage11_snapshot_publication_test                  => 73 checks PASS
war3_frame_recorder_memory(_control)_test               => 87 + 46 checks PASS
war3_frame_recorder_session_test                        => 9766 checks PASS
站点 d3d9.dll SHA256 = F2A7A6FC8FFAD077CEE70D69A2F0B16B48AC514837D64BC7D66CE3804F2CC33D
```

### 本轮新增但**只是缓解**（不可当作阴影问题已修）
- `DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB`（快照池常驻上限，下限 128 / 上限 512 / 默认 384 MiB；512 MiB = 32 页，沿用既有 32-create 安全门）。
  依据：15:20 取证中 `snapshot-alloc/v1` 4548 条 **全部 reason=2 = ResidentCapacity**；末条 `resident=384MiB, used=372.6MiB(97%), pages=24`。

---

## 6. 不允许声称（延续既有纪律）
- 不得声称「阴影已恢复」或「阴影消失已定位根因」——目前只有**静态世界 caster 被粘滞丢弃**这条有逐帧实据，动态单位无影尚未解释；
- 不得声称全门禁通过；不得把候选/纯证据/隔离数据当作稳定版或前台性能；
- 不得把 LAA 替换当作资源成本问题的解决；
- 不得用「关闭动态 caster / 减少正常阴影」充当修复。

---

## 7. 关键文件与产物索引

```
报告（无影态）      E:\Work\Warcraft III\WarVK\Log\war3_perf_report_2026_09_19_15_41_40.html
报告（更早一轮）    E:\Work\Warcraft III\WarVK\Log\war3_perf_report_2026_09_19_13_32_23.html
取证（无影态, 15:41）E:\Work\warvk-live-E\evidence3\cpu-13980-827388855559-1.json（24.3MB）+ 同目录 manifest.json 与 slot-*.tga
取证（无影态, 15:20）E:\Work\warvk-live-E\evidence3\cpu-41360-814576821788-1.json（25MB）+ history-41360-814577044041-1\（97 帧）
由帧图导出的 PNG      E:\Work\frames-r1\frame-{40,55,70,90}.png（供视觉审查）
已解析的报告 JSON     E:\Work\perf_data.json（13:32）、E:\Work\perf_data2.json / perf_d41.json（15:41）
RH0 E2 回执          E:\Work\rh0-e2-verify-20260919-150245\receipt.json
日志清理清单          E:\Work\deleted-war3-logs-20260919.txt
开发台账              docs/agent-history/DEVELOPMENT_CHANGELOG.md（含本轮全部结论与一次结论更正）
```
