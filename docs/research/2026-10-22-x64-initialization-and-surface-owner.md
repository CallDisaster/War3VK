# x64 初始化分段与 surface 窗口所有权诊断

这是开发诊断，不是完整迁移、游戏内存收益或发布验收。产品源码和玩家 DLL 未因本实验改变。
本次数据链为 x86 COM proxy → 同一 IPC → x64 DXVK/原 Presenter → 实际 acquired-image 回读。
由主线程独占编译/native/GPU lease；所有 native 使用 restricted-Medium、非输入 Win32 Desktop。

## 已完成的三轮初始化诊断

工具：`AutoTest/diagnose_render_host_initialization.py`，复制 backend TU，只插入有界计时；
保留原编译/链接 flags、原 5000ms reply 和 exit 门、45s 外层门。32项纯测通过。
对照原 r10 的 5375ms Initialize 失败；原9份 raw manifest仍原封，不追认失败。

| AutoTest/artifacts 下的目录 | init总计ms | instance ms | swapchain ms | 自然退出ms |
|---|---:|---:|---:|---:|
| x64-init-stage-20261022-r1 | 2597.236 | 1155.411 | 1136.543 | 94 |
| x64-init-stage-20261022-r2 | 3104.688 | 1649.920 | 991.274 | 110 |
| x64-init-stage-20261022-r3 | 2306.217 | 874.119 | 1211.091 | 125 |

每轮真实256帧、1048576像素、pixel/alpha失败0，原提交/完成/退休513，28条身份绑定marker。
Job空、Desktop关闭、input仍Default、无强制清场。独审逐轮重验1363条文件身份、23份raw、
29声明链接输入、8 thin archive和171外部object，实际原始输出通过既有严格读方。

receipt SHA-256：

- r1 `6B8AE7FB8EA3079DC5B29AF2E448CF6002BFBC2154F6A15A6D2FD80EC2539C62`
- r2 `73FFE88829DBC915B064E453C293A49039114FBA0F35D7F184087F3A7E960DCC`
- r3 `D589937DAB336E42FDD23FDC32B0F242392F142A6AAA629DE7B3C2DA7E1B7E72`

三次实际EXE SHA不同，属于同源同编译配方的独立链接，不是同二进制重复。
已测先前日志写出28.242/69.087/27.218ms不含最后marker自身，也没有独立剥离时钟开销。
历史完整环境没有记录，当前安全配方不等于严格same-env复现。
**三轮没有复现初始化超时；不能由快轮耗时分布推断r10慢在哪里，更不能宣布修复。**

## 退出机制的新证据与待做实验

在冻结OpenGL映像及匹配PDB的只读反汇编中，`wglSetPixelFormat → CreatePwnd`确有跨进程分支：
目标窗口所属PID不同则调用`StartPaletteWatcher`；同进程则安装`wglWndProc`。
同进程窗口的`WM_NCDESTROY`进入普通`pwndUnsubclass(...,0)`清理；进程detach使用另一种清理模式。
这与r5“历史watcher已结束而被轮询TID仍非零”相容，但尚未采到驱动调用入口/PWND参数，不能完整归因。
具体RVA、模块和线程身份见[watcher记录](2026-10-22-palette-watcher-next-probe.md)。

不调用私有OpenGL地址、不改系统全局、不卸载非自有引用、不向系统watcher发消息。
合法实验只调整项目拥有的 HWND/surface 寿命。Vulkan要求先销毁swapchain，再断开surface；
它不负责关闭native窗口。[Khronos合同](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySurfaceKHR.html)
窗口必须由创建线程销毁。[Microsoft合同](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-destroywindow)

### 实验A/B-1：只改peer窗口正常关闭顺序（尚未运行）

保持foreign HWND。等host完成GPU/Presenter清理，session-bound握手通知原x86 window owner正常关闭，
再允许host自然退出。所有等待计入原期限，不另加宽限。正在独立draft，不与A/B-2混合。

### 实验A/B-2：只改实际surface目标（r1已native，仍不授予验收）

工具：`AutoTest/diagnose_render_host_surface_owner.py`，辅助头
`AutoTest/render_lab_owned_surface_target.h`，不加入任何产品/host Meson目标。

- 一次编译同一诊断EXE，预定 peer/host/host/peer 四轮，不重试至绿。
- 两臂都创建相同独立64×64 host窗口；A surface仍指向peer，B指向host窗口。
- 原peer HWND/PID/TID/nonce和每次validateWindow完整保留，stat的peer语义不变。
  另记真实surface HWND/PID/TID/HINSTANCE、host-window thread creation、独立marker和Desktop。
- 使用相同256帧clear-soak及真实swapchain回读，不用source图冒充呈现图。
- 原GPU/WSI退休 → CPU队列排空 → Presenter.destroyResources及析构返回 →
  own window线程DestroyWindow/WM_NCDESTROY/退出/注销 → 再释放device/instance。
- 正常关闭在原Finish RPC的5000ms内；原primary-exit5000ms不变，读方不把secondary清场升级为通过。
- 继续有限只读watcher观察。未装载OpenGL/缺失样本是unknown，不记为已清零。
- 所有结果`acceptancePassed=false`。B不是向原peer HWND显示，不能用旧读方通过冒充原目标合同已过。

初版13项pure transformation/reader测试及prepare1通过；独审随后发现DWORD字段被统一按64位读取，
以及proxy/monitor两路最终身份未交叉比较。修为分域检查和最终交叉绑定后，15项pure测试双独立通过。

r1四轮实际完成，回执 `AutoTest/artifacts/x64-surface-owner-20261022-r1/receipt.json`：
`B1C633744FF59EB7382AD9603DD32F27033D8A768511CB09EEAECBFC8ECAD237`。
实际同一host 5537792B / `339F8C6B718B0880AC43D2F1675F792ADBB58DCC8560224E9821AFA266C8C721`。

| 臂 | surface目标 | init ms | 原5秒退出门 ms | 认证watcher双读 |
|---|---|---:|---:|---|
| A1 | x86 peer窗口 | 3424.918 | 172 | 42548，精确活体PaletteWatcher |
| B1 | x64 owned窗口 | 3372.795 | 172 | 0，已绑定OpenGL且valueCertified |
| B2 | x64 owned窗口 | 1842.674 | 93 | 0，已绑定OpenGL且valueCertified |
| A2 | x86 peer窗口 | 1718.208 | 109 | 56724，精确活体PaletteWatcher |

每臂256帧/1048576像素、pixel/alpha失败0，合计1024帧/4194304像素；GPU提交/完成/退休各513。
owned窗口关闭耗时0/78/31/0ms，Destroy/NCdestroy各1、class注销、线程正常退出；两臂都真实创建。
主线程编译有6条既有warning，不称warning-free。独审重验1379输入身份记录、26 raw封印、
29链接输入/8 thin/171外部object；同EXE四个执行副本确认，严格reader直接重读原始输出通过。
所有Job为空、Desktop关闭、Default输入不变，无强制终止，原r10及生产/玩家DLL未改。

**实际支持的因果结论：当前本机固定样本中，改变surface HWND所有权改变了初始化附近watcher的触发。**
B的0不是缺模块冒充；较早module-absent仍为unknown。有效采样是离散点，不能说全寿命始终无watcher。
本轮A也正常退出，所以不证明旧5秒退出故障已经修复；旧红门保留。
下一批沿用这个EXE，在预定独立矩阵内验证peer故障仍拒绝、shader八帧及更长负载；不重试凑绿。

后续若B显著改善，需要另行设计host-owned child嵌入、resize/DPI/父进程先退出/输入边界；
不能直接把top-level实验放到游戏或声称生产async/完整D3D9迁移完成。

## 同一EXE的固定12例续验：旧窗口退出红门复现，host-owned六例通过

2026-10-22 23时续验，工具`AutoTest/replay_surface_owner_continuation.py`，不重编、不改期限。
回执`AutoTest/artifacts/x64-surface-owner-continuation-20261022-r1/receipt.json`，SHA-256
`83CCAC357D2A4423A4B6F14A43E1C4F45FA5C3CD01BA4B8C538D97FCA40005CB`。
仍是上述`339F8C6B…C8C721`，使用r10冻结的proxy及fixture。

| 固定顺序 | Surface所有者 | 原primary exit ms | 严格工作负载结果 |
|---|---|---:|---|
| 1 clear-soak 256帧 | peer | 5015 | 失败，primary timeout；secondary再141ms退出0不补救 |
| 2 clear-soak 256帧 | host | 79 | 通过 |
| 3 clear-soak 256帧 | host | 94 | 通过 |
| 4 clear-soak 256帧 | peer | 5000 | 失败，primary timeout；secondary再140ms退出0不补救 |
| 5 marker失效 | peer | 78 | 通过，具名拒绝并正常清理退出2 |
| 6 marker失效 | host | 94 | 通过，具名拒绝并正常清理退出2 |
| 7 extent失效 | peer | 5000 | 失败，primary timeout；secondary再234ms退出2不补救 |
| 8 extent失效 | host | 94 | 通过，具名拒绝并正常清理退出2 |
| 9 peer销毁 | peer | 94 | 通过，具名拒绝并正常清理退出2 |
| 10 peer销毁 | host | 156 | 通过，具名拒绝并正常清理退出2 |
| 11 shader八帧 | peer | 78 | 通过 |
| 12 shader八帧 | host | 94 | 通过 |

整组`diagnosticComplete=true`、`workloadAllPassed=false`，runner exit1、`acceptancePassed=false`。
不能因12个fixture进程都exit0就算全绿：三个proxy的原primary wait确实失败，读方已拒绝。
失败原样保留，只有launcher Job空、exact host已结算、observer已停止才继续下一预定例；没有重试。
输入Desktop始终Default，专用Desktop关闭，全部Job归零，无强制终止、无游戏/系统变更。

本轮支持：**在同一冻结诊断二进制的这组测试中，host-owned target六例全部通过；peer target三例复现
原退出超时。host target没有绕过原peer marker/extent/destruction拒绝。** 不宣称普遍无退出风险，
也不把独立top-level窗口等同游戏窗口嵌入或完整Presenter接受。初始化本轮未超5秒，不解释旧r10。

测试接线收紧：原初始化reader固定28条marker，但非readback故障profile实际只有26条。
新增显式、keyword-only、严格bool的`readback`参数，False只去除两条读回阶段；不得从缺失日志猜测。
初始化38项、continuation27项纯测通过。旧reader与test逐字归档至
`AutoTest/artifacts/surface-owner-reader-revision-20261022-r1/`，continuation明确绑定old/archive/new SHA；
其余历史输入、所有二进制和原始日志不豁免、不更新旧receipt。

六fault臂的exit-watcher/WCT采样未覆盖：旧monitor只匹配normal-return/0，不匹配正常故障清理的
owner-cleanup-return/2。退出主等待仍由proxy真实日志证明；不把未请求样本当作快速退出观察。
下一步为该host-owned诊断单独运行严格VVL，保留原完整C2门，随后再处理产品窗口方案和X1异步。

## 独立VVL首轮：真实函数表缺陷与验证层误报分别处理

回执`AutoTest/artifacts/x64-surface-owner-vvl-20261022-r1/receipt.json`，SHA-256
`850F39FC0C95A7C65AC83AEB070D3712ADC025D3CA3BAEEEF7751BCB6392DF1D`。
同一冻结host下的256帧与shader八帧工作负载通过，但VVL门失败、runner exit1，**不追认全绿**。
独立VVL原始日志确实证明同步验证启用，并非只安装了SDK；两例都有函数表警告，256帧例另有三条
`UNASSIGNED-Threading-MultipleThreads-Write`（Acquire与WaitForPresent2使用同一swapchain）。

- `vkGetPhysicalDeviceMultisamplePropertiesEXT`误放在`DeviceFn`，每次建表就从
  `vkGetDeviceProcAddr`查询；物理设备查询应走instance resolver。这是本项目真实接线错误。
  已仅把这个成员移入同扩展守卫的`InstanceFn`，setter仍留device；未改渲染算法/预算/同步。
  原header逐字保留在`vulkan-dispatch-preimage-20261022/vulkan_loader.h`，SHA`6C82092B…FDBEB5`；
  新header`60C10887…5464CB`。实际header/loader/仓库registry的11项定向静态测试通过，含8个反例。
- Acquire/Wait并发不能直接判为生产竞争：[Khronos规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForPresent2KHR.html)
  明确允许Wait与除Destroy之外的其他swapchain操作并发。VVL1.4.335的自动生成线程跟踪没有应用此例外。
  上游[同类问题13167](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/13167)及
  [修正fd8605f](https://github.com/KhronosGroup/Vulkan-ValidationLayers/commit/fd8605f99aa030a4693cf42362fedfe4ff73be78)
  已明确处理WaitForPresent2。**不因此加热路径锁、禁用present-wait或屏蔽VUID。**
  Destroy仍须真正等frame worker；本项目`destroySwapchain`等待队列清空、worker在Wait返回后才pop。
  新VVL对wait-vs-destroy仍有检测缺口，零VVL错误不能替代这条生命周期证明。

2026-10-23零时已完成独立fresh x64构建`build64_render_host_dispatch_20261022`，原build及玩家DLL保留。
另取官方固定commit`edeeee0ff5ce6d79d0c4ccfc28aa9c18e2b94144`的Windows layer到项目artifact目录，
只对测试子进程显式指定，未安装/替换SDK、驱动、loader或注册表。压缩包SHA
`CF02EC73059E48669C3424CBA9B221FAC81CD05B3C987866DEFE4F9C2E31A170`，API1.4.365。
下一步先用旧host+新layer确认原函数表warning仍可见，再用新host+同layer复核；各自保留独立receipt，
原1.4.335失败不改写。完整C2、游戏窗口嵌入及迁移收益仍未验收。

## 2026-10-23：VVL365版本化对照的实际结果

第一轮新layer旧设置的control确实失败，回执`x64-dispatch-vvl-control-20261023-r1/receipt.json`
SHA `8F82518E8EB56D220DA786231680FED3F5655077AD05D0BE2783BF0C82FEFFC2`。
除原GPA外，独立raw还报`VALIDATION-SETTINGS`：`syncval_submit_time_validation`已弃用。
这份失败不由后续适配追认。新独立适配器`render_lab_vvl365.py`按固定官方源码改用
`syncval_full_validation`并显式开启record-time；其他配置、线程检查及无过滤政策保留。
新版Information完整六类横幅与非零VkInstance须唯一，新旧横幅混合拒绝；18项纯测通过。
读方明确`runtimeFilterLimitProven=false`和`runtimeAllSettingsProven=false`。

| 受控同层测试 | 工作负载 | 独立VVL原始记录 | 结果 |
|---|---|---|---|
| 旧host，正确新设置 | 256帧clear + shader8 | 每例一个完整新横幅 + 一个原GPA警告 | 工作负载通过，VVL失败保留 |
| 新dispatch host，同设置 | 同上 | 每例完整新横幅，无warning/error | 两例VVL与工作负载均通过 |

旧host正确配置回执`x64-dispatch-vvl365-control-20261023-r1/receipt.json` SHA
`7F6D74DC1AB1771E74867C89A6AC211140662FE84B891F04F6DEC238D4CC36DE`。
独审从原始日志复算：clear 1,048,576像素/513 submitted=completed=retired，shader 61,152像素/
65=65=65；present退役256/8，最终live资源/handle/query均0，两个primary exit均94ms。
其当时读方源保存在`vvl365-control-reader-preimage-20261023`；后续只收紧混合横幅反例，
不改旧回执的源码身份。旧版335、首次365配置失败的raw均保留。

新host回执`x64-dispatch-vvl365-candidate-20261023-r1/receipt.json` SHA
`D1A32369A9BC026E5BC78C4B4C6FD040C36A7D1F9CE994D2F90BDE2CEE25A0CA`。
实际copy-TU诊断EXE 5,537,792B / `9BCFF0F730A468DCE23F83626A2D2BEFBB4A21A8992F8FB46A0F6EC06BEEF715`，
链接fresh `build64_render_host_dispatch_20261022`，不是旧host重新贴标签。
两例`workloadPassed/settlementCertified/vvlDiagnostic.checksPassed=true`；
`diagnosticComplete/vvlChecksPassed/frozenInputsPreserved/rawArtifactsPreserved=true`。
全部restricted-Medium非输入Desktop，保持原5秒与45/90秒外层期限，不安装系统layer、不替换玩家DLL。

**接受范围仅B2_VVL365_VERSIONED_TWO_CASE_NOT_FULL_C2。**
旧peer退出与初始化红门不被覆盖；top-level host窗口不等于游戏嵌入；VVL没有报告错误不等于所有竞争均被证明不存在。
下一边界是实际异步Clear容量/保序/故障、随后host-owned child的独立寿命门，仍不宣称完整迁移或游戏内存收益。
