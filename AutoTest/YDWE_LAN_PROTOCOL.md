# YDWE / ydhost LAN 客户端协议与生产接线边界

本文只描述已经由分发文件和 YDWE 源码交叉确认的行为。研究过程没有启动
`YDWE.exe`、`war3.exe` 或 `ydhost.exe`。

## 已确认的命令与进程链

YDWE 的 host-test 流程不是“给多个 `war3.exe` 加 `-loadfile`”。实际顺序是：

1. `ydwe_on_test.lua` 为当前地图生成 `map.cfg`，并写入 `ydhost.cfg`：
   `lan_war3version`、`bot_defaultgamename`、`bot_autostart`、`bot_mappath`、
   `bot_mapcfgpath`。
2. 以 `plugin/ydhost` 为工作目录隐藏启动 `ydhost.exe`。
3. 启动 N 个 `YDWE.exe -war3 -closew2l -auto`（当前分发脚本的参数顺序）。
   HostTest 选项为 0 时 N=1；
   否则 N 来自 W3I 玩家槽，固定势力地图只统计 `TYPE=1` 的槽。
4. `YDWE.exe` 检测到 `-war3` 后进入 `YDWEStartup::launch_warcraft3`。它从
   `HKCU`（失败时回退 `HKLM`）的 Warcraft III `InstallPath` 读取 War3 根目录，
   创建挂起的 `war3.exe`，保留 `-auto`，移除 `-war3/-closew2l`，并在存在
   `LuaEngine.dll` 时追加 `-ydwe <YDWE根>`。
5. 启动器设置 `ydwe-process-name=war3`、注入 `bin/LuaEngine.dll`，再恢复
   `war3.exe`。LuaEngine 加载 `script/war3/main.lua`，随后加载
   `plugin/warcraft3/yd_loader.dll`。
6. `yd_loader.dll` 才是 `-auto` 的解释者。它每 300 ms 根据状态向 War3 窗口
   发送 `L`、四次 `TAB`、`J`，同时 hook UDP/Storm/`connect` 来观察搜索、地图
   广告、地图归档打开与入房请求。

因此，`-auto` 不是 Warcraft III 原生命令。直接运行
`war3.exe -auto` 不会自动获得 YDWE 的注入与入房行为，现有
`war3.exe -loadfile` 多实例启动器也不等价。

## 当前不能安全接线的原因

首要阻塞不是参数拼接，而是 War3 根目录选择。原版 YDWE 只读取当前 Windows
用户共享的注册表 `InstallPath`，没有已证明的 per-process War3 根参数。隔离 Win32
Desktop 不会隔离 HKCU；并发切换注册表会产生竞态并修改用户状态。因此，当前
AutoTest 的每实例独立 War3 根无法安全交给多个原版 `YDWE.exe` 包装器。

此外还有三项协议事实必须进入生产门禁：

- YDWE 包装器会在创建 War3 后很快退出。启动器必须以
  `CREATE_SUSPENDED → AssignJob → Resume` 启动包装器，并保留句柄；随后以父 PID、
  创建时间、镜像 SHA、Job 和 Desktop 共同锁定实际 War3 子进程，不能按进程名猜测。
- `yd_loader` 的 `connect` hook 只表示发起入房请求。源码虽枚举了 `JOIN1/GAMEING`
  状态，却没有在这条实现中推进它们；逐客户端 ready 必须来自按 PID 分流的
  DBWIN/JAPI/game-start breadcrumb。
- `-auto` 不携带 run/game id，UI 选择依赖广播房间列表和焦点顺序。同一广播域必须
  只有一个符合条件的 host，或者先实现有证据的显式主机选择协议。

## 安全的下一步

推荐实现一个独立、最小的 AutoTest YDWE 启动桥，而不是修改全局注册表：

1. 接受显式的 per-instance War3 根和哈希锁定的 per-instance YDWE 运行根。
2. 等价复现 `launch_warcraft3` 的挂起创建、环境、参数清洗、LuaEngine 注入和恢复，
   但 War3 路径只来自调用参数。
3. 在恢复前把包装器/子进程纳入会话 Job，并验证子进程的镜像、创建时间、Job、
   Desktop 与预期实例根。
4. 首次只允许一个 ydhost 广播；以真实日志确认 listening/game-created/joined，
   再以项目自己的 ready breadcrumb 确认客户端进入同一局。
5. 完成 2 客户端真实验收后再扩至 6 客户端。依赖注入 fake 永远不计入 LAN 验收。

在上述启动桥完成并取得运行时证据前，真实进程入口必须继续 fail-closed。

## 机器可读契约

同目录的 `ydwe_lan_protocol.py` 提供：

- `audit_runtime_identity`：校验 YDWE.exe、YDWEStartup、LuaEngine、yd_loader、脚本和
  ydhost 的大小及 SHA-256；
- `describe_protocol`：返回上述协议状态和源码证据；
- `build_client_command_plan` / `build_lan_command_plan`：生成不可执行的命令与身份
  计划，明确 `launchable=false`、阻塞码和所需运行时证据；
- `identity_contract`：返回可序列化的分发身份契约。

对应单元测试为 `test_ydwe_lan_protocol.py`。这些接口不导入进程创建模块，也不会启动
YDWE、War3 或 ydhost。当前契约范围是“协议关键链”，不是全部传递依赖闭包；生产启动
前还必须把 `bin`、`script/common`、`script/war3`、`plugin/warcraft3` 以及启用的
`MemHack` 虚拟 MPQ 内容纳入封闭 catalog，并在 CreateProcess 前固定不可变快照。

## 证据位置

- 分发脚本：
  `SourceMap/YDWE1.32.13 - MemoryHack/script/ydwe/ydwe_on_test.lua:105`
- 分发 War3 引导：
  `SourceMap/YDWE1.32.13 - MemoryHack/script/war3/main.lua:43`
- 源码 War3 根选择：
  `E:/Mycode/Source/Repos/YDWE/Development/Core/ydwar3/warcraft3/directory.cpp:8`
- 源码挂起创建与注入：
  `E:/Mycode/Source/Repos/YDWE/Development/Core/YDWEStartup/LaunchWarcraft3.cpp:165`
- 源码 `-auto` 解析：
  `E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/DllModule.cpp:64`
- 源码按键状态机：
  `E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/auto_enter.cpp:13`
- 源码 UDP/Storm/connect 状态：
  `E:/Mycode/Source/Repos/YDWE/Development/Plugin/Warcraft3/yd_loader/game_status.cpp:102`

外部 YDWE 源码仓库当前观察到的提交为
`a69c315ebf2886ec54d66200707ab6e38bd3484d`；分发二进制文件版本为
`1.32.12.181229`。两者只作行为关联，不能据此宣称当前源码能逐字节重建分发 DLL；
生产准入以 `ydwe_lan_protocol.py` 中锁定的实际分发 SHA 为准。
