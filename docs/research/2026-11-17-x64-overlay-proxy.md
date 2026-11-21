# 代理模式下的设置面板（ImGui 覆盖层）：怎么接回来

2026-11-17，Claude（Opus 研究笔记，只读调研，没有构建、没有跑游戏或探针）。
问题来自 [5.1-b 到 5.2 回执](2026-11-17-x64-phase5-1-proxy-mode.md) §7 与 [5.1 设计](../plan/2026-11-16-x64-phase5-1-first-wiring.md) §7：
代理模式（`WARVK_D3D9_FRONT`）下没有 Ctrl+F1 设置面板，没有它不能给玩家试用。

标记：**已核对** = 我读到了代码或文件；**推断** = 由代码推出来、没有跑过验证。
注意：`tools/render_host/d3d9_proxy.cpp` 与 `war3_proxy_mode.cpp` 在我调研期间正被别的会话修改（帧时间的同步计数 `ApiScope`），
下面的行号是 11-17 当时的，可能漂移；同时给了函数名。

---

## 0. 先说结论

1. **走设计建议的路线 (a)：数据层在 x86 里照旧跑 ImGui，在 `present` 回调里经代理的 `IDirect3DDevice9` 把绘制列表画出来。宿主一行不改，线协议不改。**
   宿主已经支持它要的一切（固定管线、裁剪矩形、托管纹理、动态缓冲）。
2. 但设计里“代理已有的调用就够、只要核对一下”**只对了一半**。原样把上游 DX9 后端接到代理设备上会出 5 个问题，其中 1 个直接让游戏崩溃：
   - **字体图集建不出来 → 断言 → 进程退出**：上游后端用 `D3DUSAGE_DYNAMIC + D3DPOOL_DEFAULT` 建纹理，代理只收 `usage=0 + MANAGED`；
     产品构建没有定义 `NDEBUG`，`IM_ASSERT` 是活的（已核对）。
   - **图集上限**：代理纹理最大 1024，ImGui 图集默认可长到 8192。
   - **面板的资源会拖住设备**：代理子对象的公有引用会给设备加公有引用，游戏最后一次 `Release` 时设备不析构，`released` 回调不来，宿主不收尾。
     进程内为此专门做了“私有引用”所有权适配器，代理这边没有对应物。
   - **游戏 `Reset` 会卡死**：面板持有 DEFAULT 池缓冲时代理的 `Reset` 返回失败并进入 `notReset`，游戏再也回不来。进程内在 `Reset` 入口先让 ImGui 交出资源，代理没有这个时机。
   - **回调里吃掉窗口缩放通知**：ImGui 的 `BeginScene/SetViewport/...` 在回调里会触发 `followWindow`，缩放被执行但 `resized` 回调因为 `observing` 被跳过，游戏的分辨率字段不更新。
3. 这些都能在 **x86 本地** 解决：前端（`d3d9_proxy.cpp`）补 3–4 处小改动（不碰环、不碰线格式、不碰宿主），
   数据层加一个生成的“代理版”ImGui 后端、一个窗口子类、在 `ObservePresent` 末尾画。**Opus 能做，不需要等 Fable。**
4. x86 内存：面板第一次打开前 **0**；打开时约 **20–28 MiB**（大头是 19 MiB 的 `msyh.ttc` 整个读进内存）。建议首次打开才初始化、关闭即整套释放。
5. 路线 (b)（把绘制列表作为记录送到宿主、宿主用 Vulkan 画）省不下 x86 内存（字体与 ImGui 本身必须留在 x86），却要新记录类型、内容生命周期与宿主执行器，是 Fable 级的传输工作。路线 (c)（叠一个分层窗口）不可取。

---

## 1. 进程内模式的覆盖层怎么工作

### 1.1 渲染后端（已核对）

- 用的是上游 `subprojects/imgui/backends/imgui_impl_dx9.cpp`（Dear ImGui 1.92.6 WIP，`imgui.h:32`），**不是**走 DXVK 内部的 Vulkan 路径。
- 构建时由 `AutoTest/prepare_warvk_imgui_dx9.py` 生成 `war3_imgui_dx9_backend.cpp`（`src/d3d9/meson.build:656-667`）：
  只改 10 处“寿命点”（`prepare_warvk_imgui_dx9.py:36-85`），入口改名为 `War3ImGui_ImplDX9_*`（`:117-123`，声明在 `war3/ui/war3_imgui_dx9.h:12-18`），上游源码钉死两个 SHA-256（`:22-25`）。
- 寿命点的意思：设备只借用不 `AddRef`；VB/IB/纹理建好后立刻把公有引用换成 DXVK 的私有引用（`war3/ui/war3_imgui_dx9_ownership.h:11-59`，
  `static_cast<D3D9VertexBuffer*>` 等具体类型）。目的是面板的资源**不拖住设备的公有引用计数**。这个头只能用于本模块的 `D3D9DeviceEx`，不能拿去接代理设备（`:8-10` 注释）。
- 每帧的 D3D9 调用见 §3 的表（`imgui_impl_dx9.cpp:91-307`）。

### 1.2 挂在哪里画（已核对）

- 默认在交换链 `Present` 里、`PresentImage` 之前画：`d3d9_swapchain.cpp:555-572`。顺序是先 `ProcessPendingFrameCapture`（控制面截帧），再 `newFrame/render(false)/endFrame`，
  所以**控制面截帧不含面板**。
- `render(false)` 自己包 `BeginScene/EndScene`（`war3/ui/war3_imgui.cpp:247-253`）。
- 增强效果在 BeforeUi 插入点执行（世界场景结束后），面板在 Present，所以 FXAA/Bloom/SSAO 不会作用到面板。
- 另有一个 `DXVK_WAR3_IMGUI_BEFORE_UI=1` 的旁路，在 UI 前插入并用状态块包住（`d3d9_device.cpp:17430-17449`），默认关闭。代理模式不需要它。

### 1.3 输入（已核对）

- 进程内窗口过程是 DXVK 的 `D3D9WindowProc`（`d3d9_window.cpp:61-223`），由交换链构造时 `HookWindowProc` 装上（`d3d9_swapchain.cpp:244-245`，只在窗口化时装）。
- Ctrl+F1 切换显隐（`d3d9_window.cpp:108-113`），IME 消息先给游戏（`:115-119`），面板可见时先交给 `ImGui_ImplWin32_WndProcHandler`，ImGui 要鼠标/键盘时吞掉对应消息（`:121-142`）。
- `d3d9_swapchain.cpp:117-169` 有一份内容相同的 `War3WndProcHook`，但 `InstallWar3WndProcHook` 标了 `[[maybe_unused]]`（`:56`），是死代码。
- 静态测试把这两处窗口过程的写法钉住了（`AutoTest/test_device_owned_lifetime_contract.py:198-221`）。

### 1.4 字体与图集（已核对）

- 初始化时 `AddFontFromFileTTF("c:\\windows\\fonts\\msyh.ttc", 18, ..., ChineseFull)`（`war3_imgui.cpp:94-97`）。本机该文件 19,704,352 字节。
  ImGui 1.92 是动态字体：整个 TTF 读进内存并常驻，字形按需栅格化，图集按需长大（推断：标准行为，没有量）。
- 进程内在交换链创建时就初始化（`d3d9_swapchain.cpp:250-253`），也就是**每个玩家都常驻这 19 MiB，不管开不开面板**（推断）。
- 图集纹理由后端 `UpdateTexture` 建：`CreateTexture(w,h,1,D3DUSAGE_DYNAMIC, A8B8G8R8 或 A8R8G8B8, D3DPOOL_DEFAULT)`，失败就 `IM_ASSERT`（`imgui_impl_dx9.cpp:400-405`）。
- 鼠标光标：`SetCursorProperties` 时把位图交给面板（`d3d9_device.cpp:14155-14163`，`war3_imgui.cpp:1484-1524`）；没有位图时画一个三角形箭头（`:1545-1558`）。

### 1.5 设置邮箱与持久化（已核对）

- 面板经 `GetMutableSettings()` 写设置（`war3_imgui.cpp:433`），它拿的是当前发布的邮箱（`war3/war3.cpp:291-294`），写完 `Commit` 递增 `pendingRevision`（`:278-289`）。
- **代理模式已经发布了同一个接口**：`PublishProxyDeviceServices(services, FrameSettingsMailbox())`（`war3.cpp:125-135`，`war3_proxy_services.cpp:116`），
  邮箱在每帧 `FramePresent → ResolveSettings` 时被采用（`war3_proxy_frame.cpp:423-430, 582-596`）。所以面板改的值在代理模式下下一帧就进记录，不用新接线。
- 持久化：`player::RecordPanelEdit/RecordExtrasEdit/FlushIfDue` 写 `WarVK\settings.ini`，关面板时立刻保存（`war3_imgui.cpp:176-181, 1350-1353`）。
  基线在第一次 `ApplyAtStartup` 时取（`war3_player_settings.cpp:845-851`），代理模式的 `CreateMailbox` 也调用了它（`war3_proxy_frame.cpp:85-91`），“恢复默认设置”在代理模式下可用。
- 静态测试钉住了“只有面板记录玩家选择”（`AutoTest/test_player_settings_static.py:139-149`）与面板函数的签名（`test_render_stats_panel_static.py:47`、`test_frame_recorder_console_static.py:33-36`）。

### 1.6 生命周期（已核对）

- `Reset`：入口先 `War3Imgui::invalidateDeviceObjects`（`d3d9_device.cpp:14274`），之后 `restoreDeviceObjects`（`:14369`）。
- 设备析构：先 `beginDeviceTeardown`（`:9022`），GPU 排空后 `shutdown`（`:9050`）。之所以能在析构里收尾，是因为面板资源只持私有引用，不阻止设备析构。
- 图集被后端销毁后 ImGui 下一帧自动重建（`imgui_draw.cpp:2825-2831`），所以“交出资源、下帧再建”是安全的。

---

## 2. 代理模式下为什么没有（已核对）

- `War3Imgui` 整个绑在 `D3D9DeviceEx` 上：`initialize(HWND, D3D9DeviceEx*)`（`war3_imgui.h:20-24`），初始化、绘制、窗口过程都只由交换链/设备/`D3D9WindowProc` 驱动。
  代理模式不建 `D3D9DeviceEx`、不建交换链、不装 `D3D9WindowProc`。
- 数据层在代理模式下**没有任何窗口子类**（`src/d3d9` 里 `GWLP_WNDPROC` 只出现在 `d3d9_window.cpp` 与 `d3d9_swapchain.cpp`）。窗口上只有前端的 `DeviceWindowProc`（`d3d9_proxy.cpp:202`，`hookWindow` `:1274`）。
- `CreateMailbox` 的注释也写明了“保存的 FPS 解锁开关属于覆盖层，代理模式还没有”（`war3_proxy_frame.cpp:81-84`）。

---

## 3. 面板要的 D3D9 调用，代理前端支持吗

上游后端每帧的调用（`imgui_impl_dx9.cpp`）对照前端实现（`d3d9_proxy.cpp`）与宿主（`dxvk_core/d3d9_backend.cpp`）。

| 调用 | 后端用法 | 代理前端 | 结论 |
|---|---|---|---|
| `GetDirect3D` / `GetCreationParameters` | Init 查格式（`:312-316`） | 实现（`GetDirect3D`、`GetCreationParameters`） | 可用（已核对） |
| `GetDisplayMode` | Init 查格式（`:316`） | **未实现**，`E_NOTIMPL` 并打一次“未实现入口”跟踪（`d3d9_unsupported.h:50`） | 结果是 `HasRgbaSupport=false`，走 A8R8G8B8 + CPU 换序，功能无碍；但会打破“193 个未实现入口一次都没响”（已核对） |
| 工厂 `CheckDeviceFormat(DYNAMIC\|QUERY_*)` | Init | 本地直接 `NOTAVAILABLE`（`usage` 不是 0，`d3d9_proxy.cpp:1172` 起） | 无 IPC，无碍（已核对） |
| `CreateVertexBuffer(DYNAMIC\|WRITEONLY, FVF, DEFAULT)` | 首帧/增长 | 只收 DEFAULT + 必须带 WRITEONLY（`MakeBuffer`，`:1544` 起），上限 `MaxBuffer=1 MiB`（`d3d9_wire.h:16-17`） | 可用；顶点数 >约 38,000 时建不出来，后端静默不画那一帧（已核对；顶点数是推断） |
| `CreateIndexBuffer(INDEX16, DEFAULT)` | 同上 | 只收 INDEX16（ImDrawIdx 默认 16 位） | 可用（已核对） |
| `CreateTexture(DYNAMIC, A8R8G8B8, DEFAULT)` | 图集 | **拒绝**：只收 `usage==0 && pool==MANAGED`（`:1567`），且边长 ≤1024（`d3d9_texture_wire.h:10`） | **失败 → `IM_ASSERT` → 进程退出**。产品编译命令没有 `-DNDEBUG`、`b_ndebug=false`（`build32_device_owner_phase52_20261117_a/compile_commands.json`，已核对） |
| `LockRect(0, rect, 0)` / `UnlockRect` | 图集首建与增量 | 托管纹理支持子矩形锁（`MakeManagedLockRegion`）；首次解锁上传整层，之后只传补丁；每块一次同步调用 | 可用（已核对） |
| `CreateStateBlock(D3DSBT_ALL)`/`Capture`/`Apply`/`Release` | 保存/恢复游戏状态 | 本地实现（`d3d9_proxy_stateblock.h:130-155`）；状态块计入 `losable`（`:14-15`），当帧释放无碍 | 可用（已核对） |
| `GetTransform`/`SetTransform`（WORLD/VIEW/PROJ） | 正交投影 | 实现 | 可用（已核对） |
| VB/IB `Lock(DISCARD)`/`Unlock` | 每帧 | 动态缓冲支持 DISCARD；VB 用借来的上传窗口，不常驻 x86 副本；IB 常驻 x86 副本（本地校验索引要读） | 可用（已核对） |
| `SetStreamSource`（步长 24）/`SetIndices`/`SetFVF(XYZ\|DIFFUSE\|TEX1)` | 每帧 | 步长 ≤256；FVF 有声明缓存（`SetFVF` `:1734`） | 可用（已核对） |
| `SetViewport` | 每帧 | 严格校验不得超出后备缓冲；会触发 `followWindow` | 可用；见 §4 的缩放问题（已核对） |
| `SetPixelShader(null)`/`SetVertexShader(null)` | 固定管线 | 实现，走固定管线批 | 可用（已核对） |
| 渲染状态 | `FILLMODE`、`SHADEMODE`、`CLIPPING` | 不在任何表里，返回 `E_NOTIMPL`（`:1816`），不打跟踪；后端不看返回值，默认值正是它要的 | 无碍（已核对） |
| 渲染状态 | `ZENABLE/ZWRITE/CULL/ALPHABLEND/BLENDOP/SRC/DESTBLEND` | `RenderField` | 可用（已核对） |
| 渲染状态 | `ALPHATEST/FOG/SCISSORTEST/SEPARATEALPHABLEND/SRC/DESTBLENDALPHA` | 效果状态表（`d3d9_effects_wire.h:9-14`） | 可用（已核对） |
| 渲染状态 | `LIGHTING/SPECULAR/RANGEFOG` | 固定管线表（`d3d9_ff_state.h:13-17`） | 可用（已核对） |
| `STENCILENABLE=FALSE` | | 只收 FALSE | 可用（已核对） |
| 纹理阶段 0/1、采样器 0（LINEAR、CLAMP） | | `ValidFFStage`、`ValidSamplerValue` | 可用（已核对） |
| `SetScissorRect` | 每条命令 | 实现（`:333`）；宿主按它裁剪（`d3d9_backend.cpp:1867-1874`） | 可用（已核对） |
| `SetTexture(0, 图集)` | 每条命令 | 只收本设备的 `Texture`；绘制前要求已上传、未锁 | 可用（已核对） |
| `DrawIndexedPrimitive(TRIANGLELIST, base, 0, n, start, prim)` | 每条命令 | 本地逐个校验索引（`:1922` 起），`base` 可非零 | 可用；每条命令都过一遍索引，面板几万个索引，CPU 很便宜（推断） |
| `BeginScene`/`EndScene` | `render(false)` 自带 | `EndScene` 会 flush 批次，分组提交时再发一次同步 `FlushDraws`（`d3d9_connection.h:574-577`） | 可用；面板可见时每帧多一次同步与一次 GPU 提交（已核对/推断） |
| `DrawIndexedPrimitiveUP` | 不用 | 未实现 | 无关 |

宿主：执行的是普通固定管线 draw；BeforeUi 效果在标记处执行（`runBeforeUi`，`d3d9_backend.cpp:2072`），宿主 `present()` 只做一次 blit（`:2221-2299`），没有作用于整帧的后处理。**宿主不用改**（已核对）。

**数量限制**：图集要把 `io.Fonts->TexMaxWidth/TexMaxHeight` 设为 1024（ImGui 按它限制增长，`imgui_draw.cpp:4161-4162`；后端写的 `Renderer_TextureMaxWidth=4096` 在这个版本里不用于限制，推断）。
面板源码里不同的中文字符约 502 个（含注释，是上限），18 号字大约需要 512×512 到 1024×512（推断），1024 够用。

---

## 4. 会不会和五回调观察者打架

- **面板的 draw 不会被当成游戏的 draw**：前端在回调里设 `observing`，回调里发出的 draw 不再回调（`d3d9_proxy.cpp:401-411`，观察者头文件 `war3_d3d9_observer.h:9-12` 写明）。
  所以只要**在 `present` 回调里画**，`FrameDraw` 不会看到它们：不会触发 `InsertBeforeUi(false)`、不会发投射体记录、不会被采样相机、`g.draws` 不变（已核对）。
- **反过来，如果在别处画（窗口过程里、Game.dll 钩子里）就会被观察**：在世界场景内会被当投射体候选，在世界之后会触发 BeforeUi 插入。所以只能在 `present` 回调里画（推断，结论确定）。
- **与 BeforeUi 的先后**：`ObservePresent` 先调 `FramePresent`，它在本帧还没插过 BeforeUi 时于 Present 补插（`war3_proxy_services.cpp:133-136`，`war3_proxy_frame.cpp:582-584`）。
  面板必须画在 `FramePresent` **之后**，否则在“世界之后没有 UI draw”的帧里面板会落在 BeforeUi 之前、被 AA/Bloom 处理（已核对顺序，后果推断）。
- **与控制面截帧**：进程内截帧在面板之前。代理模式照做：画在 `ProcessPendingFrameCapture`（`war3_proxy_services.cpp:137-144`）之后。
  前端的帧转储 `dumpFrame()` 在回调之后、`presentFrame` 之前（`d3d9_proxy.cpp:380-383`），**会包含面板**，探针可以用它看面板（已核对）。
- **世界光照改写**：前端只在 `worldLighting.active` 时改写固定管线灯光；数据层在世界场景退出时清掉；而且面板设 `LIGHTING=FALSE`，不受影响（已核对）。
- **缩放通知被吞（真问题）**：回调里的 `BeginScene/SetViewport/SetScissorRect/CreateStateBlock/Capture/Apply/GetBackBuffer` 都会调 `followWindow`（`:476-479`），
  它会执行 `resizeToWindow`，但 `resizeToWindow` 里 `if(observer.resized&&!observing)`（`:1521`）把通知跳过了 → 数据层的 `ObserveResized` 不跑 → 游戏的分辨率字段不改写。
  另外缩放发生在 `presentFrame` 之前，这一帧游戏画的内容随旧后备缓冲丢掉，只剩面板（闪一帧）。
  这个问题今天就潜在存在（截帧请求在回调里调 `GetBackBuffer`），面板会让它常见（已核对代码路径，后果推断）。

---

## 5. 三条路线

| | (a) 数据层经代理设备画（推荐） | (b) 绘制列表作为记录送宿主，宿主 Vulkan 画 | (c) 叠一个分层窗口 |
|---|---|---|---|
| 正确性风险 | 游戏状态泄漏：上游后端用 `D3DSBT_ALL` 状态块 + 手存变换恢复，代理状态块完整（已核对）；没设的状态（`COLORWRITEENABLE`、阶段 0 的 `TEXCOORDINDEX/TEXTURETRANSFORMFLAGS`）沿用游戏留下的值，进程内同样如此（推断，低）。生命周期/Reset/缩放三个坑要补（§0） | 不碰游戏的 D3D9 状态；资源归宿主，没有 Reset/引用问题；但要定义“面板在 BeforeUi 之后、Present blit 之前”在宿主里的位置，以及会话重置时内容的去留 | 焦点与激活（点面板会让游戏失活）、置顶与无边框全屏的叠放、DPI/多屏、跟随窗口移动、截图不含面板、隔离桌面上的合成。问题多且都在产品形态上 |
| 与宿主效果的先后 | 画在 `FramePresent` 之后，天然在 BeforeUi 之后（§4） | 宿主自己排，可控 | 不在同一张图里 |
| x86 内存 | 首次打开前 0；打开约 20–28 MiB：TTF 18.8 MiB、图集 CPU 像素 ≤4 MiB、前端图集副本 ≤4 MiB（产品档位下可被逐出）、IB 副本 0.1–0.2 MiB、ImGui 上下文 <1 MiB（推断） | 几乎一样：ImGui 与 TTF 必须留在 x86（字形在 x86 栅格化、文本排版在 x86），只省前端的图集副本 | 视用什么画：x86 里起 D3D11 会再加载驱动 UMD，几十 MiB 地址空间，和迁移目标相反 |
| 工作量 | 前端 3–4 处小改（几十行）、生成器加变体、数据层约 300–400 行、测试；约 700–900 行，2–3 天（推断） | 新帧记录类型（每帧最多约 1 MiB 的顶点/索引，超出现有记录大小，要走内容通道）、图集作为内容、宿主执行器（着色器、管线、裁剪、混合）、会话重置；还得保留 (a) 的输入部分 | 大，且大部分是窗口管理的坑 |
| 碰不碰传输语义 | 不碰：没有新的环消息、线格式、宿主代码。只改前端与数据层之间的进程内 ABI（观察者结构加一个回调、两个导出） | 碰：新记录、内容生命周期、Present 处的排序 | 不碰传输，但另起一套 |
| 谁能做 | **Opus** | Fable | 不建议做 |

还考虑过、没选的：
- **前端自带面板渲染器**（前端拥有面板的 VB/IB/图集，像 `upBuffer` 一样算“设备自己的”）：能顺带解决 Reset 与引用问题，但要在前端再写一个渲染器，违背“只用钉死的上游后端”的做法（`prepare_warvk_imgui_dx9.py` 文首），而且导出面更大。
- **整个 ImGui 搬到 x64 宿主**：能省下 x86 那 20 MiB，但面板读写的大量 x86 状态（设置邮箱、玩家设置文件、`War3RenderState`、着色器包、点光、帧取证）都得双向同步，是很大的传输工作。若以后 20 MiB 变得要紧再议。

---

## 6. 推荐做法与实施步骤（Opus 可照做）

### 6.1 前端（`tools/render_host/d3d9_proxy.cpp`、`d3d9_proxy.def`、`src/d3d9/war3/core/war3_d3d9_observer.h`）

都是 x86 进程内的改动，不碰环与宿主。这个文件正被帧时间工作修改，先合并再动。

- **F1 回调里不做缩放**：`followWindow()` 在 `observing` 时直接返回（`d3d9_proxy.cpp:476`）。缩放仍由 `Present` 末尾那次 `followWindow`（`:384`）执行，`resized` 回调照常发出，
  与今天没有面板时的顺序一致。观察者头文件补一句“回调内不执行窗口缩放”。
  代价：缩放挂起时面板那一帧按旧后备缓冲画；`SetViewport` 若宽高超出会被拒（面板那帧错位一帧）。数据层可以在 `ImGui_ImplWin32_NewFrame` 之后、`ImGui::NewFrame` 之前把 `io.DisplaySize` 改成后备缓冲尺寸（`GetBackBuffer→GetDesc`，本地调用）避免。
- **F2 观察者加 `reset`**：`WarVKD3D9Observer` 末尾加 `void (__stdcall* reset)(void* user, IDirect3DDevice9* device);`，
  在 `Device::Reset` 里 `isOwner` 检查之后、`losable` 检查（`:1463`）之前、`Observing` 作用域内调用。语义同进程内 `D3D9DeviceEx::Reset` 入口的 `invalidateDeviceObjects`。
  结构体大小变了，前端与产品 DLL 必须一起重建（`setObserver` 严格比较 `size`）。
- **F3 两个导出：私有引用**：`WarVKD3D9RetainPrivate(IDirect3DDevice9*, IUnknown*)` 与 `WarVKD3D9ReleasePrivate(IDirect3DDevice9*, IUnknown*)`，
  只接受本设备的 `VertexBuffer/IndexBuffer/Texture`，只在所有者线程。前者做 `AddRefPrivate(); Release();`（与 `war3_imgui_dx9_ownership.h:12-20` 同义；
  `Child::Release` 在公有计数归零时释放对设备的公有引用，`d3d9_proxy.cpp:711` 起），后者做 `ReleasePrivate()`。
  这样面板的资源不再拖住设备，游戏最后一次 `Release` 时 `~Device` 照常运行，`released` 回调（`~Device` 第一行，`:1343` 起）里数据层先放掉私有引用，资源析构时设备还活着。
  `.def` 加两行；`war3_proxy_mode.cpp` 的 `FrontEnd` 解析它们（`:79-85` 那串 `Resolve`）。
- **F4（可选，建议做）`GetDisplayMode(0)`**：用工厂的 `GetAdapterDisplayMode(0)` 回答，避免后端 Init 打出“未实现入口”跟踪。

### 6.2 生成的代理版后端（`AutoTest/prepare_warvk_imgui_dx9.py`、`src/d3d9/meson.build`）

- 生成器加 `--variant proxy`：同一份上游、同样的哈希检查、同样 10 个寿命点，只换前缀 `War3ProxyImGui_ImplDX9_`、命名空间 `dxvk::war3::imgui_proxy::`、横幅与包含的头。
  第二个 `custom_target` 输出 `war3_imgui_dx9_proxy_backend.cpp`，加进 `d3d9_dll` 的源。**进程内那份生成文件字节不变**（验收项）。
- 新头 `war3/ui/war3_imgui_dx9_proxy.h`（入口声明）与 `war3/ui/war3_imgui_dx9_proxy_ownership.h`：
  - `CreateVertexBuffer/CreateIndexBuffer`：原样创建，成功后经 F3 转私有引用；
  - `CreateTexture`：**忽略传入的 usage/pool，改用 `0` + `D3DPOOL_MANAGED`**，成功后转私有；这一处就解决了图集断言，不需要新的编辑点；
  - `Release*`：经 F3 释放私有引用。
  - 导出经 `war3_proxy_mode.{h,cpp}` 的包装函数调用（产品 DLL 不能直接链接前端导出）。
- 两份生成文件里同名的 `struct ImGui_ImplDX9_Data`/`CUSTOMVERTEX` 定义相同，静态函数各在各的翻译单元，不冲突（推断）。
- `run_x86_data_layer_cut_gate.py:61-62` 的 `classify` 要加新生成对象名 → `imgui`，否则门以“未分类对象”失败；`test_x86_data_layer_cut_gate.py` 的用例同步。

### 6.3 `War3Imgui` 去掉对具体设备的直接调用（`war3/ui/war3_imgui.{h,cpp}`）

数据层一调用 `War3Imgui`，切口门就会把 `war3_imgui.cpp`、ImGui 库、生成的后端拉进数据集（`ui`、`imgui` 都是可生长层，`run_x86_data_layer_cut_gate.py:51`）。
现在它们**不在**数据集里（`AutoTest/artifacts/x86-data-layer-cut-phase52-20261117-r1/receipt.json` 的 `dataSet`，已核对），`war3_material/shader_manager/shader_api` 已经在。
被拉进来后，下面这些直接引用会成为新的“正向”符号，`--ratchet` 会失败（推断，要以门的结果为准）：

- `m_device->BeginScene/EndScene/CreateTexture`：`D3D9DeviceEx` 是 `final`，调用被去虚化成对 `d3d9_device.cpp` 的直接引用（`war3_execution_services.h:17-19` 的说明）→ 改成持有 `IDirect3DDevice9*` 来画。
- `pipeline->IsPassEnabled/SetPassEnabled`（`war3_imgui.cpp:1002-1023`，效果层）→ 给 `IDeviceServices` 加两个成员（代理模式答“不可用”，面板隐藏这棵树），或代理模式不显示并把调用移到设备层回调里。
- `QueryShadowTaaDiagnostics`（`:814`）、`QueryCsmResolutionDiagnostics`（`:830, :1377`）、`QueryShadowReplayDiagnostics`（`:1375`）、`QueryWar3WeatherDiagnostics/VolumeDiagnostics`（`:1378-1379`）
  → 改用 `ProcessServices()` 上已有的同名成员（`war3_execution_services.h:127-136`），代理模式返回值初始化的“不可用”。
- 光标纹理 `Com<D3D9Texture2D>` → `Com<IDirect3DTexture9>`（只有进程内会用到）。

结构上做加法，少动现有签名（静态测试钉了 `initialize(HWND, D3D9DeviceEx*)`、`shutdown(D3D9DeviceEx* owner)` 等）：

- 新增 `initializeProxy(HWND, IDirect3DDevice9*)`、`shutdownProxy(IDirect3DDevice9*)`、`invalidateProxy(...)`；内部用一个小的后端函数表在 `War3ImGui_ImplDX9_*` 与 `War3ProxyImGui_ImplDX9_*` 之间选。
- 代理模式：初始化后 `io.Fonts->TexMaxWidth = io.Fonts->TexMaxHeight = 1024`；每帧画之前 `TestCooperativeLevel()==D3D_OK` 才画（本地调用，避免设备丢失时建纹理失败触发断言）。
- 代理模式下面板里：“突破帧率上限”禁用并说明（呈现间隔由建设备时决定，运行中切换要前端支持，另立项）；“模块热插拔”与“渲染统计”显示“宿主执行 / 宿主统计未接入”；“编译”按钮禁用（它要 `D3D9DeviceEx*`）。
- 可选加固：代理模式在调用后端前用一个外层状态块包住，并显式设 `COLORWRITEENABLE=0xF`、阶段 0 `TEXCOORDINDEX=0`、`TEXTURETRANSFORMFLAGS=DISABLE`（推断的低风险项）。

### 6.4 代理模式的接线（建议新文件 `war3/platform/war3_proxy_overlay.{h,cpp}`，属数据层）

- **何时可用**：与进程内同一开关（`HookUi` 运行时模块启用、不是只解锁帧率的兼容模式，`d3d9_swapchain.cpp:250-252`）。
- **窗口子类**：在 `ConnectCreatedDevice` 之后（前端已挂好它的子类）装到设备窗口上（`GetCreationParameters().hFocusWindow`）。
  逻辑照 `d3d9_window.cpp:108-142`：Ctrl+F1 切换；IME 先给下一级；可见时交 `handleWndProc`，要鼠标/键盘时吞掉；其余一律交给前端的 `DeviceWindowProc`（它负责吞 `WM_SIZE`）。
  卸载在 `released` 回调里、前端 `unhookWindow` 之前（`~Device` 先回调再卸前端子类），只在自己仍是当前过程时恢复，否则留一个只转发的桩（前端也是这么处理后来者的，`:1331` 起）。
- **延迟初始化、关闭即释放**：Ctrl+F1 只改“想要可见”的标志；真正的 `initializeProxy` / 整套释放放到下一次 `present` 回调里做，保证所有设备调用都在回调里、在所有者线程上。
  第一次打开前不读字体（x86 0 字节）；关闭时 `shutdownProxy`（`FlushIfDue(true)` 会先保存设置）。重新打开要重读 19 MiB 字体与重建图集，算一次小卡顿（推断）。
- **画**：`ObservePresent` 末尾，即 `FramePresent` 与截帧之后：`newFrame → render(false) → endFrame`。
- **回调**：`reset`（F2）→ `invalidateProxy`；`released` → 先 `shutdownProxy`、卸窗口子类，再 `DisconnectDevice`；`terminal` 不用管（进程随后结束）。
- **私有测试消息**：仿 `frame_history.test_shortcut`（`war3_internal_test_api.cpp:383-394`）走真实窗口路由：测试命令向窗口 `SendMessage` 一个 `WM_APP+x` 私有消息，由子类当作 Ctrl+F1 处理。

### 6.5 测试命令与探针

- 测试命令（`war3_internal_test_api.cpp`）：`ui.panel_open`、`ui.panel_close`、`ui.panel_status`。探针的 `test=` 动作只能发空负载（`run_x64_game_proxy_probe.py:348-349`），所以开/关拆成两个命令。
  `ui.panel_status` 回：已初始化、可见、已画帧数、上一帧 draw 数、图集宽高、建纹理/缓冲失败次数、最后一次 HRESULT、`WantCaptureMouse`、经子类转发的输入消息数、模式（进程内/代理）。
  两种模式都实现，进程内对照臂也能用。
- 探针：`TEST_COMMANDS`（`:327-328`）加这三个；`--dump-every` 调小（例如 60）让开面板前后都有转储；`summarize` 对开面板前后的转储在面板默认位置（ImGui 新窗口默认在 (60,60)，`imgui.cpp:6566`，已核对；但沙盒实例目录里若有旧的 `imgui.ini` 会覆盖位置）取一块比较。

---

## 7. 测试与验收

### 7.1 静态测试（新增 `AutoTest/test_proxy_overlay_static.py`，或并入 `test_proxy_mode_lifecycle_static.py`）

- 前端：`followWindow` 在 `observing` 时返回；`Reset` 在 `losable` 检查之前调用 `observer.reset`；两个私有引用导出只认本设备的三种子对象且查所有者线程；`.def` 有两个新导出。
- 观察者头：`reset` 成员在结构体末尾；数据层 `ConnectCreatedDevice` 填了它。
- 数据层：面板只在 `ObservePresent` 里、`FramePresent(device)` 之后、`HasPendingFrameCaptureRequest` 块之后画；`OnReleased` 里先 `shutdownProxy` 再 `DisconnectDevice`；
  代理版所有权头的 `CreateTexture` 用 `D3DPOOL_MANAGED` 且 usage 为 0；`TexMaxWidth/TexMaxHeight` 设为 1024。
- 生成器：`test_warvk_imgui_dx9_adapter.py` 扩展到两个变体（同哈希、同 10 处、前缀各自唯一、进程内输出不变）。
- 切口门分类：`test_x86_data_layer_cut_gate.py` 加新生成对象。
- 现有钉子都要继续过：`test_player_settings_static`、`test_device_owned_lifetime_contract`、`test_render_stats_panel_static`、`test_frame_recorder_console_static`、`test_war3_pipeline_lifetime_static`。

### 7.2 门

- x86 冻结构建（产品 DLL + 前端 DLL 都要重建；宿主不用）。
- 切口门 `--ratchet`，以 `x86-data-layer-cut-phase52-20261117-r1` 为基线：**正向仍是 19 个符号、24 条边**，不许增加；反向可以增加。
- 进程内回归：设备所有权寿命门通过，**进程内生成的 `war3_imgui_dx9_backend.cpp` 哈希不变**；阴影门、特效门的帧哈希与 5.2 相同（面板默认隐藏，画面不应有任何差别）。

### 7.3 探针（`run_x64_game_proxy_probe.py`，产品档位，Lost Temple）

- 第一轮：`20:test=ui.panel_open, 22:test=ui.panel_status, 24:click=100x70, 26:test=ui.panel_status, 30:size=1280x720, 34:test=ui.panel_status, 40:test=ui.panel_close`，`--dump-every 60`。
- 第二轮：`20:test=ui.panel_open`，**不关面板**就退出（验证 `released` 路径）。
- 第三轮：不开面板，作为内存与画面的对照。

验收：
1. `ui.panel_status`：已初始化、可见、已画帧数 >0、draw 数 >0、建纹理/缓冲失败 0、图集边长 ≤1024。
2. 开面板后的转储在面板区域明显变暗（ImGui 暗色主题的窗口底色），开之前没有；人看一张 PNG 确认中文正常显示。
3. 点击后 `WantCaptureMouse`/转发计数变化，游戏没有收到这次点击（例如没有选中单位）。
4. 缩放后数据层日志有 `window resized ... gameFields=1`（没有被吞），布局正确。
5. 宿主 0 拒绝；BeforeUi 插入次数、投射体记录数在开面板的帧里与不开时同量级（面板的 draw 没有混进记录）。
6. `game.stderr` 没有新的“未实现入口”首次调用跟踪。
7. 第二轮带着面板退出：日志里先有覆盖层释放、再有 `device released`；前端析构打印了 `D3D9 backing-proxy`（说明 `~Device` 跑了）；宿主正常收尾；进程自己退出。
8. 内存：第三轮的 x86 专用字节与 5.2 同图同档位在噪声内；第一轮开面板期间多约 20–28 MiB，关闭后回落。
9. 帧时间：面板关闭时与第三轮无差别；打开时允许变慢，记下数字即可（不设门槛）。

### 7.4 自动化不到、试用前要人做的

- 输入桌面上手动：Ctrl+F1 开关、拖动窗口、勾选“光影”/改 AA 模式后画面变化、重启游戏后设置保留（`WarVK\settings.ini`）。隔离桌面上窗口永远不活动，ImGui 的鼠标位置更新依赖前台（推断），自动化只能证明“消息到了”。
- 游戏选项里改分辨率（触发游戏自己的 `Reset`），面板开着和关着各一次：游戏能回来。探针触发不了游戏的 `Reset`。

---

## 给计划的结论

- 走路线 (a)，**宿主零改动、线协议零改动，Opus 可以做**；不需要排到 Fable 的传输工作里。
- 设计里“代理已有的调用就够”要改成：**代理缺 5 样东西**——图集纹理的池/用法、1024 上限、面板资源拖住设备、`Reset` 时的 `losable`、回调里吞缩放通知。
  前三样数据层自己能兜（代理版生成后端 + 设图集上限），后两样各要前端一处小改（`followWindow` 在回调内不动、观察者加 `reset`），资源寿命要前端两个私有引用导出。
- 面板第一次打开前 x86 不多花一个字节；打开约 20–28 MiB，关掉还回去。路线 (b) 省不下这笔钱，不值得为它动传输。
- 要钉住的顺序：面板只在 `present` 回调里、`FramePresent` 与截帧之后画。
- 切口门 `--ratchet` 是这项工作的硬约束：`war3_imgui.cpp` 会第一次进入数据集，它对设备和效果的直接调用要先改走 `IDirect3DDevice9*`、`IDeviceServices`、`ProcessServices()`。
- 顺序建议：前端 F1–F4 与静态测试 → 生成器变体 → `War3Imgui` 去具体设备（先跑进程内回归与切口门）→ 代理接线与测试命令 → 探针三轮 → 输入桌面手测。
- 顺带发现（不在本项范围）：进程内模式在交换链创建时就加载 19 MiB 的字体，不开面板也常驻；改成首次打开才加载是一笔便宜的 x86 节省。

## 还不知道的

- 切口门实际会多出哪些正向符号：要构建后跑门才知道（我只能按源码推）。
- ImGui 1.92 的 `Renderer_TextureMaxWidth` 在这个版本里有没有别的用途；面板全部展开时图集实际多大、顶点数会不会超过 1 MiB 缓冲（约 38,000 个顶点）。
- 游戏在玩家改分辨率时是调 `Reset` 还是销毁重建设备；后者在 F3 之前会让面板资源拖住旧设备（连同一个宿主进程）。
- 隔离桌面上 `ImGui_ImplWin32` 对非前台窗口的鼠标处理细节，决定探针里的点击能证明到哪一步。
- 帧时间工作正在改 `d3d9_proxy.cpp` 的同步语义（`ApiScope`），面板路径上的同步调用（建/毁资源、纹理分块上传、`EndScene` 的 `FlushDraws`）在那之后的代价要重新看。
- 游戏在代理模式下是否画软件光标；面板要鼠标时画的三角箭头会不会与游戏光标重叠（进程内同样的行为，没有对照过）。
- 代理模式下“突破帧率上限”怎么在运行中切换（要前端在 `Reset` 或新导出里改呈现间隔），本笔记只建议先禁用。
