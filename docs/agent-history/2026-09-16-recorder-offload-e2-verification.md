# CPU取证历史外置 E2：真实跨位数实验收口

2026-09-16。结论限 **CPU实验室**，不是64位渲染器、游戏候选DLL或闪退修复。

## 实际完成

- 实际PE32/i386 producer以8192槽MPSC交给唯一I/O worker；固定256KiB共享窗口，
  专用profile10，经现有nonce/epoch/slot/generation/只读private-copy验证交给PE32+/AMD64 host。
- 大历史只分配在host私有堆。成功publication为连续seq，loss另计；trigger由worker采样，
  producer真实join/队列排空后发Seal；缺Seal、错误退出、晚到数据均不能伪造完整性。
- 64位host冻结后逐事件核内容并流式SHA；Python独立struct/hashlib重算全部保留记录。
  不产生第二份大Event数组，不把CPU ACK冒充GPU完成。
- 本阶段仍是单producer确定性输入；E1有实际多producer核心门，但不是游戏多模块接入证明。

## 真实数据和证据

权威回执：`AutoTest/artifacts/recorder_offload_e2_parent_20260916/native-r2/receipt.json`，
SHA-256 `2980CECD6C9C4DC3CBFB1CA3896BC8D85D40D02A4048CDFD77AA6F3830B3BE69`。
其中包含source/compiler/exe/raw身份、每次helper的PID/creation-time退出验证及容器消失验证。

|最大历史场景|实测|
|---|---:|
|实际接收/丢失|262304 / 0事件|
|触发cut|196768|
|host保留/淘汰|262144 / 160事件|
|host历史存储|102760448 bytes，98MiB|
|host分配后private commit增量|103038976 bytes，98.27MiB|
|x86事务active private增量|3891200 bytes，3.71MiB|
|x86事务active VA增量|6344704 bytes，6.05MiB|
|x86结束private增量|0 bytes|
|x86事务句柄前后|111 / 111|

全部保留事件的SHA-256：
`91ed5dcde113a9aa0d609e5926cdfc3439028ed708b43ac7ef549160e1260cab`。
98MiB未映回x86；这些是独立实验进程的测量，**不能说游戏已经节省98MiB**。
测试producer的批次协调等待也不能当作产品吞吐/无尾延迟基准。

## 验收覆盖

- **16/16 E2实际场景**：最大/短历史、slow、受控消费拒绝断连、missing-seal、late-data、
  wrong-ordinal/session/scope、bad-kind、seal-totals、四种新旧profile相互拒绝，以及最后新nonce短窗恢复。
  不是任意helper崩溃/内核挂死/所有系统环境的覆盖。
- slow实际20160尝试、8352接受、11808丢失，producer尝试QPC全部在host实际阻塞区间内；
  retained6144，合法Seal但cpuComplete=false。故障类digest全部为空，没有将诊断不完整包当成功。
- **55个受影响旧接口实际场景**：P2共享槽41/41、P3 sample worker14/14；回执分别为
  `p2-regression-r1/receipt.json`、`p3-regression-r1/receipt.json`，同在上述artifact根目录。
- 8个定向Python模块 **127个unittest方法**通过（部分方法内部有多项tamper循环，不重复算数）；
  新analyzer/tests/gate三文件py_compile通过。不是全量产品回归。
- 编译仅实验exe，串行BelowNormal、Werror；没有Ninja、DLL构建、部署或游戏/GPU。

## 句柄失败没有被掩盖

最初`native-r1`与后续长窗诊断在数据正确后因110→111失败。短窗无增长。
`handles-idle-r1`在相同依赖初始化后不创建recorder/queue/worker/应用连接，55秒空闲也110→111，
新增Event与ntdll入口Thread，减少一个旧句柄；模块集合无变化。仅此不能确定具体Windows内部根因。

因此normal现在单独输出相同55秒环境观察阶段：该阶段仍110→111，随后真实事务111→111。
时间经过不是清场证明；原handles等值、join、精确helper退出、映射/互斥量消失以及内存门均保留。
没有关闭观察到的系统句柄、改变OS配置或抬内存阈值。maximum的外层watchdog130秒包含启动阶段，
原协议等待界限不变。所有早先失败文件保留，不追认为通过。

其他失败：`build-r1`为主线程新增代码误用Nonce的!=，修为已有==取反；
Python首轮label夹具多一个NUL，已修为32bytes。错误均未通过降低警告或弱化检查解决。

## 任务与上下文

Kimi提供64位host补丁并做有界复审；主线程修正并实际apply_patch、实现x86实验及联合门。
Gemini因缺apply_patch先按规则停笔，转交补丁文本阶段又真实CONTEXT_WINDOW_EXCEEDED，无E2交付。
主线程完成独立Pythonanalyzer/tests，未重发超限上下文、擅改模型/供应商、建立新会话或改数据库。
DSH当前无可用上下文用量/主动compact接口；不猜百分比。两子任务完成后停笔。

## 下一阶段准入，不是本轮已做

E3先独立冻结产品异步owner合同：helper信任/启动、准入和预算、trigger、停止/排空、断连/取消、
DLL卸载后零回调、导出schema与图像/inputs绑定，以及失败只损失诊断而不阻塞渲染的边界。
不能将lab的INFINITE取消排空/析构复制进游戏，也不能失败时默默回退本地100MiB环。

替换真实CPU环后，仍需同DLL off/on、相同地图/画质/2560×1440隔离门，记录x86 commit/VA/最大空洞、
帧时间及尾延迟、峰值/导出/断连/退出；玩家前台验收另列。GPU输入池与显存历史未迁移，
不能外推整个渲染器64位化或本次玩家OOM唯一根因。稳定CHANGELOG不晋升。
