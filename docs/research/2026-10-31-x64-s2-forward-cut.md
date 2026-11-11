# x64 增强层拆分 S2：正向切口（第一组）

2026-10-31，Claude 实施。[拆分计划](../plan/2026-10-30-x64-enhancement-split.md) §2 测出的正向切口，
是数据层到核心（设备、DXVK、特效、ImGui）的链接符号。S2 按类别逐类把它们归零，要求行为不变。
本组只改 `src/` 里 4 个子系统的函数签名。没有接游戏，没有部署玩家 DLL，没有提交、推送或发布。Game.dll guard 保持。

## 1. 基线按处理方式拆分

r2 基线有 113 个唯一符号、49 条边。同一符号可能被多个文件引用，按边计共 139 条：

| 类别 | 条目 | 边 | 内容 | 处理 |
|---|---:|---:|---|---|
| 经具体设备类调 D3D9 API | 25 | 5 | 实例化缓冲 11、材质 7、帧截图 4、着色器管理 2、队伍色 1 | 改为经 `IDirect3DDevice9` 调用（第一组，§2） |
| 设备内部入口 | 17 | 8 | 见下 | 见下 |
| 数据层驱动特效 | 37 | 10 | 见下 | 发往宿主的单向命令；getter 在 x86 留镜像状态 |
| 调试 UI 绘制 | 19 | 2 | `war3_shader_api` 经 ImDrawList 画覆盖层 | ImGui 留在 x86，把绘制数据序列化给宿主 |
| DXVK GPU 对象 | 41 | 25 | GPU 蒙皮资源与包存储、阴影 arena、帧历史、异步截图、性能监视器 GPU 计时、alpha-test 负载，以及材质的 DXSO 编译 | 属于执行端，迁到宿主 |

"设备内部入口"包括：
- 实例化缓冲的设备所有者，以及 `D3D9CommonBuffer` 的 Lock/Unlock；
- GPU 蒙皮原生桥；
- 阴影追加与调色板缓存的诊断查询；
- 阴影与语义地图的测试/验证入口；
- 帧历史快捷键测试。

"数据层驱动特效"包括：
- JASS 雾体积 19（其中 3 个是测试 getter）；
- 天气、体积光、阴影诊断 11；
- shaderpack 4、shaderpack 图像解码 2、品牌提示 1。

## 2. 第一组：只用 D3D9 API 的子系统改经接口调用

### 问题

`D3D9DeviceEx` 声明为 `final`，所以经 `D3D9DeviceEx*` 的 COM 方法调用会被编译器去虚化，变成对 `d3d9_device.cpp` 的直接引用。
数据层文件因此链接到具体设备类。代理模式下拿到的是代理设备，这些调用必须改走接口。

### 设计

函数签名改成接收 `IDirect3DDevice9*`。调用方传入的 `D3D9DeviceEx*` 会隐式上转，经 vtable 执行的是同一个函数。
签名必须改：如果仍收具体类、只在调用处转型，同一编译单元里编译器知道动态类型，照样会去虚化。

| 文件 | 改动 | 去掉的符号 |
|---|---|---|
| `war3/reimpl/war3_team_color_manager.{h,cpp}` | `Initialize` 和成员改为接口 | CreateTexture |
| `war3/shader/war3_shader_manager.{h,cpp}` | `applyGlobalUniforms` 改为接口；编译仍用 `m_device` | （与材质共有的两个常量上传） |
| `war3/shader/war3_material.{h,cpp}` | `apply`、`applyConstantStore` 改为接口；`compile` 不变 | SetPixelShader、SetRenderState、Set{Vertex,Pixel}ShaderConstantF |
| `war3/tools/war3_frame_capture.{h,cpp}` | `ProcessPendingFrameCapture`、`CaptureSurfaceToBmp` 改为接口 | StretchRect、CreateRenderTarget、CreateOffscreenPlainSurface、GetRenderTargetData |

这一组没有动下面两处：
- **实例化缓冲**：它用私有引用持有具体的 `D3D9VertexBuffer`（设备所有者，不持有设备）。改成公开引用会改变 DEFAULT 池资源的 losable 计数，从而影响 Reset。
  它属于计划里"按 draw 同步决定、会改变游戏行为的拦截"，要随设备侧的拥有者设计一起处理。
- **材质编译**：用到 DXVK 的设备、选项和常量布局（DXSO 诊断），属于设备侧。CreateVertexShader/CreatePixelShader 两个符号留在这条边上。

### 结果

按顺序运行，BelowNormal、`-j2`：

| 回执 | 结果 |
|---|---|
| `x86-s2-cut-build-20261031-r1` | `build32_device_owner_s2_cut_20261031`，ninja 476 s；DLL `67DD5D8F…7B50`，24,420,352 B（与基线同大小）；回执 `7859F49D…8DB1` |
| `x86-data-layer-cut-20261031-r1` | `--baseline` r2、`--ratchet` 通过：正向 113 → 104 个符号、49 → 46 条边，新增 0，删掉的正是上表 9 个；反向 477 个、105 条不变；回执 `96A1DEB8…89C3` |
| 静态测试 | `test_native_d3d9_survey_static.py`、`test_shadow_non_taa_continuity_static.py` 共 45 项通过（它们读帧截图的源文件和头文件） |
| `x64-s2-reference-gpu-vvl-20261031-r1` | **失败，保留**（`0F59C4E5…99EA`）："host source drift"。x64 宿主的构建回执覆盖了 `src/`，G7 的宿主构建因此失效 |
| `x64-s2-host-build-20261031-r1` | 按当前源码重建宿主，`4A30903E…8CF4`；回执 `C350D7FB…9737` |
| `x64-s2-reference-gpu-vvl-20261031-r2` | 用新 DLL 作 fork 参照跑主路径和 `--resize`，通过，回执 `411FDA2B…B317`。主路径 11 个哈希仍是 `03f7265ab853783c`；resize 两组仍是 `b3ae0d54a133b4b5`、`c31a6277983e7c78`，与 G7 用旧参照时相同；VVL 0 错误 |

宿主的 compile_commands 里没有这 4 个文件，所以宿主的漂移只是回执覆盖范围造成的；但重建后宿主哈希仍然变了，
说明在不同构建目录下宿主构建不是逐位一致的，原因没有深究。玩家 DLL 未部署、未改变（`1FFE0BD1…35CF`）。

### 未覆盖

- 改动的调用路径（队伍色贴图数组、shaderpack 材质绑定、帧截图）不在任何夹具的覆盖范围内。参照门只说明新 DLL 作为原版渲染器整体没变。
  行为不变的依据是：同一对象经 vtable 调用同一个函数。没有在游戏里跑。
- 实例化缓冲和材质编译还经具体类调用，原因见上。

## 3. 下一步

剩下的四类都要在数据层和核心之间引入接口，在进程内模式下由现有实现提供（行为不变）；代理模式再换成发往宿主的实现：

1. **诊断与测试入口**（设备内部入口 17 中的 13 个，加上特效诊断 11 个）：由设备或特效在创建时注册一个诊断提供者，数据层只经接口查询。
   代理模式下改由宿主回答，或者回答"不可用"。
   - 已查明的调用方式：`war3_shadow_runtime_bridge`、`war3_diagnostics_hub`、`war3_internal_test_api`、`war3.cpp` 拿到的都是 `D3D9DeviceEx&`。
     `War3ResetCpuSemanticMapSession` 是静态成员；`war3_diag::Query*` 是定义在 `d3d9_device.cpp` 里的自由函数。
   - 只把成员改成虚函数是不够的：调用处的静态类型仍是 `final` 类，编译器照样会去虚化。
     所以设备指针的来源也要改成接口类型，会动到 `d3d9_device.h` 的类层次和几个大文件。
     这一组要先定接口形状（设备实现一个纯虚服务接口，还是另建注册表），再动手。
2. **JASS 雾体积与 shaderpack**：数据层只经命令接口发出；getter 读 x86 侧的镜像状态。
3. **ImGui 覆盖层**：ImGui 留在 x86，绘制列表经接口交出。
4. **DXVK GPU 对象**：拆分 CPU 与 GPU 部分，GPU 部分归执行端；这一类随 S3 的特效迁移一起做。

每组照本组的方式验收：切口门 ratchet、x86 构建、相关静态测试，再用新 DLL 作参照跑一次 GPU 门。
