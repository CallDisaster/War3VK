# x86 产品 DLL 整体模拟 TLS + 自带运行时（(b′)，2026-11-17）

依据：[T1 实验](2026-11-17-x64-static-tls-experiment.md) §5 的 (b′)，作者选了方案 (b) 加这个修复。
没有提交、没有部署；只在沙箱里跑游戏，玩家目录 `E:/Work/Warcraft III` 没碰（里面的 `d3d9.dll` 仍不存在）。
标记：**已核对** = 本次看过二进制、源码或实测；**推断** = 推出来的，没有实测。

## 0. 先说结论

1. **退出崩溃没有了**（已核对）：自带的 `___emutls_get_address` 胜过 compiler-rt 的（DLL 里 compiler-rt 的模拟 TLS 符号 0 个、`Windows error` 字符串 0 处），
   进程终止时什么都不释放。最终 DLL（etls2d）的 A/B 门 **12 局全部自己退出、退出码 0**，`allRunsPassed` 为真；T1 的 etls1 是 12/12 `0xC0000409`。
2. **内存与 T1 相同**（已核对）：比原生 TLS，低 2 GiB 空闲 进程内 **+222 ～ +254 MiB**、代理 **+125 ～ +152 MiB**，峰值私有字节同量下降；静态 TLS 模板 12 B。
3. **线程退出释放、换图不涨**（已核对）：换图探针每种模式 7 次载图（地图自己 A→B→A，再用菜单重开 4 次），私有字节谷底不升，两局都正常退出（码 0）。
   新加的计数器显示进程内每秒约 85 个线程退出，全部被释放，持有存储的线程数和字节数从 40 s 起不变；`lateAccesses` 0。
4. **最后错误码**：热路径直接读 TEB 的 TLS 槽、不调任何函数，天然不动它；首次访问的慢路径保存/恢复。每次访问比原生多约 2 ns，比 compiler-rt 少（§4.3）。
5. **帧率**（推断）：交替跑里进程内扣掉时段漂移后约 −0.5%；同一个原生 DLL 前后两次就差 18%（进程内）/ 35%（代理）。r1/r2 与 T1 的帧率差是时段负载。
   平衡的 3 对 A/B 因 E: 盘满没跑成，列为待办。
6. **交叉文件已改**（lead 定的方案 A）：`build-win32-weather-llvm.txt` 的 `c_args`/`cpp_args` 加了 `-femulated-tls`。已有构建目录不会自动带上（Meson 只重新生成、不换参数），
   新建的目录才是模拟 TLS（§7）。
7. 新文件属数据层，切口门正向仍是 19 个符号 / 24 条边。没有提交、没有部署。

## 1. 改了什么

| 文件 | 改动 |
|---|---|
| `src/d3d9/war3/memory/war3_emutls.cpp`（新） | 自带的 `__emutls_get_address`（i686 符号 `___emutls_get_address`）、线程退出回调、卸载释放，383 行（含注释），只用 Win32 |
| `src/d3d9/war3/memory/war3_emutls.h`（新） | `EmutlsQueryStats`（计数器）、`EmutlsReleaseForUnload`（测试直接调） |
| `src/d3d9/meson.build` | `d3d9.dll` 源列表加一行 `'war3/memory/war3_emutls.cpp',` |
| `src/d3d9/war3/tools/war3_control_plane.cpp` | `get_runtime_status` 回复里加 `emutls` 计数器（只在模拟 TLS 构建、索引已分配时出现），一个 include + 9 行 |
| `AutoTest/test_war3_emutls.cpp`（新） | CPU 单测：payload DLL + 测试 exe，见 §3 |
| `AutoTest/run_war3_emutls_test.py`（新） | 用交叉文件里的 clang 编四个产物并跑单测、退出场景、符号检查，打印一行 JSON |
| `AutoTest/test_war3_emutls_static.py`（新） | 文本检查：源列表与分类、C 符号、回调段名、析构优先级、热路径不调 API / 慢路径保存错误码、终止期间不进锁、计数器上报、运行时里没有 `thread_local`、交叉文件开关 |
| `build-win32-weather-llvm.txt` | （§7，a–c 通过且帧率 A/B 在噪声内才改）`c_args`/`cpp_args` 加 `'-femulated-tls'` |

GCC 交叉文件（`build-win32.txt`、`build-win64.txt`）没动；x64 宿主的 `build-win64-render-host-llvm.txt` 也没动。

## 2. 运行时的规则（已核对：源码；行为见 §3 单测）

clang 在 `-femulated-tls` 下把每次 `thread_local` 访问编成 `call ___emutls_get_address`（cdecl，参数是控制块指针）。
控制块是四个 32 位字 `{size, align, index, value}`（compiler-rt / libgcc 的 `__emutls_control`，i686 上 `gcc_word` 是 32 位）。
我们在 `d3d9.dll` 自己的对象里定义这个符号，链接器就不会再从 `libclang_rt.builtins-i386.a` 拉 `emutls.c.obj`（§4.1 有证据）。

| 要求 | 做法 |
|---|---|
| 1 进程终止时不释放 | 映像里第一个构造的静态对象（`init_priority(101)` 的 `ImageTeardown`）的析构是卸载时最后一个 `atexit` 项。它先问 `ntdll!RtlDllShutdownInProgress()`：进程正在终止就**什么都不放**，TLS 索引一直有效，之后 Game.dll / Storm.dll 收尾时进我们的钩子照样能访问。查不到这个函数时按“正在终止”处理（宁可不放） |
| 1′ 动态 `FreeLibrary` | 同一个析构里：把索引换成 `TLS_OUT_OF_INDEXES`，摘下线程链表，逐块释放，`TlsFree`（系统同时清掉所有线程的槽）。之后本映像不再有代码运行。若有代码在它之后又访问 `thread_local`（只可能是卸载过程里、析构之后），会重新 `TlsAlloc` 一个索引并分配块——**每次加载最多多占一个 TLS 索引和那几块**，有界；不会崩。游戏本身不卸载这个 DLL（Storm 钩子从不卸，卸了也会崩；推断） |
| 2 线程退出释放 | `.CRT$XLY` 里的 TLS 回调，只处理 `DLL_THREAD_DETACH`：在 mingw 的 `thread_local` 析构（`.CRT$XLB`）和 `.CRT$XLC/XLD` 之后、`.CRT$XLZ` 之前运行，所以析构函数还能读到自己的对象。只做堆释放；链表用一个自旋锁保护两次指针写，持锁期间不调用任何可能等待的函数，所以不会和加载器锁互等。释放后把槽设成哨兵 `1`，若同一线程在更晚的 DLL 的线程退出里又访问，就重新分配（计数 `lateAccesses`，这块只在卸载释放时回收） |
| 1″ 终止期间不进锁 | 进程终止期间（`RtlDllShutdownInProgress()` 为真）新建的线程块不登记、不拿自旋锁：被 `ExitProcess` 停住的线程可能正持着它。终止期间本来就不释放，所以不需要登记（etls2b 起） |
| 2′ `DisableThreadLibraryCalls` | DXVK 的入口会调它，但 mingw 的映像总有 TLS 目录，这个调用对有静态 TLS 的模块失败，线程通知照样到（单测里实测：调了之后线程退出回调照样跑） |
| 3 保存最后错误码 | **热路径不调用任何函数**：变量号、TLS 索引都已分配、本线程已有该对象时，直接从 TEB 读本线程的 TLS 槽（`<winternl.h>` 公开的 `TEB::TlsSlots[64]` / `TlsExpansionSlots`，偏移用 `static_assert` 钉住：x86 `0xE10`/`0xF94`），最后错误码天然不动——`TlsGetValue` 恰恰会把它清零。其余情况（首次访问、线程已释放）进 `SlowAddress`：第一句 `GetLastError()`，返回前 `SetLastError`。线程退出回调、卸载释放也保存/恢复。（etls2d 起；etls2a–c 是每次访问都保存/恢复，见 §4.3 的代价） |
| 4 惰性分配、对齐、模板 | 变量编号：控制块 `index` 为 0 时从全局计数器取号，CAS 写回（输掉的一方浪费一个号，槽空着而已）。TLS 索引：首次 `TlsAlloc`，CAS 发布，输的一方 `TlsFree`。对象：`align = max(align, 4)`，必须是 2 的幂；`HeapAlloc(size + align − 1 + 8)` 后对齐，前面两个字存大小和原始指针；有模板就复制，否则清零。每线程的槽数组从 64 起、按需翻倍 |
| 4′ 分配失败 | `OutputDebugStringA` + `WriteFile(stderr)`（不用 CRT 的 stdio、不拿锁），然后 `abort()`——与 compiler-rt 同样的处理 |
| 5 自成一体、属数据层 | 只依赖 `<windows.h>`、`<winternl.h>`、`<cstddef>`、`<cstring>`、`<cstdlib>`；文件名 `war3_memory_war3_emutls.cpp` 由切口门的 `classify` 归 `data`；运行时里没有 `thread_local`（静态测试锁住） |

## 3. 单测（已核对）

`python -I AutoTest/run_war3_emutls_test.py --out <新目录>`（持 `build` 锁，因为它要编译）。用 `build-win32-weather-llvm.txt` 里的 clang，
全部 `-femulated-tls -static`，编四个产物：

- `payload.dll`：测试源 + `war3_emutls.cpp`。`thread_local` 有带模板的结构、`alignas(64)`、`alignas(4096)`、4 MiB 数组、
  零初值数组、带析构的对象（析构时查金丝雀）、300 个带非零模板的 `int`（槽数组要翻倍几次）。`DllMain` 像 `d3d9_main.cpp:193` 一样调 `DisableThreadLibraryCalls`。
- `payload_rt.dll`：同一份源，**不带**我们的文件，即 compiler-rt 的运行时，作对照。
- `late.dll`：在**它自己的** `DLL_PROCESS_DETACH`（比 payload 晚）里访问 payload 的 `thread_local`，模拟 Game.dll / Storm.dll 收尾时进我们的钩子。
- `emutls_test.exe`：下面的检查。

结果（`scratchpad/emutls_test5.json`，与最终 DLL etls2d 同一份 `war3_emutls.cpp`；etls2a 那份是 `emutls_test3.json`，etls2b 是 `emutls_test4.json`，都全过，只是少后加的项）：

| 检查 | 结果 |
|---|---|
| 41 项检查，跑两遍：普通（运行时的槽号 2）和 `--high-index`（先占满 0–63 号槽，运行时拿到 65 号，走 TEB 的扩展数组，游戏里加载的 DLL 多时就是这样） | 两遍都 **0 失败** |
| 主线程首次访问、最后错误码（分配路径和缓存路径都保留调用前的值） | 过 |
| 进程终止路径（`EmutlsReleaseForUnload(true)`）：索引、块、字节都不变，之后照常访问 | 过 |
| 16 线程并发：各自对象地址互不相同、`alignas(4096)` 对齐、模板值正确、退出后块数与字节数回到原值 | 过 |
| 200 个短命线程各碰 4 MiB：私有字节 **±0.1 MiB**；带析构对象的析构都在释放之前跑完（≥216 次，0 次读到坏金丝雀） | 过 |
| `FreeLibrary`：主线程 + 4 个挂着的线程共 5 块，卸载后私有字节 **−20.1 MiB**；重新加载从空开始；挂着的线程用新映像照样工作，退出时释放 | 过 |
| 退出场景，我们的运行时 | 退出码 **0** |
| 退出场景，compiler-rt（对照） | 退出码 **0xC0000409**，stderr `Windows error: 参数错误。`——与游戏里的崩溃同一个样子，所以这个场景确实能区分 |
| 退出场景变体 `--exit-fresh`：payload 的 TLS 在退出之前没人碰过，`late.dll` 在退出时第一次碰（TLS 索引、线程块、对象都在终止期间才建），读到的都是初值 | 退出码 **0** |
| `llvm-nm`：payload 有 `___emutls_get_address` 和回调变量、没有 `_emutls_exit`；对照有 `_emutls_exit` | 过 |

一处只能在测试里说明的地方：带析构的 `thread_local` **第一次**被访问时，mingw 的 `__cxa_thread_atexit` 自己会调 `TlsGetValue`、`calloc`，
改掉最后错误码——原生 TLS 下也一样，不是我们的运行时造成的。所以“保留错误码”只检查运行时自己的路径（`Payload_LastError`）。

## 4. 构建与核对（a）

- 新目录 `build32_device_owner_etls2_20261117_dev`，辅助脚本 `scratchpad/dev_x86_build_etls.py`（与 T1 相同：meson 选项照 `dev_x86_build.py`，
  setup 加 `-Dc_args=['-IE:/Dev/Vulkan SDK/Include', '-femulated-tls']` 和同样的 `cpp_args`）。持 `build` 锁，BelowNormal、`-j2`，
  一次全量 + 四次增量（每次只重编 `war3_emutls.cpp` 或 `war3_control_plane.cpp` 再链接），退出码都是 0；这两个文件没有编译警告。
- etls1 之后源码树里改过的只有本次的文件（`find -newer` 核对：`war3_emutls.cpp/.h`、`src/d3d9/meson.build`，以及 etls2c 起的
  `war3_control_plane.cpp` 几行），所以与 etls1 比只差运行时。冻结了四份，**最终是 etls2d**：

| 副本（`scratchpad/mem_dll/…/d3d9.dll`） | md5 | 与上一份的差别 | 用途 |
|---|---|---|---|
| `etls2a` | `9ae02236f71a93a09999abf3e5cf6406` | 第一版运行时 | 门 r1（§5.1） |
| `etls2b` | `16f38028e3facbe09d37cf15543a944e` | 进程终止期间新建线程块不拿自旋锁、不挂链表 | 只做了单测 |
| `etls2c` | `3ce8ee0c4d3da802b613e8328c71a546` | `get_runtime_status` 回复多一个 `emutls` 计数器对象 | 只做了核对 |
| `etls2d` | `2d92add1e8a91c3ab1d0484e5ca8b95a` | 热路径直接读 TEB，不调 API（§4.3） | 门 r2（§5.2）、生命周期（§6） |

- etls2b 补的那处的理由：`ExitProcess` 先把别的线程停在任意位置，若某个线程正好停在登记链表的那几条指针写里，自旋锁就永远不放；
  之后若有线程在终止期间才第一次碰我们的 TLS（别的 DLL 收尾时进钩子），etls2a 会在这把锁上空转（挂死）。终止期间本来就什么都不释放，
  所以这种块不需要登记；线程退出摘链时只在 `g_threads == block` 时改表头，未登记的块不会误改。`ImageTeardown` 的析构也保存/恢复最后错误码。
  这个窗口只有几条指令，实际碰上的概率极小（推断），但修起来便宜。单测加了“TLS 第一次访问发生在退出时”的场景（§3）。
- etls2c 的计数器只在模拟 TLS 构建、索引已分配时出现，供生命周期测试看游戏里线程退出到底释放了没有（§6）。

### 4.1 我们的符号胜过 compiler-rt 的（已核对，`scratchpad/etls2_verify.py`，结果 `scratchpad/etls/etls2{a,b,c,d}_verify.json`）

四份结果除了地址都一样。下表列 etls2d。

| 项 | etls1（compiler-rt） | etls2a–d（自带） |
|---|---|---|
| `___emutls_get_address` 的定义 | 1 个，旁边是 `_emutls_init@12`、`_win_error` | 1 个（etls2d：`T 0x10275170`），前后是 `EmutlsReleaseForUnload`、`SlowAddress`、`ThreadExitCallback`——都是 `war3_emutls.cpp` 的 |
| compiler-rt 的模拟 TLS 静态符号（`_emutls_init/_exit/_mutex/_tls_index/_num_object/_init_once`） | 6 个 | **0** |
| `Windows error` 字符串（compiler-rt 的 `abort` 前打印） | 2 处 | **0** |
| PE TLS 模板 | 12 B | **12 B** |
| TLS 回调数组 | `_tls_callback`、`__dyn_tls_init`、`__dyn_tls_dtor` | 同上三项，**之后**是 `ThreadExitCallback`（`.CRT$XLY`） |
| `___emutls_v.*` 控制块 | 264 | 264 |
| `War3HotHookState` 的 `state` | 1 个 `___emutls_v.…War3HotHookStateEvE5state` | **1 个** |
| `d3d9.dll.p` 与 10 个静态库里带 `.tls$` 的对象（`research/tls/tls_census.py`） | 0 | **0** |

为什么不会两个都链进来：`libclang_rt.builtins-i386.a` 的 `emutls.c.obj` 只对外定义 `___emutls_get_address` 一个符号（`llvm-nm -g --defined-only`）。
静态库成员只在解析未定义符号时才拉；我们的对象已经定义了它，这个成员就没有理由被拉进来（真拉进来会报重复定义）。DLL 里一个它的静态符号都没有，印证了这一点。

etls2d 的 `___emutls_get_address` 反汇编（`llvm-objdump`）：热路径是两次全局读、`fs:[0x18]` 取 TEB、`[teb+0xE10+4·idx]`（或 `[teb+0xF94]` 扩展数组）、
两次比较、一次数组读，**没有 `call`**；只有不命中时才跳到 `SlowAddress`。

### 4.2 切口门与静态测试（已核对）

- `run_x86_data_layer_cut_gate.py --ratchet`，基线 `x86-data-layer-cut-overlay-20261117-r2`，产物 `x86-data-layer-cut-etls2-20261117-r1`（etls2a）、`-r2`（etls2c）、`-r3`（etls2d，最终）：**都通过**，数字相同。
  - 正向 19 个符号 / 24 条边，**增 0 减 0**。新对象 `war3_memory_war3_emutls.cpp` 归 `data`（数据层对象 83 → 84）。
  - 反向 518 → 519：核心代码（设备、DXVK）的每次 `thread_local` 访问现在都调数据层里的 `___emutls_get_address`；
    另外两个本来就被反向引用的数据层 `thread_local` 换成了 `___emutls_v.` 名字（各 1 增 1 减）。反向不设棘轮。
- `test_war3_emutls_static.py` 在改交叉文件前 9/10（差的就是交叉文件那条），改后 10/10（§7.2）；`test_x86_memory_relief_static.py` 11/11；`test_x86_data_layer_cut_gate.py` 9/9。
- 本构建目录里重编并运行的两个线程相关 meson 测试（etls2a 时）：`warvk-lazy-thread-array-test` 16,319 项 0 失败；`war3_shadow_build_thread_gate_test` 9/9。

### 4.3 每次访问的代价（微基准，已核对，`scratchpad/bench/emutls_bench.cpp`）

门 r1（etls2a）里进程内帧率 LT、生与死比原生基线低（§5.1），所以单独量了每次 `thread_local` 访问的代价：一个导出的 `noinline` 函数，
内部访问 1 个或 2 个 `thread_local`，exe 经函数指针调 2,000 万次、取 7 轮最好值。同一个 clang、`-O2 -static`，四种运行时：

| 每次调用 ns（1 个变量 / 2 个变量；两轮） | 原生 TLS | compiler-rt | 第一版（etls2a–c） | 最终（etls2d） |
|---|---|---|---|---|
| 槽号 < 64 | 1.43–1.59 / 1.51–1.59 | 4.36–5.06 / 8.50–8.72 | 7.88–8.00 / 15.09–15.38 | **3.34–3.49 / 5.96–7.27** |
| 槽号 ≥ 64（扩展数组） | 1.27–1.33 / 1.54–1.67 | 5.37–5.42 / 9.49 | 7.82–8.97 / 16.38–18.40 | **3.39–3.53 / 6.18–6.84** |

（四种 DLL 同一时间连跑两轮。空函数约 1.2–1.6 ns；机器上别的会话在跑 ML，零点几 ns 的差别是噪声。）

- 第一版每次访问比 compiler-rt 还多约 3 ns：多出来的是每次都调 `GetLastError`/`SetLastError`（两个 kernel32 导入）和两次多余的原子读。
- 最终版热路径不调任何函数：每次访问比 compiler-rt（它每次调 `TlsGetValue`）省 1–2 ns，扩展数组那种更省；比原生 TLS 仍多约 2 ns/次——
  这是模拟 TLS 本身的代价（推断：游戏里每帧的 `thread_local` 访问以千到万计，量级是几十微秒，见 §5.2 的帧率）。

## 5. A/B 门（b）

命令与 T1 / r2 完全相同（`scratchpad/mem_gate.sh` → `run_x64_phase52_ab_gate.py`，3 张图 × 2 种模式 × 2 轮，前端 `fixture_build/proxy_phase52.dll`，
宿主 `build64_render_host_phase52_20261117_a`），只换 `--product-dll`。对照：原生 TLS `x64-mem-relief-ab-20261117-r2`（同源码只差开关）、
compiler-rt 模拟 TLS `x64-etls-ab-20261117-r1`（T1）。比较脚本 `scratchpad/etls_compare.py`。

### 5.1 r1：etls2a（已核对，`AutoTest/artifacts/x64-etls2-ab-20261117-r1`，18:06–18:26）

- **12 局游戏全部自己退出，退出码 0**（`timedOut=false`、`forcedTermination=false`）。T1 的 etls1 是 12/12 `0xC0000409`。
- 门的红项：
  - `paceWithinLimit` 三张图都假——原生基线 r2 也是三张都假（代理与进程内的帧率比超限，与 TLS 无关）。
  - `allRunsPassed` 假，只因生与死代理 r1 的 `sessionsEndedClean`：设备宿主自己的日志已经走到 `exit-intent strategy=normal-return code=0`、
    `wmain-locals-complete`，但前端记的终端是 `local-contract hostExit=259`（等宿主退出的 5 s 内它还活着）。这与 gates 的
    [实施记录](2026-11-17-x64-proxy-real-game-gates-impl.md) §4.3 / §5 里换图 r1 “待复查”的是同一个样子；那是 x64 宿主进程退出慢，
    不在我们改的 x86 进程里（推断）。同一张图 r2 没有出现。
  - 其余每局只有 `frameNotFlat`（代理臂），门本身不算它，r2 也一样。
- 内存（两轮中位数，MiB；空闲 / 最大洞是 `vaSample`，低 2 GiB，关窗时）：

| 图 | 模式 | 空闲 原生 → etls2a | 最大洞 | 峰值私有字节 | T1（etls1）的空闲增量 |
|---|---|---|---|---|---|
| LT | 进程内 | 789.8 → 1043.6（**+253.8**） | 305.8 → 575.6 | 604.6 → 346.3（−258.3） | +254.5 |
| LT | 代理 | 1409.2 → 1559.5（**+150.3**） | 988.7 → 1000.4 | 302.2 → 146.5（−155.7） | +151.0 |
| EI | 进程内 | 801.8 → 1051.5（**+249.7**） | 303.0 → 579.2 | 602.6 → 339.7（−262.9） | +249.5 |
| EI | 代理 | 1420.4 → 1568.2（**+147.8**） | 1000.4 → 1000.4 | 293.9 → 140.1（−153.8） | +147.8 |
| 生与死 | 进程内 | 269.3 → 496.1（**+226.8**） | 100.2 → 130.2 | 1072.2 → 824.2（−248.0） | +228.3 |
| 生与死 | 代理 | 929.8 → 1055.0（**+125.2**） | 491.0 → 635.2 | 711.5 → 564.0（−147.5） | +125.4 |

  与 T1 逐项一致（差在 ±2 MiB 内）：省下的就是那 77 / 45 / 38 份静态 TLS 块。
- 帧率（各局中位帧率，fps；两轮）：

| 图 | 模式 | 原生 r2 | etls1（compiler-rt） | etls2a（第一版） |
|---|---|---|---|---|
| LT | 进程内 | 178.2 / 194.7 | 186.1 / 168.5 | 153.6 / 146.8 |
| LT | 代理 | 26.9 / 31.0 | 35.7 / 34.5 | 29.0 / 29.4 |
| EI | 进程内 | 84.7 / 144.3 | 139.8 / 142.3 | 110.2 / 114.8 |
| EI | 代理 | 28.3 / 23.4 | 30.3 / 26.3 | 31.9 / 30.3 |
| 生与死 | 进程内 | 40.7 / 40.1 | 41.2 / 45.0 | 29.0 / 33.4 |
| 生与死 | 代理 | 8.1 / 8.2 | 8.5 / 8.2 | 6.3 / 7.7 |

  进程内三张图两轮都比 etls1 低 15–25%，方向一致，不像纯噪声。微基准（§4.3）显示第一版每次访问比 compiler-rt 多约 3 ns；
  模拟 TLS 下每个访问 `thread_local` 的函数（没被内联的）每次执行至少调用一次 `___emutls_get_address`，
  被频繁调用的小函数（钩子入口、每次绘制的状态查询）多，差别就放大（推断，没有逐函数计数）。
  所以改成了 etls2d（热路径不调 API），并在 etls2d 上重跑这个门（§5.2）。

### 5.2 r2：etls2d，最终版（已核对，`AutoTest/artifacts/x64-etls2-ab-20261117-r2`，18:32–18:52）

- **12 局游戏全部自己退出，退出码 0**；每局探针检查只有代理臂的 `frameNotFlat`（门不算）；**`allRunsPassed` 为真**。
  门的红项只剩三张图的 `paceWithinLimit`，与原生基线 r2 完全相同。
- 内存（两轮中位数，MiB，比原生 r2）：

| 图 | 模式 | 空闲 | 最大洞 | 峰值私有字节 |
|---|---|---|---|---|
| LT | 进程内 | 789.8 → 1043.6（**+253.8**） | 305.8 → 574.2（+268.4） | 604.6 → 341.1（**−263.5**） |
| LT | 代理 | 1409.2 → 1560.7（**+151.5**） | 988.7 → 1000.4 | 302.2 → 146.6（**−155.6**） |
| EI | 进程内 | 801.8 → 1051.4（**+249.6**） | 303.0 → 586.0（+283.0） | 602.6 → 338.5（**−264.1**） |
| EI | 代理 | 1420.4 → 1568.2（**+147.8**） | 1000.4 → 1000.4 | 293.9 → 140.0（**−153.9**） |
| 生与死 | 进程内 | 269.3 → 491.2（**+221.9**） | 100.2 → 130.2（+30.0） | 1072.2 → 823.7（**−248.5**） |
| 生与死 | 代理 | 929.8 → 1055.0（**+125.2**） | 491.0 → 638.6（+147.6） | 711.5 → 564.4（**−147.1**） |

  与 T1、r1 一致。进入游戏 20 s 后到关窗前的私有字节：进程内 −16 ～ +2.5 MiB、代理 ±0.2 MiB（`etls_compare.py` 的 plateauGrowth），不涨。
- 帧率（各局中位帧率）：进程内 LT 147.1 / 148.8、EI 111.7 / 113.8、生与死 34.9 / 32.3；代理 LT 29.7 / 30.8、EI 28.0 / 27.4、生与死 7.7 / 8.2。
  **和 etls2a 几乎一样**——热路径每次省下 4–5 ns 并没有让进程内帧率回来。所以 r1 / r2 进程内比 etls1 低的那 15–25%，
  不是“每次访问几 ns”能解释的；要么是这两个时段机器上别的负载，要么是 etls2 与 etls1 之间别的差别。§5.3 用交替跑来分。

### 5.3 交替帧率 A/B（已核对，`AutoTest/artifacts/x64-etls2-fps-20261117-{v3,etls2d}-{a,b}`，19:03–19:13）

同一个门脚本、只跑 LT、每次 1 轮两臂，按 原生 v3 → etls2d → v3 → etls2d 交替（`scratchpad/mem_fps_ab.sh`），让时段负载的变化落在两边。
表里是各局中位帧率对应的帧时间（ms）：

| 臂 | v3 a | etls2d a | v3 b | etls2d b | etls2d / v3（中位） |
|---|---|---|---|---|---|
| 进程内 | 8.588 | 7.599 | 7.253 | 6.828 | 0.911 |
| 代理 | 44.326 | 47.214 | 59.988 | —（没跑成） | 1.065（只有 a 一对） |

- 所有跑成的局游戏退出码都是 0。etls2d-a 的代理臂又出现一次宿主慢退（`sessionsEndedClean`）；gates 自己用原生 TLS 的 DLL（`gates_bin/d3d9.dll`，模板 3.22 MiB）
  在换图 r1 里也出过同样的事，所以它与模拟 TLS 无关（已核对：那份 DLL 的 TLS 模板）。
- **同一个原生 DLL 前后两次就差 18%（进程内）/ 35%（代理）**：这台机器上 T1、r1、r2 之间进程内帧率的差别，用“时段负载”就足以解释。
  原生 v3 这次只有 116 / 138 fps，T1 时段是 178 / 195。
- 进程内四次是单调变快的（8.59 → 7.60 → 7.25 → 6.83），漂移偏向后跑的 etls2d。按“线性漂移 + 一个常数差”扣掉漂移，两次估计是 −4.2% / +3.5%，
  合起来约 −0.5%；用前后两次 v3 的均值夹住 etls2d-a 是 −4.1%。**进程内没有看到变慢**。
- 代理只有一对（+6.5%），而该臂自身两次相差 35%，说明不了什么。
- 没跑成的原因：**E: 盘满**（1.9 T 只剩 2.0 GB），沙盒实例预检要求“预计写入 + 1 GiB 保留”，etls2d-b 的代理臂和随后补的反向一对都在预检就停了（0.2 s）。
  这不是本改动的问题，但它让所有游戏运行都停了，已报给 main。

## 6. 生命周期（c）

### 6.1 做法

- DLL：`scratchpad/mem_dll/etls2c/d3d9.dll`（md5 `3ce8ee0c4d3da802b613e8328c71a546`），即 etls2b 再加一处诊断：
  `get_runtime_status` 的回复多一个 `emutls` 对象（`war3/tools/war3_control_plane.cpp`，只在模拟 TLS 构建且索引已分配时出现），
  内容是运行时的计数器：变量数、持有存储的线程数、对象数、字节数、线程退出释放次数、`lateAccesses`、卸载释放次数。
  这样探针每次 `status` 都能看到游戏里线程退出到底有没有释放。核对：模板 12 B、回调顺序、符号与 etls2b 相同（`etls/etls2c_verify.json`），
  切口门 `x86-data-layer-cut-etls2-20261117-r2` 正向仍 19/24。
- 探针：gates 做的 `--extra-map` 换图探针（`run_x64_game_proxy_probe.py`），脚本 `scratchpad/mem_lifecycle.sh`，持 `game` 锁先进程内、后代理各一局，每局 240 s：
  - `--map` 是换图门的 A.w3x（`410436ED…`），`--extra-map` 把 A、B（`B7302655…`）放到 A/B 脚本 `ChangeLevel` 用的 `Maps/AutoTest/WarVKCrossMap20260925/`。
    地图自己走 A → B → A（A 第一次进 20 s 后切 B，B 进 20 s 后切回 A，A 第二次不再切；靠游戏缓存里的 `stage`）。
  - 地图只会自己切两次，所以之后再用游戏菜单重开 A 四次：80/120/160/200 s 各按 `F10`、`E`（结束游戏）、`R`（重新开始）、`R`（确认）。
  - 每 30–40 s 一次 `status`、几张截图；240 s 时探针自己的 `WM_CLOSE` + `X` 正常退出。
- 实际用的 DLL 是 etls2d（与 etls2c 只差热路径，计数器相同）。§6.2 的产物：`AutoTest/artifacts/x64-etls2-life-20261117-r1-inprocess`、`-proxy`（18:52–19:00，与门 r2 同一次持锁）。

### 6.2 结果（已核对）

- **菜单重开有效**：两种模式都是 7 次载图（`logs/War3_d3d9.log` 里 `WarVK map transition signal … count=1…7`；`status` 的 `publishRevision` 每次换图跳一档：
  进程内 4/6/8/10/12/14，代理 4/7/10/13/16/19）。截图 `e75/e112/e152/e192/e232.bmp` 都在游戏画面里。
- **退出**：两局游戏都自己退出，退出码 0，无超时、无强杀；代理的宿主终端三个都是 `kind=none hostExit=0`。
- 进程内（私有字节，MiB，`observer.samples` 每 2 s 一点）：

| 时段 | 私有字节 |
|---|---|
| A 第一次（~30–55 s） | 370–375 |
| B（~57–75 s） | 364–369 |
| A 第二次（~76–112 s） | 368–376 |
| 重开 1–3 前的谷底（114–124 / 154–164 / 193–206 s） | 360 / 359 / 361 |
| 重开 1–3 后的峰（126 / 166 / 208 s） | 376 / 376 / 380 |
| 重开 4 之后（216 s 峰 390 → 233–242 s） | 374–375 |

  谷底不升，结束值与第一次进图时相同：**没有随换图累积**。
- 进程内的 `emutls` 计数器（`status-e*.json`）：

| 时刻 | 变量 | 持有存储的线程 | 对象 | 字节 | 线程退出释放 | `lateAccesses` |
|---|---|---|---|---|---|---|
| 10 s | 42 | 8 | 53 | 941,246 | 0 | 0 |
| 40 s | 176 | 11 | 188 | 2,462,036 | 2,351 | 0 |
| 110 s | 177 | 11 | 189 | 2,462,037 | 8,594 | 0 |
| 190 s | 177 | 11 | 189 | 2,462,037 | 15,812 | 0 |
| 236 s | 177 | 11 | 189 | 2,462,037 | 20,078 | 0 |

  - 进程内**每秒约 85 个线程退出**（40 s 到 236 s 共 17,700 个），每个都碰过我们的 `thread_local`，全部在退出时释放：持有存储的线程数、字节数从 40 s 起不变。
    compiler-rt 的运行时从不释放这些（T1 的 etls1 就是慢漏，只是 30 s 窗口里看不出来）；原生 TLS 则要给每个新线程分配并清零 3.22 MiB 的块（推断：每秒约 270 MiB）。
  - `lateAccesses` 一直是 0：没有别的 DLL 在自己的线程退出通知里（晚于我们）碰我们的 TLS。
  - 代理模式里这个抖动不存在（`threadsReleased` 一直是 1，持有存储的线程 9 个、字节 1,375,886 不变），所以短命线程来自进程内的设备侧（推断；是谁在建线程不在本任务范围，值得另查）。
- 代理（私有字节）：进图后 129–132 MiB，7 次载图期间只在每次载图时短暂跌到 120–127，全程不升，结束 131.6 MiB。
- 代理那局探针 `ok=false` 只因 `actionsPosted`：10 s 那次 `status` 时控制管道还没建好（`PipeOpenError … Win32=2`），之后所有动作（含 16 个按键）都投递成功。

## 7. 交叉文件（d）

### 7.1 改动（已核对）

`build-win32-weather-llvm.txt` 的 `[built-in options]`：

```
c_args = ['-IE:/Dev/Vulkan SDK/Include', '-femulated-tls']
cpp_args = ['-IE:/Dev/Vulkan SDK/Include', '-femulated-tls']
```

- 这是 `dev_x86_build.py` 和 `AutoTest/run_device_owned_build.py` 用的 x86 llvm 交叉文件，只改它一个。
  GCC 的 `build-win32.txt`、`build-win64.txt` 不动（1.22.x 发布线用 libgcc 自己的模拟 TLS）；x64 宿主的 `build-win64-render-host-llvm.txt` 不动。
- 改之前先告诉了 main（由它通知其他 agent）。改的时候持 `build` 锁。
- 决定改的依据（lead 定的“方案 A”，原话的要点记在这里）：进程内扣掉时段漂移后在 ±5% 以内（约 −0.5%）；代理那一对（+6.5%）远在该臂自身 35% 的噪声之内；
  微基准里热路径比 compiler-rt 还快；内存是首要目标；这是一行没提交的改动，随时能撤。**平衡的 3 对 A/B（含代理、两种先后顺序）列为待办**，等磁盘腾出空间再跑（§8）。

### 7.2 改后核对（已核对）

- 用这个交叉文件新建一个小 meson 工程：`compile_commands.json` 里带 `-femulated-tls`，产物里有 `emutls` 符号。
- `test_war3_emutls_static.py` **10/10**（交叉文件那条现在过了；GCC 交叉文件不含开关那条也过）；`test_x86_memory_relief_static.py` 11/11；`test_x86_data_layer_cut_gate.py` 9/9。
  读这个交叉文件的只有 `run_war3_emutls_test.py`（取编译器路径）、`test_war3_emutls_static.py`、`run_device_owned_build.py` 和 `package_weather_preview_20261010.py`（后两个把它算进源哈希，所以之后的回执哈希会变）。

### 7.3 已有构建目录不会自动带上开关（已核对，Meson 1.10.0）

在 scratchpad 里用一个小工程实测：交叉文件里的 `c_args` 改了之后跑 `ninja`，Meson 会“Regenerating build files”，但 `compile_commands.json` 里**还是旧的参数**，
不重编。所以：

- 已有的 `build32_*` 目录（包括别的 agent 的）继续是原生 TLS，什么都不会坏；它们也编进了 `war3_emutls.cpp`，但没人调它（线程退出回调看到索引未分配就直接返回）。
- 要拿到模拟 TLS 必须**新建构建目录**（或 `--wipe`）。`run_device_owned_build.py` 本来就要求新目录（除非 `--reuse`）；`dev_x86_build.py` 会复用已有目录。
- 新目录第一次是全量构建；产物的静态 TLS 模板是 12 B，`get_runtime_status` 多一个 `emutls` 对象。门里的 `staticTlsCopies` 一栏在这种构建下没有意义（T1 §4.1 已说明）。

## 8. 剩下的风险

| 风险 | 现状 |
|---|---|
| 每次访问的代价 | 热路径比原生 TLS 多约 2 ns / 次（§4.3），比 compiler-rt 少。游戏里的帧率见 §5 |
| 别的 DLL 在**它自己的**线程退出通知里（晚于我们）碰我们的 TLS | 该线程会重新分配一块，直到卸载才回收（进程终止时不回收），计数 `lateAccesses`。CPU 单测里是 0；游戏里的值见 §6 |
| `FreeLibrary` 之后本映像的代码又碰 TLS | 只可能发生在卸载过程中、最后一个静态析构之后：会再 `TlsAlloc` 一个索引并分配块，每次加载至多一份，不崩。游戏不卸载这个 DLL（推断：Storm 钩子不卸，卸了本来就会崩） |
| 进程终止时另一个线程死在登记锁里 | etls2b 起终止期间不拿这把锁（§4） |
| 分配失败 | 打一行 `WarVK emutls: …` 到调试输出和 stderr，然后 `abort()`（与 compiler-rt 相同）；没有更好的退路 |
| TEB 布局 | 用的是 `<winternl.h>` 公开的 `TlsSlots` / `TlsExpansionSlots`，偏移有 `static_assert`；这两个字段自 NT 起没变过（推断），而且 `TlsGetValue` 本身就是这样读的 |
| 只有新建的构建目录才带开关 | §7：Meson 1.10 在交叉文件变化后会重新生成，但**不会**把新的 `c_args`/`cpp_args` 套到已有的构建目录上（实测） |
| GCC 构建（1.22.x 发布线） | 不受影响：`war3_emutls.cpp` 只在 clang 下编译出内容；GCC 交叉文件不加开关（静态测试锁住） |
| 帧率只做了 2 对（进程内）/ 1 对（代理） | **待办**：磁盘腾出空间后，跑平衡的 3 对 A/B（原生 v3 ↔ etls2d，LT，两臂，两种先后顺序），确认没有超过 5% 的回退；若有，撤掉交叉文件那一行即可 |
| 进程内每秒约 85 个短命线程（§6.2） | 不是本改动引入的，模拟 TLS 下它们的存储都被释放；但这个线程抖动本身值得另查（推断来自进程内的设备侧） |
| E: 盘满 | 沙盒实例预检因空间不足失败，所有游戏运行都停了；我没删任何东西，已报给 main |

## 9. 产物

- 源码：`src/d3d9/war3/memory/war3_emutls.cpp`、`war3_emutls.h`（新）；`src/d3d9/meson.build`、`src/d3d9/war3/tools/war3_control_plane.cpp`（几行）；
  `build-win32-weather-llvm.txt`（§7）。测试：`AutoTest/test_war3_emutls.cpp`、`run_war3_emutls_test.py`、`test_war3_emutls_static.py`（新）。
- 构建目录 `build32_device_owner_etls2_20261117_dev`（保留，当前内容 = etls2d）；冻结 DLL `scratchpad/mem_dll/etls2{a,b,c,d}/d3d9.dll`。
- 核对：`scratchpad/etls2_verify.py`、`scratchpad/etls/etls2{a,b,c,d}_verify.json`、`census2_*.tsv`；单测输出 `scratchpad/emutls_test{3,4,5}.json`；
  微基准 `scratchpad/bench/`。
- 门与切口门：`AutoTest/artifacts/x64-etls2-ab-20261117-r1`（etls2a）、`-r2`（etls2d）；`x86-data-layer-cut-etls2-20261117-r1/r2/r3`。
- 生命周期：`AutoTest/artifacts/x64-etls2-life-20261117-r1-inprocess`、`-proxy`；脚本 `scratchpad/mem_b_c.sh`、报表 `scratchpad/mem_lifecycle_report.py`。
- 交替帧率：`AutoTest/artifacts/x64-etls2-fps-20261117-{v3,etls2d}-{a,b}`（`etls2d-b` 只有进程内臂）；`-c` 两个目录是磁盘满时的空壳（预检失败，没有游戏运行）。
  脚本 `scratchpad/mem_fps_ab.sh`。
- Meson 行为实测：`scratchpad/mesontest`（改交叉文件后已有目录不换参数）、`scratchpad/mesontest2`（新目录带上开关）。
- 没有提交、没有部署；玩家目录没碰。
