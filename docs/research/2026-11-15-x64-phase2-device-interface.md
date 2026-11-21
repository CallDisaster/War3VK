# x64 迁移阶段 2 回执：设备接口形状与第一组（诊断与测试入口）

2026-11-15，Claude（Fable 5.2）。对应[整体路线](../plan/2026-11-13-x64-migration-roadmap.md)阶段 2；
接口形状与后续各组的步骤写在[设计说明](../plan/2026-11-15-x64-phase2-device-interface.md)里，本文只记这一组的改动与结果。
这是 x64 工作第一次改 `src/` 里的主线 x86 源码（阶段 0、1 只动了 `tools/render_host` 与 `AutoTest`）。
全部改动未提交；没有接游戏；没有碰玩家目录（`E:/Work/Warcraft III/d3d9.dll` 仍不存在，门禁记为“不存在且必须保持不存在”）。

## 1. 结论

- **接口形状定了。** 数据层经两个纯虚接口找执行端：`war3::IDeviceServices`（当前设备上的入口，进程内由 `D3D9DeviceEx` 自己实现）
  和 `war3::IProcessServices`（不需要设备的入口，进程内由一个加载时注册的提供者逐项转发给原函数）。
  调用处只拿接口类型，编译器没有去虚化的依据。
- **第一组完成。** 正向切口 **106 → 83 个符号、48 → 36 条边**，新增 0，去掉的正是这一组的 23 个符号（§4）。
  `war3.cpp`、诊断中心、内部测试接口、阴影运行时桥、启动失败报告这五个文件到设备与特效的边全部消失。
- **新 DLL 作为原版渲染器没有变。** 用它作参照跑 GPU 参照门：主夹具、内存工作量、环形缓冲、窗口缩放四组的像素哈希
  与阶段 0、阶段 1 逐个相同，38 个带校验层的宿主会话 VVL 0 错误 0 警告。
- **改动的调用路径本身没有夹具覆盖，也没有在游戏里跑**（§5）。行为不变的依据是：调用的还是同一个函数，只是改经虚表。

## 2. 改动

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/core/war3_execution_services.h`（新） | 两个接口、`ProcessServices()`、`RegisterProcessServices()` |
| `src/d3d9/war3/core/war3_execution_services.cpp`（新，数据层） | 注册槽与“不可用”提供者（全部回答 0 或值初始化的结构体） |
| `src/d3d9/d3d9_war3_execution_services.cpp`（新，执行端） | 进程内提供者：15 个入口逐项转发；DLL 加载时注册 |
| `src/d3d9/d3d9_device.h` | `D3D9DeviceEx` 多继承 `war3::IDeviceServices`；新增内联的 `QueryWar3DeviceStatus()`。原有 8 个成员不改名、不改签名 |
| `src/d3d9/war3/war3.{h,cpp}` | 随设备发布/撤销同一事务里多存一份 `IDeviceServices*`；`RunWithActiveDeviceServices`；换图回退与验证入口改经接口 |
| `src/d3d9/war3/hooks/war3_hook_render.cpp` | 世界光照作用域、GPU 蒙皮桥挂接、语义阴影验证三处改经接口 |
| `src/d3d9/war3/tools/war3_diagnostics_hub.cpp` | 两处设备事务与 7 处阴影诊断查询改经接口；文件里不再出现 `D3D9DeviceEx` |
| `src/d3d9/war3/tools/war3_internal_test_api.cpp` | 两处设备事务、天气/体积光诊断、帧历史快捷键测试改经接口；不再包含 `d3d9_window.h` |
| `src/d3d9/war3/tools/war3_perf_monitor.cpp` | 导出时的天气诊断改经接口 |
| `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp` | 5 个设备侧计数改经接口；去掉文件里对 `war3_diag` 的前置声明 |
| `src/d3d9/war3/platform/war3_startup_error.cpp` | 启动记录改经接口 |
| `src/d3d9/d3d9_war3_weather.{h,cpp}` | `War3WeatherDrawStageName` 改为头文件内联（纯映射，不走接口） |
| `src/d3d9/meson.build` | 两个新源文件 |
| `AutoTest/test_execution_services_static.py`（新） | 9 项：两个接口全是纯虚、两个提供者覆盖齐全、注册次序、随设备发布、各调用点不再持有具体设备 |
| `AutoTest/test_shadow_cross_map_lifecycle_static.py` | 一条断言随调用文字更新：无设备回退由 `D3D9DeviceEx::War3ResetCpuSemanticMapSession()` 改为 `ProcessServices().War3ResetCpuSemanticMapSession()` |

源码差异：`src/` 里 12 个已有文件共 +114 / −74 行，另有 3 个新文件。设备类里没有新增逻辑，
`d3d9_device.cpp` 一行未改。

## 3. 门禁结果

全部按顺序运行，BelowNormal、`-j2`，同一时刻只有一个构建或门。都是第一轮通过，没有失败回执。

| 回执 | 结果 |
|---|---|
| `x86-phase2-build-20261115-r1` | `build32_device_owner_phase2_20261115`，配置 14 s、ninja 485 s，精确目标再跑为 no-work。DLL `AC86B00C…6AEE`，24,576,000 B（比阶段 0 的 `128191FE…F212` 多 12,288 B）。保护清单不变，玩家 DLL 构建前后都不存在。回执 `1F724F0B…DDB1` |
| `x86-data-layer-cut-20261115-r1` | `--baseline` 阶段 0 的 r2、`--ratchet` 通过：正向 106 → 83 个符号、48 → 36 条边，新增 0；反向 485 → 486 个符号、111 → 112 条边（多的一个是执行端调数据层的 `RegisterProcessServices`）。数据层对象 74 → 75，数据集 118 → 119。回执 `DF06F22F…05DB` |
| `x64-phase2-host-build-20261115-r1` | 宿主的构建回执覆盖 `src/`，源码变了必须重建。`build64_render_host_phase2_20261115_a`，宿主 `090B01BD…D655`，5,943,296 B；源码与保护清单前后一致。回执 `F7AAB83E…E36B` |
| `x64-phase2-reference-gpu-vvl-20261115-r1` | 新 x86 DLL 作参照，`--validation --repin-loader --stream --fixed-batch --annotations --command-ring --backing --ring --resize`，通过。回执 10,416,200 B，`01D3E564…AD8C`。内容见下 |

GPU 参照门里与之前逐个相同的像素哈希：

| 夹具 | 用例 | 哈希 | 对照 |
|---|---|---|---|
| 主夹具 | 20 个（18 张图） | `03f7265ab853783c` | 与阶段 1 回执（二）相同 |
| 内存工作量 | 6 个臂 | `544ef0dcb1262325` | 同上 |
| 环形缓冲 | 5 个臂 | `b491e0cf7780cb25` | 同上 |
| 窗口缩放 | 7 个 | `b3ae0d54a133b4b5`、`c31a6277983e7c78`；呈现帧 `f476c78366fbd725`、`be1ad3ff9b018725` | 与阶段 0 的 r4 逐行相同（含呈现哈希与帧数） |

另外：38 个带校验层的宿主会话（主夹具 16、内存工作量 5、环形缓冲 4、窗口缩放 5、raw 对端 8），VVL 1.4.365，0 错误、0 非状态警告；
raw 对端 8 个用例、命令环真实宿主故障 11 个会话通过；线格式测试每种位宽 1,455,821 项、命令环单元测试每种位宽 267,078 项。
这是第一次把 `--resize` 和阶段 1 的全部档位放在同一轮里跑。

静态测试：读到改动文件的 126 个测试模块共 1,019 项，123 个模块通过。3 个失败与本次改动无关，涉及的文件这次都没有改：

- `test_d3d9_shared_boundary`：断言宿主的 meson 不列 `d3d9_fixed_function.cpp`，而 10-29 起宿主就编译它（`tools/render_host/dxvk_core/meson.build` 与 HEAD 相同）。
- `test_render_host_surface_owner`：在 `tools/render_host/dxvk_core/d3d9_backend.cpp`（与 HEAD 相同）里找不到它要插桩的锚点。
- `test_stage11_budget_census_static`：要运行 `build32/src/d3d9/war3_stage11_budget_census_test.exe`，本机没有这个构建。

三个都没有在干净的 HEAD 检出上另跑一遍确认，判断依据是它们读的文件未改。

## 4. 去掉的符号与边

23 个符号（括号里是原定义处）：

- 设备成员 8 个（`d3d9_device.cpp`）：`War3SetWorldLightingScope`、`War3AttachGpuSkinNativeBridge`、`War3RequestShadowMapEpochReset`、
  `War3ResetCpuSemanticMapSession`（静态）、`War3ExecuteSemanticShadowSceneForValidation`、`QueryWar3ShadowLifecycleDiagnostics`、
  `War3ResetGpuSkinBridgeForTest`、`War3LogGpuSkinDiagnosticsForTest`。
- 设备侧计数 5 个（`d3d9_device.cpp` 的 `war3_diag`）：`QueryShadowAppendTotal`、`QueryShadowAppendUnique`、`QueryShadowAppendRawcodeAt`、
  `QueryDevicePaletteSlotCacheServedAfterConfirmCount`、`QueryDevicePaletteSlotCacheRejectedStaleCount`。
- 特效诊断 8 个：阴影 4 个（`QueryShadowTaaDiagnostics`、`QueryCsmResolutionDiagnostics`、`QueryPointShadowPersistentDiagnostics`、
  `QueryShadowReplayDiagnostics`），体积光 2 个（`QueryWar3VolumetricExecutionDiagnostics`、`QueryWar3WeatherVolumeDiagnostics`），
  天气 2 个（`QueryWar3WeatherDiagnostics`、`War3WeatherDrawStageName`）。
- 其他 2 个：`TestFrameHistoryWindowShortcut`（`d3d9_window.cpp`）、`startup_report::snapshot`（`dxvk_startup_report.cpp`）。

这正是路线图第 2 项说的“设备内部入口 13 个 + 特效诊断 11 个（按边计）”，加上阶段 0 新增的两个。

12 条边：

| 从 | 到 |
|---|---|
| `war3_hook_render.cpp` | `d3d9_device.cpp` |
| `war3_shadow_runtime_bridge.cpp` | `d3d9_device.cpp` |
| `war3_diagnostics_hub.cpp` | `d3d9_device.cpp`、`d3d9_war3_shadow.cpp` |
| `war3_internal_test_api.cpp` | `d3d9_device.cpp`、`d3d9_war3_volumetric_light.cpp`、`d3d9_war3_weather.cpp`、`d3d9_window.cpp` |
| `war3_perf_monitor.cpp` | `d3d9_war3_volumetric_light.cpp`、`d3d9_war3_weather.cpp` |
| `war3.cpp` | `d3d9_device.cpp` |
| `war3_startup_error.cpp` | `dxvk_startup_report.cpp` |

另外两处现在不产生符号、但也一并改了：`war3_hook_render.cpp` 里 `if constexpr` 丢弃分支中的
`War3PopulateSemanticShadowSceneForValidation`（开关一翻就会进切口），以及诊断中心直接读 DXVK 设备状态的两处
（原来是内联访问器，改成接口上的 `QueryWar3DeviceStatus`，这样该文件不再需要具体设备类型）。

剩下的 83 个符号、36 条边按处理方式的归类见设计说明 §5。

## 5. 没覆盖的与要知道的

- **改动的调用路径没有夹具覆盖，没有在游戏里跑。** 其中两条在正式路径上：世界光照作用域每帧切两次（1.22.03 的光照修复），
  换图时的重置请求。它们现在经 `RunWithActiveDeviceServices` 和虚表到达同一个设备成员，持的还是同一把发布锁。
  依据是代码推理、语法与静态测试、以及“新 DLL 整体行为不变”的参照门，不是实机门。
  **如果要从这个工作区出 1.22.x 补丁，出之前应先跑一遍实机回归**（世界/头像光照、换图、阴影生命周期诊断）。
- **设备对象的布局变了**：多一个虚表指针，其后的成员整体后移 4 字节。没有行为含义；`src/` 里没有对 `D3D9DeviceEx` 用
  `offsetof`、`sizeof`、`alignof` 的代码。
- **进程级提供者靠静态初始化注册。** 注册槽与两个提供者是常量初始化的，不受初始化次序影响；
  但它确实是“加载 DLL 就执行的代码”，只做一次原子指针写入。
- **“不可用”就是全 0**，没有单独的可用性标志。进程内不会出现；代理模式下实机门拿到全 0 会判失败而不是误判通过。
- **代理模式的两个提供者都还没写。** 每个入口将来由谁回答、随哪个阶段落地，列在设计说明 §4。
- `war3_hook_render.cpp` 里还剩一处经具体设备的事务（渲染队列接管，传设备指针给实例化缓冲），属于阶段 3 的实例化缓冲组。
- 性能门没有跑：这一组不在 draw 路径上，每帧多的是两次虚调用。

## 6. 下一步

- **阶段 3（S2 其余分组）归 Opus 5.5。** 各组的归类和照做的步骤在设计说明 §5，新的切口基线是
  `x86-data-layer-cut-20261115-r1`（正向 83 / 36）。
- **阶段 4a（天气 + SSAO 模板）归 Fable 5.2**，不依赖阶段 3，可以先做。
