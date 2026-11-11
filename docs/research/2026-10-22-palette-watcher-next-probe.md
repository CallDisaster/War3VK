# 下一最小退出探针：读取真实 palette watcher 身份，不先添加消息泵

状态：**r5已取得同一watcher活体→结束后的句柄身份及未清零TID，仍是退出失败的诊断证据，不是修复或发布验收**。
接续 [Presenter 隔离研究](2026-10-22-x64-d3d9-presenter-isolation.md)。主线程仍独占 build/GPU/game lease。
本次只读源码、既有回执及 PE/PDB；用纯 Python 解读冻结文件，未 attach、启动 native fixture 或修改系统文件。

实施收敛：下文 P0/P1 保留原方案推导。实际实现统一挂在原 `ExitMonitor`，
**不在 dump helper 追加采样**；最多四个 pre-exit 与三个 exit 样本、每点两次 4B，
总请求不超过 56B，metadata 不超过 64KiB。此批不实施 HWND 读取/窗口枚举。
主线程已独立复跑 40/40 和既有 diagnostic self-test；后续只允许重放精确冻结 r9 输入，
原生 5 秒退出门及外层期限不变。采样失败是 unknown，不以零代替。

## 1. 已有实据与不能跳过的缺口

- `x64-presenter-r9-medium-empty-dump-20261022-r2`：主退出等待 **5016ms，失败保留**；
  图像/GPU 的通过、后续退出及 `nativeFixtureOk=true` 都不能覆盖该失败。
- 其 36,934B dump（SHA `5FBB27501FC09FE18529F61D6D4CE36BE7050E71B44E3C19E090234BB8CF20C7`）
  离线 unwind 与旧 dump 相同：`vCleanupWnd+0xA8 → GLUnInitializeProcess+0x6C → DllMain+0x75 → CRT`。
  对应 OpenGL RVA 为 `CAD8 / C808 / C265 / 5BF29`；不是最近 export 名表示的绘制调用。
- 当前明确缺少的是 **`tidPaletteWatcherThread` 的实读值、它在退出前所属的真实线程及其起始地址**。
  旧 dump 只含主线程；首个退出快照也只有主线程。退出后 `OpenThread(tid)` 失败不能证明该 tid 原来属于谁。
- host 已走 `Owner.shutdown`，并清空 device/adapter/instance；生产 library owner 有 `FreeLibrary`。
  退出快照中 `vulkan-1.dll` 不在，而 `nvoglv64.dll`、`opengl32.dll` 仍在，不能据此认定 instance 未销毁。
  下一步记录它们何时出现/消失；**不补无所有权的 FreeLibrary**。
- 禁用 implicit/explicit layer discovery 后仍出现同栈，不能再把“已过滤外部层”当作退出修复。

来源：上述目录的 `receipt.json`、`presenter-readback-eight-frames/offline-exit-dump-analysis.json`、
`exit-snapshot-1.json`、`host.log`；旧 `x64-presenter-r9-exit-dump2-20261022/offline-symbols/opengl32-offline-symbol-audit.json`。

## 2. 冻结绑定，不把硬编码 RVA 当通用接口

| 项目 | 本次冻结身份 |
| --- | --- |
| PE | `C:/Windows/System32/opengl32.dll`，983040B，AMD64，SHA `659BE03CF2B88B5063A35B2637D9EF38122D7F6F30DAFFF118FA41B273AA5ECE` |
| PDB | 692224B，SHA `309188E31218A16F8FE8C74A1E6BC70DFB48A047A185D71B981BB39D22C237B9` |
| GUID / age | `2632AC6B-D4D0-A889-43AD-9807B88C3546`；PE/DBI age=1，PDB Info age=3，沿用已有验证规则 |
| PE timestamp / checksum / SizeOfImage | `868189093 / 1026881 / 1097728` |
| 全局 | `tidPaletteWatcherThread` RVA `0xE4104`，已由冻结反汇编确认 dword 读取 |
| 起始地址候选 | PDB public symbol `PaletteWatcher` RVA `0x82130`；`PaletteWatcherProc` `0x822E0`；`StartPaletteWatcher` `0x82308` |
| 可选窗口关联 | PDB public symbol `hwndPaletteWatcher` RVA `0xE40F0`；加读 8B 前须另核引用指令宽度，不凭名称推出类型 |

本轮纯 Python 独立读取 PDB symbol-record stream 和同一 PE section table，得到最后三组符号 RVA。
这些是**符号位置**，尚不是本次进程曾在这些函数中运行、由谁调用或消息泵状态的证据。

未来每次远程读取前必须同时满足：

1. 继承现有 `prove_host` / `validate_dump_identity`：专属 Job、原 parent、精确 host HANDLE/PID/creation、
   原冻结 executable SHA/PE、native desktop witness 均一致；只操作实验 host，不接受任意 PID 参数。
2. Toolhelp 得到该 host 唯一匹配模块的实际 ASLR base/path/SizeOfImage；磁盘 PE/PDB SHA 与上表一致。
3. 有界读取该模块 DOS/PE/section/CodeView RSDS，核对机器、timestamp/checksum/size、GUID/age及 `.data` 范围。
   不能拿磁盘 SHA 单独代替加载映像绑定，也不能把含 relocation 的内存映像直接与整文件 SHA 比较。
4. `base + RVA + size` 做溢出/映像范围检查；`VirtualQueryEx` 核实该范围是该 AllocationBase 的可读、
   committed `MEM_IMAGE`，拒绝 guard/no-access。模块缺失、变更、部分读取均具名 unknown，**不用 0 代替失败**。
5. 只 `ReadProcessMemory` 精确 4B，核实返回成功且 bytesRead=4；读后再次核实进程与模块绑定。
   两次观察可以夹住变化，但不能声称它们构成原子的模块/线程生命周期快照。

只需 `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE`，既有 dump helper 已持有这些权限。
不申请写内存、VM_OPERATION、CREATE_THREAD、DEBUG privilege 或注入权限。
[ReadProcessMemory 的权限与完整读取规则](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-readprocessmemory)。

## 3. 实施顺序：先复用故障采集点，再补退出前身份

### P0：不改任何 native 输入，先让既有 dump 回执包含那 4B

在 `AutoTest/diagnose_render_host_presenter_exit.py::dump_helper` 已完成精确身份核验的位置、
`MiniDumpWriteDump` 前后分别执行上面的有界读取，写入同一 helper JSON。只重放已冻结 r9 case。
保持既有 1.5s soft-cancel / 3s helper 限额、native 5s 主等待和 r9 launcher 90s 上限；没有时间就记缺项。
新增采集不得通过重置计时器扩展原 dump 工作预算；不扩大 dump memory/thread flags。

这一步能把“进入轮询时先前非零”推进到“观察时实际值为 X/0/不可读”。它**不能独自证明** tid 的历史 owner。
如果同次有已绑定线程句柄，可查询其状态；否则只尝试打开该具体 tid，不全机查找“相像线程”。
保留 DbgHelp 可能瞬时暂停主线程的既有干扰说明；这些诊断运行永不晋升验收。

### P1：要回答线程是谁，必须增加少量 pre-exit 只读样本

现有 `ExitMonitor` 已在 `beforeGpu` witness 到达时绑定 host，但目前只在 exit-intent 后采样。
可在此线程上补 **最多四个**运行期采样点：首次绑定、绑定后约250ms、约1000ms、首次读到 init-apply-complete。
按实际到达时间执行、不补齐错过的点、不阻塞 host；退出后三个300/1000/3000ms点沿用原采样窗。
预退出样本只读目标全局与少量模块/线程字段，**不为每点重跑完整 WCT 或完整进程快照**。

- 观察到非零 tid 时，用 `OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE)`；立即核实
  `GetProcessIdOfThread == host.pid`、`GetThreadId == tid`、thread creation >= host creation。
  核对不通过就关闭并标 `identity-mismatch/unavailable`，不读取其他进程的线程细节。
- 对匹配的线程查询一次 `NtQueryInformationThread(ThreadQuerySetWin32StartAddress)`，动态解析API、
  检查返回状态与指针大小；记实际 start address 及绑定模块+RVA。只有精确匹配才命名 `PaletteWatcher`。
  原始线程 HANDLE 可保留至退出采样完成（最多2个不同 watcher实例），以 thread creation 区分 tid 复用。
  查询失败不升级权限；它是可选诊断而非产品依赖。
  [官方说明：线程起始地址及该内部接口的兼容风险](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryinformationthread)。
- 同样本只保留目标模块三项的在场/base/已绑定身份：`vulkan-1`、实际 loader 日志指定的 UMD、`opengl32`。
  消失/卸载竞态单列，不从单次枚举缺失推断引用计数或责任组件。
- 如4B不足，再单独批准并验证 `hwndPaletteWatcher` 的8B读取；先查 `GetWindowThreadProcessId`，
  必须与**已持有的精确线程实例及 host**相符，再记录 class（固定长度）、desktop身份等无标题查询。
  窗口查询前后必须确认保留的线程 HANDLE 仍未 signaled；线程已退出时不凭相同数字tid重新认领窗口。
  不把目标进程里的 `hPaletteWatcherThread` 数值当成本进程有效HANDLE，不 DuplicateHandle 或注入执行。

预退出身份样本与P0的故障点读值可以用同一实现，但权限/输出的 opt-in 和证据层级必须分开。

## 4. 窗口/消息泵：只描述能观察到的事实

可对已绑定 watcher tid 调 `EnumThreadWindows`（最多32个回调、仅保留同PID/TID窗口）及一次
`GetGUIThreadInfo(tid)`（绝不传0，以免查询前台线程）。任何 HWND 关联都前后核验 owner；跨桌面失败报 unavailable。
不发送 WM_NULL/WM_CLOSE/WM_QUIT，不 `PostThreadMessage`、`SendMessage`、`AttachThreadInput` 或 `SetThreadDesktop`。
也不运行 watcher 私有函数、改全局值或替它抽消息。

**窗口/输入队列存在，不等于正在运行消息泵；枚举为空/查询失败，也不等于没有消息泵。**
`EnumThreadWindows`只承诺 nonchild window 枚举，不能由此排除所有其他窗口形式；
`GetGUIThreadInfo`可能因线程不存在或没有输入队列失败。
[EnumThreadWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumthreadwindows)、
[GetGUIThreadInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguithreadinfo)。
确需确定 GetMessage/DispatchMessage 进度，应另提精确线程栈/有因果依据的受控实验，不在本探针暗加干预。

## 5. 启动延迟、GPU初始化、退出必须分账

- 源码 `d3d9_host_main.cpp:345` 的 `processStart` 是 **wmain入口**，不是 Windows process creation。
  `processElapsedMs` 不包含 loader/CRT 进入 wmain 前的真实启动时间，不可拿它诊断冷启动全部延迟。
- 分列 exact creation FILETIME、monitor首次观察witness时间、`init-apply begin/complete`（GPU初始化段）、
  `finish-reply-written / after-session-reset / exit-intent-flushed / wmain-locals-complete` 的原tick，
  最后以准确 process HANDLE 主等待结算。日志读取延迟另记，不能伪装执行时刻。
- 本例 init-apply为1282ms、wmain至exit-intent为2500ms；退出期另超出5016ms。
  三者有不同起点，不能相加/挪用后声称“都是启动慢”。
- 样本记录 requestedAt/actualAt/collectionMs；API无硬实时保证。保持现有外层看门狗，采集拖延只使证据降级。

## 6. 冻结门与结果解释

先补纯测试：错PID/creation、模块路径/PE/PDB不匹配、RVA越界、短读、模块卸载、0与读失败、
tid复用/外进程tid、start地址未归属、窗口被销毁/改owner、采样遗漏及预算耗尽；新代码不得在关闭时读取VM。
建议 monitor运行期/退出样本 <=7，远程metadata读取总额 <=64KiB，tid数据总额 <=64B
（每点前后各4B，包含可选dump helper的一组；不能每次 helper 又重新增加完整采样预算）；
可选窗口数据另列，不顺带抓堆/全内存。每项记录原始错误码，不以 try/except 静默成功。

若已绑定的 watcher线程退出，后续仍读到同一非零tid且主线程位于原轮询栈，可加强“stale watcher退出状态”解释；
仍不能只凭线程 start 位于OpenGL就识别谁触发创建。若未采到活体owner/start，诚实停在全局值证据。
任何结果都不授权改系统DLL、延长timeout、以强制healthy exit替代自然退出，或提前宣称GPU/游戏迁移完成。

## 7. 实际r1/r2与映像校验缺口

两次固定r9、restricted-Medium、实际隐式/显式空源诊断均保留`acceptancePassed=false`。
目录为`AutoTest/artifacts/x64-presenter-r9-palette-watcher-20261022-r1`与`-r2`。
原5秒门下自然退出分别94/109ms，不能覆盖此前同栈失败或证明已修复。

r1的4445B元数据读取证明在NT136B guard处拒绝。随后只增加同次已读字节的差异报告，
未增加远程读取/权限/预算或豁免；主线程复跑45项纯测试通过，另覆盖三段guard全部224个字节位置。
r2得到精确差异：ImageBase字段RVA0x128、8B，磁盘值`0x180000000`，
内存值`0x7ffaf51a0000`，准确等于该次独立模块枚举/身份核验所得base。四个差字节全在该字段，
其余NT guard字节相同。报告仍`guardExempted=false`、`mutationLegitimacy=not-determined`，
0 TID字节、0认证pair、0线程owner；原回执不补写成成功。

下一窄修仅考虑冻结PE的两种**完整**ImageBase表示：preferred，或已独立绑定的实际module base；
首次身份验证锁定所见表示，后续表示改变仍unknown。其他header/RSDS/MEM_IMAGE/精确进程检查不减，
不逐字节混合两模板，不把未知基址当通配符，不声称已证明具体系统改写者。此处是测量工具的
身份表示缺口，不是导致旧进程detach等待的根因。

## 8. r3/r4/r5：活体watcher与失败退出时的未清零状态

主线程应用上述完整8B表示校验，首次表示锁定，后续变化仍拒绝；55/55纯测试与diagnostic
self-test通过，代码仅修改观察器及测试，不修改native输入、原5秒门或读取预算。
正式watcher SHA `2BA913042A28539330C1959E792952980A4F73D652EC3CBAD4663EE381467449`。
预定三次固定r9诊断r3/r4/r5分别主等待110ms、125ms、5000ms；末轮明确失败，未重试求绿。
三次仍`acceptancePassed=false`，也不将固定旧r9诊断套到当前cache host作为验收。

r5回执：`AutoTest/artifacts/x64-presenter-r9-palette-watcher-20261022-r5/receipt.json`，
SHA `83B59B8D4F96E48942D3381BAFF52CCF194B212267D42731A41F739FF8974C4C`。

- 精确host PID51152；watcher TID55712 / creation134371530049964866，
  退出前两次观测活体，起始地址`0x7ffaf5222130`，冻结OpenGL内RVA0x82130，精确符号`PaletteWatcher`。
- 保留的同一个线程HANDLE在exit后321.062/1005.684/3009.722ms三个样本均已结束，
  exit FILETIME134371530058170440一致；每个样本的两次全局读取仍为55712。
  认证的是历史线程对象已结束，不凭同一数字TID认领可能复用的新线程。
- 初始化1031ms、原GPU提交/完成/退休65、8帧Present、逻辑资源归零，
  host已到`wmain-locals-complete`。主进程退出门5000ms失败；secondary219ms退出0不补救。
- 该次未重新取调用栈；与旧匹配PE/PDB的`vCleanupWnd`轮询栈联合解释，而非冒称同次栈采样。
  读取是有限离散点，不证明中间每一瞬间恒定，也不证明线程由哪个外部调用者创建。

实据已从“只知道系统函数在等待”推进到“该历史线程已经结束但被轮询的状态仍非零”。
接下来先离线核对正常停止/清零写者，以及项目是否拥有可提前合法结束的边界；
不写系统全局变量、不调用私有停止函数、不发送外来WM_QUIT、不额外FreeLibrary。

## 9. 冻结映像的正常创建/停止路径（离线，不是同次动态调用栈）

独审重新绑定相同PE/PDB，未执行/attach/修改系统DLL。
`wglSetPixelFormat`在RVA0x8833调用`CreatePwnd(0x8A90..0x8D38)`；后者在0x8B32调用
GetWindowThreadProcessId、0x8B4A取当前PID。不同PID分支0x8C79设置标志2，0x8C7D调用
`StartPaletteWatcher(0x82308)`；同PID分支安装`wglWndProc(0xBD40)`。
这仅是可解码静态直接路径，不声称已排除间接调用，也未实测driver到此的调用者。

`wglWndProc`处理WM_NCDESTROY时在0xBE53调用`pwndUnsubclass(pwnd,0)`，随后cleanup/free。
`pwndUnsubclass(0xD2F4..0xD427)`的标志2路径递减watcher users；最后一个使用者向系统自有
watcher窗口发WM_CLOSE，普通参数0路径在0xD409主动清零TID；detach参数1依赖线程正常尾部。
`PaletteWatcher`尾部0x8229C在临界区中核对当前TID后清零，但进程退出时历史线程可能已终止。
这与r5观察相容，尚缺内部user count/PWND参数/PostMessage结果，不能称全链根因闭合。

没有可直接调用的公开StopPaletteWatcher API；当前Vulkan路径没有自持HGLRC，不应猜测调用
wglDeleteContext或用临时GL上下文“试着清理”。下一动作仅限项目自有窗口正常寿命对照，
详见[初始化与surface所有权诊断](2026-10-22-x64-initialization-and-surface-owner.md)。
