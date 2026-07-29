# AutoTest：YDWE per-instance helper 接线状态

`ydwe_instance_launcher.py` 是新的 fail-closed 请求层。它替代“修改全局 Warcraft InstallPath
再启动 YDWE.exe”的不安全设想，直接把每个实例的 `war3.exe`、独立 YDWE 根、Desktop 和
named Job 绑定到一条机器可审计命令。

当前 API 为 `prepare_ydwe_instance_launch(...)`：

- 默认 `execute=False`，返回 `DRY_RUN_ONLY` 和 `launchable=false`。
- `execute=True` 也不会真的调用 helper；完整预检后仅返回审计 argv、
  `EXECUTION_BRIDGE_NOT_PRODUCTION_READY` 和 `launchable=false`。
- helper SHA、x86 PE、专用 sandbox、`_AutoTestInstances` 实例根、实例内 YDWE、八项
  runtime SHA、空的 KILL_ON_CLOSE Job 与 Desktop 只是审计前置条件，不构成执行授权。
- helper 固定加入一个 `-auto`，窗口模式由 `--windowed` 控制；调用方传入
  `auto/loadfile/opengl/window/ydwe/war3/closew2l` 会在 Python 与 C++ 两层拒绝。

本轮没有真实启动。`ydwe_job_owner.py` 已完成 fake-backend capability 第一阶段，但尚未接入
SessionRegistry。当前明确阻断真实执行的缺口为：实际 owner HANDLE 必须贯穿 helper 与
session；`lua53.dll`/`ydbase.dll` 需要绝对路径预载和远程模块核验；晚加载 runtime 的锁必须
保持到逐 PID ready watchdog 完成。即使实验 helper 返回非空 LuaEngine `HMODULE`，也仍需
等待 `yd_loader`、joined 与 game-ready 证据，不能计入 LAN 验收。

`ydwe_runtime_catalog.py` 已能只读列出核心文件与 YDWE 四棵运行时目录并计算稳定 SHA，
但明确不持 deny-write HANDLE，也不保证审计后的不可变性；它只是未来长驻闭包守卫的输入，
不能缩短上述阻断条件。

默认构建的 C++ helper 同样由 `YDWE_INSTANCE_LAUNCH_EXPERIMENTAL=OFF` 编译门封闭；有效参数
只会返回 `COMPILE_GATE_DISABLED`，不会进入 `CreateProcessW`。该选项只能用于后续隔离实验，
不能用于生产或本计划的联机验收。

构建、命令行和注入细节见
[`Tools/YdweInstanceLauncher/README.md`](../../../../../Tools/YdweInstanceLauncher/README.md)。
