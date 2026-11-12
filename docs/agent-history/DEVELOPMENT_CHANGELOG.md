# WarVK v1.22 Development Changelog

本账本记录集成候选、验证和未通过项。根 CHANGELOG 的旧版本历史保持不变；
1.22正式范围见 `docs/RELEASE_1.22.00.md`；带DRAFT的旧文档仅供追溯，不覆盖后续验收与发布决定。

## 2026-11-12：1.22.03 光影修复与发布（Claude）

起因：用户要求把面板设置保存作为小版本发布，并修复三项玩家反馈：头像被光影带偏、暂停时阴影/体积光
盖住界面、体积光阴影分辨率低且不够明显。排查体积光时另外发现对战图上阴影会话被反复重置。

- **头像。** `SetLight(0)`/`SetMaterial`/`D3DRS_AMBIENT` 的改写原先不分场景。设备现在保存游戏原值，
  由 `Hook_WorldRenderScene` 进入/返回切换作用域（`war3_world_lighting_scope.h`），只有世界内套用改写。
  钩子从未触发时为 Unscoped，保持旧行为。
- **暂停。** 逐帧阶段序列（新增 `DXVK_WAR3_FRAME_SEQUENCE_TRACE=1`）显示真实 F10 暂停帧的世界阶段以
  `a5 != 0` 分发且没有 15/18/21 阶段，旧分界永不成立，只剩帧末兜底。先试过“只禁止帧末兜底”，结果暂停时
  世界阴影全部消失，改为新增第 10 层分界（世界场景返回后的第一次绘制，`war3_before_ui_boundary.h`）。
  帧末兜底的守卫最初用 `uiDispatchSeen`，但它在每个对局帧里都为真（UiDispatch 包着世界场景），会误伤
  隐藏界面的帧，改为“世界之后是否有过绘制”。测试接口 `game.pause` 不复现该问题，必须用真实按键并把
  测试工具默认的 `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE=1` 改为 0。
- **换图误判。** 对照截图时发现新版偶尔整帧没有阴影。计数显示 1.22.02 发布 DLL 在 (2)EchoIsles 上同样
  持续整帧拒绝，加日志后确认来源是 `main-runner` 入口：1.27a 每次恢复 JASS 线程都会进入它。
  `War3MapTransitionGate::onMainRunnerEnter(worldLive)` 在世界仍在渲染（本帧或上一帧完成世界场景）时
  直接返回，不消耗也不解除兜底。Native 表重建与退出对局两条信号不变。
- **体积光。** 阴影图 1536→2048、近层比例 0.42→0.30；引导图分档抽成
  `War3SelectDirectionalGuideDivisor`（比较改为 `w*h <= budget/depth`，避免 2^32 级尺寸溢出）。
  A/B 截图显示默认值下效果以均匀雾化为主（+30 级），受光/遮挡只差约 12 级；提高密度只增加雾化。
  因此保持密度，强度 1.2→1.5、对比 2.1→2.5（可读性混合取满）、压暗上限 0.24→0.36。防 TDR 的
  工作量上限未动。

验证见 `docs/RELEASE_1.22.03.md`：GCC 正式 DLL `54C4642E…AD73`，meson 122/122，静态 287/287，
专项检查与 5 个实机门均在该 DLL 上完成。保留的失败回执：只禁帧末兜底的方案（暂停时无阴影）、
冻结静态首轮 282/287（断言构建尚未完成）、换图门首次因远程会话断开被测试工具拒绝启动。

## 2026-11-12：Ctrl+F1 面板设置自动保存（Claude）

起因：玩家在群里问面板改的设置会不会保存。代码确认原先只改内存，重启游戏后恢复默认。用户要求“加保存”。

- 新模块 `war3/platform/war3_player_settings.*`，玩家的选择写入 `WarVK\settings.ini`。手写的 `warvk.ini` 不碰，
  因为 Profile API 会按 ANSI 重写整份文件，可能把玩家用 UTF-8 写的中文弄坏。新文件只含 ASCII 键值，
  另带 UTF-8 BOM 的中文注释。
- 保存范围：面板“内置渲染设置”里能改的 139 个字段，加上“突破帧率上限”和“原生阴影”两项。只记玩家改过的项；
  调试输出、Stage/分组过滤、模块热插拔和测试点光源不保存。读入时按面板自己的范围限幅，CSM/点阴影分辨率
  取最近的 2 的幂。值不可用的键会被丢弃并记日志；不认识的键原样保留，留给新版本使用。
- 记录方式：面板在同一个设置写守卫内比较本帧前后的差异，所以地图/JAPI 的写入不会被记下。点击
  “调试：强制所有对象描边”或“恢复默认设置”的那一帧不记录。停手 1.5 s 后保存；关闭面板或设备拆除时立即保存。
  写入方式是先写 `.tmp` 再 `MoveFileEx` 替换；写失败时面板显示“保存失败”，不会每帧重试。
- 生效顺序：版本默认值 → 显存档位 → 已保存的设置 → 诊断环境变量 → 设置邮箱播种。玩家选过的值优先于自动档位。
  “恢复默认设置”把已保存的键恢复为首次构造管线时的基线（已经过显存档位调整），再删除文件。

验证：
- 用 clang 增量构建 `build32_startup_diag_20261111`（b_ndebug=false），DLL `B8B40792…1A61`。
- meson 121/121，含新增的 `player_settings`（571 项，GCC 单独编译运行也通过）。
- 静态脚本 286/286，含新增的 `test_player_settings_static.py`，它会核对面板每个可改字段都有保存键；
  其中 5 个依赖构建产物的脚本通过 `WARVK_CPU_TEST_BUILD_DIR` 指向本次构建。
- GCC 15.2 对 3 个改动文件做 `-fsyntax-only` 检查，无错误、无新警告。

隔离沙盒冒烟 `player-settings-smoke-20261112-r1`：
- 预置文件（2 个有效键、1 个坏值、1 个未知键）时，日志为 `applied=2 unknown=1 rejected=1 [Shadows.Strength]`，
  文件在运行中和退出后哈希都不变。
- 没有文件时没有加载日志，也没有生成文件。
- 玩家 DLL 仍为 `1FFE0BD1…35CF`。

未验证：
- 没有在游戏里实际拖动面板控件（隔离桌面不发送全局输入），只由单测、静态测试和代码审查覆盖。
- 日志里没有 CSM 实际分辨率，2048 的实际效果没有单独观察。
- 未提交、未部署、未发布。

## 2026-11-11：1.22.02 发布（Claude）

用户授权打包发布一次小版本，并要求 64 位迁移内容保留在源码中。正式配置里 64 位渲染宿主与全部诊断选项都关闭。

- 已发布：main 与附注标签 `v1.22.02` 指向 a5717bf；GitHub Release 441866256 为 latest。
- 收入：启动失败对话框与 `startup-error.txt`、显卡/驱动下限与显存档位、锁定/解锁脚本、Storm 低地址与每线程表、
  换图阴影 Reset、单位 Walk 控制。天气预览也收入，但默认关闭。
- 发布前审查新增三项修复：
  - 地图模块加载器改为默认拒绝；
  - Storm 低地址域准备失败时只放弃该域；
  - GCC/mingw emutls 在线程退出时先释放 TLS 再运行 `thread_local` 析构，因此新增 `ThreadLocalLazyArray`，4 处改用。
- 验证：断言开启 meson 120/120、静态 285/285、配置审计通过。同一 DLL 在隔离实机跑了 5 个门：
  - 启动诊断、天气、Walk 与高压 184 s 均通过；
  - 换图 A→B→A 的严格分析器返回 exit 2，原因是 Reset 请求合并后序号每次加 2，按稳定 epoch 重新分析通过。
- 发布 DLL 是 strip 后的副本，所有可加载段与做过实机门的未剥离 DLL 逐字节相同。
- 用户反馈的一款 YDWEConfig 启动器写死了 `-opengl`。沙盒复现确认此时游戏不加载 `d3d9.dll`，已作为已知边界写入 README。

未验证：玩家实机、AMD/Intel 实机、自然退出路径、S2 三条路径的专项测试。玩家现场 DLL 未动。
详见[发布范围](../RELEASE_1.22.02.md)与[发布回执](2026-11-11-v12202-release-publication.md)。

## 2026-10-31：x64 增强层拆分 S2 第一组：只用 D3D9 API 的子系统改经接口调用（Claude）

`D3D9DeviceEx` 是 `final`，经具体类的 COM 调用会被去虚化成对 `d3d9_device.cpp` 的链接引用。
本组把四个子系统的函数签名改成接收 `IDirect3DDevice9*`，执行的仍是同一个函数：
- 队伍色管理器：`Initialize` 和成员；
- 着色器管理器：`applyGlobalUniforms`；
- 材质：`apply`、`applyConstantStore`；
- 帧截图：`ProcessPendingFrameCapture`、`CaptureSurfaceToBmp`。

实例化缓冲（私有引用持有具体顶点缓冲，关系到 Reset 的 losable 计数）和材质编译（要用 DXVK 设备）没有动。

验收：
- 全新 x86 构建通过。
- 切口门以 r2 为基线、开 ratchet 通过：正向 113 → 104 个符号、49 → 46 条边，新增 0；反向不变。
- 两个静态测试共 45 项通过。
- 用新 DLL 作 fork 参照，GPU 门 r2 通过：主路径哈希仍是 03f7265a，resize 两组哈希与 G7 相同。
- 保留失败回执 r1：x64 宿主的构建回执覆盖了 `src/`，改源码后报 host source drift；已重建宿主。

未接游戏，未部署、提交、推送或发布。详见 [S2 回执](../research/2026-10-31-x64-s2-forward-cut.md)。

## 2026-10-31：x64 增强层拆分 S1 第七组：G7 窗口自己改尺寸（WM_SIZE）（Claude）

游戏改窗口尺寸时不调 Reset：fork 的窗口过程截获 WM_SIZE，自己调 `ResetSwapChain`。开发代理现在照做：
- 每种 profile 都挂设备窗口的窗口过程。窗口线程收到 WM_SIZE 只记下客户区尺寸，消息不转给应用。
- owner 线程在下一次与尺寸有关的调用里接手（场景外；Present 在调用之后接手），发带 `ResetKeepResources` 的 Reset op。
  宿主只跳过"DEFAULT 池缓冲须已释放"这一项检查。
- 代理换上新的后台缓冲，视口和剪裁矩形覆盖新目标，其余状态、绑定和缓冲都保留，与 fork 一致。
- 新增 `D3D9 window-proxy:` 行；`reset-host` 行新增 `windowResets`。
- 夹具新增 `--resize`（13 项保留/替换检查），门新增 `--resize`：game 组 1280×720 → 1902×963，a8 组 257×129 → 64×64。

897A416E 宿主 fresh BelowNormal/-j2 的结果：
- CPU 门通过：210,915 项 CPU 检查，wire 1,455,523 项。
- VVL GPU 门 r2 通过：resize 两组离屏用例与 fork 原版逐像素一致，stream + 固定功能批也一致；主路径 11 个哈希仍是 03f7265a，
  尺寸、reset、ring 哈希不变，VVL 0 错误。
- 资源寿命门、M0 内存门通过；旧呈现门仍是计数过期，计数与 G6 相同。
- 保留失败回执 GPU r1：resize-present-game 的宿主正常返回后超过 5 秒才退出，同一构建重跑通过；同一现象在 §7 出现过一次，原因未明。

War3 分辨率字段的改写（S2，留在 x86）和 fork 窗口过程的其他行为没有搬。
未接游戏，未部署、提交、推送或发布。详见 [S1 回执 §11](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-31：x64 增强层拆分 S1 第六组：G6 有序状态与缓冲内容不等回复（Claude）

异步批次档位下，UploadBuffer、SetEffects、SetSamplerState、SetTextureStages 经新的 `Connection::enqueueOrdered` 直接进有序队列，
不再等宿主回复；载荷在返回前复制，宿主拒绝是终止性的。NOOVERWRITE 内容按 Lock 约定可以越过暂存的 draw，
DISCARD、普通 Lock 和状态先刷出暂存 draw。代理的 Unlock 分块与 draw 时的三种状态同步都改走 `Device::ordered`；
同步档位不变。连接记下正在等回复的 `call` 序号，`call` 发出的有序状态被拒仍可恢复（原有故障用例的约定）。
新增 `D3D9 ordered-async:` 计数行。

CPU 测试 `asyncOrderedState`（顺序、私有副本、四种终止回复、`call` 的可恢复拒绝）；门的 ring 用例按 fixture 顺序独立算出批的切分。
CB922267 宿主 fresh BelowNormal/-j2：CPU 门（210,915 项）、VVL GPU 门 r3（主路径 11 个哈希仍是 03f7265a，尺寸、reset、ring 哈希不变，
5 个原始协议故障用例通过，VVL 0 错误）、资源寿命门、M0 内存门通过；旧呈现门仍是计数过期。
ring stream + 固定功能批：211 次上传、3 次状态同步都不等回复，批数 64 → 20、最大一批 1 → 6 个 draw。
保留失败回执：GPU r1（门漏算纹理上传）、r2（第一版把 `call` 发出的状态拒绝也变成终止）。没有测时间（机器负载噪声）。
未接游戏、未部署、提交、推送、发布。详见 [S1 回执 §10](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-31：x64 增强层拆分 S1 第五组：G2b DEPTHBIAS 与按 FVF 缓存声明（Claude）

DEPTHBIAS（渲染状态 195，游戏设置的渲染状态里远端唯一缺的）作为 effects 包的第 16 个键，要求有限 float，默认 0；
代理存取、按像素状态进状态块、ResetState 清零，Caps 加 `D3DPRASTERCAPS_DEPTHBIAS`。宿主照原版按
`VK_EXT_depth_bias_control` 选表示方式，系数取 D32_SFLOAT 目标的 R 值，每个 draw 设常数偏移。SLOPESCALEDEPTHBIAS 游戏不用，不携带。
代理照原版 `m_fvfTable` 按 FVF 保留声明，切 FVF 时直接绑定；开发宿主只有 4 个声明槽位，创建失败时先放掉未绑定的缓存声明再重试一次。

extent fixture 新增 `--depth-bias`（同深度四条竖带：零偏移 LESS 失败、负偏移 LESS 通过、正偏移 LESS 失败而 GREATER 通过），
ring 模式加"同一 FVF 取回同一声明"检查。C00F4CA7 宿主 fresh BelowNormal/-j2：CPU 门（wire 1,455,520 项）、
VVL GPU 门 r3（主路径 11 个哈希仍是 03f7265a，尺寸与 reset 哈希不变，4 个 ring 用例 `b491e0cf`，宿主声明 3 个，VVL 0 错误）、
资源寿命门、M0 内存门通过；旧呈现门仍是计数过期。保留失败回执：GPU r1（缓存占满 4 个声明槽位，主 fixture 的多纹理阶段 SetFVF 失败）、
r2（代理改动后未重新构建，门按源码漂移拒绝）。未接游戏、未部署、提交、推送、发布。详见 [S1 回执 §9](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-30：x64 增强层拆分 S1 第四组：G5 动态 VB/IB 的 Lock 模式（Claude）

新增 x86 缓冲普查 `warvk-d3d9-buffer-census/v1`（只在普查构建里，按返回地址分调用方），游戏 1951 帧的结果：
Game.dll 只用三个 DEFAULT 池 DYNAMIC|WRITEONLY 环形缓冲，512 KiB VB（FVF 0x112）、576 KiB VB（FVF 0x152）、96 KiB INDEX16 IB，
NOOVERWRITE 顺序追加，回绕时 DISCARD，VB 的 Lock 全部带 NOSYSLOCK，没有越界、嵌套、跳写和 READONLY。
由此发现远端路径接游戏的三个问题：代理拒绝 NOSYSLOCK；wire 的 MaxBuffer 256 KiB 装不下两个 VB；宿主每次上传都两次排空 GPU。
修正：MaxBuffer 1 MiB；CreateBuffer 带 DYNAMIC，UploadBuffer 带 Lock 方式（Ordered/NoOverwrite/Discard，静态缓冲只接受 Ordered）；
代理接受 NOSYSLOCK，只有第一块上传带 Discard；宿主的动态缓冲用 coherent 内存，NoOverwrite 直接写，
stream 档位的 Discard 换一块新存储并带过已初始化前缀，其他档位 settle 后原地写。新增 `D3D9 dynamic-buffer-host:` 计数行。

extent fixture 新增 `--ring` 模式重放游戏的缓冲用法，门 `--ring` 4 个用例（参考、同步、stream、stream+固定功能批），
8 帧 64 个 draw、136 次 Lock、19 次 DISCARD、16 次回绕，四个哈希相同（`8f0918f7`）。42DC1DAC 宿主 fresh BelowNormal/-j2：
CPU 门、VVL GPU 门（原 11 个哈希仍是 03f7265a，尺寸与 reset 哈希不变，VVL 0 错误）、资源寿命门、M0 内存门通过。
旧呈现门仍因写死的计数失败，多出的一次是遮挡查询轮询（上传不再提交 GPU，时序变了）。
未解决：每次 Unlock 仍是同步往返并刷出暂存 draw（固定功能批只剩 1 个 draw）；每次切 FVF 宿主新建声明。
未接游戏、未部署、提交、推送、发布。详见 [S1 回执 §8](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-30：x64 增强层拆分 S1 第三组：G4 设备参数与 Reset（Claude）

代理与 Factory 接受游戏的交换链：X8R8G8B8 后台缓冲、D16 自动深度、INTERVAL_IMMEDIATE，CreateDevice 与 Reset 共用
`ResolveSwapChain`。wire 的 Initialize 增加 args[7] 格式（22 为不透明目标），间隔允许 IMMEDIATE；新增 `Op::Reset`。
X8 仍用 BGRA8 存储，DESTALPHA 混合按 1 处理，呈现时 alpha 取 1；D16 与 fork 一样映射到 D32_SFLOAT。
Reset 按原版 D3D9 的顺序：参数被拒时状态不动；否则清空状态、释放隐式后台缓冲；还有 DEFAULT 池 VB/IB、
后台缓冲引用或状态块时进入 NotReset（DEVICENOTRESET，Present 返回 DEVICELOST）。清空的范围与参考 DLL 的
ResetState 相同：着色器、声明、常量、变换、材质、灯光保留，null 流保留 offset/stride。代理的默认 CULLMODE 由 NONE 改为 CCW。
宿主 `reset()` 按新尺寸和格式重建目标与交换链；窗口改尺寸后、Reset 之前的 Present 跳过这一帧（PresentSkipped），不再是永久故障。

extent fixture 新增 reset 模式与 `--reset` 门 6 个用例：游戏组 1902×963→1280×720，a8 组 257×129→64×64 且尺寸填 0。
两组参考与远端哈希相同（`da171c90`、`5996d86d`），呈现用例 Reset 后再呈现 3 帧。64CABE43 宿主 fresh BelowNormal/-j2：
CPU 门、VVL GPU 门 r4（原 11 个哈希仍是 03f7265a，尺寸用例哈希不变，VVL 0 错误）、资源寿命门、M0 内存门通过。
保留失败回执：GPU r1（fork 收到 WM_SIZE 后自己重置交换链，已让夹具窗口不产生 WM_SIZE）、r2（参考 Reset 后保留
offset/stride 与 FVF，已按 DXVK 改代理）、r3（一次 host 退出超过 5 秒，重跑未复现）。旧呈现门结果与 G3 时相同。
发现：游戏改窗口尺寸时不调 Reset，由 fork 的 WM_SIZE hook 处理；远端路径接游戏前 x86 侧需要等价处理。
未接游戏、未部署、提交、推送、发布。详见 [S1 回执 §7](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-30：x64 增强层拆分 S1 第二组：G3 真实分辨率后台缓冲与呈现（Claude）

协议从固定 64×64 改为协商尺寸，每轴 1..4096：Initialize 携带宽高；Readback 按整行分带（每带 ≤ 65,440 B，
1902 宽每带 8 行）；Present 在整图放不进一次回复时只回帧号，代理再按帧号分带取冻结快照。代理按 D3D9 窗口语义
取客户区尺寸并写回，默认视口与剪裁覆盖整个后台缓冲。Peer 表面模式的离屏目标与交换链用真实尺寸；
host-owned 表面仍只支持 64×64。宿主回读缓冲 256 KiB → 65,440 B；x86 侧只有诊断回读档位保留整图。

新 x86 fixture 与 `--extent` 门共 7 个用例：1902×963 原版/远端/零尺寸/呈现、257×129 原版/远端、64×64 呈现。
同尺寸哈希一致（1902×963 `42d6f4f3`，257×129 `16e2ac0d`），1902×963 呈现 3 帧且与目标图逐字节相同。
25BAC9EA 宿主 fresh BelowNormal/-j2：CPU 门、VVL GPU 门 r2（原 11 个哈希仍是 03f7265a，远端 0 错误）、
资源寿命门 r2、M0 内存门通过。保留失败回执：GPU r1（fixture 卸载后才测 Vulkan 模块）、寿命 r1（我传错工具链路径）。
旧呈现门在 G3 前后结果相同，init/clear-soak 通过，remote-normal 因写死的消息计数过期而失败，待刷新。
init fixture 修正两处过期（args[6] 改用 4，初始化前 Clear 改为 ClearRegions）。
未接游戏、未部署、提交、推送、发布。详见 [S1 回执 §6](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-30：x64 增强层拆分 S1：调用方普查与拓扑/渲染状态值域（Claude）

新增与冻结 v2 并列的 `warvk-d3d9-caller-census/v1`：公开设备方法入口取返回地址，按 PE 范围分 game/self/other，
try-cell 记录不阻塞；严格读取器与 42+8 项测试，原普查测试不变。实测 `s1-caller-census-20261030-r1` 通过：
Game.dll 只用 DrawIndexedPrimitive（三角列表 49,035、三角带 9,372、线带 12，最大 5,896 图元）、FVF 0x112/0x152、
托管 A8R8G8B8 纹理；RT/DS、StretchRect、着色器、状态块全是 WarVK 自己的调用。缺口清单 G1–G5 见回执。

第一组补 G1/G2：新 `d3d9_topology.h` 代理与宿主共用（线列表/线带/三角列表/带/扇，≤65535 图元，点仍拒绝），
宿主按图元类型设输入装配；代理接受全部剔除、深度比较、混合因子与混合运算，DITHERENABLE 本地保存进像素状态块，
caps 与物理 caps 求交。GPU fixture 新增 8 张成员判定图（5000 图元带、BaseVertexIndex、扇、CW/CCW 剔除、
DESTCOLOR、MAX、GREATER）和 2 张只进哈希的线图；raw 协议新增 4 个可恢复拒绝。

AC88CE77 宿主 fresh BelowNormal/-j2。CPU 门、VVL GPU 门（11 个哈希都是 03f7265a，远端 0 错误，raw 5 档各 24 拒绝/恢复）、
资源寿命门与 M0 内存门一次通过。未接游戏、未部署、提交、推送、发布。
详见 [S1 回执](../research/2026-10-30-x64-s1-d3d9-path.md)。

## 2026-10-30：x64 增强层拆分 S0：数据层切口门与标注传输（Claude）

作者同意全面接管渲染层，增强层拆为 x86 数据层 + x64 GPU 执行器，见[拆分计划](../plan/2026-10-30-x64-enhancement-split.md)。
新增 `run_x86_data_layer_cut_gate.py`：llvm-nm 读真实对象、明确规则分类、未知对象报错，`--ratchet` 时正向切口
只许减少。基线 r2（r1 因 `version` 未分类失败保留）：数据层 114 个对象，正向切口 113 个符号 / 21 个对象，
反向面 477 个符号（`d3d9_device.cpp` 388），难点在逐 draw 决定。

D9C2 扩展由 Initialize `args[6]` bit1 协商，未协商一律可恢复拒绝：`FrameScene`（`FrameCamera` 160 B）、
`StageMarker`（四个阶段）、`AnnotateDraws`（只绑定紧接的下一条 draw 消息，序号/schema 严格递增，
其他消息到来即拒绝并作废记录）。代理 `WARVK_D3D9_ANNOTATIONS=1`，导出 `WarVKD3D9AnnotateNextDraw`、
`WarVKD3D9SetFrameScene`、`WarVKD3D9MarkStage`；批次模式记录随暂存批走，非 OK 一律 fail-closed。
宿主只接 FNV-1a 记录器消费者，冻结 v1 行不变，另出 `annotation-proxy/v1`、`annotation-host/v1`。

94BAA54C 宿主 fresh BelowNormal/-j2。CPU 门（各 210,798 项、wire 各 1,454,928 项）、VVL GPU 门 r2
（16 个远端用例 0 错误；11 图仍为 0627ed73；fixture、代理、宿主摘要同步与流固定批两档都是 4959d1f6；
raw 27 个可恢复拒绝、摘要一致）、资源寿命门与 M0 内存门通过。GPU 门 r1 因漏写 raw 固定批账户期望失败，
保留为失败回执。只有记录器，没有真实执行器；未接游戏、未部署、提交、推送、发布。
详见[标注传输回执](../research/2026-10-30-x64-annotation-transport.md)。

## 2026-10-30：x64 迁移的 x86 内容副本按预算驻留（内存优先，Claude）

作者要求内存优先于性能：迁移初衷是减轻 32 位进程内存压力。开发代理此前为每个 VB/IB/纹理在 x86 留完整
副本，接游戏后会抵掉移出 DXVK/驱动的收益。新增默认关闭的 `WARVK_D3D9_X86_BACKING_KIB=<KiB>`：
Initialize `args[6]` bit0 协商后宿主保留 VB/IB/纹理的 x64 权威副本，新 op `ReadContent` 只读这些 CPU 副本；
x86 每个缓冲/纹理 level 为一个驻留项，Lock 和索引检查期间钉住，释放后按 LRU 淘汰到预算内，再用时读回，
读回失败 fail-closed。索引检查逻辑与拒绝结果不变。默认模式 CPU 账户逐字节不变；冻结 v1 行不变，
另出 `backing-proxy/v1`、`backing-host/v1`。

合成工作量（12 张 1024² 满 mip ARGB，约 64 MiB）同像素下 x86 已用 VA：进程内 DXVK 739.9 MiB，
代理默认 126.5 MiB，代理预算 0 为 59.5 MiB，预算 8 MiB 为 65.5 MiB；其中约 617 MiB 是进程内
Vulkan/DXVK/驱动基线。53E76905 宿主 fresh BelowNormal/-j2；CPU 门（各 210,734 项、wire 各 1,454,681 项）、
VVL GPU 门（14 个远端用例 0 错误；主 fixture 297 图仍为 0627ed73，内存工作量四模式同哈希；保留模式
4 个正向读、12 个可恢复拒绝）、资源寿命与 M0 内存门全部通过。M0 门自 10-22 后未跑，期望值已过时
（纹理上传事务每轮多 2 次调用、首个 draw 多 1 次采样器发布、宿主 v1 行新增 `queryWaits`），
r1–r3 保留为失败回执，按调用结构推导更新期望后通过。未接游戏、未部署、提交、推送、发布。
详见[内容驻留回执](../research/2026-10-30-x64-x86-content-residency.md)。

## 2026-10-29：x64 固定功能 draw 异步批次（T2 路线 ②，Claude）

新增 `DrawFixedBatch` op 与默认关闭的 `WARVK_D3D9_FIXED_BATCH=1`（需 `BATCH_DRAWS`）：每包 1–20 个完整、
独立的固定功能 draw（各 3,180 B），代理同一时刻只暂存一种 draw，切换种类、满包及任何远端调用先冲刷，
远端顺序即调用顺序。延迟拒绝同 `DrawBatch` 为终止性；代理暂存前本地 `ValidFixedFunction`，状态域拒绝仍可恢复。
宿主整包解码后全条目 prepare 再录制：流模式整包进打开列表，其他模式保持逐条 settle/submit。
`drawFixed` 即 1 条批次；冻结 v1 行不变，另出 `fixed-batch-proxy/v1`、`fixed-batch-host/v1`。

C32C135C 宿主 fresh BelowNormal/-j2 通过。CPU 门（x86/x64 各 210,734 项 + 新 `bounded-fixed-batch`）、
FF GPU 默认/强制优化（七端 297 图同为 0627ed73，281 个 FF draw 全部经批次）、raw 12 个原子拒绝/恢复
（非异步与新增流同步环境，均像素不变）、计时门 VVL（45,293 draw / 2,299 批）、SM3 回归与资源寿命全部通过，
VVL 0。1,025 固定功能 draw：同步 DrawFixed 45–47 ms → 固定批 **7.8–9.2 ms**，宿主 CPU 约降至 1/3–1/5，
仍为本地 3.3–3.6 ms 的 2.3–2.8 倍，未过建议门槛。剩余主要在 3,180 B/draw 的传输与解码、FF 常量每条重算。
本轮机器状态漂移（未改动的本地 SM3 参考 1.65 ms），只做轮内比较。未部署、提交、推送、发布。详见
[固定功能批次回执](../research/2026-10-29-x64-fixed-function-batch.md)。

## 2026-10-29：x64 准入段拆解（Claude）

新增默认关闭 `WARVK_D3D9_ADMISSION_PROFILE=1`：生产者/工作线程/宿主三方 CPU 墙钟计数，
每次 EVENT Issue 后切样本，≤128 个，关闭时不读时钟；宿主录制内再分常量/状态绑定/
8 槽采样器/DXVK draw。严格读方检查样本连续、单调、嵌套与每区间恰为一帧，纯测试 3 例。

06868F71（一级）与 27482FB2（录制细分）宿主 fresh BelowNormal/-j2 通过；1,025 draw
开/关计时三轮六端像素全一致，计时开销在噪声内。r2 安静流端完成 2.68 ms：工作线程每批
往返 69 µs 完全串行，宿主 apply 占 65%、管道+唤醒约 29%；生产者 75% 准入时间在等队列。
宿主 apply 中 DXVK 绑定+draw 约 1.0 µs/draw（≈1.0 ms/帧，70%），8 槽采样器循环仅 54 ns/draw，
解码/预检/常量合计约 0.33 ms。group 多出的 0.54 ms 全在 flight/批次 UBO 处理。
下一步：FF draw 异步批次 → 宿主原生式脏跟踪 → 预检后回复让传输与录制重叠 → 共享内存环。
未部署、提交、推送、发布；改动文件均未被 git 跟踪。详见
[准入段拆解回执](../research/2026-10-29-x64-admission-breakdown.md)。

## 2026-10-29：x64 宿主流式命令列表与原生常量环（T2 第一批原型，Claude）

新增默认关闭 `WARVK_D3D9_STREAM=1`：draw 批次与固定功能 draw 录进同一打开列表，
在 FlushDraws/EVENT/任何 settle 操作处提交；常量按原生 `D3D9ConstantBuffer::Alloc`
取 1 MiB 环切片，用尽经 `invalidateBuffer` 换存储，不再 CPU 等 GPU。单列表 ≤4,096 draw，
未完成流序号领先 GPU ≤3。与 flights/group/等待计时互斥；冻结 v1 指标不变，另出
`stream-host/v1` 行。固定功能 draw 在流模式下不再每次排空 GPU；代理 DrawFixed 仍同步。

E7CB8E38 宿主 fresh BelowNormal/-j2 通过。批次门四轮、VVL365、FF GPU 默认/强制优化
（五端 297 图同为 0627ed73，281 个 FF draw 进流）、流模式 raw 故障/恢复、96 轮资源寿命
全部通过，VVL 0。安静轮 r3 1,025 draw：stream 2.76/2.90 ms 对 group 3.65/3.81 ms，
宿主 CPU 少约 42%，仍为本地 0.61–0.82 ms 的 3.4–4.7 倍，未过建议门槛；剩余时间
几乎全在 EndScene 返回前的准入/传输段。r1 有 Steam 游戏占 6–7 核，绝对值作废。
未部署、提交、推送、发布；改动文件均未被 git 跟踪，T0 仍待作者确认。详见
[流式命令列表回执](../research/2026-10-29-x64-stream-constant-ring.md)。

## 2026-10-29：评审后暂停兼容扩展，实测 T1 VA 与 T2 flight 等待

StateBlock既有收口保持；按作者转交评审先做T1/T2，暂停RT/DS/Reset等X3扩展。
新增保留只读句柄的完整VA区域/映射文件采样和独立memory+正常退出验收；不能凭
VirtualQuery归属private堆。Lost Temple同DLL OFF/CPU事件环ON/OFF三轮均正常退出：
Private633–653/732 MiB，ON增量79–99 MiB，OFF自身波动约20 MiB。玩家与沙箱
war3.exe今天均已LAA，本批未修改；低2GiB最大空闲块分列，不混同完整4GiB。
仍有约998–1018 MiB已占VA未归属，净迁移量保持unknown，不将Arena DEVICE_LOCAL
预算、事件池容量和GPU显存相加冒充可迁出量。

x64新增默认关闭、128区间有界的flight复用分段计时，沿用原CPU提交/GPU/use退休。
813688CC宿主fresh BelowNormal/-j2及no-work通过；1,025 draw实测合并模式8次复用/
帧，约占完整时间51–55%，逐批模式29次。开启/关闭诊断及VVL365三组门通过，
像素/原始拒绝恢复/资源归零保持。当前无计时同轮本地约1.74–1.76 ms、远端14.55–
14.61 ms，尚不可接受；与旧3.6ms或25ms轮不拼接归因。

真实census独立轮取得1606 Present/173378 draw尝试，但现有稀疏累计快照缺少
逐帧分布、Lock/Unlock/GetData和字节数，不能称T2真实负载门完成。下一步补有界
逐帧来源/字节见证及T1既有allocator地址区间，再做共享环原型；不新增全局设备锁。
纯测试6+39+3+9通过；不是新的产品全量回归。T0与建议性能阈值待作者决定；未提交、
部署、推送、发布。完整事实、口径、T4对比与未覆盖见
[T1/T2首批测量](../research/2026-10-29-x64-review-t1-t2-baseline.md)。

## 2026-10-29：StateBlock 状态恢复与无纹理 shader 接线

x86 owner 新增 Create/Begin/End StateBlock、Capture/Apply，按原 D3D9 实现选择
PIXEL/VERTEX/ALL 字段，不复制整个 Device；录制期保留 live getters，按值保存常量/
变换，资源使用原 COM 私有保活，绘制继续经过现有 x64 DXVK 状态与 GPU 退休。
未结束录制在 Finish 前销毁。修正 null VB 绑定覆盖 offset/stride 的原代理缺陷；
Capture 后换短 VB 保留旧 offset 时，Apply 在任何发布前拒绝并传播失败。

实际 shader 恢复测试发现旧有限 profile 在 COM、wire、host resolver、backend 中
要求 programmable draw 必须有 stage 0 纹理。四处现允许规范空纹理并用原 DXSO
null sampler 语义；非零身份/代际/上传检查保留，缺失 VB/声明/programmable shader
继续拒绝。没有增加 StateBlock IPC、wire 布局、全局锁或逐 setter GPU wait。

80287B63 宿主独立构建/no-work 通过；`x64-stateblock-gpu-20261029-r5/r6` 强制优化/
默认 GPU 门通过：四端各 297 图 / 4,866,048 通道，hash `0627ed73c4fa582e`，远端
各 302 draws。StateBlock 新增 11 图及 native 246 / remote 247 状态检查；私有引用
保活、Capture 替换资源、shader/常量恢复、未结束录制析构均覆盖。两宽 wire 各
1,454,644、token 各 6,560、专门化与 sampler key 各 200,000。raw 门各 97 draws /
72 rejected / 342 submitted=completed=retired，VVL365 无 warning/error，资源归零，
正常退出、来源保全和玩家 DLL 不变。随后 `x64-stateblock-lifetime-20261029-r1`
完成 96 轮 / 576 资源 / 256 Query / 319,488 像素；raw 61 拒绝、48 像素恢复、13 Query
恢复同样通过，最终资源归零、自然退出及 source/reference/player 保持。

保留语法缺特化、预检宏重复、夹具选用未支持的 managed VB、遗漏 wire 解码器、夹具
写死 4096 偏移等失败记录；后续绿门不追认旧失败。仍是有限 64×64 开发 profile，
未完成 Warcraft / RT-DS / Reset / 增强 pass 接入；没有新内存或性能收益测量。
下一步为 RT/DS 和目标切换。详见
[实现、失败和验收回执](../research/2026-10-29-x64-stateblock-integration.md)。

## 2026-10-29：八纹理 stage 与独立 sampler 进入 x64 实际绘制

新增 448 B 成组绑定/采样状态事务，完整预检后发布，每次 draw 重新核验纹理代际、
上传完成和上传所有者。FF 使用原 key 的真实八位纹理掩码，SM3 使用原 DXSO 的 s0..s7；
每个 stage 独立地址/LOD/sRGB，COM 私有绑定与原 DXVK GPU 退休保持。没有新增逐 setter
IPC 或 GPU wait。16 个开发纹理槽供八纹理同时存在，不改变 Warcraft Arena/快照预算。

C68506CF 宿主默认/优化 `x64-multitexture-gpu-20261029-r3/r4` 通过：四端各 286 图 /
4,685,824 通道完全相等，新增 17 图；72 个实际非法事务、部分发布拒绝及新代际恢复
通过，VVL365 零 warning/error，资源归零及自然退出。两宽 wire 各 1,454,388、token
各 6,560，专门化/key 各 200,000 检查。最终
`x64-multitexture-lifetime-20261029-r1` 的 96 轮 / 576 资源 / 256 Query / 319,488
像素及原 raw 故障恢复通过，source/reference 保持、资源归零并自然退出。
构造函数、runner 计数门槛、夹具提前释放引用三次红门均保留；修复后重新执行而未追认。
完整 Warcraft / StateBlock / RT-DS / Reset / 增强 pass 尚未通过；未做新的 Storm、
客户端内存或游戏性能测量，玩家 DLL 未覆盖。详见
[接线、红门及完整接入边界](../research/2026-10-29-x64-fixed-function-integration.md)。

## 2026-10-29：Alpha / fog / scissor 进入 x64 实际绘制

76 B 按值 effects 状态通过同一预检入口交给原 DXVK；专门化常量变化采用原 storage
rename，保留在途旧版本，无新增 setter GPU wait。修复代理 Clear 漏用 scissor 的
真实语义缺口，并保留首先暴露它的 native 测试初始化错误。5BCCD41A 宿主的默认/
优化两门 `x64-effects-gpu-20261029-r3/r4` 通过：四端各 269 图 / 4,407,296 通道
完全一致；60 个实际非法事务和恢复通过，VVL365 零 warning/error，资源归零及自然退出。
`x64-effects-lifetime-20261029-r1` 的原 96 轮生命周期/故障恢复亦通过。
完整 Warcraft、增强 pass、真实性能和 Storm/进程内存均未验收；
遵守用户完整接入后再测内存的顺序。详情及红门见
[本轮接线回执](../research/2026-10-29-x64-fixed-function-integration.md)。

## 2026-10-29：Factory / Clear / FVF 与固定功能实际执行

原 Factory 协商和局部 Clear 已接入 x64，FVF decoder、FF key 与变换常量准备由
本地生产设备和远端共用。持续绘制暴露了优化 pipeline 的 FF 专门化常量遗漏，
已复用原打包算法修复，并以实际有效配置/双宽原实现差分重验。随后 BGRA8 的多 mip、
NPOT、局部/分块上传、LOD 接通；089A0BAA 宿主默认/强制优化两门通过：
两宽 wire CPU 各 1,150,226、专门化各 200,000 检查，四个渲染场景各 31 张图 /
507,904 通道完全相同。30 个真实非法事务及恢复、VVL365 零 warning/error、
GPU/use 退休、资源归零和自然退出通过。共享产品 fresh ED72F52E DLL 与本次配置
119/119 回归通过。详见
[接线回执与保留红门](../research/2026-10-29-x64-fixed-function-integration.md)。

后续 16 格式/表面与 sampler 批次已分别完成实际对照。sampler 共用原 native key，
SetSamplerState 本地合并；sRGB view 与 managed BC 非对齐锁域接到原资源所有者。
E7EFBD5E 宿主的默认/强制优化 `x64-sampler-gpu-20261029-r3/r4` 均通过：四端各
225 图 / 3,686,400 通道实际哈希相同；两宽 wire 各 1,180,900、专门化/key 各 200,000。
48 个非法事务与恢复、VVL365 零 warning/error、GPU/use 退休及自然退出通过。
共享 x86 26B411E9 DLL 本次实际 119/119，资源寿命门 96 轮 / 576 资源、256 Query /
319,488 像素通过，96 轮相同 sampler 仅增加一次有序消息。确切 hash 与红回执见上方报告。

完整 Warcraft 仍未接入：RT/DS/StateBlock、更多 stage、Reset/换图和
WarVK 增强 pass 尚待。Alpha/fog/scissor 后续候选正在验证，不能继承 sampler 身份。
FF 当前同步有序事务，未接受游戏性能。按用户顺序，本批不测 Storm/客户端迁移内存；
没有部署、提交、推送或发布。

## 2026-10-28 后续：GPU 提交合并、非索引覆盖和双进程成本

- 四个 32-draw 消息共享一个原 DXVK command list，仍使用四份常量 backing（测试 GPU
  总计不超过 1 MiB）。EndScene 显式提交尾部，上传/销毁/Query/读回沿原有序边界；
  不扩大队列或跳过 CPU/GPU/use 三项退休见证。
- 实际接通 `DrawPrimitive(TRIANGLELIST)`，空 IB 身份规范化，Draw/DrawBatch 共用
  宿主解析入口，逐 draw 初始化范围/身份验证和整批预检继续执行。定宽 wire helper
  与旧字节 codec 做未对齐/全宽/guard 对拍，字节布局不变。
- 最终同版 1,025 draw，提交合并完成中位数约减少 7–13%、双进程 CPU 周期约减少
  30%；97 draw 中位数约减少 20–24%，依然没有追平本地。额外的批内重复状态省略没有稳定收益，默认关闭，
  不把减少 306,744 次状态设置计作性能接受。系统竞争负载与长时尾延迟未闭合。
- 新 CPU 工作量计数绑定进程身份和保留句柄；短窗口 `GetProcessTimes` 的零增量
  不当作零 CPU，增加原始周期计数且不作固定频率换算。逐帧 EVENT 计时与包含预热/
  读回的 44 帧 CPU 工作量分开。
- r3/r4 状态切换用例先在本地参考失败：沿用了上一 tile viewport，却期待整面 Clear。
  保留失败，r5 显式设置整面 viewport 后本地/远端通过；未放宽像素容差，也没有声称
  已实现子视口/矩形 Clear（这是当前开发宿主的新登记缺口）。
- 最终低 draw r6 reader 拒绝了合法短窗口的系统 CPU 零增量。保留失败回执，新增
  unavailable/null 和原因字段，禁止当作零系统负载；r7 重跑九场景通过。CPU 周期、
  QPC 和像素门仍严格，最终高 draw r6 / 低 draw r7 与状态实验 r5 分开记账。
- 当前宿主 `3ACCB356…C5666C` fresh build/no-work；x86/x64 各 210,608 Connection +
  116,079 wire/状态判定；97 / 1,025 draw 各 9 场景 GPU；VVL365 三场景零 warning/error；
  实际管道 EVENT / DrawBatch 各五种故障拒绝通过。完整身份与回执见
  [专项报告](../research/2026-10-28-x64-submit-groups-and-nonindexed.md)。
- 未改共享产品源；玩家 DLL `1FFE0BD1…35CF` 未动，未提交/部署/发布，不声称完整游戏迁移。

## 2026-10-28：实际 x64 draw 批次与等待成本收敛，完整游戏迁移仍未验收

- **实现**：开发代理按值打包最多 32 draw（37,248 B），资源身份/代际、viewport、
  状态、常量和 index 参数一起交给 x64 原 DXSO/DXVK。整批预检后才记录 GPU，
  更新/销毁/Query/Readback/Present 保留有序屏障；没有增大四槽队列或丢 draw 换性能。
- **共用边界**：以原 DXVK 提交/finish 通知替代宿主固定 Sleep 轮询；修复 Win32
  condition variable 的 deadline 符号、虚假唤醒循环和毫秒转换。timeout 不授权
  取消或复用；原 GPU timeline 与资源使用退休仍分别核验。
- **已验收范围**：通知候选的 97-draw 实际 COM→x64 离屏 GPU 9 场景、29 组整批拒绝
  与恢复、三个独立 VVL365 场景通过；x86/x64 实际等待原语各 1,014 检查通过。
  同轮完成中位数：逐 draw 10.33–10.99 ms、32-draw 批次 0.755–0.877 ms、
  本地参考 0.127–0.174 ms；这是 64×64 提交微基准，**不是游戏 FPS**。
- **x86 共用修改回归**：新独立 DLL `F6589D0F5BC9EA7440764104ACAB3D9B1279FA53D4CE8842DE52D07D468F9C4F`，
  构建/no-work、127/127 配置测试，以及一轮同 35 秒限制内的 Lost Temple 载图、
  截图、退图、自然 exit 0 通过。此前两次启动/退出总时限失败与旧 DLL 正向对照保留，
  间歇耗时原因未证实。玩家 DLL `1FFE0BD1…35CF` 未动。
- **四批在途 / 异步批次**：97 / 1,025 draw 各 16 场景通过，1,025 draw 的三场景
  VVL365 独立回调零 warning/error；193 draw 补充判据覆盖旧批常量、纹理更新和 in-flight
  Destroy / 六类 ID 换代。1,459 批实际提交全部按原见证退休。CPU x86/x64 各 190,493
  检查及真实管道五类故障拒绝通过，完整回执见专项报告。
  高 draw 数同轮完成中位数为 24.64–26.64 ms，较逐批等待约减少 38–44%，仍远慢于
  本地 2.06–3.31 ms；不能宣称性能目标已达成。两次配置超时保留，第三次只把构建
  工具配置 watchdog 改为 600 秒；游戏/GPU/IPC 时限不变。
- **一次性 EVENT 完成通知**：新 FE6ECB 宿主仅在原 GPU timeline 完成后授权本地 Query
  成功，资源退休单独核验。97 / 1,025 draw 各 8 场景、三场景 VVL365、五种真实管道故障、
  x86/x64 各 210,589 Connection + 1,319 wire 检查、59 项严格 reader 通过。
  12 个错误 WaitEvent / 3 个合法恢复、重复 Issue、多 Query、销毁复用均有真实链路证据。
  97 draw 同轮 Poll IPC 从 6,261 / 5,723 降至零（各 46 次真实等待）；完成中位数
  4.995–5.647 → 2.465–4.164 ms，本地 0.419–0.807 ms。高 draw 尾延迟未全面改善，
  紧密本地 GetData 的 CPU 时间仍缺测量，性能接受门保留。测试后只读快照发现另一游戏
  占用约 5.28 个逻辑核；不认证这是全部抖动原因，也未干预该进程。
- **如实保留测试失败**：新 Query 用例最初漏了 D3DGETDATA_FLUSH，使本地参考 Clear
  保留在 CPU 命令流而超时；修正测试后重新全门通过，未放宽 deadline。原 r1 红回执保留。
- **未覆盖**：实际游戏仍缺完整 Factory/FF、多子资源纹理、RT/DS/StateBlock 等代理语义；
  Game.dll 保护保留，不声称全部渲染搬迁、游戏内存节省或发布。详细身份、失败与原始回执见
  `docs/research/2026-10-28-x64-draw-batching-and-cost.md`。

## 2026-10-24：低地址/TLS 压力、真实入口与管道分片修复完成一轮闭环

- **实现**：CurrentDraw 三张大 TLS 表和 material signature cache 改为冷启动
  `LazyThreadArray`；首次 backing 失败在 native hook 边界内 fail-visible，发布前
  三表成组预检，合法 caster 走既有 uncached material resolver，拒绝计数独立且可重试。
  这收紧的是分配异常安全，不是放宽准入或扩大 Arena。
- **实现**：NetEvent 初始化移到 `war3_preinit`/JASS instance 已发布之后；事件标量
  改为原子发布。早期 one-shot 初始化曾导致 `GAME_LEAVE` 永久缺失，本轮生产接线修正后
  复用既有 War3Events reset 与 Present 安全退役事务。
- **实现**：调查器对立即 `ERROR_MORE_DATA` 使用 `GetOverlappedResult` 读取实际完成
  字节数；真实消息管道回归证明旧实现会丢中间 65,536 bytes 并伪造重复 JSON，修复后
  严格读方仍拒绝真正重复字段，合法跨对象同名字段继续接受。
- **验证**：CurrentDraw 生产抽取 `6,489,896/0`；lazy runtime `16,308/0`；
  真实管道 `9/9`、20轮 `180/180`；完整 restricted-Medium Meson `126` tests exit 0；
  Storm/MDL/低地址专门门全部 exit 0。
- **真实入口**：候选 `24,469,504 B / AA4CF7F9499A325A5849C2AA463926908F209FE3A800889ACF997EE70830BCCD`
  在隔离非输入 Desktop 中载入 Lost Temple、取得 1902×963 真实截图，成功退图并以
  原 retained handle 证明自然 exit code 0；玩家 DLL SHA `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`
  保持不变。
- **边界**：这证明 32 位本地入口和低地址压力修复候选可以工作，不等于 x64 renderer
  已接产品、不等于全场景/长时/跨地图/GPU 等价，也不是发布授权。完整证据见
  `docs/research/2026-10-24-mdldata-low-address-repair.md`。

## 2026-10-23：MDLDATA 高地址真实返回/入链已采到，独立OFF再次原指令异常

- 在不重新强制设备多线程锁的基础上，新增默认编译关闭的模型域有界探针：六个精确版本入口，
  局部POD、一次try-cell、4096目录/16探测、allocationSerial+operationToken抵抗同地址复用；
  cell内无原生调用/读取/IO，固定81,936B core与2,097,408B映射。分配前不读未构造node。
  原ABI/参数/返回/LastError透传，不改变Storm/TLSF策略、Game解码、LAA或显存预算。
- build r1的LLVM intrinsic链接失败已修并保留；r2 build/no-work/输入保全通过，BelowNormal/-j2。
  候选34,582,528B / `37BB9CDA329B351C502747A9EC50D8C15CF129AA19151426BC4C420376CD0F0B`。
  实际Core40,293检查、真实MinHook synthetic ABI548检查/0失败，reader62/62（含C++8事件往返）、
  接线6/6、census14/44、runner31通过。窗口0堆分配；未测游戏p95/p99，不称全库全绿。
- 主线程发现r1 reader误把node+4当global backlink，修正为node+0x10/w18并加反例；
  r1语义验收撤销，游戏r1原receipt不改、另存corrected分析，当前以CPU r2为准。
- 三轮同DLL隔离采集：ON r1为64事件/8低分配；OFF自然0xC0000005；ON r2为112事件/19分配，
  **其中9次真实高地址返回**。r2 seq74返回80A30090，75/76入链，81低node7FC45820的backlink
  等于该先前返回值；82 unlink返回、84 free返回不认证链结构正确。预测错误写落点仅是条件复算，
  未采实际store/页属性，未证明写污染或完整寿命；合法~anchor哨兵单列，绝不把所有高位算高指针。
- OFF新dump再次在Game+12D96D异常，模型为AltarOfDarkness（旧为UBirth）；本次低arena头有保留，
  source为MDLDATAHash、两22,404B低块与native header/account吻合。高arena仍无保留，
  不借用ON事件认证OFF lifetime。缺失管道的初始化分支仍未定位。
- ON两轮均120秒超时/Job强制结束；OFF是自然AV，不合并成同类终态。三轮均未成功连接控制管道、
  未执行地图ready谓词、无截图，游戏门仍NOT_COVERED。全部Job空/Desktop关闭、玩家DLL不变。
- 收口复核1524源码身份变化0；宏关闭的同TU无代码/数据/BSS或探针符号（仅debug节）。
  后续先补既有OutputDebugString阶段见证，再设计模型分配域低地址兼容及完整free/realloc边界；
  不扩大Arena、不泛改signed分支、不把探针成功写成x64游戏迁移或崩溃修复。
  详见 `docs/research/2026-10-23-mdldata-allocation-probe.md` 及其中原始/独立回执索引。
  未提交、推送、部署玩家目录或发布。

## 2026-10-23：census 改为局部 POD / 有界 try-cell，分配来源继续收窄

- 按作者要求删除诊断强制设备多线程锁，设备仅遵守原 BehaviorFlags；原业务锁不变。
  Draw 不再额外读设备/上传页状态；未知 shader/FVF/texture state 明确 missing，FF key
  从当场局部值交接。四 draw/三 texture 的 API 和事实成为同次准入事务，Present 只存一份计数。
- ConcurrentCensus 每次一次 atomic_flag 尝试，忙则立即拒绝；固定容量、cell 内无分配/日志/
  游戏/设备调用。按值副本与序号同锁域，格式化在释放后。refusal 是独立 32 位非同刻读数，
  不与 accepted calls 相加，不用零值认证无丢失；溢出黏性可见。默认诊断编译门仍关闭。
- v2 严格读方保留 v1 精确兼容，不以 busy 放松 accepted 恒等式；实际缺失直方图全量核对。
  同一生产 serializer → Python 解析器的往返覆盖两个设备寿命、空样本、截断、Present/Reset/销毁。
- 两次定向 Python 各134项通过；两次隔离 CPU gate 各569检查零失败、3条往返记录，
  wrapper6104B/copy6072B、窗口内0堆分配，3写者+1读者60003事务、halfway交错与恢复通过。
  独立x86目标文件审查确认hot路径无CAS自旋/设备锁；冷ordinal的64位CAS重试明确排除。
  这些是局部合同证据，不是全库、完整调用图或性能证明。
- 构建r1虽477步链接成功，但输入两文件漂移/no-work失败，原回执保留；重新冻结的r2
  regenerate/build/no-work及源身份保全通过，BelowNormal/-j2。候选34,566,144B，SHA-256
  `346B25D3B6968BA338EF88EFA267318AEDFC04BC5064B83A8BBDB5C448665D80`。
- 两个全新restricted-Medium非输入Desktop游戏实例分别产出2/4条严格v2记录，日志均
  forcedDeviceLock=false、BehaviorFlags66；但都未取得ready/截图、120秒超时Job清理，
  **总门NOT_COVERED，正常退出不成立**。两个最后Present为64/1024，draw为1416/24648；
  不能把未知场景阶段计为地图覆盖，不能将超时改称旧访问违规，也不证明强制锁是旧故障根因。
- 高地址只读调查确认失败低节点是UBirth的MDLDATAHash；静态allocator请求0x5784（22404B）
  经Game+0x128EDD→Storm ordinal401。按当前产品尺寸路由原生透传，不受TLSF大块低地址保护。
  Game+0x1292D0是旧head backlink写点；三节点数值重建吻合dump四字段及错误取反地址。
  高节点/arena页未保留，数值重建不是分配史，不认证原生arena返回或live/UAF；protect-memory
  模式也可能从external VA分配。未修改Game/LAA/分配返回值/阈值，没有扩大Arena。
- 玩家DLL、源地图及原始坏证据保留，所有本轮Job/Desktop已收口，未提交/推送/发布。
  后续最小证据是此模型域 allocation lifetime→入链实际写值→free/unlink 的有界关联，
  不是泛改signed分支或将census成功冒充x64游戏迁移。详情和回执索引：
  `docs/research/2026-10-23-census-try-cell-checkpoint.md`、
  `docs/research/2026-10-23-high-address-link-allocation-followup.md`。

## 2026-10-23：真实游戏输入接线与共用 FF，实际进图红门保留

- 按作者要求进入游戏接入阶段：复用原DXSO/DXVK，x86传有类型/顺序/资源身份的数据和操作，
  x64拥有资源与GPU执行；不另写渲染引擎，不传COM/游戏裸指针，不把CPU ACK当GPU完成。
- 共用固定功能编译器已接回生产wrapper；保留缓存、注册及GPU-skin不预热行为。
  新开发census定长6064B，48 API/四draw/三纹理族，32事实及各32唯一key上限；
  计数为尝试、Unknown来源、缺失/丢失显式，默认编译关闭。当前强制device锁有诊断扰动，
  不用于性能验收；下一修订改局部POD与独立有界同步，尚未把设计写成完成。
- fresh诊断x86DLL/x64工具和census-off x86DLL构建/no-work通过；当前定向Python113/113。
  实际census CPU422检查零失败、测试窗口零分配。真实游戏进程观测3VS+1FS配置，
  两位宽4份SPIR-V逐字节一致、28个native负例按预期拒绝；不是GPU运行这些shader的证明。
- 游戏诊断r1未ready而超时；r2模块身份通过但0xC0000005异常退出。
  独审找到真实WarVK/Crash JSON+dmp，修正工具漏目录并补28/28工具纯测；旧失败receipt不改。
  稳定/本批前LLVM控制取得ready及1902×963内部BMP，稳定控制做过一次视觉检查。
  当前census-off两次控制仍无ready/截图、120秒强制收口；不能据此只归因census或FF。
- 离线dump把故障指令定位到Game+0x12D96D，真实返回边界经过JASS原函数透传。
  链字段符号/取反解码和LAA高地址是调查线索，尚缺分配/写入来源，不修改Game/LAA。
  独审未发现FF新增悬空引用，亦不以源码审查盖过实机失败。
- 全部隔离游戏已按所属Job收口、受保护源/玩家DLL不变。无完整远端游戏画面、内存收益、
  自然退出、长期/换图或发布验收；Game.dll代理保护保留。后续先闭合真实输入红门，再推进
  Factory协商、FF状态和多mip/分块纹理。详见
  `docs/research/2026-10-23-native-game-census-and-shared-ff.md`。未提交、推送或发布。

## 2026-10-23：32 位本地 / 64 位宿主真实像素对比，游戏内存收益仍未量化

- 响应作者的内存与对比图请求：未接 Warcraft/Game.dll 完整负载，故不提供虚构节省 MiB/%。
  旧 r3 的 x86/x64 内存列是同一跨进程方案的组成部分，不能相减成收益。
- 只在固定 fixture 的测试副本加可逆、CREATE_NEW 的 BGRA 导出；同一二进制分别走
  保存的 x86 WarVK/DXVK 参考与当前 x64 开发宿主。正式渲染源码及玩家 DLL 不变。
- fresh r2 两路径受限 Medium / 非输入 Desktop 正常退出、Job 清空，4,096 像素/
  16,384 BGRA 字节（含 alpha）完全一致；各 2,548 独立 oracle 检查 0 错。右侧 VVL365 零警告/错误。
- r1 的汇总器误判“host 只能一条 JSON”保留红；r2 新读方严格验证 desktop+final 两条及实际身份。
  采集纯测试20/20；独审重验1,169冻结输入及4份raw清单通过。不是全库回归。
- 真实64×64读回整理为8倍最近邻并排PNG，原始/alpha PNG及来源回执保留，完成一次整图视觉检查。
  明示是测试场景，不是魔兽画面、双窗口Present或内存收益。详见
  `docs/research/2026-10-23-x64-visual-comparison-and-memory-scope.md`。未部署/提交/发布。

## 2026-10-23：正式宿主窗口/Presenter 接线，两轮六场景 GPU 门通过

- 已把同一 HostSurfaceTarget 接入正式开发宿主，x64 独立 UI 线程拥有实际 surface HWND；
  peer 身份不偷换，原 DXVK 资源/帧/队列退休后销毁 Presenter，再复核 device health 后关 HWND。
  普通 GPU 工作统一校验窗口，Present 保留具名且受原 quiescent 谓词约束的故障清理。
- 全新独立 x64 build/no-work 通过（BelowNormal/-j2），host 5,541,888B/C58F020D…19623；
  主线程静态交接 21/21、73 内存变异 KILLED，GPU 读方 22/22。不是全库回归或设备丢失注入。
- 同源、同 host/VVL365、两轮各 fresh x86 构建和真实非输入 Desktop：top-level/child ×
  normal八帧、clear-soak256Present、quiescent父窗销毁，**各6/6通过**。
  每正常例61,152像素检查、每soak例1,048,576检查均0错；GPU提交=完成=退休，VVL零warning/error。
  原5秒primary exit未改，12例实际93–265ms；无强杀，空Job，Desktop关闭，输入Default不变。
- child父窗销毁两轮确实preRetirementDestroyed=1；只认证故障清理，不认证正常保活。
  本轮每例surface callback均1次，未证明真实recreate、GPU在途父窗销毁、完整C2或游戏收益。
- 回执r1 `CB61662C…389530`、r2 `8862ACFB…40ECD7`，每轮1,163冻结输入/39唯一raw保全。
  下一批已定位idle窗口fault不能唤醒backend、空队列parent未监听host退出，计划独立通知/取消drain闭环，尚未实施。
  详见`docs/research/2026-10-23-x64-host-window-lifetime.md`。玩家DLL/游戏/发布未动。

## 2026-10-23：真实宿主窗口八场景 CPU 门通过，接续 Presenter 集成（前置记录）

- 同一 HostSurfaceTarget 在 x86父/x64宿主真实HWND+IPC上完成2模式×4场景；policy54、
  两架构Desktop probe、纯读方19/19通过。OwnerQuit故障后实际保活102.4432/101.9532ms，
  均12样本；child父窗关闭确实外部提前销毁，单列fault cleanup，绝不计成正常保活。
- r1因测试夹具Scenario可能未初始化编译失败且未运行native，原失败保留；r2仅修夹具初始化。
  回执`host-child-surface-cpu-20261023-r2` SHA `E91A6191…E7B571`，独审重算8/8一致，
  58输入、39唯一raw文件保全；无强杀、无输入Desktop切换，Job/Desktop完全收口。
- 同字节adapter已接入开发宿主，补上post-Presenter device-health、quarantine当时窗口报告及
  普通工作统一owned-window准入；Presenter保留具名quiescent故障清理入口。
  新全量构建/真实GPU门另行验证，CPU通过不冒充GPU；不部署玩家DLL、不启动Game.dll。
  详情和未覆盖门见`docs/research/2026-10-23-x64-host-window-lifetime.md`。

## 2026-10-23：X1b事件唤醒与完整六例GPU子集两轮通过

- 实际Connection三处Sleep轮询改成两项匿名不可继承事件；通知只催促queue/done复核，
  四槽+一私有Batch、5/30秒期限、done-first及原GPU退休不变。失败先锁存再诊断，未动系统计时器。
- x86/x64各463项实际CPU断言、5项真实IPC故障通过；CPU-only固定2244命令单AB为
  38550.9463→16.7088ms。跨线程等待不相加，也不把CPU结果冒充游戏/FPS收益。
- 初次notification GPU r1因旧读方假设“只能一个背压事务”失败保留；新采集逐Full/事务/回包
  核对。独审另封堵5项读方误通过，56/56纯测与可逆插桩复验后才重新native。
- r2/r3各自fresh x86构建、同正式源/固定x64 host/VVL365，六例cap5/draw/normal/invalid/com/raw
  全部通过。96轮COM+256 Query+退出3719/1844ms，2244命令全结算；原45秒未改。
  两轮每例GPU提交=完成=退休，VVL零warning/error；Job/Desktop清场、输入Default不变。
- 回执r2 `F440A2D7…4ECE7C`、r3 `18888908…756C80`；各183输入保全。是受限离屏GPU子集，
  不是完整C2/游戏/产品异步owner/显存或VA收益。下一步验证host-owned窗口寿命，不替换玩家DLL。
  详见`docs/research/2026-10-23-x64-notification-waits.md`；未提交、推送或发布。

## 2026-10-23：X1a GPU组合四项通过，COM原期限失败保留

- r1在探针schema读方不匹配处失败、GPU cases=0；旧脚本及回执保留。窄修后r2的
  cap5/Draw屏障/normal/invalid四项通过，实际GPU completed=retired、像素及VVL365零告警。
- 96轮COM在39.531秒完成80轮，45秒外层watchdog触发；raw未执行，整组明确失败。
  exact-owned Job清空、隔离Desktop关闭，输入桌面未变；不把清场当正常退出，不延长门或删负载。
- 回执`x1a-ipc-gpu-20261023-r2` SHA `659B9DA4…62806BB`；独审复核183输入/9 raw清单。
  当前只能说四项机制通过，不能说组合完成。下一步测量实际Connection等待成本，随后重跑原组合。
  详见`docs/research/2026-10-23-x64-bounded-clear-async.md`；玩家DLL、游戏、发布未动。

## 2026-10-23：X1a真实IPC故障交接门

- x86实际Connection/worker经认证管道对x64 CPU故障对端，固定五例通过：S_FALSE、多余载荷、
  错nonce、peer正常退出与半包超时。均在pending Readback握手后触发，32字节活缓冲未被写入、
  后续Clear拒绝；原5秒transport/30秒call和四槽+一私有批未改。
- 五例restricted-Medium非输入Desktop、exit0、无外层强杀、Job/Desktop收口；半包实际5047ms，
  其余31ms。secondary drain15–16ms只作清场，不追认primary正常退出，不伪造GPU DEVICELOST。
- 读方先堵住“peer退出被timeout冒充”和进程创建身份未绑定等反例，纯测30/30；
  回执`x1a-fault-ipc-20261023-r1` SHA `BE70F2DD…56C39FD`，49源码/53输入身份与初始raw保全。
  这是CPU真实IPC故障证明，不是公共COM/GPU故障、产品owner、游戏内存收益或发布验收。
  详见`docs/research/2026-10-23-x64-bounded-clear-async.md`；接着验真实GPU背压/Draw屏障与资源回归。

## 2026-10-23：X1a异步Clear接入与双架构CPU交接门

- 仅开发代理Clear改为有界入队，Draw/资源/Readback/Present仍同步屏障；实际Initialize回包
  建立ready，Full不吃序号，失败先锁存再诊断/排空；四槽+一私有批、原5秒/30秒策略保留。
- 先前未编译草稿现已由主线程集成；r1链接遗漏`protocol.cpp`失败保留。r2补真实实现后，
  restricted-Medium非输入Desktop上x86/x64各263断言通过、Job/Desktop收口、原始输入保全。
  是同一覆盖双架构运行，不是526种用例；CPU合成回包不是真实IPC/GPU通过。
- 回执`x1a-connection-cpu-20261023-r2` SHA `B68BB675…49A66905`；旧Connection/proxy逐字归档。
  blocking lab析构、真实IPC压力/故障、产品异步owner与游戏迁移收益仍未过门。
  详见`docs/research/2026-10-23-x64-bounded-clear-async.md`。

## 2026-10-23：新旧host的VVL365隔离对照完成（仅固定两例）

- 新层首轮旧配置暴露弃用设置，失败回执`x64-dispatch-vvl-control-20261023-r1`保留。
  单独增加1.4.365适配器，不改旧1.4.335读方；精确六类启用横幅、无任何warning/error豁免，
  18项纯测通过并补上混合旧/新横幅拒绝。启用类别不等于全部配置或过滤行为均已运行时证明。
- 正确配置下旧host两例仍因真实GPA警告失败；新host两例均通过：256帧clear/Present与8帧shader，
  独立原始VVL均零warning/error，原5秒transport/primary-exit及45/90秒外层门不变。
  函数表修正未增加Present锁、未关闭特性；不能用新VVL的wait/destroy检测缺口替代GPU寿命证明。
- 新诊断host 5,537,792B / `9BCFF0F730A468DCE23F83626A2D2BEFBB4A21A8992F8FB46A0F6EC06BEEF715`；
  回执`x64-dispatch-vvl365-candidate-20261023-r1/receipt.json` SHA
  `D1A32369A9BC026E5BC78C4B4C6FD040C36A7D1F9CE994D2F90BDE2CEE25A0CA`。
  restricted-Medium非输入Desktop、exact child结算、Job/Desktop清理及初始raw/源码保全通过。
- 这是host-owned独立窗口的两例诊断，不是完整C2、嵌入游戏窗口、全D3D9、游戏内存收益或发布接受。
  旧peer退出红门与初始化5375ms红门仍保留；接下来落实Clear-only有界异步及真实背压/故障交接。

## 2026-10-23：独立VVL发现与函数分派修正（前置记录）

- host-owned两例真实VVL诊断失败已保留：错误函数表查询是本项目缺陷，Acquire/WaitForPresent2
  并发告警则对应Khronos已修正的验证层误报，不据此给Present加锁或关闭特性。
- `vkGetPhysicalDeviceMultisamplePropertiesEXT`从DeviceFn移至InstanceFn，setter保留设备分派。
  11项当前生产源/registry静态测试通过（含8反例），fresh x64构建和no-work通过；旧header、旧host、
  全部历史失败及玩家/天气/两份x86候选DLL字节保全，未作发布。
- 官方修正版VVL仅放在项目artifact下，待以原期限在restricted-Medium非输入Desktop做旧/新host对照；
  不修改系统安装、不屏蔽原始warning/error，不把验证层已修正等同完整C2验收。
- 详见`docs/research/2026-10-22-x64-initialization-and-surface-owner.md`新增VVL节。

## 2026-10-22：固定12例surface所有权续验，三处旧退出失败仍保留

- 同`339F8C6B…`执行固定12例，全部restricted-Medium非输入Desktop：host目标六例全部通过；
  peer的两次256帧及extent故障在原5000ms退出门失败，其余三例通过。整组exit1，不能称全绿。
- 原peer身份/尺寸/销毁拒绝继续生效；host窗口只改变实际surface目标，仍不是游戏窗口嵌入。
  三次secondary退出只用于安全清场，不补救原primary失败；全部Job空/Desktop关闭、无强杀。
- 显式readback profile修正测试reader的26/28阶段差异，38+27纯测通过；旧两文件逐字归档并作
  窄SHA修订映射，未刷新历史receipt或放宽原native门。fault退出watcher采样仍明确未覆盖。
- 回执`x64-surface-owner-continuation-20261022-r1/receipt.json`，SHA`83CCAC35…40005CB`；
  详细逐例结果见`docs/research/2026-10-22-x64-initialization-and-surface-owner.md`。
  接着做独立VVL诊断与X1a有界异步Clear；产品、玩家DLL、游戏和发布均未动。

## 2026-10-22：同EXE窗口所有权ABBA与真实触发边界

- copy-TU诊断完成固定peer/host/host/peer：每臂256帧/1048576实际swapchain像素零错，
  自然退出172/172/93/109ms，全部restricted-Medium非输入Desktop、Job0、无强杀。
- A两臂观测到精确活体PaletteWatcher42548/56724；B两臂在已绑定OpenGL读到认证[0,0]，
  不是module-absent填零。支持窗口所有权影响watcher触发，尚不证明全寿命无watcher或旧退出红门修复。
- 15纯测和独审闭合DWORD宽度、最终monitor/proxy创建身份；1379输入记录/26raw及执行副本重验。
  同EXE SHA `339F8C6B…C8C721`，receipt `B1C63374…AD237`；6条编译warning如实保留。
- 产品源码/host/玩家DLL未变，B不是peer HWND/compositor视觉验收。继续同身份故障/八帧/长负载，
  不部署、不发布、不用后来的快退出覆盖旧失败；详情见初始化与surface所有权研究。

## 2026-10-22：初始化分段实测与窗口所有权因果实验准备

- 同源复制TU初始化诊断三轮完成，原5秒门内2597/3105/2306ms；各256帧/1048576像素零错，
  自然退出94/110/125ms，真实restricted-Medium隔离Desktop及Job收口。原r10超时没有复现亦未修复。
  独审重新核对每轮raw与实际编译/链接输入；三轮EXE SHA不同，不称同二进制A/B。
- 冻结系统映像静态证据发现跨进程窗口会走PaletteWatcher分支；普通WM_NCDESTROY与detach清理
  的TID清零方式不同。与r5历史线程终止但全局未清零相容，尚未测到完整驱动调用链。
- 新增copy-TU surface-owner ABBA诊断及13项pure测试，产品构建不引用；两臂同EXE同窗口负载，
  仅选择peer/host surface目标，peer校验保留。native尚未执行，不声称已修复退出。
- 边界收敛：原peer身份与实际surface目标分账；窗口清理由真正创建线程负责，晚于Presenter退出、
  早于device卸载，不另建GPU寿命或改系统库。旧失败/玩家DLL保留；VVL与全游戏迁移仍未验收。
  见`docs/research/2026-10-22-x64-initialization-and-surface-owner.md`。

## 2026-10-22：新DLL全回归与真实watcher退出证据（开发续批）

- 专用新x86 DLL完整配置回归116/116通过、全目标no-work；2099项输入与初始raw字节保全。
  所有native测试在restricted-Medium非输入Desktop，父进程exit0、Job空、Desktop关闭，未发全局输入。
  回执`x86-shader-cache-regressions-20261022-r1/receipt.json`，SHA
  `E585984F353B2F26618938C79F9FC0C88EEABDBD2EF5E9640FD8ABC8A3FC06A2`。
- watcher仅允许冻结preferred或独立绑定module base的完整表示并锁定；55项纯测通过，
  读取范围/权限/预算/native输入不变。预定r3/r4/r5诊断中末轮复现5000ms退出失败。
- r5实际取得TID55712退出前活体、精确OpenGL PaletteWatcher起始地址及保留线程HANDLE；
  退出后约0.3/1/3秒历史线程均已结束但同一全局TID仍非零。旧栈与新状态证据相符，
  尚未证明创建调用者/正常清零责任，不改系统/强杀/延长时限。secondary退出0不补救原失败。
- 三轮均只诊断、acceptancePassed=false；产品异步、完整游戏迁移和显示可靠性仍未验收。
  详见palette-watcher研究的r5记录；继续用复制TU测初始化各阶段。

## 2026-10-22：当前R1/Q1重验与显示初始化红门（开发续批）

- 当前host `888B801C…` 的R1/Q1 r3实际通过：COM96轮/319488像素、raw169728像素零错，
  61个精确拒绝后48次像素与13次Query恢复；GPU/资源退休、终态逻辑归零、child自然退出79/312ms。
  独审重新核对1128源码、9binary、6执行副本、7份初始raw及39+344读方变异，不复用旧host通过结论。
- 当前完整Presenter r10失败：首次clear-soak的Initialize耗时5375ms超过原5000msreply门。
  客户端断管后的host self-fail-stop是后果；无该轮坏像素证据，也不是该轮退出期卡顿。
  不延长时限，不将schema错误标签冒充根因，原raw/源码/隔离收口证据保留。
- watcher只读r1/r2暴露测量工具的NT ImageBase表示缺口：磁盘preferred与实际module base不同，
  精确差异仅该字段；两轮尚无TID读取，不能声称已取得watcher线程归属。45项纯测试及真实差异保留，
  后续仅做绑定实际module base的窄表示校验，不改系统/权限/采样预算或旧失败。
- 新x86 DLL完整配置回归在restricted-Medium非输入Desktop进行，原玩家/天气/参考DLL不替换；
  VVL、初始化/退出可靠性、产品异步及游戏迁移收益仍未验收。

## 2026-10-22：共享DXSO实际复用与三方像素等价（开发续批，组合门待验）

- 专用clean-start x86产品构建r1达到原1800秒watchdog，失败保留；确认后代编译器结束后以r2续编，
  输入清单完全相同，完整DLL链接/no-work通过：34,500,608B / `6805CC0B07DF1D1F3314ACCC5A18CF5342823D705E02FCA367F16ED3D99DF3DE`。
  不删除失败、不延长时限、不覆盖旧reference/天气候选/玩家DLL，编译warning如实保留。
- 新M0 r3在restricted-Medium非输入Desktop完整执行96轮/256Query/319488像素，零错且自然退出62ms。
  实际8个cache观察确认192CREATE只编译/注册2次、190hits、cookie和400B token容量稳定；
  1487条M0及579条cache读方反例通过。x86 drained private固定2342912B；x64 drained private
  末值较首值少2248704B，但VA仍增长且无同条件ABBA，不宣称游戏收益或全进程无泄漏。
- 三个独立Medium Desktop的old-reference/new-reference/remote各72帧、239616选定像素零错；
  覆盖同token不同COM对象、全部public释放后重建、DEF常量变化与恢复；设备/factory终态引用归零。
  像素等价与cache命中分别取证，不将相同图像当成缓存复用证明。
- 补齐验证器实际缺口：M0/equivalence/Presenter/R1/VVL初始raw字节冻结及重验，截断Validation Error拒绝；
  全回归门拒绝零测试/重复JSON键，采用唯一Meson日志名和历史原始副本，失败路径也核验保全。
  root独立纯测：M0 39、equivalence37、cache reader52、regression reader22、watcher40均通过。
- 完整当前Presenter/R1、独立VVL、新DLL全回归仍在继续；此前退出失败不因这几项通过而消失。
  代理仍拒绝Game.dll，没有部署、游戏迁移或发布结论。详细实据见DXSO cache与M0研究记录。

## 2026-10-22：Medium因果修复、有效退出对照与共享DXSO缓存（开发续批）

- 私有派生token补真实用户默认DACL后，同一冻结x86/x64 kernel-only探针与完整CPU父子探针
  均实际通过；源token/共享安全配置不变。早期入口前失败保留，不用新的通过追认。
- 有效Medium层off/on对照出现off仍退出超时的反例；进一步真实全空层来源运行仍失败。
  新转储经匹配PDB与LLDB实际unwind，仍落在OpenGL palette-watcher的进程detach等待。
  初始化偶发超时与退出等待分列；未改deadline/系统/外部进程，不能宣称退出已修复。
- x64同内容shader每次CREATE新编译的机制已改为生产共用、设备拥有的canonical DXSO cache；
  完整token/domain校验，只有验证后的胜者注册，shader与metadata成套交接，失败注册不盲重试。
  独审无阻断项；真实DXSO CPU在Medium非输入Desktop 88条通过。完整新x86 DLL/GPU缓存收益
  尚在验证，不能用CPU通过代替adapter/GPU/内存收益。独特shader工作集仍未有界、hit仍有token复制成本。
- VVL/层来源纯读方49/10用例通过；真实VVL仍待匹配完整运行门。玩家DLL未覆盖、未启动游戏。
- 详见 `docs/research/2026-10-22-dxso-canonical-module-cache.md` 与更新的Presenter隔离回执；
  依然是受限D3D9开发链，非完整WarVK迁移或发布。

## 2026-10-22：M0死槽位账户复跑与受限验证环境定位（开发续批）

- M0 r2在真正非输入Desktop通过：54条native账户断言、96轮/256Query/319488像素零错，
  1487/1487实际回执读方变异；资源与句柄最终归零、host自然退出109ms。源、参考与玩家DLL保全。
  x86四个排空点private均2318336B，但x64排空点增长12804096B，不能宣称完整无泄漏或游戏收益。
- 真实销毁变异第一轮未执行到目标：CreateDevice响应超时，`0x88760868`，保留失败不计KILLED；
  原始Job为空、Desktop关闭。门禁新增薄归档成员冻结与预期exit1的独立清理认证，未改写raw失败值。
  第二轮真实命中dead容量12B，前三检查点通过，第四精确拒绝；fixture exit1、child自然exit0/125ms，
  输入保全。此轮KILLED不追认第一轮超时，也不解释其原因。
- 独立VVL/隐式层过滤需要有效Medium子进程；受限CPU探针与kernel32-only两宽度探针均在入口前
  `0xC0000142`。只读检查证明WinSta0/专用Desktop全权限AccessCheck通过，不能归咎Desktop ACL。
  派生令牌默认DACL仍仅BA/SY全权、logon读执行；BA已经deny-only，缺当前用户allow。
  此为下一最小因果诊断候选，尚未宣布根因。没有改共享安全、系统、玩家环境或放宽超时。
- 详见`docs/research/2026-10-22-x64-memory-account.md`；所有结果仍为受限D3D9实验，
  完整WarVK/游戏迁移、实际游戏x86内存和性能/视觉门保持未覆盖。

## 2026-10-22：原 Presenter 接线、C2 资源复用与退出红门（开发 checkpoint）

- 真正x86 D3D9 COM→IPC→x64原DXSO/DXVK已接原Presenter/SwapchainBlitter；全部新GPU验证在
  非输入Win32隔离Desktop，原子Job与精确PID/creation收口，不切桌面、不发全局输入、不碰玩家DLL。
- Fresh Clear→Present暴露deferred clear未物化：现在准确source沿原external release/acquire交接，
  不用读回预热。健康owner先用原device idle排空CPU submit/finish尾部；它不是零等待，也未证明
  是所有历史退出延迟的根因。r7的37case通过保留为限定证据，后续r8/r9重新发现退出红门。
- C2按`id/generation`绑定资源并贯穿六类draw输入；只有准确Destroy成功后才允许复用，公开COM
  release不提前释放private绑定。Query具备独立句柄与revision，同一GPU serial可重复Issue。
  同时容量、预算及GPU退休不放宽，不再受累计64次创建/Issue限制。
- R1/Q1 r2实际COM96轮绘制/319488像素及256次Query，raw51draw/169728像素、61个拒绝/恢复
  反例均零错；资源/逻辑句柄清零并正常退出，独审复核原始日志/身份/读方变异。r1的45秒超时
  只到初始化日志，原因未知；新增稀疏里程碑不是已修复的证明。
- 共用修改fresh x86产品DLL全量构建474步、配置内116/116回归与全目标no-work通过；
  34476032B / `5A8B5B691BBB18473D975DC3CAB1BEDD3FB641A938D10E8AB5A6FFBA9BECAB8C`。
  后续变化在开发host/测试诊断，不覆盖玩家DLL，warnings保留。
- **未过门**：r8连续256Present/1048576像素正确但首个5秒退出超时；r9的8帧诊断例也失败，
  已到`wmain-locals-complete`后仍未退出。保留r8二进制repeat3复现5016ms，三份精确进程快照
  只剩原主线程；后续r9精确小转储定位到系统OpenGL的palette-watcher退出等待（Sleep50ms×最多100次），
  已非“没有栈”，但谁引入/保留该watcher尚未归因。不扩大时限或secondary退出追认。
  隐式层关闭对照发现高完整性进程的环境变量未生效，实际层仍加载；独立VVL尚未实跑通过。
- 新增M0显式、按值、身份绑定的两进程内存账户；r1隔离桌面真实96轮/256Query/319488像素零错，
  8个窗口及真实回执1487条读方变异通过。x86 private最大采样2322432B；x64 drained private仍增长
  13484032B未分解，不称内存平台已稳定。独审要求补dead-slot残留容量反例；没有游戏内存节省结论。
  详见[内存账户](../research/2026-10-22-x64-memory-account.md)。
- 产品异步/背压、完整D3D9和WarVK pass、Reset及游戏VA/commit/尾延迟仍待验；代理继续拒绝Game.dll。
  未部署、未启动游戏、未修改系统、未提交推送发布。详见[Presenter回执](../research/2026-10-22-x64-d3d9-presenter-isolation.md)、
  [资源与Query回执](../research/2026-10-22-x64-resource-generation-and-query.md)和[迁移路线图](../plan/2026-10-11-x64-render-migration.md)。

## 2026-10-22：关闭 x64 对照的生产 COM 退出红门（开发 checkpoint，未接游戏）

- PostFX 的纹理/surface/shader/声明/VB、ImGui backend/cursor 改用 device-owned private 引用，
  创建结果先 private 接管再交还临时 public；外部 Create/GetDevice/stateblock 的公开保活不变。
  ImGui 从固定 SHA 的 upstream backend 生成七个明确前缀入口，未修改 upstream，产品未混入 stock Init。
  初始 device RAII 保活、部分初始化失败清理与合法重试接入实际创建路径；未声称所有 OOM 已覆盖。
- UI 按 exact device/context/window 所有者处理 Reset/Present/cursor 和两条 WndProc；退出先撤回回调，
  保留资源经过原 CS/GPU drain 后销毁，不在 CRT detach 隐式回收 GPU。未宣称任意多线程 UI 安全。
- Reset 审计发现额外生产缺口：实例缓冲是绑定首设备的进程 singleton，未启用自动 instancing 时 Reset
  也会创建 6 MiB DEFAULT VB。现由每 device 惰性独占，移除第二层全局裸缓存，VB/声明/captured texture
  用 private 引用，锁定指针归实例，执行入口筛选 owner。带锁 final Release 在 Flush/drain 前解锁，
  避免 staging Unlock 在最终 idle 后追加上传。未启用实验 instancing，未改预算/准入/GPU 退役语义。
- 全量构建抓到旧 DXSO 抽取漏改的 material 构造调用，已同步三参数生产合同。独立 fresh 配置 r1 的
  Meson subproject 路径失败与 r2 编译失败保留；最终产品 r4 完整构建/no-work 通过，已有 warning 未抹除。
- 原门实跑 `x64-d3d9-20261022-r1`：remote/reference × normal/invalid 四例各 2548 像素零错，
  shader/image 身份匹配，设备/factory 最终 refs=0/0 且正常退出；四个总判定位全 true。
  旧 `x64-d3d9-20261011-r4` 的 refs15/总 false 不改写、不追认。
- 同一产品对象独立重链 test-only companion（不增产品 export）：默认与 staging 两配置各 3 周期、
  6 台设备、60 probe、147 实际 GPU draw；PostFX 6144/光标纹理1251/实例保留纹理144像素分别零错，
  外部 DEFAULT Reset 真拒绝3次、合法 Reset6次、外部 texture/GetDevice 保活与双设备隔离通过，正常卸载/关窗。
  仅均匀低亮 PostFX oracle、cursor payload+draw；不外推高亮Bloom/ACES、最终UI像素或游戏视觉。
- staging r1 native/config 虽通过，总门因 EXE 相对 crash 状态污染旧 evidence 目录正确失败；旧证据不恢复。
  改为新目录内 EXE/DLL 精确物理副本，重新生成默认 r2 和 staging r2，全输入/旧基线保全通过。
  staging 依据实际 effective config 与冻结源码，不冒充直接量到 map-mode/copy 次数。
- 定向静态60方法、独立lifetime读方22方法、D3D9/buffer/GPU读方122方法通过；本构建配置 **Meson116/116**、
  全目标 no-work，源码与产品身份未漂移。重复测试不相加为独立覆盖量，也不叫全项目/游戏全门通过。
- 产品 DLL **34,476,032 B / `AF5F6282882FFE64B502859E4BFDF36031A69E658DC093158067722E4B044CEF`**。
  玩家DLL `1FFE0BD1…EE35CF`、旧天气DLL `8A156518…4090CE` 保持原样；无游戏/部署/系统修改/提交推送发布。
- 下一门仍是原 Presenter 接入当前真实 x64 D3D9 输出；当前远端 Present=E_NOTIMPL。
  跨线程最后 Release、默认关闭实例化的异常/锁定 replay、全 D3D9/换图/故障、游戏内存与尾延迟收益仍未验收。
  详见[本批完整回执](../research/2026-10-22-device-owned-com-lifetime.md)与[更新路线图](../plan/2026-10-11-x64-render-migration.md)。

## 2026-10-11：实际 x86 D3D9 → x64 DXSO/DXVK 离屏绘制（受限开发切片；总门仍有红项）

- 不再停在独立 Vulkan demo：真正 Direct3DCreate9/COM/Lock/shader/state/DrawIndexedPrimitive/query/readback
  通过固定窗口、专属 I/O owner 到原 DXSO/DXVK。默认不接产品、Game.dll进程拒绝，Present明确E_NOTIMPL；
  64×64/D32F/单stream与texture/受限SM3，不声称完整兼容或游戏减量。
- D0共享：DxsoOptions显式依赖device/options/常量布局，十个原SPIR-V helper抽取正文相同并只编进dxso_lib。
  五个实际生产x86 TU按原flags隔离编译成功，未重新生成旧build/链接新DLL；旧DLL/原对象SHA不变。
- 实跑抓到descriptor提交缺口：r2三draw都只得到clear，1156像素失败。getSyncHandle也负责发布未满descriptor块，
  旧direct endRecording遗漏它；现统一到endRecording收尾/finalize前，普通flush共用同一出口。
  r3、最终r4合法绘制恢复；不关descriptorbuffer、不改shader/预算/剔除。失败原证据保留。
- 新接线两边界：仅draw纯预检ProfileRejected转INVALIDCALL，GPU/owner错误仍终态；健康idle等待首字节，
  header/body仍原5秒期限且真实cancel/drain。6500ms后合法API、坏TEXCOORD1 VS拒绝后原3draw全部实测。
- 最终r4远端正常/invalid均通过，每例2548像素0错、全图FNV97937de3dc0c0c6d；GPU各14/15完成并退休，
  拒绝draw不提交，remote COM归零/helper退出0。shader scanner两宽度各6525/0、wire各1222/0、idle各3场景，
  实际读方反例59方法、共享静态11方法通过；旧RH0 16/16与实际DXVK缓冲4/4复跑通过。
- **红项未掩盖**：保留单进程reference两轮图像均一致，但最终device Release仍15/退出2；源审与PostFX14个
  public资源+ImGui1个public device持有相符，不是逐地址census。r4保持remoteGatePassed=true、
  referenceImageMatched=true、referenceLifecyclePassed=false、总ok=false。下一批收紧这些device-owned内部引用。
- 最终源1107项无漂移，x64编译数据库仅允许options/helper两D3D9 TU。无全项目门继承，validation layer/WSI/
  完整D3D9/慢GPU与故障组合/游戏画面和内存收益仍未覆盖。原天气DLL 8A156518…4090CE未改；未启动游戏、
  部署、改系统/私有共享核心、提交/推送/发布。详见[完整回执](../research/2026-10-11-x64-d3d9-first-draw.md)。
- 收口显式复跑D3D9/buffer/GPU读方与共享边界四个CPU模块，133方法/15.282秒/退出0；
  1107源码+五项最终产物/参考/编译数据库合计1112身份复核无漂移。不作新GPU/全项目通过，r4总红门不变。

## 2026-10-11：回到长期主线，立即开始真实 x64 渲染迁移（开发 checkpoint，未接游戏）

- 作者最新指令覆盖“64位延期”。路线定为薄 x86 Game/JASS/D3D9 兼容端→现有 DXVK/WarVK x64 宿主；
  不以不完整 native capture 代替全画面，不复制 FORGE renderer、不把 RH2 小后端变成第二套产品引擎。
  [实施计划](../plan/2026-10-11-x64-render-migration.md)、[真实 D3D9 边界](../plan/2026-10-11-x64-d3d9-migration-boundary.md)、
  [人话结论及逐项证据](../research/2026-10-11-x64-render-migration-checkpoint.md)。
- 主线窄修：`ResetAuthorState() noexcept` 不再临时 reserve/push 整表；锁内交换旧 registry，锁外原线程退休。
  真实 registry/typed/Weather 两位宽各26检查、天气协议各211/0、既有JAPI协议通过；相同测试恢复旧源码的两份
  隔离镜像均精确在真实 reserve 触发 bad_alloc。完整生产 JAPI TU 无测试宏独立编译通过，未链接新DLL。
  不外推测试宏排除的 point/fog/Walk/Lightning/Curve 后端、GPU或实机低内存修复。
- GPU前置：RH2真实x86↔x64几何绘制/外部图像双向GPU交还，五场景通过，wire两宽度各9518检查；
  RH3真实x86隐藏HWND消息泵→x64 swapchain，6帧/resize/recreate/close，x86未加载Vulkan。
  实际DXVK核心已fresh x64构建：原allocator/Context/submit/usage tracker，3批GPU fill/copy、3072字检验通过。
  均不是前台视觉、游戏或完整D3D9迁移证明。
  资源通道加入后，共用传输已再次实跑：RH2最终r4五场景、RH3最终r2隐藏窗口门通过。
- X1新增固定4×64KiB SPSC命令队列，无运行调用内分配/锁/I/O/内部重试；Full不消费序号、不丢已接纳指令，
  Stop与close/drain分开，CPUack不退休GPU。x86/x64各三轮，真实双线程每轮10000批通过；Fault并发仍未单独测。
- 资源接线已实际落地：x86 producer→有界队列→独立IO worker→WRC1私有preflight/commit→x64现有DXVK BufferBackend。
  两宽度各2432协议/资源/异常事务检查及独立99B golden；最终 `x64-render-20261011-buffers-r3` 四场景全过。
  正例14命令/9 GPU批，6次上传229505B，3次GPU读回360509B与独立hash一致；包括分块、非对齐更新及销毁/重建。
  三反例分别拒绝旧代际、错地图、越界：accepted/backendCalls=9、GPU=6，无额外执行。资源逻辑限额不是物理驻留/游戏减量。
- 独审抓到并封堵读方误通过：bool/float混入整数字段、expectedRejected接受null、超宽DWORD PID；
  校验在Win32 settlement前执行。新增37个实际读方反例方法，联合定向Python回归116/116；最终GPU接线另实跑重验。
  原始native输出类型正确，旧反例失败和r1/r2中间回执保留，不用新脚本追认旧版本身份。
- 未通过/待做：Khronos validation layer在当前高权限环境不可见，host-validation三份失败保留；未改注册表或静默关层。
  产品异步owner/故障退出、真实D3D9 COM/Lock/state/query/Reset/Present、WarVK所有pass、游戏画面/内存/尾延迟仍未闭合。
  阻塞lab transport只用于受外部watchdog监管的独立进程，不能直接链游戏渲染线程。
- 天气预览停止扩功能，原前台艺术/性能与天气换图/Reset/resize/退出门保留；S1 early-cache异常事务仍是已登记债务，
  不由这次Reset窄修覆盖。本批未启动游戏、部署、覆盖玩家DLL、改系统/私有共享核心、提交/推送/发布。
  原天气DLL 34,467,840B / `8A156518…4090CE` 不变；此前114/282等整包门不继承给当前dirty源码。
  收口时四份最终门禁的源码SHA清单均无漂移，本批构建/测试进程已结束。

## 2026-10-10：可编排暴风雪晨间冻结（可测试预览，非稳定发布）

- 12个天气JAPI、YDWE完整作者入口、单实例64关键帧、按值帧/地图/设备快照、固定GPU雪片与独立Medium雪雾/低空吹雪接入生产。没有天气启用环境门；雪不加入caster/Stage11/Arena，不扩阴影预算，不推进64位。完整[人话报告](../research/2026-10-10-weather-morning-report.md)与[作者文档](../../WarVK/WEATHER_API.md)。
- 真实同脚本A/B定位并修共享real文本科学记数问题：原版S2R不识别e/E，旧微秒值被读成秒；旧3红保持失败、新35/0。Froxel读取改用已重建深度的viewport-local UV；最终实机消费域2560×1440、世界viewport(0,47,2560,1081)，没有改密度/网格/预算。近雪尺寸2–6→1–3，限定静帧可读性改善，低雪仍待美术调校。
- 最终真实功能canary：15阶段、35检查/0错、13张1440p；长循环stress-b：70阶段、1083/0、32次开启/关闭、8次重建、7图，限定required全过。stress-a的70个UnsupportedFeature仍是失败；新fixture保留未实现PostFX调用并明确断言18，不宣称其总开关可用。读方隐藏第四坏帧、unknown回零、冻结参数被筛掉三反例实跑后补门，不追認旧结论。
- final fresh LLVM22.1.8/Win32 DLL、BelowNormal/-j2编译与no-work；Meson114/114，完整静态282/282（现轮CPU探针实际执行）、SPIRV18/0；功能读方54/54、stress39/39、雪片26/26、volume17/17、API9/9；runtime164/0、生产协议211/0、retry80/0。作者文档例与demo两个完整PJass均EXIT0，未把它们当真实VM执行。
- 同key天气当前账：Snow深度15MiB、天气volume约36.46MiB，反复开关/重建未累加；不含driver与retired峰值，普通雾账明确unknown。private尾段有回落但不认证无泄漏；CPU基准不外推GPU/FPS。仍缺前台性能/玩家艺术验收、移动远距/透明与战争迷雾、天气跨实际Reset/换图/extent、故障注入和自然退出。
- 最终冻结DLL **34,467,840 B / `8A156518BD029D106EC009DC3C702D0005746E2E97E4589C1C18F8F4684090CE`**，PE32/i386；dirty主树独立LLVM候选，非release-clean镜像。玩家地图/DLL/Game保护SHA不变、无全局输入/注册表写；owned Job强制清场不是自然退出通过。不提交、推送、公开发布或自动部署；早轮坏证据与既有dirty/StormBreaker保留。
- 已以CreateNew打包桌面 `WarVK-Weather-Preview-20261010.zip`：**7,415,531 B / `57AAC24B4D32E42BE09620C27C1D595F1F6D18EE17DF5CB8F2B1776D83F9D58E`**，14成员；目录/ZIP严格成员、每文件SHA、CRC及PE32审核通过，包内DLL是上述原字节。作者catalog/wrapper/常量/demo/说明与人话报告一起交付，stable/release全部标志false；生成回执为 `AutoTest/artifacts/weather-night-20261010/package-final-receipt.json`。无自动部署。
- 最后[独立审查](../research/2026-10-10-weather-stress-independent-review.md)已收口：亲验三个旧读方误通过、修后39/39、Froxel源码与17/17模型、两张04E候选新尺寸图；没有在该11行域修正中确认新P0。审查未独立全量复算stress-b或做新Froxel画面全组合，主线程的新实机/包审计与其亲验分列。共享积分修正可能改变普通雾/光学guide的空间数值，不承诺视觉逐像素不变。

## 2026-10-10：可编排暴风雪首轮整合（历史开发checkpoint，非发布）

- 03:15追加实际门：fresh LLVM DLL与no-work、Meson114/114、fresh CPU目录全静态282/282。保留原111/114失败并区分残留OUTPUT与memcmp padding原因；未放宽测试。新drain变更仍需下一轮身份，详见整合回执第二轮。
- 独立反例审查补雪片/雪雾资源准备异常隔离与120帧重试、旧地图Weather备用入口退休、only-weather关闭的一帧维护。CPU微基准235/0、默认new窗口0；不是整个GPU效果成本。正在补严格分离的真实JASS canary，未做像素验收。

- 作者授权台北09:00前开发：新增12天气JAPI与YDWE分类、固定容量时间线、一次按值帧快照、GPU雪片和独立雪雾/低空吹雪；不进入caster/Stage11/Arena、不增加既有阴影预算。音乐仍使用原版API；建筑积雪/屋顶碰撞延期。
- 独立Win32生产CPU：runtime164/0、真实协议175/0、资源重试80/0；实际编译SPIRV ABI18/0；Shader合同25、API静态7、volume静态10、Launcher反例24通过。MPQ副本pjass通过；未执行JASS VM，不等于画面已验收。
- 全静态首跑282项中275退出0、7失败（旧探针路径5、旧字面合同2）；退出0内另有动态skip。全新LLVM构建目前仍在收口，root漏UI include已认领；不声称全绿。原GCC前端加载失败未改系统DLL，LLVM fresh目录与旧对象隔离。
- 几何/深度布局、半像素、薄介质、地面低雪与位移周期已收紧；正在闭合天气分配异常不得停掉原雾的边界。玩家目录/原地图未改、未提交推送发布。后续验证与最终产物以[整合回执](../research/2026-10-10-programmable-blizzard-checkpoint.md)为准。

## 2026-09-30：数据采集第一条材质交接链（源码与离线候选）

- 不等待共享上游，补当前 D3D9 draw 状态 → native entry/exact alpha 元数据 → 最终 caster → resolved replay → schema-3 input export → 严格读方。新增按值观察，原 render/texture-stage/sampler/material/texture-transform bits 与 prepared shadow 材料分列；最终来源覆盖同步携带观察，不重新选择旧来源。详见 [实施回执](../research/2026-09-30-native-render-input-collection-checkpoint.md)。
- 纠正 sampler/stage 索引差异；跨帧保留原 ShadowProducerSerial，不和 event/batch 帧混用；无最终 part/layer 的路径保持 Unknown，不为诊断改变渲染身份。Ambient 已含 WarVK SetMaterial 改写，不能称未修改原材质；完整动画/GEOA、实例寿命、原 geoset、纹理像素及全场景 pass 尚缺，`sharedRendererReady=false`。
- 新 POD 344 B、prepared material 56 B；raw input 元数据上限 1472 B 并有生产 size 断言，完整/内部 CPU payload 准入预算 247.5/96.25 MiB。无新 GPU ring/锁/扫描；关闭 raw input 不额外读设备状态，但固定 POD 驻留与复制成本不为零。组件两路各 10 万次采集无堆分配，不外推帧性能。
- BelowNormal/-j2 exact DLL 编译与 no-work、定向 Meson 8/8、POD 1296/encoder 262 检查、往返 7/合同 57/manifest 45/静态 27 + 受影响 75 个 Python 用例、memory 46+87 检查及六文件 py_compile 通过。不是全库/游戏/GPU/视觉验收。
- 本地候选 PE32/i386 **36,551,002 B / 24C90B03AE1052882217BC24BA11990BB7B7F1E0784828D153587FDFDFB082FE**；不覆盖游戏 DLL、不复制原生资产、不部署、不提交推送、不把这份本树构建当作 release-clean 稳定候选。保留既有 dirty、StormBreaker 和 RC/evidence。
- 保全核对：status 93→111，仅新增本批 18 个路径；92 个开工 dirty 文件中，四个本批重叠文件变化，其余 88 个 SHA 不变，无缺失。HEAD/StormBreaker HEAD/status 不变；最后收口编译/游戏进程 0、用户目录编辑器 PID 45456 新启动并原样保留，不能称全相关进程 0；编译资源已释放。

## 2026-09-30：共享核心 W1-A 三份准备方案（纯文档）

- 按作者已经确定的 GPLv3/另行授权、共同规则/两个后端、War3RenderCore 唯一上游方向，完成 [A1 Footman 数据映射](../plan/2026-09-30-shared-core-footman-data-mapping.md)、[A2 某帧某实例格式与比对设计](../plan/2026-09-30-shared-core-native-frame-export-contract.md)、[A3 宿主适配层](../plan/2026-09-30-shared-core-warvk-host-adapter.md)；总回执见 [W1 准备](../SHARED_RENDER_CORE_W1_PREP_2026-09-30.md)。本轮没有复制 FORGE 实现。
- 校验边界：原/runtime geoset 和组 remap 分列；raw World 保留，未证明 W 组合不求逆；GEOA 与 draw alpha/ref 分列；最终来源被 native override 替换时旧证明不得沿用。宿主单一签发边界、CPU/实例/地图/设备/GPU 不同寿命设计与旧长期计划衔接，不改当前产品准入或渲染。
- 当前仅完成方案和源码/回执核对；parser/comparator、adapter、MinGW 核心构建和跨编译器 golden 对照未实施。没有新 DLL、编译、游戏采集、部署、提交或推送；B 等作者通知，实跑需作者在场另行安排，不能把本轮文档检查写成运行/视觉或收益证明。

## 2026-09-28：v1.22.02 RC1 干净构建与隔离回归（仅候选，发布门未通过）

- 从本主树源码与固定 gitlink 冻结外部镜像 `D:/WarVK-Builds/v1.22.02-rc1-20260927/source`，排除用户 dirty StormBreaker 工作树；镜像清单 SHA-256 `4C7707EB66D3FBA394DC414742A1983E25B8E44F715473EB73A7B94C1E0563BA`。独立测试/正式配置均为 Win32，开发取证与内部 recorder 关闭；BelowNormal、`-j2` exact DLL 构建 467 edge exit 0，随后 no-work。正式候选 PE32/i386、版本 1.22.02，36,149,331 B / `07D6AE38F4969580F15C0E2C6B5966A955EA303ABF6201200C5BED8A92EBFC23`。
- 断言开启 Meson 105/105，全量静态 279/279（冻结镜像 278，需 Git 历史的 geoset 身份脚本在原主树 5/5），帧证据 Python 56/56、截图 18/18、palette 生产往返 9907 检查/0 失败。正式配置审计 `ok=true`；以上是离线门，不是画面验收。
- 首次跨图实例已实际完成 epoch 1→2→3，但旧 runner 在 JASS 阶段 marker 出现时提前取相，误判 B/A-return 仍为前地图；保留失败原始回执，修正为**等待目标 epoch + shadow-ready 再截图**并使分析器核对全局 epoch 时序与截图 epoch。第二次全新隔离 2560×1440、同 PID A→B→A 严格分析 `ok=true`，请求/应用 0→1→2、旧会话 collected 0→1→2，三段尾部 caster/receiver 有效，零新 GPU event/incident，玩家 DLL 未动。持久 charge 与全视觉仍未证。
- `Hamg` Walk JASS 独立隔离回归：请求到达/接受、关闭后自动 Walk 被抑制、恢复后可出现；像素及其他单位/生命期未证。
- 高压图第一次未达 game-ready：继承的重型取证环境变量及中文 `-loadfile` 路径是可检风险，不归因成产品崩溃。第二次隔离实例内用原字节 ASCII 地图别名、清除继承的诊断变量，完成 180.803 秒/341 采样/38 节点，无 device lost 或 GPU incident，原 DLL/进程均恢复。**实际分辨率 1902×963**；6 个互不连续采样 `HasCompleteShadowMap=0`、`ShadowMapDrawnCasters=0`，其后下一采样恢复，未复现历史粘滞无影，但不能声称阴影全程连续或 2560×1440 压力门通过。
- 桌面 RC1 包 `C:/Users/Administrator/Desktop/WarVK-v1.22.02-RC1-20260928.zip` 已以 fail-if-exists 生成，**8,028,462 B / SHA-256 `71CBDD1943E88B0AF0D1DB7A4977C38BB112CE955A815F98E09BEFBA9D224686`**。包内 DLL 与未剥离候选原字节一致；7 个成员、每个 SHA、ZIP CRC、PE32/i386 和候选 manifest 审计通过，`releaseReady/productAccepted/stable` 等发布标志均为 false。包的首次脚本把含点的名称误用 `with_suffix` 生成 `WarVK-v1.22.zip`，随即修正脚本、将唯一生成档移到完整 RC1 名称并按新路径重审；未覆盖任何既有文件。
- 仍缺菜单返回/再进图、D3D9 Reset、持久账户及其他单位/死亡/句柄复用和前台视觉；`docs/RELEASE_1.22.02.md` 保持 `releaseAuthorized=false`。本回执不提交、不推送、不创建公开 Release、不覆盖玩家现场。候选包仅用于下一轮明确标注的测试。

## 2026-09-27：跨地图 Reset 触发缺口闭合（限定 A→B→A 候选，未发布）

- 09-25 的同 PID A→B→A 已证实地图切换但 `shadowMapEpoch` 恒 1、Reset 请求/应用恒 0。本批在真实 `Hook_InitJassNatives` 前接入一次性地图会话信号，连续 Native 重建合并为一次请求；`MainRunner` 入口作缺信号兜底，完成后重新 armed，已有 GameExit disarm。仅请求设备关旧 producer gate，由 Present 原有事务应用与退休；无预算/剔除/caster 政策变化。
- 当前 dirty 主树 exact DLL 以 Below Normal / `-j2` 构建 14 edge 并 no-work；PE32/i386 36,618,668 B / `83C1B7BE9E293A94C778FFEEE633C4A5F4089DD0C7AF6C136721FE866050B3E9`。生产共用状态机测试、接线静态 4/4、既有跨图静态 24/24、严格分析器反例 6/6 通过。
- 唯一隔离 2560×1440、零 global input、同 PID **12980** 实机 A→B→A：DBWIN 在 B/A-return 各 3 次 Native 初始化中分别只记录 1 次 Reset 请求；三段 epoch **1→2→3**、requested/applied **0→1→2**、旧会话 collected **0→1→2**，尾部两独立状态帧均 receiver complete/producer ready，三段 drawn caster >0；无新增 GPU event/incident，输入 SHA 不变，AutoTest 所属进程已停止，玩家 DLL 未覆盖。严格分析 `ok=true`，同时 `persistentChargeAccountVerified=false`、`fullVisualAcceptance=false`、`releaseAuthorized=false`。
- 完整输入、原始回执、截图审查范围和未覆盖门见[新候选记录](../research/2026-09-27-cross-map-transition-signal-aba.md)。本轮并未证明菜单换图、D3D9 Reset、高压持续性、全部像素或物理显存收益；不提交/推送/发布，已发布版建议不变。

## 2026-09-25：真实同进程 A→B→A 换图门未通过（仅测试/诊断，未改产品）

- 按用户要求自行设计并监督跨地图实机门。将两张已有编辑器保存地图的**副本**插入限时 `ChangeLevel` 与私有 GameCache/Preload marker；PJASS、MPQ 原脚本/候选读回 SHA 闭合。AutoTest 用单一实例/隔离桌面/零全局输入，在相同 War3 PID 完成 A→B→A，2560×1440 三张截图、三份 marker 与源文件三重身份留证；前台编辑器和玩家 DLL 未覆盖。
- a 次发生 runner 假阳性（嵌套 `runtimeStatus` 解析错误，采样 0），当场撤销。b 次只读到管道子集，缺生命周期字段；严格判失败。c 次启动即 perf 记录停在约 101 帧，按精确进程身份清场，不能归因于换图。d 次有效样本 PID 58416、A/B/A 返回约 1.004/25.140/48.698 秒；三段独立状态帧 3/2/3，尾帧接收完整且 caster 绘制 146/147/148，无新增 GPU event/incident。
- **阻断发布的真实反例**：`shadowMapEpoch` 三段恒 1；`shadowMapResetRequestedSerial` 与 `AppliedSerial` 恒 0；retired/collected 恒 0。地图切换发生，但新 M3 退休事务没有进入。这与 CPU 106/0、接线 80/0、静态 278/278 的离线通过并不矛盾。当前 Reset 仅从 `NET_EVENT_GAME_LEAVE` 注册链发起，而该单机 `ChangeLevel` 测试的 NetEventHook 启动 ready=0；事件缺失的精确原因待验证，不能武断等同于一个 Game.dll 函数错误。
- 冻结回执、指标及完整失败列表见[实机门](../research/2026-09-25-cross-map-aba-runtime-gate.md)；新增独立 strict 分析器及 6 个正反例用例，`ok=false` / `releaseAuthorized=false`，不会把 marker、截图或重复状态读数单独算通过。原始视频/帧画面没有全面视觉验收，活跃 charge/perf 报告也未取得。本批不改产品源码、不构建、不部署、不提交/推送/发布；修复下一步必须补世界生命周期请求，再同 PID A→B→A、设备 Reset 与压力/尾延迟分别验收。已发布版“每张地图重启进程”建议继续保留。

## 2026-09-25：Walk API 玩家无效回归——直接 CUnit handle 表项（隔离功能候选）

- 只读核玩家现场：`E:/Work/Warcraft III/d3d9.dll` 与 09-24 构建 SHA 相同；地图新保存脚本确实调用 `WarVKSetUnitWalkLoopEnabled(gg_unit_Hamg_0033, false)`。原地图/DLL 未修改。该触发没有播放替代动画，这与“关闭自动重选”不同于“立刻停止当前 Walk”的 API 语义需分开。
- 新增可重复的单进程隔离桌面自动化：从玩家地图的**副本**制作 PJASS 与 MPQ 读回一致的计时移动/Stand/恢复探针，在已有 AutoTest 实例中启动；内部测试快照分列 JASS setter、精确自动 Walk caller、抑制数、失败码与当前属性。原候选实测 DLL 已装、JASS 请求已到达，但 2 次 setter 均失败于 handle 类型检查，5 次自动 Walk 请求全未拦截。
- 根因证据：该 `Hamg` 的 handle 表项直接指向 CUnit（rawcode `Hamg`，虚表 `+0x8C`=`Game.dll+0x67BB90`），不是原实现唯一接受的 CAgent→CUnit。修复只给具名 direct-CUnit 证据开门，CAgent 路径不改，其他地址仍 fail-closed。生产共用 predicate 宿主测试与 2/2 Meson 定向通过。
- 修复候选 36,618,365 B / SHA-256 `1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF`，PE i386。两次 fresh-process 隔离测试均 game-ready、零 global input、源身份不变、零游戏残留；run c 的关闭/恢复 2 次请求均接受，576 次自动 Walk 请求中关闭期 574 次抑制，之后全局计数 2 次未抑制；run d 独立重复为 536/534/2。末尾 2 次未附对象键，不能证明同一个 `Hamg` 已恢复。两张最终帧只作图像样本，**不**冒充动画序列的像素验收。完整身份、证据路径与剩余语义见[本次诊断](../research/2026-09-25-unit-walk-japi-runtime-diagnosis.md)。
- 玩家 DLL 仍为旧 `FF3B…`，未部署修复、未修改用户地图、未提交/推送。其他单位、移动途中调用时已经在播的 Walk、跨图/句柄复用/多人及真实玩家视觉仍未验收；本候选不得晋升稳定。

## 2026-09-24：单位动画 API 作者菜单与正式启用边界

- 修复上一轮遗漏：`WarVKSetUnitWalkLoopEnabled`、`WarVKClearUnitMovementAnimationOverride` 现登记在 `WarVK/action.txt`；两个返回 boolean 的播放 Override 助手登记在 `WarVK/call.txt`；`define.txt` 增独立“单位动画”分类，`README.md` 补入口和边界。上一轮仅有 JASS 包装，故旧 YDWE 菜单确实看不到这些函数。
- 按作者决定移除 `DXVK_WAR3_UNIT_WALK_API` 环境门；支持的 Game.dll 初始化时尝试安装 Hook。目标序言和两处移动回调指令仍须精确匹配；不匹配即不安装、功能位不宣称可用，原游戏路径不变。单位未明确调用关闭时不会拦截 Walk。**环境门移除不代表游戏行为验收已经通过。**
- 验证：目标 YDWE 自带 `triggerdata.lua`/writer/WTG checker 以本树 WarVK 层覆写加载，`databaseLoaded=true`、`wtgAccepted=true`、`unknownUiRecoveryUsed=false`；JAPI 集成静态 20/20、安全 5/5、模型灯兼容静态 7/7；两项 Meson 2/2；1.27a PJASS 13,628 行解析通过；Below Normal / `-j2` exact DLL 构建 4/4，随后 no-work。DLL 36,612,087 B / SHA-256 `FF3B7BD521F95935DB204EF2FEBAB828BDE85BA743E509292B9FA9F91B05F003`，仅为本脏树离线候选，非发布 DLL。
- 安装边界：当前 YDWE 编辑器进程仍在运行，`E:/Work/War3/YDWE1.32.13 - MemoryHack/ui/WarVK` 是旧副本；本轮没有热覆盖该目录、没有触碰玩家 DLL、地图或游戏。关闭并重启编辑器且把完整 WarVK UI/JASS 层更新后，作者菜单才会刷新。真实移动动画、停止移动、死亡/销毁、换图与高压地图仍待实机验收。

## 2026-09-23：单位移动 Walk 自动重选开关（默认关闭的离线候选）

- 用户希望单位移动时可暂时不循环 Walk，而播放指定动画；DSH 以 `commandcode/deepseek/deepseek-v4.1-flash` 完成初查，主线程在 IDA 独立复核 `Game.dll` 两个移动回调、原版动画 Native 分离、虚表目标与浮点返回 ABI，并把三个关键地址的证据边界写回 IDA 注释。精确地址、身份、推理与未覆盖项见[逆向记录](../research/2026-09-23-unit-walk-animation-reverse-findings.md)。
- 新增 JAPI `WarVKSetUnitWalkLoopEnabled(unit, boolean)`，以及按名称/序号播放的 Override 组合助手和 Clear；原版 `SetUnitAnimation`、`SetUnitAnimationByIndex`、`QueueUnitAnimation` 未 Hook。仅显式 `DXVK_WAR3_UNIT_WALK_API=1` 且两处调用指令与目标序言匹配才安装；默认原游戏行为。关闭自动 Walk 还须验证实时 handle→agent→unit 身份、单位存活和两个精确调用返回地址，失配回到原函数；JASS VM 重建清除状态。
- 验证：Win32 DLL Below Normal / `-j2` 构建 exit 0，exact DLL no-work；Meson `war3_japi_v1_protocol` 与 `war3_unit_walk_policy` 2/2；JAPI 集成静态 19/19、安全静态 5/5；1.27a `common.j` + `blizzard.j` + 当前 `warvk_api.j` 的 pjass exit 0。曾因新命令把参数前缀写进命令名而使集成静态 1/19 失败，已改成既有分段序列化并复测；没有隐藏红门。构建态 DLL 36,612,087 B / SHA-256 `6B4C6118BB0B954D268F0648E07CD0F24AE30274987C14EE9F260F67D75872B6`，它还含本工作树其他未提交改动，**不是发布或玩家测试包**。
- 游戏内 Hook 命中、单位动画持续性、死亡/销毁/复用、换图和高压地图仍未验收；不能声称玩家可用或进入正式配置。没有部署、启动游戏、关闭编辑器、提交或推送。本轮也修复 DSH 启动后令牌与 MCP 的动态交接（独立 DSH/dsh-mcp 工作树，HKCU 用户环境；新 MCP 进程验证成功），当前 Codex 任务中的旧 MCP 进程需重连才读取新配置。

## 2026-09-22：R4/M3 换图资源承接与 live/retired 分账（离线候选）

- 用户批准下一步攻克换图，并顺带推进长期重构。本批只处理已确认的 persistent 会话账和退休承接，不扩大 cap、不减少 caster、不改默认剔除、不引入 64 位或另一个生命周期 Manager。
- **源码问题与修复**：旧 Reset 搬走 geometry/registry/expiry 后账仍留在单一 bytesUsed，退休收集也不扣账；现在分 live/retired，准入始终看两者总额，完成序号到达后只扣对应 session 的 charge。另把退休队列改为先准备稳定 list 节点、再进行无分配的 owners+账共同提交，避免资源搬入局部对象后 vector 扩容失败丢失保活。零收费 alias 仍按原 fence 保活；本账不是实际物理显存，现有 live GC 减账与外部 Rc 保活区别保留。
- **失败恢复边界**：map/device 准备失败不进入 quarantine、不推进 applied；提交后 CPU 清理若失败，已退休资源仍有 owner。独立审查发现 tombstone 失败重试可能被提前开门/Populate 早退破坏，已补 pending latch、最后清理成功才解除、两处 reopening 拒绝 pending、Present 独立重试及 Populate 下游前再次关门。
- **长期重构推进**：`war3_shadow_session_retirement.h` 是设备实际使用、CPU 测试共用的唯一 prepare/commit/collect 规则；11 组 owner 的承接由真实适配层和 no-throw 条件约束。Present 同次发布 total/live/retired，性能 summary 增两个 Last 字段、workloadSeries 尾部增两列，不挪旧索引或与 Arena/别名账相加。
- **离线证据**：生产共用事务 **106 检查/0 失败**（真实 STL/失败 allocator、连续失败后恢复、A→B→A 账目模型、部分回收、零 serial、近 UINT64_MAX、保活）；生产接线 **80 检查/0 失败 + 12/12 内存变异被拒绝**；报告静态 6/6。断言开启 Meson **103/103**，静态最终 **278/278 脚本 exit 0**。首轮静态 **277/278** 的旧 direct-move 形状断言失败原样保留，已收紧到实际 transfer/commit/clear 顺序。旧 A9/M2-5B TEMP 独立重抽仍缺，本轮新 EXE probe/动态差分已跑，不误报全部历史子门通过。
- **构建与身份**：固定依赖镜像 **3951 文件**；最终清单 `E:/WarVK-Builds/v1.22-maintenance-20260922-r3/source-review-final.json` / SHA `AEABBF5BE92C6FD2ED3EABAF271A4EA507BEA1D7F65A5D47FCD81DF4CC1919D7`。产品/CPU 源码冻结后未变，仅刷新两份 Python 静态脚本；镜像逐 SHA 0 差异，收口前 2910 个非归档依赖主树文件逐 SHA 一致。BelowNormal/-j2 exact DLL **466/466**、exact no-work、player-release 配置校验、PE `pei-i386/i386`、py_compile 通过；编译警告保留。
- **未剥离候选**：**36,125,214 B / `7ECA06A72C480A4943029CDA1C6CB98B3B32AE64EB25CC964B2BACB3FF396642`**，CreateNew 等价原字节冻结 `E:/WarVK-Builds/v1.22-maintenance-20260922-r3/candidate-7eca06a7/d3d9.dll`。CPU JSON SHA `3CA30507B216E7B486CF1963A9850D70F111DE6D0F7ABEDB152FE98AA4E987E2`；静态摘要 SHA `27F61CA1C3E9B1282825C91B229FD961B831AEFF07B77F7DD2AAAE81EE2E218B`。完整终检在同目录 `final-verification.json`，随后只补本日志/路线图/本批研究回执。
- **资源与验收边界**：编译资源已释放，终检编译/War3 为 0；玩家 DLL 仍为 `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`，编辑器 PID 36012 保留，用户 StormBreaker dirty 未用于构建。没有部署、游戏、提交、推送或发布。本批 CPU A→B→A 模型不等于游戏换图，未宣称已修复某份玩家日志、FPS 改善或全面恢复；真实同进程换图/设备 Reset/压力与 GPU/尾延迟仍待验收。
- **后续**：完成本批实际 A→B→A 验收；源码继续 GC expiry 异常边界、Registry/domain 账户/键，然后单条 CurrentDraw/M4。跨 Pass 统一职责和 64 位未完成；64 位继续延期。详见[会话退役合同](../research/2026-09-22-persistent-session-retirement-accounting.md)与[路线图 §8](../plan/2026-09-22-post-v12201-roadmap.md)。

## 2026-09-22：R4/M3 第一切口——persistent geometry 发布事务（离线候选）

- 用户要求按长期重构继续推进。本批把 `War3CreateShadowPersistentGeometryAfterMiss` 的 geometry map、registry、expiry heap、字节账发布收进生产与 CPU 测试共用的 `PublishPersistentGeometry`，不是另建 Manager 或仅搬文件。域准入、预算、GPU 创建、GC 与退役仍由原设备所有者负责；无新默认开关、无 64 位/Water/自动模型点光变化。
- **先红后绿**：原发布顺序抽取后，真实 STL node/bucket/vector 分配失败注入得到 345 检查 / 106 失败 / exit 1；改为新节点可回滚、旧槽最后覆写、heap 成功后再加账后，同一测试 **345 检查 / 0 失败**。含新 key/已有槽、逐分配点、连续失败后恢复、ID 不回退、原槽地址/全字段/heap序列、未知异常回滚重抛和外部强引用保活；这是 CPU 证据，不是 GPU/玩家根因证明。
- 成功路径采样：新 key 5 次容器分配、旧槽 3 次，与抽取旧序列一致；没有新增分配类别、锁或遍历，不据此宣称帧耗时等价。事务起点在 GC **之后**，不恢复已合法淘汰的资源；rollback 可保留 bucket 高水位，不能称物理显存或 CPU 内存完全复原。
- 新静态接线门禁 **9/9**；反例脚本对照通过、**14/14 变异被拒绝**，回执 `E:/WarVK-Builds/v1.22-maintenance-20260922-r2/static-mutations/shadow_geometry_publication_mutation_results.json` / `2EBA55DDFEC10DAA440C4CEE9A5B2BF2CD4439115529CCA53F9CB242EDE5A9D4`。独立只读审查确认当前 STL 与生产类型前提成立；模板并不为任意自定义容器提供异常保证。
- 新固定依赖镜像 **3946 文件**，清单 `source-review-final.json` / `6487DCB36F9EFD607332C40FB9BD67F7FFB5A1087C42A1ED1D9832881FD23EA5`，逐 SHA 复核上一镜像后创建；旧候选/回执不覆盖，用户 StormBreaker dirty 不读取构建。全量 CPU/静态及产品构建结果待本条后续收口，不预先计为通过。
- **本批收口**：全新断言开启 CPU **102/102**；静态首跑 **274/276**，两项旧门钉住 device 内直接插入，已适配并加强为唯一事务入口/domain 同源值/四流绑定顺序，13+7 定向通过后全量重跑 **276/276 脚本 exit 0**。首跑失败记录保留，未追认；旧 A9/M2-5B TEMP 原始快照重抽子门仍跳过，不能称全部历史子门通过。最终静态摘要 SHA `E3C3628B8AA38FA670E9D8DD5978E97202FC867AF14907E3D41A136BF81B5E5C`；Meson JSON SHA `B78510E19047B5299F539B9A558F2B34D6B4186DE473651D1140508573C0AC66`。
- BelowNormal/-j2 exact DLL 干净构建 **466/466**、exact no-work、正式配置 verifier `ok=true`、PE `pei-i386/i386`、py_compile 与 diff 空白通过。未剥离候选 **36,157,944 B / C65CA2B2FB755AE78D5A65455A49D04577839AB896E5E70A3D043B147CFC4F33**，原字节冻结 `E:/WarVK-Builds/v1.22-maintenance-20260922-r2/candidate-c65ca2b2/d3d9.dll`。最终输入清单 `source-final.json` / `07B38D73E216ADEBAB48FCE4EE59FFBBB79217AEDA1D51CBE05D3AD44C8B7762` 仅更新两份 Python 门；3946 文件逐 SHA 0 差异，产品输入不变。随后只补本批三份文档回执。
- **编译资源已释放**；终检编译/War3 进程 0，用户编辑器 PID 36012 保留。玩家 DLL 仍为 `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`。本批未部署、未游戏、未提交/发布，没有新 GPU/画面或性能结论。
- **下一独立问题**：Reset 将 persistent 几何移到 retired 后，字节账未随其转移、收集时也未扣除；可能在旧资源已释放后继续占用新地图准入额度。已在研究文档登记为源码风险，尚无运行复现；下一批先定义 live/retired 账户与 GPU 在途成本，再做生产生命周期故障注入，不能简单清零或扩池。M4、跨 Pass、实机和长期性能仍未完成。
- 详见[事务合同与验证](../research/2026-09-22-persistent-geometry-publication-transaction.md)。本批不部署、不启动游戏、不提交/发布；玩家 DLL 和用户编辑器保留。

## 2026-09-22：发布后首批维护——恢复证据按尝试闭合、截图 I/O 取消交接（离线候选）

- 用户授权先解决问题和渐进式重构、后做 64 位。主树仍为 `codex/v1.22-main-tree-20260920` / HEAD `a2054a9776e40edc3f01f6f390c06037965b8ef2`；本批不修改阴影选择、caster 准入、剔除默认、预算上限、GPU fence 或已发布附件，未启动 64 位产品接入。
- **恢复证据不再跨尝试拼接**：真实记录器将累计阶段观察与当前 `attemptSerial` 的证明分离，只有同一已知尝试内、来源/hit 一致且有序的 Served→Enqueued→Drawn 才可闭合；未知、A→B→A、清空来源与错序不能借旧状态认证。同帧合并仅限无中间推进的等价邻接事件；回退阶段/冲突可见且使当前证明失效。终态携带当前尝试与真实 draw 载荷，不借用旧 draw。外层 schema 7 不变，palette 扩展 v5 使用 `data[5]` 传完整 64 位尝试号，旧 v1–v4 仍严格读取但缺尝试证据时不能认证同对象恢复。
- **截图退出可请求取消私有文件 I/O**：保留 `std::thread`，worker 复制自己的真实线程句柄并发布；关闭方在私有文件阶段请求 `CancelSynchronousIo`，始终等待实际 join 后才关闭句柄/释放对象。取消成功、未找到 I/O 或查询失败均不当作完成；句柄复制/观察失败回原 join。Logger 不在可取消阶段，join 后未终结的历史请求明确失败完成，不伪造 GPU 已退役。驱动拒绝取消、rename/元数据/Logger 停滞仍可阻塞，不承诺硬退出时限。
- **成本边界**：记录器固定表从 289,064 到 370,984 B（+80 KiB CPU 诊断状态），没有逐事件堆分配、额外矩阵扫描或渲染锁；该子门仍默认关闭。截图门只在保存/退出使用，不把 I/O 放进 Present。未测游戏帧时或尾延迟，不宣称 FPS/显存收益。
- **统一离线验证**：断言开启 Meson **101/101**；最终真实记录器 45/45、跨尝试反例 **64** 检查、截图生产共用控制 122 检查、记录器→转换器→环→导出→正式读方 **9907 检查/0 失败**。两个 Python 正式读方 **192 测试**通过，相关 py_compile 与 diff 空白检查通过。截图 CPU 测试包含三轮实际阻塞同步管道 WriteFile 取消，但未实例化完整 Vulkan/游戏 `Impl::save`，不是慢磁盘实机验收。
- **静态回归 275/275 exit 0**，并单列 A9/M2-5B 两项旧 TEMP 原始快照缺失造成的“独立重新抽取”未覆盖；它们的本轮新编译 probe/动态差分已实际运行。首轮静态 267/275（两个旧版本形状断言和六项环境依赖失败）原回执保留；修正版本钉点、接入本轮新 EXE/只读历史查询后重跑，不弱化缺链和来源门。另有一次配置编排把 Meson stderr 进度当 PowerShell 异常，保留不完整目录并用新 `build32-tests-v2` 完成配置，未伪装为首次成功。
- **源码与构建闭合**：仅从固定 gitlink 导出依赖，排除用户 StormBreaker dirty/旧 build32。清单 `E:/WarVK-Builds/v1.22-maintenance-20260922-r1/source-final.json` 包含 3942 文件，SHA-256 `64153D3191F807A467578847E53AD6E598B845BA8C003270FAEBA137A8334E53`。构建后镜像逐 SHA 0 差异；2901 个非归档依赖主树文件仅路线图后补文字不同，随后只补本回执与研究文档。BelowNormal/-j2 exact DLL **466/466** exit 0、exact no-work、player-release 配置审计通过；编译警告留在日志，不声称零警告。
- **未剥离候选**：`E:/WarVK-Builds/v1.22-maintenance-20260922-r1/build32-product/src/d3d9/d3d9.dll`，PE `pei-i386/i386`，**36,156,663 B / 8238712B18619647801223F7EEFA639972CC640081B34EDC6138A9522727988D**。首次静态回执 SHA `8223591394DC4BF8BD1D3BC4E60A06AB312E6A51FCF425563ECFCBB1D41E9889`；首次 Meson JSON 回执 SHA `0FD35AD44E479F6358F72FFAE890231523EB8433066631A057B4F90F8EE52872`（后者保留在 `pre-review-test-receipts`，最终见下条）。
- **最终反例复核追加**：独立复核发现读方仍用“全链首次 S/E/D”顺序误拒旧 attempt 缺 S 或 E 后的新完整重试。本批修正 v5 RejectionRecovery 读方，顺序与 D 见证由当前 attempt 证明负责，历史真正降秩保持粘性；v1–v4 和 v5 Observation 原规则不动。真实往返新增 M/N 正例、O 真降秩负例，追加 4 个测试构建 edge 后重跑上述 101/192/275 与 9907 检查全部通过。只刷新五个读方/测试文件，DLL 产品输入与 SHA 不变，exact no-work 再确认。
- **最终收口清单**：`source-review-final.json` **3942 文件** / SHA `CCD6435E1E8E703BB0B8F31F107C3DE4ED093F6D7A58BA7C63539AEBDBCD25FE`；镜像逐 SHA 0 差异，主树差异仅四份收口文档。最终 `static-results-review-final/summary.json` SHA `885D66330BAC24399863099F0B01785437E30C7375FD462105203091E3110D1B`，来源清单/三个动态 EXE 前后不变；最终 Meson JSON SHA `3AC773B1A1DDA31355EECF1BD41925AC32782ADA79D73D68363CB8F726DB3157`。生成清单时曾因 Windows PowerShell 默认编码误写两个依赖文档的中文路径，原清单保留为 `source-review-final-before-utf8-correction.json`，已修正路径并全量复核；没有改动相应依赖字节，测试回执记录的是纠正前清单哈希，不能混写成纠正后身份。
- **没有部署、启动游戏、提交、推送或发布**；玩家 `E:/Work/Warcraft III/d3d9.dll` 仍为 `028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3`。终检编译/War3 进程 0，编译资源已释放；用户编辑器 `worldeditydwe.exe` PID 36012 已于 15:57 启动，原样保留，未操控或关闭，不把它报为“所有相关进程 0”。玩家视觉、压力恢复、A→B→A、正常/慢盘真实截图退出、GPU 与尾延迟仍需独立验证；生产未知身份/尝试仍不得认证。
- **下一批 R4/M3**：已定位 `War3CreateShadowPersistentGeometryAfterMiss` 中 geometry map→registry→字节账户→expiry queue 的部分发布风险，先做实际生产函数分配失败注入及事务回滚，再做单条 CurrentDraw/M4 交接。该风险是源码审计发现，尚未复现为玩家崩溃，本批未修改此路径。64 位、Water 与自动模型点光继续延期。
- 详见[恢复证明合同](../research/2026-09-22-palette-attempt-recovery-contract.md)、[截图退出合同](../research/2026-09-22-screenshot-synchronous-io-cancellation.md)及[后续路线图](../plan/2026-09-22-post-v12201-roadmap.md)。

## 2026-09-22：发布后路线图核对（仅计划，不修改产品）

- 以主目录 HEAD a2054a9 / 已发布 v1.22.01 为基线，核对源码、发布范围与历史长期计划；开发前只有用户 StormBreaker dirty，原样保留。
- 新建 [后续路线图](../plan/2026-09-22-post-v12201-roadmap.md)：先稳定基线与跨 attempt/慢 I/O 维护，再根据实测做采集/复制减量，逐链收口 Registry/M3、CurrentDraw/M4 和后端决策；配置/API 覆盖可离线并行，提前剔除需独立证明。
- 动态预算、快照切片复用、Arena 缩页、S2 局部成本及正式整合已实现，不重复列为未做；区分剩余实机门与未实现职责。旧 A9/A10、Registry“未导出”和09-19百分比不能直接当现状。
- 本轮仅文档整理及只读复核，未重跑测试、构建、部署、启动游戏、提交或推送；没有新增性能收益/视觉修复声明，稳定 CHANGELOG 与发布附件未改。

## 2026-09-20 21:29 台北：v1.22.01正式发布完成

- 源码提交7e281568862ba40de3be07e98d108c20144b93e7（63文件/+1943/-313）与新annotated v1.22.01已atomic、fast-forward推送origin/main；无force、未改旧版本。Release ID392439670于2026-09-20T13:29:12Z公开，draft=false、prerelease=false、latest=v1.22.01。
- 四附件远端uploaded且size/SHA与本地一致：DLL31,221,683B / 5C6AEEDF04BB1379969279033F0A9A556173D9A84F763FD09C88557CCD2A9AF3；玩家ZIP7,275,748B / 3B565191E76707127AD2687CE614DA7B355EC3C256A098DADC145112026DB924；作者ZIP94,794B / 4F99F4EB631E42D21A97990777FEBCC67EB9F87B20D8856801664CE404B9662B；摘要文件260B / D9C2F56CE1E4D395A2E27E09A1DBFE093B6219F02CBE9135106CBDA617702C7F。包成员/CRC/字节与两次独立strip重复性通过。
- 过程保留：首个实际打包因产品b_ndebug=true而打包器要求if-release被拒；统一发布配置后重新生成/no-work/配置审计通过，未剥离DLL仍为同一D7A54092完整哈希，不放宽包门禁。草稿按tag查询404，按列表解析唯一release ID复核，无重复创建；摘要校验后才公开。
- 断言开启CPU99/99、静态275/275及20/9/25发布相关测试沿用上条本轮实测；对应候选有玩家单轮反馈，正式版本增量未重新实机，不扩大为完整GPU/跨地图/长期接受。已知限制随中英README/CHANGELOG/Release一同发布。
- 玩家DLL保持028565BC候选，未自动覆盖；编译/游戏进程0。StormBreaker原dirty保留、未提交或构建。发布包与本地回执在D:/WarVK-Releases/1.22.01-20260920；本条成功回执随后单独提交，不移动已发布标签或重打包。
- [正式Release](https://github.com/CallDisaster/War3VK/releases/tag/v1.22.01)。后续优化另起批次，不继续扩大本补丁范围。

## 2026-09-20 1.22.01玩家反馈与修复版发布准备（尚未推送）

- 最终版本标识构建已通过：断言开启CPU99/99、全量静态275/275、正式配置20/20、包工具9/9、离线包读方25/25、py_compile/diff空白；exact DLL编译链接及no-work通过，PE32/i386、版本1.22.01、PRERELEASE=false。未剥离DLL36,065,732B / D7A540926C02904E96F25ADE9D643B0E768B711CC41EE5A12EFC0A87400C1BC3。与已试玩source-adaptive-r9相比，产品输入变化精确限7个版本/探针/对应测试文件；内存算法无新增变化。当前仍未上传，不把本条当成功回执。

- 用户测试上一轮028565BC候选后反馈没有观察到问题，并授权主线程决定继续优化或推送修复版；选择冻结修复范围，继续优化留给后续版本。
- 只读核玩家DLL为36,065,732B / 028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3，等于交付候选；无相关游戏/编译进程。未要求重装、未覆盖现场。玩家反馈仅一轮，无新报告/完整矩阵，不升级成全面GPU或性能证明。
- 正式增量限1.22.01产品/JAPI版本及匹配探针、测试版本断言、中英README/CHANGELOG/范围文档/打包清单；分配、caster、fence算法不再新增修改。旧1.22.00历史、标签/附件不动。Shader ABI与JASS wire不变。
- 计划用固定依赖镜像，重新跑产品构建/no-work/配置、断言开启99项CPU及275脚本；再提交白名单源码，冻结对应包，校验远端附件后公开。此条为准备状态，不是GitHub成功回执。

## 2026-09-20 20:42 自适应预算与切片退役：离线候选收口，未实机

- 最终输入 `source-adaptive-r9.json`（SHA-256 `B767B06C64A39A6B2C16C3306AFBDCAA4E3D5FDA23340E3E949C08BD7197E2B2`）含 2892 文件；构建后主树与冻结镜像逐 SHA 复核 0 差异。随后仅补本日志与研究文档回执，不回写构建输入。保留用户 StormBreaker dirty，构建只用原 gitlink 固定依赖。
- 断言开启 CPU Meson **99/99**，静态脚本 **275/275**；自适应/切片生产组件测试 **23,965** 检查通过，含真实 DXVK object tracker 与模拟完成、10,000 次跨线程最后引用释放、24 页同类小锚点负例、满表退化和所有消费者接线。并非 GPU 执行或视觉证明。
- 新增同帧扩页失败负缓存：只抑制同帧同等/更大请求的重复预算查询，较小请求与下帧可重新查询，既有尾部/已退役空洞照常可用，Reset 清除。组件测量：10,000 次 claim/release/hole 102 us；满 2048 槽重复失败查找 1000 次 50 us，测量窗口堆分配为 0。不是渲染帧耗时，也不声称 command-list tracker 的所有操作零分配。
- BelowNormal/-j2 的最终 exact DLL 增量 **40/40**、no-work、player-release 配置审计 `ok=true`、PE `pei-i386/i386`、相关 py_compile 与 diff 空白检查通过。最终未剥离候选 **36,065,732 B / 028565BCB33B6EB646E4915115B60CDA3C14C287A4225DAA50F8F6513F9E9BA3**；CreateNew 等价原字节冻结于 `E:/WarVK-Builds/v1.22-maintenance-20260920-r1/adaptive-candidate-028565BC/d3d9.dll`。
- 动态的是各池扩页额度，不是整机显存硬隔离：使用实际 heap budget、物理 commitment（含 DXVK 缓存后备）和 Win32 VA 余量。快照在可信预算下默认最多 512 MiB，显式配置上限仍生效，不支持预算时保留 384 MiB fallback；这改变旧默认 384 的增长策略，并非把上限提升当作钉页修复。Arena 每代 384/总 1152 MiB 仍不变。预留余量并不保证驱动永不 OOM。
- 最高优先级同类钉页已接入切片级租约/空洞复用，覆盖缓存、CS 复制、surface/volume/point shadow、轮廓、GPU 输入取证、同步 Shader 回调。只在最后消费者完成后复用；不搬移存活字节、不放宽 caster 完整性。元数据耗尽会封页回退原 append-only/整页退役，仍可能保留碎片；真实活跃集合超过预算也未被消除。
- Arena 只在已完成且 retireSerial 非零的代际切换点按 120 帧需求窗口缩空闲尾页，每次最多一页。逻辑退役不承诺驱动物理显存立即下降；Render Stats 明示“最近扩页额度”，不是连续显存采样。
- 未部署、未启动游戏、未提交/推送/更新稳定 Release。玩家 DLL 仍为 **31,213,491 B / 62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3**；收口相关编译/游戏进程为 0，编译资源已释放。待独立隔离 2560×1440 压力/恢复/跨消费者/GPU incident 与玩家阴影视觉门；不宣称本候选已解决现场所有无影/撕裂或提升 FPS。
- 前批 Image 异常清理、D3D9 小尾段/溢出、截图按需分配维护包含在本组合并随本轮回归；跨 attempt Recovered 与同步慢 I/O 关闭边界仍未处理，不将它们列为完成。完整契约与日志索引见 `docs/research/2026-09-20-adaptive-budget-and-slice-retirement.md`。

## 2026-09-20 自适应预算与切片退役：开发中 CPU checkpoint（历史中间态）

- source-adaptive-r6 CPU Meson99/99，切片测试23,956检查（真实DxvkObjectTracker、模拟完成，不是GPU）；r7静态275/275与DLL编译通过。满2048槽重复失败查找1000次由r5约37,649us降到r6约49us（负结果缓存随申请/释放失效；不是游戏帧时对比）。表为81,960B/页，上限32页约2.50MiB元数据；不声称DXVK批量tracker也零分配。
- 最终全消费者审查又补轮廓两路径、GPU输入采集和外部Shader同步回调的切片保留；剥离CPU保留记录时同时清租约；Arena缩页增加正数retireSerial门。这些后续修改待最终r8组合重跑，不继承r7通过。
- 过程复核：切片组件扩至20,895项检查后通过；Arena首轮编译因声明遗漏失败，第二轮又发现误插到普通Reset的缩页块（未定义frameSerial）失败。已删除Reset中的缩页，加入唯一BeginFrame缩页守卫；第三轮组合DLL编译通过。这些是中间构建，不追认为最终通过。继续统一Arena纯策略和测试、核算满表成本；未进行GPU实机。
- 主树基于 24140e4；保留前批维护改动与用户 StormBreaker dirty，不部署、不修改稳定发布。
- 统一实际 heap/physical commitment/VA 增长许可；快照切片租约随缓存、CS 复制和三类阴影消费
  进入现有 DXVK command-list tracking；元数据饱和退回原 append-only 页，不新增 caster 拒绝。
- Arena 仅在已退役代际的 owner 切换点按需求窗口减空闲尾页；保持每代384/总1152MiB工作界限。
- Win32 O2 生产组件/Rc CPU测试893检查通过；接线守卫5/5。首编译缺 util_likely include 已修。
  这些测试不运行GPU，不代表物理回收、阴影连续性、性能或跨地图验收。
- 详见 `docs/research/2026-09-20-adaptive-budget-and-slice-retirement.md`；组合构建与更多反例待完成。

## 2026-09-20 发布后内存维护：首批实现与定向回归

- 最终离线收口（source-revision3）：断言开启的 Meson **98/98**、静态脚本 **274/274**、py_compile/diff 空白通过。原文 Image 函数+Vulkan 替身 70 检查、真实 Win32 allocator 139 检查、原文截图函数+资源替身 62 检查、截图 core 100000 次并发交接通过；这些不等于 GPU/玩家验证。最终截图回收放在现有设备锁内的 beginPresent，覆盖 skipped/unsupported Present，仍不回收 Submitted/Quarantined。
- 独立固定依赖构建首次 exact DLL 466/466 完成，后续最终源码增量重生成/编译/链接通过；exact DLL no-work、player-release 配置审计 ok=true、PE pei-i386/i386。未剥离候选 **36,051,519 B / 76500A3E75BF9764D059ED7CD9EF5E4DBF5FD7F05DEE545907FB3E44EC74590C**，位于 `E:/WarVK-Builds/v1.22-maintenance-20260920-r1/build32-product/src/d3d9/d3d9.dll`；14 个变更源码/测试路径逐 SHA 与 source-revision3 一致，最终文档回执不回写源码快照。既有 StormBreaker dirty 未参与构建，未修改。
- 中间失败保留：首次显式测试目标列表漏编 wire 往返工具，完整测试为 96/97（缺 executable），补建该 exact 工具后重跑通过；最终又加入截图 core 标准用例，形成上述 98/98。第一次产品配置审计因 strip=false 未过，显式改为 true 后重新构建与审计才通过，不追认失败为通过。
- 同寿命钉页负例共 3224 检查通过的含义是**负例被正确复现**：6144 B 的存活锚点仍可钉住 384 MiB 模型池，打印 UNRESOLVED，不是修复完成。跨 attempt Recovered、同步慢 I/O 关闭也仍待后续批次；无请求截图避免的 42.1875 MiB 仅指 1440p 三槽逻辑后备，不是实测 VA/显存释放量。
- 收尾玩家 DLL 仍为 **31,213,491 B / 62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3**，未覆盖；游戏/编辑器/编译相关进程 0。未实机、未提交、未推送或更新 Release，已发布 v1.22.00 和稳定 CHANGELOG 保持不动。**编译资源已释放；游戏现场未触碰。**完整路径与验收范围见 `docs/plan/2026-09-20-post-release-memory-maintenance.md`。

以下为本批早期 checkpoint；最终结果以上述收口为准，历史失败不删除：

- 用户授权修复后，先落三项 P1：未移交 VkImage 的统一作用域 guard、Image 构造完成后登记；D3D9 CPU chunk 保留小尾段并前置拒绝零请求/对齐溢出；异步截图将请求意图与后备分离，首次真实请求才建槽，Retired 经所有者 CAS 回收（Submitted/Quarantined 不回收）。未改 caster/快照池/Arena 预算、未覆盖玩家 DLL、未提交或发布。
- 首个独立测试快照（source-revision1）：新增原文 image 函数/Vulkan 替身故障注入、真实 Win32 d3d9_mem.cpp 回归、原文 screenshot prepare/reset/请求分配函数+资源替身；连同既有 grouped-allocation 与旧 tail-offline，定向 Meson 5/5 通过。旧 tail-offline 从 5 pass + 1 FAIL + 1 ERROR 转为 7/7，测试原文未放宽；已加入标准 Meson 入口。新增 wiring 4/4、既有 screenshot 16/16、grouped static 2/2 通过。真实 Win32 allocator 139 检查通过；不等于 GPU/玩家验收。
- 后续补 sparse page-table 对象池抛异常时的本地所有权与 uint32 ceiling-division，加入同寿命小锚点钉页负例，待本次完整重跑；上项旧 5/5 不追认后续改动。
- 过程偏差：首次扩大测试用 `meson test --num-processes 2`，该参数仅限制测试，不限制自动 Ninja；实际观察到并行编译超过 -j2，已按 PID/命令行核实后停止本轮 Ninja 并等待子编译结束。该次 exit1/中断不算通过；改为显式 BelowNormal `ninja -j2 <精确测试目标>`，随后 `meson test --no-rebuild`。不得把此轮全部描述为始终满足 -j2。
- 未闭合：同类钉页、跨 attempt Recovered、同步磁盘写入取消边界。本轮不扩大池或开启索引摘要实验，不把零映射截图后备称作整个模块零内存；冷启动延迟、真实原生截图与 history export 仍需独立实机门。

## 2026-09-20 发布后独立内存审查回读（只读源码核验，未修复）

- 用户提交独立审查文本。当前HEAD24140e4；审查涉及的memory/image/D3D9 allocator/screenshot/palette recorder生产文件与v1.22.00标签无差异。本轮不修改产品源码、不构建/部署/启动游戏、不提交/推送或改Release；仅登记复核结论。
- 源码确认：createImageResource在vkCreateImage成功后、allocation接管之前没有异常清理guard；普通/导入DxvkImage均先registerResource再执行可失败步骤，而基类析构为空、注销仅在派生析构。故障路径的句柄/登记闭合需要补齐；未证明游戏现场已因此泄漏或UAF。
- 源码确认：D3D9MemoryChunk删除小于4KiB尾段但只返回请求长度、Free也仅归还该长度；顶层Alloc缺零长度与对齐溢出前置拒绝。本轮执行test_d3d9_memory_chunk_tail_offline.py：7项中5通过、1 FAIL（尾段）、1 ERROR（缺零请求守卫），exit1。其Python模型通过不能冒充生产实现通过；该非*_static.py脚本未进入此前273脚本组，需补发布测试入口与真实生产回归，不能改锚点销账。
- 源码确认：AsyncScreenshot默认启用，Present无请求也调用prepare，预热3个mapped slot；2560x1440的逻辑buffer容量42.1875MiB，非实测private/commit/VRAM增量，不能称无限泄漏。同步WriteFile配合join仍有慢I/O关闭边界，但本轮未做Windows慢盘实测。
- 寿命分组确已进入生产；整页use_count回收仍不能处理同类内部少量存活切片钉页。索引范围摘要默认关闭，未计作默认节省。审查给出的6144B钉384MiB为对方独立探针结果，本轮仅核机制，未取得或复跑探针。
- 诊断CloseWindow仍使用累计saw*，而MarkStage只按新attempt重置判序；跨尝试Recovered风险与源码相符，本轮未独立重放该C++探针，更不宣称读方已认证恢复。
- 建议1.22.01依序处理Image异常/登记、D3D9尾段/零请求/溢出、截图冷启动/空闲/关闭预算；同类页压力先采有效区间/死区/持有者证据，不扩预算、不直接开启范围/剔除实验。已有玩家正向反馈及93/273历史结果保留，但不作为上述未覆盖故障的通过证明。

## 2026-09-20 16:15 台北：v1.22.00正式发布已完成

- 用户发布授权下，将0df497914c3352539f14fea2af40a600ea7b25fb与新annotated v1.22.00标签atomic/fast-forward推送origin/main；未force、未改旧标签、未上传实验分支。GitHub Release ID392359706已从draft转为非prerelease、latest，发布时间2026-09-20T08:15:30Z。
- 四附件远端状态uploaded，名称/大小/SHA均与本地逐项相等：DLL31,213,491B/ED4BFAEE…D8812；玩家ZIP7,267,655B/F10C3287…679A8；作者ZIP94,698B/E23348CA…F38FB；SHA256SUMS.txt260B/CCCACE07…ACD01。完整值见发布回执与附件。仅白名单发布，无原始证据/游戏资产/测试环境变量。
- 本次最终Render Stats增量的离线证据与已知边界沿用上方实际记录；未重新实机、未将旧候选结果追认到最终DLL。玩家目录仍为RC1且未部署，编译/游戏资源已释放。主树仅保留既有StormBreaker dirty；原stash、A/B分支与旧包保留。
- [正式Release](https://github.com/CallDisaster/War3VK/releases/tag/v1.22.00)。随后仅将本成功回执加入main，不移动已发布标签或重打已冻结资产。

## 2026-09-20 正式包离线收口（发布事务前）

- 打包后追加核验：GNU strip默认改写PE时间戳，预览84190E1D与首包98BD6B28仅0x88/0x89时间戳、0xD8/0xD9校验和4字节不同，机器代码未变。两者未发布、保留本地；改用--preserve-dates后两次独立剥离逐字节一致，最终正式DLL改为31,213,491B / ED4BFAEEC775B6E2F088351A4F0F164609E23300056EECC16294171E9F4D8812。下条原预览身份不作为最终下载身份。

- Render Stats显示修正的精确DLL构建50/50、BelowNormal/-j2、no-work通过；断言开启CPU93/93、静态脚本273/273、Render Stats生产辅助函数372断言零失败、配置审计ok=true、正式包读方8/8及py_compile/diff检查通过。统计读取只遍历既有有界页表，未增加分配或逐draw原子、未改变384MiB/1152MiB预算及caster/fence政策。
- 首次预览剥离副本31,213,491B / 84190E1DC48CEB62381E81473F277DA53B9EA12F3DCAD61BCC0938CCB4A49863（已由上条取代）；未剥离36,051,280B / FC7EA415E222E0B16C1C27707779D25489AD86633B3B5FAE752E951E050C45A1。PE32/i386、1.22.00、PRERELEASE=false。最终副本未重新实机；用户接受的是RC1，最终增量限显示统计与版本资源。
- README中英、正式CHANGELOG/发布说明已整理，保留Issue #8与跨地图/极端预算/未覆盖API边界；Water未完成内容、自动模型灯、64位产品不纳入。发布工具使用固定依赖源码中的许可证，不依赖桌面旧RC包；包内逐成员/大小/SHA/CRC闭合后才上传。
- 发布前扫描拟提交路径及两份本地集成提交，常见凭据模式命中0；不上传原始日志/证据/游戏资产/构建目录。玩家DLL仍为62BF9F40…6381C3，未启动游戏、未部署，相关游戏/编译进程0。既有StormBreaker dirty保留且不暂存。
- 本条不是远程成功回执。Git主线/标签、正式包和GitHub资产需由随后事务分别核验；详见2026-09-20-v122-release-publication.md。

## 2026-09-20 玩家接受与正式发布准备；Render Stats显示修正

- 用户认可RC试玩并明确授权README/更新日志与GitHub稳定Release，追加要求先修显示。本轮不把历史未做的GPU/长时/API门补写为通过。
- 修复快照resident/used/reclaimed只在分配或GC填值、随scene清零造成假零：生产所有者在四处scene重置及池变动后读取有界页表，UI仍只读已发布统计；真实清池/回收可下降，不保存历史最大值。新增100帧无分配复用、回收、清池、溢出计数用例；未改预算/准入/释放条件。
- Arena新增上一提交代字节统计，跨BeginFrame游标归零保留，初始化/Shutdown清零；显示“保留容量”及不自动缩容说明。不是GPU完成证明，不增加每draw原子操作。
- 面板静态9/9、页池静态9/9与diff检查通过，DLL/全量离线回归待收口。尚未推送/标签/上传，旧RC和玩家现场原样保留。

## 2026-09-20 RC1正式配置离线闭合并交付玩家复测（未发布）

- 新目录锁定源码/依赖构建完成；产品release/O3/NDEBUG、重型取证默认关、蒙皮正确性合同保留，其余开发实验关。独立测试配置开启assert，Meson 93/93、最终静态273/273、配置/包/源输入定向49项通过（其中源输入4项也在273中，不相加冒充独立总数）；py_compile、exact DLL no-work与本轮diff空白通过。
- 桌面包为WarVK-v1.22.00-RC1-20260920.zip，7,265,923B / F9BD285B2CC7D0E93C29196892F77A48B93BA718F962382066F239CD980F7DBA；包内DLL 31,213,491B / 62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3，PE32/i386、1.22.00、PRERELEASE/RC1。仅包内副本strip，33导出/20导入及导出表摘要保持；未剥离构建产物另存。严格成员/大小/SHA/ZIP CRC校验通过，所有产品/GPU/视觉接受标记仍false。
- 全新构建另揭示旧MinHook未跟踪静态库依赖，改为锁定gitlink C源码构建；首次链接失败保留。测试目标选择错误和第二轮静态270/273失败也保留：分别修正目标选择、提供本地只读历史基线、恢复4个冻结模块的原始换行字节后重跑，不降低旧SHA合同、不追认失败。
- 交付README说明普通启动、备份与回退、Render Stats、阴影恢复/Issue #8/录制开关/截图退出复测。游戏现场FAC75C10…9E529 / 36,412,825B未修改；未启动游戏或GPU实机，不能继承FAC玩家反馈为本组合通过。未提交本轮改动、未push/tag/release，根稳定CHANGELOG不变。
- 完整回执与构建日志索引见docs/agent-history/2026-09-20-v122-rc1-player-delivery.md。编译资源已释放，后续等待用户评估是否发布。

## 2026-09-20 RC1正式配置构建中（历史中间态，后续结果见上）

- 用户请求正式配置DLL做最后一轮玩家测试；主线程独立构建，不部署、不启动游戏。产品/JAPI显示版本改为1.22.00，PE明确PRERELEASE/RC1，Shader ABI 1.2.0与JASS wire v1不改；默认重型取证关、palette correctness开、其余开发候选关。
- 采用新目录源码快照和锁定gitlink依赖，未使用主目录旧build32或StormBreaker脏文件。初次Meson setup确实失败：旧B未跟踪的war3_frame_recorder_build.h.in漏入合并清单；已按原模板补回并新增源输入检查，随后正式配置审计通过。该失败保留于E:/WarVK-Builds/v1.22.00-rc1-20260920/meson-setup.log。
- 主目录首轮273个静态脚本265通过/8失败：7项因冻结参考的Git CRLF转换破坏字节SHA；逐一核原B、只还原换行字节并加入.gitattributes，不改登记哈希。另1项是新census测试EXE尚未构建，不是产品运行失败；收尾必须用本轮测试产物重跑，旧8失败不追认。
- 另同步JASS运行探针与协议测试版本，避免仅改system.version而旧探针继续期望1.21.00。当前全新DLL构建仍在进行；后续CPU测试使用断言开启的独立测试配置，不把NDEBUG删掉assert的零退出当作回归通过。

## 2026-09-20 本地主树源码整合完成（非发布）

- 源码checkpoint为17ecf66cb97368bf3ff033c5f0061d2f94c33da3，801文件/+150268/-3601行属于此前累计工作。本次按B-primary审查结果整合，原主目录dxvk已切换到codex/v1.22-main-tree-20260920；不是把旧A全部merge覆盖B，不再在两树继续产品开发。
- A/B原分支未移动；A stash 7b1a52a8535fcd300f7af51773edc9a2ef1a3616保留、未apply/drop；独立ZIP/HEAD bundle保留。50个changed tracked/120个untracked可恢复，原字节以外部ZIP为准。旧A的Stage13历史测试、image guard及研究资产未丢失，也未混入产品。
- 2870个index条目相同、2866普通文件验证（995仅CRLF/LF差异）；A11542个ignored与StormBreaker2404个文件逐SHA不变，子仓元数据/HEAD/status不变。唯一既有dirty为StormBreaker，禁止拿其旧dirty或A旧build32编译发布；后续须独立clean依赖/配置。
- A主目录重跑树25/配置20/包25/native bridge6/native transaction18/Stage13 retention4/Render Stats7，共105项定向Python/static通过，不是全量或实机。DLL/玩家FAC未变，无构建/游戏/部署/push/tag/release。
- 如实保留两个检查失败：历史0字节superproject index.lock使首次stash失败，核零Git进程后改名保存再成功（嵌套index.lock未动）；cached全量空白exit2暴露旧untracked尾空白/EOF空行，冻结夹具与历史原文未为凑全绿改写。另stash换行差异已用ZIP复核为仅CRLF/LF，不忽略内容差异。
- 本次收尾只更新入口/文档，稳定根CHANGELOG仍不改。事务与原字节证据见docs/plan/2026-09-20-formal-main-tree-integration.md和D:/WarVK-Backups/20260920-formal-integration-transaction；正式配置/组合视觉GPU/发布包待完成。

## 2026-09-20 正式本地主树整合开始（主线程独占Git事务）

- 用户明确授权正式合并；按B-primary reviewed checkpoint + A主目录切换执行，保留A/B原分支、A未提交内容与StormBreaker。DSH写者已完成并冻结，额外复核仅只读。
- A/B最新CreateNew备份分别为D:/WarVK-Backups/20260920-formal-integration-A、-B，ZIP逐文件SHA/CRC与HEAD bundle通过。A ZIP3502151B/SHA A3299A56…E0D8，B ZIP36167653B/SHA 019B0B89…A992；旧备份不删除。
- 主线程重跑树整合25/25、配置20/20、包审计25/25纯Python通过。采用显式pathspec；自动模型灯保留B默认关闭研究代码、x64不接产品、Water不导入；buffer失败处理保留已实现B版本，不新增图形算法/预算/释放政策。
- 不搬动/更新StormBreaker，不复用A旧build32；最终状态可能保留子仓dirty，必须与superproject源码一致性分开。详细事务、恢复和未覆盖门见docs/plan/2026-09-20-formal-main-tree-integration.md；本条不是已完成或发布声明。

## 2026-09-20 Issue #8 撕裂修复补入发布日志（主线程，文档回溯）

- 用户指出遗漏。回查9月15日VIDEO25–29复算、独立审查、palette来源合同、后续高压候选及当前源码，确认旧草稿仍停在早期布局猜测/缺完整输入阶段，漏写已经实施的蒙皮来源修复。本次将Issue #8独立列为“已定位异常输入及相关代码缺陷、修复已落地，最终发布组合待验证”，不是今天新实现或新实机通过。
- 已证几何：对应边17.105469→390.213730；49组surface/volume输入角点一致，48条重点蒙皮记录输入齐全。说明异常三角形同时进入两条阴影路径；原生槽位重分配的具体所有者、最终像素归因仍未证明。不把它与384MiB快照页粘滞失影混为一因。
- 当前代码复核：model_hook的CaptureRuntimeGroupPaletteBindings在严格门下拒绝无效槽位回读；QueryOwnedRenderablePartPaletteSnapshot复制及复核当前身份/帧/槽位/代际；semantic/war3_live_palette_selection.cpp的严格分支只接owned快照，兼容分支冷/热producer数量检查共用ProducerGroupCountCovers；skin::Selection/CanReplace/Usable与canonical world选择把数据、来源、空间共同交接，提交前另核publication ticket。CPU发布身份不冒充原生allocation所有权，兼容分支也不声称全部所有权问题已闭合。
- 同步发布草稿与收尾清单：描述症状、证据、修复手段及最终高压/运动/体积阴影门；保留早期玩家正向与随后高压复发/开关未核的边界。正式配置去诊断不得误关蒙皮修复，安全拒绝不得靠漏投影冒充视觉通过。
- 本次只改三份文档；原始研究/运行证据、稳定根CHANGELOG、源码、DLL和玩家现场不改。复核本次文本差异、链接与空白，不重跑或继承旧构建/游戏门；无部署、提交或发布。

## 2026-09-20 Render Stats 首次构建闭合（主线程，未部署）

- 玩家进程退出、三个DSH写者明确冻结后，主线程以BelowNormal/-j2构建exact DLL与war3_render_stats_view_test.exe；构建exit0，两目标no-work，CPU生产头测试267检查/0失败，面板静态7/7、py_compile与diff空白通过。日志为AutoTest/artifacts/release-closeout-20260920/ui-build.log；包含一次Meson再生成，不宣称无generator。
- PE pei-i386/i386，DLL 36,413,252B / 676EE0A06A19A90123DDE9307E65D114EBE6F54DEDB12DF89A0E4766E28EC7E3。尚未跑新组合全量Meson/static、正式配置、实际面板显示或游戏门；不继承FAC验收。
- 玩家现场保持FAC75C10…9E529 / 36,412,825B，未部署/启动游戏；收尾War3/ninja/cc1plus/g++/meson为0。编译资源已释放，三个文档/工具任务恢复写入。
- 包审计初稿发现缺SHA、路径穿越后按basename兜底、字符串布尔、未列文件四项误接受，已退回修正；未接受初稿“包校验通过”。A嵌套StormBreaker存在独立dirty，顶层备份不覆盖其内容，整合前须单独保护。
- 后续已以CreateNew完成A嵌套StormBreaker备份：D:/WarVK-Backups/20260920-release-closeout-A-StormBreaker，91个去重dirty/untracked非ignored路径、zip490864B / 45DD3AF127D2C5475527A7E4558EF963D61494109860584242DE56282E4CB957；CRC/逐文件SHA/HEAD bundle及前后Git状态、tracked/staged diff全部核验。只备份，不改子仓库或refs；ignored构建产物不在此备份中。
- 主线程独立复跑DSH首批配置15/15、严格包21/21、树整合审计15/15纯Python通过，原四项包反例已封堵。配置JSON类型/重复键与作者包嵌套载荷边界仍退回补测；这些工具通过不代表产品配置/运行验收。A图像迁移guard只列为历史未审候选，不把它自动认定为B必缺修复；B已有逐子资源迁移，需要独立合同审查。
- 配置/包第二轮主线程复跑20/20与25/25通过：重复键/非有限值/错误类型、作者包嵌套二进制/路径别名等反例已补；buildConfig仍仅manifest自声明，不冒充二进制特性证明。内部现有introspection通过internal-diagnostic检查，尚未建立正式产品构建。
- 整合r3在落盘清单后CONTEXT_WINDOW_EXCEEDED结束，不能登记为成功turn。主线程保留产物并发新短上下文r4审查ignored覆盖、嵌套.git元数据及可恢复切换；尚无Git写操作、没有实际合并。不为赶进度执行未核的stash/switch清单。

## 2026-09-20 发布收尾派工与 Render Stats 接线（主线程，源代码阶段）

- 用户授权主树整合与正式配置整理；新建三个 commandcode / deepseek/deepseek-v4.1-flash DSH 会话，分别负责正式配置、A/B 主树整合审计、最终验证/包审计。当前仅授予各自新文档/工具/测试路径，不授予 Git 写操作、构建或游戏资源；不是重启已结束的夜班自动化。
- 整合前 A/B 已分别完成 dirty zip 逐文件 SHA/CRC 与 HEAD bundle 验证，备份位于 D:/WarVK-Backups/20260920-release-closeout-A 与 -B。A 171 路径/B 912 路径；B 原 DLL 为 FAC75C10…9E529 / 36,412,825B。备份不代替冲突审查，尚未执行主树合并/提交/推送。
- 确认 Render Stats 原为纯占位文字，没有统计接线。本轮新增只读面板：快照页常驻/占用/实际配置上限/回收/拒绝、静态缓存逻辑引用、Arena 当前代与多代常驻/上限/串行退休/异常、主堆预算、32位VA可用、caster/CSM图更新与接收端状态。明确重叠口径、生命周期累计与发布帧计数，不把384MiB当总显存、不把draw统计当像素证明。
- 展开面板才按250ms刷新；复用已发布 producer/replay/CSM 诊断。新增 Arena 小查询只读既有原子字段，不访问非原子页表、不调用完整诊断/扫描、不启动语义构建或录制。新增 receiver 小查询只在既有scene-stat锁内复制12字段。跨来源可能不同帧，Arena/replay原子读不构成一致性/回收证明。
- 静态接线合同7/7通过；纯CPU刷新/单位测试已加入Meson，尚未编译/执行。因发现玩家 War3.exe PID46564仍在运行，本轮不并发构建、不打断或覆盖玩家现场；不能继承FAC的92/92与271/271作为新源码门禁。
- 正式更新日志由主线程维护，稳定根CHANGELOG仍不变；待正式配置、新DLL、组合实机/视觉与包审核后晋升。详见 AutoTest/artifacts/release-closeout-20260920/coordination.json 与四件事收尾清单。

## 2026-09-20 FAC玩家移动阴影恢复正向反馈 — 开始发布准备，非发布接受

- 用户确认此前“加载过区域占住快照后其他地方无影”未再观察到，当前镜头到各处均能看到阴影；记为本次玩家视觉正向结果，不延伸到任意地图/长时/GPU/性能/随机裂缝。
- 本轮只读磁盘核验build32和E:/Work/Warcraft III现场均FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529 / 36,412,825B；无游戏进程，非加载模块证明。上轮source-final逐文件复核无变化。最新HTML仍旧E42，不伪造FAC性能数字。
- 用户同意着手发布准备；新增docs/plan/2026-09-20-v122-release-closeout-checklist.md，将昨夜基础完成、今天生产分页、长期未完成和正式发布前置分开。仍无提交/合并/推送/发布。
- 当前build32仍internal_frame_recorder=true、skin_palette_contract_candidate=true：不能把内测DLL直接改名发布；需要正式配置审查、独立干净构建、组合实机与作者包/日志收口。关闭取证不能误关蒙皮正确性路径。
- 该轮仅更新状态/发布准备文档，无C++/测试/配置/DLL变更；根稳定CHANGELOG未改，保留原测试及玩家现场。

## 2026-09-20 主线程生产分页候选离线闭合 — 尚待大图恢复门

- 长/短期与未知保留意图已接入position/UV/index实际页申请；仍共享384MiB默认预算，受限或具名OOM时可借同代合法页尾。不改caster准入、完整性、8帧旧图上限、缓存GC或GPU退休，不提高上限。
- 一并修初始buffer空allocation解引用/过早注册及裸VkBuffer异常清理；只恢复明确容量失败，device lost/未知驱动错误与通用allocator宿主机bad_alloc继续向上传播。未证明所有allocator异常事务。
- CPU对照80周期：旧逆序混页56拒绝，分组0拒绝/峰值48MiB；841断言通过。Meson92/92、根目录271个static脚本通过、py_compile/diff/no-work通过，非GPU或全量Python验收。首轮CPU旧策略建模错误及旧static文本合同失败已保留并在专项记录说明，未追认旧失败。
- 构建54/54，末次诊断保全2/2，BelowNormal/-j2。新PE32/i386 DLL 36,412,825B / FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529；仅离线候选。
- 玩家DLL仍E42/36,400,299B，游戏/编译检查0，编译资源已释放；没有部署/游戏/合并/提交/发布。高压恢复、完整caster与帧尾延迟待验证，不能宣称全屏失影已修复。
- 详见docs/plan/2026-09-20-stage11-production-lifetime-recovery.md；日志/原文件备份在AutoTest/artifacts/day-recovery-20260920。长期本次仅闭合页分配这一段，不冒充M3/M4/跨Pass完成。

## 2026-09-20 主线程生产分页修复开始 — 非稳定、未部署

- 用户要求主线程直接实施。已将既有寿命分组政策接入position/UV/index实际申请：共享384MiB不变，同类优先、创建失败后保留合法混合尾部，无页洞重用/强制GPU释放。
- 先补底层前置：buffer完全初始化后才注册资源、null不解引用、尚未移交的VkBuffer有异常清理，具名allocation错误与device lost分开。
- 首次新接线Python2/2、纯政策16/16及diff空白通过；编译、CPU实跑和实际恢复尚未验收。主线程写源码，仅委派一项只读历史/反例审查。E42玩家现场未动。
- 历史纠正：分页与16MiB均已在B的v1.21 HEAD内，当前GC/静态分类相对HEAD未扩大，不能把最近回归单归因于“新加分页”；旧A独立buffer和GC不同，但未绑定上一可恢复玩家身份。
- 合同：docs/plan/2026-09-20-stage11-production-lifetime-recovery.md；源码改前备份见AutoTest/artifacts/day-recovery-20260920/before。

## 2026-09-20 E42地点相关失影 — 只读核对页分配与缓存寿命

- 用户补充旧地点可能恢复、特定地点持续无影。源码核实快照页为尾部追加、无洞复用；仅池独持页包装对象时整页回收，静态条目级64MiB非保护目标不能限制被少量引用钉住的整页容量。
- 合法旧backing可复用而新backing缺失/容量增长需申请，解释池满时为何不同地点表现不同；具体地点与首个拒绝对象尚未逐对象关联。近期失败捕获不会刷新成功访问寿命，不把旧缺陷当当前事实。
- 磁盘DLL再次核为E42，未构建/部署/操作游戏；旧OFF03页账户与本次玩家报告明确分开。没有新增修复通过声明；详见docs/research/2026-09-20-e42-player-shadow-loss-and-night-progress-review.md追加节。

## 2026-09-20 玩家09:52 E42回归 — 全屏失影仍存在，长期主干进展不足

- 新HTML3189960B / 43651322D8A086794A55CDC707AEF997A50E5FE7A94115B9F89E22571787B120通过既有strict root/series读取；报告和磁盘均E42/36400299B。3560帧中3207生产不完整、3191零强度；快照页384MiB、capacityReject累计105990、requiredOmission累计90509。末459帧图序号固定4826、静态prepared=0而replay253..311，未解决阴影消失。census关闭/无samples，不伪称已取得本轮逐页证据。
- 玩家War3.exe PID16460仍运行；本轮只读日志和源码/计划，没有构建、部署、游戏控制或恢复自动化。默认取证目录无今天新包，若用户另存需现有路径，不先要求重录。
- 对照长期计划：昨夜生产增量主要S2局部成本与无用UV引用解除；P3仅纯组件、早期剔除无Consume、M3/M4/跨Pass主干未实质推进。52路径/9334新增代码测试行不能冒充产品推进；其中AutoTest8026行。主协调分工偏向反复诊断/工具收尾，未交付恢复结果，应纠正。未重新计算整体百分比，维持约60%/40%旧管理基线，不晋升稳定。
- 详细只读复核：docs/research/2026-09-20-e42-player-shadow-loss-and-night-progress-review.md。根CHANGELOG与原始运行证据不变。

## 2026-09-20 08:30 — 夜班正式停工与自动唤醒暂停

- 四条DSH任务均已结束或以context overflow终止，报告/信箱保留，未另开线程延长任务；写权和编译/游戏lease全部释放。官方automation_update已确认warvk-2=PAUSED，不手改任务文件。
- 父审最终源码冻结2971项 / 527ECFBDC08565E728A5CE0D75240286B6FD9665C488142801373F6E0EC25F11再次verify通过；与测试冻结仅AGENTS+新增收口文档不同，总日志按原规则排除。C++/测试/DLL/离线ZIP未变化。
- 四次运行settlement独立复核：原生hProcess exit0、terminatedByHandle=false、无PID终止命令；每次restore/protected/source/map/settlement全true、零新增GPU事件/dump。最终相关进程0。dirty904（110tracked+794untracked）；相对开工882路径实际30变化/852不变，另4原clean tracked变dirty、18新untracked，总52受影响路径含本收口文档/总日志，不是904项都由今晚产生。
- 编译资源已释放；游戏资源已释放。E42CD88C仅离线候选；压力恢复仍失败，未合并、提交、推送或发布，等待用户下一次恢复推进。

## 2026-09-20 08:20 — 夜班离线交付与最终回执核验，发布继续阻塞

- validation-b20新跑270/270静态入口、1340/1340 unittest、读方direct/module各56、UV5/政策16/ledger4/变异4、CPU661/120均通过；DLL/C++仍为b19 exact 2/2、no-work、91/91 Meson对应的E42CD88C/36400299B。旧86PASS/1FAIL/183未跑原样保留。父任务另跑5文件py_compile、diff check，并核源冻结2970项不变。
- 离线ZIP8035766B / 9E7F29971240D3424DD488222B5ACEDF22B7220BA4473798419CF9010AE29F88，仅DLL/README/manifest；父任务独立CRC、三entry逐字SHA、PE、源复制前后身份均通过。BF938测试现场/A0A51保护现场不变，相关游戏/编译/runner进程0，E可用56.19GiB。没有自动部署或新实机。
- 600015组有效输入差分0 mismatch；成本夹具core/upper reused-output仍1次32B prefix scratch，不能写零分配。P3纯策略120编译CPU checks已过，architecture-b21过期“未编译”已更正；默认false且无生产适配。
- memory-b22现有证据审查确认P2a late hit=0不足以区分UP/DynamicSysmemIBO/summary bypass/query miss；未新增计数/扫描。architecture修订和memory末轮均在报告落盘后上下文溢出，记失败turn，父任务核验接管，不伪造normal final。四线停止写源码。
- 新增夜班收口文档并更新AGENTS简短入口；代码/测试冻结不再变化，收口文档另计。累计dirty903不属于全部本夜新增。预算/阴影恢复仍FAIL，v1.22不合并不发布；编译资源已释放，游戏资源已释放。
- 08:26父审独立用backup路径与git status -z查出architecture-b21漏算4个开工clean tracked文件（union evaluator头/实现/C++测试/Python静态），HEAD差分+247/-1；报告“无clean-tracked变化”撤销，原报告不篡改。完整收口前代码/测试增量为+9334/-375，含新文件/测试而非纯生产代码；新增收口文档后dirty904=110tracked+794untracked。详见收口文档补正表。

## 2026-09-20 08:00 — 精确构建闭合、一个旧文本门失败；新纯Python验证另算

- validation-b19独占BelowNormal/-j2 DLL preflight/actual严格2edge、exact no-work；两显式CPU targets严格4edge，661/120断言通过，Meson --no-rebuild 91/91。DLL36400299B / E42CD88CD266CAF950E77001386C71B3B79105DE0A8AD41272C414B87124E814、PE32/i386；前后2970文件freeze一致，BF938/A0A51现场不变、零进程。编译资源已释放；没有新实机。
- 全静态首轮86PASS/1FAIL/183未跑，失败为active-ledger旧正则仍要求commit后无花括号直接activate。父审确认生产顺序保持，修该测试为严格成功commit→仅noUV清理→原完整key激活的整块合同；4原例PASS+4实际内存变异KILLED（无条件commit/错key/移出commit/清有效UV）。这是新修复，旧失败不追认。新freeze0800仅此Python变化，不重新编译DLL；已派validation-b20独立全量纯Python。
- strict retention纯策略默认false，16Python+120编译CPU断言通过，无生产调用或runtime开关。createBuffer审计发现更具体的前置风险：底层null返回会被assignStorage解引用；构造已注册资源、抛出后派生析构不运行可能留下resourceMap悬空指针。bind失败的Vulkan allocation有Rc RAII，不能沿用“全部泄漏”说法。本夜不扩大核心分配器改动，记后续有类型失败与回滚前置；不把源码可达风险冒充已定位玩家崩溃。

## 2026-09-20 07:47 — UV引用寿命小修与读方校正落盘；P3仍未接生产

- 父任务应用成功 no-UV 捕获后的独立 UV wrapper/proof 清除，且仅在 captureAttempt.commit 成功后、激活前执行。四条有效 UV 路径均保留；alpha 必要 UV 失败仍不发布；不动 position/index、页游标或 GPU retirement。共享 CPU helper 与新增模型用例已落盘，GPU/收益未验证，build32 的 8B4021 现 source-stale。
- 父任务独立跑新 analyzer unittest 56/56 与 UV 静态5/5；此前4条隔离变异均KILLED。发现测试文件 main 位于新增类之前，使直接脚本仅37例，已将 main 移到文件末尾后直接执行同样56/56。修复读方 render serial 高水位和 pair 的 strict report union；重复帧冲突、真实覆盖缺口仍拒绝，旧运行产物不回写。
- validation-b18 曾用 unknownPositionBytes 的512KiB POSITION请求与 INDEX summary 的16KiB/256KiB预算相比较，父审判定为单位错误，已要求并核实报告撤回。NonHostCached 与 uploadRangeHits=0 是两个事实，具体零命中原因仍缺 callsite/IB bytes/bypass reason，不能假称定位。
- memory-b19 未实施生产分页（报告明确 partial proposal），当前无P3 runtime开关/接线。父任务仅继续纯政策 strict-retention 模式与反例，并独立核 createBuffer 异常回退前提；避免为按时接入而引入新合法尾部拒绝。当前只有 architecture 纯头与两测试写权，无编译/游戏租约。

## 2026-09-20 07:22 — ON诊断未命中范围优化，恢复失败，现场已恢复

- validation-b17唯一ON01执行完成，PID34868/native original-hProcess证明自然exit0、未按句柄终止/未发PID终止命令。父任务独立核BF938现场36359521B、A0A51保护现场36271456B、候选8B4021/36399883B；零游戏/编译，newDumps/GPU事件空；desktop/video结算。游戏资源已释放，无后续实机授权继承。
- 同DLL/路线/384MiB的OFF03和ON01都发生容量拒绝、整图停止刷新，ON在relief序号2990不再前进；P2a uploadRangeHits两轮均0，因此此图未覆盖当前上传范围快路，拒绝总数差异不算性能收益。冻结一轮与pair读方均exit1。ONfresh1同样受已知0占位计数缺陷影响，真实新高为0；原产物不回写。pair另有只取最后report导致缺阶段的工具缺陷待单独核实。
- 父任务只看本次pressure-2.bmp与relief-end.bmp：相机确实移动/回归，但中央难度选择对话框仍在，外围仍有游戏场景；不能称完整玩法/视觉接受，不重复查看旧图。四图各11059254B/2560×1440，9份报告。原始报告/分析保持只读。
- memory-b17与culling-b16独立确认成功no-UV捕获会保留旧独立UV页引用，当前消费者因stride/format门不使用它；它是寿命/账户冗余，尚无实际字节收益证明。将先落最小解除引用与生产共用CPU测试，不据此宣称384MiB压力已修复。P3只生成保守同预算分页提案，未启用。

## 2026-09-20 07:09 — ON前检纠错与原冻结保持

- 父任务给b17的source-freeze SHA多抄一位5（65字符），前检正确拒绝；源文件未变。实际64位哈希为A8E4575678FE939281A7D6F7FDD49BF756DCDD05DE6B27121F039FCACB2E8AD8，父任务重新逐文件核2969项、零变化，runner直接Python前检exit0。
- validation自写PowerShell前检又遇BOM-less中文路径及字符串替换损坏脚本；父任务叫停并收到明确no-apply/no-game回执，失败脚本/日志保留。新ASCII-only父任务wrapper用既有runner核中文路径，父进程先BelowNormal、CreateNew日志，恢复同一次ON租约，不算已经执行过一次ON或重试游戏。未增加窗口/输入/预算，源码与原始OFF证据未修改。
- architecture-b16真实函数反例证明fresh计数会因0占位重置后误计旧序号；原analyzer仍因serial_decreased判FAIL，不是误通过。修复提案保持未应用，待ON按同一冻结读方完成后单列修订版本复算。culling-b15报告将全图失影链缩到required-caster遗漏使整图拒绝及8帧有限保留后zero-strength，仍待父任务逐点复核。

## 2026-09-20 06:55 — OFF03真实压力切面闭合，恢复失败；单次ON诊断

- 父任务从冻结analyzer输出读出stage0(frame10034) active176/cache170.48MiB；stage1(frame13198) active384/cache186.01/已用但无cache引用186.91MiB；stage2(frame13318) active384/cache172.49/已用无cache引用200.43MiB，尾部11.09MiB。这支持整页引用拖住大量废弃区间，不是384MiB全部为当前有效几何；不把CPU wrapper计数当GPU完成。
- analyzer实际exit1，relief4697观测帧、最长2110无有效刷新、末尾无持续有效帧，render序号始终最大3164；其fresh12受0占位后回到旧序号影响，不能当12次真实新图。业务帧18637..18742缺口不可忽略。stage3(frame18758)在reliefEnd之后/退出窗口，不认证压力解除。OFF03是有效压力诊断，不是恢复通过或产品接受。
- validation-b16在恢复/analysis/exits落盘后context-overflow，无正常final；父任务接管核验，建立同模型新validation-b17先独立收口，再授唯一ON01诊断。新manifest3D6498A2，候选/源码/路线不变，仅P2a索引范围开关0→1及run身份不同；保持384MiB、隔离2560×1440/零globalinput与完整性。允许观察原OFF故障是否变化，不授权重跑/发布/增加输入。其他线仍无生产写权。

## 2026-09-20 06:46 — OFF03事务完成并恢复，视觉/压力验收仍分开

- 一次fresh PID37788运行exit0；严格ready通过，唯一隔离HWND SPACE在22.047s，首完整帧等待11次poll、累计代际2；五段锚与160s路线完成，9份自动报告/4张2560×1440 BMP已留存。transactionOk/restoreOk/settlementOk=true，不等于阴影或内存问题通过。
- 父任务只看本轮 normal-start / pressure-2 / relief-end 三视图：起点英雄选择区域可见喷泉/围墙等投影；低角度与移动确实改变画面；中途及末尾有难度选择框，遮挡中心且完整游戏推进未证明。不能拿有限画面认证全投影完整或玩家高压恢复；pressureCovered/imageRecoveryProven仍false，分析另报。不重复看旧坏帧。
- 父任务独立核build8B4021/liveBF938/保护A0A51，sourcefreeze A8E45756无变化；零游戏编译，receipt无新dump/GPU事件，原始hProcess自然exit0且terminatedByHandle=false、pidTerminationCommandIssued=false；隔离桌面关闭、2560×1440/144Hz恢复。编译资源已释放；游戏资源已释放。当前不授权ON/补跑。

## 2026-09-20 06:37 — 启动strict返回修复通过，单次OFF03诊断准入

- b14-r1把最终ready返回也改为精确bool判定；父任务111项、pycompile/diff0，独立保留24反例全PASS，原22/24失败记录不追认。逐SHA比较b12确认仅runner/helper/test三文件变化，2969文件freeze A8E45756复核不变。architecture旧会话在报告/freeze落盘后context-overflow，无正常final；已同指定模型新只读会话收口长期清单，不转移写权。
- 新manifest8B70B369 / OFF03已只读前检通过，候选8B4021/liveBF938/保护A0A51等身份一致、目标不存在。授validation-b16唯一新诊断lease，不继承ON/重试/编译；同隔离2560×1440、一次HWND继续键、100s就绪、160s路线、原384MiB与完整性门。其余线仅只读/ignored，父任务无C++写入。运行/恢复结果另记，当前不能称有效基线。

## 2026-09-20 06:30 — 独立反例拒绝启动误通过；P2a与剔除只读复核

- architecture-b14三Python自测111项通过，但validation-b15独立24例中2例失败：strict循环拒绝字符串 transportOk/ok，最终return却仍按truthiness给ok=true。父任务回源确认，拒绝本freeze实机准入；只授原三文件修正与独立复验，失败报告保留，不能以零继续脉冲替代ready结论。当前编译/游戏lease均空。
- memory-b14已执行现有P2a纯CPU组件90113断言与20000建模输入，无具体身份/范围反例；未链接device生产消费者，非运行验收。16KiB×10000 helper121254us、memcpy1789us提示成本非零，分配节省/吞吐仍未测。转下一批核预算下CPU暴露与现有计数口径，不增生产采集或预算。
- culling-b13完整回源表确认：最终剔除晚于snapshot、当前union仅CSM2/3且无VolumeSun闭合，不能授权捕获前少收对象；窄S1静态retainSource已复用，不能推广全部static。转为现有玩家09-19报告逐帧掉点归因；384MiB拒绝能解释特定遗漏，尚不能解释全场景无影。

## 2026-09-20 06:15 — 启动门离线修订已分派，无新实机权限

- validation-b14完成工具final/报告与日志原字节保存，失败事务已独立闭合。批准architecture-b14仅三Python添加可选“先有control-plane/JASS证据才花掉一次继续键”门，默认保留其他helper调用方行为；canary启用，不增脉冲/超时/路线。validation-b15独立反例，memory-b14审计既有P2a精确索引身份与重用风险，culling-b13继续只读覆盖清单。所有编译/游戏lease均空。
- 下一次必须先完成新三文件freeze与独立测试，再由主线程重新决定；本条不授权第三次运行。P3不因normalcut就接生产，不增加预算，不发布。

## 2026-09-20 06:12 — 第二次诊断进图未覆盖，恢复闭合；启动脉冲时序线索

- OFF _02唯一PID38760运行exit1，100s ready未通过（gameStarted/runtimeReady=false、worldPtr0、累计epoch1、recording=false）。压力路线未开始、phaseMarkers/report/BMP均空；冻结analyzer正确fail(no_reports)。不把此失败归为预算/阴影故障，未启动ON或补跑。
- 父任务独立核验live BF938/保护A0A51/候选8B4021精确SHA，零相关游戏/编译/编辑器进程，源freeze无改动。native原始hProcess自然exit0，terminatedByHandle=false、pidTerminationCommandIssued=false；desktop关闭、视频2560×1440/144Hz恢复，GPU/dump0。编译资源已释放；游戏资源已释放。保留两次原始失败与备份。
- 已发现可检验的启动时序差异：_02唯一SPACE在10.023s、controlPlaneReady=false时发出；_01在22.097s且已有pipe时发出。helper旧无pipe分支会提前花掉canary仅一次继续脉冲；这是候选解释，不是画面确认。已交architecture核实，禁止增加脉冲次数或盲目重跑。
- memory-b13对唯一normalcut确认nonHostCached未知范围请求412次、每次512KiB，累计206MiB并非实际分配/节省；171MiB有效cache union仍无法按流拆分。优先验证既有P2a精确索引路径，不因该normalcut直接接P3或增加新生产采集维度。

## 2026-09-20 05:58 — 首个完成帧等待修复复核与新诊断冻结

- architecture-b12仅改runner及其测试；父任务逐SHA比较0530确认恰好两文件，其余2967文件/HEAD不变；独立105/105测试、py_compile、diff check通过。新freeze 7F3473D9；DLL仍8B4021，不需要重编译。严格pending双零且录制开启才在原5秒内等待，累计代际漂移/管道/结构/owner错误立即失败，后续严格推进门不变。
- culling-b12源代码复核确认ready先于首个endFrame确为可达；报告/回执在DSH上下文超限前保留，不能写成正常final完成。memory-b12真实normalcut176MiB中171MiB引用、5MiB尾部且age0，尚无压力/冷引用/whole-page pinning证明。validation-b13独立核首轮exit1恢复闭合；首轮永久无效，不追认。
- 新授权validation-b14一次OFF诊断（新_02路径与manifest B650D3C9），固定隔离2560×1440、零globalinput、同384MiB与完整性门，不继承ON或重试。父任务直接python只读前检通过。其他三线只读/ignored分析；无C++写者/编译权限。

## 2026-09-20 05:43 — 首轮 OFF 在初始完成帧门失败，已恢复与释放

- 本夜首轮fresh PID38060，严格就绪通过、隔离client2560×1440；未开始压力路线/未取得画面。ready时perf已enabled/recording，但completed history尚空：frameAnchorValid=false、业务帧/epoch均0、累计epoch2。runner在第一次取锚即抛错，既有5s等待只覆盖“已有上一帧但未推进”，不覆盖首个完成帧；不能将它报告为预算或阴影实机失败。
- actual exit1、phaseMarkers空、transactionOk=false；原始失败/ready/receipt和4000帧自动报告保留。该报告含正常cut0：active176MiB、rejects0、uploadHits0、complete=true，未覆盖压力和恢复。不能拿关机等待期间的report追认路线有效。
- 父任务核测试live BF938/保护A0A51/候选8B4021均未漂移，settlementOk/restoreOk=true、零游戏编译进程、无新增GPU事件或dump。游戏资源已释放。DSH在事务恢复后上下文超限，无final完成回执，父任务直接复核原始产物，不伪报代理正常完成。
- 仅授权architecture-b12两Python修首个完成帧有界等待：严格结构、已启用录制、当前双零/valid=false才可在原5s内等待；pin累计代际，缺字段/管道/所有者/代际错误立即失败。无C++或第二实机权限。P3仍只设计、无生产策略落地。

## 2026-09-20 05:34 — 运行前恢复边界闭合，授权首轮 OFF 诊断

- b10仅五Python变更，2969文件新freeze0530逐SHA核验；b11独立97项runner/analyzer、1337静态用例、270脚本入口和相关账户/帧锚/剔除门通过。父任务独立97项及freeze通过，C++/DLL8B4021未变，不重复构建。缺live条件恢复、第三方文件拒绝覆盖、隔离早检、最多一次HWND继续脉冲均有真实主流程/helper反例。
- architecture-b10与memory-b11均在报告落盘后CONTEXT_WINDOW_EXCEEDED，无final成功回执；报告/源码保留，前者同模型新会话继续只读P3设计。memory归纳工具修空页年龄/未知与证据覆盖分离，父任务13项通过，仍仅合成证明。
- 父任务首次只读runner前检被其自身py.exe启动器误判为另一runner而拒绝（未部署）；改用已核对的直接python.exe后同冻结输入前检通过，不放宽进程过滤。该调用方式写入lease。
- 所有生产写者冻结；授validation-b12唯一OFF诊断lease，manifest6E6A4E47，候选8B4021，恢复BF938，保护A0A51；固定隔离2560×1440、零global input、384MiB与完整caster不变，重型取证关、三槽原生截图开。失败/未覆盖如实记录；无ON、无重试、无发布接受。当前尚未开始运行，完成需独立复核恢复与原始数据。

## 2026-09-20 05:13 — 离线检查闭合，运行前安全与归因补强（未实机）

- validation-b09在source-freeze-0507上独立跑unittest1337/1337与root静态脚本逐入口270/270（无跳过）；py_compile/diff check通过，dirty902、DLL8B4021不变、零编译/游戏。配合b08定向8/8、Meson91/91与schema2真往返，本批离线门已闭合；不继承任何实机或视觉结论。
- architecture只读审查提出恢复缺live分支、隔离早检和ready脉冲次数等补强；父任务纠正两点：3槽原生异步截图不等于重型历史环；源码身份由父任务freeze/build/lease绑定，不能将缺额外manifest字段直接当旧DLL已混入证据。下一批只改Python事务边界，C++和现场不动。
- culling未应用提案存在滚动report重复四cut导致累计命中假倒退的错误；父任务要求按设备/代际/frame/stage去重、同key冲突拒绝再排序，不直接拼历史样本。P2a覆盖与恢复/收益分开，不将零命中称优化，也不以尚未修复的OFF基线作为拒绝所有ON诊断的理由。
- 四线新批：architecture事务安全+真实main/helper反例，culling归因解析器，memory只写ignored真实账户归纳工具，validation仅只读现场身份与草拟清单。无编译/游戏lease，待所有写者冻结与离线再验后由父任务决定唯一诊断轮。

## 2026-09-20 05:04 — 账户真往返已过，全量发现报告env守卫未同步（未实机）

- validation-b08真实dry-run严格2 edge、actual同2 exit0同步捕获、exact9目标no-work，DLL8B4021身份不变；8项定向CPU全过，91/91 Meson通过（8个目标本夜重编，余为现有二进制，非clean全重建）。真实schema2 census writer→reader12项、runner/analyzer75、producer帧锚7及相关门通过。
- unittest-discover静态1337项中1336通过/1失败，不能称270脚本全部入口执行。失败是父任务新增报告env矩阵但遗漏同步registry旧字面量守卫：129!=110；责任归父任务。编译lease释放、父任务复核零进程及2969文件未漂移；仍不部署。
- 父任务仅同步静态守卫到精确129，并对19个已有War3变量+OBS开关逐名要求BuildPerfEnvJson恰好一次；全文件两处既有history/export配置读取分别额外保留一次。父任务第一版错误要求全文件全为一次，定向14项13过/1失败已保留，随后按两处实际getEnvVar来源修正，不改registry禁新增策略env、其它导出计数或任何生产默认。新守卫待重新验证，旧失败不追认。
- S2实测本轮固定60万输入0差异，core/upper二次输出复用指针不变、各1次32B临时分配（旧各2次64B）；这是宿主CPU组件/适配建模证明，不等同游戏FPS提升。memory复核纠正：同页不重叠的长短寿命范围也能造成整页滞留，mixedTagOverlap=0不能排除；真实页寿命分组仍待实机账户。

## 2026-09-20 04:52 — 第二次编译结束、必需测试失败与夹具纠正（未实机）

- validation-b07实际47/47链接、exact9目标no-work exit0；但异步启动器未捕获actual退出码，且未执行初始`ninja -n`（只有依赖图query），均记为偏差，不补造记录。父任务观察BelowNormal父进程/Ninja/两路cc1plus，最终零编译/游戏进程、2969文件冻结复核一致，编译资源已释放。
- 8项定向CPU测试7通过/1失败。DLL为36,399,883B / `8B40212050A9C9CC7F0397F490C5D5F81D633F4D16F65A1DB205934DC3EE788E`，目前标为离线测试失败、不可部署；未运行后续全量门。
- 父任务逐行核实失败夹具：ordinal0..8中奇数1/3/5/7仅4项static，偶数5项not-static，原期望5/4颠倒。仅修正两项期望为4/5并写独立枚举注释，不改生产collector、阈值、9组合与500组位图oracle。新测试待独立重编与真实writer→Python门；原失败不被撤销。
- culling-b08指出canary尚缺P2a命中与拒绝/拷贝量归因门；编译后的index边界用例本轮已通过，但不构成inline消费几何/GPU内存收益。暂不授权任何部署，完整性与visualRecoveryProven=false不变。

## 2026-09-20 04:35 — 第二次源码冻结与独占离线编译（未实机）

- 父任务独立复跑runner/analyzer75项、producer帧锚7项、页寿命合成15项、index接线6项通过；strict phase现在只走精确PID管道，不读过时状态文件，正常ready诊断与五个phase分离，重复完成帧采用有界等待而非放宽判定。
- architecture核对仅新增既有低频status路径的一次monitor锁；War3Events→monitor嵌套存在，未见反向调用，但不称并发实机已验收。生产last-seal帧仍不能等同completed perf cut，非零累计压力解除增量保持未覆盖。
- culling-b07在报告/信箱后上下文超限，无工具final；保留失败和源，另开同模型只读复核。父任务发现其一字节超预算反例先被width2对齐门拒绝，已修为对齐2字节，Python6项通过，C++尚未跑。
- source-freeze-0434逐哈希2969文件一致，dirty902；相较0338仅26个授权路径新增/变化。所有写者冻结，授validation-b07唯一BelowNormal/-j2、exact DLL+8CPU测试目标编译lease；无部署/GPU许可。旧DLL仍不能作为新候选。

## 2026-09-20 04:21 — UV账户修正、验证帧域生产接线与失败封堵（未新编译）

- memory-b06已完成同源UV resolver与生产switch：共享UV严格使用position页/offset/capacity，逻辑引用保留、字节由union去重；无实际分配/回收/准入改变。父任务Python7项通过，新增C++未编译。b07将合成寿命模型的累计创建量与当前live/pending/freed分离，父任务15项通过；16MiB active不等于物理收益，P3仍未接生产。
- validation-b04r真实mock主流程/恢复与累计计数反例完成，64项sandbox测试由子线程实际运行；父任务审核并应用3文件补丁。全零计数也必须校验非零重置代际，非零累计的精确压力解除切点仍未证明，不从滚动报告区间猜测。既有帧恢复门与visualRecoveryProven=false保留。
- 父任务在perf monitor/hub/control-plane五文件增加同一锁内的最后已完成history帧锚、帧epoch与producer累计重置域；aggregate清零两处共用helper、wrap至0不复活旧域，报告同步导出epoch与冻结矩阵缺失env。生产接线待独立源码/锁链审查及编译；未把当前in-flight meta帧或VisibleRenderable帧冒充完成帧。
- culling-b05父任务18项Python通过；C++未跑。b06检查P2a未发现直接几何破坏，但指出inline有符号base/UV范围仍缺链接级反例；只批准两个测试文件补header级反例与精确slice接线守卫，不制造第三份计算实现。旧CSM矩阵hash不能升级为当前矩阵证明，仍禁Consume。
- 四线继续有界批次；所有编译/实机lease为空，本夜未部署，玩家现场和384MiB完整性边界不变。首轮编译失败仍保留，不继承旧DLL/EXE结果。

## 2026-09-20 03:57 — 编译失败收口复核、帧域缺口与剔除证据上限

- 主线程复核 b03 原始日志：失败为 edge71 的 T19 未限定命名空间，已在途 edge72 随后结束；exit1，DLL 未重新链接。失败收口后仅修正两处完整命名空间调用，S2 单源静态通过；未再次编译，不把旧可执行文件算新通过。
- architecture-b05 回源指出 phase marker 的 VisibleRenderable 帧号与 perf 的 Present 业务帧号不是同一来源，跨报告累计值也缺重置域绑定；validation-b04须维持缺证未覆盖，补生产主流程/跨报告并集/逐帧接收端反例。架构任务因 max-tokens 结束，保留报告与失败状态，以同供应商模型的新上下文继续只读最小接线设计，未改共享服务或历史。
- culling-b04确认当前生产仅远级联 C2/C3 Observe；相机、矩阵、资源版本存在自比较/旧态标当前的证据缺口，全消费者覆盖未闭合。继续禁止提前剔除/Consume；新批只补离屏贡献等离线反例和精确接线提案，不能宣称少占快照内存或无漏影。
- b03 our-43-unittest.log 实为错误启动的 Python REPL / WinError123 重复日志，271,374,856B，不是测试结果；主线程确认进程已不在，保留失败并要求测试启动参数与超时保护。git默认status850行与-uall900路径为统计方式不同；diff check的54行是CRLF提示，主线程复跑exit0。
- 编译与游戏lease均空；本夜无部署，玩家现场未触碰，页384MiB预算与caster完整性不变。
- memory-b05明确共享UV按精确position页/offset/capacity入账：字节由现有union去重，逻辑引用仍记两份，不能用0或陈旧uvCapacity。父任务落device侧resolver调用，memory-b06负责同源helper与反例；这是未完成集成态，须helper/测试完成再冻结编译。未改变实际捕获/分配/回收策略。
- validation-b04在四份Python已写入WIP后达到max-tokens、无完成报告；原样备份该WIP后交同模型新会话接续b04r，不重做/覆盖既有实现，不继承任何编译实机权限。

## 2026-09-20 03:44 — 联合编译失败如实收口，禁止实机

- 首次82edge联合构建在72/82停止：T19测试未限定RuntimeGroupPaletteTryByteLength命名空间，编译exit1。不是运行时差分失败，也没有新的有效DLL/no-work；旧68176A仍source-stale。父任务核零编译进程、关闭lease，测试线程先保留失败回执，不自改用例后追认。
- 后续仅批准validation-b04修正Python runner实际主流程与跨报告分析，补完整mock main五阶段与故障恢复；不允许接触实机。C++ namespace与shared-UV账户改动由父任务在失败收口后另行处理；再冻结、再独占编译，不混旧验证。
- architecture-b04已回执：scan趟数是源码/硬编码指标，成本计数仅派生adapter测试，不是实际生产链；prefix vector仍有堆分配，不能声称全部热路径分配消失。memory/culling的上下文失败发生在报告（及本地信箱）落盘后，工具final仍失败，二者分开记账。

## 2026-09-20 03:40 — 主线程发现真实runner主流程阻塞与共享UV账户缺陷

- validation-b03源码冻结核对2967文件一致，Meson重新生成后explicit7目标preflight82edge（页census头经device传递造成DLL/dxso重编），主线程观察到Ninja及两cc1plus均BelowNormal。仍在构建，不报通过。
- 主线程读runner main并独立复现identity键不匹配：manifest是liveRecovery，before是live；直接索引必KeyError。另phaseMarkers.json每阶段都调用CreateNew save，第二阶段必FileExistsError。43个helper测试未覆盖主流程，两项均须补真实mock主流程测试后才可实机；冻结期间不偷改源码。
- memory-b04报告提出shared-UV span问题，主线程回源确认：共享position页时未赋uvCapacity，而census无条件拿它计量，可能0或上一份独立UV的陈旧容量。当前结果不能直接用于P3决策/实机账户接受；需在编译lease收口后作精确alias来源修正，不能普遍跳过非法范围。
- memory亦在报告落盘后触发DSH上下文上限；保留失败及报告，用同供应商同模型新上下文继续只读核查。当前MCP无compact接口，未编辑历史/重启服务。其他源码继续冻结，玩家现场未触碰。

## 2026-09-20 03:38 — 四线源码冻结与独占离线编译准入

- 主线程独立复跑72项Python/static（runner/恢复分析器43、页reader负例5、寿命模型9、union8、coverage7）及S2单源守卫通过，diff check通过；对882个起点dirty逐SHA审计：16个获批路径变化、无文件丢失。当前dirty900，新18项均在本夜任务范围。
- S2 b03以溢出安全的整数半开区间替换b02错误相邻别名判定；页账户b03修正Known False/Unknown倒置并补独立字节oracle。尚未新编译，不能把书写的C++断言算已运行。旧census EXE实际schema1，测试线观察到2项失败不追认为成功，须schema2真实writer→reader闭合。
- culling-b03在报告落盘后因DSH上下文超限失败，无完成回执；保留报告与失败状态，改由同供应商/模型新会话只读复核，不重复源码写入。报告指出当前捕获前缺完整CSM/全消费者条件，不启用coarseCull/Consume来假造内存收益。
- 全部生产写者冻结，发validation-b03唯一BelowNormal/-j2纯编译lease，限exact DLL与6个明确CPU测试目标，允许既有Meson重新生成两项新测试。其他三线只读审计；无部署/实机lease，玩家现场不变。页寿命策略仍为未接生产的离线原型，不宣称阴影恢复或物理显存节省。

## 2026-09-20 03:19 — Observe epoch接线与联合编译准备（纯静态）

- culling-b02追加正交w行与map/device三源证明，旧reject枚举不重排；父任务回源核对pass InvalidateMapEpoch与运行时epoch拒绝路径后接六个来源字段，input/draw/pass独立供值，不把current复制成candidate证明。Observe有效mask仍不消费，不改capture/CSM时序或预算。
- 主线程独立复跑union7/7与coverage7/7；新增六条生产赋值单源守卫后union8/8，相关diff check通过。C++/实机仍未跑，不能认证剔除无漏影。两个新纯CPU测试加入Meson：lifetime page policy与culling counterexamples，尚未生成/编译。
- architecture-b02已去掉trusted max并用固定256槽单趟扫描，但别名检查把半开区间的相邻端点当重叠；退回b03补整数溢出安全范围判据及相邻反例，并清理文档中仍像现行方案的b01旧接口。未用该候选编译或部署。
- culling生产源冻结转只读接线审计；memory/architecture/validation继续完成当前批次。构建与游戏lease仍为空。

## 2026-09-20 03:12 — 页账户生产接线与三态缺陷退回（未编译）

- 主线程读完memory-b02的endpoint sweep，发现failed/touched两处把Known False计为Unknown、真正Unknown未计。这会令真实writer与reader不一致；现有Python手造正例不能覆盖该缺陷。已退回memory-b03修正并补全9组三态/独立字节oracle，当前不接受该组件验证。
- 父任务仅在Present census inspect接入RangeTags，全部取自已有entry；static=false仍非动态证明，failure与touched不受owner优先级吞并。退休会话的lastAccess/lastAttempt不能与新帧域比较，故age/touched显式unknown；没有改缓存/页分配/回收/预算/准入。
- 生产新接线与组件均等待冻结后C++编译及真实writer→reader；没有运行游戏/改变玩家DLL，不能报预算恢复或内存节省。

## 2026-09-20 03:10 — 三线首批审查与第二批修订（未构建/实机）

- architecture/validation/culling 首批均通过真实 DSH wait_turn 返回；主线程阅读 scoped diff、共享 policy 和实际 runner/reader 调用链，未把子线程自报 Python/static 通过当成 C++/实机验收。
- S2 直接复用输出容量方向保留，但新增 caller-provided maxSlot 可让 sparse fallback 输出容量小于真实索引；退回移除此信任参数、合并有界扫描与别名合同。600015 差分尚未新跑，不作等价通过结论。
- 测试线已有严格 JSON 与事务骨架，但仍有业务帧/epoch 混用回退、float墙钟丢失、复制mtime充当报告时域、缺env静默与恢复门过弱问题；派第二批补真实 writer→reader 往返、冻结身份清单与反例。上次worldPtr=0的原因未定位，不能归因为continue键已关闭。现有隔离HWND消息分支已读，尚无新实机许可。
- 剔除线确认快照早于final CSM cull，后者不能减少已分配页。批准Observe纯policy的正交/map-device证明收口；不启Consume、不跳capture，不把仅C2/C3 outside等同于全消费者无贡献。b01文档here-string直写属方法偏差，已要求后续只用补丁。
- 四线第二批均有精确工单；memory继续补静态标签/失败保留的区间并集。所有编译/游戏lease仍为空，源在变更时DLL只保留旧checkpoint身份，不称source-consistent新候选；玩家现场和根稳定日志未修改。

## 2026-09-20 02:55 — 内存线首批复核与账户缺项（纯离线）

- memory-b01 通过实际 DSH wait_turn 返回，信箱报告 SHA/大小核对；DSH 无直接 Codex 发消息工具，mailbox+dsh-final 路线已实证闭合，没有虚称直接推送。
- 主线程独立复跑 9/9 Python/static 并读生产 census：现有 Owner 是访问/失败/退休状态分区，staticReferences只是引用数，确实不足以量化同页静态标签/非静态标签/失败保留的字节。未将该缺口冒充播放器已证实的内存成因。
- 新寿命分页纯header及C++测试暂未编译/接生产；保留共享cap/尾追加/借页回退，策略正确性与收益未接受。主线程已派memory-b02补只读标签区间并集、严格schema/reader及原型反例；device接线留父任务，不扩预算/准入/fence。
- 其余三线仍在执行，无编译/游戏lease；当前DLL未重建或部署。没有阴影恢复/完整长期计划完成结论。

## 2026-09-20 — 无人值守夜班启动与可回溯备份（尚非产品验证）

- 用户授权主线程协调四条 DSH 长线：架构成本收口、预算内存、保守剔除、独立测试。四会话均已明确选择 commandcode/deepseek/deepseek-v4.1-flash 并收到任务 acceptance；acceptance 不等于完成。
- 新协作 skill 已通过 quick_validate，CreateNew 回执脚本自测通过；直接 Codex 消息仅在子线程真实工具可用时使用，否则 DSH final/wait + 本地信箱，由每10分钟heartbeat补查，未编造回调服务。
- 开工882 dirty已备份到 D:/WarVK-Backups/20260920-night-0238：HEAD bundle verify、ZIP CRC/逐路径SHA与源复核通过，当前DLL 68176A原样备份；无git提交/重置。起点源码/测试不能冒充后续候选验证。
- 08:30收口、09:00停止。白名单与单构建/游戏lease分离，BelowNormal/-j2、隔离2560x1440零全局输入；不提高预算、不减少必要caster，不接x64/Water/自动模型灯，不自动发布。当前未部署/未游戏，未因磁盘清理授权删除任何产物。
- 本夜计划、截止和证据边界见 docs/plan/2026-09-20-unattended-night-shift.md；协调账本与任务回执在 AutoTest/artifacts/night-shift-20260920/。

## 2026-09-19 — P1b 页账户 + P2a 上传索引范围减量（实现/离线通过，未部署）

- 首批实现落地：默认关闭的小账户在已识别 Present 所有者采集，使用既有 D3D9 设备锁保护 live 遍历，释放后排序并向 perf 按值发布；控制面不读 live cache。四 cut、128 页/16384 entries/32768 slices，别名区间去重；区分 touched/failed/recent/cold/retired、页尾与 cache 未归属区，明确不代表完整物理 residency 或可立即回收 GPU 空间。
- 默认关闭的上传范围候选接入 DIP-UP 与 dynamic-system-memory IB 两个现有 memcpy 点。同一 1KiB scratch 的实际上传字节同时计算 min/max/hash，封装值绑定 allocation/destination/长度/类型/map/单调 serial，消费仍需原 signed base/VB/UV 范围门。每 draw 16KiB、每帧 256KiB 上限；超限走原 memcpy/保守快照。不读 WC 输出，不新建 GPU 副本，不改预算/准入/页策略/fence。
- 组合 DLL Below Normal / 最多 -j2；最终锁边界修订 exact 2/2、no-work、PE32/i386：36,386,190 B / `68176AE78808CA2329EB9513502D805426401FC91101134E11C71C104F3C5378`。Meson 89/89；census 3534、upload summary 90032 断言通过。静态首轮 266/267（旧全文件 env 数量守卫）；仅增加两个具名开关的精确预期、保留 registry 禁新增门，重跑 267/267。另 producer 片段4/4、报告 reader6/6、离线政策模型15/15、4-file py_compile、全树 diff check 通过。
- CPU 成本实测：scratch 1,065,144 B / History 66,248 B；最大别名 Collector 2559µs。16KiB×10000 Copy+摘要175781µs，对照 memcpy4717µs，属于缓存内存微基准，不是玩家帧收益。关闭仍有门分支/小型值初始化，不能称零指令开销。开启不占取证大图 ring；所有权证明仍区分 CPU Page 引用与 CS/GPU 最后使用。
- 本轮原 dirty874→882（仅新增8路径），另保留既有改动；对既有文件的本轮修改为 device.h/.cpp、perf_monitor.h/.cpp、meson、旧 env 守卫、AGENTS 与本日志。HEAD 不变，未部署/未游戏/未提交/未合并；相关游戏/编译/runner 进程0。玩家现场与根稳定日志未写入。
- **仍未证明阴影恢复**：P2a 实际命中占比、净复制/页占用下降、合法对象持续绘制、低视角恢复和 p95/p99 待同候选 OFF/ON 实机。P3 页组织/驱逐未实施，要先取得真实页账户；候选默认关闭，不能把上轮 BF938 失败追认成通过。详情与后续判据：`docs/plan/2026-09-19-stage11-budget-p1b-p2a-implementation.md`。

## 2026-09-19 — 预算解决方案研究：混页/范围减量反例与最小落地合同（纯离线）

- 新增独立 Python policy mirror + 15 项测试，通过别名去重、尾洞不复用、required 保留、所有页回收、完成前 backing 保留及固定种子 4000 次账户闭合。它不是生产 C++/GPU 门禁；源码 SHA/常量绑定不能替代生产调用验证。
- 构造 24 页各 256B 长期锚点：6KiB CPU-owned 可留住 384MiB，下一 512KiB 失败；共享总预算但按寿命选页时同数据留 16MiB、再申请成功变 32MiB。分组模型创建/回收更频繁，成本须另测。768MiB 只把该模式延迟到 48 轮；固定硬拆额度则会产生空闲不可借问题。均非玩家现场占比。
- 400 个合成 draw 的 512KiB vs 52×32B position（含独立 IB/对齐）对应 page capacity 208MiB vs 16MiB；不能用此人工分布报游戏收益。真实全部必需工作集超限反例仍失败，不承诺只改 GC 就够。
- 进一步审计确认内容代际在写 Lock 时推进，摘要仍需成功 Unlock/上传顺序；GPU-skin index ticket 仅特殊 bypass scope 可用，direct-static/upload 是 dev-only，不能打开实验冒充通用优化。当前页缺 TRANSFER_SRC，压实还需用途/同步/额外空间，不作为首刀。
- 工单：P1b owner 安全点轻量页/切片账与索引 unknown 具名分桶 → P2 主导可证明来源范围减量 → 按账选择 P3 寿命相容页/压力解除 → 同预算完整阴影恢复。无新增图像环、无生产源码改动、无 Ninja/构建/部署/游戏/合并/提交；BF938 玩家失败仍有效，稳定日志不改。
- 资料：[解决方案研究](../research/2026-09-19-stage11-budget-solution-research.md)；模型结果 `AutoTest/artifacts/stage11-budget-research-20260919/policy-model.json`。本轮重新查阅 Khronos memory/copy 与 Microsoft D3D lock/DIP/UP 一手文档并记录适用条件。
- 收尾复验：模型 15/15、两文件 py_compile、两份既有文档 diff --check 与三个新文件尾随空白检查通过；结果绑定的五份生产源 SHA 均未变，build32 DLL 仍为 36,359,521 B / BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3。`status -uall` 为 874 项（开工 871 + 本轮 3 个新路径）；没有把既有 dirty 内容归为本轮修改。

## 2026-09-19 — BF938 玩家回归未通过阴影恢复门（九份报告离线复核）

- 再核账户术语：384MiB 属 Stage11SnapshotPage，不是帧级 ShadowArena/全部显存。draw-time cache entry 直接持有同池页中的 GPU 副本，跨帧缓存会延长该页寿命；不是给 Arena 重复加账。独立模型缓存不能与此混称，长期静态/短寿命快照混页待治理。详见调查 §8，仅源码只读核对与文档澄清。
- 玩家后续补充“大地图、镜头内对象少于高压图”，怀疑旧区域快照积累。定向源码核对确认静态快照有离屏保留、108000 帧闲置门及 inactive 64MiB 逻辑切片 LRU，另有 120 帧近期保护；它不是整页 resident 硬上限。Stage11 入口随原生 draw 捕获，未证明整图预加载；未知索引范围可放大到整个有界 VB。具体旧区域持有量未测，不以机制替代现场归因。P1b 按访问年龄/来源范围与整页持有联合核算，详见调查 §7；仅文档、未改生产源码/构建/部署。
- 用户确认全场景丢影、移动后恢复、某些地点再次消失；新报告 meta 确认 BF938 / 36,359,521 B，不是旧 DLL。九份完整报告严格 root/递归重复键解析通过；33,243 行去重为连续 15,214 帧、18,029 重叠行内容与 frameWallMs 一致，原文件 SHA 不变。
- 有连续 5110 帧 / 52.270132 秒 shadowMapRenderSerial 停在 2837、prepared 全零，而捕获/replay/receiver 仍活动；随后 2838..2964 恢复、266 帧停住、2965..3257 恢复、309 帧再停至末尾。更长 76.895273 秒区间含 8 行 serial=0，未冒充完整有效序列；玩家画面反馈与计数证据分列。
- 末报告容量拒绝 353,592、producer-incomplete 7,841、无完整图 7,794；resident 有效发布 384 MiB，GPU page allocation failure=0。20:55:30→20:56:30 成功 suballocation 累计字节不变、容量拒绝增加 223,977；后段重新分配。累计回收过 31 页，不能称完全不 GC，亦不能把池预算拒绝称物理显存 OOM。
- 源码复核：尾部追加+仅整页回收，以及未知索引范围复制完整有界 VB 仍存在。具体页持有者/页内死区占比尚无账户；完整性检查早于 prepared 统计，不能用 prepared=0 推断未进入 replay。HasCompleteShadowMap 聚合为 max，Last 字节 gauge 稀疏发布；不得当作坏态当帧的有效状态。
- 新跑 59 项只读分析断言、原报告哈希复核通过。BF938 **压力恢复未通过**，合并/发布继续阻塞；P1b 有界持有者账户与 P2 范围证明、P3 回收仍待。未改生产源码、未构建/部署/游戏/提交，稳定 CHANGELOG 不变。
- 详情：`docs/research/2026-09-19-bf938-player-shadow-loss-report.md`；原始九份身份、去重序列、脚本及验证回执：`AutoTest/artifacts/player-bf938-205659-20260919/`。

## 2026-09-19 — BF938 首批预算候选交付玩家测试（仅打包，未部署/发布）

- 用户要求自行实机测试。重新核验 build32 DLL PE32/i386、36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`，原字节冻结到桌面 `WarVK-v1.22-Budget-P0P1a-BF938FBB-20260919/`。
- ZIP 同名，8,019,253 B / `B0BF4DFB14CCB1F5AB742DBBE61C0B6529A6988FEBEE64FCECBCAF1D7F2F7BC1`，含 DLL、测试说明、PS1/CMD 启动器、候选 manifest 共 5 文件；CRC + 每文件 SHA 校验通过，source 不变。
- 启动器先核包内与现场 DLL，拒绝已有游戏/编辑器进程；不自动安装/覆盖文件或改地图/注册表。子进程重型帧取证关闭，perf 记录开启、每 20 秒自动导出、4000 帧窗口、384 MiB 预算；玩家自行设 2560×1440 并正常选图，记录低视角多点往返后阴影消失/恢复及时间。
- Windows PowerShell VerifyPackageOnly 实测通过、未启动游戏；首版预检发现 Get-FileHash 在该子进程不可用，已改用 .NET SHA256，未绕过身份门。压缩文件初次后缀拼接截短了名称，核 SHA 后改成完整包名，内容 SHA 不变；交付回执已记最终路径。
- 本轮不编译、不部署、不提交、不合并、不修改生产源或根稳定日志。包内明确此前隔离运行 ready 失败，P1b/P2/P3/压力恢复尚待验证，不能称阴影已修复。玩家测试期间不主动覆盖现场或并发编译/实机，等待反馈。
- 回执：`AutoTest/artifacts/v122-memory-recovery-20260919/player-delivery-bf938.json`。原现场 DLL 和受保护目录均未写入。

## 2026-09-19 — 一次隔离压力 canary 未通过进图门；恢复清场、合并暂缓

- 用户授权测试及“通过后合并”；核验 P1b/P2/P3 未完成，未把上一轮 BF938 离线候选晋升稳定。本轮无 C++/Shader 修改、无构建、无 merge/commit/push。
- 第一次驱动因 B API 参数不匹配在启动前失败（实际进程 0），修正并加签名回归后只启动一个 fresh process。客户区及最终 swapchain 2560×1440、非交互隔离、零全局输入、full_default/384 MiB 不变；100 秒地图 ready 超时，gameStarted=false/worldPtr=0，压力路线未执行，无新性能报告。不能称运行通过或推断 GPU/内存根因。
- stock EndGame 未退出进程；保留失败 receipt，随后用精确 PID/创建时刻/EXE/句柄在其隔离桌面正常 WM_CLOSE，无强杀。现场已恢复 F2A7，另一个 E:/Work/War3 目录 A0A5 未写；候选、EXE/Game/map 身份不变，视频注册表恢复，相关进程 0、未见新增 GPU 错误事件/dump。编译资源已释放；游戏资源已释放。
- DSH 指定 commandcode/deepseek-v4.1-flash 只读复核启动接口与事务；主线程采纳接口/部分写入风险，不采纳“复制完成后才标记部署”这一会遗漏半写恢复的建议。未委派构建或实机。
- 新驱动离线加固为 CreateNew 暂存+核验+不覆盖 rename-park+条件回滚，补精确隔离窗口正常关闭封装；13/13 纯 Python、py_compile、按仓库配置 diff check 通过。该新封装未补跑游戏，不归入旧运行证据；旧 Meson87/静态265不是本轮重跑。
- A/B 已提交差异 1/221，A dirty175，B 开工867；未提交候选不会由普通 merge 自动进入主树。本轮 B 新增3路径至870，既有变更保留。发布配置、压力恢复、范围放大与回收、视觉长期门及两树备份/集成仍待完成。
- 详情与失败/恢复证据索引：`2026-09-19-pressure-canary-and-merge-hold.md`。根稳定 CHANGELOG 保持不变，64 位、未完成 Water 与自动原生模型灯继续排除。

## 2026-09-19 — 内存恢复 P0/P1a 首批：失败寿命、backing 解除与可信输出（离线候选）

- 按 Pro 后修订计划开始实施：仅删 canonical-ready 第二组 emitter；PRE/POST 按所选 profile 只缩小并严格拒绝非法值。严格读方及旧无效报告不改。
- 共享栈上 capture transaction 分开尝试/成功帧：失败不刷新成功寿命，保留 backing 仍记账；最终完整且 settlement 结束后才激活 full-key ledger。GPU-skin 替换/失败两点统一解除 position 与 UV alias 的 Buffer/pin/Page/offset/info/capacity；独立 IB/UV 保留重试，无 fence/预算/准入放宽。
- Below Normal / -j2 exact DLL exit 0、no-work、PE32/i386；36,359,521 B / `BF938FBB6FBBB0B84A64EFE3A3E32545AAB98402D17180D2BDD6CA466A460CB3`。Meson 87/87；静态首轮 263/265，迁移两条旧形状断言后全量重跑 265/265；生产 emitter 片段 4/4、共享 lifetime 模板 617 断言及 py_compile/diff 通过。片段不是完整报告导出，CPU owner 夹具不是 GPU 完成证明。
- DSH 按用户指定使用 commandcode/deepseek-v4.1-flash，只读接线/发布文案机械复核，禁止生产编辑/并发构建。重启后的凭据已更新；不记录凭据内容，不重启共享服务。
- DSH 已返回并 idle；主线程独立处置其意见，补清册、历史默认说明与测试 SHA 绑定。它因缺 apply_patch 改用 Set-Content 写唯一 ignored 报告，已纠正为后续只回文字，不允许生产编辑；无源码被其改动。最终 SHA 绑定全量静态再次 265/265、测试文件前后不变，DLL 仍 BF938FBB；编译/游戏资源已释放。
- 未部署/未游戏/未提交；玩家现场、研究包、稳定 CHANGELOG 不改。P1b census、P2 范围放大、P3 回收及 P4 压力/画面恢复仍待办，不能宣称阴影已修复。详情 `2026-09-19-memory-recovery-p0-p1a-checkpoint.md`。

## 2026-09-19 — 用户裁定 64 位延期与 Arena 内存口径澄清（非功能改动）

- 用户明确 v1.22 不推出 64 位更新：E3 CPU 取证宿主接入、GPU 外置与完整渲染宿主全部延后；本版集中 P0–P4 收尾和阴影消失修复。已同步发布门、修订计划、进度表及 AGENTS；实验源码保留，稳定日志不变。
- 源码复核：ShadowArena 和 Stage11SnapshotPage 是两套 DEVICE_LOCAL GPU 缓冲管理，前者帧级 completion 轮转，后者持久 draw-time 切片/整页回收；本次 384 MiB ResidentCapacity 属于后者，不能称其是 CPU 模型内存。
- 模型缓存确有 CPU positions/normals/UV/index/group 数组；CPU 元数据、上传暂存、D3D9 必要副本、取证 staging 与 GPU payload 应分账。常驻模型不能直接替代原生动态/已变形流，Buffer 引用不冻结其字节；GPU copy 不等于全部回读 CPU。
- 重新查阅 Vulkan memory flags 与 D3D9 managed-resource 一手资料；未测 cache 命中比例或全局 CPU/GPU 实占。本轮仅只读源码核验与文档范围更新，无构建/部署/游戏/源码修改。

## 2026-09-19 — Pro 完整审核采纳与内存恢复计划修订（只读核验 + 文档，非修复）

- 完整阅读用户提供的 73,109 B 审核（SHA `9BD1626FC0026D3219BF3A7FCEF2C9013A22F839B4F5D1CA6630F7CDCF54CAFC`）；核对包内索引覆盖的 1,963 个 src/subprojects/render_host 文件与当前源码完全一致。附件建议不当作自动执行授权。
- 独立解码原始 inputs：224 batches / 10,026 draw metadata，2,238 Copied gather 的地址/valid/reserved 检查通过，2,199 与原始 IB 对照通过。batch4731/frame63735/draw2 的 105 索引实际涉及 52 顶点、1,664 B，而源范围为 512 KiB；只证明局部范围放大，不推算整池净浪费或画面正确。
- 采纳“ResidentCapacity → 必需输入不完整 → 整帧 CSM 拒绝 → 8 帧保留后失效”直接机制；容量拒绝在 GPU create 前，不能称物理显存 OOM。逐页所有者、混合寿命死区及失败 entry 对现场占比仍未闭合；Pro 模型 harness 本轮未重跑。
- 源码复核失败捕获刷新活跃状态、占用账滞后及条件性 Page 清理缺口。IB 的 HOST_CACHED 防护保留；不以逐 draw 扫描非缓存映射取代低成本范围证明。重新核验 vkFreeMemory、CancelIoEx、Win32 WSI 三项一手规范。
- 新计划 `docs/plan/2026-09-19-post-pro-review-memory-recovery-plan.md`：P0 可信报告/profile；P1 失败 backing + 有界 census；P2 正式同代索引范围；P3 按证据选择回收；P4 高压恢复/画面/成本门。E3 CPU 外置单列，真正 64 位先过窗口/语义原型，不外置半套 shadow pass 作为主修复。
- 本轮仅计划与入口文档更新，无 C++/Shader、Ninja/构建/部署/游戏/提交；原进度约 60% / 40% 不因读完报告上调，稳定日志不变。
- 收尾：dirty 859→860，仅新增修订计划；更新 AGENTS、旧进度入口与本日志。既有非授权 dirty 内容 SHA 无变化；DLL 仍 F2A7、审核 ZIP 仍 CE7204CB。修订计划 17,457 B / `40BBD5EB4E6C374E90558D14719556B4C89323949BC5EEC918E08B1C064B3BDA`；文档空白/相对链接与 AGENTS diff check 通过。

## 2026-09-19 — 长期重构进度复核与基础收尾队列（文档整理，非功能修复）

- 按用户要求对照当前 B 主树与原长期计划，分开评估实现/验收：管理估算约 60% / 40%，不是测试通过率或稳定发布比例；模块权重、依据和剩余门见 `docs/plan/2026-09-19-longterm-progress-and-foundation-closeout.md`。64 位 CPU 取证产品化与完整渲染器单列，不混入旧重构分母。
- 确认 M1/M2 部分迁移、语义构建工作身份/代际/按值发布及诊断生产回归已形成基础；device/back-end 决策、domain/资源账户、跨 Pass 寿命与压力恢复仍未收拢。
- 新登记反例：取证 PRE/POST 覆盖声称“只缩小”，实际可将内部 pre 96 扩至 256、post 4 扩至 16；本轮只登记，未修改代码，不据此推定某次崩溃根因。producer canonical-ready JSON 重复键仍待最小修正，严禁放宽读方或追认旧报告。
- 清册同口径扫描目前为 418 个 src 字面量而非旧 412；六个新增中含一个测试探针，不称 418 个生产开关。旧 85 个待判项仍需分类。
- 本轮新跑纯 Python：frame-evidence 55/55、palette-object 111/111，均 exit 0；既有 Meson 日志 86 OK 仅只读复核，不追认为重跑。未 Ninja/构建/部署/游戏/提交，C++、Shader、研究 ZIP 和稳定 CHANGELOG 保持不变。
- 收尾只新增本次进度文档，dirty 858→859；AGENTS 仅加当前入口、开发日志登记。与审核包索引比对 3254 个源文本及既有 dirty 哈希，无非授权差异；研究 ZIP SHA 仍为 `CE7204CBB0727BE38231838A43EADA96D5350076785D865A6941535380D43165`，DLL 身份仍为 F2A7。定向 diff check、两份文档空白及新文档链接检查通过。

## 2026-09-19 — 内存分配与64位宿主独立研究包（证据整理，非修复/非发布）

- 按用户请求打包B工作树当前源文本、Shader、测试/逆向/生命周期文档、15:41原始报告/CPU事件/inputs、分开标记的15:20证据与64位CPU实验回执；研究提示词覆盖allocator账户、回收/恢复及跨位数渲染迁移协议。
- 更正旧交接：prepared=0不能证明没进replay；主样本ProducerIncomplete拒绝后render serial停在4954，8帧保留期后complete map失效；1231与4548为不同运行；used不是live几何量，E2 x86约5MiB不是节省量。
- 无C++/Shader改动、无Ninja/构建/游戏/部署/上传。根稳定日志不变，旧dirty内容除本日志外冻结。原始captureComplete/rootCauseReady=false保留；各页持有者与具体内存放大来源仍待研究。
- 打包严格JSON检查新发现15:41 HTML重复canonical-ready字段，r1停止；r2明确报告strictRootValid=false，用保留重复键值的法证审计、歧义键省略及无歧义workload提取继续研究，不改producer/parser、不追认运行有效。
- 详情：`2026-09-19-memory-x64-review-package.md`。最终交付身份与ZIP逐文件校验以外部整理目录delivery_receipt.json为准，脚本成功后才形成交付证明；既有测试日志不追认为本轮测试。
- 收尾：r2 ZIP 68,190,330B / `CE7204CBB0727BE38231838A43EADA96D5350076785D865A6941535380D43165`，3300文件，CRC+逐文件SHA通过，法证提取关键断言通过；dirty858、diff检查exit0。外层归档SHA形成后仅本地两份审核文档追加回执，压缩包及其source快照保持冻结。

## 2026-09-18 — v3 正常观察链「读方注册」缺口闭合 + 两类链生产入口端到端（未提交 / 未部署 / 未晋升稳定）

- **缺口（自我更正）**：批次 3 的「有版本正常观察链」**只做了写方** —— 写方按 `firstSightUsed() ? 3 : 2` 会发出 **版本 3**，
  而两个读方（`analyze_frame_evidence.py` 的 `PALETTE_OBJECT_BLOCK_FIELDS` 与 palette 解析器）**只注册 {1,2}**。
  实测 `version=3` 喂入两个读方均 `ValueError: unknown paletteObject extension version 3 (registered versions: [1, 2])`
  ⇒ **实机一旦真的走首见链，整份导出会被拒绝**，而不是局部降级。此前我报告「③ 实现完成」是**错误**的。
- **漏检根因**：批次 3 的静态门禁只断言 Python 侧**符号存在**（常量、阶段表、旧版本守卫），**没有一条**把 C++ 写方与 Python 读方**绑起来**；
  记录器侧 Case24 只测 C++、不过读方 ⇒ **符号存在 ≠ 端到端可用**。
- **修复（四处，缺一不可）**：① `analyze_frame_evidence.py` 新增 `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3` 并**登记进** `PALETTE_OBJECT_BLOCK_FIELDS`；
  ② `analyze_palette_object_evidence.py` 新增 `FIRST_SIGHT_COUNTER_FIELDS`，`counters` 校验由**精确集合相等**改为**按版本**（v3 才允许两个首见计数器）；
  ③ `test_palette_object_evidence_analysis_static.py` 从"非法版本"列表移除 3，新增 **v3 端到端正向用例**，fixture 在 v3 时补齐首见计数器（照生产形状）；
  ④ **计划外第二处同类冲突** `test_semantic_build_thread_gate_static.py:1975` 也把 3 当非法版本 ⇒ 同步修正。只改①⇒失败点变成 counter mismatch；只改①②⇒测试③失败。
- **跨语言门禁（防复发）**：`test_first_sight_observation_chain_static.py` §7 新增四组一致性检查 —— 把 C++ 写方的版本表达式与 Python 读方的**注册**绑起来，
  并强制保留一条 v3 端到端用例；显式禁止"只定义常量、不登记形状"的半截状态。
- **两类链生产入口端到端（本轮新增往返场景 G）**：用生产入口 `NoteFirstSight` + `NoteEnqueued`（**没有任何 `NoteReject`**）产出一份导出，
  **不需要实机**。实测：场景 A（拒绝恢复链）`formatVersion=2`、`recovered=[(...)]`；**场景 G（正常观察链）`formatVersion=3`、`recovered=[]`**
  ⇒ 两类链由生产入口**端到端可区分**，且**正常完成不被报成"恢复"**。另修正两处：`RunScenario("G")` 必须排在 F **之后**（A..F 序号被驱动钉死值依赖，插中间曾致 6 处失败）；
  驱动 `parse_exports` 正则 `SCENARIO=([A-F])` **硬编码**曾把 G 静默过滤。
- **验证**：`CHECKS=1117 FAILURES=0`、`SPEC_SATISFIED=[A..G]`、`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`；`ninja -n` no work；
  **全量静态 259 scripts / 0 failed**；**meson 85 Ok / 0 Fail**。
- **检查点身份（c1 更新 + ⚠️ 指纹修正）**：源码指纹 → **`B363F1769E9EF2514E5DD7616FB27C0C93EE23A2318B843BF26B2F71ED010EE2`（966 文件）**；根配置 `612B91B4…` 不变；
  **修正说明**：此前记录的 `57D917F9…` / `064B316A…`（均为 **932 文件**）是用**有缺陷的过滤**算出的 ——
  `-notmatch '\build'` 把 `src/minhook/build/**` 这 34 个**真实源码**（MinHook 的 Makefile/vcxproj 等，不是构建产物）错误排除，
  致使指纹建立在任意子集上、对外不可复现。归档本身无问题（见下条）。
  **候选 DLL 不变** `BBEF3BBC8F52088FD34D6B5F68FEFA111BAA31A7CB2331674AC3917DE0DE357A`（36,297,472 B，改动只在测试 .cpp 与 AutoTest ⇒ 不进 DLL）；归档 `E:\Work\WarVK-checkpoint-c1-20260918.zip` SHA `AEBC7724F517988B3C98C02F6F1494F3B482A3BB2357E2B1379F67EB534510B2`。
- **检查点可还原性（首次实证，目标①要求"完整可还原"）**：`Expand-Archive` 后必需件齐全（`meson.build`/`meson_options.txt`/`src`/`AutoTest`/`docs`/`meson-info`），
  归档内 `meson-info/intro-buildoptions.json` 自证实际选项为 `warvk_skin_palette_contract_candidate=True`、`buildtype=release`、`warning_level=2`、`b_ndebug=false`（与文档声称一致），
  且归档 `src/` 与仓库 `src/` 用**同一方法**比对**指纹完全相同（966 文件，MATCH=True）** ⇒ 归档是仓库的忠实快照。
  比对过程中另修一个方法论缺陷：`Expand-Archive` 返回 8.3 短名而 `$_.FullName` 是长名，`Substring` 砍错字符数曾致"假不匹配" ⇒ 教训：**比对失败时先怀疑比对本身**。
- **"74/76 Win32 runnable"是假缺口（同轮修正）**：两支探针 `war3_frame_recorder_defaults_{0,1}_test.exe` 需 **2 个参数**（打印 `argc==3`）；
  查 `src/d3d9/meson.build:347` 确认它们**已注册为 meson test**（`args:[mode, profile]`），meson 中该族 **10 个用例（5 模式 × 2 profile）全部 OK**
  ⇒ **覆盖完整**，74/76 是不带参数的临时扫描方法造成的假象。
- **边界**：**未**做实机验证；**仍无实机 v3 导出**；批次 4 第④问「正常对象是否被误伤」仍只有读码与纯谓词证据（运行时对照未做）；
  未运行 TDR/ABBA 与玩家前台门。**未提交、未部署、未晋升稳定**。
- 记录：`docs/plan/2026-09-18-v3-reader-not-registered.md`、`...-v3-reader-fix-complete.md`、`...-cross-language-version-gate.md`、`...-both-chains-e2e-via-production-entry.md`。
## 2026-09-18 — R3 wire 形状回归修复：全量静态 258/258 与 meson 85/85 **双双全绿**（未提交 / 未部署 / 未晋升稳定）

- **回归性质（自我更正）**：批次 2 的 R3 除「计数器原子化 + 移入子门判断之后」之外，**额外**把一批诊断量写进了线上协议块，破坏了**版本 2 的注册形状**。此前我两次误判：先推测为「meson 缺 env 注入」（实则 meson **确实**声明了 `env:` 三变量），后据 `ROUNDTRIP checks=116 PASS` 判断「直接运行通过」（该行只是 Python 包装器断言的前半，其后三重断言全败）。⇒ 教训：包装器驱动的测试必须读到「最终退出码 + 全部断言」。
- **两处同型修复**：① `war3_frame_evidence.cpp` 删除追加的 8 个块头字段（`armed`/`activeSession`/`enqueueBlockReached`/`appendEntered`/`productionInsertReached`/`productionNoteCalled`/`resetOrClearCount`），消费者调查确认为 0 引用（`activeSession` 的唯一 AutoTest 命中实为另一字段 `activeSessions`），并在注释中写明「R3 的验收性质由 sink 内部 `std::atomic` 与调用点 gating 保证，不依赖暴露到线上 wire；将来确需必须抬版本，不得在既有版本块追加字段，也不得放宽读方注册表」；② 批次 3 曾**无条件**把 `firstSightInserted`/`firstSightEmitted` 加进版本 2 的 counters 注册集合，改为**仅在版本 3**（`firstSightUsed()`）写入。
- **验证**：`ninja -C build32` 完成、`ninja -C build32 -n` = no work；**全量静态 258 scripts / 0 failed**；**meson 85 Ok / 0 Fail**（本会话首次双双全绿）；`war3_skin_palette_admission_test` 6/6、`war3_palette_object_evidence_test` 24/24、`war3_palette_object_evidence_cost_test` all passed、`war3_frame_evidence_runtime_test` checks=723 PASS。
- **边界**：**未**运行全量 Win32 runnable 套件、TDR/ABBA 与玩家前台门；**未**做实机验证；批次 4 第④问「正常对象是否被误伤」仍只有读码与纯谓词证据。未提交、未部署；现场 DLL 仍为 `F275545B…`（`519AFA69…` 备份在位），未改用户视频设置。
- 记录：`docs/plan/2026-09-18-r3-regression-fixed-both-suites-green.md`、`...-r3-wire-shape-regression.md`。
## 2026-09-18 — 批次 4 收尾：R1 行为证明 + 等价基线重建（**全量静态 258/258 通过**；未提交 / 未部署 / 未晋升稳定）

- **步骤①：证明新行为符合预期**。把 R1 的数量规则从「两处各写一份」抽为**单一纯谓词** `dxvk::war3::render::skin::ProducerGroupCountCovers(required, producerGroupCount)`（`war3_skin_palette_selection.h`），冷/热缓存两处**都改为调用它**；新增 `war3_skin_palette_admission_test` 案例 6（**6 checks / 0 failures**，明确断言 `(10,9) == false` 即 R1 目标行为），并以静态门禁钉死「恰好调用 2 次、字面比较不得复现」。自查纠正：我最初误以为该谓词管上界（上界由调用方 `>256u`/`>64` 把关），断言已改正为实际契约。
- **步骤②：生成器可复现性实证**。`%TEMP%` 下 `device_pre_m2_1/m2_2/m2_3.cpp` 等迁移前快照**全部在位**；运行 `--m2-2` 生成器**逐字节复算**出现有参考（36,025 B / `02FF8AFE…`，`BYTE-IDENTICAL: True`）⇒ 参考具备确定性可重建、可审计属性。
- **步骤③：B1 迁移后契约补丁（重建基线，而非放行差异）**。生成器新增 `POST_MIGRATION_CONTRACT_PATCHES` 与 `apply_post_migration_contract_patches()`：补丁**具名**（`R1-cold-cache-groupcount`）、带日期与依据、为**定点替换**，且锚点**必须恰好匹配一次**否则 `raise SystemExit`（绝不静默产出错误参考）；`main_m2_2` 先应用补丁再组装 header，并在 provenance 中列出已应用补丁。参考由 36,025 B / `02FF8AFE…` 变为 **37,375 B / `3D258F48355353B948ADDAB5266524DD07EEBF849B186C329DB2C54528274C1B`**；等价门禁差分**清零**（`diffLines=0`、`EXIT=0`）⇒ 行为差异靠「参照实现带上同一契约」消除，**逐位等价比较强度未降低**。同步了等价门禁的 `CHAIN_LEGACY_INC_SHA256` 与 `test_device_palette_slot_cache_producer_confirmation_static.py`（改为断言调用共享规则）。
- **步骤④：全套件复跑**。**全量静态 258 scripts / 0 failed**（本轮由 7 failed 降至 0；B1 一并解决 5 个等价门禁包装器、`m2_4_equivalence` 与 meson `war3_live_palette_selection`）。meson **84 ok / 1 FAIL**：剩余 `warark:war3_palette_object_wire_roundtrip` 在**显式设置** `DXVK_WAR3_FRAME_EVIDENCE=1` + `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1` 时 **116/116 PASS**，属「测试需 env 前置而 meson 未注入」，**尚未判定是否既有问题**。`ninja -C build32 -n` = no work。
- **精确表述（遵守「不得声称全门禁通过」）**：全量静态 258/258 通过；meson 84/85，剩 1 个环境前置相关问题待判定。未提交、未部署、未晋升稳定；现场 DLL 仍为 `F275545B…`（`519AFA69…` 备份在位），未改用户视频设置。
- 记录：`docs/plan/2026-09-18-b1-completed-baseline-rebuilt.md`、`...-step1-new-behavior-proven.md`、`...-b1-premise-verified.md`、`...-suite-failure-triage.md`。
## 2026-09-18 — 批次 3：有版本的「正常观察链」FirstSight 落地（写方/读方/测试同步；未提交 / 未部署 / 未晋升稳定）

- **背景**：外部独立复审确认「对象级记录器回答不了正常对象实际走了什么路径」（只有 `NoteReject` 建条目），并裁定：保留拒绝恢复链，**增加**可抽样的正常观察链；须为**有版本**的协议变更，旧版本含义不变；不得伪造 `Rejected`、不得让 `NoteEnqueued` 自动插表后放宽解析器、不得把未知身份升级为已证明。
- **写方**（`war3_palette_object_evidence.h` / `war3_frame_evidence.cpp` / `d3d9_device.cpp`）：新增阶段 `FirstSight=5` 与唯一入口 `NoteFirstSight()`（采样与容量沿用同一套表/预算；表满只计 `droppedTableFull`、**不发** TableFull 终态）；新增 `StageRank()`/`StageOfRank()` 并让 `MarkStage` 改用**语义秩**（`FirstSight` 取值 5 是故意的 fail-visible，其秩为 0，否则后续阶段会被误判 orderViolation）；`CloseWindow` 终态阶段改为**只对首见链**按 `StageOfRank(maxStage)` 回填（原实现无条件写 `Drawn`，对只到入队的首见链是虚假声明）；块版本改为 `version = firstSightUsed() ? 3 : 2`；生产采集点由 `NoteServed` 改为 `NoteFirstSight`（本处无 `selectedPalette`，发 ServedCandidate 属冒充）。
- **读方**（`AutoTest/analyze_palette_object_evidence.py`）：新增 `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3` 与阶段表 `5:'FirstSight'`；旧版本出现首见阶段**具名判错** `firstSightStageUnderLegacyVersion`（不得因认得该数字而默认接受）；首见链以 `FirstSight` 为链首 ⇒ 缺 `Rejected` 不再判截断；新增 `bothRejectedAndFirstSightHead`（两链互斥）与 `servedCandidateBeforeFirstSight`；`servedCandidateBeforeRejected` 等旧规则**保留**。
- **测试**：`war3_palette_object_evidence_test.cpp` 新增 **Case24**（两类链最小集成见证，25 项断言：首见链建立条目 / 链首 FirstSight / `rejectReason=NotChecked` / 生产形状键 `identityProofKind==0` 且 `identityWeak`+`epochUnknown` 原样携带 / 已认证键正向对照 `==1` / 终态 `WindowExpired` 且不谎报 `Drawn` / 恢复链 `Recovered`+`Drawn` 冻结形状 / 仅恢复链窗口 `firstSightUsed()==false`）。新增静态门禁 `AutoTest/test_first_sight_observation_chain_static.py`（写方+读方+测试三方一致性）。
- **验证（全量重建后、最新二进制；`ninja -C build32 -n` = no work）**：`war3_palette_object_evidence_test` **24 passed / 0 failed**；roundtrip **116/116**；evidence cost all passed；frame evidence runtime **723**；recorder memory **87**；shadow build lifecycle **187/187**；shadow geometry domain **46/46**；`test_palette_object_evidence_analysis_static.py` **OK**（读方契约在 v3 改动后仍全通过 ⇒ v1/v2 语义未破坏）；其余 palette 静态门禁 ×3 PASS。
- **自查纠正（诚实记录）**：写入解析器时一处 `if` 漏 4 空格缩进，使其所在函数提前结束、`return` 落到模块级 ⇒ `SyntaxError: 'return' outside function`。**`ast.parse` 不执行符号表检查故未发现**；已改用 `compile()` 复验并修复。教训：语法检查必须用 `compile()`。
- **未完成**：**全门禁未复跑**；批次 4（真实蒙皮选择入口的正常/异常对照、决定权迁出 `D3D9Ex`）未开始；**尚无实机含 FirstSight 的非零导出**。全部改动未提交、未部署、未晋升稳定。
- 记录：`docs/plan/2026-09-18-first-sight-observation-chain-record.md`。
## 2026-09-18 — 响应外部独立复审：检查点 c0 + 三项已确认缺陷修复 R1/R2/R3（未提交 / 未部署 / 未晋升稳定）

- **背景**：外部独立复审（9/18）给出 8 项确认/复现结论，并给出四批次工作建议。本轮执行批次 1（检查点）与批次 2（三项小范围缺陷）。
- **批次 1 检查点 c0**：新增 `docs/plan/2026-09-18-checkpoint-c0-record.md` 与存档 `E:\Work\WarVK-checkpoint-c0-20260918.zip`（6.02 MB，SHA-256 `8DD64006…`）。记录：`src/` 内容指纹 `FC55FF8F070468B36A3EFAB2CDF8B45E31EB897AB3662F649502021EF11D9D1F`（965 文件）、根构建配置指纹 `6ED55D2BB3439CF16721DB2E72E4FF780483782D7684FECC0502A3EBDC9F41CD`、产出 DLL 36,288,789 B / `FA7945A1F8C34DF37ED47820EFA7902CBD37200B0ECF26EAD0E26D6FE01162C3`。**未提交**，以内容指纹代替 commit id。环境区分（玩家目录 / 构建目录 / 沙箱已删除）与回退点一并显式记录。
- **回答复审未决项**：`warvk_skin_palette_contract_candidate` 在 `meson_options.txt` 默认 `false`，但 `build32` 实际配置值为 **True** ⇒ 我们构建并实测部署的 DLL **确实启用了严格 palette 契约**。限定：仅证明构建期启用，不等于每条运行路径必经该检查。
- **批次 2 修复（三项，各自独立、最小改动）**：
  · **R1** `src/d3d9/war3/semantic/war3_live_palette_selection.cpp`：冷缓存首次查询在取得 producer slot 后**缺少** `requiredPaletteCount` 数量检查（热缓存命中路径有），导致「同一对象、同一数量条件」冷缓存接受、热缓存拒绝。现已补上同规则拒绝（不足即返回 `0xFFFFFFFFu`，不写负缓存）。
  · **R2** `src/d3d9/d3d9_device.cpp`：生产采集点 `MakePaletteObjectFrames(..., 0u, true)` 的第 4 参是 `nativeKnown`，该处**拿不到** native 帧却传 `true` ⇒ 把「未知」写成「已知且等于 0」。已改为 `false`。
  · **R3** `src/d3d9/war3/tools/war3_palette_object_evidence_sink.cpp` + `d3d9_device.cpp`：五个诊断到达计数器原为普通 `uint64_t`（渲染线程写、控制面客户线程读、无共同锁），且至少两处计数位于子门判断之前。已改 `std::atomic<uint64_t>`（relaxed）、把 Reset/Arm/Clear 自增移入 guard 之内、把两个调用点移入子门判断之内（关闭诊断零写入）。
- **新增回归门禁**：`AutoTest/test_independent_review_sep18_fixes_static.py` —— 把三项缺口钉成结构不变式（每个 producer 绑定查询之后必须出现与 `requiredPaletteCount` 的比较；采集点必须传 `false`；计数器必须原子、`fetch_add`/`load`、且受子门约束）。
- **验证（本批次范围内全部 PASS）**：palette object wire roundtrip 116/116；evidence cost 38/38；frame evidence runtime 723；recorder memory 87；shadow build lifecycle 187/187；shadow geometry domain 46/46；新增 R1/R2/R3 门禁；3 个既有 palette 静态门禁。DLL 构建成功。
- **未完成（诚实）**：**全门禁未复跑**（全量静态/meson/Win32 runnable/TDR/ABBA/玩家前台门）；批次 3（有版本的正常观察链）与批次 4（真实蒙皮路径端到端对照 + 决定权迁出 `D3D9Ex`）**尚未开始**；现场 DLL 未替换为本轮构建。
- 全部改动**未提交、未部署、未晋升稳定**。
## 2026-09-18 — 外部审核包整理（进展报告 + 全量源码清单 + 源码存档）（纯整理；未改生产代码 / 未提交 / 未部署 / 未晋升稳定）

- **背景**：用户要求整理当前源码内容供外部研究端审核「相较上一次重构的进展」。产出为三件套，不改任何生产代码。
- **新增交付**：① `docs/plan/2026-09-18-openai-review-brief.md`（进展审核报告：项目约束、两树状态、模块规模、相对 2026-09-16 架构复审基线的增量、本会话改动明细、缺口清单、给审核方的 6 个问题）；② `docs/plan/2026-09-18-openai-review-source-inventory.md`（**src/ 全量 810 文件 / 457,374 行**逐文件行数清单，含按目录汇总，并标记 **[NEW]**/[MOD]** 变更状态）；③ 源码存档 `E:\Work\WarVK-source-review-20260918.zip`（4.3 MB，SHA-256 `634F7357CD24C5B8842EC0676338117A579276B6F730CB99E3ED97E890FC0F82`，含 `src/` 全量 + `AGENTS.md` + `WAR3_LIFECYCLE.md` + 4 份审核/取证文档）。
- **实测事实（本树）**：分支 `codex/v1.22-release-integration-20260914`，HEAD `ae89054`；工作树 **594 条未提交**（103 modified + 491 untracked + 0 deleted）；已跟踪 diff **+4,792/−3,452 行 / 103 文件**（不含未跟踪新文件）；`src/d3d9/war3/` 下新增源码文件 **63 个**；最大单文件 `src/d3d9/d3d9_device.cpp` **50,106 行**。
- **相对基线的增量结构**（基线 = `2026-09-16-architecture-review-followup.md`）：新增 10 个领域簇（帧取证/帧历史/帧输入/帧时间线、帧记录器、对象级调色板证据、阴影构建生命周期/进度/线程门/几何域、原生光照桥、原生捕获/帧同步所有权、运行时组调色板内核、皮肤调色板选择、太阳光策略、路径阻断证据），并新增 13 个合同测试文件。
- **报告如实记录的关键缺口**：① 对象级 palette 证据链**对无拒绝的健康路径结构性失明**（条目仅由 `NoteReject→Insert` 创建；`NoteServed`/`NoteEnqueued` 在 `Find==nullptr` 时静默 return；解析器要求链以 `Rejected` 开头）⇒ 修复需变更证据 wire 契约，已按纪律挂起待裁定；② **本树全门禁未在本轮复跑**（本轮改动后仅验证受影响门禁：roundtrip 116/116、cost 38/38、runtime 723、recorder memory 87、`ninja -n` no work）——注：**次班独立审核**（本文件另一条目）曾对**改动前**的树复跑全门禁全绿（静态 256/256、meson 84/84、域 46/46、预算门禁 EXIT 0），二者不可混为一谈；③ 两棵并行树、全部工作未提交；④ 本轮诊断插桩仍在树中，冻结前需决定去留。
- **边界**：本轮**未改任何生产代码**（仅新增文档与只读统计）；未部署、未提交、未晋升稳定；现场 DLL 仍为本会话诊断构建（`519AFA69…` 候选备份在位）。
## 2026-09-18 — 次班独立审核：夜间工作全门禁复跑全绿 + 两处文档缺陷修正（验证记录，未提交 / 未部署 / 未晋升稳定）

- **背景**：用户指派次班审核 Deepseek 夜间工作并复跑测试。审核 = 夜间文档链通读（审阅子代理）+ 全门禁独立复跑（测试子代理）+ 硬事实一手抽查（现场 DLL `A0A51AF2…` 未变、备份在位且哈希一致、git 仍 HEAD `ae89054` 无提交、最终 DLL 36,283,128 B / `519AFA69…`、`ninja -n` no work）。
- **门禁独立复跑（全部与夜间声称一致）**：预算门禁 EXIT 0（冻结 157/站点 112/已迁出 46/device.cpp 567）；M1/M2 等价门禁 EXIT 0（动态段实际执行）；全量静态 **256/256**；meson **84/84**；域测试 **46/46**（收官报告写 37 为过时数字，文档链本身为 46）；生命周期 187/187；线程闸门 9/9（C3 在列）；记录器 23/23；成本 PASS(38)；往返 ENCODER 73/ROUNDTRIP 116/CHECKS=1107 CERTIFIED；解析器 94/94；taxonomy 动态 40,215,908 断言；M1 差分当前值恰为 3,098,194。
- **两处文档缺陷已由次班修正**：① 两条 T3 条目曾位于本文件标题之前，已移回标题之后（内容未改）；② 收官报告 §5.7/§5.7.1 对「根读方 55/55 本树不可验证」的判定错误——系检索词假阴性（unittest 输出为 `Ran N tests`），次班亲跑 `test_analyze_frame_evidence.py` 55/55 OK 且夜间日志 :341/:348 本就有 `Ran 94/55 tests` 原始行，已在 summary §5.7.2 更正并撤销相关加注（原文保留）。
- **审核发现待用户裁定项（未动代码）**：R1 Registry domain 隔离两块为交接「只读」范围内的生产代码实施；R2 A9/A10 删除/迁出属先斩后奏（备份链完整可回滚）；R3 M2-4/M2-5/M2-5B 三块迁移超出书面交接范围（证据链本身完整：34 字段差分 30,443,383、每块 ≥2 变异真跑）；R5 两个既有门禁改锚未捕获改前 SHA（断言方向为加严）。详见收官报告复核与 `docs/plan/2026-09-18-overnight-outbound-handoff.md`。
- 本条目为验证与文档修正记录，未改任何生产代码/门禁脚本；全部候选未提交、未部署、未晋升稳定。

## 2026-09-18 — T3 只读审计完成（静态可枚举范围）：闸门执行证据 9/9 与 187/187、(i)(iii) 收口、自审 11/11（纯只读取证；未提交 / 未部署 / 未晋升稳定）

- **新增交付** `docs/plan/2026-09-18-t3-readonly-audit-record.md`（约 17.5 KB）：合并跨约 20 轮的 T3 只读审计（访问面/写面/读面、发布协议、两端归属、线程闸门与执行证据、(i)(iii) 收口、自我纠正、覆盖边界、红线）。
- **线程闸门已在树中（2026-09-17「方案 B 第一部分」）**：`war3_shadow_renderer_core.cpp:9568-9579` 在**推进边界**以 `consumePermissionGranted(false)` 拒绝非所有者 ⇒ 控制面 drain 分离线程只保留请求语义、不推进分块；所有者＝`hooks::GetMainLoopThreadId()`；两处推进入口共用**唯一一份**判定（`war3_shadow_build_lifecycle.h`），委托纯函数 `DecideShadowBuildAdvance`。
- **执行证据（非仅读码）**：`war3_shadow_build_thread_gate_test.exe` **9/9 PASS**（含 `C3 pipe-thread-drain -> NotOwner`，即 T3 场景本身、`C5 direct-lower-entry -> NotOwner`、`C6 bogus-owner -> NotOwner`）；`war3_shadow_build_lifecycle_test.exe` **187/187 PASS**。
- **(i) 静态可枚举范围内闭合**：关键函数**无取地址**（`&fn` 命中 0/0/0）＝无函数指针间接；**`src/d3d9/d3d9.def` 不含这些内部函数**＝无导出面；直接调用点已枚举（读端 `war3_live_palette_selection.cpp:512`；枢纽 `War3TryBuildLiveRuntimeGroupPalette` 5 处全在 `d3d9_device.cpp`）。
- **(iii) 在 T3 路径三模块内收口**：生产者 `war3_model_hook.cpp` 仅 **2 个**可变共享非原子静态量（其一即 T3 对象，另一兄弟缓存**已有原子发布字**先例）；读侧 `war3_live_palette_selection.cpp` **0 个**；drain 宿主 `war3_shadow_renderer_core.cpp` 共享可变状态**全为原子或 `thread_local`**。
- **仍未闭合**：(ii) 实机运行期印证（可观测量＝三类拒绝计数；需实机）；类成员/堆共享状态与全仓调用图**未覆盖**。**不声称 T3 闭合、不声称运行时安全。**
- **诚信留痕**：本夜自我纠正 **6 次**；其中 3 次与 T3 直接相关（"读方无法排除撕裂"的泛化说法、"`Select-String` 双层通配符"假阴性、"data_collection 清单是运行期消费者"的错误推断）。审计文档自身行号**自审 11/11 通过**。
- 边界：全部为**静态只读 + 宿主测试执行**；**未改任何源码**；树保持冻结（`ninja -C build32 -n` no work）；现场 DLL `A0A51AF2…` 未替换。

## 2026-09-18 — T3 复审结论更正：线程修复「方案 B 第一部分」已于 2026-09-17 落地（纯只读取证；未提交 / 未部署 / 未晋升稳定）

- 只读读码发现 `war3_shadow_renderer_core.cpp:9568-9579`：控制面 drain 在**命名管道分离线程**调用 `ensureLatestFrameBuilt()`，但在**推进边界**被所有者许可门 `consumePermissionGranted(false)` 拦截 ⇒ 非所有者只保留请求语义、**不推进任何分块**；该门同时覆盖 `allowControlPlaneSemanticDrain` 与 `IsHotSemanticBuildWaitPayload`，所有者取自 hook 观测到的主循环线程。
- 影响：`docs/plan/2026-09-18-t2-t3-review-verdict.md` 第 3 节对 T3 的判断前提**已被更正**（新增 §6）。T3 性质由"待修复 UB"修订为"**已有结构性修复（2026-09-17），缺运行时印证与其它路径枚举**"；T3 剩余面收窄为 (i) 其它非所有者路径枚举、(ii) 实机许可门印证、(iii) 同类门覆盖范围。
- 同时完成 T3 只读审计链：palette 槽位缓存访问面 **9 处/单一 TU**；写面 **3 处确证**（重置 / `CaptureBlendedPaletteSlotRange` 逐帧写 / 跨地图失效）；读侧协议＝`valid` + `frameTag` + 逐条目 `writeSerial` 严格递增；全局序列号 `s_slotBlendedPaletteWriteSerial` **只写不读**（并更正了我此前一处泛化表述）；读点位于 `War3TryBuildLiveRuntimeGroupPalette`——该函数同时是 M2-5 B1/B2 的前置阻塞点（**两件事应合并规划**）；其生产调用点**恰 5 个、全在 `d3d9_device.cpp`**，其中 `:20418` 属 submit 路径（默认每帧）；承载 drain 的 `war3_shadow_renderer_core.cpp` 对选择模块命中 **0**（直接调用层面）。
- 边界：全部为**静态只读**结论，**无**运行时证据；**不声称** T3 已闭合、**不声称**运行时安全；**未改任何源码**；树保持冻结（`ninja -C build32 -n` no work）；现场 DLL `A0A51AF2…` 未替换。

## 2026-09-18 — ⑤ 第二块证据补强：三条未真跑变异各真跑一次 + 最终树全门禁复绿（未提交 / 未部署 / 未晋升稳定）

- **范围**：只补上一块（`docs/plan/2026-09-18-registry-domain-count-export-record.md` §7 第 5 条）自报「未真跑、只由静态门禁的
  所属函数/恒等映射断言覆盖」的三条变异；**不保留任何语义改动**，四个被变异源文件与实现态备份逐字节相同
  （`RESIDUE_MISMATCHES=0`），不加 env、不 git 写、不部署、不启动游戏、不称稳定版。
- **三条变异（每条：改 → Below Normal/`-j2` 真实构建 → 门禁必须红 → 还原 SHA MATCH → touch 后真实重编译 → 复绿）**：
  ① 删 `d3d9_device.cpp` 的 `diagnostics.domainPublishRejects++;`（`War3CreateShadowPersistentGeometryAfterMiss`）：
  变异态 2,257,210 B / `03A88546E940DF2E881795A759D52675C71C08BE9D7167E1CDFD22D3AD613AF5`，NINJA_EXIT=0（`[2/2] Linking`，
  变异态 DLL `4CBE8494…`）；静态出口门禁 FAILED(2) `0 != 1 : device.cpp 中 domainPublishRejects++ 的处数不是冻结值 1`、
  `{} != {'War3CreateShadowPersistentGeometryAfterMiss': 1} : domainPublishRejects 的累加点所属函数与冻结口径不符`，
  定向 FAILED(1) `device.cpp 的累加站点/所属函数与冻结口径不符`，域隔离静态门禁也红(2)。
  ② 删 bridge 行 `stats.domainGcEraseRejects = completed.domainGcEraseRejects;`：变异态 2,257,185 B / `5173E985…`，
  NINJA_EXIT=0（变异态 DLL `5A5688EC…`）；静态出口门禁 FAILED(1) `bridge 传递缺失或重复：
  stats.domainGcEraseRejects = completed.domainGcEraseRejects;`，定向 FAILED(3) `bridge 未把 domainGcEraseRejects 恒等传出：None`；
  **域隔离静态门禁保持绿**（两条门禁覆盖边界不同，不是重复断言）。
  ③ per-frame 串线（`m_currentFrameWorkload.domainPublishRejects = stats.domainLookupRejects;`）：变异态 625,321 B / `611F32C9…`，
  NINJA_EXIT=0（变异态 DLL `4FC34E81…`）；静态门禁 FAILED(1) `0 != 1 : domainPublishRejects`，定向 FAILED(3)
  `per-frame 未把 domainPublishRejects 恒等传出：domainLookupRejects`。三条还原后 SHA MATCH 并各自 touch 后真实重编译 + 复绿
  （14 OK / 5 OK / 13 OK / `ninja -n` no work）。
- **结论**：三条都**可编译可链接**（NINJA_EXIT=0、本树 DLL 确实被重链），只有出口静态门禁 + 定向测试能咬住（符合 m2-5 §2.3 失败模式 5）
  ⇒ 上一块 §7 第 5 条诚实缺口关闭。
- **最终树复核**（touch 4 源 + 真实重编译 129 s，NINJA_EXIT=0）：`ninja -n` no work（前/后/最终）；预算门禁 EXIT=0（冻结 157；
  站点 165→112；已迁出 46；device.cpp 567）；M1/M2/M2-3/M2-5/M2-4/M2-5B/A9 七条等价门禁 EXIT=0；meson **84/84**；全量静态
  **256/256**；宿主域测试 **46/46**；新出口门禁 **14 OK** + 定向 **5 OK**；域隔离 13 OK / 域离线模型 5 OK；stage13 4 OK /
  bridge-ramp 22 OK / tombstone 8 OK / expiry 2 OK；记录器 **23/23**；成本 **PASS(38/38)**；生命周期 **187/187**；解析器 94 OK；
  根读方 55 OK；往返 `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（`CHECKS=1107 FAILURES=0`）。
  最终 DLL **36,283,128 B / `519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306`**（06:37:39）。
- **DLL 身份说明（不得误读）**：字节数与上一块的 36,283,128 B 相同但 SHA 不同，是**构建不可逐字节复现**，不是语义差异：
  PE 头 `TimeDateStamp` 等于链接时刻（实测 `1789684659`），且 `war3_perf_monitor.cpp:3839` 使用
  `__DATE__/__TIME__` 构建戳；一致性由「四个源 vs 实现态备份逐字节相同 + `ninja -n` no work + 全门禁绿」证明。
- **一次无效尝试（如实登记）**：第 1 次 E1 的构建段无效 —— pwsh 后台作业默认工作目录是会话工作区
  `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk`（另一棵 dxvk 树），PowerShell 给原生命令子进程用的是
  自身 location，原 wrapper 的相对 `-C build32` 把该次 50.5 s 编译/链接落在**那棵树的 build32**；已改为绝对 build 目录的
  wrapper 重跑三条并以重跑为权威证据。副作用：那棵树只被重编/重链了它自己的 DLL，其源码未被本 agent 触碰。
- **仍未覆盖（不因本次补强消失）**：出口代码**仍未被可执行级测试覆盖**（`War3PerfMonitor` 非宿主机可链接目标）⇒
  「实机 JSON 里确有这些键且数值合理」仍未验证；F3 全长仍未执行；D4/D5 分账与 D1 key 级 domain 未动。**不得**引用为
  Registry domain 隔离完成、Stage13 就绪或稳定版。未部署、未 git 写、未启动游戏、未触碰 `E:\Work`。
  证据：`docs/plan/2026-09-18-registry-domain-count-export-extra-mutations.log` + 记录文档 §10。
## 2026-09-18 夜间窗口收尾 checkpoint — 冻结状态与验证归属（未提交 / 未部署 / 未晋升稳定）

- **冻结树（主线程实跑）**：ninja -n no work；预算门禁 exit 0（冻结 157 / 站点 165→112 / 已迁出 46 / device.cpp 567）；全量静态 **256/256**；宿主域测试 **46/46**；生命周期 **187/187**；meson **84/84**；往返 **CHECKS=1107 FAILURES=0 CERTIFIED(A–F)**；记录器 **23/23**；成本 **PASS(38/38)**；帧证据运行时 PASS。DLL **36,283,128 B / 519AFA6904F47E9C2E168FF0E5E0A17A470B27F9F9E714783D99C0333E397306**。
- **现场 DLL 未替换**：`E:\Work\Warcraft III\d3d9.dll` = 36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`；无残留游戏进程。
- **本夜已交付并被主线程验收**：M2-3（③）、M2-5 前置、M2-4、M2-5 余项（B4 迁出 + B1/B2/B5/B11 逐条不可做裁定）、A9/A10 裁定落地、⑤ Registry domain 隔离（显式 domain + 8 处 fail-closed owner-check）、⑤ 域拒绝计数对外出口、出口块三条变异补强。
- **未覆盖（不得过度宣称）**：① T1 实机对象级取证与 ② T8 双图基线**未完成** —— 三种 `-loadfile` 形式均进不了图（未进图即无控制面），AutoTest 沙箱 `preflight_instance_pool` 实测 `ok=false`（缺游戏安装）；Registry domain 隔离整体仍未完成（D1/D3/D4/D5/D7 未动、F3 未执行、实机 JSON 键未验证）；解析器 94/94 与根读方 55/55 仅子代理证据。
- **两条固化教训**：① 后台作业里跑 ninja 必须用**绝对 build 目录或显式 workdir**（本夜两次 CWD 事故，其一误编另一棵树）；② 本树 `war3_perf_monitor.cpp` 用 `__DATE__/__TIME__` 构建戳 ⇒ **同源码重链接不逐字节复现**，DLL 身份必须登记「最终树最后一次真实构建」。
- 证据与台账：`docs/plan/2026-09-18-overnight-summary.md`、`2026-09-18-overnight-progress-log.md`、各任务 `*-record.md` / `*-full-gate*.log` / `*-mutation-raw-output*.log`。

## 2026-09-18 — ⑤ 第二块：Registry domain 拒绝计数对外出口（观测接线；未提交 / 未部署 / 未晋升稳定）

- **范围**：只补 `docs/plan/2026-09-18-registry-domain-isolation-record.md` §8-2/§9-1 点名的观测缺口 ——
  6 个 domain 拒绝计数（`rejectDomainConflict`/`domainLookupRejects`/`domainPublishRejects`/
  `domainGcEraseRejects`/`domainResetPurgeRejects`/`domainResetOwnerRejects`）接到**既有** persistent-geometry
  拒绝计数族的出口链（M2-5 清册 §2.2 第 6 跳），不新创通道、**不改任何准入/发布/淘汰/退役语义、不加 env**。
- **出口链**：`d3d9_device.cpp:33671-33676` Present 区间恒等拷进 `War3PerfMonitor::PersistentGeometryFrameStats`；
  `war3_perf_monitor.h` 三处结构各 6 字段（FrameWorkloadSnapshot :214-219 / PersistentGeometryFrameStats
  :317-322 / ShadowBudgetAggregate :1274-1279，聚合用 `persistent*` 前缀）；`war3_perf_monitor.cpp` per-frame 拷贝
  :2439-2446 + 区间累加 :2510-2515；两个 JSON 写手各 1 键 1 值（shadowBudgetSummary :7527-7538、
  shadowRuntimeV2Summary :9060-9071）；`workloadSeriesColumns`/`workloadSeries` :9819-9822 / :9941-9943。
- **口径**：只有 `rejectDomainConflict`（legacy `persistentRejectCreateOrBudget` 的 ShadowCapture 失败分桶）进入
  `persistentRejectCreateOrBudgetDetailedTotal`；另外 5 个「越权触碰被拒绝」计数不进入该求和。
- **出口位置如实说明**：`runtime_status.json`（diagnostics hub）**不含本族任何字段**（全文 0 命中 `persistentReject*`）；
  既有等价出口是 perf monitor 的 `war3_perf_report.html` 内嵌 JSON —— 本块接既有等价出口，不新开通道。
- **测试**：新增 `AutoTest/test_registry_domain_count_export_static.py`（**14 tests OK**，逐字段钉死「允许处数」：
  device.h 定义 1 / device.cpp 重复定义 0 / 累加点按路径（lookup、purge 各 2，其余 1）/ bridge 1 / perf_monitor.h
  三处各 1 / per-frame 与 agg 各 1 / 两个写手各 1 键 1 值 / series 列与行各 1 / legacy 求和只含 `rejectDomainConflict`
  / 不加 env）；新增 `AutoTest/test_registry_domain_count_export.py`（**5 tests OK**，7 场景 × 2 Present 区间增量
  模型 + 恒等映射 + 稳态为 0）；宿主机域模型测试 **41 → 46/46**（补 reset 归属校验路径）。
- **变异真跑 2 条**（还原 SHA MATCH + touch 真实重编译 + 复绿）：①删掉 `agg.persistentDomainLookupRejects +=`
  ⇒ NINJA_EXIT=0（能编译能链接）但静态门禁 `0 != 1 : domainLookupRejects`、定向测试 `没有区间累加`；
  ②只在 shadowBudgetSummary 改 JSON 键名 ⇒ NINJA_EXIT=0 但静态门禁「缺 persistentDomainGcEraseRejects 的 JSON 键」、
  定向测试 `JSON 键数 != 1`。
- **全门禁（最终树）**：ninja -n no work（前/后）；真实重编译 27 步 exit 0；预算门禁 exit 0（冻结 157；站点
  165→112→112；已迁出 46；device.cpp 567）；M1/M2/M2-3/M2-5/M2-4/M2-5B/A9 七条等价门禁 exit 0；meson **84/84**；
  全量静态 **256/256**（255+新增）；域门禁 13 OK；域离线模型 5 OK；宿主域测试 46/46；记录器 23/23；成本 PASS(38/38)；
  生命周期 187/187；解析器 94/94；根读方 55/55；往返 116/0 `CERTIFIED_SPEC_SATISFIED`。DLL **36,283,128 B /
  `295C7FD25CB5549664963AB01A252BF423AD1791D9C004B9372BAB5B60CEFB1A`**（+4,096 B vs 上一块 36,279,032 B / 6D17B038…）。
- **未做到 / 不得过度宣称**：出口代码**未被执行级测试覆盖**（War3PerfMonitor 非宿主机可链接目标）⇒ 实机 JSON 里确有
  这些键仍未验证；**F3 仍全部未执行**；D4/D5（域维度字节/条目的分账）与 D1（key 级 domain）未动 ⇒ 没有可出口的分账
  字段，本块未伪造。**不得**引用为 Registry domain 隔离完成、Stage13 就绪或稳定版。未部署、未 git 写、未启动游戏、
  未触碰 `E:\Work`。证据：`docs/plan/2026-09-18-registry-domain-count-export-record.md` + `…-full-gate.log` +
  `…-mutation-raw-output.log`。
## 2026-09-18 — ⑤ 第一块：Registry domain 隔离（显式 domain 类型 + owner-check；未提交 / 未部署 / 未晋升稳定）

- **范围**：T7 勘察「审计批次 4（U5）先行」中的 domain 隔离部分（`docs/plan/2026-09-18-t7-registry-stage13-survey.md`）。
  主线程裁定逐条落实：显式字段形态 (a)，取值沿用既有隐式 tag（S1 = 0x53310001、Stage13 = 0x53314301），
  publish/GC/reset 加 owner-check，跨 domain 命中 fail-closed；**不改准入/发布语义、不加 env、不改 Stage13 默认值**。
- **新增生产头** `src/d3d9/war3/shadow/war3_shadow_geometry_domain.h`（4,029 B /
  `215443C92054A97534E6B2FDE20B8791AAB035D4748B141E6FE1BAE7F9C8161E`）：`enum class ShadowGeometryDomain`
  {Generic 0u / S1Terrain 0x53310001u / Stage13Exact 0x53314301u}、`ShadowGeometryDomainTag`、
  `DecideShadowGeometryOwner`/`ShadowGeometryOwnerAccepts`（inline 纯判定，生产与宿主机测试共用）。
  registry 条目 / 常驻条目 / Stage13 常驻条目各加显式 `domain` 字段；三个 registry 入口新增 domain 形参，
  8 个调用点全部携带（4 lookup + 1 find-or-create + 3 create）。
- **owner-check 8 个落点**：lookup 命中（跨域拒绝且**不触碰**槽位）、lookup 的常驻条目域复核、publish
  （位于任何 GPU 分配与 GC 之前，新增失败值 `War3ShadowPersistentCreateFailure::DomainConflict`）、
  GC 过期队列 + 预算回收（`eraseOwnedRegistrySlot`：只在槽位仍属于被淘汰 geometry 时擦除，字节账与
  `bytesEvicted` 语义不变）、reset 的 `std::move` 前归属校验、两处 Stage13 domain 作用域 clear。
  新增 6 个拒绝计数（`domainLookupRejects`/`domainPublishRejects`/`rejectDomainConflict`/
  `domainGcEraseRejects`/`domainResetPurgeRejects`/`domainResetOwnerRejects`）。
- **既有 key 材料逐位不变**：两个 tag 常数从设备源代码中消失（只留在 domain 头），宿主机测试用**生产同一份
  FNV-1a** 对 S1 layoutHash 链与 Stage13 sourceHash/layoutHash 链做 golden 相等断言。
- **测试**：新增宿主机边界测试 `war3_shadow_geometry_domain_test` **41/41**（meson target
  `war3_shadow_geometry_domain`）、静态门禁 `AutoTest/test_registry_domain_isolation_static.py` **13 tests OK**、
  离线域生命周期模型 `AutoTest/test_registry_domain_model.py` **5 tests OK**（12,000 帧随机等价 + 每帧跨域探测
  + F4.10 反例：legacy 形状跨域命中 2 次 vs 隔离后 0 次）。
- **变异真跑 2 条**（还原 SHA MATCH + touch 真实重编译 + 复绿）：①取消跨域校验 ⇒ NINJA_EXIT=0 但宿主
  16/41 FAIL、门禁 EXIT 1；②两 domain 取值相同 ⇒ 编译器 `duplicate case value`（NINJA_EXIT=1），
  补充 2B（去掉编译期防线）⇒ 宿主 22/41 FAIL、门禁 EXIT 1。
- **全门禁（最终树）**：ninja -n no work；真实重编译 exit 0；预算门禁 exit 0（冻结 157；站点 165→112→112；
  已迁出 46；device.cpp 567）；M1/M2/M2-3/M2-5/M2-4/M2-5B/A9 七条等价门禁 exit 0；meson **84/84**；
  全量静态 **255/255**；记录器 23/23；成本 PASS(38/38)；生命周期 187/187；解析器 94/94；根读方 55/55；
  往返 CHECKS=1107 `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`。DLL **36,279,032 B /
  `6D17B0382DEC628A1DF9A672BE0B207C1316F9DE0BB1C86A8C554FF0964DAB20`**（+403 B vs 同树日志的 pre-change
  36,278,629 B / 824AA63B…）。
- **两处既有门禁锚点更新**：`AutoTest/test_bridge_ramp_shadow_safety_static.py` 与
  `AutoTest/test_shadow_lifecycle_tombstone_static.py` 从"钉死裸常数/无条件 clear"改为"钉死显式 domain
  取值/domain 作用域清理"，护栏强度不变（取值由 domain 头 + 新增门禁双重钉死）；其 pre-change SHA
  **未捕获**，已在记录 §3.2 如实登记反向替换式。
- **明确未达成（不得过度宣称）**：F3 全部未执行（本树无 pre-U5 基线快照；未启动游戏、未部署）；
  域维度计数尚未接 `War3PerfMonitor` 对外输出；D4/D5（字节账与过期队列分账）未动、D1（key 级隔离，
  形态 (b)）未采纳、D3/D7 未动。**不得称 Registry domain 隔离已完成、Stage13 已就绪或 palette 侧职责已迁完。**
- 证据：`docs/plan/2026-09-18-registry-domain-isolation-record.md`（含 D1–D8 / F1–F4 进展表与未验证项）、
  `docs/plan/2026-09-18-registry-domain-isolation-mutation-raw-output.log`、
  `docs/plan/2026-09-18-registry-domain-isolation-full-gate.log`。未部署、未 git 写、不触碰 E:\Work、
  不启动游戏、未晋升稳定版。

## 2026-09-18 — A9/A10 死代码裁定落地：A10 未实例化模板删除 + A9「model-local → world-space 组合」逐字节迁出（未提交 / 未部署 / 未晋升稳定）

- **两次独立改动**（各自 provenance / 快照 / 门禁 / 变异）：
  A10 War3SemanticVectorStorageReadable（function template，全 src 0 实例化）**删除**：
  device.cpp 2,249,192 → 2,248,823 B（-369 = 11 行模板块；AC5FB37E… → FB4D2FF6…）；
  删除仅限该模板本身，反向证明 = 变异把同一 block 原样插回后**逐字节复现 pre-A10 SHA**。
  A9 War3SemanticBuildWorldPaletteIfNeeded（[[maybe_unused]]、0 调用，pre-A9 :7161-7175）
  **迁出**到既有调色板模块 war3_live_palette_selection.{h,cpp}：正文**逐字节 321 B**
  置于 A9 BEGIN/END 标记之间；device.cpp 2,248,823 → 2,248,277 B（-546 = 15 行），
  模块 .cpp 48,248 → 49,791 B、.h 12,358 → 13,286 B；唯一机械差异 = 签名去 [[maybe_unused]]，
  正文 0 token 改写；device.cpp 侧无锚注释（A9 无调用点，不做"仍在用"的假象）。
- **legacy 参考 + 双向 fail-closed**：新 war3_live_palette_selection_a9_legacy_reference.inc
  = 3,026 B / D92048FCB8B293C2A738051043C918240BBCD5759459FA029744D8FADE3FD66F
  （生成器新增 --a9；前六份 .inc 与 SHA 一字未动）；模块正文 == .inc 正文 == pre-A9 快照
  独立重抽正文（生成器与独立门禁各自复算）。.inc 必须 include 在 M2-4 .inc 之后 ⇒ 正文内
  无限定的 A4 调用解析到 M2-4 legacy 副本（覆盖调用链的 A4 合同语义），门禁检查该顺序。
- **宿主差分 N/N**：A9 电池 **947,458** 条断言（236,864 调用 x 4 + 2 段末 memcmp；9 palette variant
  x 2 可读性 x 8 world x 256 kind + 200,000 随机），四矩阵全 passed==total、0 DIFF；
  --probe-a9 **17 场景 x 2 env** 与门禁内**独立实现**的 A4/A9 语义推导逐行相等
  （module == legacy == 独立期望；含 256 元素输出与 0 期望两态）。
- **FROZEN_A9 = {War3SemanticBuildWorldPaletteIfNeeded: (0, 0)}**：baseline 是预算门禁**同一个**
  DEF_RE 在 pre-A9 快照上的实测值 0（[[maybe_unused]] 前缀不占行首站点；名字不匹配
  SEMANTIC_FAMILY_RE ⇒ 判定 3 对此盲）。A10 **不登记** FROZEN：删除会让判定 2 把它读成
  "没落地的删除/改名"，其捕手是 M2-4 门禁的文本缺席断言（DEAD_CODE_ABSENT_FROM_DEVICE）。
- **变异真跑（各 >=1 条）**：A10 把模板重新加回 device.cpp ⇒ **NINJA_EXIT=0 可编译**、
  M2-4 门禁 EXIT 1 并点名 "device.cpp 里的 A10 未实例化模板仍然存在…"、预算门禁仍 EXIT 0
  （该模板的删除/回流只能由 M2-4 门禁咬住）；还原 SHA MATCH + touch 真实重编译后 EXIT 0。
  A9 模块正文 (worldTransform * localMatrix) → (localMatrix) ⇒ 可编译、差分
  A9 battery 947458 checks / **26,672 failures**（首条 A9.hash module=14971461764820984933
  legacy=6770109457156564069）、A9 门禁 EXIT 1（正文与 .inc 逐字节不一致）、预算门禁仍 EXIT 0；
  还原 SHA MATCH（D019FD7DD68CC46A5C3D68AE72270713FCC5B7693A058A8224BBBDEA53F47225）+
  touch 真实重编译后复绿。
- **全门禁（最终树）**：ninja -n no work、真实重建 exit 0（[1/6]–[6/6]）、预算门禁 exit 0（冻结 **157**；
  行首站点 迁移前 165 → 上限 112 → 当前 112；已迁出 46；device.cpp **567**，A10 删除后 568→567）、
  M1/M2/M2-3/M2-5/M2-4/M2-5B/**A9（新增门禁）** exit 0、meson **83/83**、全量静态 **254/254**（253→254）、
  记录器 **23/23**、成本 **PASS(38/38)**、生命周期 **187/187**、解析器静态 **94/94**、根读方 **55/55**、
  往返 **CHECKS=1107 / CERTIFIED**。M2-3 motion-on 与 M2-5 diagnostics-on 的**全文件累计**差分数由
  39,268,450 变为 **40,215,908**（下限仍满足，门禁未改，解读口径按此更新）。
- DLL **36,278,629 B** / 824AA63B5C151831F19F7599BF89ABFFE92967564A38236B95056009C01D1887
  （最终树最后一次真实构建；对照 M2-5B 的 36,278,340 B，+289 B；同源码同尺寸不同 SHA 再次实测）。
- **未覆盖（如实）**：A9 在生产代码里**仍然 0 调用**（迁移 = 可审计的死代码，不等于被使用）；
  宿主 IsReadableRange 是替身；不覆盖 GPU/实机/性能；未部署、未 git 写、未触碰 E:\Work；
  M2-5 表 B 的 B1/B2/B5/B6-B11 与 A8 bounds 族调用点与本轮无关。**不得称 palette 侧职责已迁完或稳定版。**
- 记录：docs/plan/2026-09-18-a9-a10-dead-code-ruling-record.md +
  docs/plan/2026-09-18-a9-a10-full-gate-rerun.log。
## 2026-09-18 — A9/A10 死代码裁定落地（A10 删除 / A9 迁出）+ 主线程独立验收通过（未提交 / 未部署 / 未晋升稳定）

- 裁定与落地：A10 War3SemanticVectorStorageReadable（模板、0 实例化）**删除**（device.cpp -369 B；捕手 = M2-4 门禁文本缺席断言；不登记 FROZEN）；A9 War3SemanticBuildWorldPaletteIfNeeded **迁出到调色板模块**（device.cpp -546 B；正文 0 token 改写；新增 FROZEN_A9 baseline=0；legacy .inc 3,026 B / D92048FC…；宿主差分 947,458 断言 0 DIFF；--probe-a9 17/17×2）。
- 主线程独立验收（实跑）：ninja -n no work；预算门禁 EXIT 0（冻结 157、站点 165→112→112、已迁出 46、device.cpp 567）；六条等价门禁全 EXIT 0；全量静态 254/254；DLL 36,278,629 B / 824AA63B5C151831F19F7599BF89ABFFE92967564A38236B95056009C01D1887。变异原始输出标注为子代理证据。
- 重要点名：A10 与 A9 的变异都表明**预算门禁对这两类改动无能力**（A10 模板与 A9 值级改动），真正咬住它们的是 M2-4 门禁文本断言与宿主差分——这正是分层门禁的意义。
- 点名：**A9 在生产代码里仍然 0 调用**（迁移 = 可审计的死代码，不等于被使用）；M2-3/M2-5 门禁打印数为全文件累计（39,268,450 → 40,215,908），下限仍满足、门禁未改但记录解读需更新。
- 剩余：M2-5 表 B 的 B1/B2/B5/B11/B6-B10 与 A8 bounds 调用点（各有前置）；Registry domain 隔离→Stage13 已勘察未实施；T1/T8 待能进图前置。**不得称 palette 侧职责已迁完 / 稳定版。**

## 2026-09-18 — M2-5 余项：表 B 逐条可行性裁定 + B4「每帧 submitted skinned palette 聚合」逐字节迁出（未提交 / 未部署 / 未晋升稳定）

- **先裁定、后动手（只做可做的那一条）**：表 B 逐条判定 ——
  **B1/B2 不可做**：块内嵌 d3d9_device.cpp 私有的 inline 计时助手 War3SemanticSubmitScope（:2732-2739，
  src 下任何头文件都没有它的声明/定义：0 命中），它又读进程单例 war3::War3PerfMonitor::instance()（:2738）
  与 device.cpp 私有 fast gate（:1253）⇒ 整块逐字节外移不可能（除非改块外结构）且其效果是**无法注入的墙钟时序**；
  块还写 :20183-:20187 与 :20718-:20724 的 9 个局部量、结果被同一 2,000+ 行 device 函数 :20821-:20829 与
  :22025-:22026 消费 ⇒ 强依赖 device 状态；核心调用 War3TryBuildLiveRuntimeGroupPalette 内部 6 处读全局
  PoseRegistry::instance()（模块 .cpp :331/:540/:578/:580/:773/:791）。
  **B5 不可做**：churn 块要的类型 War3SemanticDirectCasterContractMap/Key/KeyHash/State **嵌套在
  class D3D9DeviceEx 内**（d3d9_device.h:2460/:2477/:2486/:2496，类 :387-:3194），模块无法命名它。
  **B6-B10 非独立迁移项**：定义早在 M2-1/M2-2/M2-3 已迁，剩下的是调用点，归属其外层编排
  （B6/B7 在 B1 块与 leased-packet 路径、B8 在 M4/C3、B9 在 append 主函数、B10 在 packet 构建/canonical 路径）。
  **B11 不可做 / 归类待裁定**：compose-policy 判定（A4/A5，已迁）与 device 帧缓存 + 资源原语（C2）混合。
  逐条行号与代码引用见记录文档 §1.2。**未硬做任何一条，未降低证据标准。**
- **唯一迁出（B4）**：pre-M2-5B device.cpp :22200-:22244 的 45 行 → **新模块**
  src/d3d9/war3/semantic/war3_palette_submitted_aggregation.{h,cpp}（2,124 / 3,479 B）的纯函数
  War3AggregateSubmittedSkinnedPalette(War3ShadowCaptureStats& st, bool skinned)。正文**零行删除、零 token 改写**
  （块内 st 引用成为第一个形参，名字逐字相同）。
  device.cpp **2,250,786 → 2,249,192 B（-1,594 = -2,464 块 + 531 锚注释 + 63 include + 276 说明行）**，
  可由锚注释反向**逐字节重建** pre 快照（migrate --reconstruct-check: reconstruct == pre snapshot True）。
  新模块刻意不并入既有 B3 模块，避免重钉 M2-5 门禁已登记的模块 .cpp/.h SHA。
- **provenance（fail-closed）**：pre-M2-5B 工作树 = 2,250,786 B /
  D0E80399703CBF3B7567BAD138782B50FF6093705835F2392A932F9543554811，捕获 2026-09-18T04:15:05；
  **逐字节等于 M2-4 记录的「迁移后 device.cpp」**（交叉闭合）；备份 %TEMP%\device_pre_m2_5b.cpp。
- **FROZEN_M2_5B** = {War3AggregateSubmittedSkinnedPalette: (0, 0)}：baseline 是**实测**（预算门禁同一 DEF_RE
  与同一第 0 列规则扫 pre-M2-5B 快照：全文 0 次出现、0 个行首站点；45 行全在缩进层）。该函数名**不匹配**
  SEMANTIC_FAMILY_RE（判定 3 盲），不登记就无法捕到回流。
- **legacy 参考** war3_palette_submitted_aggregation_legacy_reference.inc = 4,952 B /
  C6D32B4EF8DCCC27A50EE407037687F190CD3D72FC90FE053EB4A49D7E3EE46F（生成器新增 --m2-5b；
  **六份既有 .inc 一字未动**：本轮用 --m2-4/--m2-5 重跑分别复现 E9CB8931…/9397 B 与 A5333AC7…/22511 B）。
  双向 fail-closed：模块 .cpp BEGIN/END 之间正文与 .inc 正文**逐字节相同 2,421 B**。
- **宿主差分（口径 1）**：7 个 stats 字段逐调用整数精确相等 + **每调用整结构 memcmp**；系统网格 18,432 用例
  + 定长序列 44 步 + 定种子随机 150,000 步（跨步保持滚动状态）⇒ **M2-5B 电池 1,347,808 条断言**；
  四矩阵全文件累计 default **39,268,451/39,268,451**、palette-diag 39,268,450、contract 35,780,339、
  contract-diag 35,780,338（全部 EXIT=0）。**--probe-m2-5b** 12 场景 × 2 组 env，与门禁内**独立实现的 FNV-1a**
  推导逐行一致（不是与 legacy 互相印证）。
- **变异真跑**：A 模块侧 (curPaletteHash == 0u) → (!= 0u)（**NINJA_EXIT=0 可编译**）⇒ 差分
  38,949,935/39,268,451、**M2-5B battery 1,347,808 checks / 318,516 failures**、stderr 318,516 条 DIFF，
  首条 DIFF line 4054 M25B.ZeroHash: module=0 legacy=1；B device.cpp 插入可编译回流
  （device.cpp 2,251,841 B / 37F8BDA3…）⇒ 预算门禁点名 **War3AggregateSubmittedSkinnedPalette:
  device.cpp=1 > budget=0**（独立 M2-5B 门禁同时红）。两条还原均 **SHA MATCH**（模块 83A24150…、
  device.cpp AC5FB37E…）+ touch 后真实重编译复绿。
- **全门禁（最终树）**：ninja -n no work、构建 exit 0、预算门禁 exit 0（冻结 **156**；行首站点 迁移前 165 →
  上限 112 → 当前 112；已迁出 **46**；device.cpp 站点 **568**）、M1/M2/M2-3/M2-5/M2-4/**M2-5B（新增门禁）**
  等价门禁 exit 0、meson **83/83**、全量静态 **253/253**（252→253）、记录器 **23/23**、成本 **PASS(38/38)**、
  生命周期 **187/187**、解析器静态 **94/94**、根读方 **55/55**、往返 **CERTIFIED**。
  M2-3/M2-5 门禁打印的全文件累计差分数由 37,920,642 变为 39,268,450（解释口径需按此更新，门禁未改）。
- DLL **36,278,340 B** / 7DA555A781E54C4BFEF320105FA0AF5F301DC54A8FD13F2D97CB08FA92E87216
  （最终树最后一次真实构建；对照 M2-4 的 36,277,956 B，+384 B；同源码同尺寸不同 SHA 再次实测）。
- **A9/A10 未迁未删**，裁定建议：A10 War3SemanticVectorStorageReadable 建议**单独一次改动删除**
  （0 实例化，删除不产生代码差异；须同步改 M2-4 门禁 DEAD_CODE_STILL_IN_DEVICE（第 184 行））；
  A9 War3SemanticBuildWorldPaletteIfNeeded 建议**迁移或显式登记保留**（正文是已被 A4 使用的
  「model-local → world-space 组合」合同参考，可逐字节随迁模块并配 legacy 覆盖）。
- **未覆盖（如实）**：B1/B2/B5/B6-B10/B11 未迁未覆盖；调用点文本是新增文本（实参一致性只能靠审阅+构建+实机门）；
  7 字段出口链由既有静态门禁承担；无 registry 替身需求但 bit::fnv1a_iter 是共享真实模板；本块无 thread_local ⇒ 不做多线程差分；
  env 4 矩阵 + probe 2 组非穷举；不覆盖 GPU / Vulkan / 实机画面 / 性能。**不得**表述为「palette 侧职责已迁完」；
  未部署、未 git 写、未启动游戏、未称稳定版。
- 证据：docs/plan/2026-09-18-m2-5-remainder-record.md+ docs/plan/2026-09-18-m2-5-remainder-full-gate-rerun.log。

## 2026-09-18 — M2-4（调色板 compose-policy 判定 / env getter 余项 ×8 符号迁出）+ 同型等价证据闭合（未提交 / 未部署 / 未晋升稳定）

- **范围**：device.cpp 里清册表 A 的 10 条中可机械迁出的 **8 个符号（9 个定义）逐字节迁入既有模块**
  `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`：A1 `War3SemanticPaletteInPlaceAppendRuntime`、
  A2 `War3SemanticDrawTimePoseRuntime`（两个 env getter，与 M2-1 四个同型）；A3 `War3SemanticPaletteStorageReadable`、
  A6 `War3SemanticTranslationFinite`、A7 `War3SemanticTranslationDistanceSq`（A4 的 supporting 纯判定/纯计算）；
  A4 `War3SemanticPaletteLooksModelLocal`（pointer + vector 两个重载 = compose-policy 判定本体）；
  A5 `War3SemanticHashMatrix4`（已迁 `War3SemanticHashMatrixPalette` 的姊妹）；
  A8 `War3SemanticBoundsRadiusForObjectKind`（A4 依赖的纯 `switch(objectKind)->半径` 查表）。
  device.cpp 2,254,826 → **2,250,786 B**（−4,040；逐符号字节账目闭合，且可由 9 行锚注释**反向逐字节重建** pre-M2-4 快照）；
  模块 .h 9,517 → **12,358 B**、.cpp 41,008 → **48,248 B**。device.cpp 每个迁出点留一行
  `// M2-4: <符号> -> … (byte-identical body)` 锚注释，其余 token 一个字节未动（逐行 diff 只有 9 个 hunk）。
- **唯一机械差异在签名层，不在正文层**：两个 env getter 由 `inline` 定义改为「头文件声明 + .cpp 定义」；A4 pointer 重载的
  `checkReadable = true` 默认实参按 M2-2 先例集中在头文件声明上。函数**正文**在模块 .cpp 的
  `BEGIN/END M2-4 body` 标记之间逐字节保留。
- **A8 依赖裁定（清册 §4-2 的显式选项）**：A8 是纯查表、零 device 依赖，全仓 1 定义 + 7 调用点（仅 1 个属调色板族）。
  留在 device.cpp 匿名命名空间会让模块侧**无法调用**；把半径改成显式形参会改写 A4 正文、破坏逐字节等价。
  因此随 A4 迁入同一模块，**它的 6 个 bounds 调用点文本一个字节未改**，仍经 device.cpp 既有 `using namespace
  dxvk::war3::semantic;` 解析到模块定义。这是工程裁定，不是「bounds 族已迁完」。
- **A9/A10 死代码本轮未迁、未删**（门禁断言钉住「未被擅自删除」）：`War3SemanticBuildWorldPaletteIfNeeded`（`[[maybe_unused]]`）
  与 `War3SemanticVectorStorageReadable`（未实例化 template）全 `src` 各只出现定义一处。裁定建议：A10 **建议单独一次改动删除**；
  A9 **建议迁移或显式登记为保留参考**（它描述了 A4 已使用的 model-local→world 组合合同）；两者都不夹带进结构性迁移。
- **provenance（fail-closed）**：pre-M2-4 工作树 = 2,254,826 B /
  `7391F3071F7D2CF32583C57FF1D914FC59AA765A5EDD5749F5D3A8FCBD8A0844`，捕获 2026-09-18T03:34:55；
  **逐字节等于 M2-5 记录的「迁移后 device.cpp」**（交叉闭合）；备份 `%TEMP%\device_pre_m2_4.cpp`。同为工作树文件哈希而非 git blob。
- **盲区修补（判定 3 盲）**：A3/A4/A5/A6/A7 **都不匹配**预算门禁 `SEMANTIC_FAMILY_RE`；新增 `FROZEN_M2_4`（**独立成表再 update**），
  8 条 budget 全 0，baseline 为**实测**（预算门禁**同一** `DEF_RE` 与第 0 列规则，门禁内从预算门禁源码 eval 出同一 regex，扫 pre-M2-4 快照）
  = 1/1/1/1/1/**2**（A4 两重载）/1/1；与迁移**同一次改动**落地。原始 FROZEN 块里 A8 仍保留 (1, 1)，M1 等价门禁不受影响。
- **等价证据**：legacy 参考 `war3_live_palette_selection_m2_4_legacy_reference.inc` = 9,397 B /
  `E9CB8931B427CB87172BCC96B03497103116DC30F1C1197D3A00FF218F2F5899`（生成器新增 `--m2-4`；M1/M2-1/M2-2/M2-3/M2-5 五份 .inc 未动）。
  生成器**双向 fail-closed**：模块 .cpp 每个 BEGIN/END 之间的正文必须与该 .inc 正文逐字节相同（实测 9/9 份）；
  独立的新门禁 `AutoTest/test_war3_palette_m2_4_equivalence_static.py` 再**自己抽一次**做同样比对（不依赖生成器自证）。
  差分测试加法式扩展：M2-4 电池 **830,806 次调用 × 8 字段**（7 palette variant × 2 可读性 × 8 world × 11 count × 256 kind × 2 checkReadable
  = 630,784；nullptr 分支 22；定种子随机 200,000），逐调用 8 字段整数/位模式精确相等 + 每调用结构 `memcmp` + 段末累计 `memcmp`，
  ⇒ **7,477,257 条断言**。四矩阵全绿：default **37,920,643/37,920,643**、`PALETTE_DIAGNOSTICS=1` 37,920,642、
  `SKIN_PALETTE_CONTRACT=1` 34,432,531、两者同开 34,432,530（全部 EXIT=0）。
  `--probe-m2-4` 20 场景 × 2 组 env 对**独立期望值表**：hash 由门禁内独立实现的 FNV-1a 64 复算、半径/距离按 float32 位模式手工推导，
  修正了首版把 `ObjectKind` 底层值 2 误当 Unit 的手工推导错误（Unit=1、Building=2）后才写入门禁。
- **变异真跑 2 条**：A 模块侧判定常量漂移（`worldMagSq > 16.0f` → `> 32.0f`，可编译 NINJA_EXIT=0）⇒ 差分 **37,878,640/37,920,643**、
  **42,003 条 DIFF**、首条 `M24.looksPtr: module=0 legacy=1`；同一变异下**预算门禁仍 exit 0**（值级漂移只能靠差分咬住）。
  B device.cpp 插入可编译回流（`namespace warvk_m2_4_reflow_probe { … }`）⇒ 预算门禁点名 `War3SemanticHashMatrix4: device.cpp=1 > budget=0`。
  两条都完成「改→构建→跑→红→还原→touch→真实重编译→复绿」，还原 SHA MATCH（模块 `89D506FE…`、device.cpp `D0E80399…`）。
- **跟随迁移改锚 1 处（断言语义未削弱）**：`AutoTest/test_direct_geoset_owner_handoff_static.py` 原先以 `uint64_t War3SemanticHashMatrix4`
  为扫描区间结尾锚，A5 迁出后即崩；改锚为紧随其后、本轮未动的 M2-1 注释锚，区间边界不变、断言一条未改。
- **全门禁（最终树）**：`ninja -n` no work、构建 exit 0、预算门禁 exit 0（冻结 **155**；站点 迁移前 165 → 上限 112 → 当前 112；已迁出 **46**；device.cpp 站点 **568**）、
  M1/M2/M2-3/M2-5/**M2-4（新增）** 等价门禁 exit 0、meson **83/83**、全量静态 **252/252**、记录器 **23/23**、成本 **PASS(38/38)**、
  生命周期 **187/187**、解析器静态 **94/94**、根读方 **55/55**、往返 **CERTIFIED**。
  DLL **36,277,956 B** / `EE21E637282C56D5FAB3884DD1E7C98301B1ED45D9B928F41A80FF88A8E4ABE7`（最终树最后一次真实构建；同源码另一次重链同为 36,277,956 B、
  SHA `C71D5D16…` ⇒ 「同源码⇒同尺寸」成立、「同源码⇒同 SHA」不成立）。
- **未覆盖（如实）**：M2-5 表 B 全部调用点编排（B1/B2/B4/B5/B6–B11）一个字节未迁、未被本电池覆盖；A8 的 6 个 bounds 调用点只经 using-directive 解析、
  未显式驱动；`IsReadableRange` 在宿主机是替身；env 覆盖 4 矩阵 + probe 2 组而非穷举；无多线程差分；不覆盖 GPU/Vulkan/实机/性能。
  **不得**表述为「palette 侧职责已迁完」；未部署、未 git 写、未触及 `E:\Work`、未称稳定版。
  详见 `docs/plan/2026-09-18-m2-4-migration-equivalence-record.md` 与 `…-m2-4-full-gate-rerun.log`。
## 2026-09-18 — M2-5 前置：taxonomy 发射块抽成可宿主差分的纯函数 + 2 条变异真跑 + 全门禁复跑（未提交 / 未部署 / 未晋升稳定）

- **范围**：device.cpp `:21202-21580`（379 行 / UTF-8 19,359 B）的 skinned palette taxonomy 发射块
  （34 个 `War3ShadowCaptureStats` 字段 + 3 张跨帧 `thread_local` 探针表 8192×3）**逐字节**迁入新模块
  `src/d3d9/war3/semantic/war3_palette_taxonomy_emission.{h,cpp}`（21,020 / 2,964 B）的纯函数
  `War3EmitSemanticPaletteTaxonomy(...)`（11 个显式形参：stats 引用、skinned、来源枚举、current-draw 样本、
  provenance、effective palette 指针与计数、slotIndex、submitted hash、stale-pose 标记、帧号）。
  **唯一机械差异**：删除块首别名 `auto& stats = m_war3Scene.shadowStats;` 改为第一个形参；其余 378 行（含探针表）逐字节相同。
  device.cpp 2,273,048 → **2,254,826 B**（账目闭合 -19,359 + 59 + 285 + 793 = -18,222）。
  device.cpp 只留 1 个调用点 + 1 行 include + 4 行注释；**调用点编排（B1/B2 等）一个字节未迁**（M2-5 余项）。
  模块内**不出现**任何 device 成员 / registry / hook 全局（抽取脚本 fail-closed 扫描 0 命中）；唯一外部调用是既有纯静态哈希
  `VisibleRenderableRegistry::computeShadowManifestPartKey`。
- **provenance（fail-closed）**：pre-M2-5 工作树 = 2,273,048 B / `D8211864549BBD830080C6C7A8BE4FCA222198DF7CAC92C5C096B8583AD1A7FE`，
  捕获 2026-09-18T03:05:30；**逐字节等于 M2-3 记录的「迁移后 device.cpp」**（交叉闭合）；备份 `%TEMP%\device_pre_m2_5.cpp`。
  同为工作树文件哈希而非 git blob。
- **盲区修补**：`War3EmitSemanticPaletteTaxonomy` **不匹配**预算门禁 `SEMANTIC_FAMILY_RE` 语义选择命名族 ⇒ 判定 3（回流）对它盲；
  新增 `FROZEN_M2_5 = {该符号: (0, 0)}`（**独立成表再 update**，避免 M1 等价门禁误判），与迁移**同一次改动**落地。
  baseline=0 为**实测**：用门禁同一 `DEF_RE` + 同一第 0 列规则扫 pre-M2-5 的 `:21202-21580` ⇒ 0 个行首站点；全文亦 0 次出现该名字。
- **等价证据**：legacy 参考 `war3_palette_taxonomy_emission_legacy_reference.inc` = 22,511 B /
  `A5333AC7E7F5A080324F5D83668D394C53F26E7ADEEEC5DA2C76E9BFEE26105E`（生成器新增 `--m2-5`；M2-1/M2-2/M2-3 三份 .inc 未动）。
  生成器本轮**双向 fail-closed**：模块 .cpp BEGIN/END 正文必须与 .inc 正文逐字节相同（实测两侧 18,937 B）。
  差分测试加法式扩展：新电池 **386,008 次调用 × 34 字段**（127,008 系统网格 + 9,000 key 填满/越过 8192 槽 +
  250,000 轮定种子随机 + 定长序列群），逐调用整数精确相等 + 段末整结构 `memcmp`，并显式断言诊断门 OFF 时 34 字段必须全 0。
  四矩阵全绿：default **30,443,384/30,443,384**、`PALETTE_DIAGNOSTICS=1` **30,443,383/30,443,383**、
  `SKIN_PALETTE_CONTRACT=1` 26,955,272、两者同开 26,955,271（全部 EXIT=0）。
  `--probe-taxonomy` 16 场景 × 2 组 env 对**独立期望值表**（覆盖 34 字段全部非零语义；期望值先手工推导，
  实跑对照暴露 2 处手工错误后按源码重新推导修正）。
- **变异真跑 2 条（非声称）**：① 模块侧 hash-churn 条件反转（**可编译**）⇒ 差分 30,116,847/30,443,383、
  stderr 326,536 条 DIFF、EXIT=1；② **可编译回流**（迁出函数逐字节复制回 device.cpp 具名命名空间，DLL 链接成功）⇒
  预算门禁点名 `War3EmitSemanticPaletteTaxonomy: device.cpp=1 > budget=0`（独立 M2-5 门禁亦红）。
  两条均完成「改→构建→跑→红→还原→touch→真实重编译→复跑绿」，模块 `E8C6F74A…` / device.cpp `7391F307…` **逐字节还原**。
  原始输出：`docs/plan/2026-09-18-m2-5-mutation-raw-output.log`。
- **全门禁（最终树）**：`ninja -n` no work、构建 exit 0、预算门禁 exit 0（冻结 **150**；行首站点 迁移前 159 → 上限 115 → 当前 115；
  已迁出 38；device.cpp 站点 577）、M1/M2/M2-3/**M2-5（新增门禁）** exit 0、meson **83/83**、全量静态 **251/251**（250→251）、
  记录器 **23/23**、成本 **PASS(38/38)**、生命周期 **187/187**、解析器静态 **94/94**、根读方 **55/55**、往返 **CERTIFIED**。
  两个既有门禁（`test_active_path_palette_instrumentation_export_static.py`、`test_semantic_palette_diagnostics_hotpath_static.py`）
  按先例**跟随迁移改锚并加严**（该诊断门字符串在模块中恰好 1 处、device.cpp 中 0 处、device.cpp 调用点恰好 1 处）。
  M2-3 门禁打印的 `motion-on 差分` 因统计口径为全文件累计而由 17,314,411 变为 30,443,383（下限 12,000,000 仍满足，非 M2-3 覆盖劣化）。
- **DLL 身份**：`build32/src/d3d9/d3d9.dll` = **36,272,891 B** /
  `34FB05C517B178AFF8D1154DF95624353B4EE6CA84DD3E3959D04E1709512117`（vs M2-3 的 36,271,207 B / `0090B354…`，+1,684 B）。
  **实测构建可复现性**：同源码两次链接**同尺寸**、仅 **6 字节**不同（PE 头 `TimeDateStamp`(偏移 136) 等 3 处链接时间戳）
  ⇒「同源码 ⇒ 同 SHA」不成立；此前同一最终源码曾产出 `8253BE9E…` / `43B63F92…`。登记的是最终树最后一次真实构建。
- **不得表述**：palette 侧职责已迁完（M2-4 与 M2-5 余项 B1/B2/B4/B5/B6–B11 未做）；B4 7 条 / B5 1 条 / 护栏 2 条不在本轮口径；
  34 字段出口链、多线程、`computeShadowManifestPartKey` 宿主替身、GPU/实机均未覆盖。未部署、未启动游戏、未 git 写、未晋升稳定。
- 记录文档：`docs/plan/2026-09-18-m2-5-taxonomy-extraction-record.md`（36,169 B）；全量门禁原始输出
  `docs/plan/2026-09-18-m2-5-full-gate-rerun.log`。
## 2026-09-18 — M2-3（motion 诊断三函数迁出）+ 主线程独立验收通过；含「还原必须 touch 重编译」构建陷阱（未提交 / 未部署 / 未晋升稳定）

- 迁出：War3NoteLivePaletteMotion / War3NoteDrawTimePoseMotion / War3NoteSubmittedPaletteMotion 与其两个 Entry 结构，device.cpp :7344-7520 共 177 行 → src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}（模块 .cpp 34,556 → 41,008 B）；5 个调用点一字未改，经同一条 using-directive 解析。新增 FROZEN_M2_3（三个符号各 (1, 0)；baseline 用门禁同一 DEF_RE 实测，且与迁移同一次改动落地——budget=0 只在迁移同时成立）。
- 等价证据（子代理）：legacy 参考 8,406 B / 7DF61688219BAF83BCA1836FBEB97DC2D1269B103A703917718CA9C9177AC727（生成器新增 --m2-3）；差分 6,956,814 断言，四矩阵 default 17,314,411 / flipped 18,123,899 / contract-on 13,826,299 / motion-on 17,314,411 全绿；三条变异真跑被杀（模块条件反转 ⇒ 差分红；原样回流 ⇒ 编译器亦红且门禁点名；可编译回流 ⇒ 门禁仍点名）。
- 主线程独立验收（非转述）：ninja -n no work、预算门禁 EXIT 0（冻结 149、站点 迁移前 159 → 上限 115 → 当前 115、已迁出 38、device.cpp 站点 577）、M2 等价门禁 EXIT 0、全量静态 250/250；DLL 36,271,207 B / 0090B354659432BAC0D6F341A099987F9A15D1B2FC1602712F8D1D1A3E385193。变异原始输出标注为子代理证据（主线程未重复执行变异）。
- 构建陷阱（重要纪律）：Copy-Item 还原源码保留旧 mtime ⇒ ninja 判 no work、首次复绿实为红；还原后必须 touch + 真实重编译。源码 SHA 相同不等于二进制来自该源码。
- 不得表述：palette 侧职责已迁完；M2-4/M2-5、M3、M4 未做。未部署、未实机、未 git 写、未晋升稳定。

## 2026-09-18 — M2-3（motion / churn 诊断三函数）迁出 + 同型等价证据闭合（未提交 / 未部署 / 未晋升稳定）

- **范围**：M2 的第三片。`War3NoteLivePaletteMotion` / `War3NoteDrawTimePoseMotion` /
  `War3NoteSubmittedPaletteMotion` 连同各自 Entry 结构（`War3SemanticPaletteMotionEntry` /
  `War3SemanticHashMotionEntry`）共 **177 行**（pre-M2-3 device.cpp :7344-7520）**逐字节**迁入
  M2-1 已建模块 `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`；device.cpp 的
  5 个调用点（:19378 / :21066 / :21362 / :26877 / :26932）一字未改，经 M1/M2 同一条
  `using namespace dxvk::war3::semantic;` 机械解析。模块 .h 只前向声明 `War3ShadowCaptureStats`，
  重量头 `d3d9_war3_scene.h` **只出现在模块 .cpp**（头文件 include 面不膨胀）。
  device.cpp 2,278,494 → **2,273,048 B**（`ECC4B828…` → `D8211864…`）。
  **M2-4/M2-5（余下调色板符号 + 调用点编排/taxonomy 计数对照）未做。**
- **provenance（fail-closed）**：pre-M2-3 工作树 = 2,278,494 B / `ECC4B828AF344DCF…`，
  捕获时间 2026-09-18T02:18:25（机器本地时钟）；与 M2-2 记录"迁移后 device.cpp"SHA 完全一致，
  交叉闭合；同为工作树文件哈希而非 git blob（M2-1/M2-2 均未提交）。快照备份
  `%TEMP%\device_pre_m2_3.cpp`。生成器新增 `--m2-3` 模式（fail-closed 校验快照 SHA/字节数）。
- **盲区修补（本轮关键）**：这三个符号**不匹配**预算门禁语义选择命名族 regex ⇒ 其判定 3（回流）
  对它们完全盲。新增 `FROZEN_M2_3 = {三个符号: (1, 0)}`（**独立成表再 `FROZEN.update`**，
  避免 M1 等价门禁把 M2-3 迁出集合误判为"M1 迁出但无 M1 等价证据"），并与迁移**同一次改动**落地
  （budget=0 只在迁移同时成立）。baseline=1 为**实测**（门禁同一 `DEF_RE` + 第 0 列规则：
  三符号各 1 个定义站点、无前置声明；Entry 结构以 `{` 结尾不占站点）。
- **等价证据**：motion legacy 参考 `war3_live_palette_selection_motion_legacy_reference.inc`
  = 8,406 B / SHA-256 `7DF61688219BAF83BCA1836FBEB97DC2D1269B103A703917718CA9C9177AC727`
  （M2-1/M2-2 的 .inc 与其 SHA 未动）。差分测试加法式扩展：motion 电池 **6,956,814 断言**
  （早退/新 runtime/raw·group 四态系统网格 + 700 指针填满 512 项表并越过替换游标 +
  300,000 轮定种子随机；23 个 motion 字段逐调用 + 整结构 `memcmp`）；`--probe-motion`
  15 场景 × 2 组 env 由**独立期望值表**校验。四矩阵在最终树全绿：default 17,314,411 /
  flipped-branches 18,123,899 / contract-on 13,826,299 / motion-on 17,314,411。
- **变异真跑 3 条（非声称）**：①模块侧 raw-changed 计数条件反转（可编译）⇒ 差分
  17,519,175/18,123,899 红、独立 M2-3 门禁红、M2 门禁红；②把 `War3NoteSubmittedPaletteMotion`
  verbatim 回流 device.cpp ⇒ **编译器**亦拒绝（`error: call of overloaded … is ambiguous` @21248）
  且预算门禁红并点名 `War3NoteSubmittedPaletteMotion: device.cpp=1 > budget=0`；
  ③**可编译回流对照**（同一 verbatim 块放入具名命名空间）⇒ 构建 exit 0 而预算门禁**仍**红并点名，
  独立 M2-3 门禁仍绿 ⇒ 证明 `FROZEN_M2_3` 判定 1 是"编译器不拦"时的唯一捕手。
  三次还原均以 SHA-256 证明逐字节相同。
- **实测构建陷阱（本轮新发现）**：`Copy-Item` 还原会保留源文件旧 mtime ⇒ ninja 判
  `no work to do`，第一次"复跑绿"仍红（16,709,687/17,314,411）。必须 **touch 还原文件 + 真实重编译**
  才是真复绿；"源码 SHA 相同"不等于"二进制来自该源码"。后续所有还原点均显式 touch。
- **门禁（Below Normal + `-j2`，最终树 2026-09-18 02:30:43→02:31:29 复跑，原始输出
  `docs/plan/2026-09-18-m2-3-full-gate-rerun.log`）**：构建 exit 0、`ninja -n` no work、
  预算门禁 EXIT 0（149 冻结 / 迁移前站点 159 → 上限 115 → 当前 115 / 已迁出 38 / device.cpp 站点 577）、
  M1 等价门禁 EXIT 0（一字未改）、M2 等价门禁 EXIT 0（加法式扩展）、**新 M2-3 等价门禁 EXIT 0**、
  meson **83/83（Fail 0）**、全量静态 **250/250 PASS 0 FAIL**（249→250，新增 M2-3 门禁 1 个）、
  记录器 **23/23**、成本 **COST_VERDICT=PASS (38/38)**、生命周期 **187/187**、
  解析器静态 **94/94**、通用根读方静态 **55/55**、往返 **CHECKS=1107 FAILURES=0 /
  `ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`（A–F 全满足）**。
  跟随迁移改锚 1 处（`test_semantic_palette_diagnostics_hotpath_static.py` 改从模块 .cpp 取三函数体，
  断言未改）。
- **DLL 身份**：`build32/src/d3d9/d3d9.dll` = **36,271,207 B** /
  SHA-256 `0090B354659432BAC0D6F341A099987F9A15D1B2FC1602712F8D1D1A3E385193`
  （M2-2 后 36,270,708 B，+499 B）。**未部署、未启动游戏、未启动编辑器、未 git 写**；
  现场 `E:\Work\Warcraft III\d3d9.dll`（`A0A51AF2…`）未被触碰。
- **未做到 / 边界（如实）**：调用点编排与 taxonomy 发射（M2-5）、消费端
  （`war3_perf_monitor` / `war3_control_plane` / `war3_shadow_runtime_bridge`）行为、
  GPU/Vulkan/实机画面与性能均未覆盖；未构造"能编译且调用点真走 device.cpp 副本"的变异形态；
  `s_replaceCursor` 的 2^32 回绕结构上不可达；env 覆盖为枚举矩阵而非穷举；
  差分测试中 `Check`/`CHECK` 原语从未被调用属**迁移前既有**状态（本轮只增不删）。
  本轮**不称稳定版**，也不得表述为"palette 侧职责已迁完"。详见
  `docs/plan/2026-09-18-m2-3-migration-equivalence-record.md`。

## 2026-09-18 — 用户授权今晚游戏自动化测试 + 无人值守交接给 Deepseek（状态记录，非代码变更）

- **用户授权（2026-09-18 01:05）**：今晚游戏相关自动化测试（取证 + 性能日志）全部同意，
  归为开发测试一部分；同意下载所需工具；无人值守至中国时间 08:00 并产出一整晚任务总结。
  仍不授权：部署/替换 `E:\Work\Warcraft III\d3d9.dll`、任何 git 写操作、宣称稳定版。
- **交接文档**：`docs/plan/2026-09-18-deepseek-overnight-handoff.md`——含起点状态（M2-2 后
  DLL 36,270,708 B / `EB9D51DE…`、现场 DLL `A0A51AF2…` 勿动）、任务序列（现场保险 → T1 实机
  取证 ×2 + 三项判定 → T8 前台性能基线 → M2-3 迁移 → M2-5/T7 设计）、纪律、安全轨与收尾要求。
- 本条目不改任何代码/门禁；M2-1/M2-2 的门禁结论见其各自条目，不受影响。

## 2026-09-18 — M2-2（调色板来源选择链本体）迁出 + 同型等价证据闭合（未提交 / 未部署 / 未晋升稳定）

- **范围**：M2 的第二片（工单 docs/plan/2026-09-18-relay-m2-t2t3-workorder.md §2）。
  `War3TryBuildLiveRuntimeGroupPalette` 本体（含 `resolvePaletteSlotIndex` lambda 的 4096 项
  thread_local slot 缓存 + 8192 项 lookup 加速器、2026-09-16 P0 Gap A producer 复核）、
  `War3SemanticPaletteSource` 7 值枚举、`War3LivePaletteBuild*` 分相计时类型（QPC 校正逐字保留）、
  Gap A 两计数器定义，从 d3d9_device.cpp **逐字节**迁入 M2-1 已建模块
  `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`（同命名空间，device.cpp 调用点
  经同一 using-directive 机械解析）。唯一机械调整：默认实参集中在头文件声明（M1/M2-1 同型）；
  B 类痕迹一处（计数器上方旧布局注释尾行按先例删除，模块注释已说明）。
  device.cpp 2,310,648 → **2,278,494 B** / SHA-256 `ECC4B828AF344DCF…`。
  **M2-3（motion 诊断）、M2-5（调用点编排/taxonomy）、M3、M4 未做**。
- **provenance（fail-closed）**：pre-M2-2 工作树 = 2,310,648 B / `B70F3AF9…`，捕获时间
  2026-09-17T22:54:30（机器本地时钟）；与 M2-1 记录"迁出后"SHA 完全一致，交叉闭合；同为
  工作树文件哈希而非 git blob（M1/M2-1 均未提交）。生成器新增 `--m2-2` 模式（fail-closed
  校验快照身份），产出链 legacy 参考 **36,025 B / `02FF8AFE…`**；首版漏抽 :5969-5984
  前向声明（携带 `outSelection` 默认实参）导致宿主编译失败，已**逐字节**补入修正，无任何
  为通过编译的文本编辑。M2-1 的 .inc 与 SHA 未动。
- **差分**：同一测试文件加法式扩展（M2-1 断言一字未动；链依赖的 12 个外部符号为宿主机
  替身、两侧共用；`skin::Selection`/QPC/资源访问器为共享真实头）。**55 个系统场景 +
  20,000 轮固定种子随机世界**，比较返回值/全部输出参数/palette 逐 float-word/
  timing.calls[] 逐相/两计数器单调用增量/Selection 全 15 字段（QPC ticks 不比）：
  default **10,357,597/10,357,597**、flipped-branches **11,167,085/11,167,085**、
  contract-on **6,869,485/6,869,485** 全过。`--probe-chain` 10 固定场景 × 3 env：
  module/legacy 全 10 字段逐行一致 + 独立期望值表（合同 ON：owned-hit source=6/slot=42，
  其余 9 场景 fail-closed 不 fallthrough；flipped：cmodel-deny 翻转 source=5）。
- **门禁**：等价门禁加法式扩展（链 legacy SHA/provenance 钉死、FROZEN_M2_2↔CHAIN_COVERED、
  链符号三处出现、contract-on 矩阵、分矩阵检查数下限 ≈实测 70%、probe-chain 期望值表）；
  M2-1 断言/SHA/PROBE_SCENARIOS 未动。预算门禁加 `FROZEN_M2_2`（2→0）；6 个断锚门禁按
  "跟随迁移改锚、断言语义不削弱"修复（断言一字未改）。
- **全量复跑（留档 docs/plan/2026-09-18-m2-2-full-gate-rerun.log）**：DLL 构建 NINJA_EXIT=0，
  `build32/src/d3d9/d3d9.dll` = **36,270,708 B**；ninja -n no work；预算/M1/M2 门禁 exit 0；
  **meson 83/83**；**全量静态 249/249**。**DLL 哈希的构建非确定性**：链接器在 PE 头嵌入
  构建时刻 TimeDateStamp（已实测字段佐证），同源字节重链 SHA 不同——阶段 5 构建
  `146B34A9…`，变异回退（源码逐字节还原）后重链 `EB9D51DE…`，尺寸相同。
  git-bash 直调 meson 的 `setup_vsenv` 崩溃为已知环境问题（缺 ProgramFiles*，同
  run_ninja_m2_2.cmd 注释），经 env 传递后通过，非测试失败。
- **变异（真跑 ×2，均完成 杀死→逐字节回退→复绿 全循环）**：(a) 对调 published-pose
  ±0xA0 候选次序 → 差分 10,228,430/10,357,581（129,151 项 DIFF：poseModel/rawPoseHash/
  paletteWord），门禁 exit 1；(b) Gap A producer 复核改 `if (true)` → DIFF 精确命中
  （slotIndex 9 vs 0xFFFFFFFF、servedDelta 1 vs 0、rejectedDelta 0 vs 1）、probe-chain
  reject-then-published 翻转，门禁 exit 1。每次回退后模块 .cpp SHA-256 还原
  `213A280C…`（MATCH）。
- **对上方 2026-09-17 更正条目的闭环**：该次红树证据与 M2-2 开发中间态一致——其 crash
  签名（`stl_vector.h:1263 operator[]`，元素 `std::vector<dxvk::Matrix4>`，默认 env 崩、
  --probe 正常）与本人在 M2-2 随机电池中复现并修复的输入域违例**完全相同**（proven
  上界被生成得小于实际最大槽位，触发链信任上界的 sparse 写入越界；电池生成器已按合同
  修正，M2-1 自身电池无此代码路径）；其 DLL SHA `146B34A9…` 即本轮阶段 2 构建产物；
  台账 `D82AFF08…`/36,271,659 B 与 146B34A9…/36,270,708 B 的差异由 M2-2 迁移
  （-951 B）与上述 PE 时间戳非确定性共同解释。精确到分钟的时序事后无法逐一对齐，
  但当前树同一路径 10,357,597/10,357,597 exit 0、meson 中该测试 OK、全量静态
  249/249——更正条目"修好前不得引用"的状态在当前树不再适用。
- **未做到**：未提交 git、未部署（`E:\Work\Warcraft III\d3d9.dll` 未动）、未启动游戏、
  未实机验收；宿主机差分 + 静态门禁证据不构成稳定版依据。完整记录：
  `docs/plan/2026-09-18-m2-2-migration-equivalence-record.md`。

## 2026-09-17 — 【主线程更正】M2-1 台账数字与当前树不一致；树当前为**红**（未提交 / 未部署 / 未晋升稳定）

- 上面 M2-1 条目记载「meson **83/83**、全量静态 **249/249**、DLL 36,271,659 / `D82AFF08…`」，但主线程在 16:17 UTC **独立复跑当前树**得到的是：
  `meson` **82/83，1 个 FAIL**（`warvk:war3_live_palette_selection`，exit `0xC0000409`）；`FULL_STATIC` **248/249**（失败者即新等价门禁 `test_war3_live_palette_selection_equivalence_static.py`）；
  `ninja -n` no work；`build32/src/d3d9/d3d9.dll` = **36,270,708 B / `146B34A96ECABB28ADA89F812B2B5B9869AD59E52111A8BD0F6D962D3EC0BBB0`**（mtime 15:05），**不是**台账里的 `D82AFF08…`。
  ⇒ 该条目的门禁结论**取自更早的一次构建**，不适用于当前树；**在修好之前不得引用其 83/83、249/249 与 DLL 哈希**。
- **故障点（可复现）**：`build32/src/d3d9/war3_live_palette_selection_test.exe` 在**默认 env** 路径崩溃——
  `stl_vector.h:1263 operator[] 断言 __n < this->size() failed`，元素类型 `std::vector<dxvk::Matrix4>`，进程 exit `0xC0000409`；
  `--probe` 路径正常（exit 0），说明崩在默认差分矩阵而非 probe。等价门禁失败即因它跑同一矩阵（`default 失败 exit=3221226505`）。
- **未受影响**：M1 预算门禁 EXIT 0、M1 等价门禁 EXIT 0、记录器 23/23、解析器静态 94/94、根读方 55/55。
- **实机/部署状态**：`E:\Work\Warcraft III\d3d9.dll` 仍为 `A0A51AF2…`（11:25，未变）⇒ **Kimi 的 M2-1 未部署**，也没有新的实机取证导出；T1（实机对象级取证）仍未做。

## 2026-09-18 — M2-1（live palette 纯计算 helper ×4 + env getter ×4）迁出 + 同型等价证据闭合（未提交 / 未部署 / 未晋升稳定）

- **范围**：M2 的第一片（工单 docs/plan/2026-09-18-relay-m2-t2t3-workorder.md §2）。8 个符号
  （War3SemanticHashMatrixPalette / War3DecodeRuntimePoseMatrix48 / War3TryReadRuntimePoseArray /
  War3ResolveLivePoseRuntimeAlias + SafeCopy/Refresh/AllowCModelFallback/PaletteDiagnostics 四个 env getter）
  从 d3d9_device.cpp 迁到新模块 `src/d3d9/war3/semantic/war3_live_palette_selection.{h,cpp}`
  （命名空间 dxvk::war3::semantic，device.cpp 调用点经 M1 同一条 using-directive 机械解析）。
  **M2-2（选择链本体/枚举/类型/slot 缓存/计数器）、M2-3（motion 诊断）、M3、M4 未做**；
  两份 HashMatrixPalette 姊妹（S2 §5-9）仅登记未动；taxonomy 发射全部留在 device.cpp。
- **逐条等价**：8 个函数体脚本复核逐字节相同，唯一差异是 PaletteDiagnosticsRuntime 去掉
  匿名命名空间内冗余 inline（M1 B 类先例，行为不可观测）；无默认实参移动（8 符号均无），
  env getter 函数内 static 一次性读取逐字节保留。
- **provenance 差异（显式）**：M1 用已提交 git blob；M2-1 迁移前状态是**未提交工作树**
  （SHA-256 2736335B…，与 M1 记录"迁出后"一致），git 无对应 blob，故以工作树文件 SHA +
  捕获时间双钉死。**生成器入库**（AutoTest/gen_war3_live_palette_selection_legacy_reference.py，
  补 M1 已知缺口），产出 legacy 参考 7,702 B / D458C1AE…。
- **可执行差分**：新测试 war3_live_palette_selection_test（真实链接模块 .cpp + util_matrix.cpp；
  内存/env 原语宿主机替身两侧共用；sem::War3GetEnvU32 为与 M1 模块逐字节相同的替身，
  legacy 侧 .inc 内嵌 M1 模块同文定义）覆盖哈希/解码往返/stub CModel pose 数组/stub
  runtimeModel ±0xA0 alias/env getter：**2,380,985/2,380,985 检查点 0 差异**；--probe 6 组
  env 场景逐 getter 对照 module vs legacy vs 独立期望值。
- **变异验证（真跑）**：(a) FNV 偏移基数 +1 → 差分 220,041 条 DIFF、2160944/2380985、exit 1，
  等价门禁 exit 1；(b) AllowCModelFallback 默认值 0u→1u → default 矩阵 2380984/2380985 恰 1 失败、
  门禁 exit 1，--probe 实测 module=1 legacy=0。两条还原后模块 SHA-256 逐字节相同（C7B2434F…）。
- **门禁（Below Normal + -j2 复跑）**：构建 exit 0、ninja -n no work、预算门禁 EXIT 0
  （冻结 146 / 已迁出 34 = M1 26 + M2-1 8 / 站点 582；FROZEN_M2_1 独立成表避免 M1 门禁误判，
  M1 等价门禁一字未改复跑全绿）、新等价门禁 EXIT 0、meson **83/83**（82+1）、
  全量静态 **249/249**（248+1；既有 test_semantic_palette_diagnostics_hotpath_static.py
  跟随符号改读新模块，断言未改）、记录器 23/23、成本 PASS(38/38)、生命周期 187/187、
  解析器静态 94/94、根读方静态 55/55、往返 CERTIFIED。
- **DLL**：36,271,659 B / SHA-256 D82AFF084C80BBE76F43DF6F6C1E9C0230EA59FCEFD473A329D6A7D2250A25F6
  （迁移前候选 36,271,456 B / CB622534…）。**未部署、未启动游戏、未 git 写、不称稳定版**。
- **文档**：docs/plan/2026-09-18-m2-1-migration-equivalence-record.md（对照表、provenance、
  差分设计、变异记录、未覆盖项：M2-2/3 未做、taxonomy 对照属 M2-5、合同 ON 分支差分属 M2-2、
  实机未跑）。

## 2026-09-17 — M1 等价性证据补齐：逐条对照 + 宿主差分等价测试 + 两条变异验证（未提交 / 未部署 / 未晋升稳定）

- **背景**：执行 M1 的子代理收尾失败、未留报告，M1 的逐点等价性当时只有门禁级证据支撑（记为未闭合项）。本轮只补证据，**未改任何判定语义**（模块 .h/.cpp 与 d3d9_device.cpp 的 SHA-256 全程不变）。
- **逐条对照**：把 git HEAD ae890542d766 的 d3d9_device.cpp 中 49 个迁出定义的**逐字节原文**抽成 legacy 参考（43,452 B / SHA-256 2EA43F97…），与迁出后模块逐符号对照：**未发现任何语义差异**；全部差异只有四类搬动痕迹（inline 增删、默认实参从定义移到声明、命名空间移到 dxvk::war3::semantic、全局计数器由内部链接改为外部链接）。
- **可执行差分**：src/d3d9/war3/semantic/tests/war3_device_semantic_predicates_test.cpp 在同一输入域同时运行引用实现与模块实现（真实链接模块 .cpp；device/注册表/钩子原语用宿主机替身且两侧共用），**3,098,194 个检查点 0 差异**；新增 --probe 在 6 组 env 场景逐 getter 对照 module vs legacy 与独立期望值（0/上界/clamp/strtoul base-0/八进制/空值/无数字/溢出/负号）。
- **变异验证（真跑：改→构建→跑→红→还原→复跑绿）**：(a) 反转 War3SemanticPacketHasConsistentUnitCore 的 Building 比较 → 差分测试 504 条 DIFF、exit 1（证据门禁 exit 1）；(b) 在 device.cpp 重新内联 War3SemanticReflowedSelectionKeyProbeForGate → 预算门禁 exit 1。两条还原后源码 SHA-256 **逐字节相同**（73C7D1A2… / 2736335B…）。
- **门禁（Below Normal + -j2 复跑）**：构建 exit 0、ninja -n no work、预算门禁 EXIT 0（145 冻结/26 迁出/591 站点）、新增等价证据门禁 EXIT 0、记录器 23/23、成本 PASS(38/38)、生命周期 187/187、解析器静态 94/94、根读方静态 55/55、往返 CERTIFIED exit 0、meson 82/82、全量静态 **248/248**（上一轮 247，新增等价证据门禁 1 个）。
- **DLL**：36,271,456 B / SHA-256 CB62253417CEC91ADC9A16F7D5BAC8E758370C62DFBFEB63D84906FF031750AE；受控实验证明同源两次链接仅 6 字节（3 处时间戳字段）不同，故与现场部署版 A0A51AF2… 代码内容一致。**未部署、未启动游戏、未触碰 E:WorkWar3**。
- **文档**：docs/plan/2026-09-17-m1-migration-equivalence-record.md（对照表、差分设计、未覆盖项）。
- **边界**：M2/M3/M4 未开始；差分是同一输入域证据（替身世界为夹具），env 分支非穷举，traceWorldWidgetProbe=true 未差分；不得称为"职责已迁完"或"已稳定"。

## 2026-09-17 — 实机路径**假设错误**更正 + 首次真实实机导出分析（对象级子门未开，未采到对象级证据）+ M1 等价性证据闭合（未提交 / 未晋升稳定）

- **假设错误（必须记住）**：我一直把"测试现场"当成 `E:\Work\War3`，但 **YDWE 实际启动的是 `E:\Work\Warcraft III`**。
  因此我先前那次"部署"对实机测试**无效**（只改了不参与启动的那份，无害）；用户是**手动**把同一构建放进真实启动目录的。
  更正后的实机复核：`E:\Work\Warcraft III\d3d9.dll` = **36,271,456 B / `A0A51AF2…18E52134`**，与我离线验证的候选**同一哈希**。
  **今后实机脚本必须 `cd /d "E:\Work\Warcraft III"` 并从该目录经 YDWE 启动**（已修正包内启动脚本，并新增含对象级子门的启动脚本）。
- **首次真实实机导出分析**（`E:\Work\Warcraft III\WarVK\Log\FrameEvidence\cpu-42860-581485704044-1.json`，2026-09-17 20:12）：
  `schema 7 / state 3(Frozen) / reason 3(Capacity) / accepted 2,670,291 / session 1`；头块 `paletteObject.version=2`，
  但**所有计数为 0、watchCount=0** ⇒ **对象级证据子门当时未开启**（我给的取证脚本故意不带子门，且其 cd 目录也错），
  因此**对象级证据链在实机上仍未采到**；生产解析器对空样本正确判"未覆盖"（exit 2，chains 为空）。**不得据此声称任何阴影结论。**
- **正面发现（真实实机）**：该导出是**容量自动冻结**（`reason=3 Capacity`）后仍可完整读出的 **version 2** 导出 —— 这在实机上确认了
  「自动冻结 + 预冻结钩子」路径能产出可解析的版本 2 产物（此前只有宿主机场景 F 的证据）。
- **用户实测约束**：`Ctrl+Shift+C` **每个会话只能导出一次**（无法连按两次）⇒ 要分离"高压"与"压力解除后"必须**分两次进图**（各导一次），不能指望一个窗口同时判两件事。
- **M1 等价性证据已闭合**（子代理补齐 + 主线程独立复跑）：迁移前后共 **49 个定义逐条对照，未发现判定语义差异**（差异只有搬动痕迹：inline 增删、默认实参移到声明、命名空间、计数器链接属性）；
  宿主差分 **3,098,194/3,098,194 检查点 0 差异**；两条变异真跑被杀（判定反转 ⇒ 504 条 DIFF；职责回流 ⇒ 预算门禁点名该符号），还原后源码 SHA-256 逐字节相同；
  新增等价证据门禁 `test_device_semantic_predicates_equivalence_static.py`；全量静态 **248/248**、meson **82/82**、记录器 23/23、成本 PASS、生命周期 187/187。
  **仍未做 M2/M3/M4**；不得称"职责已迁完"。

## 2026-09-17 — 用户远程授权部署 + AutoTest 实机；M1（device.cpp 语义职责纯判定迁出）落地并部署（未提交 / 未晋升稳定）

- **授权**：用户远程明确授权「可直接部署并用 AutoTest 测试」。据此本轮**已部署**到测试现场 `E:\Work\War3`（**玩家目录未触碰**）。
- **M1 迁移落地**：按 `docs/plan/2026-09-16-device-semantic-responsibility-migration.md` §3 的 M1（**无 device 成员依赖的纯判定与常量**）
  迁出到新模块 `src/d3d9/war3/semantic/war3_device_semantic_predicates.h`（12,214 B），device.cpp 改为调用；
  并新增**防回退固定门禁** `AutoTest/test_device_semantic_responsibility_budget_static.py`（15,441 B）：逐符号钉住 device.cpp 中的语义选择符号与数量上限，
  **只允许减少**。门禁实测输出：迁出 **26 个符号**、语义断言站点 591、符号数下降。**未做 M2/M3/M4**。
- **诚实说明**：执行 M1 的子代理在**收尾阶段失败且未留报告**，因此它的等价性逐条对照与变异证据**缺失**；
  主线程独立复核了状态：构建 exit 0、`ninja -n` no work、记录器 **23/23**、成本 **PASS**、生命周期 **187/187**、
  meson **82/82**（含新预算门禁）、静态门禁 EXIT 0、全量静态 **247/247**、预算门禁 EXIT 0。**M1 的逐点行为等价性只由门禁级证据支撑，尚无逐条对照（列为未闭合项）。**
- **实机**：现场原测试版 `D1C3EBBB…` 已备份为 `d3d9.dll.bak_20260917_D1C3EBBD_pre_m1`（更早的原始版 `74CC676B…` 备份仍在）；
  现现场 = **36,271,456 B / `A0A51AF2BB9091B2C677DC34BEBDF76C02347049A89D0BB2AEF4C37518E52134`**；
  已派专职子代理按 `AutoTest/README.md` 的规则运行实机（含开启对象级证据子门的高压取证轮），要求进程收尾并如实报告未达成项。

## 2026-09-17 — D2 生产写入侧落地：记录级分段 + 载明的身份证明种类 + 头块 version=2（未提交 / 未部署 / 未晋升稳定）

- **生产写入侧（本轮 C++ 改动）**：`PaletteObjectEventRecord` 增 `windowSegment`（记录级，发出时写一次）
  与 `identityProofKind`；`PaletteObjectEvidence` 增 `m_windowSegment`（Reset 置 1、ResetForSessionTransition
  **递增**，即「清表 ≠ 分段」的修复）；`MakeRecord()` 由 static 改为 const 成员并在其中盖章，`NoteReject`
  的一次性 TableFull 终态同样盖章；`DeriveIdentityProofKind()` 是**唯一**推导规则
  （`lifecycleIdentity != 0 && !identityWeak && !epochUnknown` ⇒ 1，否则 0），无 setter / 无测试写入后门。
  sink 写 `data[3]=windowSegment`、`words32[15]=identityProofKind`，其余保留位仍不得被写；
  `PaletteObjectHeaderJson()` 头块 `version=2`（版本 2 = 现行生产形状）。
- **认证语义（收紧，未放宽）**：三个生产采集点一律 `lifecycleIdentity=0 + identityWeak=true +
  epochUnknown=true` ⇒ 生产 `identityProofKind` 恒 0 ⇒ 版本 2 下生产导出**不会**认证任何同对象恢复链
  （具名 `identityNotProven`）。**认证路径仍可执行**：往返夹具用合成键（非零实例生命周期身份 + 两个强
  标记为假）覆盖 A/F，实测 `identityProven=true` / `identityBasis=wireCarriedInstanceLifecycleProof`。
  版本 1（含全部旧实机产物）合同一位不放宽。
- **期望联动**：往返 C++ 夹具把「`data[3]`/`bits[15]` 必须为 0」改为**声明后的语义**断言
  （`data[3] == windowSegment`、`bits[15] == identityProofKind`，其余保留位仍必须为 0）；解析器静态
  写入集合改为 `{0,1,2,3,9,10,11}` / `{…,15,…}` 并同时钉死版本 1 仍把两槽当保留零；往返编排测试要求
  真导出为版本 2、按版本 2 解码、钉死分段/证明种类/`identityProven`。静态门禁新增 `11r`（只加不减，
  160 行）：分段递增、记录级盖章、证明种类三条件与无后门、sink 写入集合、头块 version=2。
- **门禁（全程 Below Normal + `-j2`）**：构建 exit 0（30/30，含 `d3d9.dll`）、`ninja -n` no work、
  记录器 **23/23**（新增案例 23 实测：Reset 后窗口 1、换图后窗口 2/3、生产键 kind=0、合成认证键 kind=1）、
  成本 **COST_VERDICT=PASS (38/38)**、生命周期 **187/187**、解析器静态 **94/94**、通用根读方静态 **55/55**、
  往返 **1107 项 0 失败 / A..F 全满足 / `CERTIFIED_SPEC_SATISFIED`**、meson **81/81**、静态门禁 EXIT 0、
  全量静态 **247/247 文件（1238 用例）0 失败**；DLL **36,272,145 B /
  `EB17FC60F532ACE37F151D69E3A66938D4C52401B8FE8C9A735303891EEB0D40`**（本树 PE 时间戳导致重建后 SHA 变化，属已知）。
- **未做 / 边界**：未部署、未启动游戏、未提交；DLL 的写入侧证据 = 重建对象 + 同一生产源码的静态钉死 +
  往返 exe 的真实导出入证（未启动游戏验证 DLL 运行时行为）。分段标签只标识「记录器把该记录归到哪个窗口」，
  **不是**设备 Reset 证据，也**不是** GPU 提交 / 像素证据。

## 2026-09-17 — D2 生产写入侧落地（窗口分段 + 身份证明种类 + 头块 version 2）；pair-0 全部前置闭合并提交申请（未提交 / 未晋升稳定）

- **D2 已落地**（生产 C++，按我的裁定）：`PaletteObjectEventRecord` 新增 `windowSegment`/`identityProofKind`；
  `Reset()` 置窗口 1、`ResetForSessionTransition()` **递增**（这才是「清表 ≠ 分段」的修复）；`MakeRecord` 改为 const 成员并给每条记录盖记录级分段；
  转换点写 `data[3]=windowSegment`、`bits[15]=identityProofKind`；头块 `version=2`。
- **身份证明种类按规则推导、无测试后门**：`kind = (lifecycleIdentity != 0 && !identityWeak && !epochUnknown) ? 1 : 0`；
  全树只有 3 处赋值，右值全部是 `DeriveIdentityProofKind(...)`（门禁 §11r 钉死）。**生产三个采集点恒为 0** ⇒ 版本 2 下生产导出
  一律具名 `identityNotProven`（有意的收紧，与今天的事实一致）；**已认证路径仍可执行**：合成键（非零身份 + 两强标记）实测 `kind=1`，
  往返 A/F 真导出实测 `Recovered + sameObjectCertified=True + identityProven=True`（`identityBasis=wireCarriedInstanceLifecycleProof`）。
- **实测见证**：记录器出口 `window1-records=5 wrong-segment=0 window1-terminal.segment=1 ; transition → window2.segment=2 ; window3.segment=3`；
  真实导出 A/F `ver=2 data[3]=[1] words32[15]=[1]`，B/C/E `words32[15]=[0]` 且具名拒绝，D 空样本未覆盖；`data[4..8]` 仍全 0。
- **门禁**（主线程 2026-09-17 18:55 独立复跑）：`ninja -n` no work、记录器 **23/23**、成本 **PASS (38/38)**、生命周期 **187/187**、
  解析器静态 **94/94**、通用根读方静态 **55/55**、往返 **1107 项 0 失败 / A..F 全满足 / CERTIFIED**、meson **81/81**、静态门禁 EXIT 0、全量静态 **247/247**；
  DLL **36,272,145 B / `EB17FC60F532ACE37F151D69E3A66938D4C52401B8FE8C9A735303891EEB0D40`**。
- **pair-0 前置全部闭合**（上游裁定的五步顺序逐项有证据）⇒ 已写申请材料 `docs/plan/2026-09-17-p0-real-machine-application-round13.md`，
  含候选身份、与上一实机候选 `8CECC495…` 的差异、前置闭合表、建议方案（现场 `E:\Work\War3`、1902×963、contract ON/OFF 一对、不重跑）与**明确不承诺项**。
  **等待用户/上级的显式部署授权**（现场当前仍是更早的冻结测试版 `D1C3EBBB…`）。
- **仍未做**：槽位所有权证明（路由 A 未批）、构建线程的运行时覆盖、⑤ 统一选择入口与 device.cpp M1–M4（迁出量 0）。

## 2026-09-17 — 主线程裁定并落地 D1（显式 version:1）+ D2 裁定；读方侧版本化/分段全套门禁复跑（未提交 / 未晋升稳定）

- **D1 已落地**（我批准的 1 行、零语义变化改动）：`PaletteObjectHeaderJson()` 现在显式写 `result["version"]=1;`，
  把「现行冻结形状」写出来；读方对 `{version:1,…}` 与旧形状判定逐字段相同，旧实机产物仍按同合同可读。
- **D2 裁定（分段 + 身份证明种类，含一次认证收紧）**：**批准分段**（生产写 `data[3]=windowSegment`、头块 `version=2`）
  与**身份证明种类声明**（`bits[15]=identityProofKind`）；但**必须成对更新期望**：三个采集点都没有实例生命周期证明
  ⇒ `identityProofKind` 恒 0 ⇒ 版本 2 下生产导出**不会**认证任何同对象恢复链（具名 `identityNotProven`）。
  这与今天的事实一致（采集点一律 `identityWeak=true`，生产路径本就无法 `sameObjectCertified`）；
  **认证能力仍须由带显式证明种类的合成键在测试中保持可执行**（不得因为收紧而让"已认证"路径无法被测试覆盖）。
  生产写入侧改动与期望联动放在下一轮（本轮只落地 D1，避免留下红灯树）。
- **门禁**（主线程 2026-09-17 18:35 独立复跑，全部 Below Normal + `-j2`）：`ninja -n` no work、记录器 **22/22**、
  成本 **COST_VERDICT=PASS (38/38)**、生命周期 **187/187**、解析器静态 **94/94**、通用根读方静态 **55/55**、
  往返 **1081 项 0 失败 / A..F 全满足 / CERTIFIED_SPEC_SATISFIED**、meson **81/81**、静态门禁 EXIT 0、全量静态 **247/247**；
  DLL **36,272,145 B / `2BFDC23CE813AE019BA724BB0D354CCA1A1AB794E99FD6BA46CC2617F202B43B`**。
- **仍未做**：D2 生产写入侧与其期望联动；槽位所有权证明（路由 A 未批）；构建线程的**运行时**覆盖；
  ⑤ 统一选择入口与 device.cpp M1–M4（迁出量 0）。
- **测试现场**：仍是你回来要测的冻结测试版 `D1C3EBBB…`（36,253,553 B）；回退镜像与站点备份经复核均为 `74CC676B…`。

## 2026-09-17 — 冻结格式显式版本化 + 窗口/Reset 可识别分段（只改 Python/AutoTest/文档；未提交 / 未晋升稳定）

- **上级裁定 ⑦（显式版本化）已闭合到读方侧**：`paletteObject` 块改为**登记在案的根扩展**，
  版本判定只有一处实现（`AutoTest/analyze_frame_evidence.py` 的 `extension_version()`）：
  无 `version` 的冻结形状 = 版本 1（旧实机产物，旧合同逐字段不变）；显式 `version:1/2` 按登记
  版本读取；**未知版本 / 未知形状 / 非整数版本一律 ValueError**。修前通用读方对**每个真实
  palette 导出**报 `root fields mismatch`，`analyze_frame_history`/`frame_history_watch` 因此
  读不了这类导出。现在：通用读方自己认扩展，palette 读方删除"投影掉该块再委托"的绕行并把
  **整个根**交给通用读方，history/watcher 入口自动接入（往返编排测试对 A..F 的真实导出新增
  "通用读方必须接受并给出登记版本"一条）。旧产物（连块都没有）仍可读。
- **上级裁定 ⑧（清表 ≠ 分段）已闭合到读方侧**：新增登记槽位 `data[3]` = `windowSegment`（记录级、
  发出时写一次）、`words32[15]` = `identityProofKind`，**只在显式版本 2 合同下启用**；版本 1
  （含全部旧产物）下两者仍是必须为 0 的保留槽（保留位检查一位不放宽）。离线分组键 =
  八元组 + 分段量，分段必须非递减、每条 >=1。旧格式没有分段量，分组与旧合同逐字节相同，
  且**绝不**按 `chainSequence` 重启之类的顺序启发式拆链（那与 wire 撕裂同签名）。
- **「字段存在 ≠ 身份已证明」**：每条链新增 `identityBasis` / `identityProven`。版本 1 如实报告
  `recorderFlagClaimOnly` + `identityProven=false`（判定语义不变，但报告不再暗示身份已证明）；
  版本 2 必须**载明**登记在案的身份证明种类 + 按值携带非零实例生命周期身份，且
  `identityWeak=false`/`epochUnknown=false`，否则具名拒绝 `identityNotProven`/
  `identityValueMissing` —— **identityWeak 的 0 本身永远不足以认证同对象恢复**。
- **反例（同键 + Reset 前后各一条完整链）修前/修后都实测**（仓库外探针，方案见
  `docs/plan/2026-09-17-p0-object-evidence-window-segmentation.md` §5）：修前 旧格式 ⇒ **整份导出**
  被拒（`one object may carry at most one terminal event`）；旧格式"上半条 + 一条完整" ⇒ 合并成
  **1** 条链并误判成"一条链自身损坏"（假 `chainSequenceNotStrictlyIncreasing@6` 等）；
  版本 2 ⇒ **2 条链**、`windowSegments=[1,2]`、两条都认证；版本 2 缺分段 ⇒
  `palette window segment must be >= 1 (got 0)`。
- **静态门禁（§11p/§11q，只加不减）**：版本常量必须存在且两个读方**同源**、未知版本必须拒绝、
  根字段检查必须放行已登记扩展、投影绕行必须消失、history/watcher 入口必须走统一读方、
  分组键必须含分段量、分段必须逐记录读取且非递减、版本 1 保留位不放宽；并含运行期反例
  （同键跨窗口分成两条；旧格式 fail-closed；版本 2 无证明不得认证）。
- **门禁（本次独立复跑，Below Normal + `-j2`）**：构建 exit 0（Python-only ⇒ `ninja -n` no work）、
  记录器 **22/22**、成本 **COST_VERDICT=PASS (38/38)**、生命周期 **187/187**、解析器静态 **71 → 94**、
  往返 **1075 项 0 失败 / A..F 全满足 / `CERTIFIED_SPEC_SATISFIED`**、meson **81/81**、
  静态门禁 EXIT 0、全量静态 **247/247**；DLL **36,272,145 B**（Python-only ⇒ 字节与 SHA 不变）。
- **未做 / 待主线程裁定**：C++ 未改 —— 显式 `version` 字段与窗口序号/身份证明的**写入侧** diff
  已在方案文档 §4 给出（D1 一行无语义变化；D2 含一次版本 2 认证收紧，必须与"分段"一起裁定，
  因为版本 2 下三个采集点都没有实例生命周期证明，生产链不会被认证）。

## 2026-09-17 — D 点独立小型按值诊断载荷（上级裁定：闭合「正常录制下 D 来源不可得」）+ 门禁 11o（未提交 / 未晋升稳定）

- **缺陷**（上级指出的最后一条真实触发路径缺口）：D 点读 `draw.inputSkinSelection`，而它只在 `InputsEnabled()`（高内存原始输入取证）下赋值，
  因此**正常录制里 D 的来源/帧标签恒不可得**，且「已清空语义 palette」只能靠 `frameTag==0` 推断。S 点无此问题（用的是局部 `selectedPalette` 按值）。
- **修法**：`d3d9_war3_scene.h` 新增独立小型按值诊断载荷 `PaletteObjectDiagnostics`（source/slot/captureSerial/publicationTicket/frameTag）；
  `d3d9_device.cpp` 在生成绘制命令时**无条件**赋值（位于 `InputsEnabled()` 分支**之前**），并在 native override 清空 Selection 的**同一处**清空该载荷；
  D 点（`d3d9_war3_shadow.cpp`）改为只读该载荷。该载荷仅用于诊断、不参与任何判定。
- **门禁 11o（只加不减）**：场景必须有该结构；设备侧赋值必须**早于** `InputsEnabled()` 分支（这正是缺陷本质）；
  native override 清空处必须同时清空载荷；D 点必须读新载荷，且 `NoteDrawn(` 附近不得再出现 `draw.inputSkinSelection`。
- **门禁**（主线程 2026-09-17 18:15 独立复跑）：`ninja -n` no work、记录器 **22/22**、成本 **COST_VERDICT=PASS (38/38)**、
  生命周期 **187/187**、meson **81/81**、静态门禁 EXIT 0、全量静态 **247/247**；
  DLL **36,272,145 B / `6E993363BD50FE4124ECA5EF79F9E7706B4E6542593899821455F22B95281859`**。
- **仍未做（下一轮）**：冻结格式显式版本化（根对象多出的 paletteObject 块仍会让通用读方报字段不匹配；需版本化并统一接入通用/palette/history 读方，旧产物仍可读、未知版本拒绝）；
  窗口与 Reset 的可识别分段（同会话同地图且 deviceEpoch 未知时，Reset 前后的记录仍可能在离线分组时相遇）。
- **测试现场仍是已交付的冻结测试版** `D1C3EBBB…`（36,253,553 B）。

## 2026-09-17 — D6 去重载荷口径落地 + 离线成本测量（④ 前置）+ 由其驱动的哈希/布隆修复（未提交 / 未晋升稳定）

- **D6（上级裁定，已落地）**：同键同帧同阶段**只有在载荷可证等价时**才能压缩；载荷冲突必须计数并阻止认证。
  新增计数 `droppedPayloadConflict`（四处同帧去重先比较载荷：Rejected 看拒绝原因、Served 看来源+hitKey、
  Enqueued/Drawn 看来源+hitKey+清空标志），并**贯通**：导出头块 → 解析器 `COUNTER_FIELDS`（精确集 18 项）与 `LOSS_FIELDS`（非零 ⇒ 未覆盖）。
  上级给的反例（同帧第一次 D 合法、第二次 D 已清空语义 palette）现有**直接见证**（Case22）：第二次不被吞掉而是计入冲突。
  变异 **M42**（退回"不看载荷直接记重复"）被门禁与宿主机测试**同时杀死**，还原为字节级精确。
- **离线成本测量完成并落盘**（`docs/plan/2026-09-17-p0-object-evidence-cost-measurement.md`，274 行，含三次运行的极差）：
  六个触发点的精确条件与实测计数、33 轮「Reset+填满1024+CloseWindow」**零堆分配**、关闭开销（满表中位 26 µs）、
  子门关闭 1.000 ns/次、开启后记录器 28.5–32.8 ns/事件、加生产转换点 104–149 ns/事件。
- **成本测量暴露并已修的两个真实退化**（这是本轮最有价值的发现）：
  1) **哈希低位聚集**：FNV-1a 低位 + `% 1024` 使 1024 个键只落 **6** 个起始槽 ⇒ 最坏探测 **1022**、最坏查找 ~800 ns（最好 8 ns）；
     取索引前加**一次强 finalizer** 后起始槽 6 → **652**。
  2) **满表负查找退化**：饱和时线性探测必然退化（负载因子而非哈希质量问题），表满 + 未知键每次走满 1024 槽（~719 ns）。
     新增 **16384 位（2 KB，无分配）布隆前置否定**（插入置位、Reset 清空、假阳性只退回线性探测）⇒ 表满未知键 **719 ns → 29 ns**、
     全表走查 **771/1001 → 10/1001**（1.0%，设计内假阳性）。**注意：1024 位版本实测被填满而失效（771/1001 不变），已改为 16384 位。**
  两项修复都由门禁 §11n（finalizer 常量、布隆位数 ≥16384、`Find` 必须先布隆后走查、`Insert` 置位、两个 Reset 清空）钉死。
- **门禁（主线程 2026-09-17 18:1x 独立复跑）**：`ninja -n` no work、记录器 **22/22**、成本测量 **COST_VERDICT=PASS (38/38)**、
  生命周期 **187/187**、解析器静态 **71/71**、往返 **1067 项 0 失败 / A..F 全满足**、meson **81/81**、静态门禁 EXIT 0、全量静态 **247/247**；
  DLL **36,272,145 B / `02DA41E3D77D13AA28C5492DD37D7DA6363605EA129F212CEA13166865EB389D`**。
- **仍未做**：D 点真实载荷交接（`draw.inputSkinSelection` 只在 `InputsEnabled()` 下赋值 ⇒ 正常录制里 D 的来源/帧标签不可得；
  计划加"独立小型按值诊断载荷"，主线程已确认 S 点用的是局部 `selectedPalette` 无此问题）、冻结格式显式版本化与窗口/Reset 分段；之后才是单独申请 pair-0。
- **测试现场仍是已交付的冻结测试版** `D1C3EBBB…`（36,253,553 B）；仓库主线已前进到 `02DA41E3…`（差异都在**默认关闭**的诊断路径上）。

## 2026-09-17 — 并发协议收尾：递归锁消除重入死锁（含反向证据）、场景 E/F 生产级证据、序号严格性（未提交 / 未晋升稳定）

- **解除一个真实结构性死锁（子代理发现，主线程修，随后补反向证据）**：palette 发射器在**持记录器状态锁**的路径内回调
  `Record()`；若该次 append 触发证据环**容量冻结**，环会调用预冻结钩子 → `ClosePaletteObjectWindow()` → **同线程再次进入记录器**。
  非递归 `std::mutex` 在「小 capacity + 大 postPresents + 该时刻恰有 palette 观测」下会**自死锁**。
  现改为 `std::recursive_mutex` 并写明原因；嵌套结算照常进行，嵌套 append 因环已 Frozen 返回 0 并计入 `ringEvictedAfterRecord`（fail-visible）。
- **反向证据（必须真跑）**：宿主机新增 Case20（发射器回调内同线程重入 `CloseWindow`，看门狗 10 s）与 Case21
  （真实生产 Ring：arm(256) + postPresents=100，恰好让一次 palette 观测的 append 触发 `Reason::Capacity` 冻结 → 钩子 → 同线程重入）；
  实测不挂死且失败可见：`hookRuns=1 / hookEmitDepth=1 / ringReason=3(Capacity) / ringEvictedAfterRecord=3`。
  **变异 M41**（把生产头改回 `std::mutex`）被两条用例**各自单独杀死**（看门狗 10 s 触发、进程 `_Exit(2)`）；还原后逐字节一致并复跑 21/21。
  平台实测前提：本工具链 i686 MinGW 的 `std::mutex` 同线程二次 lock 真死锁（20 行独立探针验证）。
- **场景 E（并发所有者协议）**：4 写者线程 × 100 键 × 4 阶段（1600 次并发 `Note*`）+ 400 次与 `Control(freeze)` **真正竞争**的调用；
  断言恒等式全部成立（`emitted(2037) == 导出事件 2037`、`terminalEmitted==导出终态 409`、`sum(closed*)==409`、全部 `dropped*==0`、
  冻结后补发观测**逐字段不改变**头块），连跑 7 次全绿。措辞上限：**已用锁串行化全部状态访问 + 并发用例通过**，不写「竞争已消除」。
- **场景 F（自动冻结路径，上级 3a）**：走真实控制路径 `trigger(postPresents=8)` + 生产 Present 事件让环**自动**冻结（导出 `reason==2` 证明**没有**走 `Control(freeze)`），
  导出里**含终态**、解析器结论 `Recovered + sameObjectCertified`；变异 M39（去掉 arm 处钩子安装）被场景 F 与静态门禁同时杀死。
- **解析器序号严格性（上级亲验反例：全链序号=1 仍被认证）**：改为「链内必须恰好 1,2,3,…」——相等/回退 → `chainSequenceNotStrictlyIncreasing`（issue，阻断认证）；
  跳号/缺头 → `chainSequenceGap`（缺口 ⇒ Uncovered）。四个反例修前/修后都实测（修前全链重复仍 `chainComplete=true / CLI 0`，修后 `CLI 2`），
  合法严格递增与两条链各自 1..N 仍认证（防过度收紧）。解析器静态 **61 → 69 例**。
- **门禁（主线程 2026-09-17 17:28 独立复跑）**：`ninja -n` no work、记录器 **21/21**（新增两条重入/容量用例）、生命周期 **187/187**、
  解析器静态 **69/69**、往返 **1060 项 0 失败 / A..F 全满足 / CERTIFIED_SPEC_SATISFIED**、meson **80/80**、静态门禁 EXIT 0、全量静态 **247/247**。
- **两套身份（务必区分）**：
  - 已交付到测试现场的**冻结测试版**：`E:\Work\War3\d3d9.dll` = **36,253,553 B / `D1C3EBBB…391AAF6`**（用户回来测试的就是这一份）；
  - 仓库主线已推进到 **36,255,186 B / `98ED33CD…E56862`**（并发协议 + 递归锁 + 预冻结钩子；这些都在**默认关闭**的诊断路径上，不影响普通游玩）。
  - 本树 PE 时间戳导致重链接后 SHA 变化（同源码不同 SHA）⇒ 身份按「本次构建」计，源码恒等性用文件 SHA-256 证明。
- **仍未做**：去重载荷口径（D6 拆分：仅"已证明等价"的重复可压缩）、D 真实载荷交接（`inputSkinSelection` 仅在 `InputsEnabled()` 下赋值）、
  冻结格式显式版本化与窗口/Reset 分段、④ 成本测量；之后才是单独申请 pair-0。

## 2026-09-17 — 交付可测试构建版本（现场部署 + 一键回退）；按上级最后一轮裁定启动并发/关闭协议与自动冻结修复（未提交 / 未晋升稳定）

- **上级额度用尽前最后一轮裁定**（已收到并记录）：①认可 round 9 往返测试，但**不批准进入实机**；
  ②S1 并发**必须修**（否决「加 owner 断言即可」），条件批准「单一状态所有者」或**更简单的同步替代方案**，
  但不得为「热路径不加锁」保留数据竞争；③`droppedDuplicatePerFrame` 一律不算损失**不批准**（同键同帧但载荷冲突必须计数）；
  ④非观察对象的 S/D 忽略**批准保留**（拒绝驱动样本，报告必须给分母）；⑤自动 post-window 冻结路径未被四场景覆盖；
  ⑥解析器序号只查 `<` 不查相等（上级亲验：全链序号=1 仍 `chainComplete=true`）；⑦`data[2]` 承载 lifecycleIdentity **批准**，
  但冻结格式必须显式版本化；⑧`ResetForSessionTransition` 保留计数方向正确，但清表≠分段，需可识别的窗口/Reset 分段。
  裁定顺序：并发所有者与关闭协议 → 自动触发与真实载荷交接 → 去重/序号/版本/Reset 反例 → 成本测量 → 冻结后**单独**申请 pair-0。
- **交付测试构建（用户直接要求）**：`build32/src/d3d9/d3d9.dll` **36,253,553 B / `D1C3EBBB…391AAF6`**，
  连同 `README-测试版.md`、干净模式/取证模式启动脚本、一键回退脚本部署到**测试现场** `E:\Work\War3`；
  现场原 DLL **35,486,415 B / `74CC676B…`** 已按既有命名规范备份为 `d3d9.dll.bak_20260917_74CC676B_pre_testbuild`，
  并在 `E:\Work\War3-test-build-20260917\rollback\` 另存一份。**玩家目录 `E:\Work\Warcraft III` 未被触碰。**
  包内明确写清：这是**测试版不是稳定版**、本哈希**没有任何实机运行**、上一实机组合候选是 `8CECC495…`（不继承结论）、
  默认开启的行为变更（native 帧同步读回跳过/异步截图/编译期取证/严格蒙皮合同/S2 内核/Gap B/path-blocker/构建所有权门）与
  **不要开启** `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1`（该路径正因并发问题修复中）。
- **S1 并发协议（生产代码）**：记录器新增 `StateLock`（不可重入 `std::mutex`）串行化**全部**状态访问
  （Configure/Reset/ResetForSessionTransition/CloseWindow/五个 Note*/读取器/SnapshotCounters）；
  发射器回调在持锁路径内，故 `NoteRingEviction` 改为**唯一无锁字段**（原子累加，由 `counters()`/`SnapshotCounters()` 折叠），
  避免自死锁。关闭协议因此变为：**持锁停止接纳 → 持锁结算终态 → 上层随后才冻结证据环**。
  导出头块改用 `SnapshotCounters()` 单次持锁取得一致快照（计数 + 观察数）。
- **自动冻结路径（上级 3a）**：证据环新增 `PreFreezeHook`，在**任何**冻结原因（post-window / 容量 / 序列回绕 / 请求）
  置 Frozen **之前**调用；arm 时安装 `PaletteObjectPreFreezeHook`、discard 时清除；带重入保护。
  这样热键触发的 post-window 自动冻结也会先结算终态（此前只有手动 `Control(freeze)` 路径被修好）。
- **仍在进行**：并发场景 E / 自动冻结场景 F 的生产级证据 + 变异验证；解析器序号严格递增（另一子任务）；
  去重载荷口径（D6 拆分）、载荷交接（D 来源在 `InputsEnabled()` 之外不可得）、冻结格式版本化与窗口分段；之后才是成本测量。

## 2026-09-17 — 只读对抗审计（3 个子线程并行）发现并修复 D1/D7/D9 + F4；解析器补恒等式与认证谓词（D3/D4/D5/D8）（未部署 / 未实机）

- **并行化（用户要求）**：本轮按 ≤4 线程组织 —— 同一时刻只有一个**构建拥有者**（共享 build32，两个构建进程会互相破坏，
  且门禁要求 Below Normal + `-j2` 串行），其余槽位放**不构建**的只读审计与 Python 工作。
- **D1（审计发现的最重缺陷，已修）**：表满时该对象**没有条目**，原实现**每次拒绝都发一条 TableFull 终态**
  ⇒ 同一 key 两条终态 ⇒ 生产解析器对**整份导出**抛 ValueError（连其他对象的链一起不可读）。
  生产从不调用 `NoteObjectGone`、只有 CloseWindow/Reset 清表 ⇒ 表满后任何持续被拒对象**连续两帧**即触发。
  现改为**每窗口只公告一次** TableFull（表满是会话级条件），其余只累加 `droppedTableFull`（仍是 LOSS 字段）。
- **D7（已修）**：`Entry{}` 的帧标记默认 0 与「帧 0」无法区分 ⇒ 进程首帧的首条观测会被同帧去重吞掉且**不落任何计数**。
  现用哨兵 `kNoFrame = ~0ull`。
- **D9（已修）**：`ResetPaletteObjectEvidence`（换图）与 `ClearPaletteObjectWatchlist`（设备代际）会在 freeze→export 窗口内清零会话计数
  ⇒ 导出头块归零。现将 `Reset`（新会话，连计数清）与 `ResetForSessionTransition`（换图/设备代际，只清表**保留计数**）拆开。
- **F4（审计发现的 F2 只关了一半，已修）**：`TableFull`/`ObjectGone` 两条终态路径原先仍在预算门**之前**递增 `closed*`
  ⇒ 被预留拒绝、从未发出的终态也被计入。现与 CloseWindow 对齐：**先 `CanEmit(true)` 再递增**。
  实测见证：`tableFull-calls=513 terminalEmitted=1 closedTableFull=1 droppedTerminalReserve=0`。
- **解析器侧（D3/D4/D5/D8，已修，静态测试 44 → 61 例）**：
  1) **头块恒等式**：`terminalEmitted == 导出终态条数`、`sum(closed*) == terminalEmitted`、**逐种类** `closed* == 该种类终态条数`、
     `watchCount <= 1024`（超界 ValueError）；任一不符 ⇒ 未覆盖/未认证（修前这些反例全部被判 Recovered）；
  2) **认证谓词对齐记录器 `closedChain`**：新增具名拒绝 `drawSourceMissing` / `selectionClearedAtDraw` /
     `liveDrawnWithoutSubmit` / `unknownFrameDomainWithValue`（修前这些自相矛盾的 wire 都被认证）；
  3) **会话镜像校验**：`bits[10]/[29]` 必须等于 `event.session`（修前不校验；旧静态夹具本身违反此约束，正说明从未被检查）；
  4) docstring 统一到当前实现（`data[2]=lifecycleIdentity`、`data[3..8]` 保留、逐位表 = 转换点实际写入集合）。
- **审计卫生点（已修）**：往返测试夹具原先用 `source=ProducerSnapshot`，而生产 S 点**不可能**给出该值
  ⇒ source 通道不是端到端覆盖。现夹具改为 `ArenaSlot`，并新增 `check_source_reachability()`：**从生产源码推导**可达来源集合，
  要求 Python 常量与 **C++ 夹具实参**都 ⊆ 可达集合且逐阶段一致（无法再靠改常量静默）。
- **可靠性教训**：`closed*`/`emitted` 这类"读方依赖的计数"必须**只看真的发出的东西**；
  「表满」是会话级条件，不能逐对象重复公告；「从未观测」必须与「帧 0」区分；换图重置不能抹掉待导出的会话计数。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、
  记录器 **19/19**、生命周期 **187/187**、解析器静态 **61/61**、**生产往返测试 961 项 0 失败 / `CERTIFIED_SPEC_SATISFIED`**、
  meson **80/80**、静态门禁 EXIT 0、全量静态 **247/247**；
  DLL **36,253,553 B / `D1C3EBBDFF184AEB908F9A97DB4F9E8EBBA39BCCE96B357ED0CC69502391AAF6`**（未部署/未实机/未提交）。
- **仍未做**：④ 离线容量/淘汰/分配/子门开关成本测量（下一步）；实机 pair-0（需上级单独授权）；
  审计提出的 S1（记录器无锁并发，**未能证伪也未证明安全**）、D6（非观察对象的忽略与 `droppedDuplicatePerFrame` 口径，待上级裁定是否计入缺失）。

## 2026-09-17 — 生产往返测试首次跑通并暴露 4 个真实接线缺口（G1–G4）+ 3 个结构性发现（F1–F3）全部修复（未部署 / 未实机）

- **新增生产往返测试**（真实链路，无复制品）：`war3_palette_object_wire_roundtrip_test.cpp`（C++ 驱动，A/B/C/D 四场景各自
  arm→记录→freeze→export）+ `AutoTest/test_palette_object_wire_roundtrip.py`（编排 + 把**真实导出文件**喂**生产解析器**），
  登记进 `meson test`。它**立刻暴露了生产导出与生产解析器不兼容**——这类缺口静态门禁与宿主机单测都看不见。
- **G1（最硬）**：导出头块把 `watchCount` 写成**字符串**而读方要求 JSON 整数 ⇒ **每一个真实导出都被解析器拒收**（exit 1）。已修为整数。
- **G2**：`DisarmPaletteObjectEvidence()` 原先在 `freeze`（离开窗口）时就调用，而 `QueryPaletteObjectEvidenceHeader()` 未 armed 时恒返 0
  ⇒ 每次真实导出的计数块**恒为 0**（连带令 `ringEvictedAfterRecord` 不可见）。现 disarm 只在 `discard` 分支；
  记录器另加 `m_windowClosed`（`CloseWindow` 置位、`Reset` 清位、五个 Note* 早退）以防 freeze 后计数漂移。
- **G3**：`deltaFrames` 与冻结读方契约不符（读方要求：R/Served/**终态** = renderFrame-firstRejectFrame，**live Enqueued/Drawn = 0**）。已按契约修正。
- **G4（最致命）**：`Control(freeze)` 原先**先 `ring.finish()` 再结算终态**，环已 Frozen ⇒ 终态 append 返回 0 且**不落任何丢失计数**
  ⇒ 导出里永远没有终态 ⇒ 结论永远不可能是 Recovered。现改为**先 `ClosePaletteObjectWindow()` 再 `ring.finish()`**；
  发射器在 `Record(...) == 0` 时 `NoteRingEviction(1)`，使「未能落环」**至少落一个可见计数**。
- **F1（结构性）**：G2 的幂等窗口使终态只能由同一次 CloseWindow 发出，而终态原先与普通事件共用每帧 64 预算
  ⇒ 单窗口终态 ≤64，**512 预留与 4096 总额都不可达**、预留记账分支成为死代码。现**终态不受每帧上限约束**（由 512 预留约束）：
  实测 1024 条目 ⇒ `terminalEmitted=512 / droppedTerminalReserve=512 / droppedPerFrame=0`，两条记账分支均可达。
- **F2**：`CloseWindow` 的 `closed*` 桶原先在预算门**之前**递增（可远大于实际终态数）⇒ 改为**只在终态真的发出时**递增。
- **F3**：`ObjectGone` / `TableFull` 两条终态路径原先靠结构体默认值凑 delta ⇒ 现**显式**写契约值。
- **修复后往返测试判定**（真实生产路径）：A `Recovered` + `sameObjectCertified=true`；B `StageCompleteUncertified`（identityWeak 拒绝）；
  C `Uncovered` + `objectLevelEvidenceDropped`；D `emptySample` + `noObjectEvidenceExported`；`ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED`。
- **并行化调整（用户指出）**：此前把「只能一个构建」错误推广成「只能一个子线程」。现按 ≤4 线程组织：
  **同一时刻只有一个构建拥有者**（共享 build32，两个构建进程会互相破坏，且门禁要求 Below Normal + `-j2` 串行），
  其余槽位只放**不构建**的工作（Python 解析/门禁、只读对抗审计、身份清单、文档）。
- **门禁**：构建 exit 0、`ninja -n` no work、记录器测试、往返测试（meson）、`meson test` 全过、静态门禁 EXIT 0、全量静态通过；
  DLL 身份见 `docs/plan/2026-09-17-candidate-freeze-record.md`（本轮结束时统一记录）。

## 2026-09-17 — 按上级 03:58 反例审查修正：分析器四项误通过、有界表探测/墓碑、实际来源与恢复判定、预算/序号口径、生命周期终态权限（未部署 / 未实机）

- **明确撤销误报**：此前报送的「meson 77/77」是在我改坏 `test_frame_recorder_defaults.cpp` 断言、且**未运行该用例**的情况下给出的，
  该结论**已作废**，也不由后来的 78/79 追认。教训：构建通过 ≠ 用例通过。
- **分析器四项"实际误通过"已封堵**（上级给出反例：空样本 chainComplete=true、弱身份/未知代际仍判 Recovered、事件 session 与根 session 不一致仍判 Recovered）：
  - 零事件 ⇒ 只能"未覆盖"（`coverageComplete=bool(chains) and …`，禁止空集合 `all()` 通过），并给 `noObjectEvidenceExported`；
  - `identityWeak` / `epochUnknown` ⇒ 允许"阶段观察完整"（`stageObservationComplete`），但**不得认证同对象恢复**
    （`sameObjectCertified=false` + `certificationRefusals`，且不进 `recovered`）；
  - **真正复用**严格根校验（投影掉 `paletteObject` 块后调用既有 `analyze_frame_evidence.analyze()`）：session 一致性、序号严格递增唯一、
    accepted/evicted 与导出条数记账、producerLosses、trigger/reserved 等，任一不符即拒绝或"未覆盖"。
  解析器静态测试 23 → **44 例**全过。
- **有界表两处真实缺陷已修**：`Find` 原先只探 8 槽而 `Insert` 可探 1024 槽（能插到永远查不到的位置）；`NoteObjectGone` 清空槽而 `Find` 遇空槽停止（删前项使后项不可达）。
  现 `Find/Insert` 探测范围一致并使用**墓碑**跨过删除空洞；新增"连续碰撞 / 删前项 / 查后项 / 重复插入"生产组件测试。
- **记录器语义修正**（六处，均为我此前设计缺陷）：
  1) **吞首条拒绝**：`Insert` 预写 `lastRejectFrame` 导致新建对象的**第一条 Rejected 被同帧去重吞掉**（会使分析器永远 Unclosed）——已修；
  2) 阶段事件把调用点**已知的** `manifestUnknown/nativeUnknown` 强制改写成 unknown（等于丢弃已知帧域）——改为保留调用点口径；
  3) **终态真实预留**：`kNormalBudget = 4096 - 512 = 3584`，普通事件不得吃满总额（此前 512 只是终态上限而非预留）；
  4) `CloseWindow` 不再清零每帧预算（否则同帧继续记录可绕过 64 条上限）；
  5) **先预检、再改状态、再发射**（`CanEmit/AccountDrop/EmitUnchecked`）：预算被拒时 hitCount/chainSequence/saw* 一律不动；
  6) `chainSequence` **真的在每次发事件时递增**（我此前"已修恒零"的声明是错的，已纠正）；`Recovered` 收紧为
     `sawServed && sawSubmit && sawDraw && !orderViolation && !drawSelectionCleared && drawSource != None`。
- **实际来源按值**：`NoteEnqueued/NoteDrawn` 新增 `source/hitKey` 形参，S/D 采集点传入**该次候选 / 该次 draw 实际携带的按值来源**
  （不再从最近一次 Served 推测）；来源映射与 hitKey 规则抽到 `war3_palette_object_capture.h` 共用。
- **`lifecycleIdentity` 贯通 wire**：发射器写 `data[2]`，解析器声明 `LIFECYCLE_IDENTITY_SLOT=('data',2)`（`data[3..8]` 仍为必须为 0 的保留位），
  分组键为八元组 ⇒ 仅生命周期身份不同的两个实例会分成两条链（此前 wire 没有槽位，读方只能报 `wireGaps`）。
- **生命周期终态权限**（上级 03:30/03:58）：`isCurrent()` 现在还要求 `building()`（完成/取消后不得再当"当前工作"回写）；
  底层 `publish/complete/cancel` 转 **private**，公开入口只剩 `*IfCurrent`；运行时陈旧预览路径改用 `cancelIfCurrent`；
  删除 C8 里过期的"覆盖面不弱于一秒空转"注释。组件头曾因我同时保留 public/private 两份声明而无法编译（下级发现并修）。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、记录器 **19/19**、生命周期 **187/187**、
  解析器静态 **44/44**、静态门禁 EXIT 0、meson **79/79**、全量静态 **247/247**；
  DLL **36,253,304 B / `44D4B968BC71AB41381D46F2B319558346367177B92E346EB37E07689B8F6DD5`**（未部署/未实机/未提交）。
- **仍未做**：④ 离线容量/淘汰/分配/开关成本测量；**生产转换器 ↔ 解析器的往返测试**；C（剔除）仍不发对象级事件（记录器只有四阶段，
  发 `NoteDrawn` 会把"未记录绘制命令"写成 `sawDraw=true`）；`analyze_frame_evidence.py` 的根字段集仍是精确相等且不含 `paletteObject`，
  故 palette 读方用"投影"复用其严格校验（是否长期给该读方加键，待上级裁定）。

## 2026-09-17 — 记录器测试按主线程两处有意修正同步期望 + 记录器门禁收紧与 M11–M26 变异验证（未部署 / 未实机）

- 性质：**只改测试与静态门禁**，未改任何产品代码。产品头 `src/d3d9/war3/tools/war3_palette_object_evidence.h`
  的 SHA-256 在整轮变异前后均为 `9FBABCC5A9F693B987369E7F1B9ABD03418E000C60E7ABCB91106414E782C1CF`
  （变异驱动写回后按原 SHA-256 校验还原）；解析器 `AutoTest/analyze_palette_object_evidence.py` 仍为
  `092206749FC61CBF10103372CCF758DE2BE5255488AFC558578E2398833E108F`（未被回退，44 例仍通过）。
- 同步的两处产品侧有意修正（`src/d3d9/war3/render/tests/war3_palette_object_evidence_test.cpp` 的期望随之更新）：
  1) **修正 A**：`Insert` 不再预写 `Entry::lastRejectFrame` ⇒ 建条目那次调用**会发射**对象的第一条
     Rejected 事件并占用 `chainSequence` 1；同一帧第二次 Reject 仍计入 `droppedDuplicatePerFrame`。
     逐条更新：Case1 `emitted == 3`（原 2）；Case2 链序号改为**相对建条目事件**的 +1…+5（原写死 1..5）且
     collector 内 Rejected 计数 `== 2`（原 1）；Case3 第二条 Served 序号 3→**4**；Case8 新增"建条目必须
     发射第一条 Rejected 且不计同帧重复"见证；Case14 同帧新对象被拒的 `droppedPerFrame` 增量
     `kKeys+1`→**`kKeys+2`**（新建 Reject 与随后 Served 各一条，两处断言）；Case15(a) 序号期望改为
     相对条目事件的 +1/+2（原写死 1→2）。
  2) **修正 B**：`PrepareStage`/`NoteReject`/`CloseWindow` 不再把 `manifestUnknown/nativeUnknown`
     强制写成 true，改为**保留调用点口径**。Case2/Case12 的"强制 unknown (fail-visible)"断言改为
     "保留调用点口径"，帧域断言由"只比数值"的弱比较收紧为 `SameFrames`（数值 + unknown 标记逐项相等），
     并保留"帧域数值原样保留"、新增"未知零 vs 已知零可分辨"（同数值、不同标记）。
- 静态门禁 `AutoTest/test_semantic_build_thread_gate_static.py` 收紧（新增 §11g–§11j，**只加不减**）：
  Insert 不得写 `lastRejectFrame`、仍须写 `firstRejectFrame`；NoteReject 的 `lastRejectFrame` 写入必须晚于
  `CanEmit`；NoteReject/PrepareStage/CloseWindow/NoteObjectGone 不得把帧域强制写成 unknown；
  PrepareStage/NoteReject 必须 `record.frames = frames;`；NoteReject 必须推进 `chainSequence` 并保留同帧去重；
  宿主机测试必须正面包含修正 A/B 的见证文本且旧口径文字不得残留；`lifecycleIdentity` 必须是
  "发射器写 `event.data[2]` + 解析器整行声明 `LIFECYCLE_IDENTITY_SLOT=('data',2)`"。
- 变异验证 **M11–M26 共 16 条全部 KILLED**：M11 Insert 写回 `lastRejectFrame`、M12 NoteReject 强制 unknown、
  M13 PrepareStage 强制 unknown、M14 CloseWindow 强制 unknown、M15 阶段沿用条目首次帧、M16 去掉 3584 上限、
  M17 hitCount 提前于预检、M18 不推进 chainSequence、M19 去掉同帧去重、M20 不记 `firstRejectFrame`、
  M21 不记 `lastRejectFrame`、M22 `lastRejectFrame` 提前到预检前、M23 测试删掉"known 保持 known"见证、
  M24 测试写回"强制 unknown"字样、M25 发射器不写 `data[2]`、M26 解析器槽位改 None。除 M22（只有静态门禁
  杀死，宿主机用例不覆盖）与 M23/M24/M25/M26（文本/线格式，只由门禁杀死）外，其余同时被宿主机用例杀死
  （原始输出：仓库根 `mutation_m11_m19.log`、`mutation_m18_m26.log`）。**M26 首版存活**：§11i 原用子串
  匹配时 `PROPOSED_LIFECYCLE_IDENTITY_SLOT=('data',2)` 令断言恒真，已改为
  `^LIFECYCLE_IDENTITY_SLOT=\('data',2\)$` 多行整行匹配后杀死。
- 门禁（原始结果，**Below Normal + 最多 `-j2`**）：`ninja -C build32 -j2` **exit 0**（含 `d3d9.dll` 链接）；
  `ninja -C build32 -n` **no work to do**；记录器宿主机测试 **19/19 EXIT=0**；生命周期宿主机测试
  **187/187 EXIT=0**；`meson test -C build32 --num-processes 2` **Ok 79 / Fail 0，exit 0**；静态门禁 **EXIT 0**；
  全量 `test_*_static.py` **247/247**。
- 本轮 `build32` 产物 d3d9.dll **36,253,304 bytes / SHA-256
  `44D4B968BC71AB41381D46F2B319558346367177B92E346EB37E07689B8F6DD5`**。产品源未改，但变异驱动以
  **同内容**重写产品头（mtime 变化触发依赖重编）而重新链接，故该哈希与更早 changelog 记录的 DLL 哈希
  不可直接比较。
- 未做 / 不确定：M11–M19 的**原始**编号未在仓库内落盘（仓库里只有另一门禁的 M1–M10），本轮把记录器变异
  按 M11–M26 重新枚举并写入门禁 docstring 以免再次丢失；未运行 AutoTest/Win32 runnable 与任何实机门；
  未部署、未提交。

## 2026-09-17 — 线程修复方案 B 第一部分：非主循环线程不再推进构建（未部署 / 未实机）

- 性质：按上级（codex 01a02e0b）裁定与 `docs/plan/2026-09-17-thread-fix-implementation-design.md`
  「修复 1」实施——**只做"非主循环线程不得推进构建"这一点**。
  **修复 2（安全进度快照发布）本轮未做**：构建进度字段仍在锁外更新、控制面快照仍在锁内读同一份
  live 字段，进度字段竞争依旧存在。措辞上限为「非主循环线程不再推进构建」，
  **不得**据此宣称竞争已消除或线程问题已闭合。
- 改动（推进边界，唯一真实入口 `ShadowValidationRuntime::ensureLatestFrameBuilt()`）：
  在 `requestLatestFrameBuild();` 之后、任何加锁 / 共享状态读取 / 推进调用之前加入线程门，
  使用 `dxvk::war3::hooks::GetMainLoopThreadId()` 与 `::GetCurrentThreadId()`：
  主循环 id 尚未建立（==0，早于首个主循环观测）时放行并单独自增
  `g_semanticBuildAdvanceBeforeMainLoopCount`；否则当前线程 != 主循环 id 时**先自增
  `g_semanticBuildOffThreadRefusedCount` 再 return**，只保留请求语义。门在推进边界，
  因此同时覆盖 `allowControlPlaneSemanticDrain` 与 `IsHotSemanticBuildWaitPayload` 两条门控
  以及未来任何新增调用者。**未改** palette 判定、drain 门控逻辑、payload 开关、默认值/env。
- 计数与导出：两个计数器为 TU 内 `std::atomic<uint64_t>`（与 A5/palette 槽位缓存拒绝计数
  **不同分母，不混算**）；新增访问器 `QuerySemanticBuildOffThreadRefusedCount()` /
  `QuerySemanticBuildAdvanceBeforeMainLoopCount()`。**只导出** `semanticBuildOffThreadRefusedCount`
  （7 处：bridge.h/.cpp、hub.h/.cpp×2、control_plane.cpp、perf_monitor.cpp 的 `json << ` 写手），
  pre-main-loop 计数不进导出链。
- 新增静态门禁 `AutoTest/test_semantic_build_thread_gate_static.py`：钉死门在函数内
  `requestLatestFrameBuild();` 之后且先于首次 `std::unique_lock`/`m_mutex` 与推进调用
  （全文 `m_core.buildFrameChunk(` 之前）；非主循环分支先自增再 return；两计数器必须是
  `std::atomic` 而非普通可变全局；七个导出点齐备；禁止肯定式宣称；回归护栏
  （`requestLatestFrameBuild();` 仍为函数首行、`drainPendingBuildForControlPlane` /
  `runObserveValidation` 调用者未改）。
- 门禁（原始结果）：`ninja -C build32 -j4` **exit 0**；`ninja -C build32 -n` **no work to do**；
  `meson test -C build32 --num-processes 4` **74/74 OK, Fail 0**；
  全量 `test_*_static.py` **245/245**（基线 244 + 新增 1）；
  新门禁单跑 `semantic build thread gate static checks passed`（exit 0）。
- 本轮 `build32` 产物 d3d9.dll **36,004,514 bytes / SHA-256
  `D286508BFFC178C5572BBE267BC55D402A017D4A91C538C9B28C94C4A454450C`**（含工作树其它未提交改动）。
  **未部署、未实机、未提交**。
- 未做 / 不确定：修复 2（进度快照发布）未做；修复 3（drain 返回具名拒绝原因）未做；
  AutoTest MCP 的 `allowControlPlaneSemanticDrain: True` **未改**（本题明确不动 AutoTest MCP，
  且改开关本来也只算卫生措施）；没有实机验证「管道线程 drain 被拒且 buildCurrentRecordIndex
  不前进」，该运行时验收仍待独立轮次；主循环 id == 0 窗口内的放行次数只在稳态下要求不再增长，
  尚未实机采样。

## 2026-09-17 — S2 追加：可复现随机差分工具 + 适配层派生/成本实测（未部署 / 未实机）

- 性质：按上级 S2 复核要求补齐上一轮被删除的差分程序（"30 万次零差异不可复现"）并补成本口径。
  **不改内核语义**：core.cpp / upper_layer_shadow.cpp / 内核头三处产品源码只加注释。
  数值等价只作为进展，**不据此宣称热路径成本等价**。
- 交付：
  1. 新测试 `src/d3d9/war3/render/tests/war3_runtime_group_palette_kernel_diff_test.cpp`
     （2,379 行 / 102,761 bytes）：内含从 pre-S2 副本逐语句抽取的旧实现（独立 `legacy`
     命名空间：5 步 core + 4 步 upper）、固定种子 SplitMix64（core=0x5332464600000001 /
     upper=0x5332464600000002，每组 300,000 次）、逐输入比较
     ok / usesAveraging / maxVertexGroupSlot / palette.size() / 逐元素 bitwise /
     失败 miss reason / 残留 palette，并打印摘要（种子、两组输入数、各自 mismatch、副本 SHA-256）。
     反例打印 + 非零退出；另含旧实现副本 SHA-256 与派生边界的显式夹具。
  2. 旧实现身份（pre-S2 副本，权威来源）记录且每次运行输出：
     `C:\Windows\Temp\warvk_s2_backup\war3_shadow_renderer_core.cpp` SHA-256 `0F7B…D8F9`；
     `war3_upper_layer_shadow.cpp` SHA-256 `61EB…17A9`。
  3. 适配层覆盖方式：两个生产 `TryBuildRuntimeGroupPalette` 依赖 d3d9 记录类型与 TU 内部状态，
     无法在宿主机链接，故用"派生建模（core 的 RuntimeVertexGroupSlotCount min 链 + 128Ki clamp、
     groupCount=matrixGroupSizes.size()；upper 的 min(vertexGroupCount,size) 与
     min(matrixGroupCount,size)）+ 三条前置校验 + 内核差分"覆盖，适配层文本一致性由静态门禁负责。
  4. `src/d3d9/meson.build` 新增 `war3_runtime_group_palette_kernel_diff_test` + `test(...)`；
     `AutoTest/test_runtime_group_palette_kernel_single_source_static.py` 新增差分目标存在、种子常量、
     旧实现副本身份（存在时逐一复核 SHA-256）、摘要字段、miss 枚举顺序一致、槽位扫描趟数结构等断言。
- 等价性结果（本次实测）：core 300,010 输入 + upper 300,005 输入 **0 mismatch**，exit 0；
  随机输入确实穿过三条前置校验、4/5 步 fallback 的每个出口与 usesAveraging / maxSlot=255 边界。
- 成本实测（同一次运行输出）：
  * 槽位扫描趟数（源码结构 + 门禁断言）：core pre-S2 2 趟 → S2 **4 趟**（适配层 maxSlot
    6643-6647 + 适配层去重 6770-6779 + 内核 maxSlot 99-103 + 内核去重 107-113）；upper 2 趟不变。
    **修正上级预估的 3 趟**：抽取点只覆盖 fallback 链/group 表/等权平均，适配层 6770-6779 的去重块
    保留了下来。该趟结果只被 `logFailure` 消费，而 `logFailure` 仅在 reason==FallbacksFailed 时调用，
    5 步集合在三条前置校验通过后不可能 FallbacksFailed（uniform-root 广播恒成功）。
  * 结构性推论：core 的第三条前置校验 `NoVertexGroups` 与 5 步 `FallbacksFailed` 都是死分支
    （前者被 `hasSkinningData()` 挡掉，后者被 uniform-root 广播挡掉），所以 core 的 `logFailure`
    采样同样不可达；差分测试用 `== 0` 断言把这些可达性事实钉住。
  * 每次调用分配次数（测试 TU 内重载全局 operator new/delete 实测；典型夹具 48 槽位 / 8 个单元素
    group / 8×8 矩阵）：旧实现复用调用方输出对象时第 2 次 **2 次分配、0 次 palette 分配、
    缓冲区指针与容量不变**；S2 core 适配层+内核 **4 次分配**（多出被丢弃的 512 B palette 新分配，
    旧缓冲区被 move 释放，实测指针改变）；S2 upper **3 次**（2→3）。即"适配层每次新建局部内核输出
    再 move 给调用方"丢掉了旧实现的调用方 vector 容量复用保证。
- 门禁（原始结果）：`ninja -C build32 -j4` **exit 0**；`ninja -C build32 -n` **no work to do**；
  `war3_runtime_group_palette_kernel_diff_test.exe` **exit 0**（打印 SUMMARY）；
  `meson test -C build32` **74/74 OK, Fail 0**（基线 73 + 新增 1）；
  `py AutoTest/test_runtime_group_palette_kernel_single_source_static.py` **exit 0**；
  全量 `test_*_static.py` **244/244**。
- 本轮 `build32` 产物 d3d9.dll 36,000,018 bytes / SHA-256
  `5B6EBBF94F8A6C0340C255D02615F11F5610E08B0A7A6B9AEDC41CFA1D7FD861`（含工作树其它未提交改动；
  本轮对产品源码的改动为纯注释，不构成代码等价证明）。**未部署、未实机**。
- 未覆盖 / 不确定：旧实现的来源链（producer 快照 / +0x08 槽位 / 记忆槽位 / Game.dll arena）
  与日志面在本 TU 不可复现；时间比例测量（kernel/single≈6.2、2 趟参考≈4.0）只作佐证、不是趟数；
  分配计数不含 CRT / Vulkan / 驱动侧；未测真实热路径帧时间。

## 2026-09-17 — S2 收窄版实施：共享纯计算内核 + 薄适配层（未部署 / 未实机）

- 性质：按上级 Q5 裁定（**只共享纯计算内核 + 薄适配层 + 生产函数级测试，不合并来源选择链**）
  与设计 `docs/plan/2026-09-17-s2-shared-compute-kernel-design.md` 落地。只做纯计算等价抽取：
  来源顺序、失败语义、坐标空间、有效期、stats/日志面均不变。
- 交付：
  1. 新增 header-only 内核 `src/d3d9/war3/render/war3_runtime_group_palette_kernel.h`（311 行）：
     `RuntimeGroupPaletteInput / SlotScan / FallbackSet / Output / KernelDetail` +
     `ScanRuntimeGroupPaletteSlots` + `TryBuildRuntimeGroupPaletteKernel`。硬规则：上界全部来自
     Input、内核内部 0 处 `min()` 收窄、0 处内存读取、无 renderablePart/slot/arena/producer/Selection；
     4 步 vs 5 步由 `RuntimeGroupPaletteFallbackSet` 表达，短路顺序逐句复刻。
  2. `war3_shadow_renderer_core.cpp`（9865 → 9777 行）与 `war3_upper_layer_shadow.cpp`（455 → 357 行）
     改为薄适配层：`tryEngineDirectPosePalette`（D1 来源链）、`logFailure`（D3 诊断面，一行未改）、
     `RuntimeVertexGroupSlotCount` 与两侧 groupCount 派生（D4/D5）全部留在适配层；两个
     `TryBuildRuntimeGroupPalette` 签名、6+1 处调用点、miss reason 与写回语义逐字未变。
  3. 新测试 `war3/render/tests/war3_runtime_group_palette_kernel_test.cpp`（907 行，T1-T13）：
     等权平均逐位、单元素组不除、`usesAveraging` 门、稀疏路径零填充、槽位扫描/去重前缀语义、
     7 条失败 reason、D2 集合差异、D4/D5 派生差异、4 段短路顺序、退化输入、确定性与失败残留、
     规模边界、表驱动等价回归。
  4. `src/d3d9/meson.build` 新增可执行目标 `war3_runtime_group_palette_kernel_test`
     （依赖 `../util/util_matrix.cpp`）+ `test('war3_runtime_group_palette_kernel')`。
- 门禁（本次实测原始结果）：`ninja -C build32` **exit 0**；`ninja -C build32 -n` **no work to do**；
  `build32/src/d3d9/war3_runtime_group_palette_kernel_test.exe` **exit 0**（T1-T13 全过）；
  `meson test -C build32` **73/73 OK, Fail 0**（基线 72 + 新增 1）；palette/slot 相关 AutoTest 静态
  **19/19**、读取 `war3_shadow_renderer_core.cpp` 的其余静态 **9/9**。
- 等价性证据（临时差分程序，已删除，非提交物）：git HEAD 的 upper-layer 4 步原实现 vs 内核，
  30 万随机输入 **0 不匹配**；core 风格 5 步（`matrixGroupSizes.size()` 派生 + 根矩阵广播）vs 内核，
  30 万随机输入 **0 不匹配**（均按适配层前置校验边界建模）。
- 本轮 `build32` ninja 产物 d3d9.dll 35,996,016 bytes / SHA-256
  `F03A84E3303E4FE552855459868196460AAC87035BBA796160C3914B2FEE55D0`（含工作树既有未提交改动）；
  **未部署、未实机**；画面/JSON/stats 面未被任何单元或静态测试覆盖，不得据此宣称阴影行为变化。
- 未做：设计 §5 的暂不涉及边界（S3 三个 slot 读取器、S4 legacy arena 降级、`HashMatrixPalette` 家族、
  device.cpp 语义大迁移、任何默认值/env 开关改动）。

## 2026-09-17 — Step 1③ 完成：生产侧采集点（R 具名原因 POD / S 最终来源 / D 绘制）+ 转换器与导出头块接线

- **生产侧采集点**（下级实现、主线程复核）：
  - **R**（`war3_shadow_renderer_core.cpp`）：`FindOrUpdatePaletteSlotCache` 增**一个小型 POD 出参**（56 B），
    填充只在 `out!=nullptr` 且全部取自**当场局部值**（未新增查询）；原因由 `ClassifyPaletteSlotRecheckReject` 给出，
    **与聚合计数共用同一返回值**（R0→BindingMiss/R1→GroupShort/R2→BindingFrameStale/其余→SlotRangeStale），
    **未执行的检查标 `NotChecked`**（不得用 R0 冒充）；原返回值/判断顺序/准入/回退/缓存更新未动；
    子门 `PaletteObjectEvidenceEnabled()` 是调用点**第一条语句**，关闭时传 `nullptr`（零取值、零查表）。
  - **S**（`d3d9_device.cpp` `War3TryAppendSemanticShadowPacket`）：子门前置；**move 之前**取出键/帧/来源 POD，
    `shadowCasters.emplace_back` **成功之后**发事件；**记录最终实际选中的来源**，native override 清空
    `draw.inputSkinSelection` 时记 `None` + `selectionClearedByNativeOverride=true`（**不回填**早先尝试值）；
    注释明确「CPU caster **候选入队**，不是 GPU 提交完成」。
  - **D**（`d3d9_war3_shadow.cpp` `renderShadowMap`）：在真实 `cmdDraw*` 之后发 `NoteDrawn`，每帧一个级联一次；
    语义「**绘制命令已记录**，不是 GPU 执行/像素正确性」；`would-cull` 仍只算聚合观察。
  - 键与帧域口径集中在 `war3_palette_object_capture.h`（三点共用），避免三处各写一套。
- **转换器与导出头块**（主线程）：`war3_palette_object_evidence_sink.h/.cpp` 按**冻结 wire** 把记录转成
  `Kind::ShadowState` + `palette-object/v1`（与 `AutoTest/analyze_palette_object_evidence.py` 的逐位表一致；
  `data[2..8]`、`words32` 保留位保持 0）；`summary()` 增 `paletteObject{watchCount,counters}` **始终输出**
  （子门关闭为 0 ⇒ 读方可区分「子门关」与「开启但无事件」）；`record.thread` 与 `words32[12]` 由 `Record` 同源填充。
- **生命周期接线**：证据 arm ⇒ `ArmPaletteObjectEvidence`（绑定发射器 + 清表）；离开 Armed/Triggered ⇒ Disarm；
  freeze/export ⇒ `ClosePaletteObjectWindow`（结算终态）；**换图** ⇒ `ResetPaletteObjectEvidence(ActiveSession(), 新 mapEpoch)`；
  **设备代际变化**（2 处提交点）⇒ `ClearPaletteObjectWatchlist()`（上级口径：未知不是通配符，不得跨 Reset 合并）。
- **诚实列出当场不可得的域**（三点一致，未用可用字段伪造）：`deviceEpoch` 一律 0 + `epochUnknown=true`（R 点无来源；
  与 `skin-selection/v1` 先例一致）；`manifestFrameSerial/manifestPublishRevision` 三点全不可得 ⇒ 0 + `manifestUnknown=true`；
  `lifecycleIdentity` 三点皆无可靠对象实例生命周期证明 ⇒ 0 + **`identityWeak=true`**（字段多 ≠ 强身份）。
- **主线程认领的一项既有测试失败**：`test_frame_recorder_memory_static.py` 因我把 sink 源加进测试目标导致折行而失败
  （我上一轮改 meson 时未跑该静态脚本）；已将断言改为按空白归一化并**把 sink 明确写入断言**（收紧而非放松）。
- **仍未做的两处（已上报上级请求裁定）**：① **C（剔除）不发对象级事件**——记录器只有四阶段，
  在剔除处发 `NoteDrawn` 会把「未记录绘制命令」写成 `sawDraw=true` 而误判 `Recovered`，要发剔除证据须新增阶段（会改冻结 wire）；
  ② 每帧预算：S 点每对象每帧 2 条（ServedCandidate + Enqueued），加 R/D 最多 4 条 ⇒ 每帧 64 条约 16 个对象后出现
  `droppedPerFrame`（有计数、有界、不静默，但高压场景可能过早丢证）。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、
  R POD 生产函数测试 **6/6**、记录器 **12/12**、生命周期 177/177、线程门禁 EXIT 0、采集点门禁 EXIT 0、
  meson **79/79**、全量静态 **247/247**；DLL **36,199,365 B / `31DC94AF564B429835DA8AD6F7304818C1C4B277C17F3B52C7C87A6595141F89`**
  （未部署/未实机/未提交）。**④ 离线成本测量尚未做**；「palette-object 事件在真实会话里长什么样」仍未经实机验证。

## 2026-09-17 — Step 1③ 实施：对象级证据子门 + 有界记录器 + 解析/缺链测试（并按实施修正 6 处语义缺口）

- **子门进入统一配置面**：`RecorderConfiguration::paletteObject`（**默认关、受主门约束**）、
  env `DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT`、访问器 `PaletteObjectEvidenceEnabled()`、
  `effectiveConfiguration` 增第 5 键 `paletteObjectEvidence`。
- **有界记录器** `src/d3d9/war3/tools/war3_palette_object_evidence.h`（生产与宿主机测试共用）：
  定长 1024 槽观察表、无堆分配、无锁、无 IO；总预算 **4096 条/会话（终态 512 含在总额内）**、每帧 64；
  阶段 `Rejected/ServedCandidate/Enqueued/Drawn`、终态 `None/Recovered/WindowExpired/ObjectGone/TableFull/EventLost/Unclosed`；
  丢失计数 `droppedPerFrame/droppedPerSession/droppedProbeLimit/droppedDuplicatePerFrame/droppedTerminalReserve/ringEvictedAfterRecord`；
  `identityWeak`/`epochUnknown` 单列；四个帧域分列（不得互相替代）。
- **实施中发现并修正的 6 处语义缺口**（子代理只读报告、主线程修正）：
  1) 终态预留耗尽原先**不记任何丢失计数** ⇒ 被拒终态不可见，违反工单 §2.2/§2.5.4.5；新增 `droppedTerminalReserve`；
  2) 每帧预算只在拒绝路径重置（接住/入队/绘制/CloseWindow 不重置）；现所有入口都推进帧边界；
  3) `Recovered` 原先只要求 `sawDraw` ⇒ 现要求 `sawSubmit && sawDraw`（候选入队 + 绘制命令已记录）；
  4) `SameKey/HashKey` 原先忽略 `lifecycleIdentity` ⇒ 已纳入键（测试观测由 collapsed=true 变为 false）；
  5) 枚举编号与工单表不一致 ⇒ **已同步工单 §2.5.2/§2.5.3 到实现编号**（解析器按实现冻结）；
  6) `chainSequence` 恒 0 ⇒ 现按链内序号赋值。
- **导出契约同步**（子代理发现真实不一致）：`analyze_frame_evidence.py` 原先只接受 4 键配置面，
  会**拒绝真实的新 schema-7 导出**；已同步 `analyze_frame_evidence.py`、`run_self_contained_recorder_gate.py`、
  `test_analyze_frame_evidence.py` 夹具到第 5 键（`test_analyze_frame_evidence` 47/47 通过）。
- **新测试**：记录器宿主机测试 `war3_palette_object_evidence_test`（12 例全过；含预算、链闭合、
  首次命中不删表、Unclosed/WindowExpired/ObjectGone/TableFull、零堆分配、帧域分列）；
  解析器 `AutoTest/analyze_palette_object_evidence.py` + 静态合同测试 23 例（缺链必须判「未覆盖」，
  「取到 palette」不得判为恢复，子门关但有事件必须报错）。
- **C8 并发用例按上级 03:58 要求改造**（生命周期测试 175→**177** 断言）：新增**共同开始握手**、
  **交错证据**（实测 200000 次读取中 48198 次落在写者完成前的中间发布）与 **30 秒运行器超时保护**；
  未使用「不弱于一秒空转」一类比较，只报实际次数/交错/耗时。
- **主线程自身错误（已修）**：上一轮我给 `test_frame_recorder_defaults.cpp` 写的断言把 `master` 传成 `"1"`
  导致主门被打开，却断言子门为 false ⇒ 该项下 10 个 meson 用例实际是红的（我当时只跑了构建，没跑该用例）；
  现改为 `master=nullptr`（构建默认关 + env 未设）并保留失败原因注释。**教训：构建通过不等于用例通过。**
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、
  生命周期 **177/177**、记录器 **12/12**、静态门禁 EXIT 0、meson **78/78**、全量静态 **246/246**；
  DLL **36,008,992 B / `B504449E65B214DCB4FB3D9A324CAED8BB4F2834A0DF084742ED4A254FCF2D1A`**
  （未部署/未实机/未提交）。**生产侧采集点（R 具名原因、S/C/D 接线）尚未实现**——记录器与解析器已就绪，
  转换器与采集点属下一步；「palette-object 事件在真实会话里长什么样」因此仍未经任何实机验证。

## 2026-09-17 — 工作身份接线修复 + A/B 接线回归（上级 03:30 裁定）+ Step 1 获准离线实施

- **上级指出并已修复的接线缺陷**：每块发布原先传入「当时的成员 `m_buildWorkGeneration`」而不是与该份 `buildWork`
  固定绑定的代际 ⇒ 旧工作可能被配上新 token；完成分支还先写 `m_lastStats`/`m_lastFrame` 再校验。
  现改为：把**工作身份（对象指针 + 固定代际）**收进生产共用组件 `ShadowBuildLifecycle`
  （`beginBuild(const void* work, …)` / `isCurrent(work, gen)` / `publishIfCurrent(...)` / `completeIfCurrent(...)`；
  `reset()` 同时清空工作身份）；运行时锁内**成对取得** `(buildWork, buildWorkGeneration)` 并成对携带；
  每块与完成发布都经 `publishIfCurrent`（旧工作不得回写由**组件**这一份规则保证）；部分帧与完成块先 `isCurrent`
  核验，完成块**先核验、再修改任何对外结果**。
- **A/B 接线回归**（上级指定的测试形态，不只是「旧代际 publish 返回 false」）：
  A 开始 → A 发布 → A 暂停 → Reset/替换并开始 B → B 发布 → **A 发布/完成均被拒** →
  **B 的全部公开状态逐字段不变**（代际、building、hasValues、values 全字段）→ B 仍可正常完成；
  另覆盖「同一 work 换旧 token」「另一 work 配当前代际」「零代际」「空工作身份」四类错配。
  测试规模 11 组 / **175 断言全 PASS**（主线程独立复核 EXIT=0）。
- **静态门禁升级**（58,272 B）：运行时不得再出现 `.publish(`/`.complete(`；必须用 `publishIfCurrent`/`completeIfCurrent`/`isCurrent`；
  完成块按偏移要求「先 isCurrent 再写 lastStats/lastFrame，且同一临界区」；锁内成对取得身份；组件接口形态；
  追加 4 条变异（把 IfCurrent 换回 publish / 完成块核验移到写入之后 / beginBuild 不传工作指针 / reset 不清工作身份）全部被拒。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、生命周期测试 175/175、
  静态门禁 EXIT 0、meson **77/77**、全量静态 **245/245**；
  DLL **36,005,019 B / `3373ECF72D58C63E708D0ECC5DD741DB8CA94F0C6D2319E78F27FABE6C8A28EE`**（未部署/未实机/未提交）。
- **上级 03:30 批准 Step 1 离线实施**（含具名原因输出与诊断帧号透传），边界：R 用具名原因 POD 结构承载、
  原返回值/判断顺序/准入/回退/缓存更新不变、未执行的检查标「未检查」不得用 0 冒充、子门关闭时零额外开销；
  manifest 帧号只作诊断且四项帧域分列；`deviceEpoch` 允许 0+epochUnknown（未知不是通配符）；
  提交点只批准为「**候选入队**」事件（`shadowCasters.emplace_back`），不得命名为 GPU 提交成功；
  **必须记录最终实际选中的来源**（native draw-time override 会清空 `draw.inputSkinSelection`，不得用早先的 `selectedPalette` 归因）；
  跨 CPU 构建与渲染消费不得共享无锁 TLS 表。**仍未批准开跑。**

## 2026-09-17 — 线程收口按 03:05 裁定补齐 + 两个计数导出 + 生产共用生命周期测试 + 三份文档授权修订

- **生产共用生命周期组件**（上级指出「9 个测试只调用判定函数、没有调用真实推进/Reset 边界」）：
  新增 `war3/shadow/war3_shadow_build_lifecycle.h`——把「所有者门 + 按值进度发布 + 工作代际守卫」
  收进 `ShadowBuildLifecycle`，运行时两处推进入口与宿主机测试使用**同一份实现**；
  计数器通过 `ShadowBuildAdvanceCounters`（三个 atomic 指针）下沉，生产传进程级全局、测试传局部。
  运行时成员 `m_buildProgress` → `m_buildLifecycle`，`consumePermissionGranted` 只做转发（不再含判定/计数逻辑）。
- **新增生产共用生命周期测试** `war3_shadow_build_lifecycle_test`（**79/79**，主线程独立复核 EXIT=0）：
  未知所有者（两种入口、并断言 `ownerUnestablished+direct != 总拒绝` ⇒ **证明入口分类不是互斥维度、不可相加**）、
  非所有者、正常所有者（两入口均放行且计数不变）、正常推进、**Reset 后旧工作不得重新发布**、取消、替换在途构建、
  以及**并发摘要读取**（写者 20 万次 publish / 读者 20 万次快照，自洽失败 0）。
- **两个计数接入导出链**（上级 03:05 明确要求）：`semanticBuildOwnerUnestablishedRefusedCount`、
  `semanticBuildDirectAdvanceRefusedCount` 各 7 处（bridge.h/.cpp、hub.h/.cpp×2、control_plane.cpp、perf_monitor.cpp），
  注释写明**进程累计值**且**不得未经证明相加成「总拒绝」**；分段增量由读数侧差分。
- **静态门禁再升级**（46,013 B）：判定只允许出现在组件内、三计数引用位置受限、禁 shared_ptr/make_shared 进度载体、
  按值成员、生命周期齐全、同一次发布守卫、reader 只读摘要、三字段各 7 处导出、新计数注释含累计/不可相加口径；
  实施者另做 **7 条变异全部 KILLED**（入口不调组件 / 组件改 shared_ptr / 新计数只接一半 / 判定泄漏到入口 /
  reader 读 live / 直接入口不再单独计数 / 删代际守卫），并在门禁 docstring 固化变异步骤以保证可复现。
- **三份文档按上级 03:05 授权修订**：
  - 对象级工单（117,987 B，修订版 R3）：预算统一为**总计 4096 条/会话、终态从中预留 ≤512**（禁止 4608 合计），
    新增 §R.5 裁定落点表与「缺链判定（分析器门禁）」；主线程另行**冻结记账口径**（总计 4096 / 其中终态 ≤512，两列不得相加超 4096）；
  - `2026-09-17-gapb-confirmation-strengthening-design.md`（61,240 B，**上级明确授权**）：§8① 与冲突文字统一到五层口径，
    明确 S4 / 同帧 frameTag 相等 / 快照大小相等**都不是**所有权证明；旧运行证据与阈值**一字未改**；
  - 冻结记录与线程证明文档：未实机候选措辞收窄为「**已完成源码所有者门加固和摘要读取改造；纯判定测试通过。
    生产生命周期与运行时覆盖待验证。**」；写者审计表述为「**未发现其他写入路径**」（源码调用链结论，非运行时见证）。
- **新增 Step 1 第②步产物**：`2026-09-17-p0-object-evidence-event-table.md`（事件 R/F/S/C/D/T × 生产函数 × 局部字段 ×
  身份/帧域 × 缺失处理；含五层各自证明到哪一步与必须先补的 5 项字段）。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、生命周期测试 79/79、
  进度测试 86/86、线程边界 9/9、静态门禁 EXIT 0、meson **77/77**、全量静态 **245/245**；
  DLL **36,005,019 B / `91C1CD0AA329EAFEBFC0CE98ADB1175047EA229B832C0D137C5DF7DB4E2F9504`**（未部署/未实机/未提交）。

## 2026-09-17 — 线程收口完成（上级三项要求的第二/三项）+ 对象证据采集点只读核实

- **上级 codex 02:41 裁定**要求线程修复必须含三项（不能只加一个快照成员）：
  ① 删除「线程 ID 为 0 时放行」（不知道所有者 ≠ 没有并发写者）——**已完成**：
     所有者未建立 ⇒ 拒绝推进（只保留安全请求）；
  ② 覆盖直接构建入口（`ensureFrameBuiltForContract` 可被 `d3d9_device.cpp` 直接调用）——**已完成**，
     并按要求把规则收敛为**唯一一份**检查 `consumePermissionGranted(bool directEntry)`，
     两处边界共用，避免两套守卫分叉；
  ③ 进度摘要按值发布、无可变别名、**不新增每块堆分配**、帧号/revision/工作代际/统计来自同一次发布、
     开始/完成/取消/Reset/**替换**都更新、旧块不得在 Reset 后回写——**已完成**：
     新增轻量头 `war3/shadow/war3_shadow_build_progress.h`（纯状态机 + 按值载体，生产与测试共用），
     取代原先的 `shared_ptr<const …>` 方案（后者既是别名、又每块堆分配，已被上级驳回）。
- **新增宿主机进度生命周期测试** `war3_shadow_build_progress_test`（8 例 / 86 断言，主线程独立复核 **86/86**）：
  正常序列、旧代际拒绝且值不被改写、Complete 后拒绝、未 Begin 拒绝、替换构建、
  **Reset 后旧块无法回写**、代际非 0 且严格递增、按值语义。
- **静态门禁再升级**（31,981 B）：唯一规则（`DecideShadowBuildAdvance` 只允许出现在 `consumePermissionGranted` 内）、
  两处入口必须调用该唯一检查且早于首次加锁/推进、禁止进度摘要使用 `shared_ptr`/`make_shared`、
  必须有按值成员、生命周期方法齐全、`Publish` 与统计赋值同临界区、reader 只读摘要；
  实施者另做**8 条变异 + 2 次对照**（去代际检查 / 改回 shared_ptr / 入口不调用唯一检查 / 插入第二处判定 /
  统计早写 / reader 读 live / Cancel 换 Complete）全部被拒。
- **对象证据采集点只读核实**（新增 45,450 B 文档）：
  - **关键修正**：本构建 **core 自己不提交**——`war3_shadow_renderer_core.cpp:9734` 的 `if constexpr`
    为真 ⇒ `:9738 m_core.submitFrame` 是死分支；**对象键 + 提交事实同时可得的最佳点是 device 侧
    `War3TryAppendSemanticShadowPacket` 的 caster 段**（`d3d9_device.cpp:24049/24052/24385-24386/24641`）。
  - 绘制/剔除信息在 `War3ShadowReceiverPass::renderShadowMap`（`:3713`，剔除 `:4795` 带具名 `rejectReason`、
    真实 draw `:5190/:5194/:5222`），但**现有导出只有无对象关联的聚合计数**。
  - manifest records 带四元组 + `frameSerial`（但 `:4253-4254` 会改写 manifest 帧号 ⇒ **不得用 record.frameSerial 冒充**）；
    canonical item **不保存** `skin::Selection`。
  - **必须先补字段**：`FindOrUpdatePaletteSlotCache` 需新增 out 参数（R0..R3 具名原因；现调用方只见 `0xFFFFFFFF`）、
    `deviceEpoch` 在 shadow-core 内不可得（只能 0 + epochUnknown，既有先例 `:23158` 亦然）、
    `manifestFrameSerial` 在 ①/④ 不可得、`slotAllocationGeneration` 恒 0（⇒ 所有权层仍不闭合）。
  - **覆盖路径已核实为实机默认路径**；旧轮 `resolved=0` 落点为 `skippedResourceMiss++`（`:7629`），
    若仍为 0 只能写「未覆盖」。
  - 自检：76 个 file:line 锚全部回源命中；另修正两处口径（`cmdDrawIndexed` 另有 `:5497/:8579` 站点；
    `:24224` 存在清空 `inputSkinSelection` 的分支）。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、
  进度测试 86/86、线程边界测试 9/9、静态门禁 EXIT 0、meson **76/76**、全量静态 **245/245**；
  DLL **36,005,019 B / `4E2B355069949E398E5FCDE2B2E44C1123ED773561CF3A78132C7EE55A9585F9`**（未部署/未实机/未提交）。
- 冻结记录与 `AGENTS.md` 身份已同步更新；**Step 1（对象级记录实施）仍待上级准入裁定**。

## 2026-09-17 — 线程修复完整落地（含进度摘要发布）+ 写者审计 + 对象级工单按裁定重写

- **修复第一部分（所有者门，按上级裁定扩大）**：门设在**两处**——`ensureLatestFrameBuilt()`（入口）与
  `ensureFrameBuiltForContract()`（真正的推进入口，防直接调用绕过）；判定抽为轻量头
  `war3/shadow/war3_shadow_build_thread_gate.h`（inline 纯函数，生产与测试共用）。
  **推翻原稿**：所有者未建立（`GetMainLoopThreadId()==0`）原先放行，现改为**拒绝推进**（只保留请求语义），
  计数 `g_semanticBuildOwnerUnestablishedRefusedCount`；底层入口被拒单独计 `g_semanticBuildDirectAdvanceRefusedCount`。
  所有者身份取自 hook 生命周期观测的主循环线程，**不是第一个请求线程**。
- **修复第二部分（安全进度快照发布，上级明确要求）已落地**：新增不可变
  `ShadowValidationBuildProgress` + `publishBuildProgressLocked()`（每块完成后在**短临界区**发布），
  各 `m_buildWork.reset()` 处同步清空；`snapshot()`/`buildStateSnapshot()` 改为只读该摘要，
  函数体内不再触达 `m_buildWork`；**未把锁持进 `buildFrameChunk`**（保持既有增量语义）。
- **新增生产边界测试** `war3_shadow_build_thread_gate_test`（宿主机，9 例，主线程独立复核 **9/9 EXIT=0**）：
  所有者未建立（含 current=0 不得当作自己是所有者）、正常所有者推进、管道线程 drain、隐式 hot-wait、
  直接调用底层入口、未知/错误所有者、Reset 后未重建。
- **静态门禁重写**（`AutoTest/test_semantic_build_thread_gate_static.py`，16,053 B）：两处入口门位置/判定函数/
  计数器原子形态/旧「未建立即放行」名不得复活/进度发布必须持锁/reader 不得读 `m_buildWork`/3 处 reset 配对/
  断言 7 导出点仍在/禁止肯定式宣称；实施者另做**变异验证 7/7**（去掉 reset 配对、计数器非原子、
  底层门改直接比较、旧名复活、reader 读 live、门移到首锁之后 均被拒绝）。
- **写者审计（上级要求核清）**：`s_slotBlendedPaletteCache` 全树 9 处命中全部在 `war3_model_hook.cpp`；
  写者为 `CaptureBlendedPaletteSlotRange`（`:2400-2406`，由 `:2699` 绑定捕获与 `:8020` 运行时矩阵写入 hook 调用）、
  `ResetMapSession` 批量失效（`:10337`，Present 地图切换）、静态初始化（`:303`）——**均属主循环/Present 所有者线程族**，
  未发现管道线程/DXVK 工作线程/点阴影 worker 的写者。措辞上限仍为「已观察到写者均属所有者线程族」。
  **顺带发现**：写入已维护单调 `writeSerial`，但**全树无任何读者消费**；这是现成但未被使用的廉价一致性检查，
  可作为对象级路线 B 的「可用 write serial」基础（不能替代所有权证明）。
- **对象级工单按上级十项裁定与七条关键点重写**（110,060 B / 1,320 行）：置顶 R.1 裁定落点表、R.2 关键点落点表、
  R.3 旧表述处置表（13 行，逐条驳回/收窄/替换，保留可追溯）、R.4 未闭合 6 项；新增 §1.5 证据覆盖路径、
  §5.6 五层证明表（缓存标签/arena 字节/所有权/提交/绘制各自证明到哪一步）；§7 由「需裁定清单」改为「裁定结果」。
  实施者报告一次 edit 锚点错配曾误删约 250 行并逐字重建；主线程已抽检结构（13 条处置行、标题清单、
  围栏 10 处配对、关键裁定词全部在）——**内容完整性已抽检但无法 git 级校验**（该文件在本树 untracked）。
- **实机申请草案按批准范围重写**（18,530 B）：仅 pair-0 两轮、无 pair-1/无补跑、CPU-only 对象证据两轮同开、
  禁止控制面 drain 且不得绕过、隔离桌面零全局输入、客户区须实测 1902×963 并同时记录 backbuffer/receiver viewport/截图尺寸、
  覆盖窗口与停止条件冻结、旧哈希授权不继承、构建纪律 Below Normal + 最多 `-j2`。
- **门禁（主线程独立复跑，Below Normal + `-j2`）**：`ninja` exit 0、`ninja -n` no work、
  边界测试 9/9、新静态门禁 EXIT 0、meson **75/75**、全量静态 **245/245**；
  DLL **36,008,386 B / `E85C2C729440D739DA345DCE8EFF438E03712EE4000D7E6814D8A1C4E56BA927`**（未部署/未实机/未提交）。
- 仍未导出 `OwnerUnestablished`/`DirectAdvance` 两个计数（仅有访问器）；**修复 3（drain 具名原因）与对象级记录未做**。

## 2026-09-17 — 线程修复第一部分落地 + 候选冻结（未部署 / 未实机 / 未提交）

- 落地线程修复**第一部分**：`ShadowValidationRuntime::ensureLatestFrameBuilt()`（core.cpp:9413）在
  `requestLatestFrameBuild();` 之后、任何状态改动与加锁之前加入主循环线程门（`GetMainLoopThreadId()`
  与 `::GetCurrentThreadId()`）：非主循环线程只保留请求语义并自增 `semanticBuildOffThreadRefusedCount`
  后 return；主循环 id==0 的早期窗口放行并单独自增内部计数。因位于推进边界，**同时覆盖**
  `allowControlPlaneSemanticDrain` 与 `IsHotSemanticBuildWaitPayload` 两条门控（上级要求）。
  新计数接入 7 处导出链（bridge.h/.cpp、hub.h/.cpp×2、control_plane.cpp、perf_monitor.cpp）。
  **未改** palette 判定、drain 门控、payload 开关、默认值/env、AutoTest MCP。
- 新增 `AutoTest/test_semantic_build_thread_gate_static.py`（门位置/计数器形态/7 导出点/
  禁止肯定式宣称/回归护栏）；主线程独立复核 EXIT=0。
- **修复第二部分（安全进度快照发布）未做**：进度字段仍锁外写、快照锁内读，该竞争仍在 ⇒
  **不得宣称线程问题闭合**（上级明确：B 必须含此项才算闭合）。措辞上限已遵守。
- 新增 `docs/plan/2026-09-17-candidate-freeze-record.md` 冻结候选身份；申请草案 §1/冻结纪律已回填。
- **门禁（主线程独立复跑，-j4）**：新门禁 EXIT 0、`ninja -C build32 -n` no work、
  meson **74/74**（Fail 0）、全量静态 **245/245**（Fail 0）；
  DLL **36,004,514 B / `D286508BFFC178C5572BBE267BC55D402A017D4A91C538C9B28C94C4A454450C`**。
- `AGENTS.md` 当前状态新增一条浓缩 checkpoint（含上述边界）。

## 2026-09-17 — S2 可复现差分 + 成本口径落地并通过主线程独立复核

- 新增 `src/d3d9/war3/render/tests/war3_runtime_group_palette_kernel_diff_test.cpp`（102,761 B / 2,379 行）
  + `src/d3d9/meson.build` 新增目标与 `test(...)`；`AutoTest/test_runtime_group_palette_kernel_single_source_static.py`
  追加 §8.0-8.9 断言（既有断言未放松）；core/upper/kernel 只加注释（**内核语义未改**）。
- **旧实现身份**（写入测试文件头 + 每次运行输出，静态门禁逐个复核 SHA-256）：
  core `0F7B720E…D8F9`（pre-S2 副本 402,159 B，抽取点 6577-7166 五步）、
  upper `61EB0C8F…17A9`（17,763 B，抽取点 98-242 四步）。
- **可复现性**：PRNG=SplitMix64，`seedCore=0x5332464600000001` / `seedUpper=0x5332464600000002`，
  每组 300,000 输入；主线程独立运行复现：core 300,010 输入 / upper 300,005 输入，**mismatch 均为 0**，
  逐输入比较 ok/usesAveraging/maxSlot/palette.size()/逐元素 bitwise/失败 miss reason/残留 palette。
- **适配层覆盖方式**：两个生产 `TryBuildRuntimeGroupPalette` 依赖 d3d9 记录类型与 TU 内部状态、
  **无法在宿主机链接**，故用 `DeriveCoreAdapterInputs`/`DeriveUpperAdapterInputs` 显式建模派生
  （min 链 + 128Ki clamp、`positions/3`、upper 两个 min）与三条前置校验，并配派生边界夹具；
  适配层文本一致性由静态门禁负责——**这不是链接级适配层验证**（测试输出已声明该限制）。
- **成本实测（只报告测量，不宣称热路径成本等价）**：
  - 槽位扫描趟数：core pre-S2 **2 趟 → S2 4 趟**（适配层 maxSlot 1 + 适配层去重 1 + 内核 2）；
    upper 仍 2 趟。**注意修正**：先前估计的 3 趟不对，实为 4 趟；且适配层那趟去重结果只被
    `logFailure` 消费，而该采样在 5 步集合上不可达（core `FallbacksFailed` ==0，upper ==145,485）。
  - 分配：旧实现复用调用方 vector 时第 2 次 0 次 palette 分配、指针/容量不变；
    S2 适配层每次新建局部输出再 move ⇒ core 第 2 次 4 次分配/608 B、upper 3 次/576 B，
    **pointerChanged=1**（丢掉了调用方容量复用）；内核单独复用同一输出时 pointerChanged=0。
  - 结构性发现（已用断言钉住）：core 的第三条前置校验 `NoVertexGroups` 不可达（==0），
    upper ==13,526；core 的 `FallbacksFailed` 不可达（==0），upper ==145,485。
- **门禁（主线程独立复跑，构建 `-j4`）**：diff 可执行 EXIT=0 且打印种子/输入数/副本哈希+0 mismatch；
  `ninja -C build32 -n` no work；meson **74/74**；静态 **244/244**；
  DLL 36,000,018 B / `5B6EBBF94F8A6C0340C255D02615F11F5610E08B0A7A6B9AEDC41CFA1D7FD861`
  （未部署、未实机、未提交）。
- 口径（上级指定措辞）：**所测输入集合零差异；适配层为派生模型验证；成本不等价。**
  不得写成所有输入/真实适配层/热路径已等价；也不得因随机测试某分支命中为零而删除生产检查。
  冻结记录早期把一次 `-j4` 写成「遵守 ≤4 线程纪律」**已被更正**（不构成对上级构建纪律的遵守）。

## 2026-09-17 — 按上级裁定修订：线程修复实施设计 + 对象级工单两处纠正（未改源码）

- 新增 `docs/plan/2026-09-17-thread-fix-implementation-design.md`：
  - **修复 1（推进边界拒绝非主循环线程）**：在 `ensureLatestFrameBuilt()` 开头用既有
    `GetMainLoopThreadId()`（先例 `war3_native_capture.cpp:174`）判定；因所有入口
    （`drainPendingBuildForControlPlane` :9448→:9466、`runObserveValidation` :9685→:9686、renderer/bootstrap）
    都经此函数，故同时覆盖 `allowControlPlaneSemanticDrain` 与 `IsHotSemanticBuildWaitPayload`（上级要求）；
    被拒时只保留 `requestLatestFrameBuild()` 语义，并新增 `semanticBuildOffThreadRefusedCount`
    （**与 A5 拒绝计数不同分母**）。**改 MCP 开关只算卫生措施，不算修复。**
  - **修复 2（安全进度快照发布）**：确认 `ensureLatestFrameBuilt` 在 :9535 释放锁后于 :9548 锁外写
    `nextRecordIndex/chunkCount/totalBuildDurationUs`，而 `snapshot()`(:9722)/`buildStateSnapshot()`(:9734)
    在锁内读同一批 live 字段 ⇒ 只给读者加锁无效。方案：每块完成后用**短临界区**发布不可变
    `m_publishedBuildProgress`，读侧只读该快照，**不把锁持进构建过程**（保持既有增量语义）。
  - **修复 3（可选）**：控制面 drain 返回具名原因，避免把「线程拒绝」误读为「无待构建内容」。
  - 验收：1) 管道 drain 请求断言拒绝计数 +1 且进度不前进；2) **用 `IsHotSemanticBuildWaitPayload`
    的隐式形态重复**；3) 宿主机并发测进度发布无撕裂；4) 门禁全过；5) 措辞上限仍为
    「已观察到主循环线程唯一推进」，**不得**写「已消除竞争」。
- 对象级工单按上级裁定纠正两处：① 后随观察表**不再首次命中即删**——改为状态机
  （`firstHitFrame/hitCount/lastHitFrame/sawSubmit/sawDraw`）保留到四段闭合或会话/窗口结束，
  闭合时才发 `closed` 标记事件；② **「恢复」定义补上实际提交与绘制证据**（新增第 3、4 条），
  仅「拒绝→重新拿到 palette」只能记「观察结果（部分）」。

## 2026-09-17 — 上级新增裁定（线程修复与对象级工单，必须遵守）

上级（codex 01a02e0b）在本轮又给出 4 条裁定：

1. **驳回「数据竞争只导致拒绝、因此仍安全」的推断**：无锁读可能读到**碰巧满足等式的旧值**，
   于是 A5 通过却用了错误所有者数据；发生 UB 后结果不再有保证。⇒ 只要 off-thread drain 可能发生，
   **A5 的通过结论完全不能作为证据**。线程证明文档 §0/§3 已据此改写。
2. **对象级工单的「恢复」定义不足**：把「重新拿到 palette」当成「阴影恢复」缺少**实际提交与绘制**的关联证据，
   验收必须补上提交/绘制证据链。
3. **线程修复优先 B，但只把 MCP 开关改 false 不够**：`IsHotSemanticBuildWaitPayload` 仍能隐式放行 drain；
   检查必须设在**真正推进构建的边界**并覆盖**所有**入口。另外**后随观察表在第一次替代命中就删记录**
   会导致压力解除后的恢复无法验证，该设计须先改。
4. **B 方案还必须包含安全的进度快照发布**：构建进度字段在**锁外**更新，而控制面快照在**锁内**读取
   同一份 `buildWork`；只给读者加锁无法保护不使用同一把锁的写者，不得只封 drain 就宣布线程问题闭合。

⇒ 本轮据此修订：线程证明文档（驳回过度推断）、对象级工单（恢复定义 + 后随表设计）、
并新增线程修复实施设计（覆盖所有 drain 入口 + 进度快照发布）。

## 2026-09-17 — 下一轮实机申请草案 + 长期计划修订（未申请开跑）

- 新增 `docs/plan/2026-09-17-next-real-machine-application-draft.md`：按上级 Q-D 要求写成
  **申请材料**（非授权、非执行记录）。要点：pair-0 主对照「只允许合同值不同」、
  `PALETTE_DIAGNOSTICS` 两轮同为 0、pair-1 交叉对账单独报告、明文禁止 ON=0/OFF=1；
  行为改变类变量（`ENDFRAME_BUILD`、`PUBLISH_REGISTRIES_BEFORE_SCENE`）单列声明且不得外推为默认生产路径；
  **分辨率先申请后跑**（实测客户区 1902×963，冻结 2560×1440 不可达）；
  2 轮 × 3 段 × 600s、fresh 进程、采样与边界快照口径、恢复与中止合同、
  证据采集面与「不得越界」判定口径、5 项待上级裁定。
- 修订 `docs/plan/2026-09-16-architecture-review-longterm-plan.md`：新增
  「2026-09-17 修订：状态校正与收敛序列」——权威状态表（六方向）、三个真正卡点、
  上级裁定要点、S1–S5 收敛序列与每步完成标志、更新后的固定门禁（meson 73/73、静态 244/244）、
  子代理路由更新（commandcode / deepseek-v4.1-flash）、明确不做。
- 资源纪律：并发线程总量 **≤4**；构建统一 `-j4`。

## 2026-09-17 — 对象级证据 Step0 只读回溯：现有产物无对象级证据（只读）

- 对 `round2_on_m2` 的 627 键全量快照做只读回溯：`shadowEvidenceCollectorAttached=0`、
  `shadowEvidenceRetentionRevision=0` ⇒ M2（`DXVK_WAR3_FRAME_EVIDENCE=0`）下**未记录任何对象级事件**；
  现场目录无 `cpu-<pid>-…json` dump，`war3_d3d9.log` 无 evidence 行。
- **净结论**：现有三轮产物无法回答「某次拒绝属于哪个对象 / 之后是否被接住 / 压力解除后是否恢复」；
  必须按对象级工单 §2 挂**有界**采集后另开一轮。**不得用聚合计数冒充对象级证据。**
- 顺带线索（建议列入下轮观察面）：`paletteCaptureInvalidEntryMissCount=1,798,896` 与
  `paletteCaptureExactHitCount=0`/`BestEffortHitCount=0` 并存 ⇒ 捕获读取路径几乎从不命中；
  `currentDrawCapturedPaletteQuery Attempt==Hit==437,756`（同点自命中，口径待确认）；
  `renderablePartPaletteSnapshotCapturedCount=0`、`runtimeSimpleGroupPaletteSlotCapturedCount=0`。
- 结果已写入 `docs/plan/2026-09-17-p0-object-level-evidence-workorder.md` 附录 D。

## 2026-09-17 — 活跃路径四项纯计数落地 + P0 对象级证据工单（未部署 / 未实机）

- **四项当帧纯计数**（上级 Q-B 条件批准：无条件编译；第 4 项必须是第二条 draw-time 路径的提交分母，
  不得落未经证明的 palette 来源桶）：`semanticSceneAppendEntrySkinnedCount`、
  `semanticSceneCanonicalGateRejectSkinnedCount`、`semanticSceneDrawTimeProducerSubmittedSkinnedCount`、
  `semanticSceneDirectCurrentDrawSubmittedSkinnedCount`。均为 `m_war3Scene.shadowStats` 当帧
  `uint32_t`（非进程全局原子），四处自增点只加独立 `++`：append 入口（`EffectProducerPolicyAllows` 之后、
  canonical 构建之前，`if (skinned)`）、canonical 拒绝分支（skinned-only，先于 resolve 判断）、
  `War3TryPopulateDrawTimeSemanticProducer`、`War3TryPopulateDirectCurrentDrawGrouped` fast-append
  （后两者紧邻既有 `semanticSceneSubmittedSkinned++`）。**未改任何判定/准入/默认值/env**。
- **导出链**：每字段 11 处（scene.h → bridge.h/.cpp → hub.h/.cpp×2 → control_plane.cpp →
  perf_monitor.h/.cpp×3），与既有 5 个 Gap 计数器同风格；主线程已逐点复核两处关键自增点条件。
- **新增静态门禁** `AutoTest/test_active_path_palette_instrumentation_export_static.py`：
  钉死四字段名与 11 出口、四处自增位置与 `if (skinned)` 条件、禁进程全局原子、
  禁 palette 来源桶命名族、禁"替代来源/同对象恢复"肯定式宣称、回归护栏（合同默认值、既有 8 桶、既有 3 个 skinned 自增点）。
- **工单文档按上级 Q-D 修正**：`2026-09-17-active-path-palette-instrumentation-workorder.md` §4.5/Q8
  改写为"同一 pair 内除合同值外必须完全一致"，只允许 pair-0（两轮=0 主对照）与 pair-1（两轮=1 交叉对账），
  **明文禁止 ON=0/OFF=1**，并撤销"唯一能产生证明的矩阵"辩解。
- **新增 P0 对象级证据工单**（61,438 B / 881 行）：`docs/plan/2026-09-17-p0-object-level-evidence-workorder.md`。
  要点：聚合计数无法回答"某次拒绝属于哪个对象/之后是否被接住/压力解除后是否恢复"；
  设计为**复用既有 `war3::tools::evidence`**（不发新 Kind——wire golden 把 kind 钉在 1..18，
  改用 `Kind::ShadowState` + label `palette-object/v1`），硬预算（≤4096 条/会话、≤64 条/帧、
  每对象每帧 ≤1、watchlist 1024 槽零分配、失败即关闭）；新增**有界"后随观察表"**（拒绝入表、
  同对象被具名来源接住时发一条 follow-up 并移除）；槽位所有权给出路线 A（per-slot owner 标签，
  +768 KB、会改判定）与路线 B（帧内 trace 证伪，零准入改动，建议先做，但其硬限制是
  writer hook 只有 nodePtr、binding 侧才有 part，两者身份域不可直接对拍）；**独立复核确认了
  off-thread drain 路径**并交叉引用线程证明文档；列 9 类"不得冒充"物；需上级裁定 10 项。
- **门禁（主线程串行复跑，构建用 `-j4` 以遵守 4 线程上限）**：`ninja -C build32 -j4` exit 0、
  `ninja -C build32 -n` no work to do、内核 **T1-T17 全过**、meson **73/73**、静态 **244/244**。
  DLL 36,000,018 bytes / `7DF26FF7345C31D431B552239E676C6EC206889CA119FE68B987252ACAF66476`
  （**未部署、未实机、未提交**）。
- 资源约束：本会话后续并发线程总量**上限 4**（含子代理与后台任务），构建统一 `-j4`。

## 2026-09-17 — P0 线程关系证明：A5 的“同线程”前提**不成立**（只读证明，未改源码）

- 新增 `docs/plan/2026-09-17-p0-thread-relationship-proof.md`。这是上级与 Gap B 设计 §9
  共同要求的前提证明（A5 读的是**非原子**槽位缓存）。
- **结论**：palette 路径并非结构性地在渲染线程，而是**取决于谁消费语义构建**：
  - 渲染线程消费（scene submit / EndFrame 小步推进）⇒ 安全。该纪律在代码里已有明确注释
    （`war3_shadow_runtime_bridge.cpp:5313-5317`：control-plane 不能同步替 render thread
    消费构建），控制面 refresh 分支只调 `requestLatestFrameBuild()`。
  - **控制面 drain 消费** ⇒ `DrainSemanticBuildFromControlPlaneIfAllowed`
    （`war3/tools/war3_control_plane.cpp:4452`）在**调用者线程**上执行
    `drainPendingBuildForControlPlane`（:4474），而控制面请求跑在
    `std::thread(HandlePipeClient, pipe).detach()`（:5136）的**分离管道线程**上。
- **读点无守卫**：`QueryBlendedPaletteFrameTagRange`（`war3_model_hook.cpp:9201-9238`）
  对 `s_slotBlendedPaletteCache[].valid/frameTag` 是普通读；对照之下
  `QueryRenderablePartPaletteSnapshot` 有 `TryCell`+busy/seqlock（不撕裂），
  绑定查询用原子字段。⇒ A5 的逐槽帧读在 off-thread drain 开启时与写者构成数据竞争。
- **不是理论风险**：`AutoTest/war3_autotest_mcp.py` 在 4 处请求
  `get_shadow_runtime_summary` 时带 `allowControlPlaneSemanticDrain: True` +
  `semanticBuildDrainMaxChunks: 32`（L8655/8659/8746/8748/9159/9162/9595 附近）
  ⇒ **标准 AutoTest 路径会走 off-thread drain**。
- **本轮实机未触发**：`warvk_p0_phase_driver.py` 未发送该标志与 refresh 标志（0 命中），
  三轮构建消费都在渲染线程 ⇒ 本轮数据，但也不能据此声称前提成立。
  **上级裁定**：只能写「该驱动未请求已发现的 drain 路径」，不得升级为「全进程无竞争/数据未受影响」。
- **方向性与边界**：A5 的判决是"三个 tag 相等"，垃圾/撕裂只会让等式失败 ⇒ 拒绝 ⇒ 落替代路径
  （fail-closed，不会把错误矩阵当正确矩阵用）；但**帧内新鲜度证据的可信度下降**，
  且与"槽位所有权未证明"叠加。
- **修复方案（三选一，建议 B，未落地）**：A 给逐槽读加与快照同源的 TryCell/原子；
  **B 在 drain 入口加渲染线程身份校验（非渲染线程拒绝 drain，只发请求），并把 AutoTest MCP 的
  `allowControlPlaneSemanticDrain` 改为 false 直到修复**；C 仅记录约束 + 静态测试钉住现状。
  三条都属源码/行为改动，**需上级裁定后再落地**。
- 验收：B 落地后注入一次管道线程 drain 请求，断言构建未被消费；A 落地后用宿主机并发单测断言无撕裂。
  实机报告今后必须**分别声明**"构建消费线程"与"槽位所有权"两项是否已证明。

## 2026-09-17 08:2x — 上级（codex 线程 01a02e0b）审查裁定记录（必须遵守）

> 请求：messageId `01a0acac-2752-7b02-bafc-b049180a911b`；补充：`01a0acb7-2aff-7623-a60e-1b7909201af4`。

- **Q-A（Gap B 资源键修复）**：**不批准**为让 `resolved>0` 直接扩大生产准入；
  顺序必须是"先记录同一对象的实际查询键/写入键/资源代际/发布时机/拒绝原因 → 确认缺失来源 →
  形成**默认关闭的 dev 候选**"，且 dev 门不是安全检查的替代品。
  **必须删除"纯 env trace 零风险"的表述**：`CONTRACT_CAPTURE_PERIOD=1` 会改变抓取频率与时序，
  需独立冻结预算，不得直接追加进下一轮。清册方向继续认可，但"正式不承诺 env 接口"≠
  所有者/优先级/发布可达性审计已完成。
- **Q-B（活跃路径仪表 MVP-1）**：**条件批准无条件编译**（不需再加默认关闭的编译门），
  但第 4 项改为**第二条 draw-time 路径的提交分母**，**不落未经证明的 palette 来源桶**；
  明确否决 `gpuSkinLeaseBacked → OwnedPartSnapshot` 与"几何 cache consume hit → 已证明矩阵
  `DrawTimeCaptured`"两种映射（几何租约/顶点快照/矩阵来源是不同证据，不得互相冒充）。
  要求：计数放准成功/拒绝位置；不得把 `ObjectKind::Unit` 等同于已证明蒙皮；明确线程所有者/
  帧号/重置/发布口径；不增加热路径日志、分配或跨线程查询；不开大容量取证；
  用生产代码测试验证计数/重置/出口一致性。**这四项只证明路径到达与提交数量**。
- **Q-C（两条候选）**：仅批准"**保留在未提交候选工作树**"，不授权 commit/发布。
  S2 不要求保留 dev 双实现，但下次冻结前必须处理：① core 已扫描+去重、内核又扫描一次；
  ② 两侧适配层新建 vector 后 move，**不能沿用旧实现的容量复用保证**；③ 差分程序已删除 ⇒
  30 万零差异**不可复现**，须保留程序/种子/旧实现身份/输出且**覆盖适配层**。
  因此数值等价可作为进展，**不得据此宣称热路径成本等价**。
  Gap B **只接受为未验收的部分加固**，三处必须明确：① 逐槽 frameTag 查的是
  `s_slotBlendedPaletteCache`，随后消费的是原生 arena，同帧标签不能独立证明两者字节/所有者/
  读取期间一致；② 快照查询本就 resize 到 `expectedCount`，`>=`→`==` **未新增**"排除别人槽位"的证明；
  ③ `BindingMiss` 未导出、`SnapshotFrameStale` 不在同一拒绝分母 ⇒ 不得笼统声称细分之和已闭合。
  S4 未完成的限制继续保留；不得顺带放宽帧容差；将来组合运行必须**分别报告两项覆盖**。
- **Q-D（下次实机）**：**本次答复不授予开跑权限**；DLL/仪表/资源准入/矩阵任一变化都需新冻结记录
  与对应实机裁定。M2 可作为**显式诊断矩阵**，但 `ENDFRAME_BUILD`/`PUBLISH_REGISTRIES_BEFORE_SCENE`
  改变构建/发布行为，不得外推为默认生产路径；ON/OFF 必须除合同值外完全一致；
  **仪表工单里"ON diagnostics=0 / OFF=1"与"只差合同"自相矛盾，须修正**；
  不接受"这是唯一能产生证明的矩阵"；无法满足 2560×1440 时应停止并提交替代分辨率申请，
  不得先跑后说明。
- **Q-E（措辞）**：认可 ②未覆盖、③未覆盖、画面反例门未覆盖。①应改为
  "**已观察到记忆槽位复核拒绝路径；'错误矩阵不再被使用'尚未证明**"（`RejectedStale` 含绑定缺失/
  数量不足等拒绝，不能全部解释为"确认发现陈旧矩阵"）。执行记录另须修正：§2.4 的样本最大值
  33,819 与边界值 31,881→88,926→91,039 需统一证据版本（上级重算 samples.jsonl 得 **91,034**）；
  0.5s 采样是"采样点最大值/占比"而非完整逐帧统计；三轮均为 `forced=true` 受控终止，
  驱动 exit 0 ≠ 游戏自然退出已验收；`CanonicalReadyCount=0` 只支持"该运行未观察到就绪"，
  **不能**定性为所有输入下的结构性死代码。
- **下一会话第一步顺序（上级给定）**：① 先修订执行记录与工单（授权偏差、真实尺寸、统计矛盾、
  来源证明边界），保留全部原始证据；② 落地四项纯路径计数及测试，不改 caster 准入；
  ③ 补齐 S2 可复现差分与分配/扫描检查，修正 Gap B 证明与计数口径；
  ④ 单独提出资源键诊断方案（优先有界对象级记录），暂不放宽 `IsContractUnitCandidate`；
  ⑤ 离线验证、冻结新候选，再提交明确轮数/矩阵/尺寸/恢复合同的实机申请。
- 裁定总纲：**当前不晋升稳定、不提交、不发布。**

## 2026-09-17 08:1x — 夜间最终门禁与身份（取代本条目之前的中间数字）

- **最终候选（Gap B 补强 + S2 内核 + 对抗性复核修复）**：DLL 35,995,922 bytes /
  SHA-256 `BFA4C4636DDEEAEA7935EF228AFD127D0CF7DDFBF67F4CCD0C971DABD22894F0`。
  **未部署、未实机、未 commit。**
- 门禁（主线程统一复跑）：`ninja -C build32` exit 0；`ninja -C build32 -n` **no work to do**；
  meson test **73/73**；AutoTest 静态全量 **243/243**；内核可执行 **all T1-T17 passed**。
- 数字订正（取代本日更早条目里的中间值）：S2 内核头 **318 行**（含 2 处空视图守卫）；
  内核测试 **1,225 行 / T1-T17**（原记 908 行 / T1-T13）；palette-slot 相关静态由 19 个增至
  **20 个**（新增 S2 单一来源门禁）。以上订正由 S2 实施者指出、主线程复核后落地。
- 新增内核守卫（对抗性复核 + 探针实测发现）：`matrixGroupSizes == nullptr && groupCount != 0` 与
  `matrixIndices == nullptr && matrixIndexCount != 0` 均返回 `InvalidGroupTable`；
  修复前后者在严格组合下实测为访问违例 `0xC0000005`（生产调用方以 `data()+size()` 成对传入，
  故非生产风险，属契约硬化）。
- 分辨率事实订正：P0 执行记录原写"2048×1152"**与产物不符**；实测为
  **客户区 1902×963**（窗口 1920×1010、运行时 receiver viewport 1902×722、截图 IHDR 1902×963
  三方印证，由 v2 分析器读出）。执行记录与整晚总结已更正，并对 2560×1440 与 2048×1152 **双向**
  声明不得外推。

## 2026-09-17 — 两候选对抗性复核：口径纠正 + 三处硬化（未部署 / 未实机）

- 复核方式：独立只读审查（另一 agent），以 2026-09-15 artifact 副本为"抽取前"基线；
  结论：**S2 未发现行为不等价**（差分门缺失属证据缺口）；**Gap B 的 A0-A5 链本身正确且
  fail-closed，但只覆盖"记忆槽位"这一条供出路径**。
- **P1 口径纠正（重要）**：`FindOrUpdatePaletteSlotCache` 另有两条**有意保留**的未证兜底路径
  ——(a) 命中条目且 `+0x08` 合法 ⇒ 覆写记忆槽位并直接供出；(b) 未命中条目且 `+0x08` 合法 ⇒
  首见插入并直接供出。它们不做绑定/帧证明，调用方随即按槽位读 arena。因此
  **"未经帧证明的 arena 读在代码路径上不可达"是过度宣称**，已更正：
  `docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md` §8① 改为
  "记忆槽位在 producer 未确认时不再供出"，并新增口径更正段（含 (a)(b) 的显式说明与
  "若要覆盖需另立候选"）；`AutoTest/test_palette_slot_cache_producer_confirmation_static.py`
  的注释同步收窄并把两条直读路径显式锚定进断言。
- **P1 度量口径修正**：`g_paletteSlotCacheSnapshotFrameStaleRejectCount` 原先把
  "当前帧不可读 / snapshotFrameTag == 0"也计入"陈旧"，而该计数是决定"是否把
  `kPaletteSlotCacheMaxFrameTagDelta` 放宽到 2"的**唯一依据**。现加门
  `currentReadable && snapshotFrameTag != 0u`：只统计确实超差，不可证明的情形同样拒绝但不计入。
- **P2 硬化**：
  1. 槽位域判定改为**溢出安全**形式（`requiredPaletteCount != 0u && requiredPaletteCount <= 0x3A98u
     && boundSlotIndex <= 0x3A98u - requiredPaletteCount`），消除
     `boundSlotIndex + requiredPaletteCount` 的 uint32 回绕（当前调用点 req ≤ 256，
     属纵深防御；A3 的 groupCount 为引擎原始 u32，不能兜底此上界）；
  2. 内核新增契约守卫 `groupCount != 0u && matrixGroupSizes == nullptr ⇒ InvalidGroupTable`，
     消除空指针解引用路径（生产调用方不会触发，属契约硬化）；
  3. `kProducerSnapshotMaxCount` 与 `war3_model_hook.cpp` 的
     `kRenderablePartPaletteSnapshotMaxCount` 增加**单源一致性静态断言**，防止两侧漂移。
- 待办（P3，需新证据/新设计，本轮不做）：A5 的**槽位所有权**缺口（FROZEN 携带 + 槽位被重分配给
  别的 part 时 A0-A5 全过，需实机 trace 或 per-slot owner 见证）；A5 读非原子槽位缓存的
  **线程亲和性**记录（设计 §9 待核实 3）。
- 门禁：相关静态测试 EXIT 0；S2 单一来源门禁新增内核守卫断言后仍通过；构建与全量门禁在主线程
  统一复跑（见同 checkpoint 的最终门禁条目）。

## 2026-09-17 — 运行时开关清册按上级 Q4(b) 口径回写（仅文档）

- 目标：闭合"三分类 A 类 156 项曾被当作缺陷标记"与 Q4(b)"env 全属内部面"之间的矛盾。
- 改动（`docs/plan/2026-09-16-runtime-switch-triage.md`）：新增 §0 口径裁定（412 env 全属内部面、
  玩家入口只认 Ctrl+F1 面板与 JAPI／`warvk:v1`、不删不改默认、85 待判定保持、A/B/C 判定标准不变）；
  §1.1-2、§3 前言、§9.3 三处把"缺玩家向文档"改写为**预期状态（内部面）**；文末新增 §10.6 关闭项
  声明未完成边界只剩两条。所有改动行带 `（Q4(b)）` 标记便于回查。
- 身份：340,116 B / 1,439 行 / `DA217FF9…E115B` → **341,920 B / 1,454 行 /
  `B25E14113202EA0429A857D60F537C554FF181482EF1B206FC1D050F83645E76`**。
- **主线程独立核验**：字节/SHA/行数一致；`DXVK_WAR3_` 编号数据行仍 **836**（未动）；
  836 行中带 `（Q4(b)）` 标记 **0** 行；§9.2 计数式与 §10 各项计数原样；
  §0 与 §10.6 存在；台账新增行含新 SHA。仅文档改动，未触及 src/测试/meson，未构建。

## 2026-09-17 — S2 防回退门禁：共享内核单一来源静态测试（未部署 / 未实机）

- 新增 `AutoTest/test_runtime_group_palette_kernel_single_source_static.py`（结构性门禁），钉死：
  1. 内核存在且两个 fallback 集合（`MatrixAndPoseRemap` / `MatrixPoseAndUniformRoot`）都在，
     `TryBuildRuntimeGroupPaletteKernel` / `ScanRuntimeGroupPaletteSlots` 为唯一定义点；
  2. **内核纯度**：剥离注释后，内核代码段不得出现 `SafeRead*` / `GetModuleHandleA` /
     `IsReadableRange` / `DecodeRuntimePoseMatrix48` / `renderablePart` / `paletteSlotIndex` /
     `kGlobalPaletteBufferRva` / `Producer` / `Selection` / `arena` / `std::min`，
     且只 include `util_matrix.h`（不得 include 记录类型头）；
  3. **重复实现已消除**：被改造的两个 `TryBuildRuntimeGroupPalette` 函数体内不得再出现
     `Matrix4 accum(0.0f)` / `accum / float(groupSize)` / `accum += ` / `matrixGroupSizes[`，
     且必须调用内核（断言收敛到函数体，因为 core.cpp 另有一处 tupleCount 变体不属 S2 范围）；
  4. **D2 差异不得被合并**：core 只能用 `MatrixPoseAndUniformRoot`、upper 只能用
     `MatrixAndPoseRemap`，且每侧只出现一次 fallback 集合选择；
  5. 来源链与前置校验仍在适配层（`tryEngineDirectPosePalette`、producer 快照查询、
     `FindOrUpdatePaletteSlotCache`、`NoSkinningData`、`uniqueGroupSlots`、`logFailure`；
     upper 保留 `vertexGroupCount`/`matrixGroupCount` 派生）；两函数签名与 upper 唯一调用点形态不变；
  6. 生产函数测试文件与 meson 目标在位，且测试**真的调用**生产函数（非纯字符串断言）。
- 门禁：该测试 EXIT 0；静态全量 **243/243**；`ninja -C build32 -n` no work；meson **73/73**。
- 边界：本门禁只证明结构与接线，**不证明画面/性能**；S2 候选仍未实机。

## 2026-09-17 — Gap B 补强实施：绑定 groupCount + 帧新鲜度见证 + 细分拒绝（未部署 / 未实机）

- 性质：上级裁定确认的 Gap B 缺口（B-1..B-6）实施，设计见
  `docs/plan/2026-09-17-gapb-confirmation-strengthening-design.md`。**静态合同完成，
  画面行为未验证**；本候选与实机过的 `8CECC495…` **不是同一个 DLL**，不得混用结论。
- 交付（`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp` 为主）：
  1. `FindOrUpdatePaletteSlotCache` 增加 `requiredPaletteCount` 参数；调用点（原 6584）
     转发 `requiredCount`，并补 `slot + requiredCount <= 0x3A98` 区间上界。
  2. 记忆槽位供出判定改为 A0-A5 全链：绑定命中 → 槽位域合法 → 与记忆一致 →
     **groupCount >= requiredPaletteCount**（对齐 device 侧口径）→ 绑定帧不旧
     （`QueryCurrentPaletteFrameTag`）→ 逐槽区间帧同源
     （`QueryBlendedPaletteFrameTagRange`：missing==0 && min==max==绑定帧）。
     任一失败仍走聚合拒绝 + `return 0xFFFFFFFFu`，**不写任何状态（无负缓存）**。
  3. 容差常量 `kPaletteSlotCacheMaxFrameTagDelta = 0u`（严格同帧，对齐 device 最强先例）；
     放宽必须由实机反例门证据驱动的下一候选，且不得超过 2。
  4. 快照优先路径：producer 上限 64、`size() == requiredCount`（原 `>=`）、
     快照 frameTag 必须可读且为当前帧；陈旧则计数拒绝并继续走 slot/CPU 路径。
  5. 新增 6 个本地计数（1 个内部不导出）+ 5 个 Query 访问器，沿既有链导出到
     bridge summary / diagnostics hub / control plane / perf monitor JSON：
     `…ShadowCoreFrameProofServedCount`、`…GroupShortRejectedCount`、
     `…BindingFrameStaleRejectedCount`、`…SlotRangeStaleRejectedCount`、
     `…SnapshotFrameStaleRejectedCount`（细分和 == `RejectedStale` 可对账）。
  6. **边界**：只做设计 §4.2 的 S0-S3，**未做 S4**（快照字节仍可能来自 FROZEN 旧槽位）
     ⇒ 报告不得宣称"快照路径已排除陈旧字节"。
- 测试：`test_palette_slot_cache_producer_confirmation_static.py` 锚点升级
  （`>=` → `== size_t(requiredCount)`，语义加强）+ A3/A4/A5/区间上界/细分计数新断言；
  `test_palette_slot_cache_counters_export_static.py` FIELDS 5→10、访问器 3→8。
  跨地图会话隔离断言未改（`g_paletteSlotCacheSessionGeneration` 语义保留）。
- 门禁：build32 exit 0；`ninja -C build32 -n` **no work to do**；meson **72/72**；
  AutoTest 静态 **242/242**。DLL 35,994,449 bytes /
  SHA-256 `05BEBA32ACAAD704FC863C3535B22125E2C290CEF99DE0D61800781C48CF5E6C`。
- 未部署、未实机；实机过的冻结候选仍是 `8CECC495…`。

## 2026-09-17 — P0 实机三轮观察完成（已恢复现场 / 未宣布修复通过 / 未发布）

- 三轮：round1(M1,ON) pilot → round2(M2,ON) → round3(M2,OFF)；全部 exit 0、
  三段各 ≈600.0s、deviceLost=false、无 GPU incident、restoreOk 全 true。
  **现场已恢复原 DLL（74CC676B…DB73，哈希核验一致）；玩家目录未触碰；实机事务结束。**
- **结果**：合同 OFF 轮 Gap A 陈旧槽位拒绝累计 31,881→88,926→91,039
  （Δ21=57,045 / Δ32=2,113）⇒ 复核机制在生产路径真实工作；
  但 ServedAfterConfirm 全程 0、替代来源计数采样瞬时全 0、Gap B 三轮全 0。
- **判定**（严格按用户三证明口径）：① 观察结果（部分）；② 未覆盖；③ 未覆盖（仅有趋势）；
  反例门 未覆盖。**不得记作"阴影消失已修复"或"P0 通过"。**
- 结构发现（下一候选）：① Gap B 需先修 core.cpp:7519-7611 资源键查找；
  ② 来源分类仪表在合同 ON 下不可达（device.cpp:23166-23172 门），真实 skinned 提交
  来自 draw-time 路径且无来源归属，需在活跃路径补仪表；③ 分辨率偏差 2048×1152 已声明。
- 详证：`docs/plan/2026-09-17-p0-real-machine-execution-record.md`。
- 本轮**未修改任何 src/ 或构建**（Q5 冻结纪律）；未 commit；未发布。

## 2026-09-17 — 上级裁定落地：P0 实机方案冻结（未部署 / 未实机）

- 授权链：用户指示提交 codex 线程 `01a02e0b…` 裁决，已获
  **Q1 条件批准实机 / Q2 否决旧 DLL 基线 / Q3 批准合同 env 覆盖 / Q4 开关方向(b) /
  Q5 收窄批准 S2**（要点见台账 2026-09-17 凌晨条目，全文在 codex 线程）。
- 方案文档 `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md`
  已按裁定重写冻结：候选 `8CECC495…`、仅 `E:\Work\War3` 测试现场、
  环境矩阵无"视需要"、三段各 10 分钟、分段增量口径、同对象关联证明要求、
  停止/恢复条款、Gap B 已知局限（boundGroupCount 未参与判定——本轮结论上限
  "观察结果与路径覆盖"）。
- 测试现场只读核验：`E:\Work\War3\d3d9.dll` = 35,486,415 bytes /
  `74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73`；
  玩家目录 F855 未触碰。
- 本条目不宣称任何画面行为；实机观察尚未执行。

## 2026-09-17 — 运行时开关三分类清册（纯文档 / 不改任何默认值 / 未部署）

- 性质：长期目标"运行时开关三分类"的交付物。**纯分析文档，不改代码、不改默认值**。
- 文档：`docs/plan/2026-09-16-runtime-switch-triage.md`（137,417 bytes / 615 行 /
  SHA-256 `E0AFD406B21E3090BC7250A0B78446C69F7CF1C98A6F02AD0B6EB832EEA352EA`）。
  注意：上一会话已有一版（347 KiB，A=0/B=24/C=44/待判定=344）被本版覆盖；
  两版分类口径不同（本版允许"生产语义证据充分但缺玩家文档"列 A 并标注），差异已记入台账。
- 结论：src/ 下 412 个 `DXVK_WAR3_*` 字符串字面量开关 =
  **A 玩家/生产 156（全部缺玩家向文档）+ B 诊断/取证 152 + C dev-only/实验 19 +
  待判定 85**；另有 9 个宏/注释专用名、3 个仅文档出现的孤立名。
- 关键事实（主线程独立复核确认）：
  1. 玩家向文档面（WarVK/、README*、CHANGELOG*、docs/RELEASE_*）命中 **0** 个开关名——
     玩家配置面实际是 Ctrl+F1 面板 + JAPI，env 全属内部面。旧记录"63 个命中"作废。
  2. `DXVK_WAR3_SHADOW_WORLD_UP` 在当前源码中不可达（`s_forcedUp==1` 恒使读取分支
     不进入），属失效开关，已列入待判定并注明。
  3. 412 名单与主线程独立枚举逐一对齐，零重复零遗漏。
- 待用户裁定：A 类 156 项缺玩家文档的处理方向（补文档升 A vs 明文声明 env 全内部面）。

## 2026-09-16 — P0 计数器接线：Gap A/B 复核计数导出到报告（纯接线 / 未部署 / 未实机）

- 性质：palette 缝隙修复的**度量前提**——实机要证明"合法对象仍有替代路径"与
  "失败后可恢复"，必须先能在报告里读到这 5 个计数器。**本条目只完成接线，
  不包含任何画面行为证据**；行为不变（只读导出进程累计原子计数）。
- 交付项（5 个计数器，进程累计 uint64）：
  - Gap A（device 侧，d3d9_device.cpp）：
    `g_devicePaletteSlotCacheServedAfterConfirmCount` / `…RejectedStaleCount`，
    声明从 8507 行前移至匿名命名空间前段（与 shadow append 计数同区），新增
    `war3_diag::QueryDevicePaletteSlotCacheServedAfterConfirmCount` /
    `…RejectedStaleCount` 访问器（与 QueryShadowAppendTotal 同模式）。
  - Gap B（shadow-core，war3_shadow_renderer_core.cpp）：
    `g_paletteSlotCacheServedAfterConfirmCount` / `…RejectedStaleCount` /
    `…ProducerSnapshotFallbackCount`，新增
    `shadow::QueryPaletteSlotCache{…}` 三个访问器并声明进
    `war3_shadow_renderer_core.h`。
  - 报告链：bridge summary 结构体 5 个新字段
    （`semanticSceneSkinnedPaletteSlotCache{Device,ShadowCore}…`，
    war3_shadow_runtime_bridge.h）→ bridge.cpp summary 填充（device 两个经
    war3_diag getter、core 三个经 shadow:: getter）→ war3_diagnostics_hub.h/.cpp
    字段 + 填充 + JSON → war3_control_plane.cpp JSON →
    war3_perf_monitor.cpp `shadowRuntimeV2Summary` JSON（直读最新 summary）。
- 测试：新增 `AutoTest/test_palette_slot_cache_counters_export_static.py`
  （计数器本体、Query 访问器声明/定义、summary 字段与填充、三出口 JSON 键全钉死；
  静态脚本 241→242）。
- 门禁：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；
  `meson test` **72/72**；AutoTest 静态 **242/242**。
  DLL 35,984,240 bytes / SHA-256 `8CECC495595D3BC6EC9D25AFBD1F0BA9F44CF9B18BBFB7E784B60B74C9569ADE`。
- 边界：计数器接线完成 ≠ 三项画面证明完成；组合候选实机验证仍待用户授权
  （docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md）。

## 2026-09-16 — P0 Gap A：device 侧 palette 记忆槽位 producer 复核（行为变更 / 未部署 / 未实机）

- 性质：用户裁定的 **P0（眼前发布问题）** 之一，对应工单
  `docs/plan/2026-09-16-palette-gap-fix-workorder.md` 的 Gap A（B-S1）。
  **这是路由式加固，不是"只改成返回失败"**；未部署、未实机、**不得记作"阴影消失已修复"**。
  本条目只覆盖 device 侧（Gap A）；shadow-core 侧 Gap B 由主线程先行完成（见下方条目）。
- 问题（工单 + 读码确认）：合同 OFF（普通/Release 生产路径）下，
  `War3TryBuildLiveRuntimeGroupPalette` 内 `resolvePaletteSlotIndex` 的
  `useCachedEntry` 在 `RenderablePart + 0x08` 槽位非法且 producer 绑定查询 miss 时，
  **无条件供出记忆槽位** `return entry.paletteSlotIndex;`，使下游按 slot 键的读者
  （Game.dll+0xBC6BD0 全局 arena、`QueryBlendedPaletteBySlotIndex`）可能读到
  **别的对象今天的矩阵字节**（palette 撕裂来源之一）。
- 交付项（`src/d3d9/d3d9_device.cpp`）：
  1. `queryProducerBindingSlot` 扩展为同时取回 producer 记录的 groupCount/frameTag
     （`QueryRenderablePartPaletteSlot` 签名本就支持，war3_model_hook.h:499）。
  2. `PaletteSlotCacheEntry` 新增 `paletteGroupCount` / `paletteFrameTag` 字段，
     写入与复核通过时由 producer 查询一并填充；renderablePart 身份与 mapEpoch 复核、
     直映射加速器 `s_paletteSlotCacheLookup` 与 `s_paletteSlotCacheCursor++` 环形分配全部保留。
  3. `useCachedEntry`：`+0x08` 当前槽位合法的写入路径不变；槽位非法时仅当
     **producer 绑定命中、且 producer 记录的槽位等于记忆槽位、且 producer groupCount
     ≥ 本次 requiredPaletteCount** 时才供出 `entry.paletteSlotIndex`（并刷新 entry）。
     任一不满足 → 返回 `0xFFFFFFFFu`，调用方随即跳过 slot 键读者，落到
     **PoseFallback**（PoseRegistry 已发布姿态、±0xA0 偏移、
     `QueryRenderablePartOwnerRuntimeModel` 反查）——合法对象的替代路径保持可达；
     拒绝不写入负缓存，下帧 +0x08/producer 绑定恢复后即可重新命中。
  4. 合同 ON 严格分支（`skin::ContractEnabled()`）代码未改，本修复只影响合同 OFF 路径。
- 新增度量（与 Gap B 同风格的本地原子计数，**尚未导出到报告**，属后续接线）：
  `g_devicePaletteSlotCacheServedAfterConfirmCount`（经复核仍命中，证明未误杀）、
  `g_devicePaletteSlotCacheRejectedStaleCount`（producer miss / 槽位不一致 /
  groupCount 不兼容被拒）。用户指定的三项证明仍需实机/隔离数据：
  ① 错误矩阵不再被使用 ② 合法对象经替代路径成功投影 ③ 失败后可恢复；
  **不得只凭拒绝计数增加宣布通过**。
- 测试：新增 `AutoTest/test_device_palette_slot_cache_producer_confirmation_static.py`
  （拒绝侧顺序 + 复核三要素 + 供出仅在复核后 + 加速器合同保留 + slotIndex 非法不进
  slot 块 + PoseFallback 可达 + 合同 ON 分支未改）；
  `test_live_palette_slot_cache_accelerator_static.py` 仅把 `queryProducerBindingSlot()`
  锚点改为 `queryProducerBindingSlot(`（签名扩展所致的行锚点漂移），断言语义不变，
  `return entry.paletteSlotIndex` 断言继续成立。
- 门禁：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；
  `meson test` **72/72**；AutoTest 静态 **241/241**（240 + 1 新增）。
  DLL 35,979,538 bytes / SHA-256 `F73F49B0295A46DF230BC83DCED85CB9F5D40114BAB23335E94A2621C293C039`。

## 2026-09-16 — 固定门禁：诊断/候选构建选项默认必须关闭（防止候选冒充发布 / 未部署）

- 性质：对准目标项"建立防止回退的固定门禁"与纪律"候选不得冒充稳定"。**纯测试新增**，未部署。
- 规则：
  1. meson 的 10 个已知诊断/候选选项必须存在且默认 `false`；
  2. 通用规则：任何 `warvk_*` 且含 `_dev`/`candidate` 的布尔选项不得默认 `true`；
  3. 源码层 `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` 的 fallback 必须为 0；
  4. 生产 `d3d9_cpp_args` 中的 `-DWARVK_*_DEV=1` / `-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1`
     必须位于对应 `if get_option(...)` 守卫内（测试可执行目标可直接开 DEV，不受本规则约束）。
- 文件：`AutoTest/test_diagnostic_build_options_default_off_static.py`（静态脚本 239→240）。
- 说明：本门禁防止"候选配置被焊进树里"，不证明任何画面行为。

## 2026-09-16 — 固定门禁：d3d9_device.cpp 语义选择职责白名单（防止职责回流 / 未部署）

- 性质：对准目标项"建立防止回退的固定门禁"。**纯测试新增，无 C++ 行为变更**，未部署。
- 规则：冻结 `d3d9_device.cpp` 中现有 **37 个** War3 语义选择符号
  （`War3*(Resolve|Select|Choose|Decide|Classify|Should|TryBuild|TryFind|TryPopulate|Promote|CanPromote)*`）。
  **迁移只允许让集合变小**；新增或改名绕过即判失败，并提示"应放到语义/渲染模块，
  或确有理由时同一改动内扩充 FROZEN 并说明原因"。
- 门禁自身已做有效性验证：合成一个回流符号（`War3ShouldSelectFancyNewThing`）能被识别为违规。
- 依据文档：`docs/plan/2026-09-16-device-semantic-responsibility-migration.md`
  （device.cpp 实测 52,387 行；语义/contract 1454、palette 1329、Stage13/S1 790 等，36 个语义选择函数定义）。
- 文件：`AutoTest/test_device_semantic_responsibility_budget_static.py`（静态脚本总数 238→239）。
- 说明：静态合同只防止职责回流，**不证明画面行为正确**。

## 2026-09-16 — P0 Gap B：shadow-core palette 槽位缓存改为 producer 复核 + 快照优先（行为变更 / 未部署 / 未实机）

- 性质：用户裁定的 **P0（眼前发布问题）** 之一，对应工单
  `docs/plan/2026-09-16-palette-gap-fix-workorder.md` 的 Gap B。
  **这是路由式加固，不是"只改成返回失败"**；未部署、未实机、**不得记作"阴影消失已修复"**。
  本条目只覆盖 shadow-core 侧（Gap B）；device 侧 Gap A（`useCachedEntry`）另行实施。
- 问题（主线程读码确认）：`war3_shadow_renderer_core.cpp` 的
  `FindOrUpdatePaletteSlotCache` 在 `RenderablePart + 0x08` 槽位非法时，
  **完全不问 producer、无帧校验**，直接返回该 part 上次记忆的槽位；
  调用方 `tryEngineDirectPosePalette` 随即 `×48` 读取 Game.dll+0xBC6BD0 arena
  ——可能读到**别的对象今天的矩阵字节**（即 palette 撕裂的来源之一）。
- 交付项：
  1. 槽位非法分支改为先 `dxvk::war3::model::QueryRenderablePartPaletteSlot` 复核：
     仅当 producer 记录该 part 当前仍绑定**同一槽位**时才供出缓存值。
  2. 复核不通过 → 返回 `0xFFFFFFFF`，使该 slot 读取路径失效（不再读 arena），
     由调用方落到**经过证明的替代来源**。
  3. `tryEngineDirectPosePalette` 新增 **producer 按 renderablePart 记录的完整调色板快照**
     优先路径（`QueryRenderablePartPaletteSnapshot`，按 `requiredCount` 校验数量），
     与 device 侧同源策略：与 writer 绑定更紧，规避 slot 复用/相位差造成的 stale bytes。
  4. 原 CPU 构建路径（`resource.matrixIndices` / `pose.matrixPalette`）保留为下游替代来源，
     因此**合法对象仍有可投影路径**，而不是被单纯拒绝。
  5. 既有跨地图会话隔离语义（`g_paletteSlotCacheSessionGeneration`）保持不变。
- 新增度量：`g_paletteSlotCacheServedAfterConfirmCount`（未误杀的快路径命中）、
  `g_paletteSlotCacheRejectedStaleCount`（陈旧被拒）、
  `g_paletteSlotCacheProducerSnapshotFallbackCount`（快照接管）。
  **尚未导出到报告**（属后续接线；当前只用于本地计数与静态合同）。
  用户指定的三项证明仍需实机/隔离数据：① 错误矩阵不再被使用 ② 合法对象经替代路径成功投影
  ③ 失败后可恢复；**不得只凭拒绝计数增加宣布通过**。
- 测试：新增 `AutoTest/test_palette_slot_cache_producer_confirmation_static.py`
  （拒绝侧 + 快照优先顺序 + CPU 替代路径仍在 + 合同 ON 严格路径未被改动）。
- 门禁：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；
  `meson test` **72/72**；AutoTest 静态 **238/238**。
  DLL 35,946,589 bytes / SHA-256 `1C314090821487505D66298B7CF003B1D5F87E036800EE1E9D177296B6C955C6`。

## 2026-09-16 — 合并 P2 批次3：legacy path-blocker EntryGate 收窄（行为变更 / 未部署 / 待实机）

- 性质：**接线与判定变更，不是修复，也没有解决阴影消失。** 静态通过 ≠ 画面行为已验证。未部署、未实机。未做批次 1 默认值翻转、未做 Stage13 预检/content-persistent（批次 6）、未扩批次 4/5。
- 交付项：
  1. Terrain 非 S1 进 `pathBlockerLane` 必须有对象证据（currentObj / TLS `HasAnyContext` / Decorations·WorldObjects·SelectionOverlay tag）。
  2. WorldObject 进 lane 必须有对象证据 **或** active CurrentDraw dispatch（Common / Special / TransparentType0）。
  3. capture 路径 `objectCasterByStage`（含 S13）同样要求 dispatch-backed。pose 路径 `objectCasterByStage`（无 S13）未改。
- 正向面：有对象证据的 Terrain/WorldObject、有 Common/Special/TransparentType0 dispatch 的 WorldObject、以及三个 batch tag 仍独立进 lane；TLS tag / currentObj 仍可独立作为 object caster。
- 测试：新增 `AutoTest/test_path_blocker_entry_gate_narrowing_static.py`（拒绝侧 + 正向进 lane）。`test_bridge_ramp_shadow_safety_static.py` 无需改锚点。
- **待实机验证（缺一不可）**：
  (a) 不该投影的 path-blocker 被拦住；
  (b) 正常单位、建筑、桥梁、装饰物仍能投影；
  (c) 失败后能恢复正常，不是持续遗漏。
- 验证：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；`meson test -C build32` **72/72**；AutoTest 静态全量 **237/237**。
- 最终 DLL：35,946,291 bytes，SHA-256 `2242185F1AA6303E2BFD5BCE4B44C7618918E66DA0F59209A66DFA265C4594DA`。

## 2026-09-16 — 合并 P2 批次2：path-blocker 五源提交闸（fail-closed 对齐 / 未部署）

- 性质：从 A `88089cd` 按符号移植 semantic-core + NativeD3D9 canonical 的 path-blocker 五源提交闸。rawcode 不可靠的 blocker 候选在提交前被五源证据复核，证明失败不提交（显示无阴影而非错阴影）。未改对外 env 默认，未部署、未实机。未做 legacy EntryGate 收窄 / Stage13 预检（批次 3）。
- 公共头：新建 `src/d3d9/war3/war3_path_blocker_evidence.h`，共享判定只此一份。canonical/semantic 两侧只保留同名薄包装（`CanonicalShouldSubmitPacket` / `SemanticCoreShouldSubmitResolvedPacket`）以避免第三份拷贝。
- 五源：现有 rawcode FourCC → jHandle `findByHandle` → CWidget `+0x0C` magic `0x2B5DB42C` / `+0x30` rawcode（unitPtr 可覆盖错误非零 rawcode）→ 地下小型 marker 第五源。marker fallback 拒绝动态 skinned unit 证据。
- 插入点（按 B 现符号，非 A 行号）：canonical `buildCanonicalFrame` emplace 前 `continue` 跳过；semantic `buildFrameChunk` 两处 resolve 成功后、`ensureFrameBuiltForContract` upper-layer emplace 前。
- 编译期闸保持 B 现值：`kPathBlockerHideEnabled=true`、`kPathBlockerBelowGroundFlatMarkerGateEnabled=true`。
- 测试：移植并按公共头重锚 `AutoTest/test_shadow_path_blocker_five_source_parity_static.py`（widget 偏移、四源顺序、marker 拒 skinned unit、八 FourCC）。既有静态测试无需改锚点。
- 验证：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；`meson test -C build32` **72/72**；AutoTest 静态全量 **236/236**。
- 最终 DLL：35,946,291 bytes，SHA-256 `D180E4C32E021797F73B5B1044D93019A10A8C25C13B2EB89DC2D0A37C6414F6`。

## 2026-09-16 — 合并 P2 批次0：native static shadow producer 观测层（MAINTENANCE / 未部署 / 行为不变）

- 性质：从 A `88089cd` 按符号移植 RegisterImage/StaticStampPath 的计数器、14 个 Query API、DecideRegisterImage 完整策略分支，以及 summary/control_plane/perf JSON 导出。未安装任何新 hook，未改玩家可见默认。未部署、未实机。
- 安装开关保持 B 现值：`kNativeShadowRegisterImageHookEnabled=false`、`kNativeShadowStaticStampPathHookEnabled=false`。hook 不安，运行时计数器为零是预期。
- doodad 默认保持 B 现值：`kNativeDoodadStaticStampRuntimeGateDefault=false`（用户裁定未下）。
- 新增休眠默认阻断常量（均为 false，不改变现网行为）：`kNativeShadowBlockStaticStampPathByDefault`、`kNativeShadowRegisterBlockStaticStampByDefault`、`kNativeShadowRegisterBlockShadowTextureKeyByDefault`。
- StaticStamp hook 函数体补 enter/cleanup(enable==0) 早退/blocked 计数；RegisterImage 既有 source 分桶计数补 Query 读取器。
- filter policy：斜杠/反斜杠两种 `ReplaceableTextures\\Shadows\\` / `ReplaceableTextures/Shadows/` 与 selection 对应分支；白名单 source / selection key / StaticStamp 默认拦（dormant false）/ shadow texture key（dormant false）；`mode<1` 不再立刻 PassThrough。
- 未移植：stage13 content-persistent、alias、EntryGate 收窄、五源闸（批次 2-6）。未动 doodad hook 逻辑。
- 测试：新增 `AutoTest/test_native_static_shadow_producer_observability_static.py`（14 Query 符号、不安钩、斜杠/反斜杠策略、dormant false）。既有 doodad/ui-decal 静态测试无需改锚点。
- 验证：`build32_safe src/d3d9/d3d9.dll -j8` exit 0；`ninja -C build32 -n` no-work；`meson test -C build32` **72/72**；AutoTest 静态全量 **235/235**（234 基线 + 1 新脚本）。
- 最终 DLL：35,936,689 bytes，SHA-256 `496944C36DE0BCF8BBFC55B1848F89F4C89E482B2EC457ABF4602784A64EAA6E`。

## 2026-09-16 — 合并 P5 切片6：AutoTest 静态守卫 + 分析器移植（MAINTENANCE / 未部署；P5 全部完成）

- 性质：只加 AutoTest 脚本，无 C++ 变更、DLL 身份不变、未部署未实机。P5 六切片至此收口。
- 移植并按 B 重锚：`test_data_collection_static.py`（114 入口体首 SCOPE + coverageComplete=false + env 三变量 + JSON 键）；`test_frame_timeline_static.py`（wallTimeline==17；PresentResult==10；Present 在 BeginNativeFrameSyncPresent 之后、entry-before-lock 之前；**不**断言 `timeline::Enabled() ||`；WaitForResource/LockImage 按符号保序）。
- 分析器原样：`analyze_frame_timeline.py`、`analyze_data_collection_perf_report.py` 及其 `test_analyze_*`（读 `mainThreadTimeline` / `dataCollectionTree`）。
- 未移植：`validate_data_collection_candidate.py` 及候选构建/预览/plan 脚本（manifest 闭合前不当 CI 门槛）。NativeFrameSync 分析器归 P7。
- 验证：meson test 72/72；静态 `test_*_static.py` **234/234**（232 基线 + 2 新守卫）；分析器单测 2/2。
- DLL 仍为切片5 DEV=false：35,930,606 bytes，SHA-256 `19EC511FAA257FCD9617D173902EF7C49CDA51C1571FA3075CB2E9BD74B2D13D`。

## 2026-09-16 — 合并 P5 切片5：114 个 WARVK_DATA_SCOPE 入口落地 + manifest 重生成（MAINTENANCE / 未部署）

- 性质：按符号在 B 现函数体首插入 `WARVK_DATA_SCOPE(Tag)`；默认 DEV=false 为空操作。未接 AutoTest 分析器（切片6），未部署、未实机。
- 落地 **114/114**，未落地清单为空。14 个文件各加 `war3_data_collection_tree.h`。PublishVisible 插在包装函数 `PublishVisibleRenderableFromDispatch`（包住 Body 模板整次调用）。`refreshShadowManifestFromCurrentDraw` 插在 B 真正干活的三参数实现（两参数包装只转发）。
- manifest：新建 `AutoTest/data_collection_entrypoints.json`，schemaVersion=1，coverageComplete=false，含 B 实际 file:line。
- 三次构建：① 默认 DEV=false `build32_safe` exit 0；② `meson configure -Dwarvk_data_collection_tree_dev=true` 后 build32_safe exit 0（真正编译 114 处 Scope），DLL 36,016,485 bytes / SHA-256 `03720B7517346D8F2F0027DCA214B235555ED825A316A8CA48C9B8239C20C4DE`；③ 恢复 DEV=false 再 build32_safe exit 0。F855 诊断选项仍开。
- 静态锚点：`test_v122_geoset_identity_static` 对函数体/全文比较剥掉 include 与 `WARVK_DATA_SCOPE(...)`，身份证明语义不变。
- 验证（最终 DEV=false）：ninja -C build32 -n no-work；meson test 72/72；静态 232/232。
- 最终 DLL：35,930,606 bytes，SHA-256 `19EC511FAA257FCD9617D173902EF7C49CDA51C1571FA3075CB2E9BD74B2D13D`。

## 2026-09-16 — 合并 P5 切片4：lifecycle 17 个 engine/event timeline::Scope（MAINTENANCE / 未部署）

- 性质：只在 B 既有 17 个 engine/event hook 函数体首插入 RAII Scope；不改 hook 安装集、不搬 NativeFrameSync。未接 114 WARVK_DATA_SCOPE，未部署、未实机。
- include：`war3_hook_lifecycle.cpp` 增 `war3_frame_timeline.h`。17 处 Scope 桶名与 A 相同（Hook_EventMessagePump … Hook_EngineSleepGateInner）。无跳过项。
- **deep-phase 门裁定：不移植 A 的 `|| timeline::Enabled()`**。B 仍为 `kNativeMainLoopDeepPhaseHookEnabled || War3PerfHookLevel() >= 2`。理由：A 的 OR 会让仅开 FRAME_TIMELINE=1 就安装深层 engine detour，改变 B 默认 PERF_LEVEL=1 的诊断成本合同。深层相位 timeline 只在既有 coverage 宏或 PERF_LEVEL>=2 装 hook 后才有数据。后续如需可用显式独立 env 再议。
- NativeFrameSync：不碰 B 已抽到 war3_native_capture.cpp 的路径。
- 验证：build32_safe exit 0；ninja -C build32 -n no-work；meson test 72/72；静态 232/232。静态锚点无需改。
- 最终 DLL：35,930,606 bytes，SHA-256 `EF3EB8218D10EA713BD077851E316B4EE6C70FBE312810A8845281726A71162C`。

## 2026-09-16 — 合并 P5 切片3：swapchain Present 切帧 + device/surface 观察点（MAINTENANCE / 未部署）

- 性质：只插 timeline 观察壳；不改截图/取证/设备丢失/DONOTWAIT/Flush 顺序。未接 lifecycle 17 Scope 与 114 WARVK_DATA_SCOPE，未部署、未实机。
- Present：BeginNativeFrameSyncPresent 后、entry-before-lock 前插 timeline::Present + Scope("Present")；endFrame 外包 Scope("FrameProfiler")。B 实际 9 个 HRESULT return 均记 PresentResult（含 vk-lost 早退、device-lost、无 backbuffer、windowCtx、GDI fallback、PresentImage 后 lost、success、exception lost、exception GDI fallback）。CreateBackBuffers emplace 后 BackbufferCreated。evidence/ScreenshotPresentGuard/双次 CaptureNativeAsyncScreenshot 未改。
- device：WaitForResource 外包 D3D9/WaitForResource，CS sync / Flush / GPU wait 各包 Scope（DONOTWAIT 路径无 GPU wait）；LockImage 整函数 + needsReadback 分支 Scope。
- surface LockRect：只记录真实 HRESULT，不改锁行为。frame_capture ProcessPendingFrameCapture 入口 Scope("FrameCapture/Poll")。
- 测试锚点：test_vulkan_device_lost_fail_stop_static 的 `return PresentImageGDI` 改为 `PresentImageGDI(m_window)`，因 exception 路径为记 HRESULT 引入 fallback 局部变量；仍断言 fail-stop return 在 GDI 之前。
- 验证：build32_safe exit 0；ninja -C build32 -n no-work；meson test 72/72；静态 232/232。
- 最终 DLL：35,926,510 bytes，SHA-256 `33448C63E4C8B77F2199B018BDB5F8A5C138DDFF7B1AEBF2745A7C38A34026BD`。

## 2026-09-16 — 合并 P5 切片2：perf_monitor JSON/recording 门 + HTML data-tree UI（MAINTENANCE / 未部署）

- 性质：把切片1入库的 collection/timeline 接到 perf_monitor 的 recording 门与报告导出；不接 swapchain/lifecycle/114 SCOPE，未部署、未实机。
- 头文件：`war3_perf_monitor.h` include 两个新头；ScopedCpuScope 追加 `m_timelineToken`；`setRecording` 同步 `collection::SetRecording`；ExportSnapshot **末尾**追加 dataCollectionJson/frameTimelineJson（保留 B 已演进的 ReportMeta 布局）。
- cpp：ScopedCpuScope Enter/Leave 40 桶精确 strcmp；移动赋值仍先 popScope 再 Leave 再覆盖（保留 B 合同）；shutdown/beginFrame/archiveFrame/resetHistory/export Pause+CaptureJson；JSON 键 `mainThreadTimeline` / `dataCollectionTree`；meta.env 增 DATA_COLLECTION_TREE/SAMPLE_PERIOD 与 FRAME_TIMELINE。
- HTML：`war3_perf_report_template.h` 最小插入 data-tree 线程下拉与 rebuildTree `data:` 分支，其余模板不动。
- 默认关：DEV 宏 false → collection CaptureJson 为 compiled:false；timeline 无 FRAME_TIMELINE=1 → enabled:false；Enter/Leave/NoteFrame 均为早退 bool。未改 cpuScope 名字。
- 验证：build32_safe src/d3d9/d3d9.dll -j8 exit 0；ninja -C build32 -n no-work；meson test 72/72；AutoTest 静态全量 232/232。
- 最终 DLL：35,922,304 bytes，SHA-256 `CA611EAE0B3B18D7E3FC6ECA5402EA367D141106A0753E08D2E62AA3069DD19E`。

## 2026-09-16 — 合并 P5 切片1：采集树/帧时间线独立库入库（MAINTENANCE / 未部署）

- 性质：A 树 collection/timeline 独立库原样入库 + meson 接线 + 两个 C++ 测试登记；无任何现有 src 行为接线，未接 perf_monitor / swapchain / hook_lifecycle / 114 个 WARVK_DATA_SCOPE，未部署、未实机。
- 文件清单（A→B 同相对路径，git hash-object 八文件完全一致）：
  `src/d3d9/war3/tools/war3_data_collection_tree_core.h`、
  `war3_data_collection_tree.h`、`war3_data_collection_tree.cpp`、
  `war3_frame_timeline_core.h`、`war3_frame_timeline.h`、`war3_frame_timeline.cpp`、
  `src/d3d9/war3/render/tests/war3_data_collection_tree_test.cpp`、
  `war3_frame_timeline_test.cpp`。
- meson：新增 `warvk_data_collection_tree_dev`（boolean，**默认 false**）；选项为 true 时给 d3d9 目标加 `-DWARVK_DATA_COLLECTION_TREE_DEV=1`。B 的 warvk 选项挂在 `d3d9_cpp_args` 而非根 `add_project_arguments`，因此宏接线落在 `src/d3d9/meson.build`（与 skin-palette/shadow-observers 同形），未改根 meson.build。两 cpp 加入 d3d9_src（perf_monitor 后、residency census 前）；未动 warvk_internal_frame_recorder 块。登记 `war3_data_collection_tree`（DEV=1 + TREE=1 + PERIOD=64）与 header-only `war3_frame_timeline`。
- 运行时开关默认关：采集树需 DEV 宏 + `DXVK_WAR3_DATA_COLLECTION_TREE=1`；时间线需 `DXVK_WAR3_FRAME_TIMELINE=1`。本切片无调用点，默认构建下两套均为空实现/未启用。
- 验证：build32_safe src/d3d9/d3d9.dll -j8 exit 0；ninja -C build32 -n no-work；meson test 72/72（70 基线 + 新 2）；AutoTest 静态全量 232/232。
- 最终 DLL：35,916,522 bytes，SHA-256 `A9D690D03B4FB57C200688961EF6EC0D3C69563A26C65D533D4FF589E611D963`。

## 2026-09-16 — 合并 P8：shader 公共化 + caster 接口单一来源（MAINTENANCE / 未部署）

- 性质：A 树 09-16 shader 整理的等价移植；无算法/精度改动，未部署、未实机。
- 核对：A 抽出的 17 函数在 B 的 receiver/visibility 均存在且两份彼此规范化相同；相对 A 有实质漂移、未强行合并：
  shadowMapDepth、shadowCompare、casterMaskValue、kPoisson16、sampleShadowGrid、
  sampleShadowPoisson16、sampleShadowPoisson25、sampleShadowPcf、computeViewNormal。
- 抽取（以 B 函数体为准，分段 #include 保序）：isTerrainMaskedOccluder、kPoisson25、
  rotateVec2、computeCascadeBiasScale、computeCascadePcfRadius、computeWorldUpInView、
  computeWallReceiverFactor、computeReceiverGrazingFactor、computeLightGrazingFactor、
  computeWallStabilityFactor → `subprojects/war3fx/shaders/war3_shadow_common.glsl`。
- caster GPU-skin 常量：新建 `war3_shadow_caster_interface.h`（数值与 A/B 原字面值相同），
  d3d9_war3_shadow.cpp 12 个 constexpr 改引用宏（static_assert 3→9），caster_vert.vert 8 处硬编码换宏。
- 测试：移植 AutoTest/test_shadow_caster_interface_constants_static.py（6 用例）；
  更新 test_shadow_gpu_skin_direct_caster_static.py 3 处宏断言；
  test_shadow_compare_pcf_static.py 的 kPoisson16 扫描终点改到 PART 2 include（kPoisson25 已入 common）。
- 行为证据（%TEMP%\warvk_p8_evidence）：meson 同款 glslang --target-env vulkan1.3；
  spirv-dis 函数体逐行相同（receiver 10244→10246、visibility 4912→4914、caster 944→946，
  仅各多 2 行 OpSourceExtension）；spirv-val 通过；预处理去 #line/注释后 receiver/visibility 逐行相同。
- 验证：build32_safe src/d3d9/d3d9.dll -j8 exit 0；ninja -C build32 -n no-work；
  meson test 70/70；AutoTest 静态全量 232/232。
- 最终 DLL：35,884,733 bytes，SHA-256 `D12C940882B39C6137E3E8742CB5980C902832A82970E819E4E75CBB65AEA9EF`。

## 2026-09-16 — 合并 P9（两个孤立测试登记）+ P1 缝隙分析落盘（MAINTENANCE / 未部署）

- 性质：A 独有改动逐项移植的第二批；无 C++ 生产代码变更、DLL 身份不变、未部署未实机。
- P9：war3_cpu_skin_mt_controller_contract 与 war3_persistent_gpu_package_recording_authority
  两个既有但未编译的测试源登记进 src/d3d9/meson.build（源文件与 A 为 git 级相同、仅行尾差异，
  已用 Node 逐字节核对）。行为核对：两 cpp 只编译进独立 test 可执行文件，不进 d3d9_src。
- 守卫测试同步改写（沿用 A 09-16 语义）：原断言"该 cpp 不得出现在 meson"改为"测试目标必须已登记、
  且对应 cpp 不得在 d3d9_src（DLL 源列表）区域"——登记不再是违规，生产隔离合同保留。
- P1 缝隙分析（只读，grok-4.6 子代理）落盘 docs/research/2026-09-16-palette-provenance-gap-analysis.md：
  结论是本树合同 ON 主路径已覆盖 A 的三模式且更严；真正待补的是本树自己的两个未设防缓存
  （合同 OFF 的 useCachedEntry、shadow-core 的 FindOrUpdatePaletteSlotCache），
  A 的 TLS 三元组缓存/快路径/FROZEN 刷新不移植。执行归入阶段 2 数据链，待排期。
- 验证：meson test 70/70（68 基线 + 2 新登记）；ninja -C build32 -n no-work；
  静态全量 231/231；build32 DLL SHA-256 前缀仍 F855A67C（无 C++ 变更）。

## 2026-09-16 — 合并起点：静态基线 227/231 → 231/231（MAINTENANCE / 未部署）

- 性质：两树合并（B 为唯一集成主线）的首个 checkpoint；仅 AutoTest  tooling/test 层，
  无 C++/shader 变更，未部署、未实机。
- 基线事实：本树静态起点 227/231；meson test 68/68（F855 诊断配置 build32）。
- 修复 1（P10 移植）：war3_autotest_mcp.py 的 mcp 2.x FastMCP 兼容 stub 从 A 树逐字移植
  （行为核对：B 的 FastMCP 用法与 A 同形态——实例化、tool 装饰器、run；stub 仅在
  ModuleNotFoundError 时激活，真正服务时仍报错提示 pin 'mcp<2'）。
  bridge_ramp_low_disk_probe / gpu_skin_p4_safe_index_proof / issue5_shadow_observe_analysis
  三个传递导入失败转绿。
- 修复 2（B 侧既有卫生）：test_trusted_current_palette_rebuild_bypass_static 的锚点
  过期——F855 合同集成后 drawTimeCapturedPaletteProvenance 在重建成功时被重赋值
  （d3d9_device.cpp:22566），不能保持 const。锚点改为 auto，并补两条合同门断言
  （ContractEnabled/CanReplace 门控替换、provenance 重赋值）使测试覆盖 F855 新增行为。
- 验证：静态全量 231/231；ninja -C build32 -n no-work（本轮无 C++ 变更，构建面不变）；
  meson 基线 68/68 未受影响。
- 台账：docs/plan/2026-09-16-merge-execution-ledger.md（P10 部分完成）。

## 2026-09-16 — 用户拍板：B 为唯一集成主线（DOCS ONLY）

- 用户决定：本树（v1.22 集成线）为唯一集成主线；A 树（dxvk 工作区，
  native-shadow-stable-baseline-20260830 + 未提交工作）保留为只读参考，
  其确有价值的独有改动逐项移植进本树。
- 差异清单（主线程+子代理只读分析）在 A 树 docs/plan/2026-09-16-tree-merge-diff-inventory.md；
  执行台账与移植流程见 docs/plan/2026-09-16-merge-execution-ledger.md（本树）。
- 用户三点执行约束：① palette 以本树 skin palette 来源合同为基础，A 的槽位缓存检查仅作
  查漏补充、不得替换；② frame timeline 与坏帧取证不是二选一，两套都保留；③ 禁止按
  diffstat 推断严格超集，hook/生命周期/默认值/shader 接口逐项核对；清单旧描述以 F855A67C
  诊断构建校准（palette 合同在该构建编译期默认开、隔离 R5 有 16 个 palette-selection 事件）。
- 本轮仅文档：台账新增 + 本日志；无源码/构建/部署/实机变更。

## 2026-09-16 — 高压图低内存撕裂取证候选（PLAYER DIAGNOSTIC，未发布）

- 新增编译期独立且默认关闭的 `warvk_skin_palette_contract_candidate`；本专用内部构建默认开启严格
  蒙皮来源合同，普通/Release构建仍为0，显式环境0仍可做旧路径对照。无需PS1或后台连接即可从
  普通 `war3.exe` 启动内置取证。
- 内部高压档把CPU事件环262144→65536、原始输入槽576→224、1440p图片前窗256→96（后窗4、
  目标1000ms不变）。宿主数据上界约320.5→110.75 MiB，降约65%；保留的draw几何/索引/权重/
  矩阵/来源字段不裁剪，manifest仍如实报告不足一秒或丢失。
- source-consistent DLL 为35,884,733 bytes / `F855A67C61A82686EF2CA4C4CE3E688C1BB7966004DDE014D82B4779EDDA39DD`，
  PE32/i386。全量一致重编176动作及后续精确2-edge修订均Below Normal/-j2，最终exact DLL no-work。
- 最终隔离2560x1440 R5在零取证环境覆盖下验证编译默认：移动视角8次、窗口快捷键、DLL自主导出，
  42图/预窗1.0108384秒、3930个raw draw可重建、16个palette selection；零global input/watcher/
  frame-control命令，正常退出0、恢复测试DLL、玩家DLL/地图不变、零dump/GPU事件。receipt为
  1,440 bytes / `76921C4B7D64BC8C2495A2162E85F9A5D98780C9AD5D0983CF9ED0636E4443E5`。
- “生与死”首轮AutoTest `-loadfile` 100秒停留菜单，未进入地图；进程未崩溃且结算干净，但不得冒充
  高压进图通过。玩家手工进图、正常阴影覆盖和低频撕裂仍是发布前门。详见
  [候选证据](2026-09-16-high-pressure-fissure-evidence-candidate.md)。根CHANGELOG不更新。

## 2026-09-16 — E2真实32→64 CPU实验已通过（产品未接入）

- `native-r2/receipt.json` **16/16实际场景**通过，含最大/短历史、慢消费、受控拒绝断连、坏包/终态、
  四种新旧profile拒绝和最后新nonce恢复；SHA `2980CECD6C9C4DC3CBFB1CA3896BC8D85D40D02A4048CDFD77AA6F3830B3BE69`。
  最大history为64位私有98MiB，host实测private增98.27MiB，x86active private增3.71MiB、VA增6.05MiB，
  结束private增0、真实事务句柄111→111。全部262144保留事件经独立Python SHA重算一致。
- 受影响旧P2/P3共55个实际场景仍通过；8模块127个定向unittest方法与三文件py_compile通过，
  不是全量回归。详细原始证据、失败、启动观察阶段与下一阶段准入见
  [E2收口报告](2026-09-16-recorder-offload-e2-verification.md)。
- 无产品DLL/Ninja/部署/游戏/GPU，CPU实验不能宣称游戏节省98MiB或闪退已修复。E3异步产品owner尚未实现。
- 最终独立复核`recorder_offload_e2_parent_20260916/final-receipt.json`为22447 bytes /
  `756B2D4FA8835299F2B5BFFBB43142A268954426CA2C4C2F7B0A221C711471C5`：E2/P2/P3源码、编译器、exe及
  E2原始输出身份仍匹配，127方法再次通过，所有helper/命名容器结算。原299文件仅协议三文件、
  AGENTS/实验README/本日志六项授权变化，新增11路径，最终309 dirty、真实index不变、diff check0。
  相关游戏/编译/实验进程0。Kimi最终只读复核完成，Gemini仍为context失败后idle；未擅自重试。
  按E2阶段合同暂停`warvk-2`监督（保留原prompt/周期，仅status变PAUSED），不自动越过E3产品owner准入。

## 2026-09-16 — E2合同冻结与双DSH派发过程（失败证据保留）

- 用户要求连续推进并直接wait子任务，不再仅派发后结束。E1不重做；E2仅CPU实验，
  [合同](../plan/2026-09-16-recorder-offload-e2-contract.md)固定32位小入口/窗口、64位私有历史、
  trigger单一worker线性化、严格profile10、缺Seal/错误退出不得伪造完整性。
- E2安全备份ref `codex/backup-before-recorder-offload-e2-20260916`，commit
  `454ba2b8b360694e0b65c6f2a21c28c54e5fce3d`，原字节/index/bundle位于
  `D:/WarVK-Backups/20260916-recorder-offload-e2/`。展开untracked计数302（原298加4新文件），
  不能把默认git status折叠目录的270行误报为路径丢失；HEAD仍ae89054，相关进程0。
- Kimi任务 `recorder-e2-host-20260916-r1` 只实现64位consumer及自身报告；Gemini任务
  `recorder-e2-analyzer-20260916-r1` 只实现独立Python判定器/测试及自身报告。
  主线程负责公共协议、32位实际worker和最终验证。源代码只apply_patch，不共享写入文件。
- DSH MCP当前无context用量/compact接口；不假定支持文本slash指令，不改会话数据库。
  使用精简任务及落盘合同保连续性。尚无本批native/IPC内存证明，不改玩家DLL或稳定日志。
- Gemini确认宿主只有pwsh而无apply_patch，按约束零写入停笔。后续调整为子任务提交完整补丁文本，
  主线程统一apply_patch；Kimi同样通知。不是授权脚本绕过。Gemini续任务ID为
  `recorder-e2-analyzer-20260916-r2-patch`，Kimi补充ID为`recorder-e2-host-20260916-r1-tools`。
- 主线程x86实验worker已在`client-r1`完成BelowNormal/Werror编译及PE32核验；0运行场景，不能称E2通过。
  受影响旧P3实际CPU回归`p3-regression-r1` **14/14场景**通过（含慢消费、断连、恢复、错profile及soak）。
  P2回归尚在运行。原299份备份文件中仅本日志及slot_wire/runtime三份授权协议文件变化，index原SHA不变。
  标准git diff --check exit0；一次错误使用core.autocrlf=false导致CRLF被当空白报错，未修改源码去迎合错误配置。
- 旧P2共享槽实际门`p2-regression-r1` **41/41场景**通过，与P3共55个旧接口场景；非产品回归。
  Kimi补丁已应用，主线程补齐active permit/nonce/epoch/长度及Begin固定session、outer decode错误和QPC失败。
  `build-r1`主线程新增比较使用Nonce未定义的!=而编译失败，改为既有operator==取反，保留失败记录。
- Gemini补丁任务真实失败为`CONTEXT_WINDOW_EXCEEDED`，没有final代码。DSH无压缩接口，未重试同一
  超限上下文、改模型/数据库或新建会话；主线程接手独立Pythonstruct验收器，Kimi改做两文件只读复审。
- `native-r1`最大历史真实传输262304条、host保留262144条/102760448 bytes并零lost、host正常退出，
  但x86 handle 110→111导致整门失败，**不得当E2通过**。`handles-r1`短窗只读诊断为110→110，
  故还不能把长门+1断言为泄漏或首次依赖初始化；进一步长窗诊断中。图片/GPU及玩家DLL均未触碰。
  Python首轮7方法中一个label padding夹具多写1个NUL，修为精确32bytes后7方法通过（含循环tamper），
  不把方法数当独立场景数；最终native和独立oracle联合仍未收口。
- 长窗`handles-normal-r2`仍110→111；只读诊断新增Event+ntdll入口Thread。无取证事务的
  `handles-idle-r1`55秒空闲对照也110→111，证明仅凭总数变化不能归因为recorder泄漏。
  未关闭系统句柄，未声称具体系统内部根因。最大场景单独记录相同启动空窗后再测事务，
  句柄等值、内存、物理join/容器消失门不放宽；原失败保留，不作为成功数据。
  主线程补强Python control/copy/首包失败代数，Kimi两文件只读复审已完成，最终native待验证。

## 2026-09-16 — CPU取证历史外置 E1 / Gemini 临时替换 GLM（CPU门，不是产品迁移）

- 用户批准正式开始CPU历史外置，并将机械任务临时改为 `cli-proxy-api/gemini-3.8-flash-high`；
  核心线程保持 `cli-proxy-api/kimi-k3`。开工284个dirty路径已原字节备份至
  `D:/WarVK-Backups/20260916-recorder-offload/`，Git安全ref为
  `codex/backup-before-recorder-offload-20260916` / `b73e39df1681de6b5af5206a9e78b447638c5a14`；
  原分支、HEAD与真实index保持不变。模型更换不意味着免除主线程审核。
- 主线程先冻结[CPU外置合同](../plan/2026-09-16-recorder-cpu-offload-contract.md)，实现真实Event的
  小型有界MPSC入口；Gemini实现逐字段little-endian codec/独立Python样本；Kimi实现唯一历史owner。
  每条成功入队记录有连续publication序号；丢失尝试另计数。禁止把失败事件的旧ticket语义冒充新schema，
  不把已有SPSC给多线程使用，不将大历史环重新映回x86。
- 主线程审查收紧：codec输出别名必须在清零前拒绝；固定trigger之后Data/Seal均须重复精确cut，
  不能用NoTrigger撤销；post范围先做无加法溢出的差值检查，ordinal/sequence不回绕；visitor重入
  accept造成Fault后不得报告导出成功。主线程增加真实queue→codec→HistoryStore交接、晚到cut判退、
  关闭时的并发producer排空，以及实际 `new Event[]` 的bad_alloc注入和新session正向恢复。
  Frozen但lost非零仅为失败取证数据，不能宣称完整包。
- 独立复验最终 `AutoTest/artifacts/recorder_offload_e1_parent_20260916/all-r2/receipt.json`：
  BelowNormal串行编译、`-Wall -Wextra -Werror`，**8/8 CPU程序**通过；PE32/i386与PE32+/AMD64均核实。
  每位数wire为1,403断言、history为1,767、交接为395；多线程断言数随实际接受人口变化，不冒充独立场景数。
  两位数golden与独立Python的472字节样本逐字相同，SHA-256
  `2401882CFF8CB5E50D23911CE278168375F5E33CCCFE76F36D9A808C2FFA11EE`。
  **97/97定向Python检查**（73既有+24新golden）通过，2文件py_compile与diff check通过；不是全量回归。
- 实际入口sizeof为3,670,400 bytes（约3.5MiB），容量8192，producer内不等待consumer且普通/对齐C++
  new注入守卫未触发；这不是任意malloc/OS调用的动态拦截证明。两位数测试均实际申请并写入
  262144条历史，storageBytes=102,760,448，无分配失败skip；该测试在同进程直接调用core，
  **不证明游戏已节省内存，也不证明真实32→64迁移、帧耗时或闪退修复**。
- 失败记录均保留：ingress-r1为测试替换new/delete触发GCC内联警告，采用noinline而未关闭警告；
  wire-r1/r2为负例同时违反多个条件，已改为单缺陷夹具；wire-r3为主线程编译器路径手误、未执行编译；
  wire-r4为Clang对跨位数范围比较的Werror，改为等价除法上界后wire-r5与最终all门通过。
  子线程初始自报PASS没有被直接采纳；Kimi交付的Python文本替换脚本不符合apply_patch流程约束，
  该偏差已记录，后续写阶段须重申并核验；原284路径直到文档checkpoint前逐SHA无变化。
- 本批新增14路径，最终298 dirty；旧路径只允许本日志、AGENTS短状态与实验室README的说明变化，
  其他原始内容保持。无Ninja/dry-run、产品DLL构建、部署、游戏/GPU；玩家现场和稳定CHANGELOG不动。
  两个E1任务完成停笔。20分钟监督已更新为本批Gemini/Kimi及E1→E2范围，不再回到旧导出/UI任务。
  下一步先冻结E2真实跨进程、内存取证及trigger线性化合同，再分工；旧阻塞lab析构不得接进产品。
  E2完毕先总结并暂停，E3产品owner、GPU输入池及隔离同配置内存/帧耗时门仍未过。

## 2026-09-16 — 取证显示归入 ImGui 控制台（源码候选）

- 按用户要求撤去不可关闭的独立 recorder 浮窗；状态、采集时长、保存进度、错误、外部连接及快捷键
  提示归入 Ctrl+F1 控制台的“帧取证”折叠栏，主窗口增加关闭按钮。隐藏时不为 recorder 单独开启
  ImGui 帧；启用取证时仍延后到 Present 的取证图像捕获之后绘制，避免控制台写入历史图片。
- 本批仅迁移显示，不改变 producer、采集默认值、内存预算、热键、导出/取消/资源退役。面板只读已有
  HUD 快照，明确说明隐藏不停止采集/释放内存；完全禁用仍须启动前设 `DXVK_WAR3_FRAME_EVIDENCE=0`
  并重启。保留 image-complete 与 packageReady 的区别，不能凭图片保存完成就宣称完整证据包已就绪。
- 首轮纯 Python 回归 11 脚本中 2 失败：旧 shortcut 测试要求常驻浮窗，self-contained 测试依赖旧空白格式。
  仅对应显示断言改为控制台合同/面板作用域检查；其余断言保留，复验 **11/11 脚本、146/146 项**通过。
  首次失败回执原样保留。生产 `war3_imgui.cpp` 按既有 build32 参数移除输出选项后，串行 BelowNormal
  `-fsyntax-only` 通过；仍有既有 `jass/war3_game_struct.h` OPCode 成员初始化顺序警告，未在本批修改。
- GLM Flash 负责独立显示静态测试，主线程负责生产 UI、受影响旧测试与复核；Kimi未启动新任务，定时监督
  仍暂停。主线程补强 private 访问域、packageReady 真假消息绑定及只读约束后，新 static **13/13**通过；
  三份受影响 Python 文件 py_compile 通过，合计 **159项定向 Python 检查**，不是全量回归。
  新 static 为15,390 bytes / `3C8ED816A09B563151821ACEDEA09D264BF8D1D204DB758CD9583E6B3302D8FF`。
  与开工281路径逐SHA比较：仅UI cpp、两份受影响旧测试、内部README及本日志5路径变化；另有原本clean的
  UI header成为modified，加新static和DSH报告，共284 dirty，无删除。diff check与零编译/游戏进程核验通过；
  证据目录为 `AutoTest/artifacts/recorder_console_parent_20260916/`，GLM报告保留交付时身份，不追溯改写。
- 再核昨晚 PID13216：非 LAA 的 32 位进程已占约 1.90 GiB 虚拟空间，驱动空指针位置伴随 OOM 状态，
  recorder 的 CPU 环/输入池预算约 320.5 MiB，故取证带来的地址空间压力是首要嫌疑；LastError/LastStatus
  不是失败分配的完整异常记录，尚缺同 DLL off/on 对照，不能断言唯一根因。GPU 图片约 3.57 GiB 不能直接
  等同于同量 x86 VA 占用。内存准入防护仍只在源码，不能宣称旧玩家 DLL 已修复。
- 没有 Ninja/dry-run、DLL 构建、部署、游戏/GPU或玩家视觉验证；未覆盖玩家现场，稳定 CHANGELOG 不晋升。

## 2026-09-16 12:36 — 双DSH本批纯离线收尾，监督暂停

- 用户询问两边idle原因；实时查询确认均completed/idle，而非仍在推理或被供应商阻塞。
  GLM已删除最后的冗余全文件计数，保留函数体接线与其他断言。主线程读实际文件并独立重跑 **13/13**、
  修订文件py_compile、diff check均通过；新static身份 **13,720 bytes / 2642E082…C346A**。
- 与parent-r2逐文件身份比较，仅最终static、GLM报告、主线程计划/总日志4路径变化；Kimi生产代码/头及
  native测试全部仍与已通过的CPU/语法回执相同，未重复编译或扩测。dirty=281，HEAD不变，零相关编译/游戏进程。
  合并依据为parent-r2的21行classifier、242次共用提交检查、既有budget/order门、35+185项Python与生产TU语法，
  加本次最终static复验；不把新static的通过追溯改写成旧回执内容。
- 本批按已签发范围完成并接受为**纯离线候选**：status与nextExport共用采样、终态提交和HUD/错误副作用，
  保持packageReady与图像Complete分离。没有重建DLL、部署、GPU/玩家验证，不声称闪退或裂缝已修复。
- 按完成条件，经产品automation工具暂停`warvk-2`，不再为空闲任务定时消耗额度；原计划/间隔/供应商配置保留。
  Kimi=`cli-proxy-api/kimi-k3`，GLM=`scnet/GLM-5.3-Flash`，没有新派发。
- 下一批建议：优先围绕诊断的32位VA/commit预算、部分初始化失败以及取消/退出的资源责任做单链审核与验证，
  收紧仍可证明的缺口后再安排独立产品构建/隔离高压门。64位宿主继续作为未接游戏/GPU的实验室方向，
  不把CPU协议当作现有闪退的替代修复。此处仅提案，不授权新增文件/实机或自动接入宿主。
  收尾回执：`AutoTest/artifacts/dsh_dual_supervision_20260916/checkpoint-final.json`。

## 2026-09-16 12:30 — 双DSH二次监督：生产共用CPU门通过，Kimi供应商按用户要求切换

- 两方完成并停笔后，主线程对281路径逐SHA冻结复验，运行前后全部不变；与原275份备份比较，仍仅
  授权生产文件、旧报告与本日志变化，无删除/额外路径。原有GPU资源/registry/释放/预算路径未在本批diff中改变。
- Kimi已把实际state/HUD/fault/recording/shortcut提交收进生产共用入口，CPU测试只绑定存储，不再复制提交分支。
  主线程串行BelowNormal Win32编译运行：classifier **21行**、实际共用提交 **242检查**、既有history core
  **14,539,926次预算/环顺序断言**全部通过；这些断言次数不是同量独立场景。三份native测试均PE32/i386。
- 主线程纯Python：新static **13/13**、统一入口自测 **35/35**、既有15脚本 **185项**通过；3-file py_compile、
  生产`war3_frame_history.cpp`按既有compile_commands去除输出/依赖选项后的`-fsyntax-only`及diff check通过。
  新native与生产TU无编译诊断。回执`AutoTest/artifacts/dsh_dual_supervision_20260916/parent-r2/receipt.json`。
  这是实际共享CPU边界+生产接线/语法证据，不是运行整个FrameHistory GPU实现，更不是DLL/实机/闪退验收。
- GLM修订虽已加函数体检查，却仍留一项masked全文件计数；主线程要求仅删除该冗余断言、保留其余13项并
  新建run4证据。此前回执留存；Kimi无新任务。整批尚待这项最终机械收口，不因测试全绿忽略合同偏差。
- 用户要求降低Kimi供应商额度消耗：原会话已由`scnet/Kimi-K3`切换至`cli-proxy-api/kimi-k3`，MCP selected及
  session list双重确认；GLM保持`scnet/GLM-5.3-Flash`。没有新建会话或重派旧实施，也不推断旧调用的实际计费。
  监督计划和原20分钟heartbeat同步更新供应商，历史dispatch不改写；只向GLM派发上述最后机械修订。
- 未运行Ninja/dry-run/产品DLL构建、部署、游戏/GPU或调试器操作；现有Git备份与玩家现场未改。

## 2026-09-16 12:13 — 双DSH首次监督：边界通过，交接测试退回修订

- 原275份备份文件（274 dirty加imgui shim）逐SHA复核：仅本日志、GLM获准更正的旧报告及Kimi获准修改的
  `war3_frame_history.cpp`变化，无缺失；新增7路径均属本批（两方6份加主线程计划），dirty=281。
  branch/HEAD保持原值，`git diff --check`通过；只读进程检查未发现War3/YDWE/WorldEditor或编译进程。
- GLM首轮已返回，Kimi仍在收尾。主线程没有沿用其自报10/10作为整批验收，也没有启动编译或运行测试。
  实际源码审查发现Kimi的Harness重复实现状态/HUD/错误提交，只共享采样与决策函数；这不能证明生产提交
  副作用正确。已要求将最小实际CPU提交边界也由生产和测试共用，保持原4路径及GPU/锁/寿命边界不变。
- GLM静态中的一项实际为全文件名字计数，与“函数体限定”的合同/报告不符；packageReady检查也未覆盖
  真正提交入口。已要求提取并验证函数体、屏蔽注释伪调用、补反例，并纠正报告中的时间点/解析方式混淆。
  两个纠正消息均accepted，不代表修订已完成；不扩旧测试阈值、不覆盖旧回执、不新开实现者。
- 本checkpoint仅只读审查与范围/SHA核验。尚未CPU编译、生产TU语法检查、产品构建或实机，不能声称
  闪退/阴影问题已修复。监督维持每20分钟；本轮结束，让原任务在原白名单内修订。
  回执：`AutoTest/artifacts/dsh_dual_supervision_20260916/checkpoint-review1.json`。

## 2026-09-16 — 用户授权双DSH线程与20分钟监督：已备份、已派发

- 用户要求GLM-5.3-Flash承担机械工作、Kimi-K3承担核心架构实施，主线程定时审查，派发轮不等待完成。
  新范围见`docs/plan/2026-09-16-dsh-dual-agent-supervision.md`；两方写路径分离，当前仅录制导出终态一条链。
- 已建立本地Git安全快照`codex/backup-before-dsh-dual-20260916` / `30c9163ea85081b367b114bb5e872362fb5dd065`；
  通过alternate index生成，不改变当前分支/HEAD/真实index。274 dirty与imgui未跟踪shim共275份raw文件逐SHA备份。
  根和4个子模块bundle均verify，外部目录`D:/WarVK-Backups/20260916-dsh-dual/`约190MiB；未推送、未删数据。
  此为用户要求的WIP备份，不是候选通过后提交。ignored DLL/build/证据留在原处，不冒充完整磁盘备份。
- 上批DSH已返回且代码身份与主线程35/35+185项复验一致；其报告“15脚本无stderr”表述错误已列为GLM修订项。
  新批次不能沿用旧测试结果宣称通过；本轮不编译/部署/实机，不修改生产代码，实施由派发任务负责。
- DSH两项send均accepted：Kimi会话`session-19569f28-5d7b-4da2-939c-25203e44f292`，
  GLM会话`session-a9b8cb49-7270-4234-898a-93ff8849114b`；原白名单测试入口不再扩张。
  `warvk-2`已更新为本线程每20分钟ACTIVE监督，替换旧夜间prompt/截止；本轮按用户要求不等待子任务。
  调度回执及精确turnRef见`AutoTest/artifacts/dsh_dual_supervision_20260916/dispatch.json`。
  accepted仅是受理；后续由主线程审核/指正/验收，完成后汇报下一目标并暂停，未授予产品构建或实机。

## 2026-09-16 — 用户新授权：主线程设计 / DSH机械任务第一批（已派发，待验收）

- 已在旧dxvk工作区创建DSH会话`session-a9b8cb49-7270-4234-898a-93ff8849114b`，明确配置`scnet / GLM-5.3`；
  供应商未提供独立思考程度选项，未自行替换模型。旧dxvk树仍只读，实际3个新文件限定在v1.22集成树。
- 主线程先写`2026-09-16-architecture-delegation-plan.md`：M1显式15脚本纯Python统一入口，M2入口正反例与测试目录。
  DSH只能新增入口、入口测试与独立报告；不改任何既有测试/阈值、C++、shader或本日志，不编译/部署/运行游戏。
  send_message返回accepted仅表示受理，尚未完成或通过主线程验收。
- 主线程独立完成`2026-09-16-recorder-export-terminal-contract.md`设计：现有status/nextExport保留采样入口，
  终态决策及HUD/fault更新必须收拢到唯一提交方法；图像完成不等于整个证据包完成，GPU寿命与join合同不变。
  这是未实施设计，不声称修复或CPU验证通过；下一批机械接线必须等实际接口由主线程冻结。
- 开工只读核验269旧路径SHA全部不变、branch/HEAD匹配；现场只观察到原x32dbg PID32228，War3 PID13216已不在列表。
  未关闭、重启或操作调试器，不推断原debuggee为何退出。夜间自动化继续暂停，不借本授权恢复无人值守。
- 主线程独立基线：串行运行计划内15份既有Python脚本，合计185项全部通过（146 recorder + 39 render-host）。
  此为定向static/synthetic及Windows命令行解析，不运行native gate/编译/游戏；子线程的新入口尚待返回审核。
- 初版入口只读审查/实际main反例：mock测试成功而输出open失败，实际exit=1但stdout JSON仍ok=true，
  违反输出失败不能冒充通过的合同。已给DSH精确修正及反例要求；此红例只验证真实main输出路径，
  测试执行结果为mock，不冒充15份真实脚本运行。初版未接受，等待修正与最终复验。
- 第二项真实函数反例：`clip_stream(b'\xff' * 70000)`先截原字节再替换解码，保存文本的UTF-8长度膨胀为196608，
  超过65536字节合同。已要求DSH补无效UTF-8/跨字符截断反例并限制最终文本编码长度；仍不把摘要长度上限
  当作subprocess全程内存上限。该项待修正，不改变生产取证或任何GPU内存预算。
- 用户后续明确允许较慢时使用GLM-5.3-Flash；读取会话时已显示该配置，随后重新discover并显式选择确认
  `scnet / GLM-5.3-Flash`。沿用原会话/原任务，不重复派发、不改变设计及验收要求，也不推断此前每一步由哪款模型生成。
- 主线程中间复验：初次输出错误已修正，入口自测31/31通过；真实入口`--group all`调度15/15脚本、合计185项通过，
  每个脚本前后身份一致。回执`AutoTest/artifacts/dsh_mechanical_20260916_parent_review/run-all-r1.json`。
  该版本尚有上述UTF-8保存长度红例且报告未完成，不能把这些通过项合称整批验收通过。
- 第二次独立复验：UTF-8红例修正，17组真实clip函数输入检查通过；入口自测35/35、实际入口15/15脚本
  （185项既有测试）、2-file py_compile通过，入口与自身测试在运行前后SHA不变。回执`run-all-r2.json`。
  从旧树启动的实际CLI `--list`为15项/零执行/verified=false；已有output拒绝exit1且原回执SHA不变。
  等待DSH最终报告与写入边界闭合；这些仍仅证明纯Python工具，不证明生产状态机或游戏修复。
- 11:13台北时间阶段边界：原269路径除本日志外逐SHA不变，当前273（两份主线程新设计+两份DSH新Python），
  报告尚未写入，整批仍未最终接受。diff check通过，build32/live仍2D4B…7FBC5；只读进程快照无相关进程，
  未操作游戏/调试器，不推断此前进程退出原因。主线程复验checkpoint已保存；DSH仍在原会话收尾，不重复派发。

## 2026-09-16 — 夜间截止收尾（08:46后，仅核验/文档）

- `warvk-2`已暂停；截止后没有新增实现、构建、native/Python测试、部署或游戏。最后实现门仍为05:51。
- 6份关键receipt SHA与106项source pin复核一致；收尾前268路径逐SHA等于05:51。
  本次只改工作计划/本日志并新增整晚成果报告，最终269；198初始路径不变、11旧改动、60累计新增、无删除。
- 玩家/build32仍35,865,159 / 2D4B…7FBC5；War3 13216/x32dbg 32228现场保留，零本夜helper/编译残留。
  不把产品源码保护/CPU通信验收当成新DLL、内存收益、GPU或闪退修复。
- 只读审计留下`status()`/`nextExport()`终态提交不一致：查询可能先改state而跳过HUD/fault更新；
  未实现修正、未做复现测试，明确列为下一次定向边界，不在超时收尾时仓促动源码。
- 整晚成果、架构、实际门与下一步见`2026-09-16-overnight-architecture-results.md`；
  最终路径/身份/回执复核见`AutoTest/artifacts/overnight_architecture_20260916/checkpoint-final.json`。

## 2026-09-16 05:47 — P3b2失败输入回执闭合

- `worker-r13` **14/14实际用例**、60 IPC lifetimes、302 shared writes/287 ACK/6,318,702bytes；
  15份已写未ACK输入也有完整SHA，Python独立重建五种恶意envelope，未降低host拒绝门。
  receipt SHA256 `B1B61B34E08403F6B34351419964956FC61726D9D49601655C9B991D6FEE4569`。
- 定向render-host Python **39/39**、4-file py_compile通过；共用P2 host仍以shared-r9 41/41为依据。
  源码与实际回执边界、ETW初始化调查、失败R1–R11保留说明见`2026-09-16-render-host-worker-lifetime-audit.md`。
- 无产品DLL构建/部署、游戏或GPU。worker仍AutoTest实验适配，不把跨位数CPU基础称作64位渲染或闪退修复。
- 05:51边界复核：dirty268（相对初始209，198原文件逐SHA不变、原有11个本夜授权修改、59新路径），无删除；
  6份最新相关native receipt共106项source pin仍匹配当前源码。build/live仍2D4B，暂停War3/x32dbg原PID保留，
  无实验helper或编译残留；`git diff --check`通过。完整路径与身份见`checkpoint-p3b2-0551.json`。

## 2026-09-16 05:44 — P3b2实际异步样本链首个完整门

- `worker-r12/receipt.json` **14/14实际用例通过**：正常/真实慢消费/断连/新连接恢复、16轮正常/断连交替、
  两种capability错配各16轮、scope/transfer/attempt/frame/length恶意输入。均为独立CPU实验，未接产品/GPU。
- 慢消费期间1000次producer尝试有4入队、996立即Full，host恢复后新合法样本仍发布；sourceFrame/attempt/transfer
  独立保留，ACK与host私有copy SHA由Python重算。队列/ACK/在途失败/未出队人口闭合。
- R1编译warning、R2/R3/R4/R7/R10句柄计数失败均保留。R11仅SELF进程NT快照明确新增3个EtwRegistration；
  R12将“精确host空参数启动”与协议运行分开取证：能力错配前image-init为103→106→106，协议完成仍106，
  所有16轮门每轮句柄相等。没有靠等待/允许浮动放宽门；ETW具体provider未识别，不能外推为玩家崩溃原因。
- 夹具线程改为显式CRT thread handle等待实际signaled后关闭；旧std::thread不是已证实根因。当前将继续补齐
  failed-in-flight原字节SHA回执后重新冻结；本条不声称游戏渲染迁移或RAM/FPS收益。

## 2026-09-16 05:18 — P3b2共用host提取的既有回归门

- 提取唯一`slot_host_runtime`，P2入口变为薄wrapper；同步payload检查接口只借用当前ReadPermit/private copy，
  位于SHA验证后、finishRead/ACK前。P2无consumer时仍严格capability2；新增6 profile不能静默降级。
- `shared-r9/receipt.json`实际32→64位**41/41通过**（含48轮寿命），双位wire/golden继续通过；
  receipt SHA256 `027A09D81FD13416F1B12A114CEE726FDD544A53367250C85582175F1C3B1E59`。
  共用host语义回归已过，不代表尚未构建的P3样本worker已通过。
- 新P3 consumer/慢消费QPC合同正实施；未修改底层transport/owned-child，未运行游戏/GPU或产品DLL构建。

## 2026-09-16 05:04 — 无人值守阶段八：原始帧与传输身份分离（P3b1）

- 先冻结`render-host-sample-envelope-contract`架构/80byte字段表/失败与容量门，再实现WVP1 codec和payload顺序validator。
  原sourceFrame、带Full/Invalid缺口的attempt、RH1连续transfer分别保留；固定scope与精确lease key逐字段匹配。
  任一解析/顺序失败清空view且连接级validator永久Fault，不修字段继续，也不替代SlotLedger/ReadPermit或GPU完成。
- payload上限明确65456，ProducerInbox构造改为命名Limits；超出值在producer copy前拒绝，不挤进65536槽再截断。
  没改RH0/RH1既有wire/host/lease行为；P3b2还须显式capability6，旧P2继续2，两种错配都要拒绝，避免旧host仅hash就误ACK。
- `envelope-r1/receipt.json`实际Win32/Win64各**56967检查、10000合法样本**，81byte golden与Python独立struct逐字一致；
  含重复源帧、丢样缺口、最大payload、尾随/短包、字段/shape/顺序错、清view、永久Fault、新scope恢复、4000变异roundtrip。
  两种编译均无warning。这是实际codec/CPU证据，手工Key不冒充真正共享槽租约或跨进程消费。
- 新Limits相应重跑P3a `inbox-r6/receipt.json`，双位131973/118645检查；queue仍262464bytes、暂停消费固定4接收/1996Full。
  50000尝试分别7387/4055接收、Full42613/45945，实际消费96670366/75560249bytes。旧R5不作为新constructor证明。
- 实际慢host/断管/worker尚待P3b2；本阶段不创建游戏或新共享进程，不重建/部署D3D9 DLL。
  Python31组、相关py_compile/diff核验；status259（原209中198不变、11旧改动、50累计新增），无删除。
  2D4B玩家/build32与暂停War3/x32dbg继续保留，稳定CHANGELOG不变。

## 2026-09-16 04:46 — 无人值守阶段七：producer与阻塞I/O隔离的有界队列（P3a）

- 04:49复审补充：新queue恢复测试明确改成不同nonce/map/device，并逐样本校验，不再把同scope的新对象称为新连接。
  最新`inbox-r5/receipt.json`双位132321/117909检查；分别7504/3901份成功、Full42496/46099、96620596/84779890bytes。
  新6组判定器通过，R4旧身份与结果保留。核心实现未改、尚未进行IPC恢复；下列R4是前一checkpoint实测。
- 先写`render-host-producer-inbox-contract`架构图/访问与关闭协定，再在独立lab新增实际SPSC核心，未接产品或RH1。
  私有4×64KiB、32位原子head/tail、release/acquire交接；producer只做一次有界copy和固定原子操作。
  Full立即拒绝并计数，不覆盖旧样本；有效源frame、带缺口的attempt、固定nonce/map/device一起交给私有copy。
  stop请求不是join/析构许可；producer close后consumer重读head再判排空，统计只在两线程join后取得。
- `inbox-r4/receipt.json`双位native **131825/116117检查通过**；两端真实queue262464bytes、Sample65584bytes，编译无warning。
  consumer被阻止取样时producer先完成2000次调用并join，精确4接收/1996Full，随后恢复消费内容正确。
  50000并发尝试分别7380/3453份接收，Full42620/46547；实际读取97328781/82195257bytes逐份内容/序号校验。
  这些调度相关数值不比较性能；不能推论游戏帧时间。另验证实际consumer请求stop、既有样本排空、计数耗尽、故障后新queue恢复。
- C++分配guard覆盖普通/数组/对齐new，非任意C分配拦截；无Tsan/GPU/真实慢host证据。P3a测试不能替代后续P3b。
  R3保留脚本调用的x64 compiler路径笔误失败；R4对应当前源码，两位数均过，不修改旧receipt。
- 发现必须提前锁死的协议差异：RH1.frame要求连续传输序号，而sourceFrame可重复、attempt可丢失。
  后续P3b必须独立payload envelope保留全部身份并扣除header后的真实容量，禁止默默重编号/截断或放宽旧ledger。
- 新6组判定器+原25组=**31组Python通过**，2-file py_compile/diff通过。standalone Meson登记CPU测试，根Meson未改。
  status254（原209中198 SHA不变、11旧修改、45累计新增），无删除；2D4B玩家/build32及暂停调试进程继续保留。
  尚未迁移任何游戏内存/渲染，不宣称闪退修复；更新日志只写候选开发账本，不改稳定CHANGELOG。

## 2026-09-16 04:33 — 无人值守阶段六：生产取证控制权由bool收敛到具体租约

- 按实施前`recorder-control-lease-contract`调用图/锁序，将FrameHistory→CPU Control的裸`true`权限与无身份Release，
  收敛到同一controlMutex保护的不可复制/移动、一次签发lease。新header不新增Manager、不引入热路径锁。
  旧/空/其他authority的凭据不能释放或控制当前owner；代际耗尽拒绝、不回绕。控制owner不是ring session或GPU fence。
- 保持退出先join再Release，删除重复claimed bool；image控制先释放authority锁再拿registry锁，
  还必须匹配当前image owner内的确切lease对象，stopping拒绝新image工作。最终CPU freeze保留收口权限。
  无GPU寿命、捕获输入、schema/manifest/导出格式修改；当前调用链未证明误释放，此为接口硬化而非闪退根因宣称。
- `recorder-lease-r2/receipt.json` **15/15 Win32 CPU入口通过**：核心9766、实际生产Control723、disabled3、
  memory45/内存拒绝与分配失败86、两种默认profile各5模式。生产mutex四线程2000次争抢，255成功、0重叠/错误，
  随后合法新owner恢复；成功数非性能目标。原导出192事件/数据/序号和取消/CreateNew测试没有删除或放宽。
- 四生产TU(history/evidence/control-plane/inputs)原flags的纯语法检查通过，无产品对象/DLL生成；
  CPU编译仍有原有focus unused-parameter warning，保留不改无关函数。明确CPU/语法不能替代swapchain/GPU退出测试。
- 定向Python **111项**通过（frame72/self-contained9/render-host25/新gate5），3-file py_compile和diff通过；非全量回归。
  当前249 dirty：原209中198内容SHA不变、11旧路径修改、40累计新增、无删除。旧RH0/RH1实现未改。
  玩家/build32仍35,865,159 / `2D4BCB400CBCA7524403E5C0939C679450865A4A48B47739941F9D697077FBC5`，
  暂停War3 PID13216/x32dbg PID32228保留，零新构建/helper残留；源候选尚未形成新DLL或游戏修复验收。

## 2026-09-16 04:07 — 无人值守阶段五：真实共享容器循环与迟到租约（P2c）

- 协议保持不变，先补P2c测试合同，再补同一Win32 producer的48轮真实mapping/4mutex/Win64 host寿命。
  四类各12轮：正常跨2epoch并Close、Cancel隔离后有效消费并Retire、slot实际复用后旧key、错误digest。
  24个故障终态没有省略后续成功轮；每轮都精确等待host、确认对象名不存在、句柄回同一初始化基线。
- 最新`shared-r8/receipt.json` **41/41**完整门通过；同producer的48轮约1.531秒，不冒充长期内存/性能稳定门。
  soak共180个成功样本、564条成功控制消息、204次write/4829208bytes，192次copy中12次digest错误不得计成功。
  Python逐轮核对实际host日志、连接nonce/进程创建身份、key及独立bytes/SHA；不是只看总数或静态字符串。
- 补Hello前操作/重复Hello、Active再Begin、无Begin的Reserve/End、Writing时End、Active直接Close、
  满槽/零byte Reserve、Cancelled Publish、shape错误与slot复用后的旧完整key；均按精确原因/人口拒绝。
- 父退出新例在3次有效控制回执和真实65536byte写入之后ExitProcess，仍处Writing租约；Job清场与映射消失闭合。
  不外推为GPU安全退出或任意指令点的故障恢复。旧Hello后退出例继续保留。
- 加强partial-write注入真实性：R8只在真实映射中复制前32768bytes，然后解锁、发布完整65536bytes预期摘要，被拒绝。
  R5/R6/R7旧例是全长buffer带零尾的缺失内容模拟；保留旧证据原义，不重标为真实半程写者退出。
- RH1 wire双位各23056检查仍过；新6组判定器加原9组，与P1的10组共**25组Python测试通过**。
  本阶段只改实验室测试和文档，没有改P1/共享槽实现；P1 R16源SHA仍匹配，不重复宣称旧证据验证新代码。
  py_compile/diff检查通过，status仍245（202原路径SHA不变、7旧修改、36新增、无删除）。
- 已只读开始下一条生产交接审计：FrameHistory worker→CPU Control所有权目前以bool标记/无身份Release接口连接。
  当前调用链的Release在swapchain析构、先join后放owner，尚未证明真实并发误释放；不能据此宣称又找到闪退根因。
  下一步考虑用实际不可复制控制租约代替bool权限，减少新旧owner可混用入口；实施前先冻结精确锁序/寿命与正反例。

## 2026-09-16 03:51 — 无人值守阶段四：真实共享槽与退出凭据（P2b CPU实验室）

- 先制定`docs/plan/2026-09-16-render-host-shared-slots-wire-contract.md`的进程/容器所有权图和RH1线格式，
  再实现独立WVS1协议、共享槽、宿主和Win32测试端。RH0保持独立，不把Echo扩成隐藏资源API；根Meson不接入。
- 32位事务只创建固定256KiB section+4个mutex，64位host只读映射；lease仍只由单一SlotLedger签发，
  共享view从不逃到消费者。每份样本持锁复制到64位私有64KiB缓冲，SHA相等、CPU消费完成后才ACK。
  一次性writer generation在等待/复制前即消耗；失败或取消不暗中重试、不重新使用有疑问的槽。
- 实际完整门`shared-r5/receipt.json` **27/27**：正常/碎片各36样本、3 epoch、4槽、1/97/4096/65536byte，
  每个SHA由Python独立生成bytes再核对；实际只读view=2、pool=262144，碎片实际2674次write。
  Cancel隔离后其余3槽18份仍有效，重复write拒绝后原byte不变；全key字段错误及重复Publish拒绝。
- 实际半份写入/错误摘要拒绝；writer线程在持锁时退出，host真的观察WAIT_ABANDONED且copies=0，
  不依赖人工设置状态。实际锁超时约5.090秒、copies=0；所有有host终态的Fault均读者收口。
  名称已存在拒绝且原对象仍可用；每次精确child退出后由native与Python独立复核section/mutex均不存在。
  父ExitProcess由原子Job结算host，但目前该case发生于Hello后，不外推为全部在途写者场景。
- 收紧共同OwnedChild析构/构造失败路径：5秒到期不能替代退出信号，必须drain实际process handle后才返回。
  异常内核可能继续阻塞，外层23秒看门狗一旦触发就判失败；这是lab隔离条件，不准塞进游戏/渲染线程。
  共同代码变化后RH0最新`transport-r16/receipt.json` **16/16**重跑通过，包含64个连续helper生命周期；R15留作中间证据。
- RH1实际codec Win32/Win64各**23056检查**，72byte golden与Python完全一致；新增9组Python判定器负例，
  连同P1判定器共**19组通过**，3-file py_compile、diff检查通过。独立Meson入口已列新target；本轮实际构建由显式编译器gate完成。
- R1编译include错误、R2首次异常句柄检查失败和R3/R4中间结果均保留。首次C++异常引起2个句柄初始化；
  单独测量119→121→121→121（首个/8次/再64次），之后每个事务必须回到精确句柄基线，未放宽门。
- 当前245 dirty：原209中202项逐SHA不变，7项既有夜间修改，累计36个新路径，无删除。
  仍是小样本CPU框架：没有64位长历史、真实渲染/显存迁移、FPS/游戏内存收益或崩溃修复接受。
  玩家/build32 DLL仍2D4B/35865159；暂停War3 PID13216与x32dbg PID32228保留，无编译或helper残留。
- 下一步先做同一Win32 producer的多轮真实mapping事务soak、在途lease退出和更多非法状态顺序门，
  不把P1管道soak冒充P2b mapping寿命验证。然后再选择一条已有生产CPU交接债务；不急于接游戏。

## 2026-09-16 03:14 — 无人值守阶段三：有限槽与一次性消费凭据（P2a CPU核心）

- 先建立`docs/plan/2026-09-16-render-host-slot-ownership-contract.md`，包含owner/数据流图、槽状态图、
  取消/退役/CPU读者与实际writer/GPU完成的区别。RH0消息与resource=0保持冻结，未借echo暗中开放资源命令。
- 新`slot_ledger.*`为单一串行CPU owner的固定4槽×64KiB元数据账本，没有共享C++布局、动态增长或GPU操作。
  当前只管理租约，不存实际样本bytes、不创建映射；大历史移至64位宿主仍是后续目标，不能报游戏内存收益。
- 精确64-byte WVL1描述符逐字节编码，connection/map/device/frame/generation/slot/bytes全字段匹配才准入。
  解码失败清空无效候选，不留上次解析结果；解析成功也不等于租约有效。槽offset未来由已验证slot计算，不能收裸指针。
- `ReadPermit`只由Published签发，不可复制、可移动且不能覆盖；对象一生只能签发一次。
  已消费或移出的permit禁止重新绑定，避免陈旧引用指向新租约；观察接口返回key副本，不暴露权威内部引用。
  外来owner、moved-from、重复完成均不能释放新数据。丢弃permit不自动宣布CPU完成，必须走连接退役/owner结算。
- cancelWrite只隔离Writing槽，不立即复用：取消消息无法证明旧写者已经停止。其余Free槽仍持续成功，
  全部隔离时明确Capacity，既有frame/generation不被失败请求消耗。retire禁止重开/新提交，在途本地读者可结束但不回Free。
  `localReadersDone`不是共享映射释放、对端退出或GPU最后使用凭据。
- `slot-r4/receipt.json`为当前完整证据：真实核心Win32/Win64各**116269检查**、各10000条合法元数据记录，
  正向满槽/释放恢复、250地图epoch、全部key字段突变、迟到key/permit、取消/退役、有限计数耗尽和大整数拒绝通过。
  两端64字节golden与独立Python编码完全相同；较小generation/frame上限只为同核心确定性耗尽测试，不改默认UINT64_MAX。
  R1–R3中间证据保留，R4才匹配最终接口；测试数量不是全部生命周期/物理内存正确性的替代。
- 编译父进程BelowNormal、两种编译器串行，只生成独立CPU测试exe；相关py_compile与diff检查通过。
  当前232 dirty：原209中仍202逐SHA不变、7为阶段一已有修改；累计新增23，无删除。未重建/部署DLL或接管游戏。
- 下一步P2b先冻结独立wire版本与映射准入：明确logon SID、Local命名、CreateNew语义、固定视图与每槽Windows mutex。
  微软文档确认跨进程共享内存需要同步，WAIT_ABANDONED内容不确定；不得继续消费或靠std::atomic对象布局跨ABI。
  之后才做实际32↔64字节、旧key/部分写/abandoned/超时与退出门；不接原游戏、不移动大历史、不创建GPU后端。

## 2026-09-16 02:55 — 无人值守阶段二：真实Win32↔Win64 CPU传输及退出（P1）

- 先补RH0合同的取消结算/进程监督边界，再实现独立`win32_transport`、`owned_child`、64位host和32位测试client。
  只在CPU实验室运行；没有产品DLL链接、Game读取、GPU设备、共享内存或窗口接管。
- 命名管道使用随机nonce名、FIRST_PIPE_INSTANCE、单实例、拒绝remote与明确logon-SID DACL；
  ACL和客户端访问掩码不包含create-instance权。Windows实际对端PID双向核对，父子保持进程句柄/创建时间。
  测试实际另起错误PID客户端抢连，Hello之前即拒绝；已有同名pipe也拒绝。
- 受控子进程通过`PROC_THREAD_ATTRIBUTE_JOB_LIST`在CreateProcess时原子加入kill-on-close Job，
  不存在先启动再Assign的无人负责窗口。只继承CreateNew日志句柄，不继承Job/管道/进程句柄。
  构造后验证失败也先关闭Job并等待自有子进程，避免依赖不会执行的类析构器；不得按进程名杀程序。
- Header/body共享5秒包截止；短片段不刷新期限。实际overlapped请求直到完成/取消完成才释放buffer/event。
  `CancelIoEx`后用GetOverlappedResult(TRUE)结算；异常内核可能继续等待，故仅可放在独立测试进程，
  外层23秒看门狗触发必须判失败，不能部署到游戏/渲染线程。Write完成不等于对端应用处理，更非GPU完成。
- 最新完整证据`AutoTest/artifacts/overnight_architecture_20260916/transport-r14/receipt.json`：
  **16/16实际CPU IPC用例**通过，包括正常/碎片、错nonce/序号/epoch/capability、超长header、断半包、
  header/body/connect超时、实际Write背压超时、错peer、first-instance、父进程ExitProcess与64轮生命周期。
  四个超时实测整例约5.09–5.11秒，取消请求=最终取消完成=1；不是依靠外层看门狗过门。
  正常会话11条消息逐byte回执；64轮为43合法/21无效nonce，所有原子Job子进程均结算，合法新会话持续成功。
- 测试早期句柄计数失败也保留：R2/R3/R4首用初始化，R9/R10/R11定位额外增长发生在第二次系统RNG调用、
  尚未创建本轮transport之前。独立32/64位RNG测试记录首次/初始化后值，再64调用无增长；
  固定初始化夹具为8次RNG、一个日志/身份可查的无IPC子进程和create/close安全pipe，不把它隐藏为重试。
  稳态64轮每轮对同一个post-init句柄基线核验相等；保留初始增加值，不声称Windows依赖零初始化成本。
- R1编译因-Werror缩进警告失败已修；R8为误写本地编译器路径、未构建/启动，全部失败receipt保留。
  R7的14例、R12的两补例、R13的16例均保留；**当前源码使用R14，不混用早期证据**。
- 10项Python证据门测试（含递归duplicate/非有限JSON、错误Write阶段、取消未完成、身份/计数突变）通过；
  3-file py_compile与git diff --check通过。独立Meson入口增加CPUhost/client/RNG目标，但本轮实测由显式compiler的Python门构建，
  不宣称另外执行了Meson/Ninja。交叉登录ACL负向门、内核取消卡死及产品异步owner仍未过，不夸大为安全产品。
- 夜间209→227路径：原209中202逐SHA不变，7项为阶段一已有授权改动；累计新增18，无删除。
  本阶段没有再改生产渲染C++；AGENTS短入口更新P1范围。玩家/build DLL仍35,865,159/2D4BCB40…77FBC5，
  War3 PID13216/x32dbg PID32228保留，编译/RH0测试进程为0。根CHANGELOG与稳定发布未动。
- 下一步仅P2：先单独制定有限共享槽/租约/迟到ACK/退出回收合同，再实现实际所有权核心与CPU正反例；
  不把RH0资源字段从0偷偷放宽，不用跨位数std::atomic布局或裸指针作为共享ABI，不把断管当GPU完成。

## 2026-09-16 02:12 — 无人值守阶段一：会话交接与RH0协议核心（CPU，未交付）

- 用户将当晚优先级转为长期生命周期/新旧交接与64位进程基础，授权至台北08:00。
  已更新当前任务heartbeat `warvk-2`，07:40起收尾；不把它当作自动产品接受或全库重写授权。
  初始209 dirty完整SHA基线存`overnight_architecture_20260916/baseline.json`。
- 实际交接缺口：TriggerMailbox的旧token消费者无条件exchange，错误清除新会话的热键。
  先补原生产核心测试，旧代码在`mailbox.consume(8)`失败；再用匹配token的CAS消费修正。
  陈旧/未来/零token、连续消费、新会话恢复与并发旧消费者都覆盖；1734项Win32 CPU检查通过。
- 图像owner和worker的Complete/Fault数字定义收敛到CaptureLifecycle单一来源，0..8 wire值不变；
  不改变GPU引用、完成信号、释放时机或渲染算法。对应history生产TU fsyntax-only通过，未生成产品对象/DLL。
- 先写RH0 ADR与组件/状态架构图，再实现独立C++17 codec/session，不链接产品。
  固定96-byte header/64KiB payload、精确版本/能力/位数、nonce、序号及map/device/frame门；
  错误只进入终态，不提交计数/epoch；正常跨epoch和新连接正向恢复保留。
  资源字段强制0，能力仅CPU echo；原型没有GPU/共享槽/游戏接管，CPU ACK不是GPU完成。
- P0真实核心分别编译PE32/i386和PE32+/AMD64，各41586检查通过；两端120字节golden与
  独立Python struct编码逐字相同。实际跨进程传输尚未实现，不能称渲染迁移/节省游戏内存。
  可复验入口`AutoTest/run_render_host_protocol_gate.py`，CreateNew receipt在`protocol-r1/receipt.json`。
- 本机未发现可用x64编译器，按用户许可获取官方llvm-mingw便携包：20260616 UCRT x86_64，
  187504083 bytes，B9B68A4D276E16FA25802AABA458E4638F64B3884C290AACCDC2D87083B6CA35，
  解压740825211 bytes；下载前固定发布digest、解压前核边界/大小。位于E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616，
  没有改PATH、系统安全设置或原MinGW。不是下载执行安装脚本；外部工具不进入发布包。
- 71项frame相关Python/static与9项self-contained测试通过，2-file py_compile通过，git diff --check通过。
  原播放器与x32dbg暂停现场保留；没有Ninja、部署或游戏/GPU运行，2D4B仍不含本轮源码。
  下一步P1首先审计overlapped取消后的buffer/OVERLAPPED寿命，再实现真实同用户32↔64通信和失败注入。

### 阶段一复审（02:17）

- Close接收与回执完成进一步分离为Closing/Closed；仅同一seq的`closeReplySent`可正常关闭，
  迟到/错序/发送失败不冒充正常退出。这个回调是传输owner本地接口，不是client wire命令或GPU完成。
  最新`protocol-r2`由两种编译器重新构建，各41668检查通过，golden仍逐字一致；R1保留，不冒充最新源码。
- 新旧相关Python定向合计140（frame71+self-contained9+analyze-frame60）通过；没有扩称全项目回归。
- 一次临时命令误设`core.autocrlf=false`，使原有CRLF被大面积报告空白错误；未改Git配置或格式化文件。
  随后恢复使用仓库原`core.autocrlf=true`执行diff检查exit0，不用批量重写“修正”已有源码。
- 边界209→218：原209中仅7项变化，其余202逐SHA不变；新增9项，无删除。
  玩家/build32 DLL仍精确2D4B，War3/x32dbg继续保留，编译进程为0，root CHANGELOG不变。

## 2026-09-16 — PID13216闪退现场与取证进程内存边界（OFFLINE，未交付）

- 用户明确今晚先排查闪退，闪烁待其返回后补样本。保留War3 PID13216/x32dbg PID32228暂停现场，
  未继续/单步/终止/重启、未部署或自动测试游戏。现场/build32仍2D4B /35,865,159 bytes。
- 用户截图：nvoglv32 EIP5C2D71D7读取[EDI+30]，EDI=0；LastError8/LastStatusC0000017。
  进程virtual=2,040,848,384，EXE无LAA；优先怀疑32位地址空间压力，尚无完整异常/分配来源证明。
  主线程“PointLightCount+大偏移”经exact符号还原实际为pthread_spin_lock/getspecific；不能归因点光。
- Computer Use按用户授权读取日志页；MCP先报启动失败却随后仍打印Started，50301实际无监听。
  管理员窗口导航无可验证效果，有限刷新/重试后停止，不改安全设置；OpenProcess也error5。
  旧PID43700 dump没有冒充本次；两份新截图与当前源/现场身份在专项文档固定。
- 收敛单一取证进程内存准入：区分VA、commit、最大连续块与原VRAM/磁盘门；CPU环分配前计入
  完整未来输入池并保留256 MiB余量，查询/预算失败与真实bad_alloc均不提交active/代际。
  图片仅PendingArm增长期间复检，失败清理未提交的私有图片批；不改GPU在途释放/同步。
- 输入槽buffer/fence/vector改为全部准备好再发布，避免部分初始化占槽；增长失败停止该owner后续
  输入采集，保留已有内容并以失败结果导出。不会以拒绝Caster、改变蒙皮/Shader或关闭阴影掩盖崩溃。
  这是实际数据/所有权边界的渐进改善，不是全库重构；稳态复用路径无新增逐draw OS查询。
- 184项定向Python/static通过（70+20+9+60+16+9）；新Win32真实采样/边界45检查、生产Control
  故障注入86检查通过，含new[]失败、代际不变及正常两次恢复。旧两profile×五配置共10入口，
  生产Control/export679检查和disabled3检查通过；5个CPU可执行文件由新源码编译，合计14个测试入口。
- 两份GPU调用方生产TU（history/input）仅fsyntax-only通过；没有Ninja/dry-run、产品对象重建或DLL链接。
  新测试先纠正100MiB严格大于断言；生产static_assert抓到Draw=1040 bytes，元数据界限向上到1088，
  最大CPU+input预算由初估316修正为320.5MiB。编译器生成头/cwd路径问题已修正，不掩饰首次失败。
- 现有2D4B对新源码为stale；未提供新防护DLL，不更新稳定CHANGELOG，不提交/发布/合入Water。
  后续仍缺完整异常/VA区域表、压力中的长期增长/安全降级，以及同图recorder off/on自然退出实机。
  详细证据、CPU测试目录和一手依据见
  [PID13216专项审查](../research/2026-09-16-player-crash-pid13216-memory-admission.md)。
- 收尾status 204→209，仅新增5个文本/测试路径；原204项中本轮10项变化，其余194逐SHA不变，
  没有删除路径。两份用户截图及该PID的runtime/crash-handler状态已CreateNew复制到独立ignored
  `crash_memory_pid13216_20260916_r1`，逐SHA与原件一致。2-file py_compile和diff检查通过。
  玩家/build32仍2D4B，根CHANGELOG与HEAD相同；零编译进程，但War3/x32dbg暂停事务仍存在，
  因此不虚报游戏资源已释放，也不启动新的实机。

## 2026-09-16 — 内部构建默认取证（限定实机通过）

- 用户从普通入口启动没有取证：上一版仅启动器设置默认off的运行开关，自包含线程不等于默认启用。
  新增独立 Meson `warvk_internal_frame_recorder`，项目默认false，本内部build32明确true。
  不使用 NDEBUG，不联动其他 observer/渲染候选；四项录制开关由单个生产解析入口统一决定。
- 内部构建未设置变量时 CPU/raw inputs/actual draws/local owner 默认开启，HUD明确内部诊断构建。
  显式0/非法值可关闭，总开关0关闭全部；Release配置仍原默认off。原蒙皮实验默认语义不变。
- DLL worker在arm之前检查输出盘6 GiB，保留现有图片4 GiB上限、显存余量、CreateNew和失败提示。
  同步增加普通启动说明，启动器只作为可选实验矩阵工具；没有改系统环境或玩家DLL。
- 165项定向Python/static通过。旧static对默认off字符串的断言改为Release选项/单一解析入口合同，
  新真实生产Win32测试对两种profile分别覆盖unset、0、非法、外部控制、显式on；共14个CPU入口通过。
  static修订中一次self.root变量拼写已纠正重跑；未弱化原输入范围/同步/生命周期断言。
- exact DLL预检/实际10/10（9 TU+link），Below Normal/-j2、no-work；PE32/pei-i386/i386，
  35,865,159 bytes / `2D4BCB400CBCA7524403E5C0939C679450865A4A48B47739941F9D697077FBC5`。
- `internal_defaults_r1_20260916` / PID35528：不传四项取证启用变量及输出目录变量，envOverrides/effective
  双矩阵均确认缺失；没有PS1启动器或watcher，零recorder control命令、零global input。
  实际隔离2560×1440、8次相机移动/俯仰后快捷键notice1，自主保留104帧、1.0022378秒前窗，重建14137条输入。
- 默认文件实际进入 `E:/Work/War3/WarVK/Log/FrameEvidence`，CPU/图片/input完整性门通过。
  manifest 38,799 / `E99F542BB3194470298CB1D3B050AAF61C66D40D31DC7BE688F8468F08AEC12B`；
  CPU 115,547,645 / `9411C1EFA38FADA2E37A8C28FEC48937CF296B93C82F8E34E2AB4FA4014F59FA`；
  inputs JSON 102,401,054 / `7CA64EEFCD8AF3DAC81386D893E827D32222E86880A351EC46C4066971853234`；
  binary 150,978,248 / `E1F85591980602A1813FFD1DE65766F55DF8E70648348AF34432954047269975`。
- 正常退出0，native witness/隔离桌面/视频结算通过；测试现场74CC恢复、玩家E8E1/地图不变，
  新dump/GPU事件0。用户/机器级四个取证变量均未配置；没有替用户修改环境或玩家DLL。
  注意AutoTest仍使用内部API控制相机/退出，以及独立显式蒙皮候选矩阵；它们不是取证开启所必需，
  不宣称模拟了用户所有快捷方式、画质、难度或高压图，也不宣称裂缝已修复。
- 唯一新视觉审查为该轮 `hud-complete.png`：明确显示“内部诊断构建：默认开启取证”“原始包已保存”，
  无需后台连接；仅确认HUD/保存状态，未按该单图接受阴影质量。原TGA保留，未重复查看旧样本。
- 收尾165项定向检查、5-file py_compile、diff检查通过；status 198→204，原198项中本任务13项变化，
  其余185逐SHA不变；新增5份文本/测试文件及1个原clean配置文件的修改，没有删除路径。
  内部开关由单一实现文件读取生成配置，不让整个渲染库通过NDEBUG隐式改变功能。
- 已CreateNew冻结桌面 `WarVK-v1.22-internal-recorder-default-on-20260916-2D4B.zip`，49份成员与清单CRC/SHA通过；
  ZIP 8,704,694 bytes / `4626C0E672FEBD36E268F742054F7BB75E3C87C65137D082735DA965C0A09D81`。
  包含普通启动说明、限定实机receipt/preflight/env，manifest明确internalRecorderDefault=true，
  requiresRecorderLauncher=false。打包拒绝内部构建与receipt候选身份不一致；未自动安装或发布。

## 2026-09-16 — 内置取证 + 快照发布组合 R4（限定通过，非视觉修复）

- exact DLL Below Normal/-j2 实际 49/49，随后 no-work；PE32/pei-i386/i386，35,864,346 bytes /
  `E8E1E78F0ACB2CFE001151A06F15B0D2A49D690CD36315857DCF2AD7B6A44A67`。
- `local_owner_r4_20260916` / PID 12752：实际隔离 2560×1440、8 次相机移动俯仰；
  零 watcher、零 recorder control 命令、零 global input。DLL 独立保存 106 帧 / 前窗 1.0037578 秒，
  CPU producerLosses=0，输入门通过并重建 13,932 条绘制输入，四项实际 configuration 均为 true。
- 当前捕获窗口 snapshot-alloc/v1 失败事件为0；这是普通编辑器地图的记录结果，不是高压预算耗尽测试。
  semantic 32 次决策仍先行拒绝 NoWorldTransform，没有正向 semantic palette 消费证据，不能以拒绝全部过视觉门。
- 正常退出码0、exact native/桌面/视频结算通过；测试现场恢复74CC、玩家8133及地图不变；
  新 dump/GPU event 为0，收尾无相关游戏/编译进程。未向玩家目录部署，不是生与死压力或玩家接受。
- manifest 54,623 / `985903845BAAE47A4178D1F0707DD13C763782BE87634947EC777300C458EC1F`；
  CPU 115,421,352 / `E330C7788D378B044B0094286B44B39489D738ECF617B89EA6232D6AF48CEC30`；
  inputs JSON 104,563,019 / `5601019C76FDEAEB41CD584F1E1B73735A631A14A78C3CE974A415EB933F90A7`；
  binary 150,985,360 / `17A0CBA7C7C497071113FC2F8C1C684758067DBD8BB2DBBBF9FD785E1C1D8D08`。
- 159 项定向 Python/static、8-file py_compile、PowerShell 独立 Parser 语法检查通过；4 个 CPU runnable
  分别1721/679/3/73检查通过。一次误把ps1传给py_compile的工具调用已纠正为对应语言解析，不是脚本缺陷。
- 工作树原189个 dirty 路径全部保留；其中21个与本问题相关文件变化，其余168逐SHA不变。
  新进入status的9项为7个新文件与2个原clean文件的相关修改，合计198项；无提交/推送，根CHANGELOG不变。
- 待解决：高压图入图后的合法caster连续性/坏帧对应、armed/exporting中途退出、正常玩家视觉与长期稳定。
  新候选包用于改善证据可得性与局部异常安全，不把本轮两项工程修复冒充全部阴影问题闭合。
- 已 CreateNew 冻结桌面 `WarVK-v1.22-built-in-recorder-20260916-E8E1.zip`，40份成员及清单逐SHA/CRC通过；
  ZIP 8,687,237 bytes / `82A93729ACFC1124E49AE7989F4B0FD587DA44524A7C9A6D2B52050AE5A3AA35`。
  内含完整启动器、默认内置调度、离线分析工具和相关源码/文档（非全库源码包）；仅文件交付，没有安装。
  包中开发日志是打包前的R4验证快照；本条包装身份在冻结后追加，不改已冻结包。

## 2026-09-15 — DLL 内置取证调度与冻结导出内存收敛（CANDIDATE）

### 快照页 CPU 发布异常安全（离线已验证，组合实机待验）

- 收紧一条实际发布链：private page → vector 接纳 → resident/id/create 计数；
  std::bad_alloc 时保留原所有者状态，未知异常不吞掉。Vulkan createBuffer 行为、页回收和上限不改。
  同时在录制中的失败路径保存准确预算/容量/发布原因，避免把所有拒绝叫作显存不足。
- 159 项相关 Python/static、4 个 CPU runnable（新 publication 故障/恢复 73 检查）通过。
  新测试首次 Win32 编译发现同条 auto 声明混用 size_t/uint64_t，已拆开并重跑通过。
  因策略头原本被 device.h 包含，exact DLL 预检为 49 edge（47 TU + dxso 静态库 + DLL）；
  后续 Below Normal/-j2 构建及 R4 已闭合，见上方9月16日记录。不是当前玩家闪退根因已确认或已修复。
- 实施合同、事件字段及 R3 HUD 单图结论见
  [所有者与发布边界](../research/2026-09-15-recorder-owner-and-snapshot-publication.md)。

### R3：无外部 watcher 的限定隔离门通过

- `local_owner_r3_20260915` / PID 5184，候选 `C14231B0...92A6B` / 35,860,049 bytes：
  实际隔离 2560×1440、8 次相机移动/俯仰，零 global input、零 watcher、零 recorder control 命令。
  安装后的窗口快捷键 notice=1，DLL 自主 arm/冻结/导出，不依赖外部 ack。
- 104 张图片，前窗 1.0087939 秒；CPU/输入/图片丢失门通过，离线重建 14,077 条绘制输入。
  schema7 明确记录四项实际配置，skin contract=true。基础原始导出通过不等于根因证据完全或视觉修复。
- manifest 53,487 bytes / `301CDBDDC8BB5B9FF617C5DFC0517527393E8CD1800F248C886BEC1B6D1F4878`；
  CPU 115,333,000 / `B26FC01D04E0E5C10FBC0BFAE475B093951CAFD01112A0E52140328728873F76`；
  inputs JSON 102,498,254 / `6ABA10B74BA442D78AA054347CC8032B03EFA2D07D53F0A1F2C10EBF3FFEDE8B`；
  binary 150,985,880 / `C04D2F48DB37E206C0A5ABC8205E50E2E2B0B7482044115A82239DDFC7A92F2A`。
- 正常退出码 0，native witness/隔离桌面/视频结算通过，测试 DLL 条件恢复 74CC；
  玩家 8133 与地图不变，新 dump/GPU event 为 0。证据位于 `AutoTest/artifacts/self_contained_recorder_runs/`。
- 限定缺口：本图 32 条 semantic 决策均以 reason=3 拒绝，未覆盖正向 semantic palette 消费；
  armed/exporting 中途退出尚缺实机门。高压崩溃、俯仰后阴影消失和裂缝仍未闭合，未部署玩家目录。

### R2：自主导出完成，但 CPU schema 未升级（合同失败）

- schema7 修订：60 项 CPU/history 读取器测试及 3 个 Meson runnable（真实 control/export 增至 679 检查）
  通过；exact DLL 2-edge 构建/no-work，35,860,049 bytes /
  `C14231B0E49071FC74E413003370777741F2F02BB526E5A2D348B07F6AD92A6B`，待独立 R3。
- `local_owner_r2_20260915` / PID 10188：零 watcher、零 frame-control 命令，真实快捷键 notice=1；
  101 张图片、1.0050917 秒前窗、CPU producerLosses=0、图片 lockDrops=0，自主写出 incident。
  CPU JSON 115,262,688 bytes，inputs JSON 102,382,207 bytes，binary 150,986,336 bytes。
- 严格读取器拒绝 `root fields mismatch`：生产端新增 effectiveConfiguration 却仍标 schema6。
  不改 R2 原证据、不放宽 schema6。后续生产改 schema7，明确四个必需 bool 字段，保留旧 1–6 精确合同；
  增加新旧格式、缺字段/多字段/错误类型和外部模式测试，另行新编号实机。
- 该轮正常退出、条件恢复测试 74CC、玩家 8133/地图不变、零 dump/GPU 事件；未算全门通过或视觉修复。

### R1：就绪前 Reset 导致本地调度提前退出（判退）

- 修订离线：3 个 Meson runnable 再次通过（session 增为 1721 检查），相关 44 项 static/runner
  与 py_compile 通过；exact DLL 3-edge 重建/no-work，35,860,049 bytes /
  `6C4A172A07187A7B1BE88D15EC75ED6B66B06788DD0CAD36A39305545980E015`，待独立 R2。
- `local_owner_r1_20260915`：隔离 PID 4668 正常进图、实际窗口 2560×1440，8 次相机俯仰/移动后
  shortcut 回调 handled=true/notice=2，未产生 history 目录，因此不是有效采集门。
- 源码复核发现窗口/设备就绪前的 reset 会设置历史 cancelled；旧外部 arm 会清除此状态，
  新 worker 却在尚无 session 时把它当成终止。修订为只有实际 session 存在后的 reset 才终止采集，
  teardown stopping 始终立即终止；补生产状态合同测试及快捷键回包的实际状态/错误。
- 正常退出、隔离桌面/视频状态结算、测试现场 74CC 条件恢复、玩家 8133 和地图不变均通过；
  新 dump/GPU event 为 0。R1 原证据保留，新修订须另跑新编号，不把 R1 追认为通过。

### 初版离线检查

- `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=1` 在已有帧取证 opt-in 下启用 DLL 工作线程：
  等待地图渲染就绪、独占 CPU/图片会话，图片完成且 CPU writer 排空后导出原始 incident。
  保留原图片/输入采集及外部显式控制模式；玩家启动器默认不再启动或依赖 Python watcher。
- 热键改为代际绑定邮箱，不因 owner/control 锁忙直接丢失；工作线程在 teardown 时先停止/join，
  再释放 registry 所有权，导出取消保留已有文件，不隐式重试、覆盖或重新 arm。
- CPU 导出使用冻结环的只读指针排序和分批写入，取消第二份完整 Event 数组；Win32 满容量
  排序索引最多约 1 MiB。此项减少额外导出内存，不代表已证明玩家崩溃根因。
- CPU 证据新增实际生效的 skin contract/inputs/local-owner 状态；基础原始包保存与离线完整性验收分开。
  当前仍每进程保留一个事件，未知中间附件/pixel draw-ID 不冒充完整根因证据。
- 离线：143 项定向 Python/static、相关 py_compile、启动器 PowerShell 语法及 diff 空白检查通过。
  Meson 新注册 3 个 CPU runnable 入口：session/mailbox/生产环 1717 检查、真实 Win32 control/export
  678 检查及关闭态 3 检查通过；不等同于 GPU 或玩家视觉验证。
- exact DLL 构建/no-work 通过：PE32/pei-i386/i386，35,860,085 bytes，
  SHA-256 `448351AB65CA5CF990D6540463EAF840520A2B512502AA2C383A66BE33F9A7F5`。
  玩家 `81339DFC...89EFA` 未覆盖；下一门为零 watcher 的隔离 1440p 单轮取证与退出/恢复。
- 裂缝、俯仰后阴影消失/闪烁和高压崩溃尚未修复或接受；不修改 Shader/蒙皮选择策略以掩盖现象。

## 2026-09-15 — 渐进式架构收敛约束（DOCS ONLY）

- 按用户要求，将“修相关模块时顺带收紧架构边界”写入本集成树 AGENTS，详细规则见
  [长期计划](../plan/2026-09-15-incremental-architecture-convergence.md)。不跨树传播，不进行全库重构。
- 固定实际输入与来源/有效期一起交接、单一决策入口、分层生命周期、真实生产交接测试及性能预算；
  fail-closed 必须同时验证合法 caster 连续性和恢复，不能把阴影不画当成修复。
- 明确基础取证不依赖外部 watcher 是待实现目标；当前裂缝、阴影消失和崩溃仍未闭合。
  同步纠正 AGENTS 顶部候选“未实机”的旧表述，保留实际开关待核实与玩家回归未过门。
- 本 checkpoint 仅修改 AGENTS、本日志并新增一份计划；本地链接通过，git diff --check exit 0，
  dirty 路径由 188 变为 189，仅新增计划；原 188 项中除 AGENTS 与本日志外的 186 项逐 SHA 不变。
  不改 C++/Shader/稳定 CHANGELOG，不构建、不部署、不启动游戏；文档检查不代表任何运行修复验证。

## 2026-09-15 — 生与死崩溃分流与隔离复现准备（DIAGNOSTIC）

### R1收尾：入口失败，不计压力回归

- 48676隔离进程120秒ready超时：jassReady=true但gameStarted=false；没有arm记录环、
  没有相机巡航/难度选择，不启动无意义的recorder-off对照。
- 申请1440p但ready前backbuffer仍1902×963，原窗口修正未执行到；不是匹配玩家条件的有效运行。
  后续仅Python把尺寸修正前置并补入口失败内部截图，尚未再跑。
- 245条外部内存采样，主动收尾前没有自然退出；主动EndGame/收尾exit0、无force。
  恢复测试74CC、玩家8133/原图不变、编辑器未动、零游戏/编译进程、GPU事件/新dump0。
- 6项定向Python及两文件py_compile通过。没有改C++/DLL，不宣称撕裂/高压崩溃已修复。
  详见`2026-09-15-life-death-player-crash-triage.md`；下一步首先闭合无输入的入图路径。

- 用户澄清：19:32约为fresh process入图/选难度后退出；19:36/19:38同图。
  三次watcher只证明记录环达到一秒后连接消失，空history目录没有崩溃栈/退出码。
- 19:43:56 PID43700的C0000005 execute dump来自另一轮x32dbg尝试，尚未入图；
  不用它归因普通游玩退出。原dump、JSON与日志冻结到skin_palette_crash_43700_20260915。
- 新纯Python单轮诊断器保留进程启动HANDLE，在独立线程0.5秒采样private/working set，
  每5秒只读VirtualQueryEx低4GiB元数据；自然退出码先于测试收尾记录。
  完整记录/不记录对照只改变三个recording env，五项palette候选门保持1。
- 只在E:/Work/War3按74CC备份恢复；玩家E:/Work/Warcraft III的8133 DLL、原图和编辑器不动。
  复用5x5相机路线但不执行旧脚本的难度点击；难度未选中不能冒充正常游玩。
- sampler本进程只读ABI检查、py_compile和身份preflight通过；尚无新实机结论。
  计数定义依据Microsoft PROCESS_MEMORY_COUNTERS_EX / VirtualQueryEx；private commit不等于
  显存或全部虚拟地址占用。不修改C++/DLL或稳定更新日志。

## 2026-09-15 — 玩家启动路径澄清（未代为启动）

- 玩家从游戏目录运行复制的AutoTest脚本，游戏根目录缺少candidate-manifest.json；
  启动器在启动游戏前按预期拒绝。桌面完整包27份成员SHA全部仍匹配。
- 游戏DLL只读核验仍为81339DFC…89EFA正确候选。无需重编译或重新替换DLL；
  改为使用桌面完整包中的启动器绝对路径并指定GameDir，保留身份校验。
- 仅回传正确命令；未修改冻结包/游戏目录、未启动游戏或占用GPU。

## 2026-09-15 — Palette 来源候选：第一阶段离线实现（尚未实机）

- opt-in `DXVK_WAR3_SKIN_PALETTE_CONTRACT=1`：禁止无效 part slot 的缓存槽位回读；
  live helper 只接受当前 model/part/frame/slot、同 CPU publication epoch 的 owned snapshot，
  失败不会落入 arena/cache/Pose/CModel 猜测。默认0保留对照路径。
- 精确捕获 decoder 传播所选快照的来源；packet、live replacement、lease、canonical 传递
  同一 selection。World group palette 不再因 Ready 标签被错记来源或重复 world transform。
  当前无法证明的原生 slot allocation generation 保持0，不冒充 CPU ticket。
- 增加 per-cell 非阻塞锁保护 owned 矩阵读写、复制后 slot/frame 重核、epoch/ticket 不回绕。
  证据附加 typed selection 和拒绝事件；原 provenance 数组不改义。来源 counter 贯穿 report。
- 新 Win32 runnable 已通过40项（实际 canonical adapter、拒绝、数量/来源/替换/reset）。
  首次 exact DLL dry-run 为84 edge（82对象、1静态库链接、1 DLL链接），未涉及shader/generator。
  尚未完成本轮DLL构建/静态回归或玩家视觉测试，不能据此声称裂缝已修复。
- 后续离线复核：84/84 exact DLL构建已成功；新增來源reader 8项和既有8份定向脚本通过
  （single palette owner 3、single reset脚本、scratch 12、world transform 3、bounds 7、
  inputs 24、CPU evidence 40、control 13）。未把这些称作全量回归。
- 收口审查补了producer身份读取前的epoch witness、提交前再次核最新publication ticket。
  来源事件改用通用ShadowState kind12，避免冒充旧CasterInput而误报missing binding；
  对应增量dry-run为device.cpp+model_hook.cpp+DLL link共3 edge，最终构建中。
- 最终收口：3/3增量成功、exact DLL no-work。PE pei-i386/i386，35,834,384 bytes，
  SHA-256 `81339DFC85B3C3CD507FBE249E6160160A8D0A920D3CAD01383158B85BF89EFA`。
  重跑40个Win32断言、14项接线、8项reader通过；4份新Python py_compile、
  PowerShell启动器语法、git diff --check通过。旧8份定向脚本仍不代表全量回归。
  本轮最终status=185；未清理/重置其他已有变更。零游戏/编译进程，编辑器PID38544保留。
- 交付准备：独立候选包将包含精确DLL、冻结源码子集、manifest、取证reader及
  `launch_skin_palette_candidate.ps1`；启动器显式设snapshot/live-refresh相关前提，
  默认候选门1，`-LegacyComparison`仅改变候选门为0，其它前提相同。
  没有部署、启动游戏、关闭编辑器、提交或推送；未完成本DLL实机/视觉/性能验收。
  安全拒绝引起的缺影必须和裂缝一起审查，不能把没有裂缝冒充修复通过。
- 本地CreateNew打包成功：桌面 `WarVK-v1.22-SkinPalette-20260915.zip`，
  8,632,708 bytes / `3EB7A50923638B51B2CF17149A7F6CE6D06FFFD68837CB51BE29843BED5A40D9`；
  27份成员原字节核验、ZIP CRC和逐成员SHA通过，DLL源复制前后不变。
  包内源码/日志冻结于本条打包回执之前；外部源码后续改动不得冒充该包。

## 2026-09-15 — 独立审查报告复核：来源契约缺口采纳为下一开发任务

- 外部报告 `WarVK-independent-review-ZH.md` 与复核包 ZIP 身份已验证；原研究包 SHA
  `9C1C49E3B6D15375D0F0237F58043C87475D0F83135C581233B02212A7C0D77B`，报告/审计成员
  与原始 2170 成员均通过 SHA。Windows 下外部 `verify_integrity.py` 的反斜杠 extra-file
  判断存在可移植性问题；未修改外部脚本，使用等价 POSIX 路径规则完成 2170 成员复核。
- 按审查包的 reader、scalar、reaudit 重新运行：JSON 数值等价、1848 个数组键等价、
  最大数组差0，98坏段重点记录、48 indexed、49 surface/volume 对、32340角点、
  scalar最大差 `2.842170943040401e-14`、group8精确矩阵命中10条。原始ZIP、交付包、
  下载报告和源码工作树在复核期间未变；无编译、游戏、部署、GPU或网络动作。
- 审查提出的三项下一阶段重点与当前代码相符，但仍是静态缺口/高优先级假设，不是本轮
  实机分支命中：`War3TryBuildLiveRuntimeGroupPalette` 取得 boundGroupCount/frameTag
  却只返回slot；`CaptureRuntimeGroupPaletteBindings` 在slot无效时按part缓存回读arena并
  重新盖当前frameTag；submit live rebuild后真实source没有完整贯穿到canonical provenance，
  FromRecord也可能不填paletteProvenance。不得把 `CurrentDrawPaletteWorld` source=5
  当作palette来源证明。
- 下一开发任务冻结为：先做不需要GPU的 A/B slot重分配、actualGroupCount不足、FromRecord→
  live→canonical来源传播和同owner合法更新单测；再申请定点运行时字段（owner/generation/
  writer ticket/groupCount/source/space），最后才评估行为修复。当前没有修改上述C++/shader，
  不关闭所有动态caster，不以固定边长或组索引钳制规避问题，`rootCauseReady=false`保持。

## 2026-09-15 — VIDEO25–29标注闭合：蒙皮不等价、surface/volume共享输入与研究包

- 玩家明确坏帧25–29并观察到体积阴影；对应render2188–2192，至恢复30的QPC40.6008ms。
  原始TGA的24/25/29/30 ROI已审查，确认细长暗条25出现/29仍有/30消失；宽暗区与玩家
  体积阴影观察保留为相关现象，不能当作未采集的体积中间图。新附件最近匹配29/30。
- 坏段重点98/98均可复算，12字节蒙皮48条所需position/IB/blend/matrix齐全。
  同part/handle/layer相对IB顺序一致，1051117最长三角形边38.8269→390.2137；
  1051099/1051135也出现拉伸，1051063有大幅平移，1051081对照仅约1e-5角点偏差。
  49对surface/volume复算world角点完全一致，蒙皮实际提交surface4级联/volume2级联。
  独立scalar不调用numpy reconstruct，308条/32340角点最大差2.8422e-14通过。
  研究优先级收窄至palette/组/实例/坐标来源，但仍无pixel draw-ID，未定罪具体代码行。
- 新分析脚本和报告位于`AutoTest/investigate_marked_input_skinning_53372.py`、
  `AutoTest/verify_marked_skinning_scalar_53372.py`与
  `docs/research/2026-09-15-marked-25-29-skinning-evidence.md`。不使用旧样本帧号，未改原证据。
- 最终桌面包`WarVK-53372-VIDEO25-29-Research-20260915.zip`：138773523 bytes /
  `9C1C49E3B6D15375D0F0237F58043C87475D0F83135C581233B02212A7C0D77B`。
  2171成员；完整当前检出1767份C/C++/头/着色器等代码（含105着色器）、必要vendor许可证/
  构建文本及项目文档；原尺寸无损PNG18张（17–34）、全部所选batch含缺失状态、原始pin和
  可复算工具。18份候选pin源码逐字匹配；其它源是带SHA的同树补充，不冒充历史全构建证明。
  未包含其它Water工作树、游戏MPQ/模型、DLL/dump/IDA库/.git或编译缓存。
- ZIP CRC和每成员SHA通过；从ZIP提取工具/数据再跑：scalar通过、选定窗口36batch
  850条可复算/重点308/308，结果与本地派生分析相同；源文件打包前后SHA保持。
  提示词已补新发现和体积阴影观察，旧桌面草稿标记被最终版取代。无上传、构建、部署、实机、
  C++/shader修改或发布。py_compile/diff检查通过。包内开发日志为封装前冻结快照，本条记录封装复验。

## 2026-09-15 — 玩家53372新录制可用性复核与研究准备

- 重核桌面候选包45份pin，再用冻结reader完整重算玩家history53372的CPU/history/input
  报告，逐项与原导出报告一致。原始旁路input bin和history副本同SHA，所有源JSON/TGA
  前后SHA不变，未修补证据。独立脚本`AutoTest/audit_frame_input_capture.py`，结果见
  `AutoTest/artifacts/player_input_audit_53372_20260915/`及
  `docs/research/2026-09-15-player-input-capture-53372-audit.md`。
- 126帧2560×1440，VIDEO0–125=render2163–2288，pre122帧/1.0065556s，post4。
  全图都有exact CPU-copy/pipeline/shadow/actual-draw对应；图像窗口内focus2272/2272
  可复算，缺输入帧/重点帧/未配对CPU scope均0，CPU/GPU input调用丢失0。全输入环
  576次调用focus4830/4830，口径比图像窗口更宽。非重点容量缺失与rootCauseReady=false保持。
  录制前后磁盘DLL6D6C与冻结源码包相同，但loaded-memory校验false；地图未指定/无pin，
  env继承Water=modern不是实际Water运行证明。不能把可用性升级为根因确认或完整GPU取证。
- 只审查新录制VIDEO0/62/125缩略拼图，确认有效游戏场景与内容变化，不判定具体坏帧。
  当前尚未获得这轮玩家坏帧标注，不能复用旧69–71/103–106。无需因当前采集数据再录一次。
- 桌面`WarVK-53372-PR-review-20260915`已生成无标记ProRes4444 MOV与编号MP4，
  一图一帧、126帧/60fps/2.1秒，ffprobe数量/分辨率与完整解码通过、源TGA重新核SHA不变。
  MOV315916058 bytes/SHA509559B2…FB9E5；MP444730247/EB80211F…CA008。
  视频仅定位用（有RGB→YUV变换），原TGA才是像素证据。CSV保留QPC与双帧号映射。
- 研究提示词已准备，明确目标、来源pin、已知假设、采集器自身审计、证据限制和预期产出；
  根据OpenAI Docs研究提示指导组织。本轮具体坏帧/异常区域仍待填，不创建伪标注研究包，未上传。
  纯离线复核/视频/文档，无构建、部署、游戏启动或源码C++修改；py_compile和diff检查通过。

## 2026-09-15 — 补原始输入取证；按用户授权清理重复测试输出

- 新增默认关闭的有界 Stage11 输入 provider：实际 GPU 几何副本、CPU-owned 上传矩阵、
  producer/cache/material/model/geoset 来源与 DirectionalDraw batch 关联。保留容量/缺失，
  不冒充像素 draw-ID、完整 GPU replay 或撕裂修复。详见
  `docs/research/2026-09-15-frame-input-evidence.md`。新 Python/static 18/18，相关 frame 选择
  101/101（包含新18）。首轮83-edge构建完成，但后续源码修订仍待重新构建/实机闭合，未交付。
- 用户本轮明确要求清理旧日志/测试数据。只在 DXVK 各树 `AutoTest/artifacts` 内删除
  271个 >=1MiB 的重复 .log/.tga/.png/.bmp，每个删除前重新核目标与保留副本 size/SHA；
  共11.546GiB逻辑内容。没有删除DLL、源码、Git历史、报告JSON/HTML、地图、dump或唯一图像。
  R14/R15和本次局部坏帧研究目录保留；玩家175帧原始history不在清理范围。
  `AutoTest/artifacts/cleanup_20260915/{plan,deleted}.json` 保留逐路径/SHA/保留副本映射，
  内容可从这些副本恢复，但文件元数据未承诺恢复。清理后E盘约72GiB可用，增量大于本次
  删除量，差额来源未核定，不归因本操作。不自动重建已删冗余副本。
- 取证自检R16安全拒绝逻辑usage未声明transfer-source的几何，0复算，判无效；后续审计
  内部分配池真实用途，并以schema2独立GPU按实际IB采集原始顶点，保留负baseVertex/越界
  witness，避免每个小draw都复制512KiB。源码不改生产渲染输入。R17重点4951/4951复算，
  包含32字节普通与12字节备用路径；非重点仍有明确容量缺失，不称全量。R18一秒首窗口
  112帧/1.001175s、重点5043/5043；第二session watcher被旧60秒父watchdog截断，不能算通过。
  两次均恢复74CC，玩家5B6D/地图未动、零新GPU事件/dump。
- R19完整watcher通过：隔离2560×1440、global input=0；第二session保存102帧，触发前
  1.0068159秒，原始输入12048条可复算，重点5522/5522、captureDrops=0。非重点容量缺失
  继续报告，不能称全量。HUD“调试包已保存，可退出游戏”已审查局部图，未据此验收画质。
  自然退出/隔离桌面结算/恢复通过，74CC测试基线、5B6D玩家DLL、9268地图保持；零新GPU事件/dump。
  最终DLL为PE32/i386、35,805,448 / `6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1`。
  最新Python/static128项通过（107 frame+16 async+5 geoset，含24新测试）；纯CPU runtime
  27开启/3关闭与input core12检查通过，Vulkan1.3 SPIR-V检查通过。仅诊断，不修复撕裂、不发布。
- 新研究打包工具仅选择坏帧前后4帧的数据，保留全部所选batch记录、缺失、原始文件SHA和
  原始偏移，并冻结对应源码。工具smoke用的是合成标注40–41（不是实际坏帧），10幅PNG
  原字节像素回环/ZIP CRC与成员SHA通过。旧175图样本及其manifest SHA仍保持7A9243F4…B12。
- 交付冻结：桌面 `WarVK-v1.22-FrameInputs-20260915.zip`，8,819,108 bytes /
  `61832539E70FCCE136EA636E3ABA889B9BBFAC5C0CD0C2DC88FB1059052E807E`，45个pin文件。
  打包器曾将含点目录名用with_suffix误写为WarVK-v1.zip；已核精确SHA后无覆盖改名，
  后续命名改为追加.zip，未改包字节。从交付包运行研究工具smoke也通过：59,058,949 bytes /
  `DE10138BFDAD74D8AAA1CD9DE75DFAD5877428EBA67D92D1A3AABA23DC3C52A5`，缺源码0，
  CRC/成员SHA通过。该smoke无玩家启动器pin，所以sourceBinding=false如实保留。
  R19的102幅图逐帧均找到可复算输入及重点输入，missingInputFrames/missingFocusFrames均空。
  原玩家cpu-events SHA45B262FE…87E9复核未变。最终游戏/编译/runner全0，保留用户编辑器PID38544。
  未覆盖玩家DLL、未提交/推送/发布。交付包内日志是打包当时冻结快照，本条为后续封装复验记录。

## 2026-09-15 — 玩家指定69–71/103–106坏帧：备用蒙皮路径线索闭合

- VIDEO0起始编号已由像素确认：render3996–3998、4030–4033；真实QPC坏段约17.86/21.42ms。
- 匿名614顶点/1587索引候选在69–71恰好走stride12/flags0x3；此前stride32/flags0，72消失。
  四个hfoo handle的52点/105索引部件在103–106恰好从32字节Alpha快照切为12字节索引蒙皮，107消失。
  不是用临时drawIndex当稳定ID。两处恢复帧对相机不变；TAA/history均关闭，CSM重绘serial连续。
- 已核本地Footman105索引材质含opaque底层，A原本就不做AlphaTest；因此不把Alpha关闭单独定罪。
  共同嫌疑是快照→重新蒙皮表示切换的等价性，具体palette/索引/变换或像素draw归因仍缺。
- 未改C++/shader、构建、部署或再启动游戏；原始175图和日志保留。本轮恢复后按有界数据继续，
  只查看必要A/B局部，不重新加载已审查概览。详见`../research/2026-09-15-fissure-localized-route-evidence.md`。
- 8项分析机械单测、5-script py_compile与diff检查通过；源manifest/cpu-events重算SHA未变。
  producer callsite、骨骼/顶点原始数据及像素draw-ID仍不可用，未据此实施未经验证的产品修补。

## 2026-09-15 — 玩家175帧TGA离线封装为PR检查视频

- 输入`E:/Work/Warcraft III/WarVK/Log/FrameEvidence/history-34548-4617011687416-1/`，
  manifest SHA 7A9243F4E7DACB2A7F8A793FCE77B3747ED1062B17A0909C2DA2BF6AE1846B12。
  按manifest而非slot文件名排序：175帧，render/Present3927–4101、ordinal3311–3485；
  源首末QPC间隔1.0242371s。全部TGA在前后重算SHA一致，原图/日志/清单未改。
- CreateNew输出Desktop`WarVK-PR-history-34548-4617011687416-1/`：无标记2560×1440
  ProRes4444 MOV和额外64像素顶部编号的2560×1504 H.264全I帧MP4。均60fps/175帧/
  2.916667s，一张源TGA对应一帧，不插值、不补/丢帧；故意放慢便于逐帧检查，不是实时录像。
  RGB→YUV编码非像素无损，根证据仍是TGA。CSV含0/1起始视频序号、60fps时间码和原QPC/源SHA。
- MOV441,618,104 /2D2140D25E930CE5D6DBEAB55E53321D5F7AE06F421B993DD4D8BF823D0F434F；
  MP4 58,740,843 /572F1120F669EDED63CB18A78529CCD334499364E6CBC14BD477A2362ECCCA59。
  ffprobe计数/尺寸/速率及双文件完整解码通过；只看首末编号局部图，确认VIDEO0→RENDER3927、
  VIDEO174→RENDER4101。编码仅CPU/BelowNormal，未构建、部署或启动游戏，不增加撕裂根因结论。
  导出脚本`AutoTest/export_frame_history_video.py`可复用，py_compile通过；根稳定CHANGELOG不改。

## 2026-09-15 上午 — 玩家热键无效修正与一秒时间窗

- 确认旧F8接在未安装的War3WndProcHook，实际D3D9WindowProc没有处理；已改为Ctrl+Shift+C
  并接入真实窗口入口，IME之前处理，保留零global input测试边界。HUD独立显示实际状态和缓存秒数。
- 原生截图含HUD、历史图不含HUD，两阶段prepare只推进一次burst；修正watcher被拒OpenProcess
  时的退出问题，用精确processNonce/session验证回读，管道消失有界退出。
- 默认最多256前帧槽/4后帧槽，按至少1秒选取；最大约3.57GiB显存，另核1GiB预算余量。
  R15最终5B6D候选相机变化24组、132图、前窗1.0002342秒、图像与实际draw全关联且0丢失。
  R14另闭合watcher第二会话1.003s和“调试包已保存”HUD视觉。旧F8物理入口从未测试通过的声明已纠正。
- DLL35,693,151 /5B6D4E4131FED3C940498ABD2DF687CF9C6A2F7317990448B7CDB9DC4D91262F；
  当前玩家8848、测试74CC保持/恢复；未改地图/MPQ、未推送。详见`2026-09-15-shortcut-and-one-second-fix.md`。
- 最终236/236定向Python/static与6-file py_compile通过；PowerShell解析通过；独立预算/窗口/
  快捷键Win32循环断言通过。exact DLL no-work；新生成包单独命名，不覆盖原试玩包。
- CreateNew最终包`Desktop/WarVK-v1.22-CtrlShiftC-1second-20260915.zip`：9,773,746 bytes /
  31E09C4F20ED16B17B558CF654D6055830DF6C12565FD1933FB4D009E621BC0C。
  同名source-review ZIP：13,896,967 /54A80D2B99ADC45A11A5822E07A42470C4CE95B3CBD13EA6BECEAB6CFE4CA0BD，
  1,905份源码/测试/文档逐项SHA复核。此checksum为包外收口，不递归改写冻结包。

## 2026-09-15 — 后半夜：GPU历史窗口、实际draw状态与源码发布准备

- GPU最终彩色环64前+4后，触发前不写盘/CPU读回；Ctrl+Shift+F8或控制端冻结后复用异步截图队列。
  独立cell并发CPU队列修复高密度draw日志丢失，schema5保留ticket/丢失/窗口边界，冻结分批导出。
- 实際方向draw在cmdDraw前记录Alpha、纹理视图、VB/IB、MVP和surface/volumeSun标记。
  R10最终8848候选68帧逐张关联，每帧584次surface draw/388次AlphaTest，0事件丢失；
  R11后台watcher端到端第二会话通过。没有用旧队列R8的5丢失或R9的1158丢失冒充完整通过。
- 原版模型灯按决定默认关闭；玩家/测试74CC均保留/恢复，编辑器未被关闭。限定编辑器例外
  逐次核PID/创建时间/路径/模块，仅用于隔离正确性，不用于FPS。零新增GPU事件/dump。
- 最终DLL35,670,484 /8848ECC43D2699D303DA4F6B8CCB2BA4EC2ED49826308F82DE527C12B03025C2。
  最终226/226定向Python/static、6-file py_compile、Win32核心/预算/并发控制测试通过；exact no-work。
  候选、工具、源码审查包与发布清单仅准备，不commit/tag/push。完整记录器仍欠中间GPU图像
  和原始几何/纹理字节，撕裂未定位/未修复，不是正式稳定版。
- 证据、失败准备/运行和边界见`2026-09-15-frame-history-overnight.md`；根稳定CHANGELOG不改。
- 已CreateNew准备Desktop `WarVK-v1.22-frame-history-candidate-20260915-r2`及同名ZIP，
  ZIP 9,201,534 bytes /D767EA133AFE878DD942F0FE7FD468E087ECE8CED15A54B02360B9BF83EF4B05；
  源码审查ZIP 13,885,146 /3D00582A658F8A6DE4EE7E08890648E0F7F25B78D3B28B418967AFDD3C152790，
  1,903个源码/测试/文档文件逐项SHA校验，不含游戏/MPQ/用户模型/ignored大工件或外部子模块内容。
  r1归档命名误把版本点当suffix，已修正工具，误命名包移入本地artifact保留；没有覆盖或删除旧证据。
  本条是包外checksum收口，不递归回写已冻结ZIP。最后226/226、no-work、PE/size/SHA和diff检查通过。
  工作树dirty133；编译/游戏测试进程结束，编辑器38544保留，测试与玩家DLL均74CC。
- 热键接线修正：旧Ctrl+Shift+F8位于未安装的旧WndProc；实际`D3D9WindowProc`现处理Ctrl+Shift+C，
  HUD显示采集状态。R12隔离窗口通过私有窗口消息验证该路由、相机移动25种矩阵并关联132帧，
  预触发1.0018525秒、零图像/CPU事件丢失。R12仍是工具canary，不是玩家撕裂复现。
- 环从固定64帧改为至少1秒目标，默认预留256帧+4后帧（总4GiB cap，按显存余量拒绝）；
  `FrameHistory`状态manifest schema2记录实际trimmed前缀和preSpanTicks，超过预算fail-closed。
  旧Ctrl+Shift+F8不再是用户入口；包内README/PS1均已更新。

## 2026-09-15 — 逐帧诊断记录器：CPU骨架与截图关联离线候选

- 独立有界事件环、默认关闭、try-lock显式丢失、session隔离、触发后窗/冻结/CreateNew导出。
  接入Present/pipeline/pass、公共阴影reconciliation和可选有界caster输入元数据；不写热路径日志。
- 截图prepare、CS复制记录、完成保存分开，旧请求保留旧会话；离线按精确ID/帧/epoch/目标值
  关联，不按时间或+1猜测。schema2类型化字段，兼容旧schema1 CPU归档。
- 198/198相关Python/static、5-file py_compile、Win32核心39,984断言以及runtime gate-off3 /
  gate-on27项通过；最大并发压力丢失完整计数，非实际游戏开销/丢失率证明。
- 最终E21DCACE…D33541 /35,562,502 PE32/i386，BelowNormal/-j2构建与exact no-work通过。
  尚无实机：用户编辑器PID38544仍开，未自动关闭或部署，玩家/测试74CC DLL未改。
- GPU图像历史、热键/启发式、完整输入字典及全帧视频关联仍未完成，不能让玩家现在抓样本。
  原版自动模型灯继续搁置；根稳定CHANGELOG不改。细节见`2026-09-15-frame-evidence-foundation.md`。

## 2026-09-14 — 自动模型灯路线搁置；玩家短暂裂缝两组图像取证

- 用户明确因美术适配成本搁置原版模型自动点光/点阴影，移出本次v1.22计划发布范围。
  旧实现/证据保留；没有擅自回滚源码、构建或覆盖玩家74CC DLL。
- 四张2560×1440原PNG CreateNew冻结、Python有符号差分与局部图完成。
  彩色城堡脚下暗块58→59消失；因子图右树旁暗多边形却是59新增，不能统一标坏帧方向。
- 源码debugMode2在点光合成前输出方向CSM可见度（可含prepass/history）；优先审查
  alpha卡片、几何/姿态与资源代际，而不是把体积雾相关性当作根因。未找到已证实修复。
- 现有日志不具PR视频帧与引擎frame/pass映射；已列出同帧彩色/深度/CSM/vis/雾前后、
  draw ID/输入版本、环形前后窗口与显式漏帧门。机制尚未实现；按用户要求本轮停止汇报。
- 新像素分析5/5单测、两文件py_compile通过；原图/玩家DLL/地图未修改，无构建部署或游戏动作。
  证据与方案：`../research/2026-09-14-player-fissure-frame-pairs.md`。根稳定CHANGELOG不改。

## 2026-09-14 — 玩家混合灯场景：注册成功，选灯与尾段回退问题确认

### 修复 checkpoint — 双灯74CC测试候选

- 自动原生灯/点阴影测试容量1→2；显式模型路径规则优先，作者灯共享总16灯/4 cube预算不变。
  原色+去A+去B+去AB四端点解决两灯独立遮挡，保留原色alpha/覆盖与所有权安全门。
- world结束后的合规FFP draw对各端点传播相同源色，不再因lighting位独自撤销整帧；
  不在尾段认领灯。旧3B0B混合fixture复现78条回退，新74CC为0，86条双灯/12面抽样提交
  持续到最终截图后；ED122导入火把实际光点Z150.935被选中，与stock policy2同时工作。
- 148/148定向Python/static、三文件py_compile、Win32 synthetic解析路径通过；58/58
  BelowNormal/-j2 exact DLL构建与no-work；receiver及10个实机dump SPIR-V离线校验通过。
  DLL35,486,415 /74CC676BA27DF727B435515D503E6129B0AAFC91691050B1A47776270205DB73。
- 新旧各一次隔离2560×1440，四阶段JASS/异步截图、零global input、exit0、恢复及零GPU/dump。
  新stage-1/3可见导入灯阴影开关差异；原版灯附近仍有强角状遮暗，未称全阴影视觉通过。
  三私有MRT有成本，不报告FPS收益。玩家DLL/原地图/ED122原模型未改，测试恢复当前3B0B。
- 准备失败、旧日志串证据修正、完整size/SHA及遗留限制见
  `2026-09-14-two-native-lights-runtime-checkpoint.md`。本项仍是候选，根稳定CHANGELOG不改。
- CreateNew交付`Desktop/WarVK-v1.22-two-model-lights-74CC-20260914`，28文件加manifest，
  ZIP逐条原字节校验通过；16,889,158 bytes /
  4F4E20BC9363D508AC606C76A5E08A66FFFC36959C92977E6A6505D94DF5578F。
  不含用户模型；旧候选包和玩家3B0B未覆盖。打包后的本条仅记录外层归档身份，不递归改包。
  收尾dirty103（43 tracked+60 untracked），空白检查exit0、相关进程0；编译/游戏资源已释放。

- 玩家现场确为交付3B0B DLL；实际加载ED12243B导入模型，ObjectID2、本地光点Z=150、
  静态衰减300/600，路径规则enabled=1/shadows=1成功。不是旧模型未更新或JASS路径失败。
- 抽样选灯记录为policy7路灯、随后policy2原版TorchHumanOmni；单灯屏幕中心优先没有
  给显式注册模型优先权。不能把注册成功等同于该灯获得阴影。
- native-world帧481至1681的11条抽样回退均为lit-draw-outside-world；后续带光照draw
  越出world scope使整个颜色事务撤销，保留原生光、失去自动阴影。具体draw调用者尚未记录，
  不直接归因头像，也不删除安全门。receiver与world帧计数不同，日志条数不是实际渲染帧数。
- 两张玩家图已检查；右侧黑块未获正确阴影视觉接受。旧32059406模型96/96遮挡结论不转移
  到ED12243B版；此次问题不能仅归因用户灯点高度。候选仍不满足稳定自动点阴影交付。
- 本轮仅诊断、证据CreateNew冻结与文档记录，没有改C++/shader、构建、部署或启动游戏。
  日志/模型/地图冻结身份及后续精确验证边界见
  `docs/research/2026-09-14-player-model-light-selection-and-tail-fallback.md`。

## 2026-09-14 — 原版点光 producer 实机闭合；事务颜色消费者编写中

### 最新 checkpoint — 模型路径 JASS API 与默认开启候选

- 用户澄清为地图导入Desktop/Light/TorchHuman.mdx，没有改MPQ；4351/32059406…72BCED。
  新增路径启用/点阴影布尔选项、注册查询与绑定数查询，协议/JASS/YDWE一致（106→109命令）。
  支持已有及未来实例，地图退出清规则，每灯独立generation，策略修改撤销旧lease。
- producer/consumer候选默认开启，保留同步优化/异步截图/退出及太阳修复/Froxel与局部雾，
  不包含Water。自动阴影仍限每帧1灯，不是无限全灯。137定向Python/static、新Win32解析/
  路径测试与既有JAPI协议runnable通过，生成JASS pjass及MPQ原字节回读通过。
- 53-edge后3-edge构建/no-work；35476894/
  3B0B548A5BE9573005424D31FCD62B36B419C59B8C10D343F209B60EC957452A，PE32/i386。
- R17及最终同DLL普通R19真实JASS四阶段通过，绑定数1/1/2/0；阴影关时地面/步兵照明可见，
  开时仍近灯大块遮暗。R18深度诊断显示最近遮挡<20；用户光点仅高杯沿0.61，静态96/96向
  地面射线仍被杯体挡。不能将注册/提交通过冒充完成点阴影视觉或原版自遮挡修复。
- R17/R18/R19均隔离1440p、零global input、四异步截图/正常exit0/恢复通过；零GPU事件/dump。
  测试A570恢复，玩家B1FCC/原地图/原MDX不改。候选用于用户编辑器/游玩验证，不是稳定发布。
- R15/R16仅地图准备失败：StormLib bool封送错误误报文件存在，按I1/缺失哨兵/字节回读修正。
  失败工件保留。具体限制与证据见model-path-point-light-japi研究，根稳定CHANGELOG不改。
- 打包前发现用户在19:56再次保存桌面模型为4351/40B3B21F…7DFFF；不是上述32059406测试版。
  首次打包在创建目录前按身份门停止；交付调整为仅DLL/SDK/说明，不附模型，不覆盖用户资产。
  新模型尚未做实机/几何验收，旧版本自遮挡结论不得外推；自定义API本身不锁死测试模型SHA。
- 交付目录`Desktop/WarVK-v1.22-model-light-candidate-20260914`以CreateNew生成；17个文件加
  manifest，ZIP逐条重算SHA与源/目标一致，无模型资产。ZIP为8072970/
  919684743EE685BBACF4BFAFB20B0253104E862AABF95B8799601E1D537E7399。
  最终137/137复跑、diff空白检查通过；测试A570与玩家B1FCC身份未变，相关进程为0。
  包内日志是生成归档前的冻结快照；本条记录外层归档身份，不递归回写归档。

### 以下为前序阶段历史

- R1 `native_light_producer_r1` 因 Game.dll 实际ASLR地址与旧固定基址守卫不符而未启动producer，
  永久保留为未通过；游戏正常退出、测试现场恢复，无新增GPU事件或dump。
- 改为对精确E04D文件的PE32 HIGHLOW重定位逐字重建8-byte入口，不关闭ASLR、不放宽模块SHA。
  actual Game.dll重定位Win32测试通过，40803A03…01D83 /35,384,810中间DLL构建与no-work通过。
- R2 `native_light_producer_r2` 在隔离2560×1440真实地图中识别3份原生资源、3模板/6克隆；
  实例随JASS创建/删除变化，最终destroyed=4，reject/overflow均0。四阶段通过、自然exit0，
  A570测试DLL恢复、B1FCC玩家DLL及原地图未变，无新增GPU事件或dump。
  这仍是producer验收，不是自动照明或点阴影验收。
- 新增双颜色附件事务方案：RT0保留原版，精确身份限定的RT1移除原生direct/specular，
  完整cube与receiver成功后才commit。异常/不支持状态保留原版；初期作者JAPI灯独占优先。
  新旧定向38项Python/static及3-file py_compile通过。C++构建仍在进行，未向玩家交付。
  公式、FFP不可逆性和Vulkan附件边界见 `docs/research/2026-09-14-native-light-transactional-color.md`。

### 事务消费者 R3：安全回退，不是照明通过

- 87FB4EE6…CE9291 /35,432,479，PE32/i386；增量构建与exact no-work通过。
- `native_light_consumer_r3` 四个原生fixture阶段及四份2560×1440异步截图通过，
  正常exit0、现场A570恢复、玩家B1FCC和地图未变，GPU事件/dump=0。
- 该地图本身含一个WarVKCreatePointLight；第一版作者灯独占规则阻止全部automatic lease，
  receiverCommits=0，整轮明确判为未通过。不可使用这份DLL声称已经看到自动点阴影。
- 已查看stage-0原始图：火把/火盆、两名fixture步兵及正常场景可见，无自动接管证据。
  该轮5份dumped SPIR-V通过spirv-val，但不包含已运行的paired变体。
- 修订为固定作者shadow prefix优先，自动最多2灯、总4 cubes/16 lights；失败按原设置与
  冻结作者快照仅尝试一次canonical point shadow，不复制完整scene、不重新查询可变作者状态。
  39项定向检查通过，修订构建进行中。未部署玩家目录、未接受产品。

### 事务消费者 R4：内置监听器准入区分

- 5965BE50…8D1DEC /35,437,205已完成53-edge与后续2-edge source-consistent构建、no-work。
- `native_light_consumer_r4`仍无commit；新增逐项诊断证明pipeline/wants/receiver/shadow均1，
  listeners=1。源码确认HasAnyRenderListeners会无条件注册UserExample，其回调仅更新后处理参数，
  不写场景颜色。四阶段/四截图/正常exit0/恢复通过，无GPU事件或dump，不追认为功能通过。
- 新准入按精确内置回调函数地址识别这个只改参数的模块；未知模块/外部回调仍回退。
  排队执行时再次记录外部改色危险，防止覆盖pre-pass事件输出。初始Clear发生在颜色副本创建前，
  不应误算为已有lease被改写；创建后Clear/Copy/不支持draw仍撤销整帧。
- IDA三个函数注释已追加并读回保存：0EF2D0/0EFB60的0x68明确为104bytes、0F01E0的BGRA解码；
  旧注释原样保留，native代码未改。当前新构建/后续R5验收未闭合，继续不可交付为完成DLL。

### 事务消费者 R5：双输出实际执行，最终交接仍未通过

- 54ECC863…984612 /35,437,274，PE32/i386、no-work。四阶段/四截图/exit0/恢复通过，GPU/dump=0。
- 配对FFP变体已运行，但world返回后至BeforeUi间隙触发outside-world保护，commit仍0；不作为完成DLL。
- 独立spirv-val发现配对VS的OpSelect向量结果用了标量条件，驱动运行不等于SPIR-V合法；
  修为显式bool4。继续镜像world结束后的无灯FFP绘制，不允许新增claim；有灯绘制仍拒绝，
  最终frame/generation/epoch验证保持。下一轮必须重新验证实际dumped变体。

### 事务消费者 R6/R7：首次实际接管与强制回退通过

- A2BC2D98…B7495F /35,437,274，3-edge BelowNormal/-j2构建，PE32/i386、exact no-work。
- R6观察到automatic=2 /cubeFaces=12的真实receiver提交；实际配对FFP SPIR-V全部通过spirv-val。
  R7相同DLL强制fallback，完整cube就绪仍不提交automatic，保留原版颜色；两轮四截图、exit0、
  A570恢复/玩家B1FCC及地图不变/GPU事件和dump为0。截图可见局部照明变化，不把它冒充遮挡者视觉验证。
- 两盏预算曾被最早terrain draw选择的LanternPost占满。下一候选使用前一完整census仅作
  screen-center优先级提示；实际claim、原生值和生命周期仍在当前帧验证，绝不复用旧光照数据。
- 之前map源码含作者light创建，但实际author=0；故未证明共存。新增独立视觉fixture使用当前
  JASS wrapper创建合法0..1颜色作者灯，并单独删除原生遮挡者；旧map/失败证据保持。
  43项定向检查通过，下一轮遮挡者/作者共存验收待完成，不宣称稳定接受。

## 2026-09-14 — 原版模型灯实际字节到实例的冷链候选（未接照明消费者）

- 新增独立有界 producer：嵌套读取事务核实际成功 path/buffer/size，SHA-256与11份冻结模型版本相等后，
  二进制解析成功、完整文档读取成功才发元数据；继续验证parsed ObjectID/ordinal与模板/实例独占CGxuLight。
- 每次实例创建给独立generation；析构、map reset、槽disable、同址复用和容量上限都撤销资格。
  原生空间桶缓存命中时从保留的exact pointer+generation槽收集本帧值，不使用最近路径或颜色猜身份。
- 环境`DXVK_WAR3_NATIVE_MODEL_LIGHTS=1`暂仅启用该producer；所有原生灯调用原样透传，
  lightingAuthorized/shadowAuthorized仍0，不能把这一中间DLL交付为自动点阴影完成。
- 28项定向Python通过；core Win32断言及实际提取TorchHuman/brazierOmni两份资源通过；
  新JASS原生火把/火盆/遮挡者/删除fixture通过pjass与MPQ逐字回读，作者地图未变。
- BelowNormal/-j2构建首轮因新文件include相对路径错误停止，修正后续编译链接成功；exact DLL no-work。
  中间DLL 35,384,484 bytes / 4EED1BE8053378022A5BC027AECC82E1AA23B0FA6E7828625F53AE5CAB7952D9，PE32/i386。
  diff check exit0；这是离线结果，下一步独立隔离1440p实际producer链取证，未接受照明或阴影。

## 2026-09-14 — 发布基线核对与独立集成树建立

- 用户要求下一版v1.22，完整编号沿用公开规则1.22.00；明确排除未完成Water分支水体工作。
- 在线核公开v1.21.00为非草稿/非预发布，tag commit ae890542d766470d1703f5bea7f5b73636039733。
- 当前性能树不是release后继：共同祖先8f232cc，release-only/head-only提交221/1。
  保留已发布PCF/Stage11工作集/exact-owner/terminal drain等能力，禁止直接更名旧树DLL发布。
- 用户明确批准另建独立工作树：codex/v1.22-release-integration-20260914，初始clean并精确位于tag。
  原性能树412项dirty、玩家DLL、其它工作树和运行证据不更改。
- 新建下一版更新日志草案，记录同步点原理、历史40.40%帧时减少/67.78%吞吐增加的严格口径，
  异步截图、诊断/metadata候选、灯光未完成与发布阻断，以及Water排除项。
- IDA只读确认parser/read-fallback/template-clone入口和既有注释，未修改IDA或native字节。
- 状态：整合开始，未构建/部署/实机、未提交/打标签/发布，不是v1.22完成。

## 2026-09-14 — 首批移植：identity-only 查询（SOURCE / TARGETED_OFFLINE_PASS）

- 从旧性能树的一个明确优化移植到release基础：ShadowGeosetIdentityView、by-data标量查询，
  ResolveGeosetMetadata仅替换一个完整record查询。原map命中/空值/互斥锁/五身份字段不变。
- 发布版已有的ready-binding、alias覆盖、immutable snapshot与几何就绪策略原样保留。
  未连带移植整棵DataCollection探针、旧source头文件或其它消费者。
- 5/5新定向unittest通过，含整个caller文件对基线精确两处替换及关键旧函数逐字冻结；
  5份既有scalar-alias/direct-source/runtime-alias/final-revision/demand-fill静态脚本通过。
- CPU-only i386投影runnable独立编译/执行exit0，父进程BelowNormal、单编译进程。
  623465bytes/SHA A768812398AC1D4001A780023E727ED5EE45EEEDF1285800BE1AB44750288481，
  位于ignored AutoTest/artifacts/v122_identity_port_20260914；测试不创建图形设备。
- 该验证不是产品TU/DLL构建、不是新实机或FPS收益，尚未改变RELEASE/版本资源、提交或发布。
  点光接入、同步/截图移植及最终组合门仍待完成。Water和当前玩家现场均不触碰。

## 2026-09-14 — 新增发布范围与太阳 API 语义审计（ASSESSMENT / STATIC_ONLY）

- 用户确认其截图测试正常，记为该次玩家功能正向反馈；不推定完整按键/分辨率/长时矩阵，
  不转移到新集成 DLL，不据此关闭此前独立 GPU incident。
- 用户要求 Froxel High、局部体积雾、方向阴影 Guide 收口及全部 JASS API 校验。
  三项已有 v1.21 实现，本版完善质量/性能/控制链；Water 分支继续排除。
- 当前清点 109 公开函数、106 后端命令、92 GUI 入口；smoke 源码只提及 36 函数，
  并非实际覆盖率。协议 runnable 有 backend stub，不能替代实际效果验证。
- 11 条 outline/bloom/postfx/aa 包装被 implemented feature mask 排除、返回 UnsupportedFeature；
  保留既有协议/测试，不改成假成功。完整校验与补齐这些接口分别估算。
- 玩家明确反馈太阳启停无区别。源码确认 JASS sun.setEnabled 有分发，但固定功能 SetLight
  在关闭时保留原生灯；GUI“启用太阳光”与实际“覆盖太阳光”语义不一致，体积消费者又另有门。
  此为源码证据，未完成玩家地图复现，不宣称已证明全部运行根因或已修复。
- 8 份定向纯 Python/static 共 87/87 通过，不是所有 API 的实机或全量回归通过。
- 新增工作量初评与验收表，同步计划/待发布草案；本次新增范围 11–18 工程人日，
  全版暂估 15–25；若 11 条 unsupported 全补齐另估 3–6。前 1–2 人日复现后重估。
- 本轮无产品代码修改、无 Ninja/编译/部署/游戏，无版本资源/提交/发布；玩家现场保持不动。

## 2026-09-14 — 太阳 API 修复与体积执行取证（SOURCE / STATIC_PASS，运行待验）

- 用户正式批准推进 JASS 可用性、局部体积雾与 Froxel 验证。所有工作在独立集成树，Water 不合入。
- 太阳关闭路径仅清主方向光的 diffuse/specular RGB，不抹除 ambient/点光；自定义材质与正常
  CSM 强度遵守同一开关。公开 wire/ABI 不改，UI/作者说明同步澄清语义。
- 添加默认关闭的体积执行快照：requested/effective backend、阶段、同帧 epoch、局部雾人口与
  composite 提交。纠正旧准入前 active 日志，保留原有全部预算，不改 shader。
- 添加受既有 internal-test gate/主线程队列约束的 public native-carrier 调用及只读设置快照。
  该接口明确标注不是 JASS 字节码验收；失败 API 不伪装成功，返回独立 errorCode。
- 10 份定向 Python/static 共 101/101 通过。旧 Froxel 测试的误导性日志断言已改为 request 与
  实际提交分离，额外保留实际结果赋值和 composite 顺序断言；其它 28 条原 Froxel 用例不削弱。
- 新增 exact D3DLIGHT9 字段/有限强度 runnable 与单进程隔离测试脚本；本 checkpoint 构建进行中，
  不宣称 runtime 或局部雾/Froxel 验收。参见 research/2026-09-14-sun-api-direct-light-contract.md。

## 2026-09-14 — 首批真实 JASS / 局部雾 / Froxel（LIMITED_FUNCTIONAL_PASS，未发布）

- 新集成DLL修复太阳启停的原生直射/CSM语义，隔离画面确认有区别，环境/自发光保留。
- 真实JASS fixture抓到独立缺陷：1.27a LoadInteger/LoadReal元数据签名尾分号缺失，使typed
  安装整体拒绝。仅修复两条精确签名、继续NUL-inclusive匹配；未放宽安装/回退合同。
- 最终D888587E…E2D4、35,258,436 bytes、PE32/i386；fresh构建曾缺MinHook archive，
  从pinned源码补齐后完成，typed修正为2-edge，exact DLL no-work。102/102定向static、
  4/4 Win32 runnable、pjass/MPQ脚本逐字回读、py_compile/diff check通过。
- 四次隔离2560×1440功能事务：首轮Legacy16超安全预算失败、原生8步兼容门通过、
  首轮VM typed握手失败、修复后VM10阶段83断言全部通过。失败证据不覆盖、不追认。
- 最终fixture直接涉及49个公开函数（含Bloom明确error18），非109全验收；typed与string
  fallback数值、太阳、三形局部雾、High/Medium、低视角/雾内、8槽/销毁/错误均有限定证据。
  未修改shader或预算，不宣称Guide动态画质、前台FPS、长期、原版模型点光已完成。
- 各轮均恢复test安装A570、玩家B1FCC原样、原map不变、零相关进程/GPU事件/新增dump；
  AutoTest短地图路径已按精确5691→11376条件恢复，fixture与日志留在ignored目录。
- 更新待发布草案与研究/作者用法；根CHANGELOG不写入候选，Water排除、不提交/发布。
  详见 [本轮功能证据](2026-09-14-v122-jass-local-fog-functional-checkpoint.md)。

## 2026-09-14 — 截图/同步移植、ReShade移除与退出取证（CANDIDATE，未交付）

- 用户实际War3目录仍是A570旧DLL，另一Warcraft III目录才是B1FCC试用DLL；未将约90FPS
  武断归因于ReShade。51个本地ReShade配置/着色器/addon文件按SHA可恢复移动到
  E:/Work/WarVK-Backups/war3-reshade-20260914；全局ReShade/注册表未卸载，Apps配置写权限
  被拒绝后保持原字节。隔离新进程模块清单已证实不加载ReShade。
- 精确native请求消除与三槽异步截图移植到release-base独立模块；缺省启用候选门，显式0
  关闭，录制无关、owner/完整代码身份/真实GPU完成/失败回退不放宽。不自动弹调试console。
- R2/R5有效功能观察：recording-off普通sync请求全部被精确消除；五份1440p TGA（单张、
  同帧三槽、复用）解码/alpha/同帧像素一致；不是玩家按键、前台FPS或点光验收。
- 修正退出验证口径：R1–R3仅WM_CLOSE后仍渲染，不证明卸载死锁；调试读取拒绝，未伪造堆栈。
  R4 EndGame后C0000409，旧receipt.ok=true永久不能作为正常退出证据；新增exitCode=0硬门。
- 控制管道joinable全局thread的进程析构修正后，42C候选R5 recording-off真实退出0、无force。
  R6 recording-on抓到性能统计TLS在进程析构后被读取的C0000005；精确dump/RVA/EBP链闭合，
  该组合判为未通过。新monitor整体进程析构保护及D3D9提前detach门已写，待新构建/实机。
- 各轮恢复A570、B1FCC不变、零残留，未改Water。旧失败dump/报告/结果不删除、不覆盖。
- 原版模型点光仍未接入增强消费者；新增IDA精确E3410→E5690指针/36字节值拷贝与缓存证据，
  三处注释回写并保存。白名单、原生贡献替代、同帧提交回退及实际点阴影视觉门仍阻断完整DLL。
- 详见 research/2026-09-14-v122-native-capture-integration.md 与 native-model-light-ffp-handoff。
  不更新稳定CHANGELOG、不提交/发布，不把中间DLL冒充用户要求的完整候选。

### 同日收口：5B70 截图/退出定向组合通过，原版点光仍阻断

- 5B705B6B97577C66D8AC96621FAFA1DFFAE216C7CB876A82BB023E32E2684116，
  35,336,455 bytes，PE32/i386；最后3-edge构建BelowNormal/-j2，exact DLL no-work。
- R7录制on、R8录制off各一次隔离2560×1440：缺省同步门实际生效，观察区间分别
  2553/2553、2663/2663普通调用跳过空读回；每轮五张TGA通过文件/像素/alpha/槽复用检查。
  两轮均ReShade未加载、无自动console、EndGame(false)→WM_CLOSE退出码0，无强制结束。
  分别恢复A570、玩家B1FCC/地图不变，GPU事件/新增dump/相关进程均0。
- 121/121定向Python/static、三份Win32 CPU runnable exit0（含10万次队列handoff）、
  5份py_compile通过；不称全项目回归、GPU长测或玩家前台接受。
- R6真实fatal dump保持原路径/原字节，60,128,518 bytes，
  97B82E267BCA99E242C4162542C2FF3582928A30A6143C1B8B099D4AA1401274。
  旧R4错误的ok布尔与R6失败永久保留，不因后续成功追认。
- 原版模型灯的正式consumer/原生贡献替代/白名单实际点阴影没有完成；5B70只属内部
  中间产物，不交付为用户明确要求的完整候选，不覆盖日常DLL，不合入Water。

## 2026-09-14 — 原版点光可见性判退与定向修正（未交付）

- R8/R9 已有实际 TorchHuman/LanternPost 六面点阴影与作者灯共存，但肉眼照明偏暗；
  用户“只有火焰，没有照明和阴影”的反馈不能被 producer/receiver 提交日志否定。
- R10 阴影可见度、R11 原生回退、R12 保留接管但阴影强度0对照：照明重建本身也在丢亮度。
  旧接收器以去掉灯光后的已着色画面充当 albedo，是不成立的替代方式。
- 原版 TorchHuman 几何另证实灯点位于不透明灯杯内部；96条地面测试射线86条受自身遮挡。
  地形Z=0、灯Z=91.1175，排除“埋到地面下”的猜测。几何证据不冒充runtime owner证明。
- 当前修订仅接管一盏原生灯：双颜色保留原版光照响应，按 C0-(C0-C1)*(1-V)只调制可见性；
  其余原生灯与作者灯保留。自遮挡排除只能来自同帧精确模型owner，且只影响自身灯；
  不扩大bias、不凭距离删除Caster。45项定向静态通过，构建/修订后视觉门待闭合。
- R8–R12 均隔离2560×1440、正常退出0、恢复A570，玩家B1FCC/原map未改，无GPU事件/新dump。
  旧判退截图和DLL原样保留。仍不能提供为完成版、更新稳定日志或发布v1.22。

### 同日 R13：修订后黑块仍判退，不把事务成功当视觉验收

- R13 的新 receiver binding 14 未进入手工 descriptor layout，出现黑块；已补第15槽及
  双端数量测试。2D9720…050A3D / 35,450,347 保持失败证据，runner.ok不代表视觉通过。
- 原 fallback owner 匹配为0；在实际三类 caster 发布点附带非回绕模型代际，匹配同代灯具，
  不用共享renderablePart猜实例。自遮挡策略仅限已审计98D1火把，不排除带灯墙体。
- 46定向静态/代数与52既有点阴影用例通过，实际scalarBlockLayout条件下内嵌SPIR-V验证通过。
  首次无该flag的spirv-val拒绝既有scalar布局，不伪报首次通过；后续owner字段编译错误已修正，
  等待新构建和隔离视觉门。玩家DLL、稳定日志和Water内容不变。

## 2026-09-18 WarVK palette-object 观察链：写方修正②（预算预检提前到 Insert 之前）

- `war3_palette_object_evidence.h` 的 `NoteFirstSight` 原先**先建条目、后判预算**：预算拒发时
  条目已建（`firstSightInserted++`）却零阶段事件，这些条目最终只剩终态，而终态对 `!sawFirstSight`
  的条目把 stage 回填为 `Drawn`（`:258-259`）⇒ 读方看到 [Drawn] 的**假链**（实机实测 75 条）。
- 现在顺序为：重复观测去重 → `CanEmit` 预算预检 → `Insert`。副作用：表里有条目 ⇒ 链首一定发过
  成为不变量；代价是预算紧张时该对象**完全不可见**（连终态也没有），丢失由 `AccountDrop` 显式
  计入 `droppedPerFrame`/`droppedPerSession`，不是静默丢失。
- 验证：`war3_palette_object_evidence_test` 24 passed / 0 failed；meson 85 Ok / 0 Fail；
  wire roundtrip CHECKS=1137 FAILURES=0；静态 259 scripts / 0 failed；`ninja -C build32 -n` no-work。
- **尚未做实机 A/B**（修正① 曾做，修正② 未做）⇒ **不得声称实机行为已验证**。未部署、未提交。
- 候选 DLL 36,289,280 B，SHA-256 `5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D`。

### 修正② 实机 A/B 验证（同日补充）

- 隔离桌面单次采集（120 s，`CONTRACT=1`、`INPUTS=0`）：`cpu-31168-75810535678-1.json`。
- **每一项均符合设计预期**：无链首的对象键 **43 → 0**；假链签名（`terminal!=NoTerminal & stage=Drawn`
  `& source=NoSource`）**32 → 0**；解析器链数 **107 → 64**（== `firstSightInserted`）；
  `stageObservationComplete` **32/107 → 64/64**。
- 已知代价确认：总 `emitted` 3691 → 3648（43 个对象完全不可见，不再消耗配额）；
  `emitted − terminal = 3584 = kNormalBudget` 仍精确打满 ⇒ 配额仍是约束。
- 这是**隔离桌面的功能对照**，**不是前台性能数据**（硬约束）。

## 2026-09-18 — P0-1 会话接纳边界修复（未提交 / 未部署 / 未晋升稳定）

- **缺陷（外部审核 Astra 实测 + 我独立复核）**：`war3_frame_evidence.cpp` arm 分支原顺序为
  `setPreFreezeHook` → `s->active.store(release)` → `ArmPaletteObjectEvidence` ⇒ **记录器初始化晚于 active 发布**。
  实测后果：初始化前到达的事件被未初始化记录器**接纳**，随后 `Reset` 清掉计数且**不落丢失计数** ⇒ 无声丢失。
  第二形态：旧会话在途对象键进入新会话 ⇒ 线里出现「顶层 session=2 / 对象键 sessionGeneration=1」的串会话事件。
- **我的旧锁漏了一半**：`test_palette_object_arm_order_static.py` 原先只锁 `hook<active` 与 `hook<arm`，
  **恰好漏掉**「先初始化记录器再发布」⇒ 把部分满足固化成了一种「规范」。已补 `check(_arm < _active, ...)`。
- **修复**：① `ArmPaletteObjectEvidence` 移到 `active.store` **之前**；
  ② 记录器新增 `AdmitsSession()` / `RejectIfStaleSession()`，6 个入口在 `m_windowClosed` 早退后校验键的会话代际，
  被拒计入**内部**计数 `droppedStaleSession`（不写入 wire，避免触碰版本表）；
  ③ 两处测试夹具现实化（`evidence_test` 的 `MakeKey` 改用 `Recorder().sessionGeneration()`；
  `cost_test` 的 `MakeKey` 由与 `Reset` 实参不一致的硬编码会话号改为 `0u`，该测试只测成本/预算）。
- **取舍**：曾试「会话代际为 0 ⇒ 一律拒绝」，它破坏 `default/unconfigured 仍推进状态机` 的既有契约（Case 1 具名失败）⇒ 已删除；
  记录器**不替调用方做 arm 门**，那个窗口由控制路径顺序关闭。已知局限：键未携带会话代际（0）时不受保护，
  但生产采集点无条件写入该字段。
- **验证（全部检查退出码）**：`ninja -C build32 -j4` exit 0、no-work、静态 **263/0**、`meson` **Ok 85 / Fail 0**、
  宿主机 **35 passed / 0 failed**（新增 Case 36）、wire 驱动 **CHECKS=1160 FAILURES=0**。
- **可失败性探针（两条）**：接纳边界失效 ⇒ Case 36 两条**具名失败**；顺序退回旧形态 ⇒ 静态锁**具名失败**。均已还原复跑。
- **不声称**：实机/前台未验证（本轮零实机采集）；disarm 路径仅初步查看、未做等价修复与探针。
  下一步为 P0-2（`MarkStage`/`maxStage`）、P0-3（`ObjectGone`）、P0-4（仅链首观察链 → ObservationClosed）。

## 2026-09-18 — P0-2 终态阶段回填修复（未提交 / 未部署 / 未晋升稳定）

- **缺陷（Astra 实测 + 我独立复核并精确复现）**：`MarkStage()` 只在「尝试号未知」分支更新 `maxStage`，
  另两条分支（新尝试 / 同尝试）都只更新 `attemptStage`；而 `CloseWindow` 的终态阶段回填读的是 `maxStage`。
  ⇒ 五点携带**同一个已知**尝试号（或携带**不同**号——生产上就是各点帧号）时 `maxStage` 永不提升，
  已走到 Drawn 的链被回填成 **FirstSight**（实测 `stage=5`，与 Astra 报的 `stage=FirstSight` 一致）。
- **修复**：`MarkStage` 开头与尝试号**无关**地维护终身最高秩 `if (s > e->maxStage) e->maxStage = s;`。
  字段职责：`maxStage` = 实际达到的最高语义秩（回填用）；`attemptStage` = 本次尝试最高秩（仅回退判定用）。
- **验证（检查退出码）**：ninja exit 0、no-work、静态 263/0、meson Ok 85 / Fail 0、宿主机 **36 passed / 0 failed**（新增 Case 37）、
  wire 驱动 CHECKS=1160 FAILURES=0。Case 35 输出未变（未知尝试号路径未被扰动）。
- **探针**：撤掉提升语句 ⇒ Case 37 **具名失败**且 `terminalStage=5`（复现 Astra 反例）。已还原复跑。
- **不声称**：判序/尝试关联（P0-5）**未修**；实机未验证。

## 2026-09-18 — P0-3 ObjectGone 双链结算 + 不伪造阶段；独立复审 R3/R4 修正（未提交 / 未部署 / 未晋升稳定）

- **缺陷（Astra 实测 + 我复核）**：`NoteObjectGone` 硬写 `record.stage = Rejected`（观察链从未被拒绝 ⇒ 伪造，且与读方规则冲突）；
  且只用 `FindOwner()` 处理**一条**链 ⇒ 另一条滞留、最终以 WindowExpired 结算。
- **修复**：抽取单链操作 `SettleObjectGone(Entry*)`，对 RejectionRecovery 与 Observation **各结算一次**；
  阶段改为 `sawFirstSight ? StageOfRank(maxStage) : Rejected`（拒绝链冻结语义不动）。
  语言规则：helper 的参数类型要求 `Entry` 先声明 ⇒ 定义必须放在 `struct Entry` **之后**。
- **独立复审 R4（已修）**：ObjectGone 终态 emit 内重入 `CloseWindow`（生产可达）会把 `m_watchCount` 无条件减到
  **0xFFFFFFFF**（读方要求 ≤1024 ⇒ 整份导出被拒）；且第二条链的 ObjectGone **静默丢失**。
  修复：递减加 `if (m_watchCount > 0u)` 护栏；`NoteObjectGone` 先记录 `hadObservation`，
  若 emit 导致窗口关闭而第二条链原存在 ⇒ `objectGoneLostToReentrancy++`（**具名 fail-visible**，内部计数不上 wire）。
- **独立复审 R3（已更正注释）**：未知尝试号分支原写「行为不变」现在不成立 —— P0-2 的提升让未知号事件与
  「混合已知尝试秩的终身最高秩」比较（混流下 Recovered→Unclosed，复审 S4）。当前唯一未知号站点是 D 点（最高秩）⇒ 不可达；
  新增未知号低秩站点会永久失去 Recovered。彻底修法属 P0-5。
- **复审推翻我的三处说法**：① 生产只有 3 个采集点传 attemptSerial（D 点省略 ⇒ 未知分支），我此前说「各点都传帧号」是错的；
  ② 因此 Astra 的 stage=FirstSight 反例**无法由当前生产接线产生**（复审 S3），只能标注为历史 wiring 实测；
  ③ wire 往返门禁与本改动**正交**（夹具恒用未知哨兵），不得作为 attemptSerial 证据。
- **复审指出的盲区（未修，下一步）**：`test_palette_object_attempt_serial_caller_static.py` 只扫 2 文件、断言 ==3，
  全树活调用点实为 4 ⇒ 结构上无法发现 D 点未接线；palette 测试目标无 depfile，改 .h 后可能跑到陈旧二进制
  （复审实测到 Fail 1）⇒ 常设配方：**门禁前先删 .obj 强制重编并打印 hash/mtime**。
- **验证（检查退出码 + 强制重编）**：静态 263/0、meson Ok 85 / Fail 0、宿主机 **38 passed / 0 failed**、wire 1160/0。
- **不声称**：实机未验证；**Case 40（重入护栏的行为用例）尚未编写**（明确遗留）；D 点未接线属 P0-5。

## 2026-09-18 — P0-4 观察链结算修正（仅有链首/只到入队 ⇒ ObservationClosed）（未提交 / 未部署 / 未晋升稳定）

- **缺陷**：结算顺序把 `hitCount == 0 && !sawServed` 排在观察链之前 ⇒ 只走到链首/入队的**观察链**
  被结算成 `WindowExpired`（借用**拒绝链**的含义，而该对象从未被拒绝）。
  我当初这么写是为了保住旧 Case 24 的断言 ⇒ **让旧错误决定新语义**（Astra 指出这是错的）。
  生产 `d3d9_device.cpp:23539-23548` **故意不发 Served** ⇒ `FirstSight→Enqueued→Close` 这类真实序列此前被误判。
- **修复**：观察链分支前移到 `WindowExpired` 之前；`WindowExpired` 收窄为**只对拒绝恢复链**成立。
- **有意更新的旧期望**：Case 24 的 `a never-rejected object closes as WindowExpired` 改为要求 `ObservationClosed`
  （修契约、非放宽；断言消息写明理由与日期）。新期望更强：不得冒充 Recovered，也不得冒充 WindowExpired。
- **新增 Case 40**（3 条，含拒绝链对照）：实测 `headOnly=7 enqueuedOnly=7 rejectNoCatch=2`。
- **探针**：退回旧顺序（观察链分支加 `&& e.sawServed`）⇒ Case 40 ①② **具名失败**（都变 2 = WindowExpired）、③ 仍通过。已还原。
- **验证**：静态 263/0、meson Ok 85 / Fail 0、宿主机 **39 passed / 0 failed**、wire 126/0 PASS；evidence.h md5 `C7707C95C3B9441BB74703723FA552F9`。
- **不声称**：实机未验证；Case 35 双链输出未变（属 P0-6）；读方对新语义的重新核对已交独立验证子线程。

## 2026-09-18 — P0-5 部分：D 点接线 + 门禁盲区修复；P0-3 复审 C1/C3 修正（未提交 / 未部署 / 未晋升稳定）

- **D 点（第 4 个生产采集点）此前漏接 attemptSerial**：`d3d9_war3_shadow.cpp:5241-5246` 省略第 5 实参
  ⇒ 取默认未知哨兵 ⇒ 「机制已实现、生产线未接线」。修复：按值携带 `draw.shadowRecordFrameSerial`
  （该值**本来就在传**，是第 2 实参；与 Served 点同源：`d3d9_device.cpp:21527 == packet.renderable.frameSerial`）
  ⇒ **不引入任何新数据流**。
- **门禁盲区修复**：`test_palette_object_attempt_serial_caller_static.py` 原只读 2 个文件并断言 live_calls==3
  （全树实为 4，漏的正是 D 点），且用字符串包含判断接线 ⇒ 结构上发现不了「省略第 5 实参」。
  重写为**全树扫描** + **按括号配平数顶层实参**（必须正好 5 个）+ 第 5 实参不得是哨兵。
  实测输出：4 live call sites, all passing an explicit serial；**探针**：撤掉 D 点第 5 参 ⇒ 具名失败（got 4）。
- **P0-3 复审 C1（整份导出拒绝，已修）**：ObjectGone 终态的 emit 内重入结算（生产可达）会让**同一条链拿到两个终态**，
  而读方硬要求『一对象至多一个终态事件』⇒ 整份导出被拒。修复：**在 EmitUnchecked 之前先认领条目**
  （先 `used=false`/`tombstone=true`/减计数，再发射，发射后不再读 e）。新增 Case 41；
  **探针**：把认领移回 emit 之后 ⇒ `terminals=2` 且 Case 41 具名失败（与复审独立复现一致）。
- **P0-3 复审 C3（判据错误，已修）**：`NoteObjectGone` 不再用 `m_windowClosed` 判断第二条链是否可结算
  （`ResetForSessionTransition` 会清表却不置该标志），改为**重新 FindChain 确认它仍可结算**；否则计入具名丢失。
- **P0-4 复审发现的三处「说反话」注释（已修）**：`evidence.h` 枚举注释仍写「WindowExpired 先于本终态判定」
  （与实现相反）；`WindowExpired` 注释强于代码（它只保证 `chainType==RejectionRecovery`，`NoteReject(NotChecked)` 同样落此桶）；
  Case 24 头部注释仍写「首见链终态 = WindowExpired」而断言已改为 ObservationClosed（自相矛盾）。
- **门禁跟随重构更新**：`test_semantic_build_thread_gate_static.py` 的 F3 块改为断言新结构（两条链各取一次、
  第二条结算前重新确认、结算两条链），并**新增 C1 契约锁**（认领必须在 EmitUnchecked 之前）。
- **复审提出、尚未处理**：(a) 读方**没有** `(Observation, WindowExpired)` 规则 ⇒ 旧标签回来时读方不会拒绝；
  建议补 `observationChainMustNotUseWindowExpired`。(b) 反例 A：拒绝事件被每帧预算吞掉时链型静默退化、
  新终态把这个退化说成「观察已结算」⇒ 可观测缺口。(c) 多份文档仍写旧语义（含观察链契约文档与真机解读文档）。
- **验证（强制重编 + 逐脚本计失败数 + 查退出码）**：静态 263/0、meson Ok 85 / Fail 0、宿主机 **40 passed / 0 failed**、
  wire 126/0 PASS。hash（**注释修正后**的最终值）：evidence.h `7B4FC69AAE6D5F825156A335D996FCA0`、d3d9_war3_shadow.cpp `EB5681C8F2758BDD3598A5C0E9AB4581`；站点仍 = 基线 `F275545B…`（未部署）。
- **不声称**：实机未验证；P0-5 的「记录级关联标识」本身尚未设计（本轮只接线 + 修门禁 + 修重入）；
  P0-6 双链 C1 未做；反例 A 与读方规则缺口已具名登记但**未修**。

## 2026-09-18 — P0-5（复审 C2）：发射路径不再无声丢弃（未提交 / 未部署 / 未晋升稳定）

- **缺陷（两位独立验证者先后指出）**：`war3_palette_object_evidence_sink.cpp` 的 `EmitPaletteObjectEvent` 有**两条
  无计数的早退** —— `session == 0u`（冻结/重入期间 `active` 被清）与 `EncodePaletteObjectEvent` 失败。
  真实环路径实测：重入场景下 1 条嵌套 emit **无计数地被丢弃**，读方据此 `emitted != exported` /
  `terminalEmitted != exportedTerminals` ⇒ **整份导出不覆盖**。对比之下 `Record(...) == 0u` 那一路**有**计数（G4）。
- **修复**：新增两个**具名内部计数** `g_paletteObjectDroppedNoSession` / `g_paletteObjectEncodeFailed`
  （与既有诊断原子同类：原子、relaxed、**不上 wire**）+ 两个访问器，两条早退各自计数。
- **两个门禁原本锁住了错误形状**（都已按修正确后的契约更新，且**加强**）：
  · `test_semantic_build_thread_gate_static.py` 断言 `if (session == 0u) return;` —— 锁的正是**无声 return**；
    现在要求「fail-closed **且**计数」，并对编码失败那一路补了同样的断言。
  · `test_independent_review_sep18_fixes_static.py` 的计数锁 `== 5` ⇒ 更新为 7，并**列出全部七个计数器名字**
    （数字锁是故意的：再加计数器必须回到这里说明用途）。
- **验证（强制重编 + 逐脚本计失败 + 不用管道取退出码）**：静态 **263/0**、meson **Ok 85 / Fail 0**、
  宿主机 **40 passed / 0 failed**、wire 原生 **126/0 PASS**、no-work；sink md5 `AE32948CD10C5B4FAA0287D65E6F7F66`；站点 = 基线（未部署）。
- **不声称**：这两个计数**不在 wire、也不在读方 COUNTER_FIELDS** ⇒ 本次修复让丢弃**在进程内可归因**，
  但**没有**让导出层面自证一致（读方仍无法看到它们）。这是与 Astra 决策（不为内部计数单升 v5）一致的取舍，已如实登记。
- **另一条同类缺口（复审 C2 的真实环路径部分）**：冻结尾环里 0 条终态 + `ringLosses=2` 的计数/导出一致性破裂，
  以及 `CloseWindow` 折叠导致的终态缺失，**尚未修**（已具名登记）。

## 2026-09-18 — P0-4 写方/读方同步：新增 observationChainMustNotUseWindowExpired（未提交 / 未部署 / 未晋升稳定）

- **缺口**：独立复审指出读方**没有** `(Observation, WindowExpired)` 规则 ⇒ 写方一旦把结算顺序改回旧形状
  （把 `hitCount==0 && !sawServed` 排在观察链之前），导出里出现「观察链冒充拒绝链的窗口过期」而读方无声接受。
- **修复**：`analyze_palette_object_evidence.py` 的 `judge_chain` 新增 `observationChainMustNotUseWindowExpired`，
  **按版本门控**（`version >= PALETTE_OBJECT_CHAIN_TYPED_VERSION`；v1/v2/v3 无链型载体、解码恒给 RejectionRecovery）
  以避免「版本无关用法」。
- **新增载荷用例**（3 条断言）：v4 观察链 + WindowExpired **必须报出**该规则；v4 同一个观察链以 ObservationClosed 结算
  **不得**被误报；v3 同一数字形状（无链型载体）**不得**被误报。
- **探针**：把 `issues.append("observationChainMustNotUseWindowExpired")` 改成 `pass` ⇒ 该用例**具名失败**；已还原。
- ⚠️ **实测澄清（同时修正我自己的错误断言与复审的一处说法）**：读方的**链型规则**（含既有三条：
  observationChainMustNotUseRecovered / rejectionRecoveryChainMustNotUseObservationClosed /
  observationChainMustNotCarryRejectedStage、以及 rejectionRecoveryChainMustNotCarryFirstSightStage）
  是记入链的 **`orderIssues`（顾问性）**，`analyze()` **不会**因此抛错；真正致命的只有 `require(...)`。
  ⇒ 「旧标签回来」在**导出结果里可见**（可被消费方/测试断言），但**不会让导出被拒**。
  我第一版用例断言 `assertRaisesRegex` 是**错的**（探针实测规则有报出但不抛错），已改为断言 orderIssues。
  「这些链型矛盾是否应当**致命**」是**合同问题**，我没有自行决定，列为待裁定项。
- **验证（逐脚本计失败 + 无管道取退出码）**：静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **40 passed / 0 failed**、
  wire 原生 **126/0 PASS**；reader md5 `C6B7D357B3CEF50E80D8694882314235`；站点 = 基线（未部署）。

## 2026-09-18 — P0-6 实施 Astra 裁定 C1（明确关联后的逐链记录）—— 未提交 / 未部署 / 未晋升稳定

### 改动（`war3_palette_object_evidence.h`）
- S/E/D 三个入口的**每链操作**各自抽成 `AdvanceServedOnChain` / `AdvanceEnqueuedOnChain` / `AdvanceDrawnOnChain`
  （原内联体逐字搬入，含 D6 载荷去重、`CanEmit(false)` 预检、hitCount 只在真的发出时递增、G3 的 deltaFrames 契约）。
- 入口改为对**两条链各自**记一份：关联依据 = `FindChain` 的**完整键相等**（八元组）+ **链型相等** + 同窗口 ——
  这是记录器真正拥有的显式证据，**不是**「同键无条件广播」（Astra 明确否决后者）。
- 顺序**显式固定**为先 RejectionRecovery、后 Observation（不再隐含「观察链先拿走最后一个槽」）；
  第一条的 emit 触发嵌套结算/冻结时，第二条链的事实**具名计入** `pathObservationSkippedAfterReentrantClose`（内部计数）。
- **实测结果**：`[OWNERSHIP-27] servedObs=0 servedRej=1 enqObs=1 enqRej=1 drawObs=2 drawRej=2`（两链各记一份）；
  `[OWN-35-dualchain] recovered=1 unclosed=0 windowExpired=0 obsClosed=1` —— 拒绝恢复链**现在能认证 Recovered**，
  且不再谎称「窗口内从未被接住」（这正是裁定「Recovered 必须有真实拒绝事实」的另一面）。
- 两处**旧所有权断言按裁定有意更新**（Case 27 的 `enqRej==0`/`drawRej<=1`、Case 35 的期望值），
  并在原地登记「旧值锁的是选项 A 的所有权规则」+ C1 后的第三组实测数。
- 门禁跟随重构：新增 `effective_body()`（入口体 + 它委派的 helper），§11d 与三处 `_signature` 循环改走它；
  关键教训：**先前的写法用 helper 的「定义签名」去匹配调用方体内** ⇒ 找不到，门禁假红。

### 本轮我自己的严重失误（如实登记）
- 编辑门禁时锚点选错：`for signature, label in (` 在文件里出现两次，我用「从 iA 起找消息行」的办法
  命中到 980 行的**生命周期断言循环**，导致**误删约 226 行门禁断言**（且该文件 untracked ⇒ 无 git 基线）。
- **恢复**：从独立验证者的副本 `_audit_verify_p04/run_orig/AutoTest/…`（2323 行）整份恢复；
  先核对「前缀 979 行 + 尾 900 行逐行相同」且 F3/C1/C2/C3 四个锁都在 ⇒ 确认恢复只丢本轮那一处编辑，再重做。
- 另有三次自伤：Case 35 替换漏一个 `;`、`dualize` 漏掉原函数收尾 `}`、批量替换把 `effective_body` 自身改成递归。
  四次都由编译器/门禁**立刻**暴露并修复 —— 这印证「每次编辑后必须真的编译+跑门禁」，不能只靠肉眼。
- 教训（已固化为纪律）：**锚点必须空间约束**（先定位到目标函数/消息行，再在近邻范围找），
  且**替换后必须复核结构**（大括号配平、行数、语法）再写盘。

### 验证
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **40 passed / 0 failed**、wire 原生 **126/0 PASS**。
- hash：evidence.h `B336E58B3D125660A739CAB79C2B66F5`、测试 `33240AE19EBCF2F8F70BD832EA5641CA`；站点 = 基线（未部署）。
- **仍未做**：E/D 的**预算耗尽**顺序行为的专门载荷用例（一条事实、两条链、只剩一个槽）、
  读方 `recoveredWithoutRejectFact`（复审发现 RR×Recovered 携带 NotChecked/Unknown 仍被认证）、
  内部计数不上 wire、反例 A/C2 的冻结尾环终态缺失、Observation×{TableFull,EventLost,Unclosed} 无规则。

## 2026-09-18 — P0-6 续：读方 Recovered 谓词对齐写方 + 两类静默丢弃改为具名计数

### ① 读方 `recoveredWithoutRejectFact`（复审 1266d8db 的独立发现；正对目标不变量「Recovered 必须有真实拒绝事实」）
- **缺口**：写方 `closedChain` 要求 `hasRejectFact = (firstReason != NotChecked)`（evidence.h:309-311），
  含义是「Recovered = **拒绝之后**被接住」；而读方解码了 `rejectReason`（:456）却**从不使用** ⇒
  一条把拒绝理由伪造成 `NotChecked`/`Unknown` 的链仍被**认证为 Recovered 且出口码 0**（复审实测）。
  读方 docstring 自称「mirrors the recorder's own Recovered predicate」但列的谓词里**没有** hasRejectFact ⇒ 该声称已过期。
- **修复**：`judge_chain` 新增 refusal `recoveredWithoutRejectFact`（与 `drawSourceMissing` 等同级，不新增致命判据），
  并修正过期 docstring。真实写方产物中 Recovered 必带真实拒绝事实 ⇒ **不误报**。
- 载荷用例 `test_recovered_requires_a_real_reject_fact`（NotChecked/Unknown 必须拒绝认证且不进 `recovered`；R1 不得误报）。
  **探针**：撤掉该 refusal ⇒ 用例**具名失败**（2 个子用例）；已还原。

### ② 两类静默丢弃改为具名内部计数（复审 41e85203 的丢弃路径清单）
- (a) **窗口关闭后到达的采集调用**：6 个入口各一处早退，此前**无计数** ⇒ 新增 `droppedWindowClosedEntry`。
  ⚠️ `CloseWindow` 自己的 `if (m_windowClosed) return;` 是**幂等返回**（不是丢弃），未计。
- (b) **路径事实无处可记**：S/E/D 在**两条链都不存在**时此前**静默 return** ⇒ 新增 `droppedNoChainForPathFact`。
  复审推断可达：FirstSight 用 `runtimeModelPtr=nullptr` 造键，而 S/D 用 `draw.shadowRuntimeModelPtr` 造键，
  `SameKey` 要求八字段全等 ⇒ 键不等时事件会无声消失。
- 新增宿主 Case 42（两条断言 + 打印 `[P0-42] noChain=1 windowClosed=2`）；
  **探针**：撤掉 no-chain 计数 ⇒ `noChain=0` 且用例**具名失败**；已还原。
- 两个计数仍是**内部计数**（不上 wire、读方看不到）—— 与 Astra 的裁定一致，但**导出层面的自证一致仍未解决**，已登记。

### 验证
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **41 passed / 0 failed**、wire 原生 **126/0 PASS**、no-work。
- hash（最终）：evidence.h `ECEC039F30456C18726C5A35C76FE6CF`、reader `9E20F3C49C7F910F473243C0996E2FA7`；站点 = 基线（未部署）。
- **仍未做（已具名）**：C3 修正**零覆盖**（复审实测改回旧形状仍 40/0）；`encodeFailed` 是**可证死代码**；
  计数锁 `==7` 是脆弱文本锁（应改按名锁）；`objectGoneLostToReentrancy` 等内部计数无导出出口；
  E/D 的**预算耗尽**顺序载荷用例；v3 降级绕过（生产不可达）；我的版本门控**冗余**（删掉后 107 全过）。
- **门禁随代码加强**：`_WINDOW_CLOSED_GUARD`（裸 `if (m_windowClosed) return;`）只保留给 `CloseWindow` 的**幂等**返回；
  五个采集入口改用新的 `_WINDOW_CLOSED_GUARD_COUNTED`（要求 **具名计损** 后再返回）。
  该加强的载荷性由它自身证明：改动前门禁正是以 `('NoteReject', '五个 Note* 入口…')` **具名失败**。

## 2026-09-18 — P0-6 修正 C1-1：重入关窗时第二条链的路径事实不再静默丢失（复审 cfb2a5b6 的实质发现）

- **缺陷（独立复审实测）**：我在 C1 里写的 `if (observationChain == nullptr) return;` + `if (m_windowClosed) {...}`
  两道护栏**都无法捕获真实重入**：`CloseWindow` 在置 `m_windowClosed = true` **之前**会清掉每个 used 条目
  并阻塞后续 Insert ⇒ 「窗口已关且 FindChain 仍非空」不可能；而观察链指针是在**第一次 advance 之后**才取的，
  嵌套 CloseWindow 已把条目清掉 ⇒ 走 `nullptr` 裸 return，**事实静默消失、`pathObservationSkippedAfterReentrantClose` 恒为 0**。
  该路径**生产可达**：`war3_frame_evidence.cpp` 注册的环预冻结钩子 → sink → `ClosePaletteObjectWindow` → `CloseWindow`。
- **修复**：三个入口统一为 —— **两条链指针都在第一次 advance 之前取好**；第一次 advance 之后**重新 FindChain**
  确认观察链**仍可结算**（与 `NoteObjectGone` 的 P0-3/C3 判据同一写法），被嵌套结算吃掉时**具名计损**。
  同时把「两条链都不存在」的具名计损合并进同一处（不再单独前置扫描）。
- **载荷用例 Case 44**（照复审 `REENTRANT_REJ` 形状）：两链 + 在**拒绝链**的 ServedCandidate emit 内同线程重入 CloseWindow；
  断言 `servedRej==1 && servedObs==0 && pathObservationSkippedAfterReentrantClose==1`。实测 `[P0-44] reentered=1 servedRej=1 servedObs=0 lostPathFact=1`。
  **探针**：把三处计损自增改成 `(void)0` ⇒ `lostPathFact=0` 且用例**具名失败**；已还原。
  （注：另一次试图还原「旧顺序」的探针因拼接破坏结构而编译失败 ⇒ 我**不把它当作证据**；旧顺序「静默丢失」这一半由复审的独立运行时复现支撑。）
- **收紧 Case 35 的第二条断言**（复审的载荷性判定：旧断言只描述终态桶 ⇒ 对「路由改成 Find(key)」的回归仍会通过）：
  现同时断言观察链上真的存在 **live 的 Served/Enqueued/Drawn** 事实。
- 验证：静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **43 passed / 0 failed**、wire 原生 **126/0 PASS**。
  hash：evidence.h `F96EFCF1212BD2594C2E47F53E38EE46`、测试 `E2F20A86AE1168BDB04FC8FDBFE0619E`；站点 = 基线（未部署）。
- **仍未做**：E/D 的预算耗尽（一事实/两链/一槽）载荷用例（复审也实测了**优先级反转**：最后一个槽现在被拒绝链拿走）；
  `FindOwner`/`Find` 已成无生产调用者的死代码（待清理或加注释禁止 S/E/D 再用）；
  复审另报：合法双链导出在**读方**仍 `stageObservationComplete=False`（观察链 4 个阶段 vs 读方要求 FirstSight 链恰好 3 个）——
  **pre-C1 亦如此**，属既存写方/读方形状分歧，已登记。

## 2026-09-18 — P0-6 续：预算最后一槽的载荷用例 + 删除两条死查找（复审 cfb2a5b6 的建议 3/5）

- **Case 45（预算只剩一个槽的顺序与计损）**：双链对象 + 单链对象把当帧预算填到 63，再发一条 Served。
  实测 `[P0-45] budget=64 servedRej=1 servedObs=0 droppedFrameDelta=1` ⇒ 与复审的 `BUDGET1` 独立复现一致：
  **C1 的显式顺序（先拒绝恢复链）让最后一个槽归拒绝链**，观察链那份经 AccountDrop **具名计入 droppedPerFrame**
  （上 wire ⇒ 读方报 objectLevelEvidenceDropped / chainMissing，fail-visible）。
  **探针**：把 NoteServed 的拒绝链 advance 改成 nullptr ⇒ `servedRej=0 servedObs=1 droppedFrameDelta=0`，
  Case 45 **3 项断言全部具名失败**；已还原。
- 登记：C1 **反转了优先级**（旧 FindOwner 路由下最后一个槽归观察链）—— 这是显式顺序的**有意**后果，不是缺陷，
  但它意味着「观察链在预算紧张时更容易丢」，故必须有具名计数（已具备）。
- **删除死代码 FindOwner / Find**（复审建议 5；全树只剩注释引用）：两者正是**修复前两条静默失效的机制** ——
  FindOwner 让拒绝链拿不到恢复事实；Find（**链型无关**）让观察链拿不到任何路径事实，而误用它们**能编译通过**
  （复审据此把路由退回去做变异时是**静默**成功的）。删除后原地留下禁止性说明：新增采集事实必须用
  FindChain(key, 链型) 并显式处理两条链的丢失路径（具名计数）。
- 本轮我自己的失误：删除 Find 时尾部三行成了孤儿（droppedProbeLimit++ / return nullptr; / }），编译器立刻暴露，已修。
- 验证：静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **44 passed / 0 failed**、wire 原生 **126/0 PASS**、no-work。
- 门禁随删除更新（**加强**）：§11n(b) 原本断言已删除的 `Find`；现改为 ① `Entry* Find(` 与 `Entry* FindOwner(`
  必须**不存在**（防止 S/E/D 静默退回类型无关查找）② 布隆否定优化必须保留在唯一剩下的键查找 `FindChain` 上。
- 最终 hash：evidence.h `09B66767B20870C7CA33201CF0834560`、测试 `19BB33A47E3FC3857C38A415987AEEC5`；
  静态 **263/0**（门禁更新后复跑）、meson **85/0**、宿主机 **44/0**、wire **126/0**；站点 = 基线（未部署）。
- **仍未做（已具名）**：`encodeFailed` 是可证死代码（复审：encode 只在 session==0 失败，已被上一行挡掉）；
  计数锁 `==7` 是脆弱文本锁（应改按名锁）；内部计数（含本轮新增）**不上 wire** ⇒ 导出层面自证一致未解决；
  读方对**合法双链导出**仍 `stageObservationComplete=False`（观察链 4 阶段 vs 读方要求 FirstSight 链恰好 3 个，**pre-C1 亦如此**）；
  v3 降级绕过（生产不可达）；我加入的读方版本门控**冗余**（删掉后 107 全过）；C2 的冻结尾环终态缺失；反例 A（链型静默退化）。

## 2026-09-18 — P0-6 续：按复审 128c3826/266e35c4 修正（refusal 双向缺陷、验证配方、第四个静默丢弃、死计数器）

### ① 我的**验证配方**有错（复审 128c3826/266e35c4 都独立指出）
- 我一直在删 `build32/src/d3d9/**war3/**war3_palette_object_evidence_test.exe.p` —— 该路径**不存在**（真实路径无 `war3/`
  子目录）⇒ 那一步**一直是 no-op**。真实路径下删除后 ninja 确实重编（实测 `Compiling … 29 targets`）。
- 影响：凡只改 `.h` 而没改 `.cpp` 的那几次，「强制重编」并未真正发生（改 `.cpp` 时会重编，故多数结论仍成立于新二进制，
  但我**不能**再把该步骤当作保证）。⇒ 之后的门禁一律用 `build32/src/d3d9/war3_palette_object_evidence_test.exe.p`。

### ② 读方 refusal `recoveredWithoutRejectFact` 的**双向缺陷**（复审 128c3826 的反例 1/2）
- **误拒**：写方谓词是 `firstReason != NotChecked` ⇒ **Unknown(0xFF) 是真实拒绝事实**；旧写法把 0xFF 也当「无事实」
  ⇒ 会拒绝**写方自己认证过**的链（复审实测：写方 `closedRecovered=1`，读方 exit=2）。
- **漏报**：旧写法用「**任意**一条事件带真实理由」⇒ 链首 NotChecked + 仅终态塞 R1 的**伪造**链能通过（exit=0），
  而写方不可能产生该形状（`MakeRecord` 把 `firstReason` 盖到**每一条**记录）。
- **修正**：严格镜像写方 —— `terminal=='Recovered'` 且**任何**事件携带 `NotChecked` ⇒ 具名拒绝认证。
  用例扩为 4 条子断言：NotChecked 必拒 / **Unknown 不得被拒**（复审反例 1）/ **混合伪造链必拒**（复审反例 2）/ R1 不误报。
  读方测试 **108 OK**。

### ③ 计数锁从脆弱计数改为**按名锁**（复审的锁审计）
旧 `SINK.count('.load(std::memory_order_relaxed)') == 7` 可被**替换攻击**绕过（把某访问器改成 `return 0u;` + 加一处无关 relaxed load ⇒ 仍为 7 ⇒ 通过），
而注释里出现该字面量或加空格会**假失败**。现改为按名锁：每个具名计数器必须恰好 1 处声明、**每一处**自增都必须是 relaxed（容忍多自增点，
例如 `g_paletteObjectResetOrClear` 有 4 处）、恰好 1 处 relaxed 读取、且必须有具名访问器。

### ④ 第四个同类静默丢弃：`NoteObjectGone` 两条链都不存在（复审 128c3826 的审计发现）
此前 `SettleObjectGone(nullptr)` 直接返回 ⇒ **零计数、零事件、零终态**。现与 S/E/D 一致改为具名计入 `droppedNoChainForPathFact`；
Case 42 增加第三条断言（**探针**：撤掉该计损 ⇒ Case 42 具名失败，实测 `43 passed, 1 failed`）。

### ⑤ 恢复 `droppedProbeLimit`（复审：它已成**上 wire 的死 LOSS_FIELD**）
该字段唯一的自增点随旧 `Find()` 被删除 ⇒ 恒 0，并使 `cost_test` 的布隆守卫（`walkDelta*10 <= kFullCalls`）**恒真、失去效力**
（删掉布隆前置否定也照样通过）。已在**唯一剩下的**键查找 `FindChain` 的探测耗尽分支恢复计数。

### ⑥ 流程缺陷（复审 266e35c4 实测）：**我的就地变异探针污染了并发验证者的构建**
我曾在工作树内直接把 `AdvanceServedOnChain(rejectChain→nullptr)` 做探针并保持数分钟 ⇒ 该窗口内验证者的门禁跑出
`Ok 82 / Fail 3`、`cost_test` 5 条失败，**不可归因于被验证修订**。⇒ 纪律修正：**变异探针一律在仓库外副本上做**，
不得让工作树处于变异态；确需就地变异时必须立即还原并在记录里标注窗口。

### ⑦ 复审 266e35c4 的**潜伏缺口**（未修，下一轮处理）
现行判据是「**存在**同键+同链型+同窗口的条目」，**不证明是同一条链实例** ⇒ 若重入期间重新开窗并重建同名观察链，
该 S/E/D 事实会被**静默改挂到新链**且计数为 0（复审实测；生产暂不可达：emit 内唯一同线程重入是 `CloseWindow`）。
另：Case 44 的 `servedObs==0 / lost==1` 是其**宿主（嵌套 CloseWindow）的性质**，不是不变量（复审把触发点换到观察链 ⇒ `servedObs=2, lost=0`）。

### 验证
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **44 passed / 0 failed**、wire 原生 **126/0 PASS**、cost test exit 0、no-work。
- hash：evidence.h `73B65349EDA4848AA50F4DF0D31F7FB6`、测试 `A3CEED5849FC258335EE69C0C1517C92`；站点 = 基线（未部署）。

## 2026-09-18 — P0-6 续：实例身份判据（复审 266e35c4 的潜伏缺口）+ 复审 dcd8c729 的两处修正

### ① 链**实例身份**判据（潜伏缺口，已加代码；**载荷用例仍缺失**，如实登记）
- 复审 266e35c4 指出：C1-1 的判据只证明「**存在**同键+同链型+同窗口的条目」，**不证明是同一条链实例** ——
  重入期间若重新开窗并重建同名链，事实可能被**静默改挂到新链**。
- 已加：`Entry::instanceId`（`Insert` 时从 `m_nextInstanceId` 分配，**跨 Reset 不重置**）；三个入口与 `NoteObjectGone`
  都在首次 advance 前捕获实例号，advance 后要求 `stillSettleable->instanceId == observationInstance` 才推进，否则具名计损。
  这是**严格收紧**（只可能减少被推进的条目），无回归风险。
- ⚠️ **我未能构造出使该判据生效的用例**：我写的 Case 46（重入 `ResetForSessionTransition` + `NoteFirstSight` 重建）
  实测 `servedObs=1 lostPathFact=0`；临时 DIAG（`captured=2652 still=2652 match=1`）显示重查拿到的**仍是同一条实例**，
  事件顺序为 `Served(chain0) → Served(chain1) → FirstSight(chain1)` ⇒ **我的宿主没有复现复审描述的路径**。
  因此我**删除了该用例与全部临时诊断**，没有把它降级为「特征化断言」（那会锁住我不理解的行为）。
  ⇒ 「重入重建同名链会导致改挂」目前是**复审的实测主张 + 我的代码级收紧**，**尚无载荷测试**。

### ② 复审 dcd8c729 的两处修正
- **门禁跟随实例判据并加强**：`_OBJECT_GONE_DELEGATE` 的 C3 文本锁原本只要求
  `FindChain(key, Observation) != nullptr`（正是「存在性判断」）；现同时要求 `stillSettleable->instanceId == observationInstance`。
- **修掉一条与实现相反的遗留注释**（`evidence.h:1237-1239`）：它写着**被删 `FindOwner()` 的规则**
  （「优先 Observation，其次 RejectionRecovery…只有没有观察链时才挂到拒绝恢复链」），与现行「两条链各记一份、先拒绝后观察」正好相反。

### ③ 复审 dcd8c729 的独立确认（第二份证据）
- **Case 45 边界精确**：其自建探针扫 nSingles=59..63 ⇒ 恰在 **已用 63 / 剩 1 槽** 翻转（n=61 得 rej=1/obs=0/delta=1），
  且 `kNormalBudget`/终态预留/表容量均不参与；delta==1 只能由观察链那份经 `AccountDrop` 入 `droppedPerFrame` 产生。
- **三处入口的顺序都被钉住**：分别对调 S/E/D 的两链顺序 ⇒ 各自的结果翻转为 `obs=1`（三个入口各自被 Case 45 形态覆盖）。
- **删除 `FindOwner`/`Find` 的编译期防护真实**：把路由退回 `Find(key)` ⇒ `error: 'Find' was not declared in this scope`；
  补回 `Find()` 定义 ⇒ 门禁 `11n(b)/C1` 具名失败。
- **「删除无副作用」在声称修订上为假**（`droppedProbeLimit` 曾沦为恒 0 的 wire LOSS_FIELD）—— 这已由我在 round 97 修复 ✅。

### 验证
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主机 **44 passed / 0 failed**、wire 原生 **126/0 PASS**、cost test exit 0。
- hash：evidence.h `C138DD7D175C9FA60BB952E3461C9B0E`、测试 `A3CEED5849FC258335EE69C0C1517C92`；站点 = 基线（未部署）。

## 2026-09-18 — P0-6 续：按复审 0224f6c8/266e35c4 修三处（读方链首判据、锁归属校验、Case 47 重写）

### ① 读方 `recoveredWithoutRejectFact` 改为**链首**判据（复审 0224f6c8 反例 1，误拒）
- 复审实测：写方 `NoteReject` 会用**本次调用的 reason** 覆盖该事件的 `rejectReason`（evidence.h:462），
  而 `hasRejectFact` 用**条目**的 `firstReason`（:318）⇒ 一条 `firstReason=R1` 的**合法**链，其后续事件可以携带 `NotChecked`。
  我上一轮写的「任何事件带 NotChecked 就拒绝」因此会**误拒写方自己认证过的 Recovered 链**（写方 closedRecovered=1、读方 exit=2）。
- **修正**：只看**链首**事件的 `rejectReason`（链首由建立该链的那次 `NoteReject` 发出 ⇒ 正是 `firstReason` 的镜像）。
  仍能拦住伪造链（链首 NotChecked + 终态塞 R1 —— 写方不可能产生）。用例加到 5 条子断言（含新增的误拒反例），读方 **108 OK**。

### ② 按名锁增加 **load 归属**校验（复审 0224f6c8 反例 2：替换攻击仍可绕过）
- 复审实测两种绕过：「访问器改成 `return 0u;` + 把唯一那处 relaxed load 搬到永不被调用的函数」、以及把自增包进 `if (false)`。
  根因：旧锁只数 `.load(...)` 出现次数，**从不校验它在哪个函数体内**。
- **修正**：要求该 relaxed load 出现在**这个具名访问器自己的函数体**内，且访问器体不得被掏空（`return 0u;`）。
  **探针**（仓库外/内存内）：基线 PASS；「掏空 + load 搬走」与「仅掏空」均 **FAIL** ✅。
  仍**未**静态堵住「把自增包进不可达分支」—— 登记为开放项（见下）。

### ③ Case 47 重写（我上一轮把自己的用例写坏了，如实记录）
- 我上一轮用外科式 splice 插断言时**把断言插进了循环体** ⇒ 该用例变成反复打印同一对 FAIL 的循环，
  进程被超时杀掉后**仍占用 exe 文件**，导致后续 `ninja` 链接 `Permission denied`（我一度误判为「测试挂死」）。
  已结束该测试进程并**整段重写**该函数（断言全部移到循环之外）。
- 实测：`[P0-47] rearmed=1 rejTerm=1 obsTerm=1 obsFirstSight=2 afterHeadTerm=0 lostToReentrancy=0 watchCount=1 stored=3`
  ⇒ **重建链未被结算**（`afterHeadTerm=0`）、旧链得到其**合法** ObjectGone。
  ⚠️ 这是**回归守卫**，不是实例判据的载荷探针：本构造下重入发生在旧链自己的终态 emit 内，判据尚未被咨询。
  另：`obsTerm=1` 是**旧链**的合法终态；我上一轮把它当失败是**期望写错**，不是代码错。
  **实例判据至今没有载荷用例** —— 两次构造（S/E/D 侧、ObjectGone 侧）都显示重查拿到**同一条实例**。

### ④ 开放项（复审 0224f6c8 提出，未修）
- `NoteObjectGone` 对**重复通知**会逐次计入 `droppedNoChainForPathFact`（低影响：全树无生产调用者）；
  该计数语义「该事实无处可记」对重复通知仍然成立，但需要在注释里说明。
- sink 的 5 个旧计数器缺少**可达性**行为证明（静态锁无法阻止 `if (false)` 式藏匿）。
- 复审两次提醒：**任务给出的 md5 会在并发编辑中失效** ⇒ 结论必须绑定**实测 hash**。

### 验证（绑定 hash）
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**（exit 0，无挂死）、wire **126/0 PASS**、cost exit 0、no-work。
- hash：evidence.h `C138DD7D175C9FA60BB952E3461C9B0E`、测试 `92E79D84D5026C3502B25D2160667567`、读方 `B60879F956F7DD575121610DA581F31C`；站点 = 基线（未部署）。

## 2026-09-18 — P0-6 续：写方/读方同步（观察链阶段集）+ Astra 裁定 firstSightUsed 门禁移除

### ① 读方观察链完整性判据：从「恰好 3 个」改为**集合**判据（**写方/读方同步**）
- 缺陷（旧）：`required_stage_count=3 if 首见链 else 4` —— 注释理由是「正常观察链**永不发** ServedCandidate」。
  但 Astra 裁定 C1 要求 S/E/D 对**两条链各自**记一份 ⇒ 「对象既被观察又被服务」时观察链**合法地**携带
  4 个阶段 {FirstSight, ServedCandidate, Enqueued, Drawn} ⇒ 旧判据把**合法**导出判成
  `stageObservationComplete=False`（并波及 `chainComplete` 与 CLI 退出码）。
- **修正**：必须含三个承重阶段 {FirstSight, Enqueued, Drawn}，且所有阶段都取自合法四元组（不得有陌生阶段）；
  拒绝恢复链仍要求完整的 {Rejected, ServedCandidate, Enqueued, Drawn}。**不是放宽**：缺 Enqueued/Drawn 仍失败。
- 用例：`test_observation_chain_stage_set_is_a_set_not_a_fixed_count`（① 4 阶段必须被接受 ② 3 阶段也必须被接受
  ③ 缺 Enqueued 仍必须失败）+ 新夹具 `observation_chain_events()`。读方 **109 tests OK**。
- **探针**：把规则退回旧「恰好 3/4」⇒ 该用例**具名失败**（`assertTrue(chain4["stageObservationComplete"])`），
  且是**唯一**失败（109 tests, failures=1）⇒ 新判据有载荷、用例未过拟合。探针就地执行但**数秒内还原**（已核验当时无并发验证者）。

### ② Astra 六项裁定之一：移除强制 `firstSightUsed()` 存在的**文本门禁**
- 核实：**动态选版本用法已删除**（`war3_frame_evidence.cpp` 恒 v4，`firstSightUsed()` 只剩注释提及）；
  只读查询保留，且被宿主用例**真实使用**（`Require(rec.firstSightUsed())` / `Require(!rec.firstSightUsed())`）。
- 删除 `test_first_sight_observation_chain_static.py` 中 `assert "bool firstSightUsed() const {" in EVIDENCE_H`：
  该文本卡只保证「函数存在」，会阻止合理重构，而正当性已由真实使用保证。门禁仍 PASS。

### ③ 遗留（仍未解决，登记）
- **实例身份判据仍无载荷用例**（两次构造都显示重查拿到同一条实例）；Case 47 只是回归守卫。
- 计数锁未堵住「把自增包进不可达分支」；sink 5 个旧计数器缺行为可达性证明。
- `NoteObjectGone` 重复通知会逐次计入 `droppedNoChainForPathFact`（低影响，无生产调用者）。

### 验证（绑定实测 hash）
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**、wire **126/0 PASS**、cost exit 0、no-work。

## 2026-09-18 — 阶段 D（批次 2）状态复核：四项实现与门禁齐备 + 两条新探针（B1 探针因并发验证者推迟）

复核结论（**只读 + 运行**，本轮**未改仓库任何文件**）：

| D 子项 | 实现 | 门禁 | 本轮证据 |
| --- | --- | --- | --- |
| D1 armed 与计数读取**统一同步域** | `std::atomic<bool> g_paletteObjectArmed` + `QueryPaletteObjectEvidenceHeader` 单次持锁 `SnapshotCounters` | 静态裸 bool 禁令（`test_palette_object_evidence_analysis_static.py:1288-1294`） | 门禁随全量静态 **263/0** 通过 |
| D2 先初始化记录器与预冻结钩子**再** release 发布 active | `war3_frame_evidence.cpp:362`(hook) → `:367`(Arm) → `:368`(active release) | `test_palette_object_arm_order_static.py` | **新探针**（仓库外最小树） |
| D3 HeaderJson 计数与版本存在性**同一快照** | `result["version"]=4` 字面常量 + `const auto& c=header.counters` | `test_palette_object_header_snapshot_static.py` | **新探针**（仓库外最小树） |
| D4 三条屏障 | B1 静态+运行期、B2、B3 | 见上 + `war3_palette_object_wire_roundtrip_test.cpp:653-675` | `BARRIER armed=1 watchCount=0 emitted=0 terminalEmitted=0` + wire **126/0 PASS** |

### 本轮新出仓库外探针（仓库内**零改动**）
- **B2**：把 arm 顺序反转（`active.store` 提到 `ArmPaletteObjectEvidence` 之前）⇒ 门禁 exit 1，**具名**失败
  `D2/P0-1: the palette recorder MUST be initialized BEFORE active is published …`（对照原样 exit 0）。
- **B3**：把版本退回**动态**形态（`result["version"]=firstSightUsed()?3:2;`）⇒ 门禁 exit 1，两条**具名**失败
  （`D3: the block version must be the literal 4 …`、`D3: the version assignment must not contain a conditional`）（对照 exit 0）。

### 推迟项（如实登记，不声称 D 已完成）
- **B1 的两个探针**（① 把 `std::atomic<bool>` 退回裸 `bool` ⇒ 静态门禁须失败；② `disarm` 不发布 `armed=false` ⇒ wire 用例须失败）
  **本轮未做**：当时有并发验证子线程在跑，就地变异会污染其构建/读取（此前已有实测污染先例）⇒ 按纪律推迟。
- 因此本轮只声称「D 的四项**实现与门禁**齐备」+「B2/B3 探针成立」，**不声称阶段 D 已完成**。

### 文档一致性（一处过期已记）
- `docs/plan/2026-09-18-astra-stageD-B1-direction-A-decided.md` 预期打印 `BARRIER checks=5`；现行实现改用共享 `Check()` 助手
  ⇒ 4 条断言计入全局 `checks=`，`armed` 的真假仍在 `BARRIER` 行**打印**（该文档真正要求的「真空通过须可见」依然满足）。

## 2026-09-18 — 两处只读发现：终态身份审计闭合 + 我的测试发射器判据写反（解释全部历史观测）

### ① 终态身份审计（闭合计论中「解析器 710/716 未排除终态」一项）——**据实闭合，无需改动**
- 逐点核对写方所有记录构造点：`MakeRecord(...)`（`evidence.h:340/460/976/1133`）与 `PrepareStage(...)`（`:872/913/954`）
  都以 `MakeRecord` 为基；**唯一例外**是一次性 TableFull 终态（`:421` 起），它在 `:429` **显式**调用
  `DeriveIdentityProofKind(key)` 盖章（`:426-427` 的注释正是为此）。
- ⇒ 读方 `proof_kinds=sorted({event[IDENTITY_PROOF_FIELD] …})` 对**全部**事件取值与写方一致，
  **不存在**「终态缺身份证明 ⇒ 误拒」的形状；反过来，**排除终态会削弱**检查 ⇒ 不改。
- 残留风险（登记）：若将来新增一条**绕过** `MakeRecord` 且忘记显式盖章的终态路径，读方会误拒 ——
  该要求在 `:426-427` 有注释，但**没有静态门禁**强制「每个 `record{}=` 构造点都必须盖章」。

### ② 我的宿主测试发射器**判据写反**（这解释了此前全部观测，且使实例判据从未被咨询）
- 我在 Case 46/47 的发射器里写的是 `if (… || !g_done.exchange(true)) return;`。
  `exchange(true)` 返回**前值** ⇒ 首次调用时 `!false == true` ⇒ **首次即返回，不做重臂**；
  重臂实际发生在**第二个**同类事件上。
- 这与全部实测一致：Case 46 `servedObs=1 / lost=0`（重臂发生在**观察链自己的** advance 之后）、
  Case 47 `[P0-47 emit] wcBefore=0`（触发时两条链都已被认领、表已空）。
- **后果**：实例身份判据（`stillSettleable->instanceId == observationInstance`）**从未被咨询** ——
  我此前写的「重入发生在旧链自己的终态 emit 内」是**对现象的描述**，根因是这条判据写反。
- **待办（下一轮，需无并发验证者时）**：把两处改成 `if (… || g_done.exchange(true)) return;`（去掉 `!`）⇒
  重臂将发生在**拒绝链**的 advance 内 ⇒ Reset 清掉观察链条目 ⇒ 重查将拿到**不同实例** ⇒
  实例判据**首次具备载荷**（预期 `servedObs=0 / lostPathFact=1`；撤掉判据则应变成 `servedObs=1 / lost=0`）。
- B1 的两个探针仍因并发验证子线程 `53302f41` 在跑而推迟（本轮已再次确认）。

### ③ B1 **静态**半边探针（同形断言的内存内再执行；仓库零改动）
- 与 `test_palette_object_evidence_analysis_static.py:1288-1294` **同形**的四条断言，对变异源文本求值：
  基线 0 条失败；①裸 `bool` ⇒ 检出「bare-bool」+「缺少 atomic 声明」；②`release`→`relaxed` 发布 ⇒ 检出「缺少 release 发布」；
  ③`acquire`→`relaxed` 读取 ⇒ 检出「缺少 acquire 读取」；三处变异均确认文本确实改变。
- ⚠️ 诚实边界：这是**同形断言的再执行**，不是整份门禁脚本（它要读全树，最小树不可用）；
  「B1 运行期半边」探针（disarm 不发布 `armed=false` ⇒ wire 用例须失败）需重编 ⇒ 仍因并发验证者 `53302f41` 推迟。

## 2026-09-18 — 实例身份判据**首次取得载荷证据**（仓库外探针）+ 根因确认（我的测试发射器判据写反）

### 探针（`E:/Work/probe_instance/`，仓库内零改动）
直接 `#include` 记录器头编译独立探针；发射器使用**正确**判据（`if (g_done.exchange(true)) return;`，
即**首次** ServedCandidate 就重臂），重臂动作 = `ResetForSessionTransition(同会话)` + `NoteFirstSight(同键)`。
对照的 `mut` 把四处 `stillSettleable->instanceId == observationInstance` 全部退回**仅存在性**判据（4 处，变异已确认）。

```
orig（有实例判据）: rearmed=1 stored=2 | servedRej=1 servedObs=0 obsFirstSight=1 lostPathFact=1
mut （仅存在性）  : rearmed=1 stored=3 | servedRej=1 servedObs=1 obsFirstSight=1 lostPathFact=0
```

- ⇒ 撤掉实例判据后，该 S/E/D 事实被**静默改挂到重建出来的新链**（`servedObs=1`）且**零计损**（`lostPathFact=0`）；
  有判据时它**不落新链**且**具名计入 `pathObservationSkippedAfterReentrantClose`**（`lostPathFact=1`）。
- ⇒ 实例判据**改变可观测行为**，且它阻止的正是**静默**失效 —— 与复审 266e35c4 的「反例 1/5」描述一致。
  （此前两次宿主构造都测不到它，根因见下。）

### 根因确认：我的测试发射器判据写反（round 102 的只读发现，本轮由探针独立证实）
- Case 46/47 写的是 `!g_done.exchange(true)`；`exchange` 返回**前值** ⇒ 首次即返回、**不做重臂** ⇒
  重臂实际发生在**第二个**同类事件上 ⇒ 实例判据**从未被咨询**。
- 本轮探针把判据改成正确的 `if (g_done.exchange(true)) return;` ⇒ 重臂发生在**拒绝链**的 advance 内
  ⇒ 观察链被清表销毁 ⇒ 重查拿到**不同实例** ⇒ 判据生效（`stored=2/lost=1`）✅

### 待办（需仓库写入，因并发验证者 `53302f41` 仍在跑而推迟）
1. 修 `war3_palette_object_evidence_test.cpp` 两处发射器判据（去掉 `!`）；
2. 按新语义校正 Case 46/47 的期望（重臂改在拒绝链 advance 内发生 ⇒ 观察链被清 ⇒ 期望 `servedObs=0/lost>=1`）；
3. 加**载荷断言**：撤掉实例判据 ⇒ 用例必须具名失败（探针已证明该差异可观测）。
4. B1 **运行期**半边探针（`disarm` 不发布 `armed=false` ⇒ wire 用例须失败）——需重编，同因推迟。

## 2026-09-18 — 实例身份判据**收口**：Case 47 判据修正 + 载荷探针成立（附一次自我更正）

### ① 自我更正：`!g.exchange(true)` 的语义**取决于它所在的形式**（我此前把范围说宽了）
- **`&&`-动作式**（Case 43/44：`if (stage==S && !g.exchange(true)) 动作;`）：`exchange` 返回**前值**，
  首次为 false ⇒ `!false` = true ⇒ **首次即动作** ✅ **这些用例一直是对的**。
- **`||`-提前返回式**（Case 46/47：`if (stage!=S || !g.exchange(true)) return;`）：首次 `false || true` = true ⇒
  **首次即返回、不做动作** ❌ ⇒ 我此前写的「判据写反、解释了全部观测」**只对 Case 46/47 成立**；
  Case 43/44 从未受影响。round 102/103 的措辞过宽，此处更正。

### ② Case 47 修正（判据 + 期望一起改，二者必须同时改）
- 判据改为「先判类型、再用**已完成**作提前返回」：`if (terminal != ObjectGone) return; if (g.exchange(true)) return;`。
- 期望按<u>修正后的真实语义</u>重推：重臂发生在**拒绝链的 ObjectGone** emit 内 ⇒ `ResetForSessionTransition`
  清表销毁**旧观察链**（它拿不到 ObjectGone）⇒ 重建一条新链 ⇒ 观察侧只剩**重建链的 live 链首**。
- 实测：`[P0-47] rearmed=1 rejTerm=1 obsTerm=0 obsFirstSight=1 afterHeadTerm=0 lostToReentrancy=1 watchCount=1 stored=2`
  ⇒ 宿主 **45 passed / 0 failed**。

### ③ **载荷探针成立**（撤回四处实例判据）
```
变异后: rearmed=1 rejTerm=1 obsTerm=1 obsFirstSight=2 afterHeadTerm=1 lostToReentrancy=0 watchCount=0 stored=3
[FAIL] 47 … (8 checks, 5 failures)   ← 5 条具名失败
```
⇒ 撤掉实例判据后，**重建出来的新链被静默结算为 ObjectGone**（`obsTerm=1`、`afterHeadTerm=1`）且**零计损**；
这正是复审 266e35c4 描述的静默失效 ⇒ **Case 47 现在是实例判据的真实载荷用例**（此前两轮都没有覆盖它）。
（仓库外探针 `E:/Work/probe_instance/` 在 round 103 已独立给出同一结论：`orig servedObs=0/lost=1` vs `mut servedObs=1/lost=0`。）

### ④ 验证（绑定实测 hash）
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**、wire **126/0 PASS**（含 `BARRIER armed=1 …`）、
  cost exit 0、no-work。
- hash：测试 `93A0ADB0688BB426BD0632D956188371`、evidence.h `C138DD7D175C9FA60BB952E3461C9B0E`；站点 = 基线（未部署）。

### ⑤ 仍未做
- **B1 运行期半边**探针（`disarm` 不发布 `armed=false` ⇒ wire 用例须失败）：需链接完整生产二进制，
  无法在仓库外单独构造 ⇒ 下一步在无并发验证者窗口内做。

## 2026-09-18 — **阶段 D（批次 2）收口**：四项实现 + 三条屏障**全部具备载荷证据**（B1 运行期探针在仓库外完成）

### B1 运行期半边探针（**仓库外**，因并发验证者无法就地变异；`E:/Work/probe_b1/`）
做法：把生产 sink 的**同一个 TU**（`war3_palette_object_evidence_sink.cpp` 逐字复制）与我自写的桩
（`InputsEnabled` / `PaletteObjectEvidenceEnabled` / `ActiveSession` / `Record`）及同形驱动链接成独立可执行文件，
驱动里的 5 项检查与 `war3_palette_object_wire_roundtrip_test.cpp:653-675` 的 B1 块**同形**。
```
orig: BARRIER armed=1 watchCount=0 emitted=0 terminalEmitted=0 / BARRIER checks=5 failures=0 PASS   (exit 0)
mut : 把 Disarm 的 `g_paletteObjectArmed.store(false, …)` 改掉 ⇒
      FAILURE: B1 disarm must publish armed=false (the load-bearing check) / failures=1 FAIL        (exit 1)
```
- ⚠️ 诚实边界：本轮**没有**在仓库内对真实 wire 用例做变异（并发验证者 `9436f307` 在跑）；
  但探针使用的是**同一 sink TU**、同形检查，且 `armed` 的真假在该驱动里被**打印**（真空通过可见）。

### D 阶段四条子项 + 三条屏障的完整证据状态
| 条目 | 实现 | 门禁 | 载荷证据 |
| --- | --- | --- | --- |
| D1 armed 与计数同一同步域 | atomic + 单次持锁 `SnapshotCounters` | 静态裸 bool 禁令 | 同形断言内存内探针（裸 bool / relaxed 发布 / relaxed 读取 三种变异均被检出） |
| D2 先初始化+挂钩子再 release 发布 active | `:362→:367→:368` | `test_palette_object_arm_order_static.py` | 仓库外最小树变异（顺序反转）⇒ **具名失败** `D2/P0-1` |
| D3 HeaderJson 单快照 | `version=4` 字面量 + 同一 header 快照 | `test_palette_object_header_snapshot_static.py` | 仓库外最小树变异（退回动态版本）⇒ **两条具名失败** |
| D4 三条屏障 | B1 静态+运行期、B2、B3 | 见上 + wire `:653-675` | **本轮 B1 运行期探针成立**（orig 5/0 PASS、mut 1 failure） |

⇒ **据实收口阶段 D**（在本目标的 D 条目范围内：四项实现齐备、门禁齐备、四条探针全部成立）。
残留（**不**属于 D 条目、另行登记）：`NoteObjectGone` 重复通知的计数语义、sink 五个旧计数器的可达性行为证明。

### 验证（绑定实测 hash）
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**、wire **126/0 PASS**、cost exit 0、no-work。
- hash：测试 `93A0ADB0688BB426BD0632D956188371`、evidence.h `C138DD7D175C9FA60BB952E3461C9B0E`；站点 = 基线（未部署）。

⇒ 两个 P0 中 **D 已收口**；剩 **B/C**（其 C 尾项：`注` sink 的未计数静默丢弃、反例 A、C4、v3 降级绕过、
「生产编码往返测试 / 生产调用方传参测试」两项具名确认）。**在新结论/晋升前必须完成 B/C。**

## 2026-09-18 — B/C 尾项清理：C4 与反例 A 定性；两项具名交付确认；sink 静默丢弃已闭合

### ① C4（拒绝链 ObjectGone 的阶段回填）——**非缺陷，属既定契约**（据实关闭登记）
- `SettleObjectGone`（`evidence.h:973-1006`）**不再硬写** `Rejected`：
  `record.stage = e->sawFirstSight ? StageOfRank(e->maxStage) : PaletteObjectStage::Rejected;`（`:982`），
  注释明写「观察链从未被拒绝过，给它写 Rejected 就是伪造事实，且与读方规则『观察链不得携带 Rejected 阶段』冲突」。
- **拒绝恢复链保留 `Rejected`** 是**有意**的（其链首语义就是拒绝），读方容忍且不做反悔 ⇒ 我此前的登记措辞把它当成缺陷，**更正**。

### ② 反例 A（拒绝事实被当帧预算吞掉 ⇒ 链型退化）——**已特性化：是「已计数的后果」，不是静默失效**
- 仓库外探针 `/E:/Work/probe_caseA/`（记录器头逐字复制后编译）：先填满当帧预算，再对目标键发拒绝，
  然后**新帧**观察同一对象并关窗。实测：
```
caseA: dDroppedPerFrame=1 dDroppedPerSession=0 | watch 64->64 | target: RejectedEvents=0 FirstSightEvents=2 terminals=1
  [target terminal] stage=5 terminal=7 chain=1
```
- 三条可判定事实：① 被吞掉的拒绝**计入 `droppedPerFrame`**（**上 wire** 的 LOSS_FIELD）⇒ 读方报
  `objectLevelEvidenceDropped` / `chainMissing`；② 条目**未建立**（`watch 64->64`，无 headless 条目）；
  ③ 该对象的链型变成 `Observation` 并 `ObservationClosed` 收尾 —— 这是**诚实**的：记录器确实没有它的拒绝事实。
- **残留（据实登记）**：`droppedPerFrame` 是**聚合**计数，读方**无法把它绑定到具体对象** ⇒ 「该对象的拒绝被丢过」
  只能由聚合计数 + 该对象首见时机的关联推断，不能由导出直接判定。

### ③ sink 发射路径的「未计数静默丢弃」——**已闭合**
- 四条出口逐条核对：子门关闭的裸 `return`（`:65-66`）是**按设计**（且被门禁锁定为「必须不计」）；
  会话未激活（`:68-72`）计入 `droppedNoSession`；编码失败（`:74-77`）计入 `encodeFailed`；
  环 append 失败（`:80-81`）经 `NoteRingEviction(1u)` 计入。⇒ C2 指出的那条未计数丢弃**已不存在**。

### ④ 裁定要求的两项**具名交付**——确认存在
- **生产编码往返测试**：`src/d3d9/war3/render/tests/war3_palette_object_wire_roundtrip_test.cpp`
  （调用生产转换点 `EncodePaletteObjectEvent` 与生产环 `Record`；本轮实测 **126/0 PASS**）
  + `AutoTest/test_palette_object_wire_roundtrip.py`。
- **生产调用方传参测试**：`AutoTest/test_palette_object_attempt_serial_caller_static.py`（K3：工厂必须真的接收并写入 attemptSerial；
  全树扫描、恰好 4 个调用点、裸未知号拒绝）+ 宿主真实传参路径 `war3_palette_object_evidence_test.cpp:3547`。

### 状态
- 阶段 D **已收口**（见上一条）；B/C 的上述尾项**均已定性/闭合**，剩余为：**v3 降级绕过**（生产不可达，登记）
  与 C2 的**冻结尾环终态**说法待复核。
- 门禁（本轮未改产品代码）：静态 **263/0**、meson **85/0**、宿主 **45/0**、wire **126/0**、cost 0、no-work；站点 = 基线（未部署）。

## 2026-09-18 — **阶段 B/C 收口**：C 的十项逐条落地并取得证据（含 C2 自动冻结说法的实测闭合）

### C2「自动冻结丢终态」——**实测闭合**（跑真实 Python 驱动，非静态推断）
```
AUTOFREEZE postPresents=8 state=3 reason=2 triggerSequence=6 reserved=23 accepted=23 postRemaining=0
HEADER_F_AFTER_AUTOFREEZE watchCount=0 emitted=5 terminalEmitted=1 droppedPerFrame=0 …
HEADER_F_AFTER_EXPORT    emitted=5 terminalEmitted=1 …
CHECKS=1160 FAILURES=0 ; ROUNDTRIP_VERDICT=CERTIFIED_SPEC_SATISFIED   (driver_exit=0)
```
⇒ 自动冻结（**无** `Control(freeze)`）路径上，终态由**预冻结钩子**在环冻结**之前**写入，
冻结环里 `terminalEmitted=1` 且导出后不变 ⇒ 「冻结环 0 终态」是**修复前**的旧观测，现已不成立。

### C 十项逐条的落地证据（本轮补最后两项）
| # | 裁定项 | 证据 |
| --- | --- | --- |
| 1 | 两类链独立、链型建立后不可改 | `FindChain` 谓词 `SameKey && chainType==type`；Case 27/28/35 |
| 2 | 同一对象可同时持有两条链 | Case 27（`[OWNERSHIP-27] servedObs/servedRej…`）|
| 3 | 查找维度 = 对象键 × 链型 × 窗口 | `SameKey + chainType + windowSegment`，跨窗口命中计入 `windowMismatchLookups` |
| 4 | 两类入口**先 CanEmit 再 Insert** | **本轮核对**：`NoteReject` 顺序「去重 → 表满 → `CanEmit(false)`@:443 → `Insert`@:450」；`NoteFirstSight` 同类顺序 ✅ |
| 5 | 跨帧判序按一次明确关联的尝试（按值携带）| attemptSerial 按值；`AutoTest/test_palette_object_attempt_serial_caller_static.py` + 宿主 `:3547` |
| 6 | 新增 `ObservationClosed=7`；Observation 永不 Recovered；Recovered 需真实拒绝事实 | 读方具名规则 + 用例（含 `recoveredWithoutRejectFact` 的双向反例）|
| 7 | 新写方恒 v4；v1/v2/v3 按各自版本解析；终态/阶段表**按版本**校验 | `war3_frame_evidence.cpp` 恒 `version=4`；语义门禁 §11r + 读方版本表 |
| 8 | 修「解析器 710/716 未排除终态」的误拒不变量 | 审计：所有记录构造点（含一次性 TableFull 终态 `:429`）**都**盖 `identityProofKind` ⇒ **无需**排除终态（排除反而削弱）|
| 9 | 写方/读方/测试同步 | 观察链阶段集集合化（读方）+ 终态矩阵规则；109 读方用例 + 45 宿主用例 |
| 10 | 分别列项「生产编码往返测试」「生产调用方传参测试」 | 前者 = 原生 wire 往返（用生产 `EncodePaletteObjectEvent`/`Record`，**1160/0**）+ `…wire_roundtrip.py`；后者 = `…attempt_serial_caller_static.py` + 宿主真实传参 |

### 残留（登记，非阻断）
- **v3 降级绕过**：生产不可达（写方恒 v4）⇒ 登记为 API/健壮性问题。
- 反例 A 的**聚合性**：`droppedPerFrame` 是聚合计数，读方无法把它绑定到具体对象（已特性化，非静默）。
- `NoteObjectGone` 重复通知的计数语义；sink 五个旧计数器缺可达性行为证明。

### 结论（据实）
- **B/C 与 D 两个 P0 均已收口**（D 见 round 105 记录；B/C 见本条与 round 106）。
- 这不等于「全门禁通过」，也不构成任何实机因果结论或稳定候选晋升；
  下一步按固定顺序进入 **E（受控实机：D 点原因字段按值携带 + 五个点位 + 白名单采集）**。
- 可能推翻本收口的条件（预先写明）：若任一条目被独立验证者构造出反例（例如 `CanEmit` 仍有未计数出口、
  或某条链型/阶段/终态组合能被写方产出而读方规则自相矛盾），则该条目回退为未完成。

## 2026-09-18 — **阶段 E（批次 3）代码半边完成**：D 点原因**按值携带**，不再由 frameTag 反推

### 缺陷（旧形态）
- S/E 判定处已改为携带值（`paletteObjectSelectionFromLiveNative`，见 `test_palette_object_native_reason_static.py`），
  但 **D 点**仍在反推：`nativeKnown = draw.paletteDiagnostics.frameTag != 0u`、
  `selectionClearedByNativeOverride = draw.paletteDiagnostics.frameTag == 0u`。
- 两处都不成立：① packet 回退时 `selectedPalette` 带的是 **packet 的 tag（非零）** ⇒ 把「packet 的帧」谎报成
  「native 帧已知」；② 「不是 live native」（packet 回退/陈旧）**不等于**「被 native override 清空」。

### 改动（三个文件，四处真实分支 + D 点两处消费）
1. `d3d9_war3_scene.h`：`PaletteObjectDiagnostics` 新增嵌套 `enum class PaletteObjectDrawCause`
   （`Unknown/LiveNative/PacketFallback/StaleRefresh/NativeOverrideCleared`）与**按值**字段 `cause`。
2. `d3d9_device.cpp`：
   · `:20873` **来源判定处**赋 `LiveNative` / `PacketFallback`；
   · `:20885` **新鲜度判定处**（`IsSkinPaletteSelectionCurrent` 为假）赋 `StaleRefresh`；
   · `:21543` 随诊断载荷**按值携带**；
   · `:21720` **真实清空分支**（`draw.paletteDiagnostics = {};` 的同一处）重新赋 `NativeOverrideCleared`
     —— 整体清空会把 cause 归零，必须在真实分支写回。
3. `d3d9_war3_shadow.cpp` D 点：`nativeKnown ⟺ cause == LiveNative`；
   `selectionCleared ⟺ cause == NativeOverrideCleared`（**不再**由 frameTag 反推）。

### 门禁扩展 + 探针
- `AutoTest/test_palette_object_native_reason_static.py` 增加 D 点半边：
  ① 不得出现 `draw.paletteDiagnostics.frameTag != 0u` / `== 0u`；② 必须消费 `LiveNative` 与 `NativeOverrideCleared`；
  ③ 载荷必须带 `cause` 字段；④ 四个真实分支都必须赋值。
- **探针**：把 D 点退回 `frameTag != 0u` ⇒ 门禁 exit 1，**两条具名失败**
  （`E/D: the D point must not re-derive nativeKnown from frameTag …`、`E/D: the D point must consume the carried LiveNative cause`），
  已还原。

### 验证（绑定实测）
- DLL 构建 **exit 0**（`build32/src/d3d9/d3d9.dll` SHA-256 前缀 `69379AB40A4E9AFAF584E4BD`；
  **注意构建不可复现** ⇒ 该 SHA 不是源码指纹）、`ninja -C build32 -n` no-work。
- 静态 **263/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**、wire 原生 **126/0 PASS**、
  Python 协议驱动 **CHECKS=1160 FAILURES=0**、cost exit 0；站点 = 基线 `F275545BAA65A015`（**未部署**）。

### 尚未做（E 的实机半边）
- 白名单采集与完整启动环境保存；
- 五个点位的**实机**确认（`:20321/:20452/:20865-20870/:21694-21697/:23299-23530` 的运行时到达与取值）。
  ⇒ 代码半边完成**不**等于阶段 E 完成；**不**新增实机因果结论。

## 2026-09-18/19 — **阶段 E 实机半边：部署 + 启动环境快照 + 夜间采集启动**（状态变更，必须知悉）

### ⚠️ 站点状态已变更（不再是基线）
- `E:\Work\Warcraft III\d3d9.dll`：`F275545BAA65A015…`（基线） ⇒ **`69379AB40A4E9AFAF584E4BDC086A6BD2853A10C3615BE7F0DF41FC0DF12C2C9`**（阶段 E 构建，36,347,930 bytes）。
- **备份**：`E:\Work\Warcraft III\d3d9.dll.F275545B_backup_20260918-212247`（= 基线本体，已校验存在）。
- 站点自带的 `回退到A0A51AF2.cmd` 仍在；**回退路径 = 把上述备份复制回 `d3d9.dll`**。
- 替换前确认**无任何游戏/编辑器进程**（`game_running=0`）⇒ 未 kill 任何进程、未触碰 `E:\Work\War3`。

### 启动环境快照（裁定要求的「保存完整启动环境」）
- 装置：`AutoTest/capture_startup_environment.ps1`（**只读**；白名单从 `src/` 机械枚举的 `DXVK_WAR3_*`，并另记全量 `DXVK_*`、主机 OS/CPU/GPU、dev+site 两个 DLL 的 SHA-256/大小/mtime）。
- 产物：`E:\Work\warvk-capture\startup-env-20260918-212256.json` + `.md`（required 变量 **all present**）。

### 实机序列（夜间窗口内，授权范围）
- 本机 `local_now=2026-09-19T05:24+08:00` ⇒ **在批准的夜间窗口 02:30–09:30 内**；
- **隔离桌面在本机不可用**（`AutoTest/README.md:134`：所有 `use_isolated_desktop=true` 请求会在 CreateProcess 前失败）⇒ 按授权使用可见桌面 + 夜间窗口；
- 运行手册：`E:\Work\warvk-live-E\run_live_E.ps1`（**只操作站点树**；检测到已有游戏进程即**中止**；导出后只关闭**自己启动的** PID）；
- 前置校验（全部通过）：`Game.dll == E04D1716…`、站点 DLL == E 构建哈希、地图 `Maps\光影测试(高压).w3x` 存在（`D9230E04…`）；
- 启动形式：`war3.exe -loadfile "<map>"`，环境 `DXVK_WAR3_FRAME_EVIDENCE=1` + `…_PALETTE_OBJECT=1` + `…_OUTPUT=<evidence dir>`；
- 控制面：`frame_evidence_control.py --pid <pid> arm|trigger|export`；导出后用 `analyze_frame_evidence.py` 与 `analyze_palette_object_evidence.py` 分析；
- 已在窗口内启动（后台任务 `pwsh-148`），流程：前置 → 快照 → 启动 → 150s → arm(65536) → 180s → trigger(8) → export → 分析 → 关闭自有 PID。

### 尚未完成 / 不声称
- **采集结果尚未读取**（本轮只到「已启动」）；在拿到导出并分析之前，**不新增任何实机因果结论**；
- **不**把隔离/可见桌面的采集当作前台性能数据；
- **不**声称阶段 E 完成（实机半边尚待结果判读与五点位的运行时确认）。

## 2026-09-19 — 实机半发现并修复一个**真实仓库缺陷类**：PowerShell 脚本缺 UTF-8 BOM

### 现象（实测，非推断）
- 无人值守运行手册用 **Windows PowerShell 5.1**（`powershell.exe`）执行；`AutoTest/capture_startup_environment.ps1` 在那一刻**解析失败**：
  `The '<' operator is reserved for future use` / `The string is missing the terminator` / `The assignment expression is not valid`，
  并伴随中文乱码（`蹇呭～椤癸紙…`）。
- **根因**：该 `.ps1` 含中文但**没有 UTF-8 BOM** ⇒ PS 5.1 按 ANSI 解码。PowerShell 7（harness 的 `pwsh`）默认 UTF-8，
  所以我先前交互跑它是好的、无人值守跑它是坏的 ⇒ **同一脚本两种结果**，且失败只在 stderr、外层退出码仍可能为 0（**静默**）。
- 后果：裁定要求的「保存完整启动环境」这一步在无人值守路径上**实际没有执行**。

### 修复
- 给两个含非 ASCII 且无 BOM 的脚本补 BOM（内容一字未改）：
  `AutoTest/capture_startup_environment.ps1`、`AutoTest/send_input_plan_same_desktop.ps1`。
- 新增门禁 `AutoTest/test_powershell_bom_static.py`：任何**含非 ASCII** 的 `AutoTest/*.ps1` 必须带 UTF-8 BOM（检查 11 个脚本）。
- **探针**：临时去掉 `capture_startup_environment.ps1` 的 BOM ⇒ 门禁 exit 1，具名失败
  `… contains non-ASCII but has no UTF-8 BOM (Windows PowerShell 5.1 will mis-decode it)`；已还原（BOM 字节 239,187,191 复验 ✅）。

### 教训（写入记录）
- 我此前把「用 `pwsh` 跑通」当成该装置可用 ⇒ **解释器差异必须与产物一起记录**；
  无人值守路径的解释器（`powershell.exe` = 5.1）才是**实际生效**的那个。

## 2026-09-19 — 独立证伪回报（验证者 2475360e）与**C1 插队修复**；C2/M3/双轨漂移据实登记

### 验证者确认（我复现的部分）
- named static PASS ✅、探针 M1a/M1b 均 exit 1 + 具名失败 ✅、meson **85/0** ✅、宿主 **45/0** ✅、
  `py …wire_roundtrip.py` ⇒ **CHECKS=1160 FAILURES=0** ✅；静态它测得 **264/264**（与我一致；此前记 263 是**记账滞后**，非失败）。
- ⚠️ 它指出：`war3_palette_object_wire_roundtrip_test.exe` **零引用** `paletteDiagnostics`/`LiveNative`/`NativeOverrideCleared`
  ⇒ wire 系列**不覆盖** D 点改动（我此前也未如此声称，但记录下来避免误用）。

### ⚠️ C1（**我引入的真实退化**，已修）
- `device.cpp:20794-20801`：`liveRuntimeGroupPaletteReady` 只要 `drawTimeCapturedPaletteReady` 就置真，**该处无本帧刷新证明**；
  真正的证明 `capturedPaletteCurrentFrameProven`（`:20392-20400`）只用于「是否尝试 rebuild」；
  且唯一陈旧判据 `IsSkinPaletteSelectionCurrent` 被 `ContractEnabled()`（**默认 0**）短路 ⇒ 默认构建无新鲜度检查。
- 后果：`frameTag==0 ∧ liveReady` 时我写出 `nativeFrameTag=0 + nativeUnknown=false`，而旧式 `frameTag!=0` 反推写 `true`
  ⇒ **旧式才是事实**；`frameTag==0` 是契约明确容忍（`war3_current_draw_contract.cpp:1397`）且**不**表示 native 帧已知。
- **修复**：`LiveNative` 追加合取项 `&& selectedPalette.frameTag != 0u`（`:20873-20884`），
  并写入门禁第 8 条；**探针**：退回无合取项 ⇒ exit 1 具名失败（`E/D(C1): LiveNative must require a non-zero frame tag …`），已还原。
- 该修正的性质：**只能更保守** —— 它绝不新增任何 `LiveNative` 断言，同时保留「packet 的非零 tag 不再冒充 native 帧」的原始修正。

### ✅ C3（本次改动确有收益，验证者确认）
- `device.cpp:30346-30347` 清空 palette 后提交（`override` **没有发生**）⇒ 旧式 `frameTag==0` 会谎报
  `selectionClearedByNativeOverride=true`，新式按 `cause != NativeOverrideCleared` 给 `false`（**事实**）。

### 未闭合（据实登记，不得销账）
- **C2（漏报 nativeKnown，推断）**：`device.cpp:26317-26341` 只在 `producerPaletteCurrentFrameProven` 时把
  `leased.packet.paletteSelection = liveLeaseSelection`（frameTag==当前 native 帧、非零、**已证明本帧**）；
  若该 packet 提交时 submit 端 live 门（`:20806` 分支 b / `:20817` 失败）未给出 `liveRuntimeGroupPaletteReady`，
  则记 `PacketFallback` ⇒ D 点 `nativeUnknown=true`，而旧式记 `true`（**旧式才是事实**）。可达性需实机确认。
- **M3（门禁能力边界）**：门禁是**文本形态**检查 —— 把判定条件改成恒真（每次 packet 回退都谎报 `LiveNative`）仍 PASS。
  ⇒ 探针 exit 1 **只证明文本没被改回 `frameTag`**，不证明语义正确。**不得**用本门禁冒充语义证明。
- **双轨漂移**：S 点（`device.cpp:22160-22161`）仍用另一变量 `paletteObjectSelectionFromLiveNative`，不读 `cause`；
  门禁第 40 行还**强制**这种双轨。两轨目前推理等价，但**没有任何门禁锁住它们不漂移**。
- 覆盖性穷举（验证者做，我采信其方法）：全仓库仅 4 个写点（A `:20873-20876` / B `:20884-20886` / C `:21537-21543` / D `:21717-21720`），
  写回不会被后续覆盖；其余 8 个 caster 产出点都不写 `paletteDiagnostics` ⇒ `cause=Unknown`，且均**不携带**语义 Selection
  ⇒ 不存在「确实是本帧 live native 但 cause 停在 Unknown」的漏报。

### 本轮门禁（含 C1 断言与 BOM 门禁）
- 静态 **264/0**；named static PASS；探针（C1 退回）exit 1 具名失败；
- 早前实测：meson **85/0**、宿主 **45/0**、wire 原生 **126/0**、Python 协议驱动 **1160/0**、cost 0。

## 2026-09-19 — C2 定论（验证者 2475360e 第二轮）与我的处置：**不实现我设想的判据**

### Q2（我设想的廉价判据被**证伪**，采纳）
- 我设想在 `:20873` 追加 `hasRuntimeGroupPalette ∧ min!=0 ∧ min==max ∧ min==m_war3ShadowPersistentFrameSerial ∧ selectedPalette.frameTag==min`。
- 验证者的域对齐证据：`runtimeGroupPaletteMin/MaxFrameTag`（写点 `6706-6707`/`6738`/`26347`）、`selectedPalette.frameTag`、
  `QueryCurrentPaletteFrameTag` **三者同一计数器**（都读 Game.dll+0xBDA4CC）；而 `m_war3ShadowPersistentFrameSerial`
  全仓库唯一写点是 `device.cpp:33689` 的 `++`，**从不**加载 +0xBDA4CC ⇒ **跨域**。
- ⇒ 该项**要么恒假（等于没修）**，**要么偶然相等（新的谎报）**。且 (a)(b)(c)(e) 由 `26345-26348` 的同一次刷新一起写入，
  属**构造性自洽**，对任何曾被刷新过的 packet 都成立，**不构成「本帧」证明**。
- **处置：不实现 (a)-(e)**（我未写任何相关代码 ✅）。

### Q1（C2 可达性）
- 默认配置下 **不可达**：lease 恢复端只接受 `poseFreshForLease`（`:26567`），lease 创建/恢复端都必须过
  `packetSafeForDirectPartLease`（`:25260`，其 `25305-25312` 强制 `hasRuntimeGroupPalette && !empty()`），
  而权威门要求 `size() >= vertexCount`（`20252`）⇒ 二者共同迫使 `drawTimeCapturedPaletteReady==true` ⇒ `:20794` 必置位，
  `:20806/:20817` 分支不参与决策。
- **残留缺口（验证者推断，未静态闭合）**：若某 packet 能以「**非空但短**的 owned group slots」且无 immutable 证明进入 direct-part-lease，
  则 `:20794` 可能为假。验证者确认直接 CurrentDraw 路径不会如此，但**未**证明
  `war3_shadow_native_runtime.cpp:278` 与 `war3_shadow_renderer_core.cpp:8209` 产出的 packet 不会。
- **加固建议（验证者）**：把 `:25306` 的 `empty()` 改为 `size() < size_t(vertexCount)` — **纯比较、零新增读**。
  ⚠️ **我**不执行：该改动**改变 palette/lease 准入**，被本目标的硬约束（不得借本计划改变 palette 准入）明确禁止；
  登记为**待用户决定**的独立后续。同样登记验证者的另一建议（去掉新鲜度检查上的 `ContractEnabled()` 门，
  显式接受每 draw 一次 native 读）。
- **实机判定锚点（供将来采集用）**：`:26357` `semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount` /
  `:26638` `semanticSceneDirectPartLeaseRestoredCount` >0 的帧上，看 `:20851` `semanticSceneLivePaletteRefreshMissCount`
  是否同时 >0；`:25309` `semanticSceneDirectPartLeaseRejectedNotSelfContainedCount` 判断「短 slots」候选是否已被拒。

### Q3（成本）与 C1 复核
- 我坚持的「不新增热路径 `QueryCurrentPaletteFrameTag()`」被确认满足（`:20770-20872` 内不读那三个字段）——
  但**这正是它们无法证明本帧的原因**；唯一 per-frame 缓存 `CachedCurrentPaletteFrameTag`（`contract.cpp:1081-1091`）
  是文件内静态函数、无头文件声明 ⇒ 复用需先导出。
- C1 修正复核：`liveReady ∧ frameTag!=0` 才 `LiveNative`，其余落 `PacketFallback` —— **更保守、无新谎报** ✅；
  仅「liveReady 真而 tag==0」时 cause 标签不够精确（对取证读者轻微误导），已记入待办。
- 验证者探针：`_falsify_stageE_20260919/probe_Q1Q2Q3.py` ⇒ **CHECKS=19 FAILURES=0**（当前修订）。

### 另：`NoteObjectGone` 重复通知残留 — 据实**关闭**
- 计数只存在于记录器内部（定义 + 4 个自增点 + 宿主用例），**不在** `war3_frame_evidence.cpp` 的导出路径 ⇒ 不上 wire；
- 「路径事实到达却无链可挂」逐次计数**按定义正确** ⇒ 关闭该残留，不视为缺陷。

## 2026-09-19 — 🎯 **首份实机 palette-object 证据**（阶段 E 实机半边）：无人值守外部监视器路径打通

### 装置（此前两次失败后的正确路径）
- **自包含模式需按 Ctrl+Shift+C 触发**（我无法发送组合键，且强杀进程会跳过导出）⇒ 改用**外部监视器**：
  `AutoTest/frame_history_watch.py --pid <pid> --pre 256 --post 4 --minimum-seconds 1.0 --auto-trigger-after 45`，
  环境 `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=0` + `DXVK_WAR3_FRAME_EVIDENCE=1` + `_PALETTE_OBJECT=1` + `_OUTPUT=<dir>`。
  观察者**自己认领本地租约**（`war3_frame_history.cpp:148`）、arm 记录器与历史、按 `--auto-trigger-after` **自动触发**、
  等 `state==6` 后导出 ⇒ **全程无需按键、无需人工** ✅。
- 运行手册：`E:\Work\warvk-live-E\run_live_E3.ps1`（含前置哈希校验、只操作站点树、只关闭自己启动的 PID）。
- 观测到的状态序列：`1 → 2`（缓冲）→ "Ready: at least 1.00 seconds buffered" → `5 → 6`（采集完成）→ 导出 ✅。

### 实机事实（`cpu-events.json` 头部，权威）
```
accepted=4672782 evicted=4460490 capacity=262144 captureComplete=false
effectiveConfiguration={frameEvidence:true, localRecorderOwner:false, paletteObjectEvidence:true, skinPaletteContract:true}
paletteObject={version:4, watchCount:0,
  closedObservationClosed:64, closedRecovered:0, closedObjectGone/WindowExpired/TableFull/EventLost/Unclosed:0,
  emitted:3648, terminalEmitted:64, firstSightEmitted:64, firstSightInserted:64,
  droppedPerFrame:9462, droppedPerSession:1097578, droppedDuplicatePerFrame:291649,
  droppedTableFull:0, droppedTerminalReserve:0, droppedProbeLimit:0, ringEvictedAfterRecord:0,
  weakIdentityRecords:64, epochUnknownRecords:64 }
producerLosses=0 processId=22356 processNonce=466842777145
```
- **生产写方实机输出 `version=4`** ✅；`paletteObjectEvidence=true`；格式/envelope 校验 `schemaValid=true`、`formatVersion=4`。
- **新增终态 `ObservationClosed=7` 真的在用**（64 条），且 **`closedRecovered=0`** ⇒ 实机印证「Observation 永不使用 Recovered」✅。
- 恒等式成立：`closedCounterSum == terminalEmitted == exportedTerminalCount == 64` ✅。
- **预算丢弃在 wire 上可见**（`losses={droppedPerFrame:9462, droppedPerSession:1097578}`）⇒ 反例 A「非静默」在实机一致 ✅。
- 该次运行 `skinPaletteContract=true` ⇒ C1 报告的「默认构建无新鲜度检查」在**本次采集**中不成立（该门是开的）；
  但 C1 的**修复**针对默认关闭的构建仍然必要 ✅。

### ⚠️ 据实边界（不得越过）
- **`captureComplete=false`**，且 `evicted=4,460,490 / accepted=4,672,782`（≈95% 被环淘汰）⇒ 这是**部分观察**，**不是**完整事件。
- `weakIdentityRecords=64`、`epochUnknownRecords=64`，注册身份证明种类仅 `[0,1]` ⇒ 这 64 条链**都没有强身份证明** ⇒
  **「观察完整不代表对象已证明」**：不得据此声称对象已证明，更不得声称阴影已恢复。
- 隔离/可见桌面数据（本机隔离桌面**不可用**，用可见桌面 + 夜间窗口）**不得**当前台性能数据。
- **不**声称阶段 E 完成；**不**声称全门禁通过；**不**晋升稳定候选。
- **C2 实机锚点未取到**：验证者建议的 `semanticSceneLivePaletteRefreshMissCount` 等计数器**不在**导出的
  `manifest.json`/`cpu-analysis.json`/`history-analysis.json` 中（它们在诊断聚合里，不随本导出）⇒ C2 的实机可达性**仍未判定**，
  该项保持**未闭合**。

### 产物（供复核）
- `E:\Work\warvk-live-E\evidence3\cpu-22356-466842777145-1.json`（117 MB）、
  `…\history-22356-466843239090-1\cpu-events.json`（135 MB）、`cpu-analysis.json`（196 KB）、
  `history-analysis.json`（50 KB）、`capture-identities.json`（12.8 KB）、`manifest.json`（38 KB）、97 张 `slot-*.tga`；
- 站点 DLL = `7120104ECE8ED1581958E113869E967D905A9485DD1DD05DA987264C5E97BCA9`（C1 修正版），地图 `D9230E04…`。
- palette-object 判读：`E:\Work\warvk-live-E\analyze-palette-object-live.txt`（4791 行）。

### 实机产物的**读方判定**（同一份 `analyze-palette-object-live.txt`，读方 version-gated 规则）
```
schemaValid=true formatVersion=4 paletteObjectEvidence=true objectEvidencePresent=true emptySample=false
objectCount=64 eventCount=64 chains=64
coverageComplete=false  chainMissing=true  uncovered=[64 条键]  uncertified=[]  recovered=[]
chain 样本: identityBasis=wireCarriedInstanceLifecycleProof  identityProven=**false**  identityProofKinds=[0]
            eventCount=1  stages=["Drawn"]  windowSegment=1
closedCounters={closedObservationClosed:64, 其余六个恒 0}  wireGaps=[]  producerLosses=0
losses={droppedPerFrame:9462, droppedPerSession:1097578}
missing=[casterMetadataOmitted, "emittedCounterVersusExportedEvents(3648 recorded, 64 exported)",
         objectLevelEvidenceDropped, ringEvictionObserved, unmatchedCpuSpans]
frameEvidenceGaps=[casterMetadataOmitted, unmatchedCpuSpans]
```
- **实机印证**：生产写方 v4 ✅；`ObservationClosed=7` 在用且 `closedRecovered=0` ✅；`recovered=[]`（读方独立确认
  「Observation 永不 Recovered」）✅；预算丢失在 wire 上可见 ✅；`wireGaps=[]`（无接线缺口）✅。
- **实机**同时也说明**不能**据此声称对象已证明：`coverageComplete=false`、`identityProven=false`、
  `identityProofKinds=[0]`、每条链 `eventCount=1`、`stages=["Drawn"]` ⇒ **单事件、弱身份、不覆盖**。
- `emittedCounterVersusExportedEvents(3648 recorded, 64 exported)` 是**环淘汰**造成的部分导出（`evicted=4,460,490`）⇒
  **一次部分观察不能认证一条链**（正是「一条链结算不代表观察完整」）。
- ⇒ **第二次采集计划**：改用**更小的地图**（减少 draw 速率）并把触发提前（`--auto-trigger-after` 8 s、`--pre 64`），
  以降低环淘汰、争取拿到完整观察链；仍未完成前**不**新增实机因果结论。

## 2026-09-19 — 🎯 **实机首次拿到阶段集合完整的观察链**（第三次采集；前两次的根因见下）

### 前两次采集的根因（据实）
- 第 1 次（`--pre 256 --auto-trigger-after 45`，`DRAWS=1`）：环 `accepted=4,672,782 / evicted=4,460,490` ⇒
  导出只剩 64 条 ⇒ 每条链 `stages=["Drawn"]`、`eventCount=1` ⇒ **部分观察**（`emittedCounterVersusExportedEvents(3648 recorded, 64 exported)`）。
- 第 2 次（我把 `--pre` 降到 64 帧）：**从未 ready** —— 高压图 fps 高 ⇒ 缓冲跨度 < `minimum_seconds*1000` ⇒
  `announced_ready` 永不置位 ⇒ 自动触发不发生（**我的参数化缺陷**；已把 `--pre`/`--minimum-seconds` 一并参数化）。
- **洞察**：`emitted=3648` 而导出只有 64 条 ⇒ 丢失在**帧证据环**（被 `DRAWS=1` 的 CPU 边界记录挤满，`accepted=4.67M`），
  **不是** palette-object 记录器本身。

### 第 3 次采集（`DRAWS=0` + `--pre 256 --minimum-seconds 1.0 --auto-trigger-after 3`）
```
accepted=11468 capacity=262144 evicted=0 ✅（完全不再淘汰）
paletteObject: emitted=3648 droppedPerSession=39300 droppedPerFrame=2756
                closedObservationClosed=64 closedRecovered=0 closedObjectGone/…/Unclosed=0
cpu-events.json = 4.4 MB（第 1 次 117 MB）  captureComplete=false
读方: eventCount=3648 chains=64 wireGaps=[] closedCounters={ObservationClosed:64, 其余 0}
      每条链 stages=('Drawn','Enqueued','FirstSight') ← 必需观察阶段集合**完整** ✅
      terminal=ObservationClosed（64/64）✅  identityProven=**False**（64/64）⚠️  identityProofKinds=[0]
      missing=[casterMetadataOmitted, objectLevelEvidenceDropped]
      （"emittedCounterVersusExportedEvents" **已从 missing 消失** ⇒ emitted==exported ✅）
```

### 结论（严格按纪律表述）
- ✅ **实机达成**：palette-object **观察链阶段集合完整**（`{FirstSight, Enqueued, Drawn}` 全部命中）、
  全部以**新终态 `ObservationClosed`** 结算、`closedRecovered=0`（Observation 从不 Recovered）、
  计数恒等式成立、`wireGaps=[]`、`emitted == exported`。
- ❌ **实机未达成（不得越界）**：`identityProven=false`（64/64）、`identityProofKinds=[0]` ⇒ **对象身份未被证明**；
  `coverageComplete=false`/`chainMissing=true` 与 `casterMetadataOmitted` 是**我关闭 CPU 边界记录**（`DRAWS=0`）
  的直接结果（换取环不淘汰）⇒ 这是**权衡**，不是新事实。
- ⇒ 正是纪律的那句话：**「观察完整不代表对象已证明，更不代表阴影已恢复」**；
  本轮**不**新增任何实机因果结论、**不**晋升稳定候选。

### 后续（登记）
- 若要同时拿到 caster 元数据与完整观察链：需在**不淘汰**的前提下重新打开 CPU 边界记录（更早触发/更短会话/更大环），
  或分两次采集（一次要元数据、一次要 palette-object 完整性）。
- C2 实机锚点（`LivePaletteRefreshMissCount` 等）仍不在导出中 ⇒ **未闭合**。
- M3（门禁只锁文本形态）、双轨漂移：**未闭合**。

## 2026-09-19 — 实机链级判定的**权威结果**，以及**撤回我自己的一条错误检查**

### ⚠️ 自我更正（必须记档，避免以后又被当成事实）
- 我先用「语义秩单调」自写检查，对这份实机数据报出「阶段序违规 64/64」❌。
- **该结论错误**：真正的判序规则是**按一次明确关联的尝试（attemptSerial）分组**，而不是整条链上的秩单调；
  实机 `stageOrder` 里 `Drawn` 之后再次 `Enqueued` 正是**新的尝试**。
- **权威判据在读方**：每条链都有 `orderIssues` 字段 ⇒ 实机 64 条**全部 `orderIssues=[]`** ✅。
- ⇒ **撤回**「实机存在阶段序违规」；我的检查忽略尝试号，属方法错误。

### 实机链级判定（读方逐链字段，第 3 次采集）
```
每条链（64/64）:
  stages = ["Drawn","Enqueued","FirstSight"]   stageObservationComplete = true   missingStages = []
  orderIssues = []            terminal = ObservationClosed        servedCandidates = 0
  sawSubmit = true            sawDraw = true                      terminalSource = "Unknown"
  identityWeak = true         epochUnknown = true                 identityProven = false
  lifecycleIdentity = "0"     lifecycleIdentityCarried = true     identityProofKinds = [0]
  covered = false             chainComplete = false               sameObjectCertified = false
  conclusion = "Uncovered"
  certificationRefusals = [identityWeak, epochUnknown, identityNotProven, chainNotCovered, recoveryNotProven]
```
- ✅ **实机达成（结构面）**：观察链阶段集合完整（`missingStages=[]`）、判序干净（`orderIssues=[]`）、
  终态唯一（每键 1 条终态）、**无任何观察链携带 `Rejected` 阶段**、**无以 `Recovered` 结算**、`wireGaps=[]`、`emitted==exported`。
- ❌ **实机未认证（身份面）**：`conclusion="Uncovered"`，且五个**具名拒绝理由**：
  `identityWeak`、`epochUnknown`、`identityNotProven`、`chainNotCovered`、`recoveryNotProven`。
- ⇒ 这正是纪律要求的区分被**实测出来**了：**观察完整 ≠ 对象已证明 ≠ 阴影已恢复**。
  `recoveryNotProven` 也说明：把观察链当成「恢复」是无效的（本采集 `closedRecovered=0`）。

### 本轮另开：实机产物与上述结论的**独立验证子线程**
- 子线程 `a991aef6`（后台）：要求它独立重跑读方、复现数字，并专门证伪「stage 集合完整 / 无 Rejected / 终态唯一 / 无 Recovered / 真的没有 RejectionRecovery 链」，
  且允许它判我错、并要求它指出我结论中被夸大或不可推广之处。

## 2026-09-19 — **双轨漂移收敛**（验证者设计注记的处置）：D 载荷与 S 点共用同一个 live 事实

### 问题（验证者 2475360e 指出）
- S 点（`device.cpp:22160-22161`）用 `paletteObjectSelectionFromLiveNative`；而我在 `:20884-20885` 的载荷原因
  用 `liveRuntimeGroupPaletteReady` **二次推导** ⇒ 两轨**推理等价但无门禁锁住不漂移**。

### 处置（单一来源）
- D 载荷条件改为 `(paletteObjectSelectionFromLiveNative && selectedPalette.frameTag != 0u)` —— **消费 S 点同一个变量**，
  不再从 `liveRuntimeGroupPaletteReady` 二次推导 ⇒ 两轨只有**一个**来源，结构上不可能漂移（语义未变：该变量在 `:20873` 即 `liveRuntimeGroupPaletteReady`）。
- 门禁第 8 条同步改为锁定新文本；**新增第 9 条**：`device.cpp` 中**不得**再出现 `(liveRuntimeGroupPaletteReady && selectedPalette.frameTag` 形式。
- **探针**：改回二次推导 ⇒ 门禁 exit 1 具名失败（`… the live fact must be the SAME variable the S point consumes, not a second derivation`），已还原 ✅。

### 验证（本轮实测）
- 静态 **264/0**、meson **Ok 85 / Fail 0**、宿主 **45 passed / 0 failed**、wire **126/0 PASS**；
- DLL 构建 exit 0，新 SHA-256 前缀 `BCF234C85DA5E16BEA32C746`。
- ⚠️ **站点仍是最前一次部署的 `7120104E…`**（本次重构只改原因来源、不改变 D 点判定的取值）⇒
  **已有实机证据对应 `7120104E…`**，不声称对应 `BCF234C8…`；下次采集前需重新部署并记录哈希。

## 2026-09-19 — 独立复核（验证者 a991aef6）**纠正我多处夸大/错误**；并**实机发现一条判序异常链**

### ⚠️ 必须更正的表述（我错了的地方，逐条撤回）
1. **「64 条链 orderIssues=[]」错误** ❌：我只读了 `chains[0]` 就外推到全部。验证者逐链复核（并独立解码 wire，不依赖读方）得 **63/64**：
   反例链 `key=("857217852","0")`（chainType=1、36 条事件、`seq` 1870..11408）第 1 条 FirstSight 后，
   **frame 1536..1539 有 4 条 `Drawn` 出现在首次 `Enqueued`(frame 1540) 之前** ⇒ 读方具名 `orderIssues=["drawnBeforeEnqueued"]`，
   同链另有 refusal `liveDrawnWithoutSubmit`（这 4 条 live Drawn `sawSubmit=0`）。⇒ **撤回「阶段序全部干净」**。
2. **`wireGaps=[]` 不是证据** ❌：它来自读方常量 `lifecycle_identity_carried()`（脚本 `:352-353`），对**任何**输入都返回 `[]`；
   而我拿它当「身份已贯通」的证据 ⇒ **撤回**。事实上全部 3648 条记录 `data[2](lifecycleIdentity)=0`。
3. **「identityProven=false」是恒真属性，不是本次发现** ❌：生产采集点 `MakePaletteObjectKey()` 恒写
   `deviceEpoch=0 / lifecycleIdentity=0 / identityWeak=true / epochUnknown=true`（`evidence.h:150-152` 亦明写生产恒为 `NoIdentityProof(0)`）。
   ⇒ 更强的陈述应改为**键退化**：64 条链的 `runtimeModelPtr` 全 0、`lifecycleIdentity` 全 0、`jHandle` 只有 **55** 个不同值
   （6 个 handle 被 2~4 条链共用），链间只靠 `renderablePart` 区分 ⇒ **终态唯一性是记录器表结构的不变量，不是身份证据**。
4. **`chains=64` / 「阶段齐全」是预算饱和窗口的产物，不是对象普查** ❌：非终态 palette 事件恰 **3584 = kNormalBudget(4096−512)**，
   全部落在 frame 1514..1569（56 帧）、每帧末恰 64 = kPerFrameBudget；**frame 1570..1869（约 300 帧）对 palette 证据完全不可见**
   （写方 `evidence.h:516-526`：预算耗尽后新对象**连条目都不建**）。
5. **`evicted=0` 几乎无信息** ❌：`accepted=11468 ≤ capacity=262144`（后者本身即 schema≥6 的格式上限）⇒ 环从未需要淘汰；
   真正的损失是记录器自身预算（`droppedPerFrame 2756 + droppedPerSession 39300`）；同一次运行的 `manifest.json`（历史/截图通道）报 **`evicted=84`**。
6. **「没有 RejectionRecovery 链」只是导出内容陈述**，不可推广为「本采集没触发过拒绝恢复链」：
   拒绝事件是**非终态**、吃同一预算，且唯一生产调用点（`war3_shadow_renderer_core.cpp:6831`）受 `ShouldNotifyPaletteSlotReject` 门控。
7. **口径**：`cpu-events.json` 实为 **5,357,176 B**；我写的「4.4 MB」其实指 `cpu-7256-470979603758-1.json`（4,393,761 B），
   两者**逐结构完全相等**（仅 JSON 空白差异）⇒ **不构成独立佐证**。
8. 验证者做了**判据活性探针**（改 `counters.emitted` 为 `"3649"` ⇒ 立刻多出 `emittedCounterVersusExportedEvents`；
   把一条 Observation 链 `data[4]` 改为 `0` ⇒ 立刻报 `rejectionRecoveryChainMustNotUseObservationClosed` + `…MustNotCarryFirstSightStage`）
   ⇒ 读方这些判据**不是死代码** ✅（这条对读方质量是正面证据）。

### 🔍 实机发现：一条**判序异常链**（新登记，未闭合）
```
key=(data[0]="857217852", data[1]="0")  chainType=1(Observation)  36 条事件
  seq=1870 frame=1535 FirstSight  attempt=1  sawSubmit=0 sawDraw=0
  seq=1920 frame=1536 Drawn       attempt=2  sawSubmit=0     ← 首条 Drawn 早于首次 Enqueued
  seq=2023 frame=1537 Drawn       attempt=3
  seq=2104 frame=1538 Drawn       attempt=4
  seq=2141 frame=1539 Drawn       attempt=5
  seq=2300 frame=1540 Enqueued    attempt=6
  … 每条事件带一个**不同**的 attempt 号
```
- 读方判据：`orderIssues=["drawnBeforeEnqueued"]` + refusal `liveDrawnWithoutSubmit`。
- **两种可能，尚未判定**：① 同一键确实经由**不经 palette enqueue** 的路径被绘制（`sawSubmit=0` 指向「没有前置提交事实」）；
  ② **尝试关联缺口**（submit 事实的 attempt 号没对上）—— 后者正是本目标 C 项「跨帧判序按一次明确关联的尝试」与 P0-5 的关切。
- ⇒ **登记为未闭合**：需在宿主层构造同形用例（或在生产侧加 attempt 关联的具名计数）才能区分 ①/②；**不得**据此下因果结论。

### 站点哈希自行核验（回应验证者「无法核实」）
- 验证者查的是 **`E:\Work\War3`（禁区玩家树，我从未部署）**= `A0A51AF2BB9091B2…`；
- **实际站点** `E:\Work\Warcraft III\d3d9.dll` = **`BCF234C85DA5E16BEA32C746…`**，与 `build32/src/d3d9/d3d9.dll` **一致** ✅；
- ⚠️ 因此：**本文档前述实机产物对应的是 `7120104E…`（双轨收敛前的构建）**，而站点现已更新为 `BCF234C8…`；
  下次采集前必须重新记录「产物 ↔ DLL 哈希」的对应关系，**不得**把已有产物归到新构建上。

## 2026-09-19 — 实机异常链**判定为「正确的 fail-visible」**，并**钉成两个可失败用例**（分析套件 109 → 111）

### 根因（读方代码为据）
```python
# 链级「首次出现」规则，不按尝试分组：
if i_enqueued is not None and i_drawn is not None and not i_enqueued < i_drawn:
    issues.append('drawnBeforeEnqueued')
live_draws = [e for e in draw_events if e['terminal']=='NoTerminal']
live_drawn_without_submit = any(not e['sawSubmit'] for e in live_draws)   # ⇒ refusal
```
- 该实机链的 4 条 「首条 Drawn 早于首次 Enqueued」事件**同时**是 `sawSubmit=false` 的 live Drawn。

### 判定：**不是写方缺陷，也不是读方缺陷**
- 写方可写出该形状：`NoteDrawn` 只要求条目存在；`sawSubmit` 由 `NoteEnqueued`/`PrepareStage` 置位 ⇒
  **缺提交事实的 live Drawn 可写出**（预算丢失场景：同一次运行 `droppedPerFrame=2756` / `droppedPerSession=39300`）。
- 读方**必须**拒绝认证（`liveDrawnWithoutSubmit` 是具名 refusal）⇒ 这正是 fail-visible 的正确行为。
- 澄清：目标 C 项「跨帧判序按一次明确关联的尝试」是**写方**的跨帧判序（attemptSerial/maxStage）；
  读方的 `drawnBeforeEnqueued` 是**链级首次出现**的保守健全性规则，两者**不冲突**。⇒ 该异常**闭合**（登记为已解释），
  但「该对象为何缺提交事实」仍需实机/宿主进一步证据才能归因（**不下因果结论**）。

### 钉成用例（`AutoTest/test_palette_object_evidence_analysis_static.py`，109 → **111 tests OK**）
- `test_live_draw_before_enqueue_is_flagged_and_refused`：实机形状必须被 `drawnBeforeEnqueued` 标记、
  必须带 `liveDrawnWithoutSubmit` refusal、且 `sameObjectCertified == False`。
- `test_control_ordered_chain_has_no_order_issue`：对照（首次 Enqueued 在前、所有 live Drawn 带 `sawSubmit`）⇒ 两条规则都保持沉默。
- **可失败性探针**：① 把实机形状改成有序 ⇒ `AssertionError: 'drawnBeforeEnqueued' not found in ['deltaFramesMismatch@6']`，FAILED ✅；
  ② 把 live Drawn 的 `saw_submit` 强制 True ⇒ `AssertionError: 'liveDrawnWithoutSubmit' not found …`，FAILED ✅；均已还原。
- 全量静态：**264/0** ✅。

### 过程中我犯并自查出的小错（记录以免复现）
- 我先把新测试类**追加到 `unittest.main()` 之后** ⇒ 那两个用例**静默不跑**（套件仍报 109，我按计数发现）❌；
- 随后一次重排**把 `main` 块复制了一份** ❌ ⇒ 按结构重建修好；
- 夹具三次报错都是我写错：`chainSequence`（`words32[23]`）未按 1,2,3… 设置、`window_segment`/`chain_type` **只在 v4 合法**（须 `format_version=4`）、
  以及 `firstSightEmitted=0` 与导出里 1 条 live FirstSight 矛盾（须 `counter_overrides`）⇒ 均按读方自己的报错定位修正 ✅。

## 2026-09-19 — C2 实机锚点**已能导出**（机制成立），但轻量地图上路径**惰性**（全 0 ⇒ 无信息）

### 机制（已验证可用）
- 控制面 action **`get_shadow_runtime_summary`**（`war3_control_plane.cpp:4783` → `QueryShadowRuntimeSummary` + `ToJson(summary)`）
  返回含 C2 锚点的 summary；`get_runtime_status` **不含**这些计数器（我先前试错了这一点）。
- `E:\Work\warvk-live-E\dump_runtime_status.py` 现在同时取 `get_runtime_status` 与 `get_shadow_runtime_summary`；
  第 5 次采集实测 `anchor_hits=14`（`/result` 与 `/response/result` 两个路径各 7 项）✅。

### 第 5 次采集（地图 `Maps\(4)Adrenaline.w3m`，DLL `BCF234C8…`）实测
```
semanticSceneLivePaletteRefreshAttemptCount = 0
semanticSceneLivePaletteRefreshHitCount     = 0
semanticSceneLivePaletteRefreshMissCount    = 0
semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount = 0
semanticSceneDirectPartLeaseRestoredCount   = 0
semanticSceneShadowManifestPartLeasePaletteRefreshAppliedCount = 0
semanticSceneDirectPartLeaseRejectedNotSelfContainedCount = 0
```
- `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` **默认为 1**（`war3_live_palette_selection.cpp:36-41`）⇒ 开关不是原因；
- `AttemptCount=0` 说明 `:20787` 的守卫 `skinned && War3SemanticLivePaletteRefreshRuntime()` 从未为真 ⇒
  **该地图在这个采集窗口内没有蒙皮 draw** ❌。
- ⇒ 这份全 0 是**无信息的负结果**（既不能支持也不能否定 C2）；**不得**当作「C2 不存在」。
- 记录：该次产物 `history-40092-478720869250-1`（`cpu-events.json` 5,665,499 B）**绑定 DLL `BCF234C8…`** ✅（首次做到产物↔构建哈希绑定）。

### 下一步
- 换**高压图**（`Maps\ShadowTest\光影测试(高压).w3x`，已知大量 caster）重跑同一配方 + 锚点 dump ⇒ 期望 `AttemptCount>0`，
  再看 `MissCount` 与 `Restored/Applied` 是否同帧共现（验证者给出的 C2 实机判据）。

### 第 6 次采集（高压图 `Maps\ShadowTest\光影测试(高压).w3x`）—— 锚点**仍全 0**
```
semanticSceneLivePaletteRefreshAttemptCount = 0   （其余 6 个锚点同样 0）
产物: history-7292-479408287904-1  cpu-events.json 7,901,925 B（绑定 DLL BCF234C8…）
```
- `shadowStats` 是**累计**结构（全树**未找到**显式复位点；`war3_perf_monitor.cpp:1524` 亦按累计聚合）⇒
  全 0 **不是**「查询时机在帧复位之后」造成的；
- ⇒ 合理的解释只剩：**这两张地图 + 采集窗口内没有满足 `skinned` 守卫的 draw**（`:20787` 要求 `skinned && …`）⇒
  即 **C2 的状态是「场景未覆盖」，不是「C2 不成立」**。
- **不**据此下任何 C2 结论；C2 的实机判定**仍未完成**（需要蒙皮单位参与的场面，例如带英雄/召唤物的实战地图与更长的观察窗口）。

### 本轮状态小结（据实）
- ✅ 已具备：C2 锚点的**导出机制**（`get_shadow_runtime_summary`）+ 产物↔DLL 哈希绑定（`BCF234C8…`）。
- ❌ 未完成：C2 的实机判据（`MissCount` vs `Restored/Applied` 同帧共现）——场景未触达。
- 其余未闭合项不变：M3（门禁只锁文本形态）、「该对象为何缺提交事实」的归因、C2。

## 2026-09-19 — **M3 加固**：门禁从「子串存在」提升为「条件恰为该合取式 + 三元块内不得出现析取」

### 问题（验证者 2475360e 的 M3）
- 旧断言只要求那段文本子串**存在** ⇒ 把条件改写成
  `… && selectedPalette.frameTag != 0u || !paletteObjectSelectionFromLiveNative`（**恒真**）后，
  原文本子串**仍然存在** ⇒ 门禁照旧 PASS（只锁文本、不锁语义）。

### 处置
- 门禁新增第 10 条：
  (a) 条件必须**完整一行**地恰好等于 `      (paletteObjectSelectionFromLiveNative && selectedPalette.frameTag != 0u)`；
  (b) 承载原因的三元块（从 `PaletteObjectDrawCause paletteObjectDrawCause =` 到 `PacketFallback;`）内**不得出现析取运算符**。
- **探针（正是验证者用的那个变异）**：加上 `|| !paletteObjectSelectionFromLiveNative` ⇒ 门禁 exit 1，
  并报**两条具名失败**（C1 那条 + 新的 M3 那条），已还原 ✅。

### ⚠️ 诚实边界（不得夸大）
- 这仍然只是**文本形态**检查：换一种写法（合取拆两行、等价布尔重排）仍可能绕过 ⇒
  M3 只是「**已知绕过被关闭**」，**不等于「语义已被证明」**。语义证明仍需端到端行为证据（如 D 点运行时取值判据）。

## 2026-09-19 — E 项「五个点位运行时确认」进展；C2 定性为**场景未覆盖**并停止追打

### C2：确认是「场景未覆盖」（不是「C2 不成立」）
- `g_shadowSceneStats` 是**独立全局**（`war3_shadow_runtime_bridge.cpp:977`），由场景统计**合并发布**（`:4565`，读 `previous`）
  ⇒ 属**累计**量；`semanticSceneLivePaletteRefreshAttemptCount` 的自增点唯一（`d3d9_device.cpp:20788`，守卫 `skinned && …`）。
- 4 次采集（2 张地图 × 窗口 3 s / 6 s / 90 s）该量**恒 0** ⇒ 该守卫从未为真 ⇒ C2 的实机判据**场景未触达**。
- ⇒ **停止在本会话继续追打 C2**（它是阶段 C 审计的**残留项**，不在本目标的条目清单内）；登记为「需蒙皮场面 + 更长窗口」。

### E 项运行时确认（新证据，长窗口产物 `history-19944-480090261422-1`）
```
chains = 64，unknownFrameDomains 分布 = {(): 64}   ⇒ 无任何链把 native 域标为未知
含 native 未知的链数 = 0                          ⇒ D 点写出的 nativeKnown 全为 true
refusals 样本 = [identityWeak, epochUnknown, identityNotProven, chainNotCovered, stageStreamTruncated, recoveryNotProven]
```
- 在我 C1 修正后的条件下（`cause==LiveNative` **且** `selectedPalette.frameTag != 0`）⇒ `nativeKnown=true` 说明
  **`LiveNative` 这一支在实机确实被走到**（这正是「D 点原因按值携带」的可用性证据）。
- 注意区分：这与 C2 的 `semanticSceneLivePaletteRefresh*`（**蒙皮 lease 刷新**路径）是**两条不同路径** ——
  前者由 draw-time captured palette 供数、后者由 skinned 刷新供数；前者实机可达、后者本次不可达。
- ⚠️ **未触达**：`frameTag==0 ∧ liveReady`（我 C1 修的**谎报组合**）在本次采集中**没有出现** ⇒
  **C1 修复目前只有宿主/静态/探针证据，尚无实机证据**（不得声称已在实机验证）。
- ⚠️ 本次还出现 `stageStreamTruncated`（长窗口 + 预算饱和导致的截断）⇒ 与既有结论一致：预算封顶决定可见窗口。

### 本轮另：M3 加固
- 门禁第 10 条把「子串存在」提升为「条件恰为该合取式 + 三元块内不得有析取」；
  验证者的恒真变异现在**被具名判出**；仍只是文本形态检查（边界见 M3 条目）。全量静态 **264/0**。

## 2026-09-19 — E 项**五点位运行时可达性表**（据实：可达/可达未赋值/未达且已解释/本导出不可观测）

数据来源：长窗口采集 `history-19944-480090261422-1`（DLL `BCF234C8…`，地图 `光影测试(高压).w3x`）的
`get_shadow_runtime_summary`（1689 字段）与 palette-object 读方判定。

| 点位 | 运行时计数器 | 判定 |
| --- | --- | --- |
| `:20321` 选择来源（packet vs currentDraw） | `currentDrawCapturedPaletteQueryAttemptCount=380` / `HitCount=380` | 可达 ✅ |
| `:20452` 回退到 `rebuildSelection` | `submitLiveRebuildAttemptCount=380` / `MissCount=380` | **被求值但从未赋值** ⚠️ |
| `:20865-20870` live native vs packet fallback | 7 个锚点**全 0**；且 `semanticSceneShadowMapSkinnedCasterCount=0` | **未达，且已正面解释**（零蒙皮 caster）❌✅ |
| `:21694-21697` UV/几何 override 分支 | 无专用计数器；`semanticScenePaletteOverrideNoCompose/WouldCompose=0` | **本导出不可观测** ❓ |
| `:23299-23530` skip 路径（非 caster / alpha-test） | `semanticSceneShadowCastersCount=262`、`ShadowMapDrawnCasters=1048`；专用 skip 计数器不在本 summary | **部分可观测** ⚠️ |

### 由此得到的两个**实质结论**
1. **C2 的零锚点已获正面解释**：`semanticSceneShadowMapSkinnedCasterCount = 0` ⇒ 本会话**没有任何蒙皮 caster** ⇒
   `:20787` 的守卫 `skinned && War3SemanticLivePaletteRefreshRuntime()` 结构上不可能为真 ⇒ C2 的实机判据在本场景**不可达**（不是「C2 不成立」）。
   要触达它必须让场景真的画出蒙皮单位（例如带英雄/召唤物的实战地图 + 更长的游戏进行时间）。
2. **D 点原因按值携带的可用性证据成立**（64/64 链 native 域非未知 ⇒ `cause==LiveNative` 且帧标签非零）✅；
   但 `frameTag==0 ∧ liveReady`（C1 修的谎报组合）**本次未出现** ⇒ 该修复仍**只有宿主/静态/探针证据**。

### 其它要点（据实）
- `paletteCaptureInvalidEntryMissCount = 1,269,034`、`currentDrawContractPublishSkippedNonWorldContext = 1,478,115`：
  大量无效条目/非世界上下文跳过 ⇒ 与「预算与准入决定可见窗口」的既有结论一致，**不**据此下新因果结论。
- `semanticSceneDirectSelectionLeaseActiveKeyCount = 95` = `SubmittedKeyCount = 95`（lease 有提交）但 refresh 锚点全 0 ⇒
  说明 lease 提交路径与 **skinned 刷新**路径是**两条不同路径**（前者有活动、后者未触达）。

## 2026-09-19 — 阶段 E 交付包建成 + 五阶段收口核对

### 交付包
- `E:\Work\WarVK-delivery-20260919-stageE-r1.zip`（692,092 B，SHA-256
  `1DEF036EAA4D9682787B96AD84DB5E8CA86F791A7D1E44C290037F39CA055948`，13 条目）
  内含 `VERIFICATION.md`（范围/改动/门禁/实机/未闭合/边界/可推翻条件）、`MANIFEST.txt`（逐文件 SHA-256）、
  `files/`（3 个 C++ 源 + 4 个门禁/测试/脚本）与 `live/`（运行手册、dump 脚本、锚点产物）；
- 暂存目录：`E:\Work\WarVK-stageE-r1\`。

### 阶段收口核对（固定顺序 A→E，每阶段单独出包）
| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| A | 文档更正版 `WarVK-delivery-20260918-doc-r1`（内层四指纹逐项未变、外层新 SHA）| 完成 |
| B | 门禁恢复（`check_scenario_g` 移出注释、`REQUIRED_SCENARIOS={A..G}` 硬条件、5 个可失败性探针）| 完成 |
| C | 链型与 v4（十项：双链独立不可改类、键×链型×窗口、CanEmit→Insert、attemptSerial 按值、`ObservationClosed=7`、Observation 永不 Recovered、恒 v4、按版本校验、误拒不变量、写读测同步、两项具名交付）| 完成（残留登记）|
| D | 会话发布·关闭·冻结·快照协议（D1 单同步域、D2 arm 顺序、D3 单快照 HeaderJson、D4 三条屏障）| 完成（4 条探针全部成立）|
| E | D 点原因**按值携带**（四个真实分支 + D 点消费）、五点位覆盖、白名单采集与完整启动环境保存 | 代码/门禁/探针/实机可达性完成；实机半边为一次完整观察链，场景未覆盖项已获正面解释 |

### 硬约束遵守情况（逐条）
- 两个 P0（B/C 与 D）**均已收口** ⇒ 门槛满足；即便如此，本会话**仍未**新增实机因果结论、**未**晋升稳定候选；
- 不声称全门禁通过（门禁数值均为本会话实测）；
- 未使用任何 git 写操作；未触碰 `E:\Work\War3`（玩家树 SHA `A0A51AF2…` 全程未变）；
- 未放宽任何判据（所有新增门禁都带可失败性探针）；
- 未改变 palette 准入 / 几何选择 / 渲染优先级（E 的改动只在**诊断载荷**与 D 点取证，`cause` 不参与准入）；
- 未以关闭动态 caster 或减少正常阴影作为任何修复成功的依据。

### 实机环境遗留（供用户处置）
- 站点 `E:\Work\Warcraft III\d3d9.dll` = `BCF234C8…`（源码与最新构建相同、字节不同，因构建不可复现）；
- 备份：`d3d9.dll.F275545B_backup_20260918-212247`（= 基线 `F275545B…`）、`d3d9.dll.69379AB4_backup_*`、`d3d9.dll.7120104E_backup_*`；
- 回退：把 `d3d9.dll.F275545B_backup_20260918-212247` 复制回 `d3d9.dll` 即可；
- 站点内 7 个 `d3d9.dll.locked_*`（更早遗留）**未删除**（无明确要求不动）。

## 2026-09-19 — 独立验证者（9979c467）**证伪 M3 加固并纠正五点位表**；插队修复与更正

### 一、M3 第一版**被证伪**（我错了，据实撤回）
验证者在仓库外镜像上实测（每条都真跑门禁）：
| 变异 | 形态 | 旧门禁 |
| --- | --- | --- |
| p1 朴素析取 | 同行追加 `|| !paletteObjectSelectionFromLiveNative` | exit 1（判出）|
| **M1** | 同行追加 `| true`（token 不是 `||`）| **exit 0 绕过** |
| **M2** | 条件前插注释内含 `PacketFallback;`（截断块扫描）| **exit 0 绕过** |
| **M3** | 条件提为 helper，三元只用裸标识符 | **exit 0 绕过** |
| **M4** | 更早处插注释同时含声明文本与 `PacketFallback;` | **exit 0 绕过** |
根因：① 我当时仍只做**裸子串**检查（不做整语句锚定）；② 用两次 `str.find` 定界（注释可挪起点/提前终点）；③ 只禁字面 `||`。
验证者还用 g++ 跑真值表实测语义：`| true` 才是**真正恒真**（allTrue=1），而我当时称作恒真的 `(X&&Y)||!X` 其实等价于 `!X||Y`（allTrue=0）——**我的描述不准确，撤回**。

### 二、M3 重做（机制层面）
- 新门禁：**(a) 先剥掉注释**（行注释与块注释），使注释诱饵无法参与定界或截断；
  **(b) 用精确正则要求整条语句恰为那个内联合取三元**（条件必须内联、不得是裸标识符、不得追加任何运算符，含 `|`），且全文件恰好出现一次。
- 修复过程中我自己又踩了两个坑（记录）：正则字符类写成 `[A-Za-z_:]*` **不含数字** ⇒ 匹配 `War3ShadowCasterDraw` 失败 ⇒ 改为 `[\w:]*`；
  以及在 JS 里替换时多写一层反斜杠，生成 `[\\w:]*`（Python raw 串下等于字面反斜杠）⇒ 复验 baseline 才发现。
- **五个对抗探针（全部达标，已还原）**：
```
p0_baseline          exit=0  （正确放行）
p1_naive_or          exit=1  具名判出
p2_pipe_true         exit=1  具名判出   ← 旧版曾 exit 0 绕过
p3_decoy_comment     exit=1  具名判出   ← 旧版曾 exit 0 绕过
p4_helper_bare_id    exit=1  具名判出   ← 旧版曾 exit 0 绕过
p5_decoy_before      exit=1  具名判出   ← 旧版曾 exit 0 绕过
```
- 全量静态：**264/0**。⚠️ 诚实边界不变：这仍是**文本/结构形态**检查；更强语义保证仍需端到端行为证据。

### 三、五点位表**逐条纠正**（验证者逐条复现；旧表见本文件上一日期条目，**以本条为准**）

| 点位 | 旧表 | 更正后 |
| --- | --- | --- |
| `:20321` | 可达（380/380）| **降级**：这两个计数在 `war3_current_draw_contract.cpp:1341/2957`（capture 侧）递增，**不在** `:20321`；
  只能证明 capture 路径活着，**不能**证明该处第 2/3 分支被取。另 bridge 还算了 `MissNoContract/InvalidCount/NoSnapshot/UnreadablePalette`，但**只导出了 Attempt/Hit** |
| `:20452` | 被求值但从未赋值 | **成立且加强**：另有 `submitLiveRebuildHitCount=0`、`submitLiveRebuildAppliedCount=0`（Hit 在 20437、Applied 在 20458 同分支触发）⇒ 三条为 0 才无懈可击 |
| `:20865-20870` | 7 个锚点全 0 | **锚点数应为 `6`**（Attempt/Hit/Miss/LastMatrixCount/LastMatrixHash/LastRuntimeModelPtr）；
  且**行号漂移**：当前树该处是注释块，对应代码在 `20887-20898`，refresh 计数在同函数更早的 `20788/20833/20851` |
| `:21694-21697` | 不可观测（据 `PaletteOverrideNoCompose/WouldCompose=0`）| **范畴错误，撤回**：真实站点是 `:21724-21727`；
  我引用的两个计数在 `18575/18584`，属**另一个函数** `War3GetOrCreateSemanticShadowPalette`（18546 起），观测不到该站点（`War3TryAppendSemanticShadowPacket` 19862 起）。
  「不可观测」结论**侥幸成立**，真正理由是该站点触碰的 `drawTimeVBCacheConsumeHitCount`/`ConsumeMissCount`/`skippedAlphaTest` **一个都没导出** |
| `:23299-23530` | 部分可观测 | **升级为直接可观测**：`semanticSceneDrawTimeProducerSubmittedSkinnedCount=115`（全树**唯一**递增点 23518 在区间内）、
  `semanticSceneRejectedPathBlockerProducerCount=24`（区间内专用 skip 计数、已导出、非零）。⚠️ `semanticSceneSubmitted*` 另有 22479/27442 两个递增点，**非站点唯一** |

其余穷举结论（验证者）：95 个字段匹配 `override`，其中只有 `NoCompose/WouldCompose` 一对是 `PaletteOverride*`（皆 0、且属错误函数）；44 个匹配 `skip`，
非零者 `currentDrawContractPublishSkippedNonWorldContext=1478115`、`drawTimeSemanticProducerOwnedDirectGroupedSkipCount=176`、`semanticManifestCopySkipStableCount=12257`。
⇒ **没有任何字段能证明 `:21694-21697` 可达**；而 `:23299-23530` **有**（见上）。

### 四、C2「零锚点」的解释**更正**（我此前的因果解释不成立）
- 我此前用 `semanticSceneShadowMapSkinnedCasterCount=0` 解释 `MissCount=0` ⇒ **不严格**：该字段与 `packet.path == Skinned` 是**不同谓词**
  （写点 (i) 按 `draw.vertexBlendEnabled` 计且 5867-5868 是 **latched-if-zero**；写点 (ii) `d3d9_war3_shadow.cpp:9659` 无条件取协调值）。
- **正确的同谓词同函数字段是 `semanticSceneAppendEntrySkinnedCount = 0`**（`device.cpp:20132-20133`，紧接 20127 的 `if (skinned)`）⇒ 它=0 才直接证明该守卫从未进入。
  **残留缺口**：20127 之前若有提前 return，该计数覆盖不到。
- **未排除的替代解释**（如实登记）：① 采集时 env gate 被显式设为 0（summary 无法区分）；② 该字段**双重语义**（latched-if-zero vs 无条件覆盖）。
- 验证者**复现并排除**了「读错对象」：`m_war3Scene.shadowStats` 类型即 `War3ShadowCaptureStats`，`NoteShadowSceneStats` 在 bridge:4220 以 `merged = stats` 整体覆盖 `g_shadowSceneStats`，
  4222-4565 回填白名单**不含** refresh 字段 ⇒ 计数确实能流通到导出。
- 另：`semanticSceneSubmittedSkinned=115` 是按 `objectKind == Unit` 计（23512-23514，注释明说不把它当作已证明蒙皮），与 `packet.path == Skinned` 是不同谓词，**不**与 `AppendEntrySkinnedCount=0` 矛盾（验证者先疑后排除）。

### 五、方法论更正
- 我记录里的点位行号取自**旧 revision**（已漂移）⇒ 今后结论必须绑定**实测时的 revision/哈希**，不得只写行号。

### 六、E 交付包**重建**（r1）
- `E:\Work\WarVK-delivery-20260919-stageE-r1.zip` = 692,395 B，SHA-256
  `D4DF89224D4DFA236956D6FAC848C3DB39312791E05072A75166633073A8D6F1`（**取代**初版 `1DEF036E…A055948`）；
- 内含更正后的 `VERIFICATION.md`（新增 §0 修订记录、更正五点位表与 C2 解释、M3 五个探针）与重做后的门禁脚本
  （`test_palette_object_native_reason_static.py` SHA-256 前缀 `6BB4E263…`）；
- 全量静态复验 **264/0** ✅。

## 2026-09-19 — 第二轮证伪（验证者 c3d5f591）：M3 重做版**仍可绕过**；已再加固并**明确机制边界**

### 一、第二轮实测反例（我被证伪的部分，全部据实记录）
| 反例 | 形态 | 重做版（第 10 条）| 现版 |
| --- | --- | --- | --- |
| N0 | 真三元之后加一行无条件赋值 `paletteObjectDrawCause = …::LiveNative;` | exit 0 ❌ | **exit 1 具名** ✅ |
| N2 | 在 `bool paletteObjectSelectionFromLiveNative = liveRuntimeGroupPaletteReady;` 后加 `paletteObjectSelectionFromLiveNative = true;` | exit 0 ❌ | **exit 1 具名** ✅ |
| N3 | 把真实初始化式改成 `= true;`，原文只留在**注释**里 | exit 0 ❌ | **exit 1 具名** ✅ |
| N1 | 真条件放宽为 `… != 0u) | true`，把语句原文搬进**字符串字面量** | exit 0 ❌ | **exit 1 具名** ✅ |
| N1c | 真条件换成常量 `true`，同上字符串诱饵（此时真实代码完全不消费合取式）| exit 0 ❌ | **exit 1 具名** ✅ |
| N1b | 诱饵放 `#if 0` 死代码（预处理指令不是注释）| exit 0 ❌ | **exit 1 具名** ✅ |
| N1d | 诱饵放未使用的 `#define` 宏体 | exit 0 ❌ | **exit 1 具名** ✅ |
验证者还用 g++ 真值表**实测**语义：ORIGINAL 无谎报；N0/N1/N1c 各三行谎报；**N2 在 `(live=0, tag=0x2A)` 一行谎报** ——
即「packet 回退的非零 packet tag 被当成 native 帧已知」，正是 C1 要防的那件事。

### 二、本轮加固（三条新措施，全部带探针）
1. **第 11 条 (a)**：`LiveNative` 只允许被赋值**一处**（防 N0 的覆盖）。
   ⚠️ 我第一版写成「`paletteObjectDrawCause` 恰好一次」⇒ **误报**（device.cpp 合法地有两处赋值：声明处三元 + 陈旧分支 StaleRefresh），
   靠复跑 baseline 才发现 ⇒ 改为只约束 LiveNative 那一处。
2. **第 11 条 (b)**：live 事实**不得被赋常量**（`= true;`/`= 1;`），防 N2/N3。
3. **非代码文本剥离升级**：`_strip_comments` → `_strip_noncode`，一并剥掉**字符串字面量**、字符字面量、`#if 0 … #endif` 区域与其余预处理行 ⇒ 关掉 N1/N1c/N1b/N1d。
**九个探针现状**（均已还原）：baseline exit 0；N0/N1/N1b/N1c/N1d/N2/N3 与上一轮四类（`| true`/注释诱饵/helper 提权/前置诱饵）**全部 exit 1 且具名**。
全量静态 **264/0**。

### 三、⚠️ 机制边界（验证者的结论，我接受并写死在此）
- 第 10/11 条合起来**只能保证**：文件中恰好存在一处、未被注释/字符串/预处理文本包裹、文本上严格等于该内联合取三元的**源码文本**（仅此而已）。
- **不能保证**：该文本就是被执行的赋值；也**不能保证** `paletteObjectSelectionFromLiveNative` 本身仍来自 `liveRuntimeGroupPaletteReady`。
- 未闭合的同类绕过（据实登记，不再以文本加固追打）：例如把 live 事实改成由**函数调用**赋值（`= SomeRuntimeCheck();` 返回真），
  或者把诱饵放到**另一个文件**（门禁只读 `d3d9_device.cpp`）。⇒ **文本门禁无法证明语义**。
- **真正的语义闭合路径**（需用户/设计决策，涉及热路径成本）：把「本帧币值证明」（`capturedPaletteCurrentFrameProven` / `producerPaletteCurrentFrameProven`）
  真正接进原因判定，或先导出 `CachedCurrentPaletteFrameTag` 的每帧缓存 —— 二者都不是纯文本改动，且后者会引入每 draw 原生读（验证者 Q3 已指出）。

### 四、E 交付包 r2
- `E:\Work\WarVK-delivery-20260919-stageE-r2.zip` = 694,455 B，SHA-256
  `8C3056F599E025E75E03A8FD467502DA0FA0121A444B0E5865975AC9058FBADE`（13 条目），
  含 `VERIFICATION.md` §7（第二轮证伪反例表 N0/N1/N1b/N1c/N1d/N2/N3、r2 三条加固、机制边界）
  与加固后的门禁 `test_palette_object_native_reason_static.py`（SHA-256 前缀 `55BB8168…`）；
- 前一版 `…-stageE-r1.zip`（`D4DF8922…`）与初版（`1DEF036E…`）保留在磁盘上，**以 r2 为准**；
- 全量静态 **264/0**；baseline 与九个对抗探针结果见 `VERIFICATION.md` §2/§7。

## 2026-09-19 — 上级裁定后的**零新增原生读取诊断修复批次**（M3 从「锁文本」转为「执行语义」）

### 一、裁定的问题（我确认成立）
- 旧实现：S 点 `nativeKnown = !override && paletteObjectSelectionFromLiveNative`，D 点 cause 由
  `(paletteObjectSelectionFromLiveNative && selectedPalette.frameTag != 0u) ? LiveNative : PacketFallback` 得出 ⇒
  `liveReady=true ∧ tag=0` 时 **S 说已知、D 说回退**（两轨分叉），且真正的 packet 回退分支
  （`if (!liveRuntimeGroupPaletteReady) selectedPalette = packet.paletteSelection;`）**根本未执行**却被记成回退。
- 一个枚举同时承载「动作 / 来源与本帧新鲜度证明 / 数据字段」三件事 ⇒ 不成立。

### 二、本批实现（只改诊断，不新增任何原生内存读取）
- 新自包含头 `src/d3d9/war3/render/war3_palette_object_diagnostics.h`：
  `DrawAction`（Unknown/LiveNativeSelected/PacketFallbackSelected/StaleRefreshObserved/NativeOverrideCleared）、
  `FrameEvidence`（来源 + `checkExecuted` + `checkPassed` + **覆盖的 Selection 身份**）、
  `SelectionIdentity`、以及**唯一**解释规则 `Verdict(...)`；交接辅助 `NoteLiveNativeSelected / NotePacketFallbackSelected /
  NoteStaleObserved / NoteFrameEvidence / NoteSelectionReplaced / NoteOverrideCleared`。
- `d3d9_war3_scene.h`：`paletteDiagnostics` 改为按值携带整份 `palette_object::Diagnostics verdict`（删掉过载枚举）。
- `d3d9_device.cpp`：动作只在**真正执行**的分支记录（live 选择后、packet 替换处、陈旧分支内、清空分支内）；
  **证据只在既有检查本来就已经执行的位置取用它当时的结果**（`capturedPaletteCurrentFrameProven`、
  `IsSkinPaletteSelectionCurrent`）；`selectedPalette = rebuildSelection;` 之后调用 `NoteSelectionReplaced` ⇒
  **A 的证明不得被 B 继承**；S 点改为消费 `Verdict(draw.paletteDiagnostics.verdict)`。
- `d3d9_war3_shadow.cpp`：D 点的 `nativeKnown` 与 `selectionCleared` 都消费**同一个** `Verdict`。
- **零新增读取（可核验）**：`d3d9_device.cpp` 中 `QueryCurrentPaletteFrameTag(` 仍为 **3** 处、
  `IsSkinPaletteSelectionCurrent(` 仍为 **1** 处 ⇒ 与改动前一致；门禁已把这两个数字钉死。

### 三、执行型验证（执行型宿主测试，取代文本语义断言）
- 新增 `src/d3d9/war3/render/tests/war3_palette_object_diagnostics_test.cpp` + meson 注册；
  直接调用**生产使用的同一批 inline 交接函数**，按生产顺序跑「选材诊断赋值 → 选择被替换 → 载荷拷贝 → S/D 消费」。
- 覆盖裁定的六个用例：`ready∧tag=0`（S/D 不分叉、不得记回退）/ `packet 回退且 tag 非零`（不得升级为本帧证明）/
  `未检查 vs 不匹配 vs 已证明`（三者不得混淆）/ `A 证明后换成 B`（不得继承）/ `合法候选正向可达` /
  `真正清空 vs 默认载荷`（后者保持未知、不得推断清空）；另加 `陈旧只在检查执行时成立` 与
  `皮肤选择仍当前本身不是本帧证明`。
- **测试当场抓到我自己的一处真实语义缺陷**：初版规则让「皮肤选择仍当前**通过**」也能升级为
  `KnownCurrent` ⇒ 已收紧为「只有**做了本帧比较**的证据（Captured/Producer CurrentFrame）才可给出 KnownCurrent」，
  皮肤检查只在上面的 `KnownStale` 分支起作用。这正是「执行语义」相对于「锁文本」的价值。

### 四、门禁（`AutoTest/test_palette_object_native_reason_static.py` 重写）
- 从「锁文本」改为：锁**共享接口与消费点**（两点都用 `palette_object::`、D 点两处用同一 `Verdict`、
  载荷整份按值携带、S 点消费同一规则、旧过载枚举必须消失）、锁**动作记录位置**（回退必须在真实替换处/之后、
  陈旧必须在真正执行的分支内、清空必须在真实清空分支内）、锁**证据只出现在既有检查处**、
  锁**原生读取次数不变**、并要求**执行型测试存在且已注册**。
- 同时修掉验证者指出的缺陷：旧 `#if 0` 正则用 DOTALL 且以 `.*$` 收尾，会把 `#endif` 之后的**有效源码**一并吞掉
  ⇒ 改为**配平计数**扫描；并在文件头写死「本文件不是语义验收依据，只识别特定文本形状」。
- **可失败性探针**（均已还原）：`S 点硬编码 true` exit 1 — 具名；`packet 回退记录挪位` exit 1 — 具名；
  `证据点新增第三处` exit 1 — 具名；`新增一次原生读取` exit 1 — 具名；`D 点退回 frameTag 反推` exit 1 — 具名；
  `meson 注册被删` exit 1 — 具名；baseline exit 0。
  ⚠️ 诚实说明：**收窄类**改写（例如在 S 点再与一个额外条件取合取）仍 exit 0 —— 那是**设计正确**（只会更保守），
  不是绕过；门禁防的是**放宽**。

### 五、门禁电池（本批实测）
```
build32 (ninja -C build32)            => exit 0；ninja -C build32 -n => no work to do
war3_palette_object_diagnostics_test  => SUMMARY: 17 passed, 0 failed
AutoTest/test_*_static.py（全量）      => 264/0
ninja -C build32 test（meson）         => Ok: 86  Fail: 0（新增 1 例）
war3_palette_object_evidence_test      => 45 passed, 0 failed
war3_palette_object_wire_roundtrip     => checks=126 failures=0 PASS
test_palette_object_wire_roundtrip.py  => CHECKS=1160 FAILURES=0
war3_palette_object_evidence_cost_test => exit 0
```

### 六、⚠️ 行为变化的预估（**尚未实机验证**，且本裁定不含部署/启动授权）
- 新语义下 `nativeKnown` 为真需要「有**通过**的**本帧比较类**证据且其身份**恰好**是本次选择」。
  上一轮实机采集的对应字段是 `submitLiveRebuildAttempt=380 / Miss=380`，即
  `capturedPaletteCurrentFrameProven` 为**假** ⇒ 在**该次**数据上，新的 `nativeKnown` 会变成 **false（Unknown）**。
  ⇒ 我此前「LiveNative 支实机可达、`nativeKnown=true` 出现 64/64」的说法**在新语义下不再适用**；
  必须等**下一次授权的采集**才能给出新的实机结论，**不得**用旧数据推断新行为。
- 未接线项（如实登记）：生产者侧 `producerPaletteCurrentFrameProven` 目前**没有**可携带的诊断载荷，
  本批未接线；`:21694-21697` 的专用可观测性、C2 场景缺口、提交事实缺失的归因均**未变**。

## 2026-09-19 — 独立验证者（aba69304）证伪三处；**当轮修复**与永久固化

### 一、反例 A（最严重：**我引入的回归**，已修复）
- 我在改写陈旧分支时**误删了既有的 `selectedPalette = {};`**（fail-closed）。
  后果（验证者源码级论证）：`war3_canonical_draw.cpp:262-268` 在 `hasSelectedPalette` 时用 `skin::Usable(selectedPalette,…)` 决定 `NoPalette`；
  `{}` 必不 Usable ⇒ 旧实现下陈旧 caster 一律被拒；删除后只要 hash/part/groupRange 通过就可能**被准入**
  （`CapturedWriter` 的 Usable 不查 frameTag，而 `IsSkinPaletteSelectionCurrent` 恰恰只查 frameTag），
  且实际构建带 `-DWARVK_SKIN_PALETTE_CONTRACT_DEFAULT=1` ⇒ 非死代码。
- **这是对「不得改变 palette 准入」硬约束的违反 ⇒ 已立即修回**（`d3d9_device.cpp:20916`），
  并在门禁加入回归断言：陈旧分支内**必须**存在 `selectedPalette = {};`（删除即具名失败）。

### 二、反例 B（放宽类改写：门禁与执行型测试都察觉不到）—— 已用**规则**堵住
- 验证者实测：把证据实参 `capturedPaletteCurrentFrameProven` 改成一个 `true`（一个 token）后，
  门禁仍 exit 0；在 packet 回退 + packet tag=0x2A 的重放下甚至得到 `KnownCurrent` + `nativeKnown=1` + `nativeFrameTag=42`
  —— 正是本批要消灭的「packet 帧冒充 native 帧已知」。
- **修复（在批准的边界内，仍零新增读取）**：证据增加 `observedFrameTag`，其值取自**既有**
  `QueryCurrentPaletteFrameTag` 调用已经得到的结果；`KnownCurrent` 现在还要求
  「被描述选择的标签 == 证据观测到的当前帧标签 ≠ 0」。只把布尔实参写成 true **不再足够**。
- 固化：执行型测试新增**穷举真值表**（8640 组合）断言 `relaxViolations == 0`，
  以及**重放用例**（放宽实参 + 真实 packet 回退 ⇒ 必须 Unknown）。

### 三、反例 C（契约身份与导出字段脱钩）—— 已按「被描述身份取自实际导出的选择」修复
- `KnownCurrent` 曾可能与导出 `nativeFrameTag=0 ∧ nativeUnknown=false` 共存（上一轮 C1 明令禁止的组合），
  因为证据/身份取自契约字段而参与 `Usable`/导出的是 `selectedPalette`。
- **修复**：`NoteLiveNativeSelected` 的被描述身份改为**取自 `selectedPalette` 本身**
  （`source/slot/frameTag`）；结合上面的标签相等要求 ⇒ `KnownCurrent` ⇒ 被导出标签**非零且等于观测值**。
- 固化：真值表断言 `decoupleViolations == 0`。

### 四、验证者指出的其余缺口（已补）
- S 侧 `NoteEnqueued` 的 `selectionCleared` 原为另一个标志位（当时恒等）⇒ 现统一为
  `draw.paletteDiagnostics.verdict.overrideCleared`（消费同一份描述，消除漂移面）。
- 执行型测试补齐：`passed=true 但无观测标签` ⇒ Unknown；观测标签不匹配 ⇒ Unknown；
  生产者侧正向可达；本帧比较类检查**执行但未通过** ⇒ Unknown；packet 回退作废既有证明；同身份替换保留证据；
  并把恒真断言替换为**非恒真**性质（替换后，先前取下的副本不得跟着变）。
- 验证者确认成立项：唯一 `Verdict` 规则（25088 组合零放宽）、**零新增原生读取**（QCFT 3/3、ISPS 1/1）、
  动作只在真实分支、A 的证明不被 B 继承（生产路径）、17/0（当时）。

### 五、本批实测（修复后）
```
ninja -C build32                     => exit 0
war3_palette_object_diagnostics_test => ENUM: combos=8640 relaxViolations=0 decoupleViolations=0
                                        SUMMARY: 30 passed, 0 failed
AutoTest/test_*_static.py（全量）     => 264/0
ninja -C build32 test（meson）        => Ok: 86  Fail: 0
war3_palette_object_evidence_test     => 45 passed, 0 failed
```

### 六、仍然诚实登记的边界
- 执行型测试仍**不编译** `d3d9_device.cpp`（体积原因）⇒ 它锁的是**共享规则与交接函数**；
  生产**接线点**（哪个实参传进来）由门禁的文本/位置断言 + 实机证据承担，**不是**语义证明。
- 一个**刻意伪造**的代码路径（同时把观测标签也写成常量并保持身份匹配）仍能骗过任何诊断规则；
  本批消灭的是**自然的放松式改写**，以及「证明与导出字段脱钩」。
- 未接线：生产者侧 `producerPaletteCurrentFrameProven` 仍无可携带载荷；
  `:21694-21697` 可观测性、C2 场景缺口、提交事实缺失归因均未变。
- **未做任何实机验证**（本裁定不含部署/启动授权）；新语义下的实机行为需下一次授权采集才能谈。

## 2026-09-19 — 第二轮独立验证（2ecc3e33）：规则层可证，接线层不可由文本保证；响应与收口

### 一、验证者确认成立的（保留）
- 唯一 Verdict 规则：**全空间 622080 组合**零放宽（decoupleViolations=0）；生产形状子空间 12960 组合 0 反例；
- device.cpp 的 `QueryCurrentPaletteFrameTag(` 计数 3/3（HEAD 3、2026-09-15 快照 3）⇒ 本批在该文件 **0 新增读取**；
- 动作只在真实分支记录；生产路径上 A 的证明不被 B 继承（逐点源码核对）；
- fail-closed 清空恢复后生产确实是 fail-closed：探针实测 Usable(stale)=1、Usable({})=0 ⇒ 该清空是这条分支唯一屏障。

### 二、验证者证伪的（接受）与处置
| 项 | 事实 | 处置 |
| --- | --- | --- |
| A2 回归锁是子串 | `if (false) selectedPalette = {};` ⇒ 门禁 exit 0 而分支已不清空 | **未再加码文本**；登记：真正锁住需把该分支变成可执行检查（编译 device.cpp 或抽 helper 进可测头）⇒ 待裁定 |
| B1 | S 点前置 `true ||`（9 字符）⇒ 门禁 0、执行测试 30/0，导出 (nativeFrameTag=0 ∧ nativeUnknown=false) | **结构性收敛**：S 点/导出的标签改取自**同一份按值描述**，消除两载体靠约定相连的分叉面 |
| B5 | 分支后强制写字段 ⇒ KnownCurrent | 属**接线层**（非规则层）⇒ 登记为不可由本机制保证 |
| B8 | D 点 `... || true` | 同上 |
| B3/B3B4/B6 | 写死实参 / 放宽观测标签 / 放宽 covered | 规则层已要求单帧身份 + 观测标签相等；接线层实参仍非机制保证 |
| H1 来源枚举放宽 | 门禁 0 但**执行测试 FAIL 2 条**（relaxViolations=2） | 唯一被**执行**抓住的类别 ⇒ 头内规则确有机制保护 |
| H2 / C 的 min≠max | 48 个 KnownCurrent 中 32 个 min≠max；Verdict 不约束 max | **已修**：Verdict 现在要求身份为**单帧**（min == max），否则 Unknown |
| (e) D 点前件冗余 | `Verdict != KnownCurrent && overrideCleared` 前件冗余 | **已清理**，只保留 overrideCleared |
| ⑤ 空/恒真断言 | 副本与原件同 Verdict 恒真；combos==8640 只验循环边界；约 5 条只验交接函数存下实参 | **已修**：删恒真断言；枚举扩为全空间（combos=622080）；新增可执行屏障断言（Usable({})=false、Usable(stale)=true） |
| ⑥ 整树读取计数 | device.cpp 0 新增，但整树 8 vs HEAD 6（多出 2 处在 war3_shadow_renderer_core.cpp:874/6756，均为未提交新增行） | **如实登记**：门禁只钉 device.cpp；那 2 处**不属于本批**（我只读 git diff 确认其为该文件未提交 + 行，本批未编辑该文件） |

### 三、本批（第二轮响应后）实测
```
ninja -C build32                       => exit 0；ninja -n => no work to do
war3_palette_object_diagnostics_test   => ENUM: combos=622080 knownCurrent=16 relaxViolations=0 decoupleViolations=0
                                          SUMMARY: 31 passed, 0 failed
AutoTest/test_*_static.py              => 264/0（含更新后的 capture_points / native_reason 门禁）
ninja -C build32 test（meson）         => Ok: 86  Fail: 0
war3_palette_object_evidence_test      => 45 passed, 0 failed
war3_palette_object_wire_roundtrip     => checks=126 failures=0 PASS
test_palette_object_wire_roundtrip.py  => CHECKS=1160 FAILURES=0
war3_palette_object_evidence_cost_test => exit 0
```

### 四、机制边界（写死，不再以文本加码）
- **可由机制保证**：头内规则的语义（全空间枚举）与该规则自身的严格性。
- **不能由机制保证**：生产**接线**（哪个实参传进来、分支是否真的调用、D 点是否真的读该对象）。
  验证者实测的 B1/B5/B8/B3 等自然放宽都能在门禁 exit 0 且执行测试 30/0 下重造谎报。
- 关闭该缺口只有两条路，均超出本批授权（需用户裁定）：
  (i) 把 device.cpp / d3d9_war3_shadow.cpp 的相关片段编译进宿主测试（构建/测试架构改动）；
  (ii) 以实机/运行时证据替代静态接线保证（需部署与采集授权）。
- 另：刻意伪造的路径（连观测标签也写常量并保持身份匹配）仍会骗过任何诊断规则 —— 本批目标是消灭自然放松与证明↔导出脱钩。

## 2026-09-19 — 全权授权后的推进：路径 (i) 接线收敛 + 路径 (ii) 实机验证（新语义已部署并采集两次）

### 一、路径 (i)：把「接线」里可测的部分搬进共享头（缩小不可测面）
- `war3_palette_object_diagnostics.h` 新增（生产与宿主测试执行**同一段代码**）：
  · `NoteCapturedPaletteCurrentFrameEvidence(...)`：本帧证明的**组合规则**（provenance ∧ min≠0 ∧ min==max ∧ 当前标签可读 ∧ 观测==min）；
  · `ExportedNativeFrameFor(d)`：**被导出的本帧字段由同一份描述推导** ⇒ `known=false ⇒ tag=0`、`known=true ⇒ tag≠0`。
- `d3d9_device.cpp`：接线点只传**既有**原始值（`QueryCurrentPaletteFrameTag` 调用数保持 **3**；门禁钉住），
  S 点/导出标签改为读 `ExportedNativeFrameFor` ⇒ 消除验证者指出的「两载体靠约定相连」（B1 的攻击面）。
- 执行型测试扩到 **39 passed / 0 failed**，新增：
  · 全空间枚举 `combos=622080 knownCurrent=16 relaxViolations=0 decoupleViolations=0 exportViolations=0`；
  · 导出不变量（杜绝 `(tag=0 ∧ known=true)` 与 `(tag≠0 ∧ known=false)`）；
  · 直接驱动生产接线函数：放宽 provenance(B3)、不可读时回退观测标签(B3B4)、身份 min≠max(H2)、观测≠选择标签 ⇒ 全部 Unknown；
  · fail-closed 屏障可执行断言：`Usable({})=false`、`Usable(stale)=true`（证明该清空是唯一屏障）。
- 两条既有门禁（`test_trusted_current_palette_rebuild_bypass_static`、`test_palette_object_native_reason_static`）按**意图不变**更新：
  组合规则改在共享头检查，device 侧改检查「仍传既有原值」。全量静态 **264/0**。

### 二、路径 (ii)：实机验证（已部署 + 两次采集）
- 站点部署（安全流程）：旧 `BCF234C8…` 备份为 `d3d9.dll.BCF234C8_backup_20260919-120445`；
  `war3_running=0` 故未 park；站点现为 **`8697A86C…`**（= 本批构建）。
- 采集 1（高压图，窗口 30s）：`history-29908-699519292772-1`（cpu-events 34MB）；
- 采集 2（轻量图 `(4)Adrenaline.w3m`，窗口 20s，**判别性尝试**）：`history-19804-700583923671-1`（31MB）。
- 两次的共同结果（读方）：`chains=64`、`unknownFrameDomains = {(): 64}`、拒绝理由含 `identityWeak/epochUnknown/identityNotProven/chainNotCovered/recoveryNotProven`
  （采集 2 另见 `liveDrawnWithoutSubmit: 2`，与第 116 轮的解释一致：缺提交事实的 live Drawn 会被 fail-visible 拒绝）。
- 采集 2 的交叉核对：`submitLiveRebuildAttemptCount=21 / Hit=0 / Miss=21 / Applied=0`、
  `semanticSceneAppendEntrySkinnedCount=0`、`SkinnedCasterCount=0`、`DrawTimeProducerSubmittedSkinnedCount=10`、`RejectedPathBlockerProducerCount=0`。

### 三、⚠️ 诚实结论（不得夸大）
- **两次采集都不区分新旧语义**：`unknownFrameDomains` 均为 `{(): 64}`，与旧构建（`7120104E…` 时代）**相同** ⇒
  记录链的 native 域在两次里都不是未知；那 21 次 rebuild miss 属于**未进入 palette 记录链**的 draw。
- 因此本批**没有取得「Unknown 路径」的实机证据**（新语义下 `nativeKnown=false` 的分支未在记录链上出现）❌；
  也**不能**声称实机验证了「拒绝谎报」的改进 ✅ —— 只能说：新语义的**正向路径**（KnownCurrent + 非零标签）在生产中确实可达，
  且 `(tag=0 ∧ unknown=false)` 这一被禁组合**未出现**（链级 + 共享推导层面）。
- 要取得判别性实机证据，需要一次「被记录 draw 缺少本帧证明」的采集（例如能触发该分支的图/时机）⇒ 登记为后续。

### 四、环境处置（据实）
- **E: 盘曾被占满（0GB 可用）导致一次采集失败**；已清理：5 个旧采集目录（7.2GB）+ 3 个验证者镜像（0.3GB）+ 本轮 `slot-*.tga`（数十个 14MB），
  现可用 **6.7GB+**；`E:\Work\War3`（119GB，玩家树）**全程未触碰**；
- 站点回退：把 `d3d9.dll.BCF234C8_backup_20260919-120445` 复制回 `d3d9.dll` 即可。

## 2026-09-19 — 用户指令：实机采集必须走**隔离桌面**（不得干扰前台）—— 已切换并如实登记验证状态

### 一、处置
- 新增 `E:\Work\warvk-live-E\run_live_isolated.py`：与 `run_live_E3.ps1` 的唯一差别是**游戏进程经
  `war3_autotest_mcp._launch_process_on_desktop` 在隔离桌面启动**；监视器以 `CREATE_NO_WINDOW` 子进程运行；
  只终止**自己启动的 PID**；结束后 `_close_desktop_handle` 关闭桌面句柄并复核输入桌面名。
- 立即核查：当前**无任何游戏进程残留**；上一批可见桌面采集已自行退出（未影响前台）。

### 二、隔离采集实测（新构建 `8697A86C…`，轻量图）
```
desktop_ok=True  name=War3AutoTestCapture_38596_...  inputDesktopBefore=Default  nonInteractiveOnly=True
launch_ok=True   game_pid=33220                    inputDesktopAfter=Default
uses_isolated_desktop=False   <-- ⚠️ 未通过
artifact=history-33220-703165001648-1  cpu-events.json 25,459,029 B
desktop closed; inputDesktopAfterAll=Default
```
- ✅ 前台输入桌面名**前后都是 `Default`**（未被切换、未抢焦点）；
- ⚠️ **但 `_uses_isolated_desktop(pid)` 为 False**（我在启动后 1.5s 查询）⇒ **尚不能声称隔离已成立**；
  下一轮必须查清该谓词的判据（是否按窗口站/桌面句柄、是否查询过早、是否需要进程已建窗口），
  在它通过之前，**不把该次采集登记为合规隔离数据**。

### 三、判别性实机证据：仍未取得（据实）
- 本次（隔离）与上两次（可见）均为 `chains=64`、`unknownFrameDomains = {(): 64}`；
- 交叉核对：`submitLiveRebuildAttemptCount=8 / MissCount=8`、`semanticSceneAppendEntrySkinnedCount=0`、`SkinnedCasterCount=0`；
- ⇒ 这些 miss 仍出现在**未进入 palette 记录链**的 draw 上 ⇒ **三次采集都不区分新旧语义**；
  「新语义下 Unknown 分支」的实机证据**仍未取得**，继续登记为后续（不夸大）。

### 四、门禁现状（本批最终）
```
ninja -C build32                      => exit 0；ninja -n => no work
war3_palette_object_diagnostics_test  => ENUM: combos=622080 knownCurrent=16 relaxViolations=0 decoupleViolations=0 exportViolations=0
                                         SUMMARY: 39 passed, 0 failed
AutoTest/test_*_static.py             => 264/0
ninja -C build32 test                 => Ok: 86  Fail: 0
站点 d3d9.dll                          => 8697A86CA166CD25...（旧 BCF234C8… 已备份）
```

## 2026-09-19 — **对原始长期计划（docs/plan/2026-09-18-night-marathon-plan.md）的逐条取证与两项补齐**

### 本轮补上的两个计划项
- **P2-1 ③（此前无任何调用者）**：`capture_startup_environment.ps1` 现已接入 `run_live_isolated.py`，
  并以**同一份采集环境变量**调用（快照落在 `E:\Work\warvk-live-E\startup-env`）⇒
  「部署并采集的到底是哪一份环境/DLL」可复核。
- **P1-3（计划明文要求交付包含真实读方/测试/构建配置）**：新建 `E:\Work\WarVK-delivery-20260919-stageE-r2-full.zip`
  = 715,637 B、SHA-256 `13473E844123762807F417D1987F8DB139288470CF097ED94A69D04A593B7621`、22 条目；
  含：产品源（3 + 2 头）、**真实读方** `analyze_palette_object_evidence.py`、门禁 5 个、执行型测试、
  `frame_history_watch.py`、**构建配置**（`src/d3d9/meson.build` + `build32_safe.cmd`）、隔离采集脚本、
  `REPRO.md`（可复跑命令）、`VERIFICATION.md`（含未闭合与不可声称）、`MANIFEST.txt`（逐文件哈希）。
  ⚠️ 该包仍**不含**完整树或 `git apply` 补丁（本树有大量既有未提交改动、我不用 git 写操作）⇒ REPRO.md 已如实声明。

### 逐条取证结果（代码/产物为准）
| 计划项 | 现状 | 取证 |
| --- | --- | --- |
| P0-1 ① 初始化早于发布 active | ✅ | `war3_frame_evidence.cpp:367-368`（`ArmPaletteObjectEvidence` 在 `active.store` 前，注释即该修正）|
| P0-1 ② 会话代际校验 | ✅ | `:332` `generation != s->ring.session()`；`:336` 代数耗尽拒绝 |
| P0-1 ③ 四组确定性屏障 | ⚠️ **未见按四条命名者** | 仅见 `AutoTest/test_recorder_ingress.cpp`（容量/冻结/关闭/丢失计数）；需复核或补 |
| P0-2 MarkStage/maxStage | ✅ 机制在场 | `maxStage` 13 处 |
| P0-3 ObjectGone 不伪造阶段/双链 | ✅ | `NoteObjectGone` 5 处 |
| P0-4 ObservationClosed 结算 | ✅ | 头内 10 处 + **实机**观察到链终态均为 ObservationClosed |
| P0-5 关联标识 + D 点接线 + wire 决定 | ✅ | `attemptSerial` 按值 8 处；D 点（第 4 个工厂调用点）已接；wire 决定=不上 wire，读方用 chainSequence |
| P0-6 双链 C1 | ✅ | `ChainType` 24 处 + v4 链型字段 + 实机 `data[4]` |
| P1-1 读方同步 | ✅ | 本轮实机分析即用该读方（v4/终态 7/链型）|
| P1-2 chainId + path scope | ❌ **未实现** | `chainId` 在全树读方/头中 0 处 |
| P1-3 交付包含读方/测试/构建配置 | ✅（本轮补齐）| 见上 |
| P1-4 门禁外红处置 | ✅（早前轮次）| 内存两条已建真实方法测试 |
| P2-1 部署流程 ① ② ⑤ | ✅ | 备份哈希、rename-park、收尾复核哈希 |
| P2-1 ③ 启动环境记录 | ✅（本轮接入）| 见上 |
| P2-1 ④ 回退脚本自测 | ❌ 未做 | 站点有回退 cmd，但未自测 |
| P2-2 隔离桌面采集 | ⚠️ **未通过** | 实测 `_uses_isolated_desktop(pid)=False`（前台桌面名前后均为 Default）|
| P2-2 v4 八项核对 | ✅ 部分 | version=4、终态 0-7、链型、`emitted==exported` 已核 |
| P2-3 判读纪律 | ✅ | 未新增实机因果结论、未当前台性能、未声称阴影恢复 |
| P3-1 门禁外测试登记入口 | ⚠️ 部分 | — |
| P3-2 陈述性断言审计 | ⚠️ 部分 | 本轮新增断言均配探针 |
| P3-3 写读契约矩阵（版本 1/2/3/4）| ❌ 未见交付物 | docs 下无矩阵文档 |
| P3-4 两份测试清单载荷性核对 | ⚠️ 部分 | — |
| P3-5 子门关闭零开销实测支撑 | ❌ 未见 | — |
| P3-6 evidence.h 拆分评估 | ❌ 未做 | — |
| 子线程验证协议 | ✅ 执行 | 本轮又开 3 个（含两轮证伪并插队修复）|
| 纪律（无 git 写／不碰 War3／探针／锚点唯一性／退出码）| ✅ | 全程遵守 |

## 2026-09-19 — 用户要求「当前版本的稳定构建用于测试」：按项目规矩交付**测试候选包**（非稳定版）

### 交付物
- `E:\Work\WarVK-candidate-20260919-8697A86C.zip`
  = 8,209,219 B，SHA-256 `70A615B3454FED1A5D8B8331D18A39A9936A7BE5EDBD9D33DDB789E09596DA51`，4 条目：
  `d3d9.dll`（36,348,301 B，SHA-256 `8697A86CA166CD2509274DA0B579EF72EB2299E741F8144501744DBAEC8B1C6B`）、
  `README.md`（改动/未改/已验证/已知限制/回退）、`SHA256SUMS.txt`、`回退到BCF234C8.cmd`。
- 站点 `E:\Work\Warcraft III\d3d9.dll` **已是**该哈希（此前为采集部署，可直接测）。
- 复用/回退备份：`d3d9.dll.BCF234C8_backup_20260919-120445`（上一候选）、
  `d3d9.dll.F275545B_backup_20260918-212247`（更早基线）、站点既有 `回退到A0A51AF2.cmd`。

### 命名纪律（不得含糊）
- 该包**不叫稳定版**：按 AGENTS.md，候选/纯证据不得冒充稳定更新；本构建**未过玩家前台视觉/性能门**。
- README 已写死四条已知限制：未过前台门；诊断语义改变导致「本帧已知」标签更少、与旧读数不可直接对比；
  「未知路径」无实机证据；隔离桌面 `_uses_isolated_desktop` 实测 False。

### 顺带闭合的计划项
- **P2-1 ④ 回退脚本自测**：`回退到BCF234C8.cmd` 支持 `WARVK_SITE` 覆盖，
  已在临时目录用假文件自测（新候选文件被备份正确覆盖，exit 0）⇒ **闭合**。

## 2026-09-19 — 玩家实机反馈两项：导出报错归因 + 输出盘清理（用户明确授权清理旧日志）

### 一、`local-recorder-raw-export-failed; partial files retained` 的机制
读代码确认（`war3_frame_history.cpp`）：该字符串只是导出阶段的**笼统前置标签**，真实原因被外层丢掉：
- arm 阶段门槛：`RecorderDiskHeadroom = 6 GiB`（`war3_frame_recorder_config.h:28`）；
- 默认档 `ExternalRecorderProfile{ cpu=262144, pre=256, post=4, preMs=1000, slots=576 }` ⇒ 帧图 ring 上限 **4 GiB**；
- 导出阶段（`:214-222`）三条真实失败路径：ring 未 Frozen（`freeze before export`）、
  `CreateNew export failed`（写盘失败，最可能是空间不足）、`input provider never initialized`。
本轮改动（**只改诊断、零新增原生读取**）：
1. 按 CPU 侧返回的 `error` 给**具名**标签：`(freeze-before-export)` / `(write-failed; check free space >= 6 GiB)` /
   `(input-provider)` / `(cpu-control-error)`；
2. 新增**只缩小**的采集规模开关 `DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES`（∈[4,256]）与
   `_POST_FRAMES`（∈[1,16]）——夹在既有策略边界内，**不可能放宽**任何准入/预算判据；
   已同步登记进 `capture_startup_environment.ps1` 白名单。

### 二、环境处置（用户报告 War3/WarVK 日志占 ~80 GB）
- 盘点：`E:\Work\War3\WarVK\Log` = **55.58 GB**（`.jsonl` 49.2 GB/226 个，`.html` 6.37 GB/**13,724** 个，跨度 2026-01-03…09-17）；
- 处置（**只删日志**，不碰游戏文件/DLL/配置/地图）：删 **13,508 个文件 / 54.39 GB**，跳过(占用中)=0，
  清单存 `E:\Work\deleted-war3-logs-20260919.txt`；
- 保留：最近 30 天的 506 份 `.html` 报告、最近 7 天的其它日志、`Crash\*.dmp` 崩溃转储（未删）；
- 结果：`WarVK\Log` → 513 文件 / 1.60 GB；**E: 可用 5.95 GB → 60.30 GB**（同时满足录制器 6 GiB 输出余量）。

### 三、本轮构建与部署
- 新 DLL SHA-256 `670028D23EA9DCD0CED84985AFCA44EB5665FB7AE264CE426D8EA564521D53EA`；门禁全绿
  （静态 264/0、meson Ok:86 Fail:0、录制器 memory/session 测试全 PASS）；
- 部署时 `war3_running=1` ⇒ 按不变项用 **rename-park**：旧 `8697A86C…` → `d3d9.dll.parked_20260919-143636`，
  新 DLL 就位（**不打断玩家，下次启动生效**）。

## 2026-09-19 — 玩家实机反馈：采集失败 `reset-or-owner-change`（已具名化）

### 一、机制（读代码确认）
`war3_frame_history.cpp:257` 在 `m->cancelled` 置位后把下一帧 capture 判失败，而 `cancel()` 的调用点是：
- `d3d9_swapchain.cpp:1130` `D3D9SwapChainEx::Reset(...)` ⇒ **交换链重置**（分辨率/全屏窗口切换、设备重置、游戏自身重建交换链）；
- `:1059` 后缓冲图像缺失；`:1086` 历史拷贝提交失败（并隔离槽位）；`:1089` 捕获异常；
- `war3_frame_history.cpp:144/162`（已有 owner 被替换 / owner 释放）。

### 二、本轮改动（**只改诊断，不动任何准入/策略**）
- `cancel(const char* reason=nullptr)`：记录具名原因并在 capture 失败处使用（缺省仍为 `reset-or-owner-change`）；
- 四个调用点具名：`swapchain-reset` / `history-copy-submit-failed` / `history-source-image-missing` / `history-capture-exception`；
- 已部署构建 `ED9CB5568209996FC289B8CFDE3ED84CA4128DAE67CA33212CCAFBF02272174E`（部署时 `war3_running=0` ⇒ 直接备份+复制）；
- `AutoTest/test_frame_history_static.py` 因文本变化一度变红：按**意图不变**修正（这些路径仍必须取消录制），
  并**加强**为要求四处具名原因齐全 ⇒ 全量静态恢复 **264/0**，meson **Ok:86 Fail:0**。

### 三、如实登记的缺口（未修）
- **自包含模式下故障后无法导出**：本地 worker 一旦 `fail()` 即停止循环，冻结的 CPU 环留在内存里，
  而外部控制需要租约 ⇒ 该次证据**取不出来**。⇒ 登记为待修：**故障前先落盘冻结证据**（salvage-on-fault）
  以及**交换链重置后自动重新 arm**（两者都是行为改动，需独立门禁与探针）。

## 2026-09-19 — 定位玩家实际测试路径（关键澄清）
- 构建产物：`<repo>\build32\src\d3d9\d3d9.dll` = `ED9CB556…`；
- 我历轮部署目标一直是**站点树** `E:\Work\Warcraft III\d3d9.dll`（现为 `ED9CB556…`）；
- **玩家实际启动的是 `E:\Work\War3`**（其 `d3d9.dll` = `A0A51AF2…`，2026-09-17 19:25）⇒ 我的构建**从未**进入该树；
- 其启动器 `启动-测试版-取证模式.cmd` 只设 `DXVK_WAR3_FRAME_EVIDENCE=1`，**没有** `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED=1`
  ⇒ 本地录制器不启动 ⇒ Ctrl+Shift+C 被识别却没有可 arm 的录制器（与玩家反馈一致）；
- 已创建修正版启动器（**放在站点树**，避免触碰玩家树）：`E:\Work\Warcraft III\启动-测试版-取证模式-WarVK-ED9CB556.cmd`；
- ⚠️ 本轮构建**不含阴影修复**（只有诊断与录制器易用性）⇒ 用它可以复现/看清报错，但不能期望阴影变好。

## 2026-09-19 — 玩家实机 `recorder-process-address-space-headroom`（arm 阶段 fail-closed）

### 判据（读代码）
- `war3_frame_recorder_memory.h:8` `RecorderProcessHeadroom = 256 MiB`（明确声明不改宿主 EXE 的 LAA 标志）；
- `:22-27` `availableVirtual` 与 `availableCommit` 都必须 ≥ 预留 + 附加字节，否则分别报
  `recorder-process-address-space-headroom` / `-commit-headroom`；
- `war3_frame_evidence.cpp:340-354` 附加字节 = `Ring::storageBytes(capacity=262144)` + 输入预算，并要求连续区 ≥ ring+64 KiB。
⇒ 即 **32 位进程剩余虚拟地址空间不足**，属诚实 fail-closed，非缺陷。

### 本轮改动（诊断 + 一个**只下调**的救命开关）
- 失败文本现在带上实测数字：`required=… availVirtual=… availCommit=… largestFree=…`；
- 新增 `DXVK_WAR3_FRAME_EVIDENCE_VA_HEADROOM_MB`：**只允许下调**预留（下限 64 MiB，上限默认 256 MiB），
  语义不变（仍 fail-closed；若 ring 仍放不下会得到 `recorder-cpu-ring-allocation-failed`）；已登记进启动环境白名单；
- 构建 `87C867A6CBDA1634123C64CE2F642130F3AFF0A5A6ADDE5463D79BC099A55C09`；静态 264/0、meson Ok:86 Fail:0、
  录制器内存测试 87+46 checks PASS；部署时 `war3_running=1` ⇒ park `ED9CB556…` 为 `d3d9.dll.parked_20260919-145909`；
- 修正版取证启动器改名稳定化：`E:\Work\Warcraft III\启动-测试版-取证模式-WarVK.cmd`
  （含 `EVIDENCE=1` + `SELF_CONTAINED=1` + `PALETTE_OBJECT=1` + 输出目录 + `VA_HEADROOM_MB=96`）。

### 仍未解决（用户关切）
- 玩家游戏树 `E:\Work\War3` 里仍是 `A0A51AF2…`（09-17）⇒ **我的任何构建都还没被真正测试过**；
- 阴影缺失的快照池容量耗尽（384 MiB 顶格 / 28536 次容量拒绝）**尚未修**；
- 自包含模式故障后冻结证据**无法导出**（salvage-on-fault）**尚未修**。

## 2026-09-19 — 用户提问：之前做的「64 位渲染进程（RH0/RH1）」现在能用上吗？—— 取证回答

### 它是什么、现在什么状态
- 源码在 **`<工作树>/tools/render_host/`**（不在 `src/`，所以先前按 `src` 搜不到）：
  `protocol` / `win32_transport`(命名管道+SID DACL+双向 PID/创建时间核验) / `shared_slots` / `slot_ledger` / `slot_wire` /
  `producer_inbox` / `sample_envelope` / `recorder_ingress` / **`recorder_event_wire`** / **`recorder_history_store`** /
  `recorder_host_main.cpp` / `process_memory_probe` / `owned_child`(父子 Job kill-on-close) + `README.md`；
- 契约文档在本树 `docs/plan/2026-09-16-render-host-*.md` 与 recorder-offload 系列；
- 64 位工具链**已就位**：`E:\Dev\Toolchains\warvk-rh0-llvm-mingw-20260616`（含 i686/x86_64 clang++）；
- **本地实测（2026-09-19）**：`AutoTest/run_recorder_offload_e2_gate.py` 于本机 **15/15 用例全通过**，
  回执 `E:\Work\rh0-e2-verify-20260919-150245\receipt.json`；
- 回执自述边界（照抄，不夸大）：`scope=RECORDER_OFFLOAD_E2_CPU_LAB`、`productIntegration=false`、
  `shippingDllBuilt=false`、`gameLaunched=false`、`gpuTested=false`、`gameMemoryBenefitMeasured=false`。

### 实测数字（32 位父进程，capacity=262144）
```
attempted/accepted=262304  lost=0  sentPackets=1654  workerJoined=true  hostExit=0  containersGone=true
privateBytes     before 1.22MB → active 5.11MB → after 1.22MB
usedVirtualBytes before 24.1MB → active 29.4MB → after 24.1MB
handles          107 → 107
```

### 结论（诚实的部分）
- ✅ 能力**真实存在且今天可复跑**：32 位 ingress/worker + 64 位 history helper，事件零丢失，容器与句柄干净回收；
- ❌ **没有产品接线**：README 首行即声明 not linked into the shipping DLL；`src/` 内**零客户端符号**；
- ⚠️ 但**它省的是 CPU 事件环（约 5 MB 峰值）**，而玩家遇到的 arm 失败是**缺约 256 MiB 虚拟地址空间**，
  所以“把录制器搬到 64 位进程”**不能单独解决**该 arm 失败；真正的大头是 **384 MiB 的 Stage11 快照几何池**
  与 Vulkan staging 映射（跨进程搬它们需要共享内存/外部句柄，远超 E2 今天证明的范围）；
- ⚠️ README 明确限制：该阻塞式 lab 传输**不得**直接链进 DLL 或渲染线程（产品化需要非阻塞变体）。

## 2026-09-19 — 玩家 15:20 一轮「阴影消失」取证：视觉证据与计数器互相矛盾（并修正我此前的结论）

### 一、玩家记录（站点树，已换 LAA 的 War3.exe）
- `cpu-41360-814576821788-1.json`（25 MB，ok=true，49,882 事件）；`history-41360-814577044041-1`：97 帧 / 1.07 s / state=6(Complete) / 0 failed；
- 系统配置：frameEvidence/localRecorderOwner/paletteObjectEvidence/rawInputs/skinPaletteContract=true，capacity=65536（内部档）；
- 该轮**唯一** named 证据：`snapshot-alloc/v1` **4548 条，全部 reason=2（ResidentCapacity）**；
  末条 data = required=524288, aligned=524288, resident=402653184(384MiB), used=390774528(372.6MiB), pages=24, budget/帧=32, nextPageId=63, reclaimed=38；
- 另一条 14:31 incident 里 inputs.error=recorder-process-address-space-headroom（**LAA 之前**）⇒ 与 LAA 判断一致。

### 二、视觉审查（子代理 f8eb1930 / gemini-3.8-flash-high，看 frame-40/55/70/90）
「四帧中动态投射阴影**全程完全缺失**：全屏所有区域、所有对象（**不论动态单位还是静态建筑/树木/栅栏**）均无投射阴影；
 无局部渲染、闪烁或断裂边缘」⇒ 不是「部分 caster 被省略」，而是**全场景无影**。

### 三、⚠️ 修正我此前的结论（重要）
13:32 性能报告（已存 `E:/Work/perf_data.json`）显示下游**声称在工作**：
```
shadowReceiverReplayCasterCountAvg=395.1  Max=553     ← 回放里有几百个 caster
shadowReceiverReplayGeometryWorkAvg=61778
shadowReceiverFrames=4523  shadowTaaReceiverExecutedFrames=4523（每帧都跑）
semanticSceneReceiverHasCompleteShadowMap=1  ReceiverInputValid=1  NeedPass=1  NeedShadowMap=1
drawTimeSemanticProducerSubmittedCount=254960   semanticSceneLivePaletteRefreshHitCount=4020
producerRequiredCasterOmissionCount=28162（约 6/帧）
```
⇒ 快照池容量拒绝是**真实缺陷**（4548 次），但**不足以解释全场景无影**（回放仍有约 395–553 caster/帧）。
⇒ 我先前把「池耗尽」直接当作阴影消失根因的说法**过强，予以更正**：正确表述是池耗尽导致**部分** caster 被 fail-closed 省略。

### 四、下一步（待定）
- 真正断点应在**下游**：回放 caster 是否退化（零面积/错误变换）、阴影强度/衰减是否为 0、CSM 级联与偏置、深度比较方向/格式、
  或「完整阴影图」只是 CPU 记账而 GPU 附件并未真正写入；
- 需要玩家**失败会话的 fresh 性能报告**（同一 DLL/地图/场景）以区分；现有 13:32 报告属于更早一轮。

### 五、本轮构建
- 快照池上限**可配置且封顶 512 MiB**（32 页，沿用既有 32-create 安全门）：`DXVK_WAR3_SNAPSHOT_RESIDENT_CAP_MB`（下限 128/上限 512/默认 384）；
- 依上述更正，它只是**缓解**（针对省略），不是「全场景无影」的修复；
- 构建 `F2A7A6FC8FFAD077CEE70D69A2F0B16B48AC514837D64BC7D66CE3804F2CC33D`；
  Stage11 publication 73 checks PASS。

## 2026-09-19 — 15:41 一轮（无影态）取证分析：计数器与像素再次矛盾，且转折点不在窗口内

### 一、本轮材料
- 报告 `E:\Work\Warcraft III\WarVK\Log\war3_perf_report_2026_09_19_15_41_40.html`（3.12MB，frameCount=3600，windowSec=40.2，avgFps=91.9），
  meta.dllSha256=F2A7A6FC…（= 本轮部署的池上限可配置构建）；
- 取证 `E:\Work\warvk-live-E\evidence3\cpu-13980-827388855559-1.json`（24.3MB）+ 图像目录（15:41，无影之后）。

### 二、关键观察（逐帧序列，136 列 × 3600 行）
```
hasShadowBudget=1 全程    shadowTaaReceiverExecuted=1 全程    shadowTaaRuntimeModuleEnabled=1 全程
replayCasterCount 377→427（有变化但从不掉到 0）   replayGeometryWork 79497→72846
shadowMapRenderSerial 3159→4954（持续推进）  semanticSceneSubmitted 51→88  skinned 12→67
shadowMetadataCaptureCalls = 0 全程（该 Stage11 元数据捕获路径在本机从未执行）
```
⇒ 窗口内阴影链路自认为一直在正常生产，但像素上完全没有阴影（同一 DLL 的 15:20 与 15:41 两轮皆无影）。
⇒ 断点在当前计数器覆盖不到的层：阴影图实际内容 / 级联-偏置数学 / 最终合成。

### 三、为什么抓不到转折点
- 该报告是滚动 3600 帧（约 40 秒）窗口，玩家所述中途变无影发生在更早 ⇒ 转折点不在窗口内；
- 玩家侧限制：本地录制器模式下一轮只能导一次（owner 已存在或故障后无法再 arm）⇒ 拿不到有影/无影对照。

### 四、给出的可执行采集配方（无需手动按键）
```
set DXVK_WAR3_PERF_MONITOR=1
set DXVK_WAR3_PERF_RECORD_ON_START=1
set DXVK_WAR3_PERF_AUTO_EXPORT_SEC=20    :: 每 20 秒落一份 war3_perf_report_auto_*.html
```
- 依据：`war3_perf_monitor.cpp:1085-1100`（自动录制 + 周期自动导出，原注释即无人值守自动化采样）；
- 采样到无影后，用最后一份有影与第一份无影两份报告做差分，即可把断点定位到具体子系统；
- 对照取证若必须要：两次采集之间重启游戏（绕开本地录制器一轮一导的限制），或改用外部监视器模式。

## 2026-09-19 — 15:41 报告逐帧序列分析：静态世界 caster 从中途起永久消失（玩家判断正确，先前的窗口推断需更正）

### 一、覆盖率（更正我先前说法）
- meta.perfFrameEpoch=5528，frameCount=3600，序列 epoch 1929→5528，businessFrameSerial 62139→65738；
- ⇒ 报告包含取证那段（无影态）与转折点；缺失的只是最前面 1928 帧。玩家的判断正确。

### 二、逐帧序列里的决定性信号（136 列 × 3600 行）
```
terrainDoodadPreparedCount / terrainDoodadCascade0..3DrawnCount:
  行 0..1778    约 241→228（正常，偶发单帧 0）
  行 1779..1881 0（14 秒缺口）
  行 1882..1919 261（短暂恢复）
  行 1920..3599 0   ← 业务帧 64059 → 65738，直到窗口结束再未恢复
同期：terrainDoodadCaptureAttemptCount 仍约 262（仍在尝试捕获）、replayCasterCount 272..431（动态 caster 仍在）、
      effectiveShadowResolution 恒 4096、shadowMapRenderSerial 持续推进
```
- ⇒ 静态世界几何（doodad + terrain S1）的 caster 从业务帧 64059 起被永久丢出阴影级联，与视觉子代理「静态建筑/树木/栅栏也无投射阴影」一致。

### 三、代码定位（待深挖）
- 这些 prepared/drawn 计数在 d3d9_war3_shadow.cpp:4243-4262 的 replayDraws 准备循环内递增；
- 因此 prepared=0 而 captureAttempt≈262 ⇒ doodad 已被捕获但未出现在 replayDraws 中（在捕获与 replay 之间被丢弃）；
- 且「从此不再恢复」的形态提示粘滞状态（配额/缓存耗尽后无回收），而非逐帧抖动；
- 注意：这与先前的 snapshot-alloc/v1 ResidentCapacity（约 6/帧的省略）是两个不同问题，不可混为一谈。

### 四、下一步
- 到捕获→replay 之间找丢弃点（验证/身份/新鲜度/配额过滤），确认是否粘滞；
- 若要看转折点之前的对照，可用 DXVK_WAR3_PERF_AUTO_EXPORT_SEC=20 让报告每 20 秒自动落盘（war3_perf_monitor.cpp:1085-1100）。

## 2026-09-19 — 交接文档（致 Astra）
- 新增 `docs/agent-history/2026-09-19-handover-shadow-loss-and-evidence-issues.md`：
  环境与部署现状（站点 d3d9.dll=F2A7A6FC…、站点 War3.exe=LAA=true、玩家树仍为 09-17 的 A0A51AF2…）、
  阴影消失问题的视觉证据 + 逐帧转折点实据（业务帧 64059 起 terrainDoodadPrepared/Drawn 永久归零）、代码入口（d3d9_war3_shadow.cpp:4243-4262）、
  已排除/未排除清单、建议下一步；取证类 4 个独立问题（导出失败/交换链重置/32 位地址空间/一轮一导）；
  RH0 64 位宿主现状（E2 15/15 通过但无产品接线）；门禁与构建状态；不允许声称清单；产物索引。
