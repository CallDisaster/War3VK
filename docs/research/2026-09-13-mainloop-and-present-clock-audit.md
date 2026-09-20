# GameMainLoop 与 8.426ms 的真实计时边界

日期：2026-09-13。状态：READ_ONLY_AUDIT / CLOCK_CONTRACT_CORRECTION。
本轮仅回答/核对，不修改 C++、Hook、构建、部署、游戏状态或原报告。

## 1. 8.426ms 怎么产生

当前139B主树中 `war3_perf_monitor.cpp:73` 使用 `std::chrono::steady_clock`。
`beginFrame` 保存 m_frameStart；`archiveFrame` 在 endFrame 调用链中记录
`snapshot.totalCpuMs = toMs(snapshot.timestamp - m_frameStart)`。
字段名totalCpuMs有历史误导：这是墙钟区间，不是线程CPU执行时间。

生成报告时选取保留帧，算术平均 totalCpuMs，写到 avgFrameTimeMs；avgFps 写为
`1000.0 / avgCpu`，而不是独立计数实际扫描显示的帧数。
18:06报告3600个导出frameTimes的均值为8.426081944444444ms，显示8.426，倒数约118.68。
导出逐帧值已三位小数化，和内部未四舍五入的平均倒数可能有微小差异。

## 2. 必须纠正之前“完整帧墙钟”的说法

`d3d9_swapchain.cpp:350` 在本次 PresentImage **之前** endFrame；`:526` 在 PresentImage
**之后** beginFrame。正常路径实际为：

```text
上次 PresentImage 返回
  beginFrame ── 游戏/渲染/部分Present入口 ── archiveFrame(endFrame)
                                               |
                         本次Present余下工作、UI fallback、PresentImage
                                               |
                                          下一次beginFrame
```

所以8.426ms是profiler start→archive的平均墙钟，不是严格的同一Present入口到下一入口。
落在endFrame与下一beginFrame之间的工作没有进入这个分母。GDI fallback/失败路径还需
独立检查，不能用成功路径图覆盖所有状态。

先前回答把这个值称为完整帧/真实FPS证明过强。当前报告间+17.84%只能精确称为**当前计时口径
的FPS估值变化**，用户另观察到的窗口/全屏涨帧保留，但不等于有完整呈现节拍计数的闭合证据。

注意：这个计时盲区在8.426ms分母之外，不能用它直接解释8.426-3.911=4.515ms。
4.515仍是**已测窗口之内**未被选中主线程root覆盖的墙钟；不是整个游戏CPU损耗，也不是
全部原生渲染。报告windowSec还涉及保留窗口/导出时间，不应另用3600/windowSec当精确显示FPS。

## 3. GameMainLoop 可以记录，但不是入口/退出包一次就够

当前address book已列 `mainLoopRoot=0x05F710`，没有在lifecycle安装代码中看到该根的迭代
计时实现。IDA对 `0x6F05F710` 的新只读复核显示：函数包含长生命周期while循环，同时用作
EvtSched Engine线程入口，也可能由其他入口直接调用。一次函数调用不是一帧。

循环大致包括SelectWorker、WaitGate/SleepGate、PrepareDispatch、RunCallbacks、MessagePump、
FinalizeDispatch、QueueFlush、TickUpdate、Finalize/Reschedule。回调和条件分支决定是否发生
渲染；有些迭代只处理调度/等待。必须保留OS TID及循环序号，不假定函数名代表唯一主线程。

当前已安装的轻量消息泵/WaitGate不是完整主循环：

- eventMessagePump RVA059B00只覆盖一个阶段。
- WaitGate RVA158940：以本线程上次wait返回→这次wait进入为Active，再加本次wait为EngineCycle。
  Active是两次闸门间墙钟，不等于纯CPU；报表聚合不应默认代表单一渲染线程。
- 深层RunCallbacks/TickUpdate等代码存在，kNativeMainLoopDeepPhaseHookEnabled默认关。
- 新报告mainLoopCycle.present=true、cyclesPerFrame=2.322；这只是已记录的等待门周期/报告帧
  比例，不是“一帧执行了恰好2.322次完整主循环”。avgCycleMs=7.251、Active=3.738、Idle=3.513
  属于不同clock domain，不能用3.513直接抵消4.515未覆盖值。

## 4. 后续正确方案：两个时钟共用事件时间线

1. **呈现帧时钟**：同一主swapchain的PresentEx入口→下一正常呈现入口；记录入口、返回、
   success/result、map/device/swapchain generation，避免多个swapchain混成FPS。失败/菜单/
   loading窗口独立标记，不补造帧。该计数代表应用提交节拍，不等于面板实际扫描帧率。
2. **循环时钟**：在已证明的循环迭代边界建LoopCycle，并记录主要阶段实际begin/end；
   不包住整个线程函数直到退出才生成一次记录。
3. 每条事件携带QPC tick、TID、scope token、cycleId、frame关联；跨frame区间按交集分割，
   不把worker耗时相加到主线程帧墙钟。
4. 原生RunCallbacks中可能回到WarVK/D3D9，因此原生父与自有子是包含关系。模块exclusive
   时间由同一栈得出，不独立抽样后相减；等待是独立状态维度，不把NativeOriginal标成纯Game.dll。
5. 保留两类未知：已知父内Self/Unclassified；完整主呈现区间内无任何已知scope的OutsideRoots。
   只要其中有大头，就继续在对应时间段追踪，不预先归给数据收集。
6. QPC度量墙钟；OS线程CPU与Running/Ready/Waiting用窗口计数及本机WPR/WPA辅助验证。
   现有GetThreadTimes逐帧样本出现0/15.625ms等粒度，不能用它精确分配每个微小模块CPU。

这会使主要模块逐步可测，但不能仅凭新增GameMainLoop外层scope宣称所有模块自动可见。
下一阶段应先统一完整时间分母与线程/cycle/frame关联，再扩分支，不再只堆更多互不对齐scope。

## 5. 证据和动作边界

源码：当前主树88089cdf加139B构建时的工作改动；对应报告
`E:/Work/Warcraft III/WarVK/Log/war3_perf_report_2026_09_13_18_06_51.html`，SHA
`455726BCC46E35171E8D1BC6AABEFEFAAF7EE003AD1AAF8E3BCDC5E269DC9C3C`。
JSON重复键问题仍保持失败关闭，仅分析唯一的计时字段，未修复/重解释旧证据。
IDA只读结果：`AutoTest/artifacts/data_collection_tree_20260913/main_runner_readback.json`。
没有执行新的测试/构建/部署/游戏；本结论修正统计解释，不代表完成新FrameCoverage实现。
