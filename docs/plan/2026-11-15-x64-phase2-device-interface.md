# x64 迁移阶段 2：设备接口形状

2026-11-15，Claude（Fable 5.2）。[整体路线](2026-11-13-x64-migration-roadmap.md) 阶段 2 的设计说明：定下数据层到执行端的接口形状，
并写清后续各组（阶段 3、4）照做的步骤。第一组的实现与门禁结果另记[回执](../research/2026-11-15-x64-phase2-device-interface.md)。

## 1. 要解决的问题

[S2 回执](../research/2026-10-31-x64-s2-forward-cut.md)已查明两件事：

- `D3D9DeviceEx` 是 `final` 类。调用处的静态类型只要是它，编译器就把调用去虚化成对 `d3d9_device.cpp` 的直接引用，
  数据层文件因此链接到具体设备。把成员改成虚函数没有用，必须让调用处**拿不到具体类型**。
- 数据层需要的不只是设备实例上的成员，还有一批不依赖设备的入口：`d3d9_device.cpp` 里的自由函数和静态成员、
  特效文件里的诊断查询、DXVK 的启动记录。其中两个必须在**没有设备时**也能用：启动失败报告（设备甚至接口都没建出来），
  以及换图时没有设备的 CPU 语义重置。

所以需要两样东西：一个“当前设备”的接口，和一个不依赖设备的入口。

## 2. 定下的形状

一个头文件 `src/d3d9/war3/core/war3_execution_services.h`（数据层），两个纯虚接口：

| 接口 | 内容 | 进程内由谁实现 | 数据层怎么拿到 |
|---|---|---|---|
| `war3::IDeviceServices` | 当前设备上的入口 | `D3D9DeviceEx` 自己（多继承这个接口，成员原样作为覆盖） | `war3::RunWithActiveDeviceServices(回调)`，回调参数是 `IDeviceServices&` |
| `war3::IProcessServices` | 不需要设备的入口 | `d3d9_war3_execution_services.cpp` 里的 `War3InProcessServices`，逐项转发给原函数 | `war3::ProcessServices()`，永不为空 |

**设备接口随设备一起发布。** `war3.cpp` 在发布和撤销当前设备的同一把锁、同一个事务里，多存一份 `IDeviceServices*`
（`s_activeDeviceServices`）。单独存一份而不是每次从 `D3D9DeviceEx*` 上转，是为了让经它发出的调用不带具体类型的来历，
编译器没有任何去虚化的依据。`RunWithActiveDeviceServices` 与原有的 `RunWithActiveDevice` 持同一把递归锁，
一次回调里的多个调用仍是一个事务（内部测试接口的“查询 → 重置 → 再查询”依赖这一点）。

**进程级入口走注册表。** 注册表在数据层（`war3_execution_services.cpp`），是一个原子指针，初值指向数据层自带的
“不可用”提供者（全部返回 0 或值初始化的结构体）。执行端的提供者在 DLL 加载时由一个静态对象注册。
用加载时注册而不是在 `Direct3DCreate9` 里显式调用，是因为启动失败报告可能在接口创建失败时就要用，
没有一个足够早又一定经过的显式调用点；漏注册的后果（诊断全 0、无设备重置不生效）也不容易被发现。
注册槽和两个提供者都是常量初始化的（`constexpr` 构造），所以不存在“先注册、后被别的静态初始化覆盖”的次序问题。

**规则：**

1. 数据层文件里不出现经 `D3D9DeviceEx` 的调用。需要当前设备就用 `RunWithActiveDeviceServices`，
   回调参数类型写 `IDeviceServices&`。
2. 接口方法与它代表的原函数**同名同签名**。设备成员因此不用改名、不用转发；调用处只换参数类型，函数体文字不变，
   钉住这些文字的静态测试继续有效。
3. 两个接口的成员全部是纯虚的。每个实现都必须逐项表态；设备成员的签名一旦漂移，`D3D9DeviceEx` 变成抽象类，
   `new` 它的地方编译失败。不给设备里的这些成员加 `override`：这个类的 COM 方法都没有标，混用会触发
   `-Winconsistent-missing-override`。
4. “不可用”的表示是值初始化（数值 0、结构体 `{}`）。诊断结构体里凡是有“未运行”语义的字段，默认值本来就是这个意思
   （例如体积光的 `stage = "diagnostics-disabled-or-not-run"`）。实机门拿到全 0 会判失败，不会误判通过。
5. 接口头只前置声明执行端的结构体，不包含它们的头文件；调用方本来就包含了。数据层自带的“不可用”提供者要构造这些结构体，
   所以它的 `.cpp` 包含执行端头文件——只用类型，不产生链接依赖。产品是同一个 DLL 的两种模式，这些头文件始终在。
6. 纯映射函数（枚举 → 名字）不走接口，直接内联进它所在的头文件（`War3WeatherDrawStageName`）。两端都需要它，也都能自己算。
7. 不要把 `RunWithActiveDevice` 重载出一个接口版本：`D3D9DeviceEx` 派生自接口，接收 `IDeviceServices&` 的 lambda
   对两个 `std::function` 签名都可转换，调用会有歧义。所以是两个名字。

## 3. 没有选的方案

- **只有注册表，不动设备类。** 进程内提供者在里面自己取当前设备再调成员。设备类一行不用改，但一次回调里的多个调用
  不再是一个事务（每个入口各自加锁），要保住事务就得为每种组合另写复合入口；代理模式下也还是得回到“当前设备对象实现接口”。
- **设备持有一个转发适配器成员。** 不改继承关系，成员偏移也不变，但每个入口多一层手写转发，没有换来别的东西。
  多继承一个无数据成员的接口只是让设备对象多一个虚表指针、其后的成员整体后移 4 字节，没有行为上的变化。
- **数据层留一个由执行端定义的访问函数（链接期接缝）。** 最省事，但正向切口永远剩一个符号，代理模式下也没法换实现。

## 4. 每个入口的去向

进程内全部是原函数，行为不变。代理模式一栏是这个接口将来由谁回答，决定了它们随哪个阶段落地。

`IDeviceServices`：

| 入口 | 调用方 | 代理模式 |
|---|---|---|
| `War3SetWorldLightingScope` | 世界 `RenderScene` 边界 hook，每帧两次 | 阶段标记 WorldSceneEnter / WorldSceneExit（传输层已有） |
| `War3RequestShadowMapEpochReset` | 换图信号（`RequestActiveDeviceShadowMapResetOrCpuFallback`） | 会话重置请求（传输层已有） |
| `War3AttachGpuSkinNativeBridge` | GPU 蒙皮三个原生 hook 安装成功后 | 随 4c（GPU 蒙皮） |
| `War3PopulateSemanticShadowSceneForValidation`、`War3ExecuteSemanticShadowSceneForValidation` | 语义阴影验证路径 | 随 4c；之前回答“未执行” |
| `QueryWar3ShadowLifecycleDiagnostics` | 诊断中心、内部测试接口 | 宿主回答，随 4c |
| `QueryWar3DeviceStatus` | 诊断中心（原来直接读 DXVK 设备状态） | x86 连接的终止原因（阶段 1 的 `TerminalReport`）加宿主回传的设备状态 |
| `War3ResetGpuSkinBridgeForTest`、`War3LogGpuSkinDiagnosticsForTest` | 内部测试接口 | 随 4c |

`IProcessServices`：

| 入口 | 原定义处 | 代理模式 |
|---|---|---|
| `War3ResetCpuSemanticMapSession` | `d3d9_device.cpp`（静态成员） | 拆开：数据层各 registry 的重置留在 x86，执行端缓存（雾体积、alpha-test 负载、直接包缓存）由会话重置消息带走。拆分随 4c |
| `QueryShadowAppend*`（3 个）、`QueryDevicePaletteSlotCache*Count`（2 个） | `d3d9_device.cpp` 的 draw 路径计数 | 这些计数来自逐 draw 判断，按拆分方案留在 x86 代理前端；随 4c 决定归属 |
| `TestFrameHistoryWindowShortcut` | `d3d9_window.cpp` | 窗口过程在 x86（代理已接管窗口消息），由代理前端实现 |
| `StartupReportSnapshot` | `dxvk_startup_report.cpp` | 设备在宿主创建，记录由宿主在握手或故障消息里回传；x86 只负责弹窗与写文件（阶段 5） |
| `QueryShadowTaaDiagnostics` 等 4 个阴影诊断 | `d3d9_war3_shadow.cpp` | 宿主回答，随 4c |
| `QueryWar3VolumetricExecutionDiagnostics`、`QueryWar3WeatherVolumeDiagnostics` | `d3d9_war3_volumetric_light.cpp` | 宿主回答，随 4a（天气体积）与 4d（体积光） |
| `QueryWar3WeatherDiagnostics` | `d3d9_war3_weather.cpp` | 宿主回答，随 4a |

“宿主回答”的做法到 4a 定模板时再定（一次同步查询带回结构体字节，结构体要先核对两种位宽下布局相同）。
在那之前代理模式的提供者一律回答“不可用”。

## 5. 后续各组照做的步骤

每一组从切口回执（`run_x86_data_layer_cut_gate.py` 的 `forward.edges`）里取本组的符号，逐个归入下面四种之一：

| 情况 | 做法 |
|---|---|
| 设备实例上的成员 | 在 `IDeviceServices` 加同名同签名的纯虚成员；调用处的回调参数类型换成 `IDeviceServices&`，改用 `RunWithActiveDeviceServices` |
| 自由函数、静态成员、别的单例上的查询或命令 | 在 `IProcessServices` 加纯虚成员；两个提供者各加一个覆盖（数据层的回答“不可用”，执行端的转发并写全限定名，否则会递归调到自己）；调用处前面加 `ProcessServices().` |
| 纯函数 | 内联进头文件 |
| 状态放错了层 | 不加接口，搬状态。例：雾体积的描述是地图脚本写的 CPU 状态，应归数据层，执行端每帧反向读取（将来进场景包）；查询直接读数据层自己的状态 |

然后：

1. 改完先对改动的翻译单元跑 `-fsyntax-only`（借用已有构建目录的 `compile_commands.json`），再跑读到这些文件的静态测试。
   钉住旧文字的断言要改就改，但先确认它钉的是合同而不是巧合。
2. 调用处如果还在 `if constexpr` 的丢弃分支里，现在不产生符号，也一并改：开关一翻它就回到切口里。
3. 接口上的默认实参按静态类型取值，调用处把实参写全，不依赖默认值。
4. `test_execution_services_static.py` 里有两个接口的成员个数和各文件的调用计数，随改动更新。
5. 新的 x86 构建（`run_device_owned_build.py`，全新构建目录）→ 切口门带 `--baseline <上一份> --ratchet` →
   重建宿主（宿主的构建回执覆盖 `src/`，源码一变旧回执就失效）→ 用新 DLL 作参照跑 GPU 参照门（主路径与 `--resize`）。
6. 写回执：去掉了哪些符号和边、新增了什么（反向面会多出执行端调数据层的注册入口，这是预期的）。

阶段 3 各组对应的情况：

| 组 | 符号 | 归类 |
|---|---|---|
| JASS 雾体积（`war3_shader_api` 16 个 + 内部测试接口 3 个） | `War3FogVolumeManager::*` | 状态放错了层：管理器的 CPU 状态搬到数据层 |
| shaderpack（4 个）、品牌提示（1 个） | `war3shader::*`、`TryShowBrandingMessage` | 进程级命令，进 `IProcessServices` |
| shaderpack 图像解码（2 个） | `stbi_*` | 不是执行端：把 `war3_shaderpack_image.cpp` 在切口门里重新归类，或把解码挪进数据层文件 |
| ImGui 覆盖层（19 个） | `ImGui::*`、`ImDrawList::*` | ImGui 留在 x86：切口门里 `imgui` 不再算执行端，后端提交绘制数据的那一步进 `IDeviceServices` |
| 实例化缓冲（设备 12 个 + 缓冲 2 个） | D3D9 API 11 个、`GetWar3InstanceBuffer`、`D3D9CommonBuffer::Lock/Unlock` | D3D9 API 改经 `IDirect3DDevice9`（S2 第一组的做法）；设备所有者的取得进 `IDeviceServices`；缓冲的 Lock/Unlock 改经 `IDirect3DVertexBuffer9` |
| 材质编译（设备 2 个 + DXSO 5 个） | `Create{Vertex,Pixel}Shader`、`DxsoModule`、`DxsoOptions` | 编译整体是执行端的事：一个“编译并返回着色器对象”的入口进 `IDeviceServices`，DXSO 的使用搬到执行端文件 |

剩下的 `DXVK GPU 对象` 一类（GPU 蒙皮资源、阴影 arena、帧历史、异步截图、性能监视器 GPU 计时、alpha-test 负载、显存档位）
不是接口问题，是代码归属问题，随阶段 4 的特效迁移拆文件。

阶段 3 实际的做法（11-15，[回执](../research/2026-11-15-x64-phase3-s2-groups.md)；正向 83/36 → 19/24）：

| 组 | 实际做法 |
|---|---|
| JASS 雾体积 | 照表：`war3/state/war3_fog_volume.{h,cpp}`。没有另建镜像，搬过去就是唯一的一份；执行端（设备换图清空、阴影、体积光）反向读它，反向面多 4 个符号、3 条边 |
| shaderpack、品牌提示 | 照表，`IProcessServices` 15 → 19。例外：“不可用”的两个命令回答 `INTERNAL_ERROR` 而不是值初始化（0 是 `OK`）；错误名是纯映射，内联进 `war3_shaderpack.h` |
| shaderpack 图像解码 | 走第一种：`war3_shaderpack_image.cpp` 在切口门里归新层 `stb`（可移植） |
| ImGui 覆盖层 | 只改切口门：`imgui` 移出执行端，归可增长层。数据层只用 ImGui 核心构建绘制列表，不碰后端，不需要 `IDeviceServices` 入口；覆盖层由谁驱动归阶段 5 |
| 实例化缓冲 | 照表：`m_device` 改为 `IDirect3DDevice9Ex*`，`GetWar3InstanceBuffer` 进 `IDeviceServices`，缓冲经 `IDirect3DVertexBuffer9` 加解锁 |
| 材质编译 | 照表，但只搬了 DXSO 预检与着色器创建（`d3d9_war3_material_shader.cpp`）；`d3dcompiler` 编译留在数据层（它是独立 DLL，不是执行端）。`IDeviceServices` 9 → 12 |

## 6. 验收

- 切口门 ratchet：正向只减不增；本组去掉的符号与边在回执里逐个列出。
- x86 构建通过，受保护文件不变，玩家目录的 `d3d9.dll` 仍不存在。
- 读到改动文件的静态测试通过（改了哪些断言写进回执）。
- GPU 参照门：新 DLL 作参照，像素哈希与上一轮相同，VVL 0 错误。它证明的是新 DLL 作为原版渲染器整体没变；
  本组改动的调用路径（诊断、测试入口、换图回退）不在夹具覆盖范围内，行为不变的依据是“同一个函数，经虚表调用”。
