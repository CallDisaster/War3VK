# x64 迁移阶段 4b 回执：AA（FXAA/SMAA）与 Bloom/色调映射

2026-11-15，Claude（Opus 5.5）。对应[整体路线](../plan/2026-11-13-x64-migration-roadmap.md)阶段 4b，照
[阶段 4a 设计](../plan/2026-11-15-x64-phase4a-effect-executors.md) §3 的八个步骤做。
全部改动未提交；没有接游戏；没有碰玩家目录（`E:/Work/Warcraft III/d3d9.dll` 仍不存在，门禁记为“不存在且必须保持不存在”）。
这一组改了 `src/` 里玩家正在用的 AA pass 的实现位置，并把 Bloom/色调映射从 D3D9 API 实现换成了 Vulkan 执行器，见 §6。

## 1. 结论

- **AA 与 Bloom/色调映射照模板迁完。** 两个执行器（`war3_aa_executor`、`war3_bloom_executor`）进程内与 x64 宿主共用；
  记录编号 7（AA）、8（Bloom）；宿主在 BeforeUi 处按 SSAO → 雪 → AA → Bloom 执行，与进程内的顺序相同。
- **两种模式像素逐字节相同。** 对照门 11 帧（4a 的 6 帧加 fxaa、smaa、bloom、bloom-tone、四个特效全开）在直连、流、
  流 + 命令环、流（无校验层）四个代理变体下全部 0 像素差异；D24S8 参照组与 D32F 0 差异。3 个带校验层的宿主会话 0 错误 0 警告。
  4a 的 6 帧哈希与 4a 回执逐个相同。
- **Bloom 的 Vulkan 移植与旧 D3D9 实现逐字节相同。** 夹具的旧 Bloom 参照臂（运行时 d3dcompiler 编 HLSL、D3D9 API 画）
  在三张 Bloom 帧上与新执行器 0 像素差异，其余 8 帧也 0 差异。
- **x86 内存**：代理模式开四个特效 x86 增量 0 字节（私有 2.4 MiB 不变），特效资源 329 KiB 在宿主里；
  进程内开四个特效私有内存多 23.2 MiB（另一组 1.9 MiB，同 4a 的现象，见 §4）。游戏里画 Bloom 不再加载 d3dcompiler_47。
- **原有路径没有变。** 新 DLL 作参照的 GPU 参照门：48 个像素哈希字段与 4a 逐个相同，38 个带校验层会话 0 错误 0 警告。
  正向切口 83/36 不变。
- **描边与局部雾没有做**，改归 4d（§2）。**没有在游戏里跑**（§6）。

## 2. 四个特效各归哪里

4b 原定四个特效。逐个按 4a 设计 §3 第 1 步读现有实现之后：

| 特效 | 现有实现 | 读的东西 | 结论 |
|---|---|---|---|
| FXAA/SMAA | Vulkan pass `War3AAPass`，在管线里注册在 `Weather` 之后 | 颜色目标、AA 设置 | **本组完成**：执行器 `war3_aa_executor`，记录编号 7 |
| Bloom/曝光/ACES | `War3PostProcess`：用 D3D9 API 画，运行时 `LoadLibrary("d3dcompiler_47.dll")` 编内嵌 HLSL，渲染到 D3D9 纹理；设备在 `War3MaybeInsertBeforeUi` 里、管线的 BeforeUi pass 之后调用 | 世界颜色目标、后处理设置 | **本组完成**：移植成 Vulkan 执行器 `war3_bloom_executor`（GLSL），记录编号 8。代理只允许渲染到后备缓冲，D3D9 API 的 pass 不能原样留在 x86 |
| 单位描边 | `renderUnitOutlineScreenSpace`（`d3d9_war3_shadow_outline.cpp`）重放阴影投射体的 draw 画遮罩 | 投射体列表（4c 的采集） | **改归 4d**：投射体采集与重放属于 4c，描边排在它之后 |
| 局部雾 | 地图作者的雾体积由体积光的 froxel 介质画（`War3VolumetricLightPass::RunMedium`），天气的雾与低空雪介质也在这里 | 体积光的介质与阴影 | **改归 4d**：随体积光一起迁。在那之前代理模式下天气只有雪粒，没有雾/低空雪介质 |

## 3. 改动

| 文件 | 内容 |
|---|---|
| `src/d3d9/war3/effects/war3_effect_scene.h` | 记录 `Aa`（7，28 字节）与 `Bloom`（8，24 字节）的布局、编解码、`ValidEffectRecord`、`FramePacket`（`hasAa`、`hasBloom`） |
| `src/d3d9/war3/effects/war3_aa_executor.{h,cpp}`（新） | AA 执行器：原 `War3AAPass` 的录制逻辑（颜色拷贝、FXAA 或 SMAA 三 pass、查找表上传），布局改成读/选/恢复，分配全部提前；查找表上传仍是事务式（失败本帧走 FXAA，下一帧重试） |
| `src/d3d9/war3/effects/war3_bloom_executor.{h,cpp}`（新） | Bloom 执行器：场景拷贝 → 预过滤 → 5 次降采样 → 5 次叠加升采样 → 合成（曝光、可选 ACES），各级与目标同为 8 位格式，与旧 D3D9 渲染目标一致 |
| `subprojects/war3fx/shaders/war3_bloom.frag`（新） | 旧实现内嵌 HLSL 的逐句移植，四个 pass 一个着色器 |
| `src/d3d9/d3d9_war3_aa.{h,cpp}` | 改成适配器：设置开关、`PostFX/AA` 性能区间留下，Vulkan 命令全部移走 |
| `src/d3d9/d3d9_war3_bloom.{h,cpp}`（新） | Bloom 的进程内适配器：设置 → 记录，`PostFX/Bloom` 性能区间 |
| `src/d3d9/d3d9_device.{h,cpp}` | `m_war3PostProcess` 换成 `m_war3BloomPass`；`War3MaybeInsertBeforeUi` 里同样的开关下改为 `EmitCs` 跑执行器；删掉 Reset 时释放与改分辨率时重建旧资源的两段；夹具入口按“SSAO → 天气 → AA → Bloom”跑，`runBloom` 参数留给旧 Bloom 参照 |
| `src/d3d9/d3d9_war3_scene_effects_fixture.{h,cpp}` | `DXVK_WAR3_SCENE_EFFECTS_FIXTURE_LEGACY_BLOOM=1` 时 Bloom 记录交给旧 D3D9 实现（参照臂） |
| `tools/render_host/d3d9_annotation_wire.h` | `AnnotationSchema` 加 7、8 |
| `tools/render_host/dxvk_core/before_ui_effects.h`、`d3d9_backend.{h,cpp}`、`d3d9_host_main.cpp` | 宿主执行顺序 SSAO → 雪 → AA → Bloom；`BeforeUiOutcome` 与 `scene-effects-host` 日志行加 AA、Bloom 的阶段与次数 |
| 构建清单 | `src/d3d9/meson.build`、`tools/render_host/dxvk_core/meson.build`、`subprojects/war3fx/meson.build`（`war3_bloom.frag` 进两个列表，FXAA/SMAA 四个着色器进 `war3fx_executor_shaders`） |
| 夹具与门 | `test_render_host_d3d9_scene_effects.cpp` 加 5 帧（fxaa、smaa、bloom、bloom-tone、all）、3 种拒绝（AA 模式 0 与 6、Bloom 开关取 2），计时帧四个特效全开；`run_x64_scene_effects_gate.py` 改期望计数、加旧 Bloom 参照臂 |
| 静态测试 | `test_scene_effects_static.py`（执行器集合、SMAA 查找表包含、四个特效的顺序、旧 D3D9 后处理只剩参照臂，14 条）；`test_render_host_annotation.h` 加 AA、Bloom 手写字节；`test_vulkan_release_review_static.py`、`test_war3_owned_image_layout_static.py` 改读执行器 |

`war3/render/war3_post_process.{h,cpp}` 没有删：夹具的参照臂和设备所有权探针（`test_device_owned_lifetime_probe.cpp`）还在用它。
设备不再引用它，游戏里不再加载 d3dcompiler 来画 Bloom。

### 3.1 Bloom 移植里定下的事

- **sRGB 转换无条件做。** 旧实现运行时编译的是 `war3_post_process.cpp` 里内嵌的 HLSL，它在预过滤和合成里**总是**先解码、
  最后再编码，从不读 `useSrgb`（仓库里另有一份按 `useSrgb` 分支的 `shaders/war3_postfx.hlsl`，运行时没有用到）。
  执行器照玩家实际看到的行为做：着色器保留开关位，执行器总是置位；记录里的 `useSrgb` 只携带、不读。
- **纹素中心。** 旧顶点着色器给 uv 加半个目标纹素，在 DXVK 的 D3D9 视口偏移（本仓库 0.5）下正好落在目标纹素中心，
  所以 GLSL 用 `gl_FragCoord.xy / 目标尺寸`。降采样和升采样的采样间隔是源级尺寸的倒数，与旧实现相同。
- **采样器。** 预过滤、降采样、升采样线性；合成时场景点采样、Bloom 线性，与旧实现的采样器状态相同。
- **Bloom 关也合成。** 旧实现 Bloom 关时强度取 0 仍跑整条链和合成（曝光、色调映射照做），执行器同样如此，代价不变。
- **失败不再改设置。** 旧实现失败时把 `postFx.enabled` 写成 false；执行器失败只是这一帧不录命令，下一帧照常尝试。

## 4. 门禁结果

产物：

| 产物 | 位置 | SHA-256 |
|---|---|---|
| x86 `d3d9.dll`（24,625,152 字节） | `build32_device_owner_phase4b_20261115_a/src/d3d9/` | `81FD9FF15632481C0101F5D4C8D339584E5A95BC3021F423F1B7B8D84934A4AD` |
| x64 宿主（6,291,456 字节） | `build64_render_host_phase4b_20261115_a/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe` | `FA76D030E79B5C3D4FDFA858F126BE1392F9F21BBF709CC5A19C7A7DE0BF4D01` |

| 门 | 回执（`AutoTest/artifacts/`） | 结果 |
|---|---|---|
| x86 构建 | `x86-phase4b-build-20261115-r2` | 通过。r1 是我在构建中途改了 Bloom 执行器（§3.1 的 sRGB）后停掉的，留作失败记录；r2 用同一构建目录 `--reuse` 重跑 |
| 切口门（`--ratchet`，基线阶段 2） | `x86-data-layer-cut-phase4b-20261115-r1` | 正向 83 / 36，增 0 减 0。反向 486 → 483：去掉旧路径失败时改写设置的 4 个符号（`GetMutableSettings`、`War3SettingsWrite`），多了 Bloom 适配器的性能区间移动赋值 1 个 |
| x64 宿主构建 | `x64-phase4b-host-build-20261115-r1` | 通过 |
| 特效对照门 | `x64-phase4b-scene-effects-20261115-r1` | 第一次运行通过；7 组（进程内 D32F、D24S8、旧 Bloom 参照，代理四组） |
| GPU 参照门（阶段 2 的全部开关） | `x64-phase4b-reference-gpu-vvl-20261115-r1` | 通过。48 个像素哈希字段与 4a 逐个相同；38 个带校验层会话 0 错误 0 警告；线格式 CPU 检查 1,455,967 项（x86、x64 各一遍，0 失败；多出的是 AA、Bloom 记录的手写字节检查） |
| 静态测试 | — | 222 个模块 1,397 项，失败 7 个模块，全部是 4a 之前就失败的 |

对照门的新帧（64×64，UI 的 144 个像素不计，共 3,952 个）：

| 帧 | 记录 | 哈希 | 相对“世界”改变的像素 |
|---|---|---|---|
| fxaa | AA 模式 1 | `cd34dd6d51d6dd00` | 244 |
| smaa | AA 模式 5（默认 SMAA Ultra 参数） | `fae6812848de6e0f` | 180 |
| bloom | 阈值 0.5、强度 0.4、曝光 1 | `9c120c4d5a05ebf8` | 3,952 |
| bloom-tone | 阈值 0.8、强度 0.6、曝光 1.2、ACES | `021de66e523df6a6` | 3,952 |
| all | SSAO、雪、SMAA、Bloom | `19a25236c266a5aa` | 3,952 |

- Bloom 帧改变了全部像素：合成对每个像素做 sRGB 解码、加 Bloom、再编码，夹具场景里每个非黑像素都有一点 Bloom
  （阈值 0.5、软拐点 0.5 时权重至少是亮度的平方）。默认阈值 1.18 在 8 位目标上只让近白像素发光，这个场景达不到，所以夹具用 0.5。
- 十种应拒绝的输入在两种模式下都被拒绝：4a 的七种（“未知编号”改用 9），加 AA 模式 0、AA 模式 6、Bloom 开关取 2。
- 宿主账目（四个代理组相同）：特效记录 112 条，`BeforeUi` 执行 33 次，SSAO、雪各录制 28 次，AA、Bloom 各 27 次，
  拒绝插入 3 次，最后一次 AA 阶段为 `SmaaRecorded`、Bloom 为 `CommandsRecorded`，执行器资源 336,896 字节。
- 每帧 BeforeUi 之后画的 12×12 UI 方块 144 个像素全部原样。

## 5. 分账

64×64 目标，每帧含一次回读，计时帧开四个特效。机器与别的会话共用，只能同一轮内比较。

| 组 | x86 私有内存 | 开四个特效后 x86 的增量 | 整帧中位数：只有世界 → 四个特效 | 宿主执行器 |
|---|---|---|---|---|
| 进程内（D32F） | 276.2 MiB | 私有 +23.2 MiB，VA +20 MiB | 178.2 → 400.9 µs | — |
| 进程内（D24S8） | 279.1 MiB | 私有 +1.9 MiB，VA +0 | 232.1 → 436.9 µs | — |
| 进程内，旧 Bloom 参照 | 277.5 MiB | 私有 +15.8 MiB（计时后 +34.6），VA +8 MiB（计时后 +24） | 284.5 → 990.1 µs | — |
| 代理，直连，校验层 | 2.4 MiB | 0 | 3,124.4 → 4,277.7 µs | 11,971.6 µs/次（含提交） |
| 代理，流，校验层 | 2.4 MiB | 0 | 1,481.0 → 3,086.1 µs | 1,374.7 µs/次 |
| 代理，流 + 命令环，校验层 | 2.4 MiB | 0 | 1,131.9 → 2,216.1 µs | 1,410.9 µs/次 |
| 代理，流，无校验层 | 2.4 MiB | 0 | 1,118.4 → 1,480.7 µs | 224.5 µs/次 |

- 代理模式下四个特效不占 x86 内存；x86 只多每帧四条记录（SSAO 24、雪 76、AA 28、Bloom 24 字节）和相机。
- 进程内两组的增量差 20 MiB 上下，和 4a 一样是 DXVK 内存块分配时机不同（推断，没有查证）。旧 Bloom 参照组计时后多出的部分
  包含 D3D9 纹理和 d3dcompiler_47 的映像。
- 宿主录四个特效约 225 µs/次（4a 两个特效 91 µs），不含 GPU 执行时间。整帧数字只能看作量级。

## 6. 没覆盖的与要知道的

- **进程内的 AA 换了实现位置，Bloom 换了实现，都没有在游戏里跑。** AA 的录制逻辑是逐句搬的，变化是布局改成读/选/恢复、
  分配提前、多采样目标拒绝。Bloom 从 D3D9 API 换成 Vulkan 命令：插入点（`War3MaybeInsertBeforeUi`、UI 未开始、世界目标）
  和开关条件没变，像素与旧实现在夹具上逐字节相同；但游戏里的目标格式、分辨率、改分辨率与 Reset 都没测。
  **从这个工作区出 1.22.x 补丁前应先跑实机回归**：AA 关/FXAA/SMAA 各档、Bloom 开关与 ACES、改分辨率、Alt+Tab（Reset）。
- **Bloom 在游戏里的计时口径变了**：CPU 区间 `PostFX` 现在只包住发命令，GPU 时间进新的 `PostFX/Bloom` 区间。
- **夹具入口仍绕过 `War3RenderPipeline::Execute`**，pass 的启用开关和与阴影接收、体积光的相对顺序不在对照里（同 4a）。
- **游戏里代理模式还没有人发记录**（阶段 5 接线），描边与局部雾/天气介质在代理模式下还没有（4d）。
- 进程内的命令流仍没有校验层覆盖（同 4a）。

