# x64 迁移阶段 3 回执：S2 其余分组

2026-11-15，Claude（Opus 5.5）。对应[整体路线](../plan/2026-11-13-x64-migration-roadmap.md)阶段 3；
照[阶段 2 设计](../plan/2026-11-15-x64-phase2-device-interface.md) §5 的步骤与分组做。
全部改动未提交；没有接游戏；没有碰玩家目录（`E:/Work/Warcraft III/d3d9.dll` 仍不存在，门禁记为“不存在且必须保持不存在”）。

## 1. 结论

- **正向切口 83 → 19 个符号、36 → 24 条边，新增 0。** 去掉的 64 个符号正是设计 §5 列的五组（§4）。
- **剩下的只有“DXVK GPU 对象”一类**（路线图阶段 3 第 4 项的终点）：GPU 蒙皮资源与包仓库、阴影 arena、显存档位、
  alpha-test 负载、异步截图、帧历史、性能监视器的 GPU 计时，共 9 个数据层文件到 10 个 DXVK 文件。
  它们不是接口问题，是代码归属问题，随阶段 4 拆文件。
- **反向面多了 4 个符号、3 条边**，都是执行端（设备、阴影、体积光）读数据层里的雾体积管理器。这是“状态放错了层”的
  搬法预期的结果：雾体积描述归 x86，执行端每帧反向读快照，代理模式下这份快照进场景包。
- **新 DLL 作为原版渲染器没有变**：GPU 参照门 48 个像素/呈现哈希与 4b 相同、VVL 0 错误；场景特效门 154 个哈希与 4b 相同；
  宿主与 4b 只差链接时间戳（§3）。
- **改动的调用路径没有夹具覆盖，没有在游戏里跑**（§5）。

## 2. 改动

### 2.1 JASS 雾体积：状态搬到数据层

| 文件 | 内容 |
|---|---|
| `src/d3d9/d3d9_war3_fog_volume.{h,cpp}` → `src/d3d9/war3/state/war3_fog_volume.{h,cpp}` | 原样搬家，除包含路径外代码不变；头文件加一段说明它是数据层状态 |
| `d3d9_war3_shadow.cpp`、`d3d9_war3_volumetric_light.h`、`war3_shader_api.cpp`、`war3/render/tests/war3_fog_volume_manager_test.cpp` | 包含路径 |
| `war3/tools/war3_internal_test_api.cpp` | 原来经体积光头文件间接包含管理器，改为直接包含 |
| `src/d3d9/meson.build` | 主列表与单元测试列表换路径 |
| `AutoTest/test_war3_local_volumetric_fog_static.py` | 两个路径常量 |

JASS native（`war3_shader_api.cpp` 16 个）与内部测试接口（3 个）的写入和查询现在都落在数据层自己的对象上，不跨层。
设计 §5 写的是“改成单向命令，查询读 x86 侧镜像状态”；搬状态后 x86 侧就是唯一的一份，不需要再建镜像。

### 2.2 shaderpack 命令与品牌提示：进 `IProcessServices`

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/core/war3_execution_services.h` | `IProcessServices` 加 4 个纯虚成员：`LoadShaderPack`、`EnableShaderPack`、`SetParamVec4`、`TryShowBrandingMessage`（15 → 19）；`ShaderPackError`/`ParamSlot` 前置声明 |
| `src/d3d9/war3/core/war3_execution_services.cpp` | “不可用”提供者：两个命令回答 `INTERNAL_ERROR`，`EnableShaderPack` 回答 `false`，品牌提示不做事 |
| `src/d3d9/d3d9_war3_execution_services.cpp` | 进程内提供者转发到 `::war3shader::*` 与 `War3VKBranding::TryShowBrandingMessage`；文件开头定义 `WAR3_SHADER_API_INTERNAL`（DLL 内部调用，不走 dllimport） |
| `src/d3d9/war3_shaderpack.{h,cpp}` | 错误码名字是纯映射，内联进头文件（`ShaderPackErrorName`）；导出的 `GetErrorString` 保留，改为调它 |
| `src/d3d9/war3/war3_user_example.cpp` | 4 处命令改经 `ProcessServices()`，错误名用内联映射 |
| `src/d3d9/war3/hooks/war3_hook_render.cpp` | 两处品牌提示改经 `ProcessServices()`，不再包含 `d3d9_war3_branding.h` |

### 2.3 实例化缓冲：COM 接口 + 设备入口

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/reimpl/war3_instance_buffer.h` | `m_device` 的类型由 `D3D9DeviceEx*` 改为 `IDirect3DDevice9Ex*`（`D3D9DeviceEx` 是 `final`，经具体类型调用会被去虚化成直接调用） |
| `src/d3d9/war3/reimpl/war3_instance_buffer.cpp` | 设备上的 11 个 D3D9 API 因此改经虚表；`Get()` 经 `IDeviceServices&` 取设备所有的缓冲；`GetActive(device)` 按 COM 指针比较；缓冲的 `Lock`/`Unlock` 改经 `GetBuffer()`（`IDirect3DVertexBuffer9`） |

### 2.4 材质着色器：编译留数据层，DXSO 与创建进执行端

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/core/war3_execution_services.h` | `IDeviceServices` 加 `GetWar3InstanceBuffer` 与 `War3CreateMaterialVertexShader`/`War3CreateMaterialPixelShader`（9 → 12） |
| `src/d3d9/d3d9_device.h` | 两个新成员的声明 |
| `src/d3d9/d3d9_war3_material_shader.cpp`（新，执行端） | 从 `war3_material.cpp` 搬来的 DXSO 预检、`Create*Shader` 与失败诊断，错误文字不变 |
| `src/d3d9/war3/shader/war3_material.cpp` | `d3dcompiler` 编译留在原处；拿到字节码后经 `IDeviceServices` 创建；不再包含 DXSO 头文件（+11 / −60） |
| `src/d3d9/meson.build` | 新源文件 |

### 2.5 切口门的层：ImGui 与 stb 不再算执行端

| 文件 | 内容 |
|---|---|
| `AutoTest/run_x86_data_layer_cut_gate.py` | `CORE` 去掉 `imgui`；`imgui` 归 `GROWABLE`（数据层用到就拉进数据集）；`war3_shaderpack_image.cpp`（只有 stb_image 的实现）归新层 `stb`，属 `PORTABLE` |

依据：数据层（JASS 画线/画字 native）用到的只是 ImGui 核心，即构建绘制列表的纯 CPU 代码；stb_image 是纯 CPU 解码。
两者两端都能链接。只改归类、不改代码，就去掉了 21 个符号，且没有产生新边：拉进数据集的是 ImGui 核心的 4 个对象
（`imgui.cpp`、`imgui_draw.cpp`、`imgui_tables.cpp`、`imgui_widgets.cpp`），它们不引用设备与 DXVK。
把绘制列表画出去的 DX9 后端（只经 `IDirect3DDevice9`）与驱动覆盖层的 `War3Imgui`（由设备在换帧、重置、拆除时调用，
自己持有具体设备类型）只经设备到达，不在数据集里。

### 2.6 静态测试

| 文件 | 改了什么 |
|---|---|
| `AutoTest/test_execution_services_static.py` | 成员个数 9/15 → 12/19；进程内转发的正则加 `::war3shader::`；`WAR3_SHADER_API_INTERNAL` 在第一个 `#include` 之前；“不可用”回答 `INTERNAL_ERROR` 两处；新增 `test_phase3_callers_use_the_interfaces`（各调用处只经接口、实例化缓冲不再持有具体设备、材质文件里没有 DXSO） |
| `AutoTest/test_device_owned_lifetime_contract.py` | 两条钉旧调用文字的断言：`device ? device->GetWar3InstanceBuffer() : nullptr` → 经 `IDeviceServices&` 的同一逻辑；`s_activeBuffer->m_device == device` → 按 COM 指针比较。钉的合同（每设备一份、私有）不变 |
| `AutoTest/test_x86_data_layer_cut_gate.py` | `war3_shaderpack_image.cpp` 归 `stb`；新增 `test_layer_sets`（`CORE` 恰为设备/特效/DXVK，三组互不相交） |
| `AutoTest/test_d3d9_connection_async_cpu_reader.py` | 与本阶段无关的陈旧断言：阶段 1 给 `run_d3d9_connection_async_cpu.py` 加了 14 个命令环/内容载荷用例，读取器测试还钉着旧的 12 个。按 runner 同步 |

## 3. 门禁结果

全部按顺序运行，BelowNormal、`-j2`，同一时刻只有一个构建或门。都是第一轮通过，没有失败回执。

| 回执 | 结果 |
|---|---|
| `x86-phase3-build-20261115-r1` | 全新构建目录 `build32_device_owner_phase3_20261115_a`，配置 18 s、ninja 742 s，精确目标再跑为 no-work。DLL `E6B1DBA3…E94D`，24,625,152 B（与 4b 的 `81FD9FF1…4A4D` 大小相同）。保护清单不变，玩家 DLL 构建前后都不存在。回执 `0F6A6A1C…186A` |
| `x86-data-layer-cut-phase3-20261115-r1` | `--baseline` 4b 的 `x86-data-layer-cut-phase4b-20261115-r1`、`--ratchet` 通过：正向 83 → 19 个符号、36 → 24 条边，新增 0；反向 483 → 487 个符号、113 → 116 条边。数据层对象 75 → 76（雾体积），数据集 119 → 124（拉进 44 → 48 个，多的是 ImGui 核心 4 个）。回执 `63303772…B7B2` |
| `x64-phase3-host-build-20261115-r1` | 宿主的构建回执覆盖 `src/`，源码变了必须重建。`build64_render_host_phase3_20261115_a`，配置 19 s、ninja 255 s；宿主 `F462D97A…0BF6`，6,291,456 B。与 4b 的宿主 `FA76D030…4D01` 逐字节比只差 4 个字节（PE 头与 build-id 里两处链接时间戳），本阶段没有改到宿主的代码。源码与保护清单前后一致。回执 `6DD36610…5FDD` |
| `x64-phase3-reference-gpu-vvl-20261115-r1` | 新 x86 DLL 作参照，`--validation --repin-loader --stream --fixed-batch --annotations --command-ring --backing --ring --resize`，通过。回执 10,428,997 B，`EA84A805…3D90`。与 4b 的 `x64-phase4b-reference-gpu-vvl-20261115-r1` 逐路径比：36 个像素哈希、12 个呈现哈希、21 个摘要、8 个标注摘要全部相同。38 个带校验层的宿主会话 0 错误、0 警告；线格式测试每种位宽 1,455,967 项、命令环单元测试每种位宽 267,078 项 |
| `x64-phase3-scene-effects-20261115-r1` | 补充：新 DLL 作进程内臂，`--validation --repin-loader`，通过，两模式逐字节相同。与 4b 的回执逐路径比 154 个哈希全部相同（含 Bloom 旧实现参照臂距离 0）；宿主账本相同（特效记录 112、BeforeUi 33 次、资源 336,896 B）；3 个带校验层的宿主会话只有启用提示，0 错误、0 警告。回执 `B9394D13…E8DA` |

静态测试：全套 397 个模块、3,537 项。改完断言后 16 个模块失败，逐个在 `git archive HEAD` 的快照上重跑：
15 个失败的测试项与 HEAD 完全相同（环境：8.3 短路径与临时目录、缺 `run_frame_timeline_diagnostic` 模块、
缺 `build32` 构建产物；陈旧：宿主 meson、插桩锚点、Vulkan 加载器路由、内存分配器、GPU 蒙皮快照等断言）；
`test_surface_owner_continuation` 在快照上先因快照位置报缺文件，在工作区报冻结输入 `test_render_host_d3d9_init.cpp` 的哈希变了，
而这个文件与 HEAD 相同。没有本阶段引入的失败。

## 4. 去掉的符号与边

64 个符号：

- JASS 雾体积 17 个：`War3FogVolumeManager::` 的 `Instance`、`AddBox`、`AddSphere`、`AddCylinder`、`Remove`、`Clear`、`SetActive`、
  `SetPosition`、`SetBoxSize`、`SetSphereRadius`、`SetCylinderSize`、`SetRotationDegrees`、`SetDensity`、`SetEdgeFeather`、
  `GetGeneration`、`GetVolumeCount`、`IsAlive`。
- shaderpack 4 个：`LoadShaderPack`、`EnableShaderPack`、`SetParamVec4`、`GetErrorString`；品牌提示 1 个：`War3VKBranding::TryShowBrandingMessage`。
- stb 2 个：`stbi_load_from_memory`、`stbi_image_free`。
- ImGui 19 个：`ImDrawList::` 13 个（`AddLine`、`AddRect`、`AddRectFilled`、`AddCircle`、`AddCircleFilled`、`AddTriangle`、
  `AddTriangleFilled`、`AddPolyline`、`AddBezierCubic`、`AddText`、`AddImage`、`PushClipRect`、`PopClipRect`），
  `ImGui::` 6 个（`GetIO`、`GetCurrentContext`、`SetCurrentContext`、`GetBackgroundDrawList`、`GetForegroundDrawList`、`ColorConvertFloat4ToU32`）。
- 实例化缓冲 14 个：`D3D9DeviceEx::` 的 `CreateVertexBuffer`、`CreateVertexDeclaration`、`SetStreamSource`、`SetStreamSourceFreq`、
  `GetVertexDeclaration`、`SetVertexDeclaration`、`GetVertexShader`、`SetVertexShader`、`GetTexture`、`SetTexture`、`DrawIndexedPrimitive`、
  `GetWar3InstanceBuffer`，`D3D9CommonBuffer::Lock`、`Unlock`。
- 材质着色器 7 个：`D3D9DeviceEx::CreateVertexShader`、`CreatePixelShader`，`DxsoModule` 的构造、`analyze`、`compile`，`DxsoOptions` 的两个构造。

12 条边：

| 从 | 到 |
|---|---|
| `war3_shader_api.cpp` | `d3d9_war3_fog_volume.cpp`（16）、`imgui.cpp`（6）、`imgui_draw.cpp`（13） |
| `war3_internal_test_api.cpp` | `d3d9_war3_fog_volume.cpp`（3） |
| `war3_user_example.cpp` | `war3_shaderpack.cpp`（4） |
| `war3_hook_render.cpp` | `d3d9_war3_branding.cpp`（1） |
| `war3_lightning_runtime.cpp` | `war3_shaderpack_image.cpp`（2） |
| `war3_instance_buffer.cpp` | `d3d9_device.cpp`（12）、`d3d9_common_buffer.cpp`（2） |
| `war3_material.cpp` | `d3d9_device.cpp`（2）、`dxso_module.cpp`（3）、`dxso_options.cpp`（2） |

反向新增的 3 条边：`d3d9_device.cpp`（换图时 `Clear`）、`d3d9_war3_shadow.cpp`（`HasActiveVolumes`）、
`d3d9_war3_volumetric_light.cpp`（`GetFrameSnapshot`、`HasActiveVolumes`）→ `war3/state/war3_fog_volume.cpp`。

剩下的 19 个符号、24 条边（全部到 DXVK）：

| 数据层文件 | 用到的 DXVK |
|---|---|
| `war3_gpu_skin_resources.cpp`、`war3_persistent_gpu_package_store.cpp`、`war3_persistent_gpu_package_d3d9_observe_owner.cpp` | `DxvkDevice::createBuffer/createFence`、`DxvkFence`、`DxvkMemoryAllocator::freeAllocation` |
| `war3_shadow_arena.cpp` | `createBuffer`、`DxvkAdapter::getMemoryHeapInfo`、`freeAllocation` |
| `war3_vram_preset.cpp` | `getMemoryHeapInfo` |
| `war3_shadow_alpha_test_payload.cpp` | `DxvkImageView`、`DxvkSampler` |
| `war3_async_screenshot.cpp` | `createBuffer/createFence`、`DxvkFence`、`freeAllocation` |
| `war3_frame_history.cpp` | `createImage`、`getMemoryHeapInfo` |
| `war3_perf_monitor.cpp` | `createRawQuery`、`DxvkGpuQuery`、`DxvkCommandList`、对象跟踪器、设备地址绑定跟踪器 |

## 5. 没覆盖的与要知道的

- **改动的调用路径没有夹具覆盖，没有在游戏里跑。** 在正式路径上的：实例化 draw（`war3_instance_buffer` 的 11 个 D3D9 调用改经虚表，
  每个实例化批次都走）、材质着色器创建（地图加载时）、JASS 雾体积 native、shaderpack 示例与品牌提示。
  依据是代码推理、语法检查与静态测试、以及“新 DLL 整体行为不变”的参照门。
  **出 1.22.x 补丁前应跑实机回归**（实例化单位、自定义材质、局部雾地图、首次进入游戏的品牌提示）。
- **实例化缓冲每次 D3D9 调用多一次虚调用。** 原来编译器把 `final` 类上的调用去虚化成直接调用。实例化路径每批十来次，
  没有测性能；与 draw 本身的开销比可以忽略（推断）。
- **“不可用”的 shaderpack 命令回答 `INTERNAL_ERROR`，不是值初始化。** 阶段 2 的约定是“不可用就是全 0”，
  但 `ShaderPackError` 的 0 是 `OK`，全 0 会把失败读成成功。代理模式下这两个命令将来由谁执行归阶段 5 接线。
- **ImGui 的处理与设计 §5 不同。** 设计写的是“后端提交绘制数据的那一步进 `IDeviceServices`”；查下来数据层根本不碰后端，
  只用 ImGui 核心往绘制列表里加图元，所以切口这边不需要新入口。覆盖层本身（`War3Imgui` 与 DX9 后端）现在由进程内设备驱动，
  代理模式下由谁驱动、画到代理设备还是交给宿主，是阶段 5 接线的问题，本阶段没有处理。
- **雾体积没有建镜像。** 设计 §5 写“查询读 x86 侧镜像状态”；状态整体搬到数据层后它就是唯一的一份。执行端反向读它，
  进程内是直接调用；代理模式下宿主拿不到这个对象，要由场景包带过去（归 4d，局部雾随体积光迁移）。
- **新的执行端文件 `d3d9_war3_material_shader.cpp` 按文件名规则归 `effects`。** 它实际是设备的一部分（两个成员的实现），
  归哪一层不影响切口：两层都是执行端。
- 切口门的 `imgui` 归 `GROWABLE` 意味着：以后执行端如果开始用 ImGui，门不会报；只有数据层用到的 ImGui 对象会被拉进数据集。
  现在执行端不用 ImGui。

## 6. 下一步

- **阶段 3 完成。** Opus 名下没有别的可以先做的段落：4d 依赖 4c 的阴影与投射体采集，5.2 依赖 5.1 的接线。
- **4c（阴影与蒙皮）归 Fable 5.2**，同时会处理剩下这类“DXVK GPU 对象”里的 GPU 蒙皮资源与阴影 arena；
  帧历史、异步截图、性能监视器 GPU 计时、显存档位跟随各自的特效或在阶段 5 接线时拆。
