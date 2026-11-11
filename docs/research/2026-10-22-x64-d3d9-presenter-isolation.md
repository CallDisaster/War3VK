# X2：真实 DXVK Presenter、非输入桌面和故障边界

状态：开发验证中，最终门禁结果见本页末尾。未接 Warcraft，未部署、提交、推送或发布。
本页接续[生产 COM 所有权收口](2026-10-22-device-owned-com-lifetime.md)，不改写旧失败回执。

## 数据链和证据边界

真正的 x86 `Direct3DCreate9` / D3D9 COM 代理，经受限 IPC 和专属 x64 owner，使用本仓库原有
DXSO、DXVK device/context/allocator/submission/retirement；本批再接原
`Presenter + DxvkSwapchainBlitter + DxvkDevice::presentImage`。没有另建 Vulkan 渲染器。

所有新 GPU 用例均由 `AutoTest/isolated_render_desktop.py` 创建真正的非输入 Win32 Desktop：
独立 HWND 消息泵、x86 API 线程、x64 host 都有实际 native Desktop 身份见证。启动时原子设置
Job 和继承 handle list，使用精确进程 HANDLE/PID/creation time 收口；不调用 SwitchDesktop，
不发送全局输入，不修改显示模式或玩家目录。CPU 桌面探针先于 GPU；清场、超时或外部强杀永不算通过。

当前只接受 64×64 的特定 D3D9 profile，仍拒绝 `Game.dll`。成功 Present 分列：acquire、
queue、API 返回、GPU 完成、资源退休、Present frame retirement。它们都不能代替实际图像检查。
显式诊断 profile 才申请 16 KiB GPU/CPU 图像存储，在同一 command list 将**实际 acquired swapchain
image**复制后再 queuePresent；之后只读冻结 CPU 副本。不读已经交给 WSI 的图像，不拿 source target
冒充 swapchain image。该测试不证明 compositor/屏幕 scanout、游戏画面或玩家视觉接受。

关闭读回 profile 不申请这份像素存储、不执行 copy、不返回像素 payload；仍有少量状态字段和条件判断，
不能宣传成“零指令开销”。诊断帧号与完整 BGRA/alpha、输出边界 guard 一起校验。

## 已保留的发现和失败

回执根目录统一是 `AutoTest/artifacts/`。

| 回执 | 实际结果 | 不允许外推 |
|---|---|---|
| `x64-presenter-20261022-r2` | 原始真实 Presenter + 隔离桌面门通过；8 Present、24 draw、48 primitives；source 40768 checks 零错，GPU 65/65/65，正常 COM/进程退出 | 此版没有检查 swapchain 像素 |
| `x64-presenter-20261022-r3` | 8 个不同底色的实际 swapchain 图像 20384 checks，加 source 40768，共61152零错；extent故障返回DEVICELOST、无追加GPU，但首个5秒退出等待失败 | 整体 `ok=false`，第二次等待看到退出2不能追认第一次 |
| `x64-presenter-20261022-r4` | 原有图像通过；marker/extent/destroy首组通过；新 clear-only 读回失败 | 绘制/读回预热的正例不能覆盖 fresh Clear→Present |
| `x64-presenter-20261022-r5` | 重跑在 CreateDevice 的 transport 等待失败；外层无超时/强杀，精确Job已空，输入Desktop未变 | 尚无充分故障栈定位初始化延迟，不称为已修复 |
| `x64-presenter-20261022-r5-clear-repro` | 保留r5源码/二进制的单例诊断：4096/4096 swapchain像素均为`0x00000000`，期望`0xff204060`；正常Finish后又见首个退出等待超时 | 单例是诊断，不是组合通过；保留首次失败和后续清理事实 |
| `x64-presenter-20261022-r6` | fresh clear和完整第一组7场景通过；第二次control正常Finish后首个5秒退出等待失败，secondary在140ms后见退出0 | 总`ok=false`；正常host JSON不能代替准确子进程已退出 |
| `x64-presenter-20261022-r7` | 补充健康owner的CPU finish-tail drain后，37个case与4轮7场景组合通过；准确主等待0–94ms；无外部超时/强杀 | 受限实验profile通过，不证明过去超时已取得根因栈、长期可靠率或游戏兼容 |
| `x64-presenter-20261022-r8` | 新C2的256帧clear-soak完成1048576像素检查零错，GPU513/513/513、Present256/256；但主退出等待5000ms超时，secondary1328ms后退出0 | 退出可靠性红门重新出现；总false保留，不以像素或后续退出追认。读方另发现startup pulse漏算及把HANDLE pulse当OS消息的两个测试错误，已单独修正 |
| `x64-presenter-20261022-r9` | 修正读方并增加冷退出里程碑后，clear-soak退出110ms；8帧诊断Presenter图像/GPU全过，却再发生5000ms主等待失败、secondary156ms退出0 | 此次已到`exit-intent-flushed`和`wmain-locals-complete`，两者同tick；定位到局部析构之后，尚不能指定CRT/模块根因；总false保留 |

`replay_render_host_presenter_fault.py` 只允许从完整来源回执校验并复制原二进制，在新隔离桌面重放单例。
不会替换成当前较新产物，不重新构建，不扩大等待时限。上述所有失败回执和原始日志均保留。

## 1. 修复 source 图像的真实交接，而非增加读回预热

源码确定的链：`clearRenderTarget` 只进入 CPU deferred clear；`endRecording` 的 timeline 完成不说明
尚未物化的 clear 已执行。`beginExternalRendering` 只处理已有命令及 shared 的非默认布局图像；
fresh source 始终为 GENERAL，可能根本不在 non-default 列表。

生产 D3D9 backbuffer 设 `DxvkImageCreateInfo::shared`，实验源图此前没有。补齐该标志是正确的布局合同，
但**仅补 shared 仍不足**。本次在宿主每次 Present 的准确 source 上使用已有 DXVK 接口：

```text
releaseExternalResource(source, GENERAL)
→ beginExternalRendering
→ 原 SwapchainBlitter
→ 可选 actual acquired-image 诊断 copy
→ acquireExternalResource(source, GENERAL)
→ synchronizeWsi / 原 submit / 原 present
```

release 通过原 `acquireResources` 对准确图像执行 deferred clear 和屏障；归还重新登记 raw blit 的访问。
该 source 有 COLOR_ATTACHMENT usage，归还选择 ExecBuffer，不会被提前移到 blit 之前的 InitBuffer。
`shared` 不代表导出跨进程内存，也不增加 source readback、独立 raw 屏障或第二套生命周期。

必须检查：fresh clear-only、连续不同颜色、原8帧draw/blend/alpha、故障后拒绝旧图、诊断关闭模式。
“Clear A→Clear B→Present”和 draw后clear等更广排列尚未自动继承证明。

## 2. 终态分类与收口

首次、acquire 前的窗口验证失败单独命名 `window-preflight`。只有正确 owner、CPU 设备状态健康、
没有 pending/current command list、没有 acquired image、资源不在用、作者资源账和typed表都为空、
GPU及WSI各阶段全部对齐，才允许走原owner拆除。仍永久返回DEVICELOST；`normalTeardown=false`，
另报 `gpuCleanupCompleted`，不把失败的帧当成成功。

其他状态继续 quarantine，特别是还存在作者资源或不能证明GPU状态。host发送完整终态回执后执行
仅作用于自身的 `TerminateProcess(GetCurrentProcess(),4)`，具名 `self-fail-stop`。不称其正常回收，
不运行不安全的GPU析构。外层必须在原5秒主等待内观察准确进程退出；Job兜底、secondary wait和强杀
均不能升级通过。0=正常结束，2=已证安全清理后的API终态，4=明确自停，三者不混用。

依据：[Microsoft ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess)
明确说明其他线程先终止、随后DLL detach可能遇到锁；
[TerminateProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess)
绕过DLL通知，但仍可能等待I/O取消/完成，因此不保证绝对有界。这个风险依据**不是**r3/r5已取得驱动死锁栈。

Connection现在在secondary drain前保留并记录首个异常，最后重抛同一异常；不再让清理超时盖掉原始
header/transport错误。只在主wait真正返回时标 `childProcessSettled`；退出4另报`childSelfFailStop`，
不能冒充`childNaturalExit`。读方还要结合host策略、终态reply和准确PID/creation证明。

### CPU finish队列尾部

独立源码审查发现 `signalFrame` 早于 finish线程放下其Presenter引用以及queue.pop。
仅观察frame signal不足以允许owner放掉最后device引用：可能由finish线程接手析构并自等待。
这是有源码依据的可构造竞态，**不是已取得r3故障栈证明**。收口要求在健康shutdown、仍持有
owner引用时使用原device idle入口先排空CPU submit/finish队列；未知device状态不得进入此路径。
它包含真实Vulkan idle，不能称零GPU等待；外部watchdog仍必须保留。

## 尚未覆盖的迁移效果

- 产品异步transport、游戏多线程入口、非丢帧背压仍未闭合；本实验是同步RPC，不能直接上游戏热路径。
- C2已消除资源ID和EVENT query Issue的累计64次上限，保留同时64逻辑句柄及原typed容量。
  资源`id/generation`与query自身revision已实跑；[R1/Q1回执](2026-10-22-x64-resource-generation-and-query.md)
  的r2通过不覆盖Presenter退出红门或r1未解释的45秒超时。
- x86仍保留buffer/texture CPU backing，尚无游戏x86 VA/commit减量或FPS/p95/p99对照。
- Reset/Ex、stateblock、完整纹理/mip/固定功能/多pass和WarVK实际pass未迁移；64×64单一profile不是全游戏。
- validation layer、其他GPU/驱动、compositor/前台视觉未过门。
- 玩家DLL和旧天气/生产参考DLL保持原身份；不使用本候选替换它们。

## r7 实际回执（2026-10-22）

`AutoTest/artifacts/x64-presenter-20261022-r7/receipt.json`：`ok=true`，源码、参考与玩家DLL身份保全。
独立桌面 `WarVK_RenderLab_56712_c2f494c154d2ad10` 已关闭，输入Desktop始终为`Default`。
共37个case：3个CPU-only初始化/关闭、4个remote/reference离屏对照、2个8帧Presenter配置，
以及4轮×7个窗口故障/控制配置。故障期保留live VB的4例退出4，明确是自停，不算正常GPU回收。

- 诊断8帧：24 draw / 48 primitives / 16 source readback，source 40768、实际swapchain image 20384，
  合计61152像素oracle检查零失败；8种底色、帧号1..8、BGRA/alpha与边界guard都独立核验。
- GPU submitted/completed/retired均65，8个WSI acquire/API/GPU/frame-retirement阶段各8；
  正常device/factory public refs=0/0，`shutdownQueueDrained=true`。
- clear-only和第二颜色、故障后不返回旧图、带64B live VB时quarantine、自停与准确子进程结算分别通过。
- 禁用诊断的独立配置同样通过，没有用读回预热掩盖fresh clear交接问题。

产物身份（仅开发实验，不部署）：

| 产物 | bytes | SHA-256 |
|---|---:|---|
| x86 proxy | 966144 | `3A999DAA23A867D37FC8C9015C0BC603D939557676A487E6CBDF3B185CD9A610` |
| x64原DXVK宿主 | 5476352 | `4A474B32D2513F8B722753CB234271965814C50F6AA3A5F879B32B9664F1209C` |

共用Presenter修改的全新x86产品构建和配置内回归已执行：
`device-owned-presenter-build-20261022/receipt.json` 与
`device-owned-presenter-regressions-20261022/receipt.json` 均为 true；116/116测试、全目标no-work，
源码与受保护DLL未漂移。新x86 DLL为34,476,032B /
`5A8B5B691BBB18473D975DC3CAB1BEDD3FB641A938D10E8AB5A6FFBA9BECAB8C`。
该离线通过不覆盖上表r8后来暴露的64位host进程退出红门。
本批不声称“64位迁移效果已充分验证”：游戏pass迁移、真实x86内存减量、性能和画面验收仍待后续门。

## 保留二进制的退出诊断（2026-10-22）

`diagnose_render_host_presenter_exit.py`先校验并复制r8原二进制，再在新非输入Desktop运行。
不重编译、不变更5秒native主等待/45秒外层上限；回执始终`acceptancePassed=false`。
default及repeat2分别在78/94ms退出，未到首个采样窗；这不是原故障修复。
`x64-presenter-r8-exit-diagnostic-repeat3-20261022`复现5016ms主等待失败、secondary156ms退出0。

repeat3在退出意图之后约302/1011/3001ms取得三份查询快照：精确PID/creation/镜像及父进程均核验，
只剩原主线程，其CPU计数在三次采样间不变。WCT只有单节点、没有可解释等待对象；87个模块相同，
其中有OBS hook和NVIDIA overlay，但这不能把延迟归因给它们。`vulkan-1.dll`已不在当时枚举表中，
也不能单凭此排除驱动/残留模块。原始退出日志、三份快照和所有失败产物均保留。

r9的局部析构完成标记与repeat3的单线程现场共同指向**进程收尾阶段待定位**；
下一探针须取得准确主线程调用栈，不能加长等待、强制正常退出或拿一次未复现代替根因证据。

## r9 实际退出栈与验证环境限制（2026-10-22 后续）

`x64-presenter-r9-exit-dump2-20261022` 保留了同一冻结 r9 八帧例的失败：图像/GPU通过，
`finish-reply-written` 到 `wmain-locals-complete` 都已到达，主退出等待仍超出5000ms，
随后secondary约313ms取得退出0。只在这次诊断里对已核实的PID35280 / creation
134371421256398060 / 原主线程45884取得46,202B的小型转储；它不是验收运行。

独立离线验证绑定了进程/线程创建时间、AMD64与原host镜像；原始失败receipt不改写。
SDK DbgHelp实际写入`0x201000`，即请求的ThreadInfo加AVX寄存器上下文，不是triage。
解析器现在只接受`0x1000`和`0x201000`；新增正反例与独立解释在原artifact旁，
`offline-dump-identity-interpretation.json`不把退出失败升级为通过。

离线栈位于 `NtDelayExecution → SleepEx → opengl32 → DllMain → LdrShutdownProcess`。
匹配GUID、DBI age与section table的微软公开PDB将OpenGL四个地址精确映射到：

- `+0xCAD8`：`vCleanupWnd+0xA8`，对`tidPaletteWatcherThread`的轮询，Sleep(50)，最多100次；
- `+0xC808`：`GLUnInitializeProcess+0x6C`；
- `+0xC265`：`DllMain+0x75`；
- `+0x5BF29`：`__DllMainCRTStartup`内部调用返回点。

这解释了本次**进程退出期约5秒停顿所在的代码**，不是draw/present未完成，也不是最近export名
`wglSwapLayerBuffers`所指的绘制调用。转储没有包含该全局变量的值，尚未证明谁创建了watcher、
哪个外部层导致其保留，不能单凭OBS/NVIDIA/ReShade模块存在归因。不改系统OpenGL，不滥用
FreeLibrary卸载不属于本项目的引用，也不以强制健康退出/放宽5秒门掩盖它。

固定r9的`implicit-off/default-a/default-b/implicit-off-b`四个新诊断都自然退出，但不能作有效A/B：
`implicit-off`的实际loader日志仍插入OBS、ReShade、NV_present、NV_optimus。当前高完整性进程下，
loader的安全环境变量读取未采用禁用过滤；**设置环境变量不等于过滤已生效**。这些回执继续
`acceptancePassed=false`。下一步仅对测试子进程准备restricted/medium完整性启动，先验CPU隔离
Desktop与token，再根据真实loader日志检验独立VVL；不改UAC、注册表或全局录屏配置。

## Medium 启动修复与有效层对照（2026-10-22 续批）

前述入口前 `0xC0000142` 已在同一冻结 kernel-only 探针上完成因果复测：
`render-lab-startup-20261022-r4/receipt.json` 的 x86/x64 均到达 kernel entry、User32加载、
准确非输入Desktop和CPU完成标记，退出0；随后完整父子CPU探针
`render-lab-medium-probe-20261022-r3/receipt.json` 也通过。

修复只在私有派生token的 `TokenDefaultDacl` 添加真实TokenUser的allow，保留原ACE顺序/字节。
源token真实前后读回不变，实际suspended child读回一致后才放行；不调整共享Desktop ACL、
Owner/Group、privilege、UAC或全局配置，无elevated fallback。
独审及DACL34/token37/Desktop13个pure测试通过；早期失败不追认。

同一冻结r9二进制的有效Medium off/on/on/off对照结果：

| artifact短名（均在AutoTest/artifacts） | 初始化观察 | 原5秒退出门 |
|---|---:|---|
| x64-presenter-r9-medium-off-20261022-r1 | 500ms | 78ms通过 |
| x64-presenter-r9-medium-on-20261022-r1 | 7125ms | 初始化响应超时，不能当退出失败 |
| x64-presenter-r9-medium-on-20261022-r2 | 4969ms | 141ms通过 |
| x64-presenter-r9-medium-off-20261022-r2 | 4610ms | 失败；secondary1640ms退出不能补救 |

off实际日志证明隐式层被禁用且未插入，故“关闭隐式层足以修复退出”的假设被反例否定。
初始化时限和进程detach时限是两个不同问题，所有诊断仍 `acceptancePassed=false`。

仅禁用层仍会扫描系统陈旧manifest。后续使用当前实际1.4.341 loader支持的**进程局部**
`VK_IMPLICIT_LAYER_PATH` 指向本次自有空目录；完整空源实验同时隔离未用explicit来源。
目录身份/空性前后校验，真实discovery block要求准确路径+Found no files，绝非只看env请求。
无ICD/driver替换、不改注册表；此为实验隔离方式，不是产品兼容修复。

`x64-presenter-r9-medium-empty-dump-20261022-r2` 在真实隐式/显式空源、无loader ERROR/WARNING
条件下仍复现原5秒退出失败。新的36,934B dump（SHA256 `5FBB27501FC09FE18529F61D6D4CE36BE7050E71B44E3C19E090234BB8CF20C7`）
独立离线LLDB实际unwind为16帧；OpenGL连续四帧仍为CAD8/C808/C265/5BF29。
PE/PDB GUID/age及文件哈希绑定通过。raw栈14个地址样值单列，未冒充调用帧。
回执为该case目录下 `offline-exit-dump-analysis.json`，工具为
`AutoTest/analyze_render_host_exit_dump.py`。原失败和原dump未修改。

尚未读到真实watcher全局值，也未证明创建者。下一步仅按精确子进程/模块身份增补有限只读观测；
不补猜测消息泵、不卸载非自有库引用、不改系统OpenGL或退出期限。
VVL严格读方与空源读方分别49/10个pure用例通过，但**VVL native尚未验收**。

## 当前 cache host 的 r10：初始化超时，不是退出或像素失败

`AutoTest/artifacts/x64-presenter-20261022-r10/receipt.json` 为 false，失败原样保留。
该轮当前host为 `888B801C…D3B4C3`。前三个CPU/初始化拒绝用例通过，首个`clear-soak`
在任何绘制前失败：原单次reply ReadWindow为5000ms，host的`init-apply complete result=0`
实际耗时5375ms。客户端先超时断管，host随后写回复失败并具名self-fail-stop；secondary
546ms退出4不能将主请求升级为通过。原始Job为空、Desktop关闭、raw与源码保全。

总门最后显示的`unexpected case record schema warvk-d3d9-presenter-fault-fixture/v1`
不是实际因果根因；真实stderr与host时间线证明上述先后关系。还没有阶段计时，不能据此
归因于driver冷启动、外部layer或shader编译；初始化只创建canonical cache，不编译应用VS/PS。
旧host的r8/r9耗时不是当前二进制的受控A/B。不延长原5秒门，也不通过重复跑绿抹掉失败。
下一诊断只在复制backend TU中加有界冷启动阶段计时，原源码/二进制与通过标准均不覆盖。

只读watcher诊断r1/r2分别自然退出94/109ms，均`acceptancePassed=false`。
r2证实仅加载映像NT头的8B ImageBase表示与磁盘不同，值准确等于已独立绑定module base；
现有严格guard因此停止，尚未读取任何TID，不能声称已取得watcher所有者证据。
见[下一探针记录](2026-10-22-palette-watcher-next-probe.md)。
