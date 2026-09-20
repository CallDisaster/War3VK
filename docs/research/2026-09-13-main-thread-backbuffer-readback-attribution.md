# 主线程空白时间：BackBuffer LockRect 同步读回归因

状态：DIAGNOSTIC_CANDIDATE / EVIDENCE_ONLY，未进入稳定版本。

## 结论与边界

已经定位到一条旧手工树没有包围的主要路径：

`Engine callback → Game.dll+0x0ED080 → GetBackBuffer → LockRect(0x810)`
`→ D3D9DeviceEx::LockImage → image-to-buffer readback → WaitForResource`
`→ CS synchronize → DxvkDevice::waitForResource → native wait`

随后才是原生 Present。它位于旧 beginFrame→archiveFrame 窗口内，却在旧
Present scope 外；因此 Present 很短与帧内有数毫秒空白完全可以同时成立。
这不是已经证明的“4.5 ms 纯 CPU 数据采集计算”，也不是 GameMainLoop SleepGate。

2026-09-13 20:21:39 启动的 R10 在非交互隔离桌面、2560×1440 实际 backbuffer、
相同指定地图下完成 60 秒。未启用挂起线程采样。取最后连续 2000 个完整 Present 间隔：

| 项目 | ms/帧 | 次/帧 | 口径 |
| --- | ---: | ---: | --- |
| 完整 Present entry→entry | 14.579669 | 1 | 分母 |
| 与旧测量窗口的交集 | 14.278677 | — | 不同于整份旧树4000帧均值 |
| GPU resource wait | 5.580505 | 1 | exclusive；全部位于旧窗口内 |
| CS synchronize | 0.155144 | 2 | exclusive；不与GPU wait重复 |
| ResourceWaitFlush | 0.004500 | 1 | exclusive |
| LockImage Readback 总范围 | 5.745122 | 1 | inclusive，已含上述等待，不能再相加 |
| FlushAndReset NativeOriginal | 4.140164 | 10 | inclusive；含既有WarVK hooks，不能全算原生CPU |
| WorldFramePrepare | 2.120131 | 1 | inclusive |
| GameMainLoop WaitGate | 0.011212 | 2 | exclusive；不是主要空白 |
| 完整 Present 函数范围 | 0.377092 | 1 | inclusive；不是主要空白 |
| FrameCapture 文件轮询 | 0.209885 | 1 | exclusive；在旧窗口外 |
| OutsideScopes | 0.700721 | — | 明确保留未知，占完整墙钟4.806% |

所有2000行 QPC 整数独占分区与分母相等；旧窗口交集也独立闭合；连续帧、零
ledger fault、单一swapchain/TID、成功Present均通过。OS线程CPU窗口均值8.609375ms；
它不是可与墙钟、GPU时钟再次相加的子节点。

这些是隔离诊断数字，不是玩家前台FPS，也不是与18:06:51玩家报告的严格AB对照。
旧玩家报告4.515ms缺口没有历史原生栈，不能宣称逐毫秒追溯完全证明；本轮已经证明
同分辨率复现中的主要遗漏边界，并把未知量降到可继续追踪的约0.70ms。
GPU wait包括等待此前已排队的图形工作及本次读回，不等于“复制本身就花5.58ms”。
旧GPU union仅包围部分GPU passes，不能拿它与此等待相减来制造另一个精确剩余。

## 独立栈证据与IDA复核

R9先在相同隔离/分辨率合同下完成45秒外部WOW64墙钟采样：4216个有效context，
4216次stack read，2542条EBP链到达终点。1510个样本具有完整核心调用链：

`ntdll → KERNELBASE → DxvkDevice::waitForResource → D3D9DeviceEx::WaitForResource`
`→ LockImage → D3D9Surface::LockRect → Game+0xED0F6 → +0xE377C → +0x5B00C`
`→ +0x5FD0A → +0x5E721`

另32个样本落在同一LockImage链的CS synchronize。R9仅用于找调用链；采样总挂起
371.1664ms，单次最大6.3526ms，必须承认扰动；它不是ETW CPU Running百分比。
EBP链不完整部分仍保留；不把栈内任意整数当返回地址。

IDA输入Game.dll SHA E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A。
反编译0x0ED080表明：先结束scene，然后在`a2 || *(this+1376)`条件下GetBackBuffer、
GetDesc、LockRect(NULL,2064/0x810)、可能复制像素、Unlock、Release，最后才Present。
`this+1376`分支会分配并逐行复制图像；不能删除真正的像素读取路径。
传入a2的业务含义、每帧取值与完整截图生命周期仍需专项证明，不能从本次证据
直接授权全局跳过LockRect、假返回成功或删除同步。

上述0x0ED080与0x05F710的证据边界已追加到IDA函数注释，原名称/注释/字节保留并保存。
原数据库备份与读回回执位于`ida-writeback/`，没有修改Game.dll本体。

另一个明确的CPU热点线索是IsReadableRange/IsReadableRangeFast→系统内存查询，
特别是材质绑定proof路径；R9该系统入口有452个IP样本。它在既有render/collection
范围内，不能再次算入已经归因的GPU等待。后续应按材质/模型生命周期验证缓存机会，
不能移除指针安全检查。

## 实现

- 新增默认关闭`DXVK_WAR3_FRAME_TIMELINE=1`诊断。一个Present owner TID/swapchain，
  QPC绝对时间，4096帧固定环、40个固定桶、64层TLS栈；非平衡/overflow/时钟倒退
  永久标fault。inclusive按同桶union去重，self独占，未知保留。
- 17个已有Engine/事件函数边界显式计时；跨Present与旧窗口边界的scope不被裁掉。
- 为LockImage、Readback、资源Flush/CS/GPU wait补低频scope；只包围原操作，不改调用
  参数、数量、顺序、DONOTWAIT/fence/资源所有权。FlushAndReset分出Native/EndFrame/Reset。
- JSON增加`mainThreadTimeline`，配套独立严格分析器。当前HTML旧树尚未展示新桶，
  不冒充已完成spark式前端；原始新JSON与离线摘要可以查阅。
- 清理当前producer的8个重复key的第二次emitter；原报告不改写，parser继续递归拒绝
  重复键。新R9/R10根JSON严格解析通过。
- 外部栈采样只用持有的启动HANDLE与精确TID；先挂起/取context/有限读栈/立即resume，
  恢复后才解析和写盘。WPR受SeSystemProfilePrivilege限制未能启动；未改系统安全策略。

## 运行事故与清场

- R1–R4为早期默认桌面证据，不能充当2560×1440隔离基线。R1 WPR失败，R3/R4
  外部新开进程句柄失败；R2仅为初步边界证据。
- R5客户区被窗口边框限制为2560×1415，判无效。R6改为仅本进程隔离桌面无边框后
  客户区正确，但外部挂起调用失败。R7执行会话中断，无正常结算记录；后续确认
  runner/游戏都不存在，核candidate/backup/live后恢复139B，另闭合其临时Video注册表值。
- R8真实backbuffer2560×1440通过，随后camera.snapshot请求触发Game+0x7E5766访问异常。
  dump EBP链为SnapshotJassCameraForTest→ProcessPendingInternalTestRequest→EngineTickUpdate。
  该命令经Engine worker运行，不可因路径标签写了main-loop就当JASS线程有效；没有在本轮
  扩大为JASS产品修复。runner已停用可选camera.snapshot，保持地图相机和零全局输入。
- R9、R10完成、无新增dump、精确启动HANDLE退出结算、桌面关闭，live均恢复139B，地图不变。
  最新Windows nvlddmkm/Display 153/4101事件仍来自9月3日及以前；本轮没有新事件。
- 前景桌面始终Default（R5之后）；只操作各自隔离桌面窗口，不切换、不激活玩家前台。

## 产物与验证身份

根目录：`AutoTest/artifacts/frame_timeline_20260913/`，受现有根.gitignore覆盖。

- 最终诊断DLL：34,481,749 bytes / PE32/i386 /
  `3646D9BFB2EBE79C4EFC29E1BCA0FE69137A409CE797AE92A8E9FB2160E77CB6`。
  `build-v3/build.json`：Below Normal/-j2，3对象+DLL共4/4，exact target no-work。
- R10完整报告：`mainloop_isolated_r10_20260913/war3_perf_report_auto_2026_09_13_20_22_48.html`，
  6,282,925 bytes / `25E41BE3C79913EF8542886F9E72B3AA91F181984EA881E8F6E3F89689C18360`。
- R10分析：`r10-timeline-analysis.json`，9,073 bytes /
  `6EE84BFE19BC1186C23C2B5403040221803E24B6FC1AF5C2F84E4E67425A1FAD`。
- R9栈原始证据：`mainloop_isolated_r9_20260913/wall-stacks.json`，2,535,801 bytes /
  `EB6F02D6D25E93374925B0393432D8B0832655B690FCA0E275D1A0D1611E80F3`。
- R9报告：5,709,788 bytes / `DE89EF214C968199DF0721C0C980E367DF56DDCDBC11669CEC55D3FD3BAB2834`。
- 恢复现场：34,450,973 bytes / `139BBE8CE6F941A176B9F4582E601E2841FC26956CB0DD7884903295F4505FD2`。
- 地图：5,857,721 bytes / `11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF`。
- 12项既有DataCollection + 11项新增timeline/static/analyzer通过；Win32 ledger断言通过，
  10次独立WOW64采样自检通过，py_compile/diff check/no-work通过；不是全量回归。

## 一手资料

- [Microsoft LockRect](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3dsurface9-lockrect)
- [Lockable backbuffer的性能代价](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresentflag)
- [资源锁定与CPU访问](https://learn.microsoft.com/en-us/windows/win32/direct3d9/locking-resources)
- [WOW64线程context](https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-wow64getthreadcontext)

下一阶段：先证明原生每帧LockRect的sync-only与真实读像素分支边界，再设计保持截图/读回
正确性的优化；同时继续细分0.70ms余量与材质proof内存查询。此文不宣称稳定优化、FPS收益
或所有GPU工作均已覆盖，不把未测的跳过同步方案写入根CHANGELOG。

## 原生约3ms为何没有同等等待：新增只读线索

用户追问后核对到`D3D9SwapChainEx::CreateBackBuffers`始终设置`desc.IsLockable=true`，
源码说明是内部BlitGDI回退需要；传入的D3DPRESENTFLAG_LOCKABLE_BACKBUFFER没有直接决定
这个值。D3D9Surface::LockRect转交LockImage，后者按desc.IsLockable检查。
git blame显示该行为已在仓库根导入900cd93中，不是本轮加计时新增的产品hunk。

Microsoft D3DPRESENTFLAG合同规定未请求LOCKABLE_BACKBUFFER的后缓冲不可锁定。
因此存在一个必须验证的条件分支：若游戏未请求该flag，原生D3D9可能拒绝同一次LockRect，
而当前DXVK允许它并执行同步读回。**尚没有本轮原生实际Flags/HRESULT证据，不能宣布已证实
此行为解释性能差或直接把IsLockable改成false**；内部GDI/GetDC/截图兼容也需保留。

另一种并存的解释是增强绘制/数据上传增加待完成的GPU工作，原有同步点承担其延迟。
当前只证明等待发生的位置，未量化后端差异与新增工作各占多少。旧“增强关闭3.031ms”
为历史非ABBA且旧计时窗口单轮，不是本次匹配场景的完整呈现对照。
下一轮应以原生D3D9+解限、最小DXVK、完整WarVK三组同条件记录实际Flags、LockRect次数/
返回值/读像素分支、读回尺寸与提交/完成/唤醒时间。当前仅源码审计与文档记录，无新运行。

## 后续实测纠正：游戏确实主动请求可锁定后缓冲

上节“原生可能因未请求flag而拒绝”是假设，不再作为当前案例的解释：
1. Game+0EC6B0反编译先清零0x38-byte参数，再将参数+0x2C(a2[11])写1；
   对应D3DPRESENT_PARAMETERS.Flags=LOCKABLE_BACKBUFFER。
2. R11真实创建Flags同为1，连续2000帧均在Game+ED0F6返回站点进行一次
   LockRect(NULL,0x810)，2000次均成功，全部2560×1440；无未知surface或其它caller。

R11完整Present间隔13.093914ms；LockImage墙钟4.820508ms(36.8149%)，其中GPU
wait4.661011ms(35.5968%)，CS0.144747ms；Outside0.648412ms。仍有前台并行负载，
比例同样会变化，不接受为FPS收益。原生D3D9的真实游戏HRESULT和等待时长仍未取得。

R12(禁用render hook及消费者)和R13(保留render hook、禁用消费者和semantic.data)
都未通过ready，帧号停在约82/83、gameStarted/runtimeReady=false，未开始采样。
两轮均恢复139B、video、关闭desktop、零进程、无新游戏dump。保留render hook仍失败，
因此不能把ready失败根因仅归于该hook；需独立核实启动/进图检测与消融配置的耦合，
不能将无效对照中的低数字或零数据当成性能结论。

独立API探针也未形成有效原生/DXVK对照：原生CreateDevice返回0x8876086C；DXVK
清屏/读像素循环有部分数据，但进程0xC0000005退出，probe日志含
VK_ERROR_OUT_OF_DEVICE_MEMORY，dump文件为空，无法解栈。移除显式FreeLibrary后
仍发生，故未证明为热卸载原因；停止进一步尝试。数据、错误、空dump全部保留。
后续只读nvidia-smi为4361/16380MiB、7%利用率，不足以将错误归咎于用户前台占用。
不引用这些失败探针的约2ms局部计时作为有效基线或可直接相减的“复制成本”。

新默认关闭census只观察创建/锁定，不改变IsLockable、Flags、LockImage返回结果或同步。
产物1A8C0F645BC7FE9CE245B185DCA061FEBA9DE5FF649AE70BC6650510576E7058，
34,486,618 bytes / PE32/i386；exact8/8、BelowNormal/-j2、no-work。实测与失败均未
晋升稳定版本。原生参数生成函数和锁定函数的证据注释已备份后追加到IDA并读回保存。
详细工件/恢复信息见AutoTest/artifacts/backbuffer_lock_census_20260913/checkpoint.json。

## 已完成的因果对照：sync-only 原生请求

后续完整反汇编与动态计数确认：每帧 request=1，但 native capture flag=0，没有像素
消费者。默认关闭实验只将该精确调用的 request 改为0，仍调用原函数并保留真实截图、
所有D3D9 API返回/读回、GPU资源fence和Present frame-latency fence。不是全局删等待。

同一EE90候选隔离2560×1440 ABBA（R15–18）完整帧均值13.154779→7.840445ms，减少
40.3985%；完整Present调用cadence提高67.7810%。两候选轮次都快于两对照，回切原策略
后等待恢复；GPU等待均值约4.697ms消失，Present尾部未增长，caster/geometry与GPU工作保留。
这是对所定位同步点的因果支持，不是原生300FPS的匹配复测，也不证明所有剩余时间来自采集。
实验仍仅在诊断recording owner开启，截图/输入延迟/长期/玩家前台验收待完成，不能作为稳定版。

[完整合同、逐轮指标、异常与证据](../plan/2026-09-13-native-frame-sync-experiment.md)。

## 2026-09-14 等待机制与吞吐量口径复核

本次仅解释/离线重放，不部署、构建或启动实机。四份 R15–18 原报告 SHA 与已冻结身份一致，
strict 根解析、每轮末 2000 帧 native-sync population、整数完整帧闭合全部重新通过。
完整区间均值 A=13.154778925ms、B=7.840444925ms；按均值倒数计算 76.018001→127.543782
Presents/s，时间减少40.398505%，吞吐增加67.781026%。这是同DLL隔离2560×1440历史ABBA，
不转移为新高压图、玩家前台、原生300FPS或显示器实际扫描吞吐的同比。

原生并不按FPS预算预设一个4–5ms的Sleep：普通帧request=1、capture=0也请求后缓冲
LockRect(0x810=READONLY|NOSYSLOCK)，只是没有CPU像素消费者；真正截图另有像素复制和
同步TGA保存。NOSYSLOCK不是DONOTWAIT，不表示可以提前返回尚未完成的数据。
在当前DXVK路径，render-target读回排image→buffer复制，WaitForResource先完成必要提交/CS同步，
再以资源不再in-use为条件等待；原生参数未指定DONOTWAIT。

时间来自QPC包围`m_dxvkDevice->waitForResource`，不是拿帧时间减掉旧树后反推：
每帧毫秒为`1000 * sum(endQpc-startQpc) / QpcFrequency`，再按同窗完整帧数求均值。
这测得主线程停在资源完成边界的墙钟，含相关GPU排队/执行/读回及完成通知/线程调度，
不是GPU-copy-only时间，也不是4.697ms纯CPU计算。Flush/CS有独立桶，不能重复相加。
GPU若已完成就无需同样的等待；原生D3D9与WarVK的负载、执行/读回后端和到达时间不同，
不存在“凡用暴雪截图路径每帧必付4ms”的结论。已实证Flags=1，不能复活“未请求可锁定
后缓冲所以原生拒绝”的旧假设。原生实际同场景LockRect的HRESULT/耗时对照仍缺失。

移除无消费者的普通帧读回恢复CPU/GPU重叠，并消除相应复制/提交开销，属于实际吞吐性能优化，
不意味着shader/场景算法突然少算40%，也不直接证明输入到显示延迟降低。
最新09:43玩家报告4096帧该锁定/等待为0，不能继续称其仍有本项约4ms停顿；
仍可能存在其它等待，须按其自身范围取证。
