# x64 宿主退出延迟：取证、根因与开关修复

日期：2026-11-17。接续 `2026-11-17-x64-texture-upload-and-exit-repair-design.md` §5/§6-P0-2/§6-P3。

范围：`tools/render_host/dxvk_core/d3d9_host_main.cpp` 的退出路径，以及 `tools/render_host/owned_child.cpp` 退出等待的阶段标签。

两个修复都在开关之后，默认关闭（行为与现状相同）。没有延长 5 s 期限，没有使用 `_Exit`/`TerminateProcess`，也没有改变 Finish、账本或 session.reset 的顺序。原有红色回执保留，不因 secondary drain 的 exit 0 升级为通过。

## 1. 结论

Finish 回复之后，宿主退出由两段组成。

1. **退出账本的日志 I/O，约 340–370 ms（旧 Echo 一轮为 1,485 ms）。**
   - shadow-host 一行约 144 KB，由约 800 次 `fprintf` 写到无缓冲的 stderr（ucrt 每次调用各自 WriteFile，文本模式）。
   - 慢窗采样中主线程处于 running，栈为 `NtWriteFile ← WriteFile ← ucrt fflush/fwrite ← vfprintf`。两个约 140 ms 的采样各消耗 CPU +109 ms 和 +125 ms。
   - 这一段是自身开销，不是调度饥饿。

2. **opengl32.dll 的 DLL_PROCESS_DETACH 轮询，约 62 ms 或约 5.0–6.3 s。**
   - ExitProcess 先终止其它线程，再由 LdrShutdownProcess 调用各 DLL 的 detach。
   - opengl32 的 detach 轮询它自己的 “Palette Watcher” 线程 ID，每次 `Sleep(50)`，最多 100 次，然后强杀。下文称 watcher 线程。
   - 这个线程已被 ExitProcess 杀死，不会再自己清零 ID。之后的走向取决于 opengl32 停止路径里的 `PostMessage(WM_CLOSE)`：
     - PostMessage 失败（窗口已被内核清理）时，等待后会清 ID，只睡一次（约 62 ms）。Lost Temple 和生与死都是这条路。
     - PostMessage 仍然成功时，ID 不清，满 100 次（Echo：6,201 ms）。这超过了前端的 5 s 退出窗口。

CRT 退出、静态析构、我们自己的 TLS 回调和内核收尾合计约 20–30 ms，不是问题所在。

**watcher 线程的来源**（反汇编本机 `C:\Windows\System32\opengl32.dll`，只读、本地工具 llvm-objdump，没有下载符号）：
- `wglSetPixelFormat` 在**窗口属于其它进程**时无法子类化该窗口，就启动这个线程（隐藏窗口 + GetMessage 循环，引用计数）。判定是 `GetWindowThreadProcessId(hwnd)` 与 `GetCurrentProcessId()` 比较。
- 我们的宿主向游戏进程的窗口呈现。

**推断，未直接观测**：在宿主里对游戏窗口调用 SetPixelFormat 的是 NVIDIA Vulkan ICD（nvoglv64.dll 静态导入 GDI32 `SetPixelFormat`）。本仓源码没有调用 SetPixelFormat；已加载模块中 OBS graphics-hook64.dll 只导入 GDI32 `SwapBuffers`。

opengl32 自己的调试串印证了这段逻辑：
- “Palette watcher thread id %d possible deadlock with DLL unload, killing the palette watcher”
- “PostMessage to hwnd: %08x failed with error: %08x”

## 2. 取证方法

`scratchpad/exit_watch.py`，只用系统自带 DLL（ntdll/kernel32/psapi/dbghelp），符号路径是空的本地目录，不访问符号服务器。

- 盯住 host.log* 里的 `finish-reply-written`，用 PID 打开**这个**进程，记录创建时间作为身份。
- 在进程对象 signalled 之前，约每 100 ms 采样一次：
  - `NtQuerySystemInformation(SystemProcessInformation)` 给出每个线程的调度状态、等待原因、优先级和 CPU 时间；
  - `GetSystemTimes` 给出系统负载；
  - SuspendThread + StackWalk64 抓用户栈，随即 Resume，用模块+偏移和最近导出名标注。
- 不终止任何进程。

宿主新增的退出里程碑：
- `exit-accounts-begin` / `exit-profile-written`：夹住退出快照。
- `wmain-locals-write-returned`：证明最后一个 witness 的 WriteFile 已返回。
- `crt-exit-begin`（atexit）。
- `static-destructors-last`（init_priority(101) 对象）。
- `loader-process-detach`：exe 自己的 TLS 回调，在其它 DLL 的 detach 之后执行。

这些里程碑都只用 kernel32 和栈缓冲区，不分配堆、不取锁，可以在 loader lock 下使用。

`OwnedChild::wait` 的退出等待现在报 `stage=exit-wait`，不再误报 `stage=setup`（win32_transport.h `Stage::ExitWait`）。

## 3. 时间线（QPC，自 exit-accounts-begin 起，ms）

| 运行 | 账本写出 | CRT+静态析构 | 其它 DLL detach（opengl32） | 内核收尾 | 前端 primary |
|---|---|---|---|---|---|
| Echo echo-r1（PID 42276） | 368.1 | 0.1 | **6,200.8** | 20.7 | 5,000 ms 超时；secondary 1,593 ms 后 exit 0 |
| LT wake-r1（PID 7288） | 339.4 | 0.2 | 66.5 | 29.0 | 438 ms 正常 |
| LT lt-ab-r1（PID 25500） | 361.6 | 0.2 | 72.2 | 31.8 | 469 ms 正常 |
| 生与死 sys-ab-r1（PID 30640） | 338.2 | 0.2 | 62.4 | 28.2 | 422 ms 正常 |
| LT wakefix-ab-r2（PID 5904，未挂观察器） | 474.2 | 0.2 | **6,288.6** | — | 5,015 ms 超时；secondary 1,781 ms 后 exit 0 |
| Echo echo-off-r3（PID 5688） | 479.8 | 0.2 | 64.3 | 38.8 | 578 ms 正常 |

开关关闭的 6 轮中，有 2 轮落进 100 次循环：Echo 一轮，LT 一轮（运行前 CPU 约 4%）。所以这不是 Echo 独有的问题，也不能简单归因于机器负载。哪一轮会撞上，取决于 ExitProcess 中被杀线程的窗口何时被内核清理，本轮没有再往下追。

宿主均以 BELOW_NORMAL 优先级（16384）运行，四轮在 Finish 时都已加载 opengl32，并都有一个 opengl32 消息线程。

Echo 慢窗共 48 个采样：
- 只剩 1 个线程（其它线程已被 ExitProcess 终止）。
- 状态 waiting / DelayExecution，栈为 `SleepEx ← opengl32+0xcad8 ← +0xc808 ← +0xc265 ← +0x5bf29 ← ntdll(LdrShutdownProcess) ← RtlExitUserProcess ← ExitProcess ← ucrt exit ← host.exe`。
- 6.16 s 内只有 219 次上下文切换、15.6 ms CPU。这是有意的睡眠，不是饥饿。
- 其中 47 个采样落在 100 次循环体内（+0xcad8）。LT 和生与死只出现一次落在尾跳 `Sleep(50)`（+0xc808）。
- 100 × Sleep(50) 在 15.6 ms 定时器粒度下约等于 100 × 62.5 ms = 6.25 s，与实测吻合。

**旧 Echo 回执**（`x64-p0-sync-profile-20261117-echoisles-r1`，PID 5348）：
- Finish→before-session-reset 1,485 ms，是同一个 144 KB 账本行；那一轮的机器负载更重。
- 之后 primary 在 5,047 ms 超时，secondary 4,422 ms 后 exit 0。
- 与上述机制一致，但当时没有采栈，因此只算旁证。

## 4. 修复（开关，默认关）

### 4.1 `WARVK_D3D9_HOST_EXIT_BUFFERED=1`

- 退出账本块（draw-batch-host 到 admission-host，含 shadow-host）先在内存里格式化，块末一次 `fwrite` 加 `fflush`。字节和顺序不变。
- 如果分配失败，剩余部分退回直接写 stderr。
- 发生在 wmain 内、session.reset 之前，不涉及 loader lock。

### 4.2 `WARVK_D3D9_HOST_EXIT_GL_WATCHER=close`

在正常 teardown 和全部账本之后、wmain 返回之前执行，此时进程内所有线程都还活着：
- 用 EnumWindows 找本进程的 “Palette Watcher” 窗口；
- 照 opengl32 自己停止路径的做法，`PostMessage(WM_CLOSE)`，再有界等待该线程退出（≤500 ms）；
- 线程退出时会自己清零 ID，loader 的 detach 于是只睡一次。

没有找到窗口或线程没有退出时，与现状完全相同。不跳过任何关闭步骤，不终止任何东西。

日志行：`D3D9 host exit gl-watcher windows=… thread=… posted=… waited=… elapsedMs=…`。

风险：依赖 opengl32 的未公开类名和行为。默认值须经审核。

长期替代方案：由宿主进程自己拥有呈现窗口（例如宿主创建的子窗口），这样 ICD 设置像素格式时 opengl32 会走子类化路径，不再启动 watcher。这会牵涉输入、焦点、DPI 和尺寸，本轮不做。

## 5. 验证

宿主：`build64_render_host_p1_20261117_dev`，`a9342ae65becca82`。两个开关同时打开（`WARVK_D3D9_HOST_EXIT_BUFFERED=1`、`WARVK_D3D9_HOST_EXIT_GL_WATCHER=close`）。

| 运行 | 账本写出 | session.reset | 关闭 watcher | 其它 DLL detach | 内核收尾 | 前端 primary |
|---|---|---|---|---|---|---|
| LT `x64-p1-staging-slices-20261117-lt-ab-r1` | 4.5 | 0 | windows=1 posted=1 waited=0，0 ms | 67.4 | 30.7 | 93 ms 正常 |
| Echo `…-echo-ab-r1` | 6.9（含 reset） | — | windows=1 posted=1 waited=0，0 ms | 59.4 | 63.8 | 109 ms 正常 |
| 生与死 `…-sys-ab-r1` | ≈0 | ≈141（tick） | windows=1 posted=1 waited=0，0 ms | 67.0 | 38.8 | 250 ms 正常 |
| Echo `x64-host-exit-delay-20261117-echo-on-r2` | 5.7（含 reset） | 0 | windows=1 posted=1 waited=0，0 ms | 62.4 | 101.8 | 110 ms 正常 |

（单位 ms，QPC。生与死的 session.reset 用 GetTickCount 测得，粒度约 16 ms。）

开关开启的 4 轮：
- 账本写出从 340–480 ms 降到 4.5–6.9 ms；
- watcher 每次都被找到，并在 0 ms 内退出；
- opengl32 detach 都走单次 `Sleep(50)`（59–67 ms）；
- primary 退出 93–250 ms，四轮全部通过，回执 `sessionsEndedClean=true`。

只看次数，关闭时 6 轮中 2 轮撞上，开启 4 轮 0 次撞上，这本身还不足以下统计结论。决定性的是机制：
- detach 的 100 次循环只在 watcher ID 非零时进入；
- watcher 线程自己退出时会清零 ID，开启时 `waited=0` 证明线程已退出；
- 因此在 Finish 之后不再有新的 SetPixelFormat 的前提下，开启后按构造只会走单次睡眠。

生与死另有一段约 141 ms 的 session.reset（释放会话内容等）。它不受这两个开关影响，仍远在 5 s 窗口之内。

## 6. 未做与限制

- 运行次数少。哪一轮会落进 100 次循环，取决于 ExitProcess 中被杀线程的窗口何时被内核清理。需要多次重复确认开关后不再出现。
- “调用 SetPixelFormat 的是 NVIDIA ICD”是推断。确认需要在 SetPixelFormat 时刻抓栈，本轮没有做。
- 账本缓冲只消除 I/O 次数，不缩短内容。是否压缩 runLog/deviceMemoryLog 属于回执格式问题，不在本项。
