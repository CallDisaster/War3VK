# 代理模式设置面板（ImGui 覆盖层）实施回执

日期：2026-11-17。设计：`docs/research/2026-11-17-x64-overlay-proxy.md`（路线 (a)：x86 数据层自己跑 ImGui，在 `present` 回调里经代理 `IDirect3DDevice9` 画）。
标记：**已核对** = 本次亲自跑过、读过或量过；**推断** = 按源码推理，运行中没有证明。

## 0. 结论

代理模式下设置面板能用了，可以进入玩家试用前的人工手测。设计 §7.3 的九条验收：
- 第 1、2、4、6、7 条已核对。
- 第 8 条已核对，但只对照了本次的第三轮，没有对照 5.2 的旧数据。
- 第 3 条只证明到“消息到了窗口过程”：隔离桌面上点击没能被 ImGui 截住，原因与设计预期相同。
- 第 5 条已核对同量级；“面板 draw 没混进记录”属推断。
- 第 9 条记下了数字，但被机器噪声淹没，按设计不设门槛。

具体数字（均已核对）：
- 宿主零改动、线协议零改动。
- 切口门 `--ratchet` 正向仍是 19 个符号 / 24 条边，没有增加。
- 进程内生成的后端哈希不变。
- 面板第一次打开前 x86 不多花字体和图集的内存；打开多 19.1 MiB 专用字节，关闭后回到对照轮水平。

## 1. 改了哪些文件

前端（宿主零改动、线协议零改动）：
- `tools/render_host/d3d9_proxy.cpp`
  - F1：`followWindow` 加 `!observing`，回调里不改尺寸。
  - F2：`Device::Reset` 在健康检查之后、`losable` 检查之前调用 `observer.reset`。
  - F3：新增 `WarVKD3D9RetainPrivate` / `WarVKD3D9ReleasePrivate` 两个导出。只认本设备的 VB/IB/纹理三种子对象，并且只在所有者线程上生效。
  - F4：`GetDisplayMode` 转给 `factory->GetAdapterDisplayMode(0,…)`。
  - `setObserver` 两种结构大小都接受：带 `reset` 的新版，和到 `reset` 之前为止的旧版。
- `tools/render_host/d3d9_proxy.def`：加两条导出。
- `src/d3d9/war3/core/war3_d3d9_observer.h`：`reset` 加在结构体末尾，另加两个导出的声明。

生成器与构建：
- `AutoTest/prepare_warvk_imgui_dx9.py`：新增 `--variant proxy`。和进程内是同一组 10 处寿命改动，改走 `dxvk::war3::imgui_proxy::`，前缀 `War3ProxyImGui_ImplDX9_`，另加 1 处：去掉“建纹理失败就断言”。进程内变体的输出逐字节不变。
- `src/d3d9/meson.build`：
  - 新 custom_target `warvk-imgui-dx9-proxy-backend`；
  - 新源文件 `war3/platform/war3_proxy_overlay.cpp`、`war3/ui/war3_imgui_inprocess.cpp`。

面板去掉对具体设备的依赖：
- `src/d3d9/war3/ui/war3_imgui.{h,cpp}`
  - 渲染后端改成函数指针表 `ImGuiRenderer`；
  - 设备专属的两块面板（模块热插拔、ShaderPack）改成 `DevicePanels` 钩子；
  - TAA/CSM/回放/天气的诊断查询改走 `ProcessServices()`；
  - 新增 `initializeProxy` / `shutdownProxy` / `invalidateProxy` / `isProxyOwner` / `status()`。
- `src/d3d9/war3/ui/war3_imgui_inprocess.cpp`（新）：只属于进程内设备的部分。`initialize(HWND, D3D9DeviceEx*)`、`setCursorBitmap`、四个管线开关、ShaderPack 面板都从 `war3_imgui.cpp` 原样搬来。
- `src/d3d9/war3/ui/war3_imgui_dx9_proxy.h`、`war3_imgui_dx9_proxy_ownership.h`（新）：代理版后端的入口声明和资源所有权助手。

代理模式接线：
- `src/d3d9/war3/platform/war3_proxy_overlay.{h,cpp}`（新，数据层）：窗口子类、Ctrl+F1、按需打开与关闭、内存日志、`ui.panel_*` 的路由、状态计数。
- `src/d3d9/war3/platform/war3_proxy_mode.{h,cpp}`
  - 解析两个可选导出；
  - 填 `observer.reset`；
  - `OnReleased` 先 `OverlayReleased` 再 `DisconnectDevice`；
  - `ConnectCreatedDevice` 末尾 `OverlayConnect`；
  - 前端是旧版时退回旧结构大小，并关掉覆盖层。
- `src/d3d9/war3/platform/war3_proxy_services.cpp`：在 `ObservePresent` 里、截帧块之后调用 `OverlayPresent`。
- `src/d3d9/war3/tools/war3_internal_test_api.cpp`：`ui.panel_open` / `ui.panel_close` / `ui.panel_status`，两种模式都实现。

测试与门：
- `AutoTest/test_proxy_overlay_static.py`（新，13 个用例）。
- `AutoTest/test_warvk_imgui_dx9_adapter.py`：加 4 个用例。
- `AutoTest/test_x86_data_layer_cut_gate.py`：加 3 个对象分类。
- `AutoTest/test_war3_pipeline_lifetime_static.py`：ShaderPack 段改读两个文件。
- `AutoTest/run_x86_data_layer_cut_gate.py`：代理版生成后端归 `imgui` 层。
- `AutoTest/run_x64_game_proxy_probe.py`：`TEST_COMMANDS` 加三个 `ui.panel_*`，经主线同意，别的没动。

## 2. 五个坑各怎么处理

1. **图集纹理的池和用法（前端只收 usage=0 + MANAGED）。**
   - 代理版所有权头的 `CreateTexture` 忽略上游传来的 `D3DUSAGE_DYNAMIC`/`D3DPOOL_DEFAULT`，固定用 `0, D3DPOOL_MANAGED`。上游锁纹理用的标志是 0（`LockRect(0, …, 0)`，已核对 `imgui_impl_dx9.cpp:408,425`），对 MANAGED 合法。格式（`A8B8G8R8` 或 `A8R8G8B8`）由上游经 `GetDirect3D`/`GetDisplayMode`/`CheckDeviceFormat` 选定，前端收不收看 §5。
   - 代理变体去掉了 `IM_ASSERT(hr >= 0 && "Backend failed to create texture!")`：产品构建里 `IM_ASSERT` 是活的，前端一旦拒绝就会直接终止游戏。
   - 拒绝后纹理保持 `WantCreate`。`War3Imgui::uploadTextures` 在画之前先上传；若还有不是 OK 的纹理，这一帧整帧不画、记一次 `framesSkipped`，所以不会走到 `ImDrawCmd::GetTexID` 的断言。（已核对：源码与静态测试）
   - VB/IB 上游本来就是 `D3DUSAGE_DYNAMIC | WRITEONLY, D3DPOOL_DEFAULT`，前端收的是 DEFAULT + WRITEONLY。`DYNAMIC` 位前端是否接受，看 §5 的 `resourceFailures`。
2. **纹理边长上限 1024。**
   - `initializeContext` 在 `AddFontFromFileTTF` 之前设 `io.Fonts->TexMaxWidth = TexMaxHeight = 1024`，只对代理生效（`textureLimit>0`）；进程内传 0，不设。（已核对：源码与静态测试）
   - 图集实际多大，看 §5 的 `atlasWidth/atlasHeight`。
3. **面板资源拖住设备。**
   - 代理版所有权头建资源成功后，先 `WarVKD3D9RetainPrivate` 加私有引用，再 `Release()` 掉公共引用，所以面板不再让设备的公共计数 >0。
   - 释放走 `WarVKD3D9ReleasePrivate`。这两个导出只在所有者线程上、只认本设备的 VB/IB/纹理，别的一律 `D3DERR_INVALIDCALL`。
   - 加私有引用失败时，把刚建的资源 `Release` 掉、输出置空，把失败返回给上游。
   - 设备的 `released` 回调里先 `OverlayReleased`（`shutdownProxy` 释放全部私有引用），再 `DisconnectDevice`。（已核对：源码与静态测试；运行见 §5 第二轮）
4. **`Reset` 时 `losable>0` 必失败。**
   - 观察者新增 `reset` 回调。前端在健康检查之后、`losable` 检查之前调用它；数据层借此 `invalidateProxy`，放掉 DEFAULT 池的 VB/IB。
   - MANAGED 的图集不算 losable，留着。
   - 回调在 `observing` 作用域里跑，期间的 draw 不会被观察记录。（已核对：源码与静态测试。推断：探针触发不了游戏自己的 `Reset`，只能人在输入桌面改分辨率验证）
5. **回调里吞掉缩放通知。**
   - `followWindow` 在 `observing` 时直接返回。`present` 回调里面板的 `BeginScene` 不会触发改尺寸；改尺寸留到 `Present` 之后那次 `followWindow`，`resized` 回调照常发。
   - 面板的 `DisplaySize` 取自后备缓冲描述，不依赖窗口客户区。（已核对：源码与静态测试；运行见 §5 的 `size=1280x720`）

另外两处：
- F4：`GetDisplayMode` 不再走“未实现入口”。
- 兼容混用的构建：
  - 新前端也接受旧观察者结构大小；
  - 新数据层碰到旧前端（`setObserver` 拒绝新大小，或缺两个导出）时退回旧大小，覆盖层关闭，并记一行 `WarVK proxy overlay: off`。
  - （已核对：源码；推断：混用组合没有实际跑）

## 3. 静态测试

（已核对）

新增的 `AutoTest/test_proxy_overlay_static.py`，13 个用例全过，钉住设计 §7.1 的每一条：
- 前端：
  - `followWindow` 含 `!observing`；`Present` 里帧后的 `followWindow` 还在。
  - `Reset` 先查所有者，再调 `observer.reset`，最后查 `losable`。
  - `setObserver` 两种大小都接受。
  - 两个导出只认本设备的三种子对象，并查所有者线程；`publicReferencesZero` 时拒绝加私有引用。
  - `.def` 有两条新导出；`GetDisplayMode` 转给工厂。
- 观察者头：成员顺序是 `draw, present, resized, terminal, released, reset`，`reset` 在最后。
- 数据层：
  - `ConnectCreatedDevice` 填 `reset`，并在 `InstallHooks` 之后才 `OverlayConnect`。
  - `OnReleased` 先 `OverlayReleased` 再 `DisconnectDevice`。
  - `ObservePresent` 里 `OverlayPresent` 在 `FramePresent` 和截帧之后；`platform/hooks/tools` 下别处都不调用它。
  - 关面板先于开面板处理，开面板前查 `TestCooperativeLevel`。
  - 1024 上限在加字体之前设。
  - 窗口子类：测试消息只在路由时认，Ctrl+F1 照进程内，输入法消息直通，吞输入的条件与 `d3d9_window.cpp` 一致。
- 共享面板文件 `war3_imgui.cpp`：
  - 不含 `War3ImGui_ImplDX9_`、`m_device->BeginScene/EndScene/CreateTexture`、`pipeline->`、`LockRect`、`GetLevelDesc`，也不含非 `internal::` 的 `war3shader::` 调用。
  - 五个诊断查询都走 `ProcessServices()`。
  - `initialize(HWND, D3D9DeviceEx*)`、`setCursorBitmap` 只在 `war3_imgui_inprocess.cpp` 里定义。
- 代理帧：图集没上传就不画；`uploadTextures` 调 `updateTexture`。
- 测试命令和探针都有三个 `ui.panel_*`；`meson.build` 有两个新源文件和代理版生成目标，`d3d9_dll` 用到它。

生成器 `test_warvk_imgui_dx9_adapter.py` 新增 4 个用例：
- 进程内输出哈希仍是 `B545855C…57ED`。
- 代理变体同样 10 处寿命改动加 1 处不断言；前缀各自唯一；每处缺失都单独报错。
- 代理所有权头：MANAGED/0，先加私有引用再放公共引用，不出现 `AddRefPrivate`。
- 两个变体互不覆盖输出。

切口门分类 `test_x86_data_layer_cut_gate.py`：代理版生成后端归 `imgui`，`war3_ui_war3_imgui_inprocess.cpp` 归 `ui`，`war3_platform_war3_proxy_overlay.cpp` 归 `data`。

回归：凡是读到本次改动文件的测试模块（22 个、266 个用例）全过，0 失败。其中包括设计点名要继续过的钉子：
- `test_player_settings_static`
- `test_device_owned_lifetime_contract`
- `test_render_stats_panel_static`
- `test_frame_recorder_console_static`
- `test_frame_history_shortcut_static`
- `test_war3_pipeline_lifetime_static`
- `test_proxy_mode_lifecycle_static`
- `test_shadow_release_hardening_static`
- `test_shadow_taa_v2_static`

搬家是否逐字（已核对，与 `git show HEAD:src/d3d9/war3/ui/war3_imgui.cpp` 比对）：
- `setCursorBitmap` 逐字相同。
- ShaderPack 面板去掉缩进后逐字相同。
- 四个管线开关 19 行里 17 行逐字相同。另外 2 行是 `pipeline` 改成从参数 `device` 取，加一个空检查；原来外层已经保证非空，等价。

## 4. 构建与切口门

构建（已核对）：
- x86：`build32_device_owner_overlay_20261117_dev`，新配置，-j2，BelowNormal，1381 s，0 错误。本次改动的文件没有新增警告。
  - `d3d9.dll` 的 SHA-256：`11365F7E…87FD`，24,961,024 字节。
  - 生成的进程内后端 `war3_imgui_dx9_backend.cpp`：`B545855C…57ED`，与 `build32_device_owner_phase52_20261117_a` 的同名文件逐字节相同。
  - 代理版 `war3_imgui_dx9_proxy_backend.cpp`：`CAA9F5CB…F568`。
- 前端：`scratchpad/fixture_build_overlay/proxy.dll`，构建参数与门相同，0 错误。导出表里有 `WarVKD3D9RetainPrivate` 和 `WarVKD3D9ReleasePrivate`。
- 宿主：没动，用冻结的 `build64_render_host_phase52_20261117_a`。

切口门 `--ratchet`（已核对，只跑 llvm-nm）：

| 产物 | 基线 | 正向 | 正向增/减 | 反向 | 结果 |
|---|---|---|---|---|---|
| `x86-data-layer-cut-overlay-20261117-r1` | phase3-20261115-r1（19/24） | 19 符号 / 24 边 | 0 / 0 | 487→518 | ok |
| `x86-data-layer-cut-overlay-20261117-r2` | phase52-20261117-r1（19/24） | 19 符号 / 24 边 | 0 / 0 | 505→518 | ok |
| `x86-data-layer-cut-overlay-20261117-pre` | phase52（gates 的构建，同一棵树） | 19 / 24 | 0 / 0 | 505→518 | ok（预检，保留） |

数据集（共 135 个对象）的变化：
- 新进入：`war3_ui_war3_imgui.cpp`、代理版生成后端、`war3_platform_war3_proxy_overlay.cpp`，以及 ImGui 库本身（`imgui/imgui_draw/imgui_tables/imgui_widgets/impl_win32`）。
- 留在外面：`war3_imgui_inprocess.cpp` 和进程内生成后端。这正是拆文件的目的。

相对 phase52，反向多了 13 个符号。全部是进程内设备（`d3d9_device.cpp`/`d3d9_swapchain.cpp`/`d3d9_window.cpp`/`d3d9_war3_pipeline.cpp`）调用 `War3Imgui` 的成员：`get/newFrame/render/endFrame/shutdown/beginDeviceTeardown/invalidate/restoreDeviceObjects/handleWndProc/toggleVisible/wantsCapture*`，以及 `s_fpsUnlocked`。
- 原因：`war3_imgui.cpp` 现在两种模式共用、进了数据集，进程内设备照旧驱动它。设计允许反向增加。
- 推断：这些只在进程内模式发生，宿主不需要它们作为注释或负载。

## 5. 探针

运行条件：
- 沙盒 `E:\Work\War3_AutoTestSandbox`，隔离桌面，产品档位，Lost Temple，`--dump-every 60`。
- 每轮持 `game` 锁、先跑 `nvidia-smi`（GPU 约 30%、3.1 GiB 被别的会话占用）。
- 宿主是冻结的 phase52 版；前端是 `fixture_build_overlay/proxy.dll`；x86 是上面那个构建。
- 产物在 `AutoTest/artifacts/x64-overlay-probe-20261117-{r1,r2,r3,r4-inprocess}`，四轮都是 `ok=True`、游戏自己退出（退出码 0）、`failedChecks=[]`。（已核对）

| 轮 | 做了什么 | 结果 |
|---|---|---|
| r1 | 16 s 查状态 → 20 s 开 → 22 s 查 → 24 s 点 (100,70) → 26 s 查 → 30 s 缩放 1280×720 → 34 s 查 → 40 s 关 → 44 s 查 | 见下 |
| r2 | 20 s 开 → 点两次 (100,70) → 30 s 点游戏区 (900,500) → 33 s 查 → 面板开着退出 | 走 `released` 路径 |
| r3 | 不开面板，同样的查询、点击、缩放 | 对照 |
| r4-inprocess | `--in-process`，开/关/查 | 进程内对照 |

逐条验收：

1. **`ui.panel_status`**（已核对）
   - r1 打开后：`initialized/visible/proxyPanel=true`，`framesDrawn` 20→87→202，`framesSkipped=0`，`textureFailures=0`，`lastCommands=2`，`lastVertices=590`。
   - 图集 512×128，不超过 1024。
   - 覆盖层：`resourcesCreated=3`（VB、IB、图集），`resourceFailures=0`，`lastResourceResult=0`。
   - 所以前端接受 `D3DUSAGE_DYNAMIC|WRITEONLY` 的 DEFAULT 缓冲和 MANAGED/0 的 A8R8G8B8 图集，一次没拒。
   - 关闭后：`initialized=false`，图集 0×0，`closes=1`。
   - r4 进程内同一面板也是 2 条命令、590 个顶点、512×128，与代理模式一致。
2. **画面**（已核对，人看过 PNG）
   - `r1/frames/frame-420`、`480`（打开后）左上角有面板，开之前的 `frame-360` 没有。
   - 缩放后的 `frame-720-1601x901` 面板位置、布局正确；关闭后的 `frame-960` 面板消失。
   - 全分辨率裁图 `r1/panel-crop-frame720-fullres.png`：中文（“War3VK 调试器”“帧率”“突破帧率上限”“开始性能日志记录”“帧取证”“着色器系统”“内置渲染设置”“渲染统计”）显示正常。
   - “突破帧率上限”和“开始性能日志记录”在代理模式下按设计置灰。
3. **点击**（部分）
   - 窗口子类装上了：`available=true`。测试命令经子类路由：`routedRequests` 1→2，`toggles` 1→2。
   - 但 r1/r2 在 (100,70) 的点击都没被吞：`swallowedInput=0`，`wantCaptureMouse=false`。
   - 原因（推断）：探针一次投递 MOVE/DOWN/UP，`WantCaptureMouse` 要下一帧才算出来；隔离桌面上窗口不在前台，ImGui 也不会自己更新鼠标位置。进程内 `d3d9_window.cpp` 是同一判断，结果会一样。
   - 能证明键盘一侧：
     - 面板打开后 `wantCaptureKeyboard=true`（新窗口得焦点，与进程内 r4 相同）；
     - r2 点游戏区 (900,500) 后变成 `false`，说明鼠标消息确实到了 ImGui，“点空白处取消焦点”生效。
   - 要人在输入桌面上确认点击被面板截住、游戏没选中单位。
4. **缩放**（已核对）
   - r1 数据层日志有 `window resized 1902x963 -> 1601x901 gameFields=1`，没被吞。
   - 缩放后 `framesSkipped` 仍为 0，面板继续画（202 帧）。
5. **宿主与记录**
   - 三轮代理宿主都是 `rejected=0`、`insertionsRefused=0`、`liveResources=0`、`normalTeardown=true`。（已核对）
   - `beforeUiRuns`：r1 710 次 / 1085 次呈现，r3 941 / 1322。都约等于“游戏内每帧一次”，r1 并没有因为面板多出来。（推断：按每帧一次的对应关系）
   - 面板 draw 在 `observing` 作用域里，不进观察记录。（源码已核对）
6. **未实现入口**：三轮 `unsupported=[]`，`game.stderr` 没有新的未实现入口跟踪。（已核对）
7. **带着面板退出（r2）**（已核对）
   - 日志顺序：`panel released with the device` → `detached` → `device released`。
   - 前端析构打印了 `D3D9 backing-proxy`，说明 `~Device` 跑了。
   - 宿主 `normal-return code=0`、`liveResources=0`；游戏自己退出（退出码 0）。
   - r2 点了游戏区取消面板焦点，否则面板会吞掉探针退出确认用的 `X` 键。这和进程内行为相同。
8. **内存**（已核对；数字来自数据层日志 `WarVK proxy overlay:` 行和 `ui.panel_status`）
   - 打开前不加载 19 MiB 字体：r3 全程 `initialized=false`。
   - 开面板：r1 专用字节 318.5 → 337.6 MiB（+19.1 MiB），虚拟地址 670.0 → 689.1 MiB（+19.1）；r2 也是 +19.1 MiB（地址 +27.1 MiB）。
   - 关面板：r1 降到 315.5 MiB，比开面板后少 22.1 MiB，比开之前还低 3 MiB（游戏自身波动）。之后采样 315–316 MiB，r3 同时段 314–315 MiB，回到对照水平，差在 1–2 MiB 内。
   - 峰值专用字节：r1 342、r2 339、r3 321 MiB。
   - 没有和 5.2 的旧探针逐项对照（同图同档位的旧数据要另找）。
9. **帧时间**（已核对，不设门槛）
   - 整轮呈现次数：r1 1085、r3 1322。
   - 面板打开那段（转储时间 17–41 s）：r1 约 20.4 fps，r3 同段约 25.5 fps。
   - 但 r1 关面板之后那段也只有 20.7 fps（r3 同段 24.3 fps），所以差距主要是共享机器的噪声，面板本身的代价没法从这组数字里分出来。

## 6. 进程内行为不变的证据

- 生成的进程内后端 `war3_imgui_dx9_backend.cpp`，与 phase52 构建的同名文件逐字节相同（SHA-256 `B545855C…57ED`）；单元测试也钉住了这个哈希。（已核对）
- 搬到 `war3_imgui_inprocess.cpp` 的函数，与 HEAD 比对逐字或等价，见 §3。（已核对）
- `war3_imgui.cpp` 里每处新分支都以 `m_proxy` 或 `m_devicePanels` 为条件；进程内 `m_proxy=false`，两个面板钩子都设了。（已核对：源码）
  - “内置渲染设置”的门是 `!m_proxy && (!m_device || !m_device->GetWar3Pipeline())`，进程内等于原来的条件。
  - 代理才设图集上限；进程内 `textureLimit=0` 不设。
  - `render` 里 `!m_proxy ||` 短路，进程内不走 `uploadTextures`。
  - 诊断查询从 `dxvk::Query…` 改成 `ProcessServices().Query…`。已核对：进程内的 `War3InProcessServices`（`d3d9_war3_execution_services.cpp`）在 DLL 加载时就注册，五个查询都原样转发到 `::dxvk::Query…`，返回值相同。
- `shutdown(D3D9DeviceEx*)` 的清理顺序和字段复位与 HEAD 相同，只是挪进了 `destroyContext()`；测试钉住的文字都还在。（已核对）
- r4 进程内探针：（已核对）
  - 面板在启动时初始化：日志有 `War3Imgui initialized.`，图集 512×128，`visible=false`。
  - 开关走 `in-process-owner-thread-toggle`；开时画了 680 帧，关后 `visible=false`。
  - 游戏退出码 0。
- 没跑阴影门、特效门的帧哈希对照：共享机器，GPU 门要轻。推断：面板默认隐藏，进程内代码路径不变，画面不应有差别。

## 7. 剩下的

人工（试用前必须做，在输入桌面上）：
- Ctrl+F1 开关；拖动面板窗口；点击面板时游戏不应收到点击（第 3 条自动化证不到）。
- 勾选“光影”/改 AA 模式后画面变化；重启后 `WarVK\settings.ini` 保留。
- 游戏选项里改分辨率（触发游戏自己的 `Reset`），面板开着、关着各一次，游戏都能回来。F2 的 `reset` 回调在运行中还没被触发过。

已知限制（按设计）：
- 代理模式下“突破帧率上限”“开始性能日志记录”置灰。
- “模块热插拔”和“Vulkan ShaderPack”显示“宿主执行：代理模式下不可用/不可切换”。
- 面板打开并有焦点时吞键盘（与进程内相同）。

没做的：
- 阴影门、特效门帧哈希对照。
- 与 5.2 旧探针的内存逐项对照。
- 新旧前端、数据层混用的组合没实际跑；兼容代码只经源码核对。

失败与旁注：
- 第一次编前端失败在 `d3d9_proxy.cpp:966` 的 `profileTextureUpload` 参数个数上。那是测量 agent 当时进行中的改动，没有留下产物；之后他们改完，本次重编通过。
- 我在 15:04 改了 `meson.build`，gates 的 x86 构建在此之前（15:03）已经配置。它重跑 ninja 时自动重新生成了构建文件，构建通过；已告知主线。
- 切口门预检产物 `x86-data-layer-cut-overlay-20261117-pre` 用的是 gates 的构建，保留。
