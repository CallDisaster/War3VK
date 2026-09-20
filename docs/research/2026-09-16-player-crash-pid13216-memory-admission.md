# PID13216：玩家闪退现场与取证内存准入

状态：只读现场 + 源码防护候选。不是崩溃根因完全闭合，不是新 DLL/实机通过。
本轮优先排查闪退；用户已明确将闪烁取证延期到其返回后。

## 已保留的现场

- War3 PID13216，创建于 2026-09-16 00:33:57 +08:00；x32dbg PID32228，00:34:11 创建。
- 玩家目录 `E:/Work/Warcraft III/d3d9.dll` 与未重建的 build32 都是
  35,865,159 bytes / `2D4BCB400CBCA7524403E5C0939C679450865A4A48B47739941F9D697077FBC5`。
  这是磁盘身份；没有通过 ReadProcessMemory 证明整个已加载映像逐字相同。
- 同 PID crash_handler_status 记录 UEF-only、first-chance disabled。
  `runtime_status.json` 最后更新时间 00:34:41，frameIndex=2400，isInGame/inGameRenderReady=true。
  当前 PID 对应 `WarVK/Log/FrameEvidence/history-13216-100587015569-1` 在 00:34:33 创建，仍无文件。
  这能支持本地录制开始过，不能证明所有 GPU 图像/输入槽成功建成。
- 目录中的 `latest_crash.json` 仍属于 9月15日19:43的 PID43700；绝不能拿旧 dump 当本次异常。
  当前地图的精确字节身份未从调试进程核到，不把本次自动并入旧“生与死”样本。

用户两张 CPU 页截图（不需要重复开图）：

| 文件 | SHA-256 | 看过的关键信息 |
| --- | --- | --- |
| `D:/tmp/AppData/ADMINI~1/Local/Temp/codex-clipboard-0346aec1-59e2-46f5-bf92-142600114f74.png` | `C72BC173990167D728316722D2FF30FFD5493424F0B2ED0F189DD340997E69F6` | PID13216/TID4212、寄存器及错误状态 |
| `D:/tmp/AppData/ADMINI~1/Local/Temp/codex-clipboard-3fe3d498-0942-4dfe-a61a-67ecddbabbbd.png` | `E702BF7DD799AA288527EB829A5BFB64D186130114F4D4A051F89D17C72DB6CC` | 完整指令地址与参数栈 |

截图明确给出 EIP=`5C2D71D7`，`cmp dword ptr [edi+30],1`，EDI=0。
因此该指令的读取地址是 `0x30`。同线程显示 LastError=8 / ERROR_NOT_ENOUGH_MEMORY，
LastStatus=C0000017 / STATUS_NO_MEMORY。EAX=FFFFFFC0、EBP=053DF478、ESP=053DF474。
未取得原始 EXCEPTION_RECORD，因此不杜撰异常代码、first/second chance 或请求分配字节数。
LastError/LastStatus 可能来自较早调用，不能单独证明“紧前一次分配返回空”。

## 符号纠正与当前判断

对 exact 2D4B 使用 nm/objdump，导出 PointLightCount 的链接 VA=62666140，ImageBase=62440000。
用户粘贴的三处“PointLightCount+大偏移”一致推得运行导出 VA=12AE6140，推导模块基址=128C0000。
这是从同一导出偏移推导，不冒充 debugger 模块表的独立证明。

| 用户地址 | 还原到链接地址 | 实际符号 |
| --- | --- | --- |
| 13239428 | 62DB9428 | pthread_spin_lock + 8 |
| 1323AB98 | 62DBAB98 | pthread_getspecific + 38h，位于 spin_lock 返回后 |
| 12EAC985 | 62A2C985 | Hook_WorldDispatch 内 |

主线程不是据此被证明在 PointLightCount 内崩溃；NVIDIA 栈上的 vkGetInstanceProcAddr+大偏移也
不能当成实际 Vulkan API 函数名。优化后的 x86 栈回溯可能漏帧，暂不推断具体锁对象或死锁。

只读进程计数：private=1,339,854,848，working set=1,319,104,512，virtual=2,040,848,384 bytes。
玩家 War3.exe 的 PE Characteristics=0102，没有 LARGE_ADDRESS_AWARE；SHA=
`487622DFEE2DCDB994FA71EA590E60BD165B6183164DEDE8DACC2F8C7EE5E0F2`。
这组事实支持“接近 2 GiB 地址空间上限/碎片导致申请失败，再发生驱动空指针访问”的优先假设。
它不证明显存耗尽，不证明地图缓存泄漏，也不证明取证是唯一压力源。
未读取到 VirtualQueryEx 区域表，不能从 virtual 总量算出可用的最大连续块。

## MCP / Computer Use 边界

50301 没有 TCP listener；有限超时的 localhost SSE 请求失败。
实际插件配置 `D:/BaiduSyncdisk/工具/dbg/release/x32/plugins/x64DbgMCPServer/mcp_config.json`
仍是 `IpAddress=+ / Port=50301`，没有改写。
Computer Use 能激活并读取 x32dbg 的日志页；日志先显示 MCP 启动失败（文件被另一程序使用），
随后仍打印 Started 和 URL。这不是成功监听证据。CPU 页点击/日志导航没有可验证的效果；
刷新目标后有限重试也无变化，已停止，不改权限、不重启插件、不绕过管理员保护。
War3 OpenProcess(QUERY_LIMITED_INFORMATION 或 QUERY_INFORMATION|VM_READ) 均 error5。
整个现场保持暂停；无 F9/单步、detach、终止、重新启动、DLL 替换或独立游戏实机。

## 这一轮能明确修正的源码缺口

旧录制只核磁盘与 device-local 显存预算，未核宿主进程可用 VA/commit。
环形与有上限不代表适合 32 位地址空间：

- Win32 Event cell=400 bytes，262144 槽恰好 **100 MiB**。
- 输入 readback payload=576×256 KiB=144 MiB。
- 生产编译器测得 Draw=1040 bytes；576×128 条元数据另需73.125 MiB。
- 新预算以1088 bytes/条向上界定，输入池上界220.5 MiB，CPU+输入合计320.5 MiB。
- 上述不含驱动元数据、DXVK分配器对齐/块、其他渲染资源；图片显存仍约3.57 GiB。

防护采用单一 `war3_frame_recorder_memory` 准入模块：

1. GlobalMemoryStatusEx 读**当前进程** VA/可提交量；CPU 环首次申请额外有界 VirtualQuery
   扫描最大连续空闲区域。不读取其他进程、不改 LAA/分页文件/系统环境。
2. 预留256 MiB进程余量；CPU arm 还计算完整输入池未来 payload，不仅第一槽。
   使用减法防溢出；查询失败、连续块不足或余量不足明确拒绝，代际/active不提交。
3. CPU ring 的实际 bad_alloc 返回失败而不遗留 active session；正常 freeze/discard/re-arm 保留。
4. 图片 PendingArm 每次 createImage 前再核宿主余量；失败只释放本次尚未提交的私人图片批，
   不清历史文件、不释放此前 GPU-in-use 或冻结输入。GPU资源仍由原所有者/DXVK退役。
5. 输入槽首次增长前核预算；buffer/fence/vector 全部准备好才发布。申请/捕获异常停止此 owner 的
   后续输入捕获，不反复申请、不替换渲染结果；已采集内容仍可导出，但返回 ok=false。
   预算拒绝留 `recorder-memory/v1`：reason,totalVirtual,availableVirtual,availableCommit,serial,request。

256 MiB 是保守工程准入值，不是已验收的无崩溃阈值；检查不等于原子资源预留。
未增加逐 draw 全局锁/分配/OS查询；稳态已存在槽及图片复用路径不新增内存查询。
正常配置、1秒窗口、合法 Caster、蒙皮/阴影/Shader、原生同步优化均不改变。

**未闭合限制：** 并发申请仍可抢占余量；录制完全建立后地图继续增长的压力尚无主动缩容/回收合同。
若已有有效证据，不能为了腾空间无声销毁；需要独立设计低成本常驻诊断与显式重型取证等级，
或受所有者/完成信号约束的降级导出。当前只是拒绝危险增长，不保证所有未来 OOM 被解决。
输入故障详细信息可在冻结导出结果读取；本轮不声称新增了运行中所有故障的即时HUD覆盖。

## 离线验证与下一道门

- 184项定向 Python/static 通过：frame70、skin20、self-contained9、analyze-frame60、async16、snapshot9。
- 真实 Win32 内存采样/边界测试45检查通过；实际生产 Control 链接故障注入86检查通过，
  包括真实 new[] bad_alloc、不提交失败代际及随后两次合法恢复。
- history/input 两份生产 TU 的源一致 fsyntax-only 通过；不是产品对象重建或 DLL 链接。
- 测试初次把100 MiB写成严格大于，已纠正；生产 static_assert 又抓到 Draw 实际1040而非1024，
  预算改为1088并重新检查。两次手工预处理缺生成头路径/扩展cwd已用既有cmd普通cwd方式纠正，
  未通过Ninja补生成或改变冻结build32。
- 旧profile及实际Control/export回归的最终结果记入开发日志。

测试输出：`AutoTest/artifacts/crash_memory_pid13216_20260916_r1/`，纯CPU，不创建Vulkan设备。
2D4B/build32/桌面旧候选保留；当前源码已有新变化，build32 对新源码应视作 stale，禁止冒充新防护候选。
未生成、部署新 DLL，未改变 root CHANGELOG、发布/提交或未完成Water。

下一步：先由可访问的暂停调试器导出异常、模块、内存区域及必要dump；确认现场保存后才结束调试事务。
之后单独构建源一致候选，在明确地图/1440p/隔离桌面/零global input下对比 recorder off/on，
记录启动前后 private/VA/最大空闲块、各池已分配量、拒绝和自然退出，再决定是否扩大到长门。
不修改 War3.exe 的 LAA 位来掩盖资源成本，不把未复现、禁用阴影或拒绝全部录制当作修复接受。

## 一手资料

- [Microsoft：进程内存限制](https://learn.microsoft.com/en-us/windows/win32/memory/memory-limits-for-Windows-releases)：
  32位进程无LAA时用户VA上限2GB；增加物理RAM不直接扩大此上限。
- [MEMORYSTATUSEX](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex)：
  ullAvailVirtual是未保留/未提交VA，ullAvailPageFile是当前进程可提交量，不等于磁盘剩余空间。
- [VirtualQuery](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualquery)：
  当前进程连续属性区域元数据；不完整扫描不给连续分配授权。
- [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)：
  虚拟地址保留与提交/物理存储分离。新模块仅查询，不额外保留地址池。
