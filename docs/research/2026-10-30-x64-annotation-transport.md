# x64 增强层拆分 S0：数据层切口门与标注传输

2026-10-30，Claude 实施。作者同意全面接管渲染层，并把增强层拆成 x86 数据层 + x64 GPU 执行器
（[拆分计划](../plan/2026-10-30-x64-enhancement-split.md)）。本批是计划的 S0：先能量化切口，再把
x86 数据层的决定以数据形式送到宿主。只改开发代理、x64 宿主和测试，默认关闭；不接游戏、不部署玩家 DLL、
不提交/推送/发布。Game.dll guard 保持。

## 1. 数据层切口门（`AutoTest/run_x86_data_layer_cut_gate.py`）

用 llvm-nm 读真实构建的对象文件，按明确规则把对象分成数据层、设备、特效、DXVK、ImGui 等类，
未知对象直接报错（不默认归类）。数据层集合闭包扩展：被数据层引用的非核心对象一并计入。

- **正向切口**：数据层 → 核心（设备/特效/DXVK/ImGui）的未定义符号。S2 要把它降到 0。
- **反向面**：核心 → 数据层的引用，即设备每个 draw 要问数据层的决定。S3 要把这些决定改成 x86 标注。
- `--baseline` 对比、`--ratchet` 时正向切口新增任一符号即失败。8 个纯单元测试通过。

基线 `x86-data-layer-cut-20261030-r2`（r1 因 `version` 对象未分类失败，保留为失败回执）：
构建 `build32_device_owner_fixed_function_20261029`，DLL `26B411E9…5A69`（24,420,352 B）。

| 量 | 值 |
|---|---|
| 数据层对象 | 114（70 个按规则 + 44 个闭包拉入） |
| 正向切口 | 113 个符号、49 条边、21 个对象（设备 42、特效 37、DXVK 41、ImGui 19） |
| 反向面 | 477 个符号、105 条边（`d3d9_device.cpp` 388、交换链 44、`d3d9_war3_shadow` 41、管线 33、天气/AA/体积光各 3） |

结论与计划一致：数据层本身大体可分离；难点在 `d3d9_device.cpp` 的逐 draw 决定。

## 2. 标注传输（D9C2 开发协议扩展）

Initialize `args[6]` bit1 `InitializeAnnotations` 协商（bit0 仍是 RetainContent，其余位仍须为 0）。
未协商时三个新 op 一律可恢复拒绝。记录只携带值，不携带游戏或 COM 指针。

| op | 内容 | 校验 |
|---|---|---|
| `FrameScene` | `args[0]` schema；当前只有 `FrameCamera`（view/projection/viewport/depthRange/frame/gameTime，160 B） | 字节数恰好、除帧号外全部有限、无多余参数、无资源 id |
| `StageMarker` | `args[0]` 阶段：WorldBegin/BeforeUi/UiBegin/FrameEnd | 值域、无负载、无多余参数 |
| `AnnotateDraws` | `args[0]` 记录数、`args[1]` draw 数、`args[2]` 绑定的 draw op；负载为记录序列 | 见下 |

`AnnotateDraws` 的记录头是 u16 批内序号、u16 schema、u16 字节数、u16 保留（=0），数据按 4 字节补零。
当前唯一的 draw schema 是 `DrawTag`（u64 数据层实例键、u32 kind、u32 flags，16 B）。宿主要求：

- draw op 只能是 Draw/DrawFixed（draw 数 1）或 DrawBatch（≤32）/DrawFixedBatch（≤20）；
- 序号小于 draw 数，`(序号, schema)` 严格递增（无重复、无乱序），保留与补零字节为 0，负载长度恰好；
- 记录只绑定**紧接着的下一条消息**：它必须是声明的 op，单条 draw 时 draw 数为 1，批次时 `args[0]` 等于 draw 数。
  其他任何消息到来时，该消息被拒绝、记录作废；draw 本身被拒绝时记录同样作废，不会借给后面的 draw。

代理侧（`d3d9_connection.h`，`WARVK_D3D9_ANNOTATIONS=1` 严格开启）：

- 导出 `WarVKD3D9AnnotateNextDraw`、`WarVKD3D9SetFrameScene`、`WarVKD3D9MarkStage`，供 x86 数据层调用；
  每个 draw 最多 4 条记录、schema 递增，本地先校验，非法输入不发送。
- 非批次模式：draw 之前先冲刷暂存批，再同步发 `AnnotateDraws(n,1,op)`。批次模式：记录随 draw 进入暂存批，
  按批内序号改写，冲刷时在批次前发一条 `AnnotateDraws`（异步批次下同样走队列）。任何非 OK 都 fail-closed。
- `FrameScene`/`StageMarker` 与其他非 draw 调用一样先冲刷暂存批，所以宿主看到的顺序就是调用顺序。
- 每个 Device draw 调用结束时丢弃未被消费的记录（计入 `discardedRecords`）。

宿主侧先接一个记录器消费者：按交付顺序对 draw 记录（连同该 draw 的图元类型、图元数、顶点数、起始索引）、
场景包和阶段标记做 FNV-1a，只在协商时输出 `annotation-host/v1` 行。fixture 用自己的代码按同样字段折叠，
摘要相等即顺序、负载和 draw 绑定都相等。冻结的 v1 行不变。

## 3. 测试

- **wire（x86/x64）**：标注编码、FrameCamera 有效性、记录追加、解码正例与每一种拒绝、绑定、满 32 条批次、
  摘要顺序敏感，共 238 项；三个新 op 编解码；Initialize 特性位 1/2/3 合法、4/5/高位非法。
- **CPU Connection**：新 worker `annotated-draw-batch`，核对批次前的 `AnnotateDraws(2,3,DrawBatch)`、
  阶段标记先冲刷、固定批 `AnnotateDraws(1,1,DrawFixedBatch)`，以及未协商、坏 schema/大小/空指针、
  阶段 0/5、NaN 相机全部不发送任何消息。
- **raw 协议**（`test_render_host_fixed_rejection.cpp`，`WARVK_TEST_RAW_ANNOTATE=1` 由 raw 对端自己协商）：
  - 正例：场景包、三个阶段、单条 DrawFixed 与 3 条 DrawFixedBatch（只标 0 号和 2 号，三条形状各不相同）；
  - 9 个场景/阶段拒绝、12 个畸形记录拒绝、6 个绑定拒绝（夹入阶段标记、op 不符、draw 数不符、重复
    AnnotateDraws、draw 自身非法、夹入 Readback），每个拒绝后跟一条普通 draw，证明没有残留；
  - 未协商时三个 op 都被拒绝；异步批次档位只跑同步可恢复的部分（异步下 `AnnotateDraws`/批次的拒绝按合同是终止性的）。
- **GPU fixture**（`test_render_host_d3d9_factory_clear.cpp`，`WARVK_TEST_ANNOTATE=1`，只走远端）：
  放在纹理 level 用例之后，用独立 VB 画 8 个形状各异的 draw（7 个带标注），中间插 BeforeUi，
  另有一个被拒绝的空 draw 带走一条记录、9 个本地拒绝；不读回，所以 297 张图的哈希不变。
  非标注远端用例确认三个导出在未协商时本地拒绝。

## 4. 结果

全部 fresh、BelowNormal、串行。

- **构建** `x64-annotation-build-20261030-r1`：`-j2`，181 s，sourcesPreserved、protectedUnchanged 通过。
  宿主 `94BAA54C6AB4ABA6B40728ECB276D2E674619A3A77C49A494579BF76CA8480AB`，5,894,144 B。
- **CPU 门** `x64-annotation-cpu-20261030-r1`：x86/x64 各 210,798 项（新 worker 多 64 项），wire 各 1,454,928 项。
- **GPU 门**（`--validation --stream --fixed-batch --backing --annotations`）：
  - r1 失败，保留为失败回执：raw 标注档位里被接受的那条 `DrawFixedBatch(3)` 也计入宿主固定批账户
    （14 批 / 59 条，期望写成了 13 / 56）。这是门禁漏写的期望；在它之前的标注、draw 与拒绝平衡检查都已通过。
    补上期望（+1 批、+3 条）后重跑。
  - r2 通过，回执 4,863,754 B / `8AF90468689828E54346C0A7F7EC06BAAB4A5D0BFCE910549DCBC2DB565032BA`。
  - VVL 1.4.365、同步校验开：16 个远端用例（9 个主 fixture 远端、3 个内存工作量远端、5 个 raw）均 0 错误、
    0 个非状态警告。
  - 11 张主 fixture 哈希全部仍为 `0627ed73c4fa582e`：标注用例不改图。

| 用例 | fixture | 代理 | 宿主 |
|---|---|---|---|
| `remote-annotate`（同步） | 8 draw、7 带标注、1 场景、3 阶段、9 个本地拒绝 | 7 条记录、7 条 `AnnotateDraws`、丢弃 1 | 7 条记录、宿主 draw 310 |
| `remote-stream-fixed-annotate` | 同上 | 7 条记录、2 条 `AnnotateDraws`（BeforeUi 分成两批）、丢弃 1 | 同上 |
| 摘要 | `4959d1f6d825e524` | — | 两档都是 `4959d1f6d825e524` |

  - raw 标注档：27 个可恢复拒绝、23 条 draw、4 条带标注，摘要 `6e28607a0c1350cd` 与宿主一致；宿主 draw
    176 = 97 + 56 + 23，拒绝 112 = 72 + 12 + 1 + 27。其余 4 个 raw 档未协商：3 个拒绝（异步流档 2 个），
    宿主没有标注行。
  - 7 个未标注的远端用例：3 个导出都在本地拒绝，代理与宿主都没有标注行。
- **资源寿命门** `x64-annotation-lifetime-20261030-r1`、**M0 内存门** `x64-annotation-memory-20261030-r1`：通过。
  内存门逐条核对交换序列，默认关闭时线上流量不变。

## 5. 不做的事、风险

- 只有记录器消费者，没有任何真实执行器；S3 的第一个效果（天气）才会消费 `FrameCamera`。
- `DrawTag` 是临时词汇；需要更多字段时新增 schema，不复用字段含义。
- 导出用 `dynamic_cast` 识别本代理的 Device，传入别的模块的对象属于调用方错误。
- 内容载荷（合同第 4 项）与共享内存命令环尚未开始。
