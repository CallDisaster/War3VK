# 2026-09-16 — P5 采集树/帧时间线（A → B）移植前置核对

> 来源：子代理只读核对（grok-4.6）。用户裁定：帧时间线（时间花在哪）与坏帧取证
> （这帧用了什么数据）**两套都留**，禁止因名字相近合并/删除。

## 结论先行

B **完全没有** A 的 collection/timeline 源文件与接线；B 的 frame_evidence/history/recorder
栈与之互补不替代。冲突面在 **Scope 同名（不同 ns）、Enabled() 同名、Present 入口双观察、
三套"FrameHistory"名词、DXVK_WAR3_FRAME_* 前缀**——移植时必须改接线，禁止整文件覆盖。

## 两套系统定位

- **采集树**（dxvk::war3::collection）：多线程、数据入口 Tag 树（33 tag）、采样继承、
  session 累计；编译门 meson warvk_data_collection_tree_dev（默认 false，关闭=零代码）；
  运行时 DXVK_WAR3_DATA_COLLECTION_TREE=1 + SAMPLE_PERIOD（2 次幂 1..4096，默认 64）。
- **帧时间线**（dxvk::war3::timeline）：单 Present-owner 线程、Present-to-Present 40 固定
  墙钟桶、exclusive self 可加总；无编译门（始终链 DLL）；运行时 DXVK_WAR3_FRAME_TIMELINE=1。
- 共同门：War3PerfMonitor recording；JSON 并列 mainThreadTimeline / dataCollectionTree。

## A 侧集成点（file:line 摘要）

- swapchain：d3d9_swapchain.cpp:327 timeline::Present（**必须在 entry-before-lock 之前**）、
  328 Scope("Present")、365 Scope("FrameProfiler")、510/557/570/573 PresentResult、
  1505 BackbufferCreated。
- perf_monitor：ScopedCpuScope 每次 Enter/Leave 尝试匹配 40 桶（cpp:714/723/742）；
  beginFrame LegacyWindow(true)+NoteFrame；archiveFrame LegacyWindow(false)；
  setRecording 同步 collection；export 先 Pause 再 CaptureJson。
- hook_lifecycle：17 个 engine/event hook 的显式 timeline::Scope（EventMessagePump/
  EventDispatch/EngineTlsPump/SelectWorker/RunCallbacks/QueueFlush/FinalizeTick/Reschedule/
  PrepareWait/PrepareDispatch/FinalizeDispatch/TickUpdate/FinalizeWorker/ComputeWakeDelta/
  WaitGate/SleepGate/SleepGateInner）；**A 的 installDeepPhaseHooks 额外 OR timeline::Enabled()**。
- device：WaitForResource（34968-35002）、LockImage/Readback；surface：backbuffer LockRect
  普查（186-198）。
- 采集树 114 个 WARVK_DATA_SCOPE 入口（AutoTest/data_collection_entrypoints.json，
  coverageComplete:false）。

## B 侧核对要点（禁止整文件覆盖）

1. **NativeFrameSync 高冲突**：B 已抽到 war3_native_capture.cpp，Present 用独立
   Begin/Complete/Revoke owner lease，安装只认 persistent 路径、不再依赖 FRAME_TIMELINE。
   照搬 A 的 timeline owner/OBSERVE 安装门 = 双 hook。P5 第一刀不接 NativeFrameSync。
2. **installDeepPhaseHooks**：B 默认 PERF_LEVEL=1 不装深层 engine detour；照搬 A 的
   `|| timeline::Enabled()` 会让仅开时间线就改变主循环 hook 集。须单独决策
   （建议：要求 PERF_LEVEL=2，或做成显式独立 env）。
3. **swapchain Present 整函数**：B 多了 device-lost fail-stop、evidence::Scope、
   ScreenshotPresentGuard、双次 CaptureNativeAsyncScreenshot、更多 return 早退。
   只允许插入 timeline 调用；A 静态测试的 PresentResult==4 断言须按 B 实际 return 点重锚。
4. **perf_monitor 整文件**：B 已有 perf_history_policy 与 getFrameHistory()；
   只补 collection/timeline 字段与 ScopedCpuScope token（注意 B 已修的"先 pop 再覆盖"合同）。
5. **d3d9_war3_debug.h 不移植**（B 已改 DXVK_WAR3_DEBUG_CONSOLE=1 显式分配合同）。
6. **114 入口 manifest 必须对 B 重新生成**，禁止按 A 行号打补丁。
7. meson：新增 warvk_data_collection_tree_dev（默认 false）；两 cpp 进 d3d9 源列表；
   登记两个测试；不动 warvk_internal_frame_recorder 块。

## 建议执行顺序（P5 切片）

1. 拷 core/h/cpp + meson 选项 + 两个 C++ 测试（零行为变更）。
2. perf_monitor JSON/recording 门 + HTML data-tree UI 最小插入。
3. swapchain Present 切帧 + device/surface 观察点（与 B evidence 并存的最高风险点）。
4. lifecycle 17 个 Scope；deep-phase 门单独评审。
5. 按 B 现函数插 WARVK_DATA_SCOPE 并重生成 manifest（coverageComplete 保持 false）。
6. AutoTest 静态/分析器移植（断言按 B 重锚）；NativeFrameSync 计数最后再说。

## 明确不做

- 不删 B 的 evidence/history/recorder 任何一行；不把 timeline 环命名为 FrameHistory；
  JSON key 保持 mainThreadTimeline；禁止把 timeline 接到 FRAME_EVIDENCE=1。
- 不搬 A 的 hook_lifecycle NativeFrameSync 段、debug.h、整文件 perf_monitor。
- 不把 tree 测试的 DEV=1 做成全局默认。

（完整 file:line 明细在会话子代理报告；执行时以当时 B 代码重核行号。）

## 执行记录（切片4 / 2026-09-16 晚）

- **installDeepPhaseHooks 门：不移植 A 的 `|| timeline::Enabled()`。**
  B 现条件保持 `kNativeMainLoopDeepPhaseHookEnabled || War3PerfHookLevel() >= 2`（默认 PERF_LEVEL=1 不装深层 engine detour）。
  理由：A 把 `timeline::Enabled()` OR 进安装门后，仅开 `DXVK_WAR3_FRAME_TIMELINE=1` 就会改变主循环 hook 集与默认诊断成本合同；这与 B 已裁定的 PERF_LEVEL=1 基线冲突。
  后果：TlsPump/SelectWorker/RunCallbacks/QueueFlush/FinalizeTick/Reschedule/PrepareDispatch/FinalizeDispatch/TickUpdate/FinalizeWorker/ComputeWakeDelta 这 11 个深层 hook 上的 timeline Scope 只有在既有 coverage 宏或 PERF_LEVEL>=2 安装 detour 后才有数据。EventMessagePump/EventDispatch/PrepareWait/WaitGate/SleepGate/SleepGateInner 仍随既有默认安装路径计时。
  后续如需「只开时间线也装深层 detour」，用显式独立 env 再议，不走 timeline::Enabled() 隐式耦合。
- NativeFrameSync：本切片未搬 A:362-422 的 owner/OBSERVE 逻辑；B 仍走 war3_native_capture.cpp。
