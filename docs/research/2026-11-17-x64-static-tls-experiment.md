# x86 产品 DLL 整体 `-femulated-tls` 实验（T1，2026-11-17）

依据：[静态 TLS 清单与方案](2026-11-17-x64-static-tls.md) 的方案 (b) 与 §5 的 E1/E2。**只是实验**：作者还没选 TLS 路线，
仓库里的源码、交叉文件、`meson.build`、`meson_options.txt` 都没动；没有提交、没有部署，玩家目录没碰。
标记：**已核对** = 本次看过二进制、源码或实测；**推断** = 推出来的，没有实测。

## 0. 先说结论

1. **省的量和估算一致**（已核对）：静态模板 3,375,544 B → 12 B；每线程 3.22 MiB 的块从进程内 77 块（250.9 MiB）、代理 46 块（149.9 MiB）变成 0。
   与同源码的原生构建（r2）比，低 2 GiB 空闲：进程内 **+228 ～ +255 MiB**，代理 **+125 ～ +151 MiB**；峰值私有字节同量下降。
2. **帧时间看不出代价**（推断）：六组里五组中位帧率持平或更高，只有 LT 进程内 −4.9%。机器上别的会话在跑 ML，噪声远大于估算的 1–3%。
3. **但它在每次退出时崩溃**（已核对，12 局全部）：游戏退出码 `0xC0000409`，`game.stderr` 里是 compiler-rt 模拟 TLS 的
   `Windows error: 参数错误。`。原因（二进制已核对，调用者是推断）：DLL 卸载时 compiler-rt 的 `emutls_exit` 会 `TlsFree` 掉索引、
   置成 `TLS_OUT_OF_INDEXES`；之后谁再碰 `thread_local`，`TlsGetValue` 就失败，`abort()`。我们的 Storm 内存钩子从不卸载
   （`StormHook_Uninstall` 没有调用者），`d3d9.dll` 又先于 Game.dll / Storm.dll 卸载，它们收尾时调 `SMemFree` 进到钩子，钩子入口读
   `War3HotHookState()`，正好撞上。
4. 所以方案 (b) **原样不能发**。要么自带一个 `___emutls_get_address`（不释放索引，顺带保存/恢复最后错误码），要么退到 (d1)
   （只给 5 个内部链接的大表文件开，它们只在 D3D 线程渲染时访问）。§5 有对比。
5. `GetLastError` 清零：我们自己的 45 处读法没有隐患；跨边界的一类（钩子入口清零）推断风险很低（§3）。

## 1. 构建（已核对）

- 新目录 `build32_device_owner_etls_20261117_dev`，辅助脚本 `scratchpad/dev_x86_build_etls.py`（由 `dev_x86_build.py` 复制）：
  meson 选项与 `dev_x86_build.py` 完全相同，只在 setup 命令行加
  `-Dc_args=['-IE:/Dev/Vulkan SDK/Include', '-femulated-tls']`、`-Dcpp_args=[同上]`。
  - 为什么要重复那条 `-I`：命令行的 `c_args`/`cpp_args` 会**替换**交叉文件 `[built-in options]` 里的同名值
    （`build-win32-weather-llvm.txt` 里只有这一条 `-IE:/Dev/Vulkan SDK/Include`），不重复就会少一个包含路径。
  - 结果 `meson-private/cmd_line.txt`：`c_args = ['-IE:/Dev/Vulkan SDK/Include', '-femulated-tls']`，`cpp_args` 同。
- 子项目：`build.ninja` 里 571 条编译命令全部带 `-femulated-tls` 和那条 `-I`——`src` 509 条、`subprojects/dxbc-spirv` 46、
  `subprojects/imgui` 7、`subprojects/libdisplay-info` 9；没有一条缺。`war3fx` 子项目不产生目标文件。
  所以**不需要** `-D<subproject>:cpp_args`：这个 meson 版本里子项目继承了命令行的全局值。
- `d3d9.dll` 的链接输入：`d3d9.dll.p` 的对象加 10 个本构建目录里的静态库（`libwarvk_minhook`、`libdxso`、`libdxvk`、`libspirv`、
  `libutil`、`libvkcommon`、`libwsi`、`libdxbc_spv`、`libimgui`、`libdisplay-info`），其余只有系统导入库和工具链运行时（libc++、compiler-rt）。
  也就是说，定义或使用 `thread_local` 的**我们自己的**对象全部带了这个开关。
- 构建：持 `build` 锁，BelowNormal、`-j2`，退出码 0。冻结副本 `scratchpad/mem_dll/etls1/d3d9.dll`（md5 `ca2b8a4ca4534c262667eb2d14c9ea80`）。
  源码与 `x64-mem-relief-ab-20261117-r2` 用的 v3（md5 `3b9c0049…`）相同，**只差这个开关**。

## 2. 结果核对（E1）

| 项 | 原生 TLS（v3） | 模拟 TLS（etls1） | 来源 |
|---|---|---|---|
| PE TLS 模板（`pe_tls.py`） | 3,375,544 B（3.22 MiB/线程） | **12 B** | 已核对 |
| `.tls` 段 | 3.22 MiB | 0.00 MiB | 已核对 |
| `.rdata` | 2.71 MiB | 3.90 MiB（+1.19） | 已核对 |
| `.data` | 0.74 MiB | 0.75 MiB | 已核对 |
| 带 `.tls$` 段的对象（`research/tls/tls_census.py`） | `d3d9.dll.p` 里 41 个、4.17 MiB | **0**（`d3d9.dll.p` 与 10 个静态库全部） | 已核对 |
| DLL 里的 `__emutls_v.*` / `__emutls_t.*` | 无 | 267 / 33 | 已核对 |
| `___emutls_get_address`、`emutls_init`、`emutls_exit` | 无 | 已链入（compiler-rt） | 已核对 |

- 剩下的 12 B 原生模板来自工具链自带的预编译运行时（CRT / libc++），不是我们的变量。（推断：我们的对象里已经没有 `.tls$` 段）
- **`War3HotHookState` 只有一份、只经模拟 TLS**（已核对）：11 个对象各自以 COMDAT 定义 `___emutls_v._ZZN4dxvk4war35hooks16War3HotHookStateEvE5state`，
  没有任何对象引用原生名字 `_ZZN…state`；DLL 里只有一个 `___emutls_v.…state`。原生构建里则是 `.tls` 里的 `__ZZN…state`。
- 更一般的检查（已核对）：对象里出现的 264 个模拟 TLS 变量，没有一个同时以原生符号出现——不存在“一半原生、一半模拟”的分裂。
- `.rdata` 多出来的是 33 个非零初值模板（按 DLL 符号地址间距合计 1,319,176 B）：最大的是 M2-5 分类发射那组（786 KB）、
  实时调色板的两张缓存（197 KB、164 KB）、阴影渲染的 `s_paletteSlotCache`（64 KB）、时间线的 `local`（40 KB）。
  模板只在某个线程**第一次**访问该变量时被复制一份。映像净减约 2.0 MiB。
- 测试（已核对）：
  - meson 测试 `warvk-lazy-thread-array`（`ThreadLocalLazyArray`，TLS 指针加 FLS 回调）在这个构建里重编后运行：16,319 项检查 0 失败。
  - 之前跑过的静态脚本：`test_x86_memory_relief_static`、`test_shadow_cross_map_lifecycle_static`、`test_skin_palette_contract_static`、
    `test_palette_slot_cache_producer_confirmation_static`、`test_frame_timeline_static`、`test_current_draw_trusted_palette_direct_pack_static`、
    `test_current_draw_redundant_atomics_static`、`test_issue6_render_identity_cache_reset_static`、`test_proxy_mode_lifecycle_static`、
    `test_live_palette_slot_cache_accelerator_static` 全过（它们按源码文本查，开关不影响）。
  - `test_war3_palette_taxonomy_equivalence_static` 退出 1：fail-closed，找不到差分可执行文件 `build32\src\d3d9\war3_live_palette_selection_test.exe`
    （门写死了 `build32` 目录）。与开关无关，本次没去补那个目录。

## 3. `GetLastError` 被清零的审计（只读）

compiler-rt 的 `emutls_getspecific` 直接调 `TlsGetValue`，成功时会把线程的最后错误码清成 0，不保存也不恢复（上游源码与微软文档，见清单 §4b 风险 3）。
所以危险的形状是：**某个 Win32 调用失败 → 中间有代码碰了 `thread_local` → 再读 `GetLastError()`**。

### 3.1 我们自己读 `GetLastError()` 的地方（45 处、21 个文件，已核对）

全部是下面三种之一，**没有发现真正的隐患**：

- 失败调用之后**紧接着**读（中间没有别的调用）：`util/util_env.cpp:146`、`war3/core/war3_crash_handler.cpp:68,76,328,351`、
  `war3/japi/war3_module_loader.cpp:583`、`war3/platform/war3_proxy_mode.cpp:79`、`war3/platform/war3_player_settings.cpp:784,827`、
  `war3/tools/war3_frame_history.cpp:37`、`war3_frame_evidence.cpp:143`、`war3_async_screenshot.cpp:191`、`war3_async_screenshot_io.h:82,92`、
  `war3_control_plane.cpp:5095,5150,5163`、`war3_perf_monitor.cpp:12575`、`war3/memory/war3_storm_hook.cpp:716,1000`、
  `war3_storm_native_small_repair.cpp:46`、`war3_tlsf_pool.cpp:182,276,435`、`minhook/src/hook.c:312`、`wsi/win32/wsi_monitor_win32.cpp:243,252,264,274`。
  `d3d9/d3d9_mem.cpp:285,305` 中间只有计数器自增和 `AdvanceDiagnosticMutationLocked()`（`:148-158`，纯成员运算，不碰 TLS）。
  `Logger::err(str::format(…, GetLastError()))` 这种写法里，`GetLastError()` 在进入 `Logger` 之前就求值了，其余实参是字面量或局部变量。
- 先保存、做事、再恢复：`war3_storm_low_domain.cpp:70,78`、`war3_frame_recorder_memory.cpp:6-7`、`diagnostics/war3_startup_witness.cpp:18-20`、
  `war3_mdl_trace_hooks.h:88-225`（开发开关，产品不编）。在模拟 TLS 下这种写法照样正确。
- 只用于诊断打印、而且本来就可能读到陈旧值的：`war3_control_plane.cpp:5154` 在 `EndStartupStage`/`NoteStartupStage` 之后第二次读
  （那两个函数在产品里是空的内联函数，`war3_startup_witness.h:76-78`；开发版用 `PreserveLastError` 保护）。

### 3.2 钩子在调用方读错误码之前清零（跨边界，推断）

我们的钩子在**入口**就碰 `thread_local`，于是在原函数运行之前把调用方的最后错误码清成 0：

- Storm 内存钩子 `war3/memory/war3_storm_hook.cpp:748`（`SMemAlloc`）、`:777`（`SMemFree`）、`:797`（`SMemGetSize`）、`:817`（`SMemReAlloc`）：
  `War3HotHookCallTiming` 的构造函数无条件读 `War3HotHookState()`（`war3/hooks/war3_hook_perf.h:780`）。
  原函数返回之后不再碰 TLS（析构只用缓存的指针，`:716-720`、`:809-815`、`:874`），所以**原函数自己设的错误码会保留**。
- Storm IAT 里的 `VirtualAlloc` 钩子 `war3/memory/war3_storm_low_domain.cpp:66` 先读 `thread_local g_selected`（`:24`），
  `:70` 才保存 `before`，所以成功时 `:78` 恢复的是 0 而不是调用前的值。
- 其余 MinHook 钩子（渲染、模型、光照、JASS）入口处的计时也一样。

只有“调用方先失败、再调一个被钩的函数、再读 `GetLastError()`”才会看到差别。查到的事实（已核对，只读二进制）：

- Storm 自己的错误码**不走** Win32：`SErrGetLastError`（序号 463，`0x1501d410`）是 `mov eax,[0x1504BFDC]; ret`，读的是 Storm 的全局变量；
  `SErrSetLastError`（465）写这个全局变量后再转调 `SetLastError`。所以 Storm 内部“设错 → `SMemFree` → 读 `SErrGetLastError`”不受影响。
- `Game.dll` 直接调 `GetLastError` 的 15 处，往前 96 字节内的调用都是 Win32 API（`FindFirstFileW`、`FindNextFileW`、`CreateWindowExA`、
  `ReadFile`、`WriteFile`、`SetFilePointer`、`WaitForMultipleObjects` 等）或游戏自己的函数，没有一处紧跟在 Storm `SMem*` 之后。
  （启发式扫描，不是完整反汇编；前三处前面是游戏内部函数 `sub_6f0208d0`、`sub_6f01dbf0`、`sub_6f01db40`，没往里追。）

结论：**没有找到真实隐患**；残余风险是跨边界那一类，推断很低。真要上这条路，最稳的做法是自带一个保存并恢复最后错误码的
`___emutls_get_address` 包装（例如 lld 的 `--wrap`），把这类风险整体关掉，代价是每次访问多两次极便宜的 TEB 读写（推断，没试）。

## 4. A/B 门（E2）

命令与 r2 完全相同，只换 `--product-dll scratchpad/mem_dll/etls1/d3d9.dll`，输出 **`AutoTest/artifacts/x64-etls-ab-20261117-r1`**，
持 `game` 锁一次跑完（16:59–17:19，12 局）。对照是 `x64-mem-relief-ab-20261117-r2`（同源码，原生 TLS）。

### 4.1 门本身的结果（已核对）

- 12 局的探针检查只有一条不过：`gameExitedByItself`（游戏退出码 `0xC0000409`）。所以门的 `allRunsPassed` 与三张图的 `measured` 都为假，
  门的汇总里每臂 `runs=0`。关窗之前的测量（内存、帧、截图）都完整，下表按“只有这一条不过”的运行取中位数（`scratchpad/etls_compare.py`）。
- 没有挂起：12 局都没有超时、没有被强杀，进程正常结算（`timedOut=false`、`forcedTermination=false`、`processSettled=true`）。
- 截图两种模式的平均差与 r2 同一量级（LT 1.5–1.8、EI 1.7–1.8、生与死 6.6–7.5）。
- 门里 `staticTlsCopies` 一栏这次没有意义：模板只有 12 B，门按“模板加块头取整到页”= 4 KiB 去数，数进来的是无关的 4 KiB 区域
  （清单 §5 已预告）。改按块大小 0x339000 直接数：原生 LT 进程内 77 块 250.9 MiB、代理 46 块 149.9 MiB；模拟 TLS 两边都是 0。

### 4.2 内存（两轮中位数，MiB；空闲 / 最大洞是 `vaSample`，低 2 GiB，关窗时）

| 图 | 模式 | 空闲 原生 → 模拟 | 最大洞 | 峰值私有字节 | 低半区私有提交 |
|---|---|---|---|---|---|
| LT | 进程内 | 789.8 → 1044.3（**+254.5**） | 305.8 → 575.3（+269.5） | 604.6 → 347.0（−257.6） | 565.7 → 319.9 |
| LT | 代理 | 1409.2 → 1560.2（**+151.0**） | 988.7 → 1000.4（+11.7） | 302.2 → 146.6（−155.6） | 286.7 → 141.1 |
| EI | 进程内 | 801.8 → 1051.3（**+249.5**） | 303.0 → 579.7（+276.7） | 602.6 → 345.7（−256.9） | 561.1 → 319.3 |
| EI | 代理 | 1420.4 → 1568.2（**+147.8**） | 1000.4 → 1000.4（0） | 293.9 → 139.7（−154.2） | 278.4 → 134.2 |
| 生与死 | 进程内 | 269.3 → 497.6（**+228.3**） | 100.2 → 130.2（+30.0） | 1072.2 → 828.3（−243.9） | 1013.8 → 791.0 |
| 生与死 | 代理 | 929.8 → 1055.2（**+125.4**） | 491.0 → 639.1（+148.1） | 711.5 → 568.2（−143.3） | 678.7 → 557.8 |

- 省下的与线程数对得上：进程内约 77 份、代理 45 份（LT/EI）/ 38 份（生与死）× 3.22 MiB，减去真正用到的线程按需分配的那点。
- 代理 LT/EI 的最大洞停在 1000.4 MiB：再往上被别的固定分配挡住了，不是 TLS 的事（推断）。

### 4.3 帧时间（各臂中位帧率的中位数，fps）

| 图 | 进程内 原生 → 模拟 | 代理 原生 → 模拟 |
|---|---|---|
| LT | 186.5 → 177.3（−4.9%） | 28.9 → 35.1 |
| EI | 114.5 → 141.0 | 25.8 → 28.3 |
| 生与死 | 40.4 → 43.1 | 8.17 → 8.34 |

六组里只有一组变慢，且幅度在这台机器同一构建两轮之间的波动之内（r2 自己 EI 进程内两轮是 84.7 / 144.3）。估算的 1–3% 在这里量不出来（推断）。

### 4.4 泄漏与退出

- **线程退出不释放**：进游戏 20 s 之后到关窗前 8 s 的私有字节，模拟 TLS 与原生一样平或下降（进程内 −16 ～ −32 MiB / 30 s，代理 ±0.2）。
  这段时间内没看到上涨。重开地图多次的生命周期测试（E3）**没有跑**，所以换图循环里会不会慢慢涨还不知道。
- **退出时崩溃**（12/12，两种模式）：
  - 现象（已核对）：退出码 3221226505 = `0xC0000409`；`game.stderr` 末尾是 `Windows error: 参数错误。`（GBK 解码，即 `ERROR_INVALID_PARAMETER`）。
    这个格式只在模拟 TLS 构建的 DLL 里有（`Windows error` 字符串：etls1 里 2 处，原生 v3 里 0 处）。
  - 机制（反汇编已核对）：`_emutls_exit`（`0x10a67050`，由 DLL 的 `atexit` 在卸载时调用）依次 `DeleteCriticalSection`、`_aligned_free`、
    `TlsGetValue`、`TlsFree`，再把 `emutls_tls_index`（`0x11011a64`）写成 `0xFFFFFFFF`。之后任何 `___emutls_get_address`
    都会 `TlsGetValue(0xFFFFFFFF)` 失败，compiler-rt 打印错误后 `abort()`，UCRT 的 `abort` 走 `__fastfail`，所以是 `0xC0000409`。
  - 谁在那之后碰了 TLS（推断）：最可能是 Storm 内存钩子。`StormHook_Uninstall`（`war3_storm_hook.cpp:1166`）没有任何调用者，
    钩子一直挂到进程结束；`d3d9.dll` 由 Game.dll 运行时加载，所以按初始化逆序先于 Game.dll、Storm.dll 收到 `DLL_PROCESS_DETACH`；
    它们收尾时调 `SMemFree`/`SMemAlloc`，钩子入口 `War3HotHookCallTiming` 读 `War3HotHookState()`（`war3_hook_perf.h:780`）。
    另一个候选是我们自己在第一次 TLS 访问之前构造、之后才析构的静态对象（`atexit` 后进先出）——没有调试器，没能区分。
  - 发布版 1.22.x（GCC 模拟 TLS）没有这个问题：libgcc 的模拟 TLS 不在卸载时删除线程键（推断，本次没核对 libgcc 源码）。

## 5. 下一步的选项（都没做，作者定）

| 做法 | 能拿回 | 退出崩溃 | `GetLastError` | 代价 |
|---|---|---|---|---|
| (b′) 整个 DLL 模拟 TLS + 自带 `___emutls_get_address`（索引永不释放；按线程数组用 `HeapAlloc`；保存/恢复最后错误码），让链接器不再拉 compiler-rt 的 `emutls.o` | 本次实测的全部（进程内 +228～+255、代理 +125～+151 MiB） | 解决（推断：索引一直有效） | 解决 | 一个新源文件约百行，要处理模板复制与对齐；要单测；锁住“不准再有原生 TLS”的静态门 |
| (d2) Storm 钩子在 `PERF_LEVEL<2` 时不碰 `War3HotHookState` | 单独做 0；配合 (b) 时每个 Storm 线程省 0.1 MiB | **不够**：`war3_storm_low_domain.cpp:24,66` 的 `g_selected` 还是在 Storm 的 `VirtualAlloc` 钩子里读 | — | 小 |
| (d1) 只给 5 个内部链接的大表文件开 | 约 2.3 MiB/线程，约 103 / 175 MiB（清单估算） | 推断安全：这些表只在 D3D 线程渲染时访问，不在钩子里 | 只影响这几张表 | 拆一个静态库；加静态检查防 COMDAT TLS 混入 |
| 退回原生 TLS、(c′) 惰性数组 | 约 53 / 90 MiB | 无此问题 | 无此问题 | 改动面中等 |

建议（推断）：如果作者要 (b) 的全部收益，先做 (b′) 再重跑这个门加 E3（重开三次地图 + 正常退出），以“12 局全部 `gameExitedByItself`”为准；
否则 (d1) 是改动最小、退出风险最低的那条。

## 6. 产物

- 构建目录 `build32_device_owner_etls_20261117_dev`（保留）；辅助脚本 `scratchpad/dev_x86_build_etls.py`；
  冻结 DLL `scratchpad/mem_dll/etls1/d3d9.dll`。
- 门：`AutoTest/artifacts/x64-etls-ab-20261117-r1`（12 局，均在退出时崩溃，测量完整）。对照 `x64-mem-relief-ab-20261117-r2`。
- 核对脚本与中间结果：`scratchpad/etls/`（`census_*.tsv`、`dll_nm.txt`、`objs_nm.txt`）、`scratchpad/etls_compare.py`。
- 仓库里没有改任何文件；新增的只有本文。
