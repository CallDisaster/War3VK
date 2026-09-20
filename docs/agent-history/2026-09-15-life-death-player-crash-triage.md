# 生与死：玩家崩溃分流与隔离入口失败

## 当前结论

还没有捕获到玩家描述的“选完难度后过一会闪退”的现场，不能判定是新的
palette来源保护、旧缓存、重型记录器、地图MemHack或显存问题。
玩家暂未观察到撕裂是有价值的视觉反馈，不等于高压稳定性已通过。

## 不混用的四组记录

| 时间/进程 | 已知阶段 | 可用证据 | 不能推导的结论 |
| --- | --- | --- | --- |
| 19:32 / 54556 | 用户指认为fresh process入图、选难度后退出 | watcher一秒历史Ready后连接终止；history目录空 | 没有退出码/崩溃栈，不知道退出原因 |
| 19:36 / 12084 | 用户确认同图 | 同类watcher连接终止 | 不能自动解释为与19:32同一exception |
| 19:38 / 6400 | 用户确认同图 | 同类watcher连接终止 | 同上 |
| 19:43:56 / 43700 | 用户为x32dbg尝试，尚未入图 | C0000005 execute @E5DCBCE8，完整dump，MemHack堆栈邻域 | 不归因为19:32的游玩崩溃 |

19:43的原dump/JSON/日志只读复制到
`AutoTest/artifacts/skin_palette_crash_43700_20260915/`。`MemHack.dll`在堆栈中
不证明其导致前面三次退出。19:20–19:46未找到对应的Windows GPU事件或普通运行fatal dump，
日志缺失也不证明不存在CPU/GPU故障。

## R1：2026-09-15 20:02，入口未通过（不是压力回归）

- 唯一独立进程48676，非交互桌面`r1_full_20260915`，未附加调试器，零global input。
- DLL：35,834,384 bytes / `81339DFC85B3C3CD507FBE249E6160160A8D0A920D3CAD01383158B85BF89EFA`。
- 原图：62,290,145 bytes / `548101C395F30853D9B117BFAF85258329EE528F26488F9C94878350218F968F`。
- 使用现有内部测试链；五项palette前提均1、申请完整记录器env。
  120秒ready超时：`jassReady=true, gameStarted=false, runtimeReady=false`。
  无按键继续、无难度点击、无相机巡航，**记录环尚未arm**。因此没有开展recording-on/off对照。
- 注册表请求2560×1440，但此轮ready前日志backbuffer为1902×963；原runner把窗口修正放在
  ready之后，未执行到。因此此轮也没有通过实际1440p门，不能作为匹配玩家条件的运行。
- 外部保留HANDLE采样245条；测试收尾前239条没有自然退出。观测private最大993,558,528 bytes，
  仅为加载/入口阶段，不可外推入图后内存。没有证据支持“已经复现OOM”。
- 受控EndGame/退出的native exitCode=0，未force，桌面关闭、视频配置恢复，GPU事件0、新dump0。
  **这个0是测试主动收尾，不是玩家崩溃的退出码**。
- 恢复测试目录`E:/Work/War3/d3d9.dll`为35,486,415 /
  `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`；
  玩家`E:/Work/Warcraft III/d3d9.dll`保持8133，原地图SHA不变。
  editor38544路径/creation pin未变且无Game/d3d9/dxgi模块，未关闭。相关游戏/编译进程0。

证据目录：`AutoTest/artifacts/skin_palette_crash_runs/r1_full_20260915/`。

- `receipt.json` SHA256 `4655E92DE7599D906FFA2FB02E68ACE6913F16E58F86709B815728CA084BABF8`
- `process-memory.jsonl` SHA256 `8FEEB28F22275A4F06AEF277843AA821CA8F5AD6E4DB452A6AA7609C6E6CE36F`
- `ready.json`、`launch.json`、`stop-final.json`和冻结`runner.py`保留该轮原义，不追认。

## 新诊断器及边界

新`run_skin_palette_crash_diagnostic.py`以启动原HANDLE的duplicate采样，不在地图
保护加载后重新OpenProcess；0.5秒private/working set、5秒VirtualQueryEx区域元数据，
不暂停游戏线程，不读取目标缓冲内容，不写目标内存，不注册系统级dump策略。
自然退出码在任何主动收尾之前单独记录；没有dump的突然退出也不杜撰调用栈。

采样定义依据微软原始文档：
[PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)、
[VirtualQueryEx](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualqueryex)、
[MEMORY_BASIC_INFORMATION](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-memory_basic_information)。
PrivateUsage是private commit，不是全部虚拟地址/显存；低4GiB扫描是明确范围，不能把free汇总
冒充任意一次分配可成功的空间。目标war3.exe只读核为LAA，但没有改变其PE标记。

R1之后仅修订诊断Python：把窗口1440p修正前置，并在ready失败时尝试有界内部截图；
receipt显式写ready/resolution/recorderArmed。该修订只有离线验证，尚未再次启动。
6项定向Python（含本进程只读native ABI/错误句柄检查）及两文件py_compile通过，
不等于完整项目回归。C++/DLL/冻结玩家包没有修改或重编译。

## 下一步阻断

现有内部接口有camera/visibility，但没有已验证的“加载确认/难度选择”入口。
旧巡航脚本有PostMessage点击难度的遗留分支，当前隔离合同禁止输入计划，不能调用。
尚不能仅凭ready=false断言屏幕一定是哪一个对话框，因为R1没有保存入口画面。
需要先取得无键鼠的可验证入图路径，或由玩家在正常前台手动入图配合保留HANDLE的监测启动器；
不能用改图跳过逻辑、未选择难度或加载页巡航冒充原问题复现。
不因入口失败重复运行无意义的recorder-off；不发布、不提交，也不把8133晋升稳定。
