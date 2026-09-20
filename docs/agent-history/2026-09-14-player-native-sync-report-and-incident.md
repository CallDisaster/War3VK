# 2026-09-14 玩家 native sync 复测、驱动事件与截图边界

状态：FOREGROUND_PERFORMANCE_EVIDENCE / INCIDENT_UNRESOLVED / NOT_STABLE。
本轮仅分析既有报告、源码与本机事件，不修改产品源码、启动器或现场DLL，不构建/启动游戏。

## 输入身份与完整帧结果

- 玩家报告：`E:\Work\Warcraft III\WarVK\Log\war3_perf_report_2026_09_14_00_27_42.html`。
  7,449,968 bytes / SHA256
  `B3EC1A6232A4E106C3B729217A70EA85260D8585727301711E3CD5AFC77D5CE9`。
- 唯一const-data根对象，递归duplicate=0；DLL为EE90、34,495,769 bytes，full_default、无禁用模块；
  backbuffer census为2560×1440。新进程日志也确认fullscreen/immediate，显示模式请求为60Hz；
  本文FPS是应用Present调用cadence，不是显示器扫描次数。
- 报告旧4000帧窗口：4.089ms、244.555FPS、GPU pass union 2.105ms、旧墙钟缺口0.454ms。
  旧窗口不含完整Present尾部，不用其FPS替代完整帧cadence。
- 完整账本4096帧：4.369402001953125ms / 228.8642701113332FPS，
  同窗口GetThreadTimes约4.268646ms/frame；QPC=10MHz，区间
  3378049632937–3378228603643，逐帧连续、整数self闭合、fault=0、Present result=0。
- 末2000帧复核：4.38464775ms / 228.06849193301787FPS，p95=5.176ms，最大19.9591ms。
- 对全部4096帧以及末2000帧运行既有native策略验证：每帧request=1/capture=0、elided=1，
  未知/嵌套/不可读=0，后缓冲锁=0，ResourceGpuWait/ResourceCsSync=0。
  进程累计sites中的962次锁定不是这个选中窗口，不能当作优化未生效。
- 新报告framesIncomplete/budget/receiver拒绝等既有零门均为0；4级4096 CSM，平均caster
  143.777、geometry 39275.514，MissingRequiredPartCount本报告为0。
  不能把这次快照外推为旧计数2已修复，也不能据此宣告截图或崩溃门通过。
- 此次是玩家前台复测，不能与有前台并行负载的隔离ABBA直接拼成精确收益百分比。

分析输出：`AutoTest/artifacts/native_frame_sync_player_report_20260914/` 中的
`timeline-4096.json`、`timeline-2000.json`、`data-collection.json`；全部保持productAccepted=false。

## 停止录制后掉回约110FPS的确定原因

`CanElideNativeFrameSync`要求recordingOwner；`NativeFrameSyncRecordingOwner()`实际为
`owner() && local.previousRecording`，后者由Present记录录制状态。因此关闭录制后，
下一对应边界恢复原生request=1；这是当前实验版策略耦合，不是录制本身能提高性能。
用户110FPS为观察值，当前报告没有录制关闭区间的完整账本，不能伪造该区间测量。

下一候选应分离功能gate、渲染owner/生命周期授权与诊断gate；不能仅去掉全部owner检查。
须保留精确Game代码身份、真实截图、未知请求/嵌套/不可读回退，并验证录制开关不再改变
产品行为。当前有未解释驱动事件，尚未扩大到默认常开或稳定部署。

## 闪退与驱动事件

- 用户报告录制期间发生一次闪退，随后不能复现。系统System日志在本地
  **2026-09-14 00:26:03.398**（UTC 2026-09-13 16:26:03.3984445）记录
  `nvlddmkm / EventID 153 / Level 2 / RecordID 3552557`。
  数据为 `\Device\000000ea`、`Error occurred on GPUID: 900`；事件进程是System PID4，
  不是可直接归责给War3的调用栈。仅此事件号不能断定TDR或具体GPU故障机制。
- 00:25:40报告TID23380、1540帧；00:26:36.850新crash-handler状态PID38128/TID15664，
  后者对应00:27:42报告。时间支持重启前后存在驱动错误，但尚未确认玩家闪退精确时刻/因果。
- 00:00–01:00 Application查询成功，4条事件，1000/1001=0；System查询成功，14条事件，
  Display/nvlddmkm/WHEA筛选仅上述153。未找到该时段新WarVK/游戏dump、WER或LiveKernel dump。
  最新WarVK fatal仍为9月13日20:13的旧camera.snapshot实验，严禁冒充本次闪退证据。
- `war3_d3d9.log`已由新进程写入；当前无device-lost错误不能替代已丢失的前一进程尾日志。
  新进程日志、crash-handler状态已原字节复制到上述离线目录，未触碰原件。
- 不能因重跑成功而关闭incident；也不能直接归因于日志导出、显卡硬件或候选缺陷。
  去掉原生逐帧等待改变了CPU/GPU在途重叠，有可能暴露过去被串行化掩盖的资源last-use问题；
  这是必须排查的假设，不是已证明根因。稳定验收暂停，仍需带进程身份的设备错误/退出证据。

## 剩余热点与报告导出本身

完整账本同一4096帧的主要互斥桶为NativeOriginal 2.147ms、WorldFramePrepare 0.889ms、
WorldRenderScene self 0.526ms、OutsideScopes 0.333ms；Present inclusive约0.305ms（与子桶重叠）。
NativeOriginal是“调用原函数期间”，其中会重入WarVK，不能称为2.147ms纯暴雪代码。

旧4000帧语义树进一步显示：

| 完整路径的关键尾部 | ms/frame | 解释 |
|---|---:|---|
| WorldRenderScene/NativeOriginal/FlushAndReset/NativeOriginal/FlushSortedItems/NativeOriginal | 0.964 | 原生队列执行边界，不代表全是排序，需细分遍历、提交及重入 |
| WorldFramePrepare/NativeOriginal | 0.884 | 世界帧准备，下一层仍待归因 |
| FlushAndReset/NativeOriginal/Populate/DirectGrouped/BuildEligible | 0.341 | 数据准备的具体优化候选 |
| ShadowCapture | 0.489 | Gates 0.376 + PostGate 0.113，不能再加到已覆盖它的父路径 |

DataCollection会话采样根中DrawCapture占39.66%、Populate38.45%、VisiblePublish12.28%；
这不是完整帧比例，也不是上述4000帧窗口的精确每帧时间。

另外FrameCapture/Poll约0.083ms/frame，可审计无请求时的文件轮询成本。
报告导出已有后台worker，但`captureExportSnapshotLocked`仍在调用侧复制frameHistory，
并在持有monitor锁时调用timeline/collection CaptureJson。应考虑只冻结有界数据快照，
把字符串化移到worker；手工导出队列满时也可能等待。这支持导出卡顿审计，不能证明153根因。

## 原生截图：每帧同步与按需读回分开

现候选在capture非零时保留原请求，原生像素读取、EndScene/Present和API结果均未删除。
本轮capture总数0，所以只有源码保留证明，没有实际截图成本或像素正确性证明。

1. 保持同步截图：普通帧不读回，真正截图那一次仍可能等待GPU完成、传输及编码；
   不能保证该帧小于5ms，也不能把历史4.7ms等待当成14MiB传输的固定费用。
2. 保持用户截图功能但避免主线程长停顿：需要独立异步截图流程，而不是让既有同步LockRect
   返回尚未完成的数据。渲染owner在选定原生截图像素边界排一次image→staging复制，绑定精确
   frame/尺寸/格式/颜色/生命周期；GPU完成后worker处理CPU像素与文件，结果晚到但不偷换帧。
3. 使用有界2–3槽、完成信号/last-use管理和非阻塞准入；满槽显式拒绝或排有界请求，不在
   主线程强制flush/wait。Reset/地图退出/device-lost应取消未提交请求并按真实GPU完成退役，
   不跨线程调用原生游戏对象，也不保留可变backbuffer裸指针。
4. 2560×1440×4=14,745,600 bytes，单份未压缩像素14.0625MiB；3槽仅像素载荷42.1875MiB，
   不含其他副本/对齐/资源开销。GPU复制与完成延迟不是零，实际CPU/GPU成本必须测量，不能
   承诺固定微秒或固定几帧完成；编码及磁盘延迟不应阻塞渲染主线程。

一手合同：[Microsoft LockRect](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dsurface9-lockrect)
明确DONOTWAIT无法立即完成时返回WASSTILLDRAWING，不能伪造成功；
[Khronos readback同步示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html#cpu-read-back-of-data-written-by-a-compute-shader)
要求GPU完成同步与非coherent内存invalidate后才允许CPU读取。映射到截图时还需image layout
与transfer→host依赖，具体实现尚未进行。

## 本轮收尾

现场仍为EE90，原139B备份未变，零游戏/编译进程；没有恢复或覆盖玩家现场。
仅新增本说明及离线分析证据、同步开发日志/AGENTS边界；未改产品、未跑新实机或编译。
下一顺序：解释/取证incident并隔离诊断耦合 → 截图按需/异步合同 → 约0.964ms队列体与
0.884ms世界准备细分。收益与稳定性分别判断，根CHANGELOG不晋升。
