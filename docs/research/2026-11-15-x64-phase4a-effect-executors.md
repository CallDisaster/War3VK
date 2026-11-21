# x64 迁移阶段 4a 回执：特效执行器模板与第一组（天气雪、SSAO）

2026-11-15，Claude（Fable 5.2）。对应[整体路线](../plan/2026-11-13-x64-migration-roadmap.md)阶段 4a；模板本身和 4b 照做的步骤写在
[设计说明](../plan/2026-11-15-x64-phase4a-effect-executors.md)里，本文只记这一组的改动与结果。
全部改动未提交；没有接游戏；没有碰玩家目录（`E:/Work/Warcraft III/d3d9.dll` 仍不存在，门禁记为“不存在且必须保持不存在”）。
这一组改了 `src/` 里玩家正在用的两个 pass（天气雪、SSAO）的实现位置，见 §5。

## 1. 结论

- **模板定了，并且第一组照它跑通。** 一个特效一份执行器（`src/d3d9/war3/effects`），进程内的 pass 是薄适配器，
  x64 宿主编译同一份源码；输入经场景包（`FrameScene` 记录）过进程边界；宿主在 `StageMarker(BeforeUi)` 处执行，
  一个完成的世界场景一次。
- **两种模式像素逐字节相同。** 同一个夹具、同一组 draw/记录/标记，进程内 DLL 与“代理 + x64 宿主”的 6 帧
  （世界、SSAO、雪、两者、两者推进时间、关闭）在直连、流、流 + 命令环、流（无校验层）四个传输变体下全部 0 像素差异。
  3 个带校验层的宿主会话 VVL 0 错误 0 警告（含同步校验）。
- **x86 内存分账**：同一工作量下 x86 进程私有内存 284 MiB → 2.4 MiB、已用地址空间 691 MiB → 59 MiB（进程内 → 代理）。
  打开两个特效，进程内 x86 多 1.5–19.5 MiB 私有内存；代理模式 x86 多 0 字节，特效的资源（本夹具 48 KiB）在宿主里。
- **帧时间分账**：宿主录两个特效的命令约 91 µs/次（无校验层，两轮 92.0、90.8）。整帧的差值在这台共用机器上淹没在噪声里（§4）。
- **原有路径没有变。** 新 DLL 作参照、新宿主跑 GPU 参照门：48 个像素哈希字段与阶段 2 逐个相同，38 个带校验层的宿主会话
  0 错误 0 警告。正向切口 83 个符号、36 条边，没有回升。
- **没有在游戏里跑。** 进程内的天气雪和 SSAO 现在走新执行器，依据是夹具（含 D24S8 深度的进程内参照组）和代码对照，不是实机门。

## 2. 改动

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/effects/war3_effect_scene.h`（新，无依赖） | 场景包记录（相机 2、SSAO 5、雪 6）的布局与编解码、`FramePacket`、阶段枚举与 `SceneTracker`（从代理协议头挪来，加上插入规则） |
| `src/d3d9/war3/effects/war3_effect_frame.h`（新） | `EffectFrame`、`EffectCamera`、`EffectRecording`、相机矩阵推导、`InferFarClearRaw`（从混合光追文件挪来）、屏障与布局辅助 |
| `src/d3d9/war3/effects/war3_snow_executor.{h,cpp}`（新） | 雪的执行器：原 `War3WeatherPass::Run` 的录制逻辑，字段换成帧与记录，其余逐句相同 |
| `src/d3d9/war3/effects/war3_ssao_executor.{h,cpp}`（新） | SSAO 的执行器：按天气的写法重写（§5） |
| `src/d3d9/d3d9_war3_weather.{h,cpp}`、`d3d9_war3_ssao.{h,cpp}` | 改成适配器：设置开关、诊断、性能区间留下，Vulkan 命令全部移走（共减约 1,050 行） |
| `src/d3d9/d3d9_war3_effect_adapter.h`（新） | `War3PipelineInput` → `EffectFrame` |
| `src/d3d9/d3d9_war3_scene_effects_fixture.{h,cpp}`（新） | 夹具专用私有 COM 接口，环境变量开启才可查询到 |
| `src/d3d9/d3d9_device.{h,cpp}` | 记住天气 pass 指针；`QueryInterface` 的夹具分支；`War3RunSceneEffectsForFixture` |
| `src/d3d9/war3/render/war3_hybrid_ray_tracing.cpp` | `InferFarClearRaw` 改为调用共享版本 |
| `tools/render_host/d3d9_annotation_wire.h` | 引用场景包头；`AnnotationSchema` 加 5、6；`ValidFrameScene` 把特效记录交给 `ValidEffectRecord` |
| `tools/render_host/dxvk_core/before_ui_effects.h`（新） | 宿主的 BeforeUi 执行器集合与顺序 |
| `tools/render_host/dxvk_core/d3d9_backend.{h,cpp}` | `runBeforeUi`；目标代数；执行器随设备销毁 |
| `tools/render_host/dxvk_core/d3d9_host_main.cpp` | 场景包的接收与清空、插入规则、`scene-effects-host` 日志行 |
| 构建清单 | `src/d3d9/meson.build`、`tools/render_host/dxvk_core/meson.build`、`subprojects/war3fx/meson.build`（`war3fx_executor_shaders`）、根 `meson.build` |
| 门禁脚本 | `run_render_host_core_gate.py`（宿主允许清单放行 `war3/effects/*.cpp` 和被跟踪管线）、`run_x86_data_layer_cut_gate.py`（`effects` 目录归执行端） |
| 新测试 | `AutoTest/test_render_host_d3d9_scene_effects.cpp`（夹具）、`run_x64_scene_effects_gate.py`（对照门）、`test_scene_effects_static.py`（模板规则 13 条）；`test_render_host_annotation.h` 加手写字节的布局检查 |
| 改指向的测试 | `test_war3_weather_shader_contract.py`、`test_war3_weather_spirv_abi.py`、`test_war3_owned_image_layout_static.py`：断言不变，改读执行器源码 |

## 3. 门禁结果

产物：

| 产物 | 位置 | SHA-256 |
|---|---|---|
| x86 `d3d9.dll`（24,592,384 字节） | `build32_device_owner_phase4a_20261115_a/src/d3d9/` | `75552D271586678020AA44748BC86D3C98F4C99796735D3A1243819D2A3B29B7` |
| x64 宿主（6,017,024 字节） | `build64_render_host_phase4a_20261115_a/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe` | `68BE6A76FB8AD287319817E44864C955BE24700BA577D7BBBCF9AFC6F76E0BF2` |

| 门 | 回执（`AutoTest/artifacts/`） | 结果 |
|---|---|---|
| x86 构建 | `x86-phase4a-build-20261115-r1` | 通过 |
| 切口门（`--ratchet`，基线阶段 2） | `x86-data-layer-cut-phase4a-20261115-r1` | 正向 83 符号 / 36 边，增 0 减 0。反向 486 → 487：多的是 `War3PerfMonitor::ScopedSection` 的移动赋值（天气适配器把性能区间存进钩子），反向不设棘轮 |
| x64 宿主构建 | `x64-phase4a-host-build-20261115-r1` | 通过 |
| GPU 参照门（阶段 2 的全部开关） | `x64-phase4a-reference-gpu-vvl-20261115-r1` | 通过。48 个像素哈希字段与阶段 2 回执逐个相同；38 个带校验层会话 0 错误 0 警告；线格式 CPU 检查 1,455,821 → 1,455,922 项（x86、x64 各一遍，0 失败） |
| 特效对照门 | `x64-phase4a-scene-effects-20261115-r1`、`-r2` | 两轮都通过；r2 多一组 D24S8 参照 |
| 静态测试 | — | 206 个模块 1,270 项，失败 5 个模块，全部是本组之前就失败的（§6） |

对照门 r2 的六组（帧哈希六组完全相同）：

| 帧 | 哈希 | 相对“世界”改变的像素（共 4,096，UI 的 144 个不计） |
|---|---|---|
| world | `1e124feb631735e5` | 0 |
| ssao | `c0df6c736777fd96` | 2,263（全部变暗） |
| snow | `7a9bc1243d22b11d` | 97（全部变亮） |
| both | `86d70ba52ce937e3` | 2,296 |
| both-later | `14715795f843e1c6` | 2,298 |
| off | `1e124feb631735e5` | 0 |

- 每帧 BeforeUi 之后画的 12×12 UI 方块 144 个像素全部原样。
- 七种应拒绝的输入在两种模式下都被拒绝：SSAO 记录短 4 字节、含 NaN、雪记录 id 为 0、未知编号 7、没有世界场景时的 `BeforeUi`、
  场景内的 `BeforeUi`、同一场景的第二次 `BeforeUi`。
- 宿主账目（四个代理组相同）：特效记录 56 条，`BeforeUi` 执行 28 次，SSAO、雪各录制 27 次，拒绝插入 3 次，资源 49,152 字节。
- D24S8 参照组（`VK_FORMAT_D24_UNORM_S8_UINT`，本机 NVIDIA 驱动原生支持）：夹具断言全过，6 帧与 D32F 参照 0 像素差异。
- 我把 r1 的 12 帧（进程内与流模式各 6 帧）拼图看过：地面与三个盒子、SSAO 在盒子周围和地面上压暗、雪粒在世界和天空前面、
  推进时间后雪粒移动、UI 方块不变，两行图一致。

## 4. 分账

对照门 r2，64×64 目标，每帧含一次回读。机器与别的会话共用，帧时间噪声大，只能同一轮内比较。

| 组 | x86 私有内存 | x86 已用地址空间 | 开两个特效后 x86 的增量 | 整帧中位数：只有世界 → 加两个特效 | 宿主执行器 |
|---|---|---|---|---|---|
| 进程内（D32F） | 284.1 MiB | 690.6 MiB | 私有 +1.5 MiB，VA +0（r1：+19.5 MiB，+16 MiB） | 146.7 → 233.2 µs（r1：193.8 → 249.7） | — |
| 进程内（D24S8） | 284.5 MiB | 690.6 MiB | 私有 +1.5 MiB，VA +0 | 210.2 → 300.1 µs | — |
| 代理，直连，校验层 | 2.4 MiB | 59.2 MiB | 0 | 3,024.8 → 3,653.7 µs | 1,112.4 µs/次（含提交） |
| 代理，流，校验层 | 2.4 MiB | 59.1 MiB | 0 | 1,419.5 → 1,873.0 µs | 527.6 µs/次 |
| 代理，流 + 命令环，校验层 | 2.4 MiB | 63.2 MiB | 0 | 1,004.1 → 1,368.5 µs | 529.0 µs/次 |
| 代理，流，无校验层 | 2.4 MiB | 59.1 MiB | 0 | 1,342.2 → 1,257.5 µs（r1：950.2 → 1,109.1） | 90.8 µs/次（r1：92.0） |

怎么读：

- **内存是这一阶段要的东西。** 代理模式下特效不在 x86 里占任何内存；x86 一侧只多了每帧三条记录（相机 160、SSAO 24、雪 76 字节）。
  进程内两轮的增量不同（1.5 与 19.5 MiB），差值正好 16 MiB 上下，推断是 DXVK 的内存块在不同时刻分配，没有查证。
- **帧时间**：进程内两个特效约多 56–90 µs。代理模式无校验层那一组两轮的差值一正一负（+158.9、−84.7 µs），
  说明在这个规模上特效的增量小于整帧噪声；稳定的数字是宿主录制耗时 91 µs/次。它不含 GPU 执行时间。
- 这些是 64×64 合成场景的数字，不能外推到游戏分辨率；代理模式整帧比进程内慢（约 1 ms 对 0.2 ms）是阶段 1 就有的传输与回读成本，
  不是特效带来的。

## 5. 没覆盖的与要知道的

- **进程内的天气雪和 SSAO 换了实现位置，没有在游戏里跑。** 雪的录制逻辑是逐句搬的（只换了字段名）；SSAO 是重写的，命令流有这些变化：
  颜色与深度的布局转换合并成两批屏障；深度按图像的全部 aspect 一起转换（深度模板合一时不再只转深度）；
  渲染前显式转到附件布局、结束后恢复原布局；管线改由 `War3TrackedVkPipeline` 持有（以前在析构里直接销毁）；多采样目标改为拒绝。
  着色器和 push constant 布局没动。**如果要从这个工作区出 1.22.x 补丁，出之前应先跑实机回归**：
  开关 SSAO、JASS 天气雪、两者同开，换图与改分辨率各一次，并看 `Weather/Snow` 性能区间还在。
- **进程内的命令流没有校验层覆盖。** 固定的 VVL 是 64 位的，只挂在宿主会话上。同一份执行器在宿主里 0 错误，
  但进程内走的是另一组布局分支（附件布局、深度直接采样）。D24S8 参照组只证明它能跑、结果一致，不证明没有校验层问题。
- **夹具入口绕过了 `War3RenderPipeline::Execute`。** 它直接跑设备注册的两个 pass。pass 的启用开关、与阴影接收/体积光/AA 的
  相对顺序、游戏内 BeforeUi 的触发，都不在这个对照里。
- **游戏里代理模式还没有人发记录。** 记录由夹具发。x86 数据层每帧发相机与特效记录、在世界场景结束处发 `BeforeUi`，属于阶段 5 的接线。
- **雪的宿主路径与进程内不同**：宿主的深度没有采样用途，雪走深度拷贝；进程内直接采样深度。两条路径像素相同，
  但宿主因此多一张深度拷贝（本夹具 16 KiB，游戏分辨率下上限 32 MiB，超过则该帧不画雪）。
- **只有 BeforeUi 一个插入点**，以及设计说明 §4 列的其他未定事项（历史帧、特效间共享图像、x86/x64 浮点一致性靠对照门发现）。
- 对照门第一次运行就通过。为了确认它不是空比较，我核对了：进程内组的 x86 进程里有 Vulkan（私有内存 284 MiB）而代理组没有
  （2.4 MiB，`x86VulkanAbsent`）；宿主日志里两个执行器各录制 27 次；各帧哈希互不相同而跨模式相同；并看了拼图。
  没有做“故意改坏执行器看门是否报错”的变异测试。
- `tools/render_host/dxvk_core/meson.build` 新增的三个源文件使宿主多编译 `war3/render/war3_tracked_vk_pipeline.cpp`，
  这是允许清单里第一次出现 `war3/render` 的源文件，按文件逐个放行，不是按目录。

## 6. 本组之前就失败的静态测试

与阶段 2 回执相同的三个（`test_d3d9_shared_boundary`、`test_render_host_surface_owner`、`test_stage11_budget_census_static`），
以及这次跑到的另外四个。四个都对照 HEAD 核实过，与本组无关：

- `test_dxso_shader_module_cache_contract`：要求 `void shutdown(Stats& stats)` 恰好出现一次，HEAD 里是 0 次。
- `test_host_surface_integration`：HEAD 里 `createTexture` 就有两处。
- `test_render_host_initialization`：它在初始化片段里找的锚点 `  try {` 在 HEAD 里就出现两次。
- `test_surface_owner_continuation`：冻结回执说 `test_render_host_d3d9_init.cpp` 变了，而该文件与 HEAD 相同。

本组改坏过的只有 `test_war3_weather_shader_contract`（断言读的文字搬进了执行器），已改指向并通过。

## 7. 下一步

- **阶段 4b（Opus 5.5）**：SMAA/FXAA、Bloom/后处理、描边、局部雾，照设计说明 §3 的八步做，每个特效在对照门里加自己的帧。
- **阶段 3（Opus 5.5）** 可与 4b 并行。
- 4c（阴影与蒙皮）回到 Fable；它要先回答设计说明 §4 里“特效之间共享的图像归谁”。
