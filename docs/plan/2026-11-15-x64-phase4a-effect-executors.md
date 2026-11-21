# x64 迁移阶段 4a：特效执行器模板

2026-11-15，Claude（Fable 5.2）。[整体路线](2026-11-13-x64-migration-roadmap.md) 阶段 4a 的设计说明：定下“一个特效怎样同时在进程内和
x64 宿主里执行”的模板，并写清阶段 4b 各特效照做的步骤。第一组（天气雪、SSAO）的实现与门禁结果另记
[回执](../research/2026-11-15-x64-phase4a-effect-executors.md)。

## 1. 要解决的问题

阶段 4 的验收是“同输入两种模式像素对照”。两种模式指同一个 x86 `d3d9.dll` 的进程内模式（特效在 x86 里用 DXVK 画）
和代理模式（x86 只发命令，x64 宿主画）。要让对照有意义，必须回答四件事：

1. **特效代码放哪。** 宿主不编译 `src/d3d9`（只有一份明确的允许清单）。如果宿主另写一份特效，对照比的是两份实现，
   以后每改一次都要改两处。
2. **特效的输入怎么过进程边界。** 进程内的 pass 从 `War3PipelineInput` 读设置、相机、天气快照，这些都是 x86 内存里的对象。
3. **宿主在回放流的哪一点执行特效。** 进程内靠 Game.dll 的渲染状态静态量判断“世界画完、UI 未开始”，宿主没有这些量。
4. **怎么对照。** 进程内的 BeforeUi 插入依赖 Game.dll，夹具进程里永远不会触发。

## 2. 模板

| 部件 | 定下的形状 | 文件 |
|---|---|---|
| 执行器 | 每个特效一份实现，只依赖 `DxvkDevice`、帧的颜色/深度视图、相机和它自己的记录。进程内的 pass 是它的薄适配器，宿主编译同一份源码 | `src/d3d9/war3/effects/war3_*_executor.{h,cpp}`、`war3_effect_frame.h` |
| 场景包 | 每帧的相机和各特效的记录，都是 `FrameScene` 消息；记录布局写在一个无依赖头文件里，两端与夹具共用 | `src/d3d9/war3/effects/war3_effect_scene.h` |
| 插入点 | 宿主收到 `StageMarker(BeforeUi)` 且场景包里有特效记录时执行，一个完成的世界场景只执行一次 | `d3d9_host_main.cpp`、`d3d9_backend.cpp::runBeforeUi`、`before_ui_effects.h` |
| 对照 | 一个夹具，同一组 draw、记录和标记，分别跑在进程内 DLL 和“代理 + 宿主”上，每帧回读，逐字节相同 | `AutoTest/test_render_host_d3d9_scene_effects.cpp`、`run_x64_scene_effects_gate.py` |

### 2.1 执行器

```
namespace dxvk::war3::effects
  struct EffectFrame { colorView, depthView, EffectCamera camera, frameSerial, mapEpoch, deviceEpoch }
  class XxxExecutor {
    explicit XxxExecutor(Rc<DxvkDevice>);
    Stage run(const Rc<DxvkCommandList>&, const EffectFrame&, const scene::Xxx& record,
              EffectRecording* recording = nullptr);
  }
```

规则（`AutoTest/test_scene_effects_static.py` 逐条钉住）：

- **不认识设备和游戏。** 执行器及 `war3_effect_frame.h` 只包含本目录、`war3/render` 里三个执行端辅助头
  （自有图像布局、被跟踪的管线、天气资源重试）、`src/dxvk`、`src/util` 和标准库。不出现 `D3D9DeviceEx`、
  `War3PipelineInput`、`War3RenderSettings`、性能监视器、环境变量、Game.dll。
- **先分配，后录命令。** 管线、采样器、拷贝图像、视图描述符都在第一条命令之前准备好；可恢复的分配异常只在这一段捕获。
  第一条命令之后没有 `catch`，也不再创建图像。失败时返回一个具名阶段，不录任何命令。
- **布局只读不猜。** 目标图像的当前布局用 `queryLayout` 读，`LayoutKnown` 拒绝未定义布局；用 `pickLayout` 选中间布局，
  结束时把颜色和深度都恢复成原布局并 `trackLayout`。宿主的目标是 `GENERAL` 布局、深度没有 `SAMPLED` 用途，
  进程内是附件布局，同一份代码两边都对。
- **命令里的裸句柄都有主。** 图像、采样器、管线用 `ctx->track` 交给命令列表持有；管线用 `War3TrackedVkPipeline`。
- **诊断留在适配器。** 执行器只返回阶段（和少量数字）。需要“恰好包住录下的命令”的计时时，适配器传一个
  `EffectRecording`，执行器在第一条命令前调 `beginRecording`（天气的 `Weather/Snow` 性能区间就是这样保持原样的）。

进程内的 pass（`d3d9_war3_ssao.cpp`、`d3d9_war3_weather.cpp`）只做三件事：按设置决定这一帧开不开、
把 `War3PipelineInput` 里的值抄成记录（`d3d9_war3_effect_adapter.h` 提供相机和帧的转换）、发布诊断。里面没有任何 Vulkan 命令。

### 2.2 场景包

`FrameScene` 消息的载荷。记录规则：

- 只有值：没有指针、句柄、指向 x86 状态的索引；
- 完整：除相机和目标外，执行器读的一切都在记录里；
- 只管一帧：记录属于发它的那一帧。**某个特效的记录不在，它这一帧就不执行**——数据层关掉一个特效的方式就是不发；
- 小端字；`float`、`double` 传 IEEE 位。

| 编号 | 记录 | 大小 | 内容 |
|---|---|---|---|
| 2 | `Camera` | 160 | view、projection（D3DMATRIX 内存序）、视口 x,y,w,h（浮点形式的整数像素）、minZ、maxZ、帧号、游戏时间。布局就是已有的 FrameCamera |
| 5 | `Ssao` | 24 | radiusPx、strength、bias、power、fadeNear、fadeFar |
| 6 | `Snow` | 76 | 11 个 float（snowfall、gust、haze、lowSnow、区域 minX/minY/maxX/maxY、bottomZ、topZ、edgeFeather）、3 个 double（timeSeconds、风位移 X/Y）、seed、id（≠0） |
| 7 | `Aa`（4b） | 28 | mode（1 = FXAA，2–5 = SMAA 四档；0 与其余值拒绝）、FXAA 三个 float、SMAA 阈值、两个搜索步数（int，执行器夹到设置界面的范围） |
| 8 | `Bloom`（4b） | 24 | threshold、softKnee、intensity（bloom 关 = 0）、exposure、ACES 开关、sRGB 开关（都只许 0/1；sRGB 只携带不读，见 4b 回执） |

雪记录里的 `haze`、`lowSnow` 只有雪粒的执行器在读（粒子的雾化与低空密度）。天气的雾和低空雪**介质**是体积光的 froxel 介质画的，
不在这个执行器里，随体积光归 4d；在那之前代理模式下天气只有雪粒，没有介质。

校验分两层。传输层（代理与宿主的 `ValidFrameScene`）：相机只要求大小对、浮点有限；特效记录用 `ValidEffectRecord`，
大小不对、非有限数、`id == 0` 都按协议错误拒绝。执行层：相机还要求视口是界内的整数像素且非空，否则 `FramePacket`
丢掉相机（连同上一份），需要相机的特效这一帧报“相机不可用”而不执行——相机不能用不是协议错误。

`FramePacket`（同一头文件）是一帧的记录集合：同编号后到的替换先到的；被拒绝的记录不改变已接受的。
**场景包不跨帧**：宿主在 `FrameEnd` 标记、`Present`、`SessionReset` 三处清空它。

### 2.3 阶段标记与插入点

`Stage { WorldBegin=1, BeforeUi, UiBegin, FrameEnd, WorldSceneEnter, WorldSceneExit }`。阶段 1 已经有世界场景的进入/退出标记和
`SceneTracker`；4a 把它们挪进 `war3_effect_scene.h`（代理协议头改成引用），并加上插入规则：

- 宿主收到 `BeforeUi`：场景包里**有特效记录**才是一次插入。此时必须满足 `canInsertBeforeUi()`：本帧有一个已完成的世界场景、
  当前不在场景里、这个场景还没插入过。不满足就拒绝（`D3DERR_INVALIDCALL`），什么都不执行。
- 场景包里没有特效记录时，`BeforeUi` 仍是普通标记，只进记录器。阶段 1 的各组用例因此不变。
- 一帧里第二个世界场景有自己的一次插入。帧结束时没用掉的插入机会不带到下一帧。

执行位置（`D3D9Backend::runBeforeUi`）：流模式下接着当前打开的命令列表录，于是特效正好夹在已录的世界 draw 和后面的 UI draw 之间；
非流模式下单独一个列表，像 clear 一样提交。两种情况都先 `beginExternalRendering()`，由上下文在下一个 draw 重新应用自己的状态。
帧序号用“第几次插入”（保证天气的重复帧保护不误伤），地图纪元用会话代数 + 1，设备纪元用宿主目标的代数。

**顺序**：SSAO 先，雪后——雪画在被遮蔽后的世界上，自己不被遮蔽。这就是进程内的注册顺序（`D3D9DeviceEx` 构造里
`"SSAO"` 在 `"Weather"` 之前），宿主的 `BeforeUiEffects::run` 和夹具入口照同一顺序。

### 2.4 两模式对照

进程内的 BeforeUi 插入靠 Game.dll 的状态触发，`War3RenderPipeline::Execute` 又牵连证据采集和 shader API 监听，夹具里驱动不了。
所以给进程内 DLL 加一个**只给夹具用的私有 COM 接口** `IWarVKSceneEffectsFixture`（`d3d9_war3_scene_effects_fixture.{h,cpp}`）：

- 只有环境变量 `DXVK_WAR3_SCENE_EFFECTS_FIXTURE=1` 时 `QueryInterface` 才认它的 IID；没有新导出函数；
- 两个方法 `SetFrameScene`、`MarkStage`，语义与代理的 `WarVKD3D9SetFrameScene`、`WarVKD3D9MarkStage` 相同，
  用同一个 `FramePacket` 和 `SceneTracker`，所以拒绝规则逐条一致；
- 插入时调 `D3D9DeviceEx::War3RunSceneEffectsForFixture`：用当前 RT0/深度和记录拼一个 `War3PipelineInput`，
  在 CS 线程上直接跑**设备自己注册的** `War3SsaoPass`、`War3WeatherPass`（即玩家用的适配器 + 执行器），
  之后做与游戏内插入相同的状态恢复。它绕过的只有 `War3RenderPipeline::Execute` 的调度。

对照门 `AutoTest/run_x64_scene_effects_gate.py`：

1. 夹具画一个类 War3 的场景（地面加三个盒子，CPU 上按一份相机投影成 XYZRHW 顶点，所以深度缓冲和发出去的相机一致），
   用同一份相机发 `Camera` 记录；
2. 帧序列：只有世界、SSAO、雪、两者、两者（时间推进）、关闭；每帧 BeforeUi 之后再画一个 UI 小方块；每帧回读写成 `frame-<名>.bgra`；
3. 夹具自己的断言：特效改变了像素；UI 方块逐像素没被碰；没有记录的帧等于只有世界的帧；七种应被拒绝的输入都被拒绝；
4. 门禁比较：代理的每个传输变体（直连、流、流 + 命令环）的每一帧与进程内参照**逐字节相同**；
5. 分账：x86 进程的内存采样（世界、开特效后、计时后）和帧时间中位数（只有世界对比两个特效），宿主侧执行器的录制耗时和
   资源字节。只记数字，不设阈值；
6. 深度模板合一的参照组：宿主只有一种深度格式（D32F），两模式对照用它。进程内另跑一组 `--depth-stencil`，把深度换成 D24S8
   （深度和模板在同一张图里），让执行器的“两个 aspect 一起转布局、只拷深度 aspect”那条路径也被执行到。这一组没有宿主对应物，
   只要求夹具自己的断言通过；它与 D32F 参照的像素距离记数字、不判定。

校验层只挂在宿主会话上（固定的 VVL 是 64 位的）。进程内参照的命令流没有校验层覆盖，这是这个方法的已知缺口：
同一份执行器在宿主里经过校验，但进程内走的是另一组布局分支（附件布局、深度可直接采样）。

## 3. 阶段 4b 照做的步骤

对每个特效（SMAA/FXAA、Bloom/后处理、描边、局部雾）。4b 实际只做了前两个：描边重放阴影投射体的 draw，局部雾是体积光的
froxel 介质，两者改归 4c 之后的 4d，见 [4b 回执](../research/2026-11-15-x64-phase4b-aa-bloom.md)。

1. **读现有 pass，列出它读的东西。** 分成三类：目标（颜色、深度）；相机；其余（设置里的参数、数据层的快照）。
   “其余”就是这个特效的记录。读 Game.dll、读别的 pass 的中间结果、读历史帧的，先停下来看 §4。
2. **在 `war3_effect_scene.h` 加记录。** 取下一个编号（7 起），写 `struct`、`XxxBytes`、`EncodeXxx`/`DecodeXxx`
   （非有限数拒绝）、接进 `ValidEffectRecord` 和 `FramePacket`（`hasXxx`、`accept`、`effects()`）。
   在 `d3d9_annotation_wire.h` 的 `AnnotationSchema` 加同编号并补 `static_assert`。
   在 `AutoTest/test_render_host_annotation.h` 用**手写字节**加布局检查（照 Ssao、Snow 那两段）。
3. **写执行器。** 把 pass 里录命令的部分搬进 `war3_xxx_executor.{h,cpp}`，照 §2.1 的规则：`input.` 换成 `frame.` 和记录字段；
   布局改成读/选/恢复；分配全部提前。着色器不动。
4. **把 pass 改成适配器。** 保留设置开关、诊断、性能区间；删掉所有 Vulkan 命令。
5. **接进两边的构建。** `src/d3d9/meson.build` 加执行器源文件；`tools/render_host/dxvk_core/meson.build` 加同一个文件；
   着色器加进 `subprojects/war3fx/meson.build` 的 `war3fx_executor_shaders`（它是 `war3fx_shaders` 的子集，两处都要有）。
   `run_render_host_core_gate.py` 的允许清单已经按目录放行 `src/d3d9/war3/effects/*.cpp`；执行器若需要 `war3/render` 里新的
   辅助源文件，要显式加进允许清单和宿主构建。
6. **接进宿主。** `before_ui_effects.h` 加成员并在 `run` 里按**进程内注册顺序**调用；`BeforeUiOutcome` 和宿主日志行加它的阶段。
   夹具入口 `War3RunSceneEffectsForFixture` 里按同一顺序加这个 pass（设置快照里对应的开关由“记录在不在”决定）。
7. **扩夹具和门。** `test_render_host_d3d9_scene_effects.cpp` 加它单独一帧、与其他特效叠加一帧、关掉一帧；
   `run_x64_scene_effects_gate.py` 的期望计数随之改（`FRAMES`、`DRAWS`、`EFFECT_RECORDS`、`BEFORE_UI_RUNS`、各特效的
   `CommandsRecorded` 阶段号）。跑门：每帧两模式逐字节相同、带校验层的会话 0 错误 0 警告、D24S8 参照组通过。
8. **跑回归。** x86 构建、切口门（`--ratchet`，正向切口不许回升）、宿主构建、GPU 参照门、特效对照门；
   回执里记像素哈希、内存与帧时间分账，以及没覆盖到的部分。

## 4. 这个模板没有回答的事

留给后面的阶段，照做时遇到就要先定规则，不要顺手绕过去：

- **只有 BeforeUi 一个插入点。** 进程内目前也只有这一个。需要 UI 之后或呈现之前执行的特效要先加标记的规则
  （`UiBegin` 目前不带规则）。
- **读历史帧或别的特效中间结果的特效**（TAA、依赖阴影图的体积光）。执行器之间要共享的图像归谁、活多久，属于 4c/4d。
- **多采样目标。** 两个执行器都拒绝多采样目标（返回 `UnsupportedSamples`）。SSAO 以前不拒绝，而是按目标的采样数建管线；
  但它的拷贝图一直是单采样的，多采样目标本来也拷不过去。这是一条写明了的限制，不是新能力的丢失。
- **代理模式下谁发记录。** 夹具直接发。游戏里由 x86 数据层在每帧发相机和各特效记录、在世界场景结束处发 `BeforeUi`，
  这部分接线属于阶段 5（首次接线）。现在游戏走的仍是进程内路径。
- **记录的生产者在哪一帧取值。** 进程内天气用的是冻结快照（带帧序号和纪元）；跨进程后“快照属于哪一帧”由消息顺序保证，
  宿主不再核对 x86 的帧序号。接线时要保证相机和记录在同一帧的 `BeforeUi` 之前发出。
- **不走 Vulkan 的 pass。** 以前的 Bloom 是用 D3D9 API 画的（运行时用 d3dcompiler 编 HLSL，渲染到 D3D9 纹理）。
  代理只允许渲染到后备缓冲，这种 pass 不能原样留在 x86：4b 把它移植成 Vulkan 执行器，进程内也改用执行器，
  旧实现只留作对照门的参照（夹具的“旧 Bloom”开关）。
- **x86 与 x64 的浮点一致性。** 执行器里的 CPU 浮点（矩阵求逆、参数量化）在两种位宽下必须给出相同的位，像素对照才会逐字节相同。
  目前靠对照门本身来发现；加特效时避免依赖 x87 扩展精度或 `long double`。
