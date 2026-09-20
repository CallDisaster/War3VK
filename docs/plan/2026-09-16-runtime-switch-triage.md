# 运行时开关三分类清册（2026-09-16）

> 工作树 B：`E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914`
> 性质：只读分析产出。除本文件外未修改 B 树任何文件；未执行任何 git 写操作；未构建、未部署、未启动游戏。
> 所有结论均由 `src/` 源码与仓库内文档的实际文本检索得出；证据不足的条目一律进入「待判定」并写明所缺证据。
> 第 10 章（扩展维度登记）为 2026-09-16 Q4 裁定后的后补扩展章节：只补充所有者／读取时机／重启需求／面板-JAPI 优先级／发布可达／支持范围与诊断风险，不改动第 1–9 章的既有结论与 412 行定义域。

## 0. 口径裁定（2026-09-17，方向 b）

> **本节为 2026-09-17 按 Q4 裁定方向 (b) 追加的口径回写（Q4(b)）；不改变第 1–10 章的实测数字、412 行定义域与 A/B/C 判定标准。**
>
> 1. **412 个运行时 env 开关全部属于内部面**（诊断／工程／实验用途），**不承诺**向玩家提供文档。
> 2. **玩家可见入口只认两处**：Ctrl+F1 运行时面板 与 JAPI／`warvk:v1`；env 名不构成玩家配置面。
> 3. **不得删除任何开关、不得改默认值**（Q4 明令）；既有 env 启动配置语义不变。
> 4. **85 项待判定保持待判定**，不得强行归入 A/B/C。
> 5. A/B/C 三分类的**判定标准不因本裁定而改变**；但 A 类「缺玩家向文档」**不再是缺陷，而是预期状态（内部面）**——§1.1-2、第 3 章前言与 §9.3 的相关表述一律按此口径阅读。

## 1. 概述与统计

| 分类 | 含义 | 数量 |
| --- | --- | ---: |
| A 类 | 玩家/生产（默认生效，或未设置时保留内置生产默认的可调覆盖项） | 156 |
| B 类 | 诊断/取证（默认关，名称或注释表明诊断/对照用途） | 152 |
| C 类 | dev-only／实验（被编译期门包裹，或 Release 二进制内不可达） | 19 |
| 待判定 | 证据不足以归入 A/B/C | 85 |
| — | **运行时字符串字面量开关小计** | **412** |
| 宏/注释专用 | 仅出现在宏/注释中的开关名（§7） | 9 |
| — | **合计** | **421** |

### 1.1 四条需要首先知道的事实

1. **枚举复核通过**：`src/` 下以双引号 C/C++ 字符串字面量形式出现的唯一 `DXVK_WAR3_*` 名为 **412**，与上一会话数字一致。定义域为 `.cpp/.h/.hpp/.inl/.c/.cc/.hh`（§9.2 给出全部计数变体与差异说明）。
2. **严格玩家向文档面命中为 0**：在 `WarVK/`、根 `README.md`/`README_CN.md`、`CHANGELOG.md`、`docs/RELEASE_*` 中，**没有任何一个运行时开关名出现**。`CHANGELOG.md` 中唯一一处 `DXVK_WAR3_` 是通配写法 `` `DXVK_WAR3_*` ``，其语义恰为"即使启动器遗留旧环境变量也不能重新打开这些路径"。因此 A 类 156 项**全部缺玩家向文档**——按 Q4(b) 属**预期状态（内部面）**，不再是缺陷；玩家可见入口只有 Ctrl+F1 运行时面板与 JAPI／`warvk:v1`，而不是环境变量。（Q4(b)）
3. **按任务字面 corpus 可得 59（上一会话为 63）**：若把"`README*`"按递归基名匹配，`docs/research/**` 与 `docs/plan/**` 下的工程 README 会被计入，命中 59 个开关。这些是**内部工程文档**而非玩家向面。上一会话的 63 无法在本树复现，差异分析见 §9.3。
4. **"默认关"不等于"无用"**：待判定中多数条目是生产正确性回退开关或未晋升的实验路径；本报告不对其下结论，只标注所缺证据。其中 `DXVK_WAR3_SHADOW_WORLD_UP` 有**充分证据表明在当前源码中不可达**（见 §6 该行）。

## 2. 分类规则

| # | 条件 | 归入 |
| ---: | --- | --- |
| 1 | 读取点位于引用 `WARVK_*_DEV`／`WARVK_DATA_COLLECTION_TREE_DEV`／`WARVK_INTERNAL_FRAME_RECORDER_DEFAULT` 的预处理门内 | C |
| 2 | 读取点在同一函数内先经开发策略常量 `if constexpr` 提前返回（`kReleaseFreezeExperimentalShadowRoutes`、`kDevelopmentShadowObserversEnabled`、`kCoherent*DevelopmentEnabled`、`kCurrentUpShadowReplayDevelopmentEnabled`、`kDevelopmentRtsShadowCandidateEnabled`、`kNativeInternalTestApiEnabled` 等），Release 二进制内不可达 | C |
| 3 | 名称前缀为 `DXVK_WAR3_TEST_`／`DXVK_WAR3_AUTOTEST_`，且默认非开 | B |
| 4 | 默认关或仅显式覆盖生效，且名称含诊断/取证标记（DEBUG／TRACE／DIAG／EVIDENCE／PROBE／VERIFY／ASSERT／STATS／METRIC／OBSERVE／SAMPLE／PERIOD／LOG／DUMP／SURVEY／REPORT／CENSUS／HEALTH／TIMING／BREAKDOWN／SNAPSHOT／VISITS／AUDIT／SPIKE／HISTOGRAM／TIMELINE／FINGERPRINT／INVENTORY／DIFF／PROFILE，或 `PERF_`／`FRAME_EVIDENCE*` 等前缀） | B |
| 5 | 默认关，且读取点所在函数的注释明确表明诊断/对照/取证用途（诊断／取证／A-B／ABBA／对照／排障／定位／观测／观察／统计／计数／采样／trace／probe／verify／observe／diagnostic／census／survey／debug） | B |
| 6 | 读取点位于恒不成立的运行时守卫内（如 `if (s_x == -2)` 而全文件无任何把 `s_x` 赋为 -2 的路径） | 待判定（注明不可达） |
| 7 | 默认开启：数值 ≠ 0、`true`、`readDefaultOnFlag`、`getEnvVar(X) != "0"`、或读取点注释声明"默认开" | A |
| 8 | 仅显式覆盖生效、未设置时保留内置生产默认（`ParseEnvInt`／`ParseEnvFloat` 覆写型）且非诊断命名 | A |
| 9 | 其余：默认值不可提取，或默认关且既无编译期门也无诊断命名/注释证据 | 待判定 |

**默认值的两条硬规则**：

- **"未设置"不等于"默认关"**。覆写型 helper（`ParseEnvInt`／`ParseEnvFloat`）未设置时保留内部生产默认，因此归 A 而不是 B；`getEnvVar(X) != "0"` 这类"除 0 以外都算开"的写法（含未设置）同样判为默认开。
- **代码字面量优先于注释**。只有当提取到的是符号常量或存在性判断时，才允许用读取点注释的"默认开"声明校正默认值；若已提取到 `0u`／`false` 等明确字面量，注释不得覆盖。本规则修掉了两处注释误判（§9.6）。

默认值提取覆盖的 helper 形态：`War3GetEnvU32`、`ReadEnvU32`、`EnvU32Default`、`EnvFloatDefault`、`EnvFlagDefault`、`EnvFlagOrDefault`、`GetEnvBoolCached`、`GetEnvBool`、`ParseEnvFlagOrDefault`、`EnvIntOverride`、`readDefaultOnFlag`、`readExactFlag`、`readEnabled`、`exactFlag`、`EnvFlagEnabled`、`IsEnvironmentFlagEnabled`、`ParseArenaMegabytesEnv`、`readPeriod`、`readU32`、`ParseEnvInt`、`ParseEnvFloat`，以及直接 `getEnvVar`／`std::getenv`／`GetEnvironmentVariableA/W` 的比较式。

## 3. A 类清单（156 项）

全部 156 项**均缺玩家向文档**——严格玩家向文档面命中 0（§1.1-2）；按 Q4(b) 属**预期状态（内部面）**，不再作为缺陷/待补项（Q4(b)）。第四列给出唯一的文档侧证据：`docs/research|plan` 下的内部工程 README（若有）。

| # | 开关名 | 默认 | 玩家文档出处 | 生产语义证据（读取点） |
| ---: | --- | --- | --- | --- |
| 1 | `DXVK_WAR3_AA` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:447` |
| 2 | `DXVK_WAR3_BEFOREUI_REQUIRE_STRONG_UI_MARKER` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:19700` |
| 3 | `DXVK_WAR3_BEFOREUI_REQUIRE_UIDISPATCH` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:19656` |
| 4 | `DXVK_WAR3_BEFOREUI_TIER1` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:19642` |
| 5 | `DXVK_WAR3_BEFOREUI_TIER6` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:19754` |
| 6 | `DXVK_WAR3_COHERENT_REAL_DOMAIN_CACHE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3255` |
| 7 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:755` |
| 8 | `DXVK_WAR3_CURRENT_DRAW_ACTIVE_SLOT_SNAPSHOT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:634` |
| 9 | `DXVK_WAR3_CURRENT_DRAW_BATCHED_BOUNDED_SNAPSHOT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:643` |
| 10 | `DXVK_WAR3_CURRENT_DRAW_GROUP_RANGE_CACHE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1975` |
| 11 | `DXVK_WAR3_CURRENT_DRAW_INDEX_SLICE_CACHE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1982` |
| 12 | `DXVK_WAR3_CURRENT_DRAW_PUBLISH_AFTER_ORIGINAL` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:861` |
| 13 | `DXVK_WAR3_CURRENT_DRAW_REJECT_SMALL_VIEWPORT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:884` |
| 14 | `DXVK_WAR3_CURRENT_DRAW_SMALL_VIEWPORT_MIN` | 300u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:890` |
| 15 | `DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_TRACE_PERIOD` | 16u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:710` |
| 16 | `DXVK_WAR3_DISABLE_SHADERPACK` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:1486` |
| 17 | `DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3348` |
| 18 | `DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3385` |
| 19 | `DXVK_WAR3_ENABLE_HOOKS` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_hook.cpp:576` |
| 20 | `DXVK_WAR3_EXACT_INDEX_DOMAIN_BULK_READ` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:48197` |
| 21 | `DXVK_WAR3_FXAA_SUBPIX` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:454` |
| 22 | `DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE` | dxvk::war3::internal::kShadowSemanticCoreSceneKeepS1TerrainLegacyCapture ? 1u : 0u | 无（缺玩家向文档） | 默认开启（生产默认路径）；默认值由读取点注释声明为默认开；`src/d3d9/d3d9_device.cpp:2161` |
| 23 | `DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_shadow_producer_policy.cpp:56` |
| 24 | `DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_shadow_producer_policy.cpp:63` |
| 25 | `DXVK_WAR3_MODEL_HOOK` | true（编译期常量 kShadowRuntimeModelHookEnabled） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:9467` |
| 26 | `DXVK_WAR3_MODEL_POSE_HOOK` | true（编译期常量 kShadowRuntimePoseHookEnabled） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:9477` |
| 27 | `DXVK_WAR3_NATIVE_HINT_PRODUCERLESS_SKIP` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3338` |
| 28 | `DXVK_WAR3_PALETTE_ARBITRATION_STRICT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:2396` |
| 29 | `DXVK_WAR3_PERF_SHADOW_PHASE_SAMPLE_PERIOD` | 16u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:726` |
| 30 | `DXVK_WAR3_POINT_LIGHTS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:464` |
| 31 | `DXVK_WAR3_POINT_RAY_SHADOW` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:549` |
| 32 | `DXVK_WAR3_POINT_RAY_SHADOW_HIZ` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:559` |
| 33 | `DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:593` |
| 34 | `DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:569` |
| 35 | `DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:607` |
| 36 | `DXVK_WAR3_POINT_RAY_SHADOW_STEPS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:577` |
| 37 | `DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:614` |
| 38 | `DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md`、`docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:600` |
| 39 | `DXVK_WAR3_POINT_SHADOW` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:471` |
| 40 | `DXVK_WAR3_POINT_SHADOW_BIAS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:512` |
| 41 | `DXVK_WAR3_POINT_SHADOW_MAX_FACES` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:621` |
| 42 | `DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:478` |
| 43 | `DXVK_WAR3_POINT_SHADOW_PCF_FAR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:526` |
| 44 | `DXVK_WAR3_POINT_SHADOW_PCF_NEAR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:519` |
| 45 | `DXVK_WAR3_POINT_SHADOW_RANGE_FADE` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:542` |
| 46 | `DXVK_WAR3_POINT_SHADOW_RESOLUTION` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:489` |
| 47 | `DXVK_WAR3_POINT_SHADOW_TEXEL_BIAS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:535` |
| 48 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VIEW` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2473` |
| 49 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY` | ON（readDefaultOnFlag） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/hooks/war3_hook_render.cpp:1495` |
| 50 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_SAMPLE_PERIOD` | 256u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/hooks/war3_hook_render.cpp:1502` |
| 51 | `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:2415` |
| 52 | `DXVK_WAR3_RUNTIME_MATRIX_BATCH_CAPTURE` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:7917` |
| 53 | `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:8330` |
| 54 | `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH_DEDUP` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/model/war3_model_hook.cpp:7037` |
| 55 | `DXVK_WAR3_S1_PERSISTENT_BORROW_STATIC` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2306` |
| 56 | `DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2277` |
| 57 | `DXVK_WAR3_S1_TERRAIN_PERSISTENT_GEOMETRY` | dxvk::war3::internal::kShadowS1TerrainPersistentGeometryEnabled ? 1u : 0u | 无（缺玩家向文档） | 默认开启（生产默认路径）；默认值由读取点注释声明为默认开；`src/d3d9/d3d9_device.cpp:2294` |
| 58 | `DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_TRACE_PERIOD` | 64u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1324` |
| 59 | `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` | 240u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1233` |
| 60 | `DXVK_WAR3_SEMANTIC_COVERAGE_DROP_MAX_HOLD_FRAMES` | 30u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1091` |
| 61 | `DXVK_WAR3_SEMANTIC_CURRENT_DRAW_GRACE_FRAMES` | 8u | 无（缺玩家向文档）；内部工程文档：`docs/plan/shadow_pose_stutter_investigation_2026_05_11/README.md` | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2994` |
| 62 | `DXVK_WAR3_SEMANTIC_DIRECT_ONLY` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2151` |
| 63 | `DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE` | 1u | 无（缺玩家向文档）；内部工程文档：`docs/plan/automation_exchange/README.md` | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2540` |
| 64 | `DXVK_WAR3_SEMANTIC_DIRECT_RECORD_CAP` | 256u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2117` |
| 65 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_DIRECT_PRODUCER` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2546` |
| 66 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_REGION_CACHE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1967` |
| 67 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1286` |
| 68 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_TRACE_PERIOD` | 32u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1275` |
| 69 | `DXVK_WAR3_SEMANTIC_GATE_VALUE_CACHE` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:32` |
| 70 | `DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2567` |
| 71 | `DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_ON_COVERAGE_DROP` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1079` |
| 72 | `DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_UNTIL_STABLE_IDENTITY` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1067` |
| 73 | `DXVK_WAR3_SEMANTIC_IDENTITY_STABLE_FRAMES_BEFORE_REDRAW` | 2u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1073` |
| 74 | `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:871` |
| 75 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2085` |
| 76 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_SAFE_COPY` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2078` |
| 77 | `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_EPOCH_PLANNER` | 1u | 无（缺玩家向文档）；内部工程文档：`docs/plan/automation_exchange/README.md` | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2658` |
| 78 | `DXVK_WAR3_SEMANTIC_MANIFEST_DEFER_PROVISIONAL_PARTS` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2622` |
| 79 | `DXVK_WAR3_SEMANTIC_MANIFEST_GEOMETRY_CACHE_FRAMES` | 3u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2635` |
| 80 | `DXVK_WAR3_SEMANTIC_MANIFEST_LEASE_PALETTE_REFRESH` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2674` |
| 81 | `DXVK_WAR3_SEMANTIC_MATERIAL_CACHE` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:5241` |
| 82 | `DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2368` |
| 83 | `DXVK_WAR3_SEMANTIC_PALETTE_ATTRIBUTION_SNAPSHOT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:622` |
| 84 | `DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1304` |
| 85 | `DXVK_WAR3_SEMANTIC_POSE_ONLY_CAPTURE_PERIOD` | 4u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1241` |
| 86 | `DXVK_WAR3_SEMANTIC_RECEIVER_FREEZE_LAST_GOOD_LIGHTING` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1019` |
| 87 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_STRENGTH_CLAMP` | 0.55f | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_shadow.cpp:1055` |
| 88 | `DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3155` |
| 89 | `DXVK_WAR3_SEMANTIC_REQUIRE_AUTHORITATIVE_SKINNED` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2948` |
| 90 | `DXVK_WAR3_SEMANTIC_REQUIRE_DIRECT_UNIT_VISIBLE_BACKING` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1202` |
| 91 | `DXVK_WAR3_SEMANTIC_REQUIRE_VISIBLE_INDEX_SLICE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1184` |
| 92 | `DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:65` |
| 93 | `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:77` |
| 94 | `DXVK_WAR3_SEMANTIC_SHADOW_FORCE_BEFOREUI` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_war3_pipeline.cpp:1005` |
| 95 | `DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:113` |
| 96 | `DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:40` |
| 97 | `DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION` | true | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:53` |
| 98 | `DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2534` |
| 99 | `DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION_MIN_RECORDS` | 16u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2682` |
| 100 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2521` |
| 101 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL_MARGIN` | 8u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2528` |
| 102 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2500` |
| 103 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES` | 120u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2506` |
| 104 | `DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1196` |
| 105 | `DXVK_WAR3_SHADOW_ALPHA_HASH` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:416` |
| 106 | `DXVK_WAR3_SHADOW_ALPHA_MIP` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:424` |
| 107 | `DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:432` |
| 108 | `DXVK_WAR3_SHADOW_ARENA_CAPTURE` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/war3/memory/war3_shadow_arena.cpp:169` |
| 109 | `DXVK_WAR3_SHADOW_BIAS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:287` |
| 110 | `DXVK_WAR3_SHADOW_CAPTURE_TRACE_PERIOD` | 16u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3486` |
| 111 | `DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:315` |
| 112 | `DXVK_WAR3_SHADOW_CASCADES` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:352` |
| 113 | `DXVK_WAR3_SHADOW_FREEZE_DYNAMIC` | ON（≠ "0"） | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:48186` |
| 114 | `DXVK_WAR3_SHADOW_GATE_TRACE_PERIOD` | 16u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3480` |
| 115 | `DXVK_WAR3_SHADOW_METADATA_ALPHA` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3400` |
| 116 | `DXVK_WAR3_SHADOW_METADATA_BLOCKER` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3406` |
| 117 | `DXVK_WAR3_SHADOW_METADATA_CAPTURE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3394` |
| 118 | `DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:304` |
| 119 | `DXVK_WAR3_SHADOW_PCF` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:345` |
| 120 | `DXVK_WAR3_SHADOW_PCF_KERNEL` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:369` |
| 121 | `DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE` | 240u | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/32_2026_08_26_render_layer_coverage/README.md` | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1177` |
| 122 | `DXVK_WAR3_SHADOW_PERSISTENT_MB` | 512u | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/32_2026_08_26_render_layer_coverage/README.md` | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:1170` |
| 123 | `DXVK_WAR3_SHADOW_RECEIVER_MODE` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:294` |
| 124 | `DXVK_WAR3_SHADOW_RES` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:360` |
| 125 | `DXVK_WAR3_SHADOW_SPLIT_LAMBDA` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:335` |
| 126 | `DXVK_WAR3_SHADOW_STABLE_SNAP` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:326` |
| 127 | `DXVK_WAR3_SHADOW_STRENGTH` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:280` |
| 128 | `DXVK_WAR3_SHADOW_TAA` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:390` |
| 129 | `DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:404` |
| 130 | `DXVK_WAR3_STAGE13_LATE_FULL_INDEX_FINGERPRINT` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2234` |
| 131 | `DXVK_WAR3_STAGE13_LATE_SAMPLE_COUNT` | 32u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2223` |
| 132 | `DXVK_WAR3_STAGE13_STATIC_RETENTION_CAP` | 64u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2263` |
| 133 | `DXVK_WAR3_STAGE13_STATIC_RETENTION_FRAMES` | 240u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:2252` |
| 134 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_PRIMS` | 30000 | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:10810` |
| 135 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_START` | 60000 | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:10812` |
| 136 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_VERTS` | 65536 | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:10814` |
| 137 | `DXVK_WAR3_VOLUMETRIC_BACKEND` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:743` |
| 138 | `DXVK_WAR3_VOLUMETRIC_DECAY` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:697` |
| 139 | `DXVK_WAR3_VOLUMETRIC_DENSITY` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:706` |
| 140 | `DXVK_WAR3_VOLUMETRIC_EXTINCTION` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:811` |
| 141 | `DXVK_WAR3_VOLUMETRIC_FADE_FAR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:789` |
| 142 | `DXVK_WAR3_VOLUMETRIC_FADE_NEAR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:783` |
| 143 | `DXVK_WAR3_VOLUMETRIC_FROXEL_FAR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:751` |
| 144 | `DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:804` |
| 145 | `DXVK_WAR3_VOLUMETRIC_INTENSITY` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/26_volumetric_light_probe/README.md`、`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:688` |
| 146 | `DXVK_WAR3_VOLUMETRIC_LIGHT` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/26_volumetric_light_probe/README.md`、`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:681` |
| 147 | `DXVK_WAR3_VOLUMETRIC_MAX_RAY` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:797` |
| 148 | `DXVK_WAR3_VOLUMETRIC_POINT_MAX_LIGHTS` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:762` |
| 149 | `DXVK_WAR3_VOLUMETRIC_RES_DIVISOR` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:773` |
| 150 | `DXVK_WAR3_VOLUMETRIC_SAMPLES` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/26_volumetric_light_probe/README.md`、`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:735` |
| 151 | `DXVK_WAR3_VOLUMETRIC_SKY_THRESHOLD` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档） | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:724` |
| 152 | `DXVK_WAR3_VOLUMETRIC_UNSHADOWED` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:818` |
| 153 | `DXVK_WAR3_VOLUMETRIC_WEIGHT` | OVERRIDE（未设置保留内部默认） | 无（缺玩家向文档）；内部工程文档：`docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 生产可调覆盖项（未设置时保留内置生产默认）；`src/d3d9/d3d9_war3_pipeline.cpp:715` |
| 154 | `DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES` | 8u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3465` |
| 155 | `DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3454` |
| 156 | `DXVK_WAR3_WIDGET_PROBE_SAFE_COPY` | 1u | 无（缺玩家向文档） | 默认开启（生产默认路径）；`src/d3d9/d3d9_device.cpp:3444` |

## 4. B 类清单（152 项）

| # | 开关名 | 默认 | 用途证据（归类依据／注释与读取点） |
| ---: | --- | --- | --- |
| 1 | `DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE` | OFF（仅显式启用生效） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；1.27a IDA 证据：Storm 的窗口过程在 WM_ACTIVATEAPP 失焦时将 active=0， Storm_EventLoop_PeekMessage 随后唯一调用 Game.dll+0x1552E0 取得 idle Sleep 毫秒数。这里只关闭该精确后台分支，不 Hook 全局 Sleep，也不改变 SetThreadPriority/SetPriorityClass 或地图的 PauseGame 语义。；`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp:83` |
| 2 | `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE` | OFF（仅显式启用生效） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/war3/hooks/war3_hook_lifecycle.cpp:71` |
| 3 | `DXVK_WAR3_BOOTSTRAP_NO_GAME_HOOKS` | false | 默认关；读取点注释表明诊断/对照用途；诊断态可完全跳过 Game.dll bootstrap hooks；此时不会建立控制管道， 只用 Present/窗口证据判断游戏自身是否继续启动。默认路径不变。；`src/d3d9/d3d9_war3_hook.cpp:813` |
| 4 | `DXVK_WAR3_CRASH_FIRST_CHANCE_TRACE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/core/war3_crash_handler.cpp:403` |
| 5 | `DXVK_WAR3_CSM_CONTINUITY_TRACE` | OFF（== "1"） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_war3_shadow.cpp:1778` |
| 6 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY` | false | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_war3_shadow.cpp:761` |
| 7 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY_ASSERT` | false | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_war3_shadow.cpp:767` |
| 8 | `DXVK_WAR3_CURRENT_DRAW_GENERATION_INDEX_SLICE_CACHE` | 0u | 默认关；读取点注释表明诊断/对照用途；Same-DLL A/B switch. Reuse remains map/generation backed; disabling this restores the old per-Populate cache without disabling all slice caching. The physical A-B-B-A reduced slice allocation work but saved only about 0.02 ms/frame, below the 0.15 ms product admission threshold.；`src/d3d9/d3d9_device.cpp:1992` |
| 9 | `DXVK_WAR3_CURRENT_DRAW_LAST_SAMPLE` | 0u | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_current_draw_contract.cpp:961` |
| 10 | `DXVK_WAR3_CURRENT_DRAW_REDUNDANT_ATOMICS` | 0u | 默认关；读取点注释表明诊断/对照用途；The old publish path performed one unread provenance bucket RMW for every ready snapshot and a second lifetime counter RMW for every trusted hit. On the 32-bit build each uint64_t fetch_add lowers to lock cmpxchg8b. Keep the exact legacy writes for same-DLL A/B; the default path derives the compatib；`src/d3d9/war3/render/war3_current_draw_contract.cpp:671` |
| 11 | `DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；Coarse frame-level tracing only. Keep this separate from the per-draw publish probes so production snapshots pay no timer or scope cost. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_current_draw_contract.cpp:651` |
| 12 | `DXVK_WAR3_DEBUG_CONSOLE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_war3_debug.h:16` |
| 13 | `DXVK_WAR3_DESTRUCTIBLE_SURVEY` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Phase 7.102 destructible rawcode 调查日志（env gate，默认关）： 用户报告 path blocker 屏蔽视觉验证不通过；可能存在 IsLosBlockerFourCc 没收录的 fourcc 变体。env=1 时前 30 次每个 unique fourcc 各 1 行写入 dxvk log，便于复盘是否需要扩展黑名单。 Phase 7.104 perf 优化：default false，env=1 才执行 atomic load + SafeRead。；`src/d3d9/d3d9_device.cpp:22159` |
| 14 | `DXVK_WAR3_DISABLE_PUBLISH_CONTRACT` | 0u | 默认关；读取点注释表明诊断/对照用途；Phase 7.94：诊断用 — 完全禁用 publish contract hook 的数据层工作。 用于隔离"是 publish hook 导致卡顿还是其他 hook"。；`src/d3d9/war3/render/war3_current_draw_contract.cpp:1501` |
| 15 | `DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE_VERIFY` | 0u | 默认关；名称含诊断/取证标记；Opt-in proof mode deliberately performs the legacy second lookup and asserts that both routes select the identical node. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:3417` |
| 16 | `DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE` | 0u | 默认关；名称含诊断/取证标记；The historical fingerprint covers the position source and draw range but not the complete IB/UV/topology/source descriptor. A matching value can therefore reuse stale backing and make bridge/ramp shadows flicker. Fail closed until the complete descriptor verifier is implemented; the old reuse contra；`src/d3d9/d3d9_device.cpp:3432` |
| 17 | `DXVK_WAR3_FRAME_EVIDENCE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Display-only snapshot: capture and resource lifetime remain recorder-owned.；`src/d3d9/war3/tools/war3_frame_evidence.cpp:123` |
| 18 | `DXVK_WAR3_FRAME_EVIDENCE_CASTERS` | OFF（== "1"） | 默认关；名称含诊断/取证标记；Finalized caster inputs are copied as bounded metadata while the frame owner is still valid. This is not a replay authorization.；`src/d3d9/d3d9_war3_pipeline.cpp:1353` |
| 19 | `DXVK_WAR3_FRAME_EVIDENCE_DRAWS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_frame_evidence.cpp:124` |
| 20 | `DXVK_WAR3_FRAME_EVIDENCE_INPUTS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_frame_evidence.cpp:123` |
| 21 | `DXVK_WAR3_FRAME_EVIDENCE_OUTPUT` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_frame_evidence.cpp:138` |
| 22 | `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_frame_evidence.cpp:124` |
| 23 | `DXVK_WAR3_FRAME_TIMELINE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/tools/war3_frame_timeline.cpp:75` |
| 24 | `DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:208` |
| 25 | `DXVK_WAR3_GPU_SKIN_DIAGNOSTICS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:206` |
| 26 | `DXVK_WAR3_GPU_SKIN_DIFF_EVERY_N` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:212` |
| 27 | `DXVK_WAR3_GPU_SKIN_DIFF_PERIOD` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:214` |
| 28 | `DXVK_WAR3_GPU_SKIN_DRAW_CHAIN_TIMING` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_device.cpp:719` |
| 29 | `DXVK_WAR3_INTERNAL_EXIT_TEST` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Separate exact opt-in. Not part of the public author API or normal exit.；`src/d3d9/war3/tools/war3_internal_test_api.cpp:957` |
| 30 | `DXVK_WAR3_LEGACY_MATERIAL_SIGNATURE_SCOPE` | 0u | 默认关；读取点注释表明诊断/对照用途；The old coarse scope runs once per material lookup and its own push/pop cost is larger than a cache-hit lookup. Keep it only as a same-DLL A/B fallback; normal diagnostics use the globally sampled detail scope.；`src/d3d9/d3d9_device.cpp:5250` |
| 31 | `DXVK_WAR3_LEGACY_PER_DRAW_SEMANTIC_SCOPES` | 0u | 默认关；读取点注释表明诊断/对照用途；The coarse profiler scope builds multiple std::string paths, touches the section cache and queues a delta on every push/pop. BuildSemantic and NativeHint execute hundreds of times per frame, so always-on coarse scopes materially perturb the path they are intended to measure. Keep the old behavior as；`src/d3d9/d3d9_device.cpp:3327` |
| 32 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:126` |
| 33 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY_ASSERT` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:124` |
| 34 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:99` |
| 35 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY_ASSERT` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:97` |
| 36 | `DXVK_WAR3_MODEL_LOG` | false | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/model/war3_model_hook.cpp:9469` |
| 37 | `DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；Historical diagnostic spelling remains a fallback only. The canonical policy/UI/report state is DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/hooks/war3_hook_shadow.cpp:1511` |
| 38 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_DIAGNOSTIC_UNSHADOWED` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；Diagnostic only: keep cube publication and color takeover, bypass only automatic-light visibility. Never changes the authored channel.；`src/d3d9/d3d9_native_light.cpp:262` |
| 39 | `DXVK_WAR3_NETEVENT_LOG` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/core/war3_net_event_hook.h:230` |
| 40 | `DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME` | false | 默认关；读取点注释表明诊断/对照用途；Generation-backed object bounds remain Observe-only by default. A physical A/B must prove zero false negatives before this experimental switch may consume C2/C3 visibility decisions. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_war3_shadow.cpp:4307` |
| 41 | `DXVK_WAR3_PERF_AUTO_EXPORT_SEC` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；周期自动导出（秒）：用于无人值守自动化采样。；`src/d3d9/war3/tools/war3_perf_monitor.cpp:1104` |
| 42 | `DXVK_WAR3_PERF_CURRENT_DRAW_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；This is deliberately separate from the frame-level snapshot breakdown. It adds fixed-ID QPC regions to the sampled CurrentDraw HotHook tree and therefore stays opt-in at PERF_LEVEL=2. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_current_draw_contract.cpp:660` |
| 43 | `DXVK_WAR3_PERF_DRAW_SAMPLE_PERIOD` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；per-draw 采样周期（flush 粒度，默认 8；PERF_LEVEL=2 时生效）。；`src/d3d9/war3/core/war3_internal_test_config.h:427` |
| 44 | `DXVK_WAR3_PERF_HISTORY_FRAMES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_perf_monitor.cpp:1059` |
| 45 | `DXVK_WAR3_PERF_HISTORY_SEC` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_perf_monitor.cpp:1066` |
| 46 | `DXVK_WAR3_PERF_KEEP_RECORDING_ON_GAME_START` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_swapchain.cpp:100` |
| 47 | `DXVK_WAR3_PERF_LEVEL` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；PERF_LEVEL：0=off，1=frame（默认，仅帧级低频 scope），2=detail（含 per-draw 采样）。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/core/war3_internal_test_config.h:415` |
| 48 | `DXVK_WAR3_PERF_MONITOR` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。 ／ 全局主开关：=0 时整个性能监控系统关闭（所有 scope/采样入口均为空操作）， 用于量化监控自身的运行时开销（monitor on/off A/B）。；`src/d3d9/war3/tools/war3_perf_monitor.cpp:916` |
| 49 | `DXVK_WAR3_PERF_OVERRIDE_GRAPH_BREAKDOWN_HOOKS` | false | 默认关；名称含诊断/取证标记；Optional third-level split below EvaluateOverrideGraph. Keep this behind its own opt-in gate so the two additional detours never enter the default or second-level diagnostic configuration. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/model/war3_model_hook.cpp:2475` |
| 50 | `DXVK_WAR3_PERF_PENDING_MAX` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_perf_monitor.cpp:1077` |
| 51 | `DXVK_WAR3_PERF_PUBLISH_VISIBLE_BREAKDOWN` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；诊断门在进程期只解析一次；必须同时满足 detail 级和显式环境变量， 普通 PERF_LEVEL=0/1 以及未显式启用的 level 2 都保持关闭。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/hooks/war3_hook_render.cpp:1437` |
| 52 | `DXVK_WAR3_PERF_PUBLISH_VISIBLE_SAMPLE_PERIOD` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；周期同样只解析一次。非法值回退 8，4096 上限避免误配置导致权重 乘法和长窗口统计失去可读性。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/hooks/war3_hook_render.cpp:1449` |
| 53 | `DXVK_WAR3_PERF_RECORD_AFTER_GAME_START` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_swapchain.cpp:113` |
| 54 | `DXVK_WAR3_PERF_RECORD_ON_START` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；自动化常用：若通过环境变量开启“启动即录制”，默认不在进图后重置。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。 ／ 自动录制开关：便于无人值守压测/自动化回归。；`src/d3d9/d3d9_swapchain.cpp:105` |
| 55 | `DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；These are native queue-internal detours. Keep them out of production and frame-level profiling until each entry has an isolated runtime gate. ／ This detour was historically expensive even with an empty probe. Preserve the original business-probe gate instead of charging every draw in normal or frame；`src/d3d9/war3/hooks/war3_hook_render.cpp:2147` |
| 56 | `DXVK_WAR3_PERF_SHADOW_PHASE_BREAKDOWN` | false | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_war3_shadow.cpp:720` |
| 57 | `DXVK_WAR3_PERF_SPRITE_FRAME_HOOKS` | false | 默认关；名称含诊断/取证标记；SpriteFrameUpdate 四入口是 WorldFramePrepare 原生阶段中的高频边界。 默认不因性能监控而安装；只有 detail 级且显式 opt-in 时，才允许在 pose/dt producer 都关闭的配置下安装纯 trampoline 诊断 Hook。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/model/war3_model_hook.cpp:2436` |
| 58 | `DXVK_WAR3_PERF_SPRITE_NATIVE_BREAKDOWN_HOOKS` | false | 默认关；名称含诊断/取证标记；SpriteFrameUpdate 四入口是 WorldFramePrepare 原生阶段中的高频边界。 默认不因性能监控而安装；只有 detail 级且显式 opt-in 时，才允许在 pose/dt producer 都关闭的配置下安装纯 trampoline 诊断 Hook。 ／ Optional second-level split below the four CSprite frame-update entry points. This mode deliberately reuses only three IDA-grounded helpers whose fastcall A；`src/d3d9/war3/model/war3_model_hook.cpp:2437` |
| 59 | `DXVK_WAR3_PERF_TRACE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。 ／ 环境变量只在首次调用时解析；热路径后续只读一个静态 bool。；`src/d3d9/war3/tools/war3_perf_monitor.cpp:921` |
| 60 | `DXVK_WAR3_PERF_TRACE_SAMPLE_PERIOD` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_perf_monitor.cpp:3057` |
| 61 | `DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；The five native transparent-dispatch entry points do not share one ABI. Keep their timing detours opt-in until every signature has passed an isolated per-type crash gate. The enclosing FlushTransparent hook remains enabled, so the queue still has a stable aggregate timing boundary. ／ Type0 is a corr；`src/d3d9/war3/hooks/war3_hook_render.cpp:1964` |
| 62 | `DXVK_WAR3_PERF_WINDOW_SEC` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_perf_monitor.cpp:3896` |
| 63 | `DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Third diagnostic tier for the still-unattributed Prepare native self. Every target below has a mechanically proven 1.27a ABI. This remains an installation-time opt-in because even sampled observers change the code entry points whose cost they measure. ／ This must run before any Render-domain detour；`src/d3d9/war3/hooks/war3_hook_render.cpp:2197` |
| 64 | `DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；These detours exist only to partition the native WorldFrameUpdateAndPreparePasses body. Requiring both switches keeps the normal and level-1 paths at exactly zero installation/runtime overhead. ／ This must run before any Render-domain detour changes the opcode bytes included in the exact Game.dll fi；`src/d3d9/war3/hooks/war3_hook_render.cpp:2163` |
| 65 | `DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；This second diagnostic tier partitions three still-unattributed direct callees without changing the established seven-hook deep probe. It is installation-time opt-in so level 0/1 and ordinary level 2 remain byte- for-byte on the original call path. ／ This must run before any Render-domain detour cha；`src/d3d9/war3/hooks/war3_hook_render.cpp:2180` |
| 66 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_D3D9_OWNER_MODE` | 0u | 默认关；读取点注释表明诊断/对照用途；Separate from CPU source evidence so the proven low-overhead Stage11 observer can still run without allocating an atlas. Consume remains a hard failure in this stage.；`src/d3d9/d3d9_device.cpp:2436` |
| 67 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_STAGE11_EVIDENCE_MODE` | 0u | 默认关；名称含诊断/取证标记；This bridge is evidence-only. Keep release behavior entirely absent until an explicit Observe request; Consume remains a hard capability failure.；`src/d3d9/d3d9_device.cpp:2419` |
| 68 | `DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS` | OVERRIDE（未设置保留内部默认） | 默认仅显式覆盖生效；名称含诊断/取证标记；点光源运行时入口：默认关闭；可由 JASS 命令或环境变量打开。；`src/d3d9/d3d9_war3_pipeline.cpp:585` |
| 69 | `DXVK_WAR3_POINT_SHADOW_DEBUG_DISTANCE` | OFF（== "1"） | 默认关；名称含诊断/取证标记；Explicit diagnostic only, consumed exclusively by debug view 6.；`src/d3d9/d3d9_war3_shadow.cpp:8790` |
| 70 | `DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT` | OVERRIDE（未设置保留内部默认） ／ OFF（EnvIntOverride 未设置返回 -1） | 默认仅显式覆盖生效；名称含诊断/取证标记；点光源运行时入口：默认关闭；可由 JASS 命令或环境变量打开。 ／ 2026-07-21 优化：进程环境变量在启动后不可变，每帧两次 getenv+string 分配是纯浪费。与其他 flag 一致改为静态缓存（语义完全等价）。；`src/d3d9/d3d9_war3_pipeline.cpp:501` |
| 71 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY` | 0u | 默认关；名称含诊断/取证标记；VERIFY-only：复制并完整 rebind 独立 legacy materialization shadow， 对照 mapping + append 输入；绝不执行第二次 append。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:2481` |
| 72 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY_ASSERT` | 0u | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:2488` |
| 73 | `DXVK_WAR3_PREPARED_SLICE_PROBE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/hooks/war3_hook_render.cpp:1316` |
| 74 | `DXVK_WAR3_PREPARED_SLICE_PROBE_SAMPLE_RATE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/hooks/war3_hook_render.cpp:1325` |
| 75 | `DXVK_WAR3_PROFILE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/core/war3_runtime_profile.cpp:221` |
| 76 | `DXVK_WAR3_PUBLISH_PROBE` | 0u | 默认关；名称含诊断/取证标记；Phase 7.77：Phase 7.49 用来诊断 Pose stutter / FROZEN 段的 publish-side probe 在 PublishCurrentDrawContract 主路径里**始终运行**，每次 publish 至少 5 次 atomic CAS-loop + frameTag 内存读，每帧 10K-30K publish 调用累计 1-3 ms 纯诊断开销。Phase 7.55 v4 + Phase 7.57 producer 已闭环 stutter 根因， 所以默认关掉这一组诊断，只有 `DXVK_WAR3_PUBLISH_PROBE=1` 显式开启时才；`src/d3d9/war3/render/war3_current_draw_contract.cpp:972` |
| 77 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；SafeCopy passed a period-1 shadow verifier over 670k production calls and a same-DLL reverse A/B/A/B gate. It is now the production default; exact env value 0 remains the immediate rollback to historical SafeReadPtrFast. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/hooks/war3_hook_render.cpp:1499` |
| 78 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_ASSERT` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；SafeCopy passed a period-1 shadow verifier over 670k production calls and a same-DLL reverse A/B/A/B gate. It is now the production default; exact env value 0 remains the immediate rollback to historical SafeReadPtrFast. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/hooks/war3_hook_render.cpp:1497` |
| 79 | `DXVK_WAR3_REGISTRY_HEALTH_VERIFY` | OFF（== "1"） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/model/war3_model_registry.cpp:160` |
| 80 | `DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT` | OFF（== "1"） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/model/war3_model_registry.cpp:161` |
| 81 | `DXVK_WAR3_RENDER_LOG` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_war3_debug.h:64` |
| 82 | `DXVK_WAR3_RENDERER_CORE_TIMING_PROBE` | OFF（== "1"） | 默认关；名称含诊断/取证标记；2026-07-21 优化：逐记录 slowest*Us 计时诊断默认关闭。开启前每条 manifest 记录要付 ~20 次 steady_clock::now()（9 个 ScopedMaxUs + 6 个 ResolvePhaseTimer + 循环体 recordStart），与被测的 resolveRecord 本身 同量级；这些 slowest*Us 只用于 control-plane 诊断展示，不影响任何 功能路径。`DXVK_WAR3_RENDERER_CORE_TIMING_PROBE=1` 显式恢复完整计时。；`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:61` |
| 83 | `DXVK_WAR3_RESOURCE_CENSUS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/tools/war3_resource_residency_census.cpp:66` |
| 84 | `DXVK_WAR3_S1_FORCE_IDENTITY_WORLD` | 0u | 默认关；读取点注释表明诊断/对照用途；Diagnostic A/B for bridge/ramp flicker. Native Stage1 can contain both terrain tiles and terrain-owned decorations; this switch tests whether a stale D3DTS_WORLD transform is moving the otherwise valid replay geometry. It must stay opt-in until a same-frame matrix/content contract proves that every；`src/d3d9/d3d9_device.cpp:2339` |
| 85 | `DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE_STATS` | OFF（== "1"） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:119` |
| 86 | `DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；独立于旧的 SEMANTIC_SUBMIT_BREAKDOWN。旧开关会在每次 BuildPacket 内创建二十多个 scope，诊断本身足以显著放大 BuildEligible。这里仅启用 每帧粗分段和低频 record 采样，允许在产品路径其余探针关闭时取样。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:1318` |
| 87 | `DXVK_WAR3_SEMANTIC_DIRECT_OWNER_SCAN` | 0u | 默认关；读取点注释表明诊断/对照用途；Direct current-draw packets already carry renderablePart/meshPayload, sceneNode and authoritative palette/group-slot data. The legacy owner scan walks runtime-model records and copies large records per caster; keep it as an explicit diagnostic fallback instead of the default hot path.；`src/d3d9/d3d9_device.cpp:2959` |
| 88 | `DXVK_WAR3_SEMANTIC_DIRECT_PHASE_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；Frame-level DirectGrouped child phases only. Unlike the legacy submit breakdown this does not create a scope per record/draw, so it can expose SnapshotPreselect and Submit structure without multiplying probe overhead by caster count. Keep it opt-in for clean product A/B runs. ／ 记录到报告里的关键环境变量（schema；`src/d3d9/d3d9_device.cpp:1262` |
| 89 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE` | 0u | 默认关；读取点注释表明诊断/对照用途；1.27a 默认关闭：实机报告证实 D3DRS_VERTEXBLEND 恒为 DISABLE， draw-time D3D palette 路径 100% NoVertexBlend 拒绝（Published=0）。 默认开时每帧数百次空跑会直接抬高 capture 热路径与 Untracked。 诊断时再用 DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE=1。；`src/d3d9/d3d9_device.cpp:2102` |
| 90 | `DXVK_WAR3_SEMANTIC_DYNAMIC_EVIDENCE_STATS` | 0u | 默认关；名称含诊断/取证标记；This predicate performs live CUnit validation but its result feeds only a diagnostic counter; no submit, ownership, lifetime, or rendering decision consumes it. Keep exact collection as an opt-in investigation mode rather than paying one SafeRead chain for every successfully submitted geoset. ／ 记录到报；`src/d3d9/d3d9_device.cpp:2586` |
| 91 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE_VERIFY` | 0u | 默认关；名称含诊断/取证标记；Strong opt-in verifier: repeat the legacy resolver on every successful fast append and abort if the gate/result contract ever diverges.；`src/d3d9/d3d9_device.cpp:1295` |
| 92 | `DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE_VERIFY` | 0u | 默认关；名称含诊断/取证标记；Opt-in rollback verifier. It recomputes the legacy predicates only to assert that the cached eligibility result and the published caster agree. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:2576` |
| 93 | `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_STALE_POSE_ONE_FRAME_RESTORE` | 0u | 默认关；读取点注释表明诊断/对照用途；Missing-core no longer rejects the whole object, so the historical reason for restoring a stale one-frame pose no longer exists. Keep the switch as an explicit diagnostic only; production requires a real current-frame palette/CModel rebuild before a leased skinned packet can be submitted.；`src/d3d9/d3d9_device.cpp:2647` |
| 94 | `DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_visible_renderables.cpp:320` |
| 95 | `DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY_ASSERT` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_visible_renderables.cpp:311` |
| 96 | `DXVK_WAR3_SEMANTIC_MANIFEST_SIBLING_POSE_PROPAGATION` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；Phase 7.31 Iteration G：sibling propagation 默认关闭。 benchmark 下每帧 20K+ 次调用是 Populate 瓶颈之一；Iter B 关闭 stale restore 后，sibling pose-fresh 保守一点只是让 lease 更不容易命中， 这是可接受的（live pose 本来就 fresh）。 保留 env var 开关便于 A/B 复核。；`src/d3d9/war3/render/war3_visible_renderables.cpp:3638` |
| 97 | `DXVK_WAR3_SEMANTIC_OBJECT_FIRST_SNAPSHOT` | 0u | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_device.cpp:2494` |
| 98 | `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` | 0u | 默认关；名称含诊断/取证标记；These historical motion/churn probes do not participate in palette selection, replay validation, or publication. Keep their large lookup tables out of the release hot path unless a targeted capture requests them explicitly. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:2596` |
| 99 | `DXVK_WAR3_SEMANTIC_PERF_TRACK` | false | 默认关；读取点注释表明诊断/对照用途；Phase 7.142：env-gated SemanticPerfEnabled — DXVK_WAR3_SEMANTIC_PERF_TRACK 默认关。每帧 50K-150K SemanticHookPerfScope 构造析构 × chrono/atomic 开销 累计 1ms+/帧。需要诊断时通过 env 启用。；`src/d3d9/war3/model/war3_model_hook.cpp:399` |
| 100 | `DXVK_WAR3_SEMANTIC_SHADOW_MANIFEST_CMODEL_POSE_DIAG` | 0u | 默认关；名称含诊断/取证标记；单独的诊断开关允许不启用 restore 行为也持续观察 pose 状态。；`src/d3d9/war3/render/war3_visible_renderables.cpp:301` |
| 101 | `DXVK_WAR3_SEMANTIC_SHADOW_TRACE` | OFF（== "1"） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:40` |
| 102 | `DXVK_WAR3_SEMANTIC_SLICE_TRACE` | OFF（== "1"） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:48` |
| 103 | `DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；Phase 7.137：runtime gate 改成内联 static + atomic bool 缓存。 原 War3SemanticSubmitScope 函数每个 sub-scope 调用一次（每个 caster 22 次）， hot-path 上的非内联函数调用 + ABI return-by-value 开销不容忽视。 改成 inline，编译器能 strip 掉 disabled 分支的 ScopedCpuScope 对象创建。；`src/d3d9/d3d9_device.cpp:1252` |
| 104 | `DXVK_WAR3_SHADOW_APPEND_SURVEY` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_device.cpp:1046` |
| 105 | `DXVK_WAR3_SHADOW_CAPTURE_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；Default-off recursive probe for the legacy-capture body after all Gates. Raw sampled phases are normalized to the exact always-on PostGate timer at frame flush, so the tree remains closed. Each leaf subtracts the measured QPC lower bound; nested probes can still add a small observer tax to the exact；`src/d3d9/d3d9_device.cpp:3181` |
| 106 | `DXVK_WAR3_SHADOW_DEBUG` | OVERRIDE（未设置保留内部默认） ／ OFF（EnvIntOverride 未设置返回 -1） | 默认仅显式覆盖生效；名称含诊断/取证标记；2026-07-21 优化：进程环境变量在启动后不可变，每帧两次 getenv+string 分配是纯浪费。与其他 flag 一致改为静态缓存（语义完全等价）。；`src/d3d9/d3d9_war3_pipeline.cpp:273` |
| 107 | `DXVK_WAR3_SHADOW_DEBUG_CASTER_STAGE` | OFF（EnvIntOverride 未设置返回 -1） | 默认关；名称含诊断/取证标记；Diagnostic-only isolation used by the bridge/ramp visual probe. Keeping this filter at the final CSM replay boundary proves whether Stage13 geometry itself is present in the shadow map without changing capture, publication, sorting, or receiver behavior.；`src/d3d9/d3d9_war3_shadow.cpp:4120` |
| 108 | `DXVK_WAR3_SHADOW_DRAW_SURVEY` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Shadow draw survey 属于取证输出，默认关闭；需要复查 caster 身份时可用 DXVK_WAR3_SHADOW_DRAW_SURVEY=1 临时打开。；`src/d3d9/d3d9_war3_shadow.cpp:1394` |
| 109 | `DXVK_WAR3_SHADOW_DRAWTIME_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；Default-off recursive split of the exact DrawTimeCapture child. It shares the outer ShadowCapture sampling admission with Gates, then frame-flush normalization closes the mutually-exclusive phases to the exact parent. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:3164` |
| 110 | `DXVK_WAR3_SHADOW_EXACT_INDEX_TRIM` | 0u | 默认关；读取点注释表明诊断/对照用途；Disabled by default after the life-and-death TDR A/B. The CPU-readable IB generation can prove an exact index domain, but the dynamic REAL position backing copied later on the command stream has no matching immutable content generation. Even canonical zero-based indices only delayed the NVIDIA reset；`src/d3d9/d3d9_device.cpp:2383` |
| 111 | `DXVK_WAR3_SHADOW_FAR_CASTER_DEPTH_EXTENSION` | -1.0f | 默认关；读取点注释表明诊断/对照用途；Diagnostic override for separating upstream-caster Z clipping from capture/publication loss. The normal path remains byte-for-byte unchanged when the variable is absent. Values above the production 384-unit volume allowance are intentionally permitted only through this explicit gate.；`src/d3d9/d3d9_war3_shadow.cpp:10419` |
| 112 | `DXVK_WAR3_SHADOW_GATE_BREAKDOWN` | 0u | 默认关；名称含诊断/取证标记；默认关闭：递归热点调查时才启用。采样结果在帧末按 Gates 精确总 ticks 归一化，因此诊断树保持加和闭合。 ／ 记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/d3d9_device.cpp:3143` |
| 113 | `DXVK_WAR3_SHADOW_PASS_TRACE` | 0u | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_device.cpp:1219` |
| 114 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1110` |
| 115 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1135` |
| 116 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTERS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1121` |
| 117 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CONTRACTS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1119` |
| 118 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1123` |
| 119 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CASTERS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1133` |
| 120 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CONTRACTS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1131` |
| 121 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_OBJECTS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1129` |
| 122 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_POSES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1127` |
| 123 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1125` |
| 124 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_OBJECTS` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1117` |
| 125 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_POSES` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:1115` |
| 126 | `DXVK_WAR3_SHADOW_STAGE_HISTOGRAM` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；Exact, diagnostic-only producer census. The visual probe captures this published contract on the render thread alongside the same final backbuffer, so a blinking bridge frame can be compared with a visible bridge frame without relying on a later asynchronously polled report. ／ 记录到报告里的关键环境变量（schema v；`src/d3d9/d3d9_device.cpp:20090` |
| 127 | `DXVK_WAR3_SHADOWMAP_REPLAY_LOG` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_war3_shadow.cpp:3955` |
| 128 | `DXVK_WAR3_SPRITE_HOST_BIND_DISABLE` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；Phase 7.105：RecordSpriteHostOwnerBinding 在 12-人对战图实测每次调用累计 ~600ms 同步阻塞 Game.dll 主线程（4-人地图约 120ms），导致用户报告的 "对战地图开局卡顿 10 秒以上"。 A/B 实测（IceCrown 12-人 30s 窗口）： - 启用此 hook: 25.5 FPS, 84% 帧卡住 - 禁用此 hook: 107.5 FPS, 0 卡住 决策：默认完全跳过 RecordSpriteHostOwnerBinding 的 metadata 抓取。 - 这条 hook 的功能是"把 sprite-h；`src/d3d9/war3/model/war3_model_hook.cpp:7500` |
| 129 | `DXVK_WAR3_STAGE13_COMPONENT_DIAGNOSTICS` | 0u | 默认关；名称含诊断/取证标记；Diagnostic-only O(retained entries) miss analysis. This was used to prove that content/world/material/layout are stable while raw D3D identities rotate. It has no rendering effect and must not tax the default hot path.；`src/d3d9/d3d9_device.cpp:2243` |
| 130 | `DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE` | 0u | 默认关；读取点注释表明诊断/对照用途；Unsafe diagnostic only. A bounded sample is not an exact geometry identity: unobserved indices/position/UV leaves may differ. Do not enable in production; the verifier can reproduce and abort on a false hit.；`src/d3d9/d3d9_device.cpp:2205` |
| 131 | `DXVK_WAR3_STAGE13_SORT_UNIQUE_READS` | 0u | 默认关；读取点注释表明诊断/对照用途；Rejected exact-equivalence locality experiment. Same-DLL ABBA showed no improvement after normalizing against the adjacent IndexParse phase, so production preserves first-reference order. Keep the switch only for repeatable diagnostics on other maps.；`src/d3d9/d3d9_device.cpp:2196` |
| 132 | `DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY` | 0u | 默认关；名称含诊断/取证标记；Diagnostic-only proof gate. An early dynamic-upload cache hit still rebuilds the exact late referenced-content hash and aborts on mismatch. Static mapped sources already use that late hash in production.；`src/d3d9/d3d9_device.cpp:2186` |
| 133 | `DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE` | 0u | 默认关；读取点注释表明诊断/对照用途；Rejected diagnostic: Stage13 has no stable engine object identity, and a bridge test produced a real content mismatch even when exactly one live entry matched world transform, material and complete draw layout. Keep the switch solely to reproduce that verifier result; never enable by default.；`src/d3d9/d3d9_device.cpp:2215` |
| 134 | `DXVK_WAR3_STORM_THRESHOLD_KB` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；只允许把阈值调高做诊断，绝不允许降到小块域。；`src/d3d9/war3/memory/war3_storm_hook.cpp:1014` |
| 135 | `DXVK_WAR3_STREAM1_LAYOUT_PROBE` | 0u | 默认关；名称含诊断/取证标记；2026-07-21 优化：与 Phase 7.49 probe 同款的逐 publish 诊断（CAS + 多次 fetch_add），但这组 counter 全工程无消费者（连 control-plane JSON 都未 导出），属于 Phase 7.77 收口时的遗漏。默认关闭，`DXVK_WAR3_STREAM1_LAYOUT_PROBE=1` 显式开启。；`src/d3d9/war3/render/war3_current_draw_contract.cpp:982` |
| 136 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_LAG_THRESHOLD` | OFF（未设置=关闭） | 默认关；读取点注释表明诊断/对照用途；Phase 7.38：诊断数据证实 Pose 卡顿根因是 record 帧龄导致的 palette 延迟。 paletteCaptureExactHit=100%，submitLiveRebuild=100% 命中，但 Lag>=3 仍占 36.5%。 将默认阈值从 3 降到 1：只要 record 不是当前帧就立即用 PoseRegistry 刷新。 环境变量 DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_LAG_THRESHOLD 可覆盖。；`src/d3d9/d3d9_device.cpp:22534` |
| 137 | `DXVK_WAR3_TEST_INDEX_OVERFLOW` | OFF（未设置=关闭） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_device.cpp:10799` |
| 138 | `DXVK_WAR3_TEST_POINT_LIGHT` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；0 = 全更；1..6 = 每帧 face budget。；`src/d3d9/d3d9_war3_pipeline.cpp:640` |
| 139 | `DXVK_WAR3_TEST_POINT_LIGHT_B` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:657` |
| 140 | `DXVK_WAR3_TEST_POINT_LIGHT_CLEAR` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；0 = 全更；1..6 = 每帧 face budget。；`src/d3d9/d3d9_war3_pipeline.cpp:633` |
| 141 | `DXVK_WAR3_TEST_POINT_LIGHT_G` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:656` |
| 142 | `DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:658` |
| 143 | `DXVK_WAR3_TEST_POINT_LIGHT_R` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:655` |
| 144 | `DXVK_WAR3_TEST_POINT_LIGHT_RANGE` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:654` |
| 145 | `DXVK_WAR3_TEST_POINT_LIGHT_SHADOW` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:659` |
| 146 | `DXVK_WAR3_TEST_POINT_LIGHT_X` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:651` |
| 147 | `DXVK_WAR3_TEST_POINT_LIGHT_Y` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:652` |
| 148 | `DXVK_WAR3_TEST_POINT_LIGHT_Z` | OVERRIDE（未设置保留内部默认） | TEST/AUTOTEST 前缀，测试夹具专用，默认关；（无邻近注释）；`src/d3d9/d3d9_war3_pipeline.cpp:653` |
| 149 | `DXVK_WAR3_VB_ALLOC_SPIKE_LOG` | OFF（未设置=关闭） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_device.cpp:6538` |
| 150 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_visible_renderables.cpp:68` |
| 151 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY_ASSERT` | OFF（仅显式启用生效） | 默认关；名称含诊断/取证标记；记录到报告里的关键环境变量（schema v9 meta.env）。；`src/d3d9/war3/render/war3_visible_renderables.cpp:66` |
| 152 | `DXVK_WAR3_VISUAL_API_DIAGNOSTICS` | OFF（== "1"） | 默认关；名称含诊断/取证标记；（无邻近注释）；`src/d3d9/d3d9_war3_volumetric_light.cpp:103` |

## 5. C 类清单（19 项）

| # | 开关名 | 编译期门 | 用途／不可达依据 |
| ---: | --- | --- | --- |
| 1 | `DXVK_WAR3_COHERENT_REAL_INDEX_TRIM_MODE` | `if constexpr (!kCoherentRealIndexTrimDevelopmentEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3245` |
| 2 | `DXVK_WAR3_COHERENT_UP_INDEX_TRIM_MODE` | `if constexpr (!kCoherentUpIndexTrimDevelopmentEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3224` |
| 3 | `DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3191` |
| 4 | `DXVK_WAR3_CURRENT_UP_SHADOW_REPLAY_MODE` | `if constexpr (!kCurrentUpShadowReplayDevelopmentEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3234` |
| 5 | `DXVK_WAR3_DATA_COLLECTION_SAMPLE_PERIOD` | `#if defined(WARVK_DATA_COLLECTION_TREE_DEV) && WARVK_DATA_COLLECTION_TREE_DEV` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/war3/tools/war3_data_collection_tree.cpp:19` |
| 6 | `DXVK_WAR3_DATA_COLLECTION_TREE` | `#if defined(WARVK_DATA_COLLECTION_TREE_DEV) && WARVK_DATA_COLLECTION_TREE_DEV` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/war3/tools/war3_data_collection_tree.cpp:17` |
| 7 | `DXVK_WAR3_INTERNAL_TEST_API` | `if constexpr (dxvk::war3::internal::kNativeInternalTestApiEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/war3/tools/war3_internal_test_api.cpp:161` |
| 8 | `DXVK_WAR3_POINT_SHADOW_PERSISTENT_PREPARE_MODE` | `if constexpr (kReleaseFreezeExperimentalShadowRoutes)` | 读取点先经 release 冻结门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_war3_shadow.cpp:5702` |
| 9 | `DXVK_WAR3_RTS_SHADOW_BASE_WORLD_TEXEL` | `#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/d3d9_war3_shadow.cpp:10402` |
| 10 | `DXVK_WAR3_RTS_SHADOW_CANDIDATE_MODE` | `#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/d3d9_war3_shadow.cpp:10387` |
| 11 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_BAND_HALF_HEIGHT` | `#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/d3d9_war3_shadow.cpp:10395` |
| 12 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_PADDING` | `#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/d3d9_war3_shadow.cpp:10399` |
| 13 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_PLANE_HEIGHT` | `#if defined(WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV) && \` | 读取点位于编译期 DEV 预处理门内；读取点 `src/d3d9/d3d9_war3_shadow.cpp:10393` |
| 14 | `DXVK_WAR3_SEMANTIC_COMPACT_WORK_TABLE` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:2402` |
| 15 | `DXVK_WAR3_SEMANTIC_PRODUCER_CLAIM_LEDGER` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3313` |
| 16 | `DXVK_WAR3_STAGE11_ALLOC_OBSERVER` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3201` |
| 17 | `DXVK_WAR3_STAGE11_DIRECT_STATIC_SOURCE_MODE` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3214` |
| 18 | `DXVK_WAR3_STAGE11_DIRECT_UPLOAD_SOURCE_MODE` | `if constexpr (!dxvk::war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_device.cpp:3275` |
| 19 | `DXVK_WAR3_UNION_CONSUMER_CULL_MODE` | `if constexpr (!war3::render::kDevelopmentShadowObserversEnabled)` | 读取点先经编译期开发策略门提前返回（Release 二进制内不可达）；读取点 `src/d3d9/d3d9_war3_shadow.cpp:667` |

## 6. 待判定清单（85 项）

| # | 开关名 | 默认（已提取） | 已有证据 | 缺什么证据 |
| ---: | --- | --- | --- | --- |
| 1 | `DXVK_WAR3_ASYNC_SCREENSHOT` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/tools/war3_async_screenshot.cpp:47`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 2 | `DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/hooks/war3_hook_shadow.cpp:1507`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 3 | `DXVK_WAR3_BOOTSTRAP_MINIMAL` | false ／ OFF（仅显式启用生效） | 读取点 `src/d3d9/d3d9_war3_hook.cpp:501`；最小化模式只保留能建立控制管道的 JASS/MainRunner 入口，用于排除 Storm、Shadow producer 与 model provenance 的启动期侵入。 ／ 启动最小化二分只保留运行时激活所需入口；其余 lifecycle hooks 全部延后，避免把事件泵、等待闸门或参数覆盖误归因给图形初始化。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 4 | `DXVK_WAR3_CASTER_COMPOSITION` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_device.cpp:6462`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 5 | `DXVK_WAR3_COHERENT_REAL_HINT_DOMAIN` | dxvk::war3::memory::DefaultWar3CoherentRealHintDomainEnabled | 读取点 `src/d3d9/d3d9_device.cpp:3265`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 6 | `DXVK_WAR3_CRASH_HANDLER_SELFTEST` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/core/war3_crash_handler.cpp:419`；Register last and keep this handler memory-only so recoverable OS/runtime exceptions are never delayed by DbgHelp, disk I/O or module enumeration. | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 7 | `DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL` | false | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:4301`；每个级联预先计算“世界半径 -> NDC 半径”的缩放（row length） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 8 | `DXVK_WAR3_CURRENT_DRAW_GLOBAL_PUBLISH` | 0u | 读取点 `src/d3d9/war3/render/war3_current_draw_contract.cpp:628`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 9 | `DXVK_WAR3_CURRENT_DRAW_REQUIRE_MAIN_WORLD_STAGE` | 0u | 读取点 `src/d3d9/war3/render/war3_current_draw_contract.cpp:878`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 10 | `DXVK_WAR3_CURRENT_DRAW_SAFE_PALETTE_READ` | 0u | 读取点 `src/d3d9/war3/render/war3_current_draw_contract.cpp:955`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 11 | `DXVK_WAR3_DISABLE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/core/war3_runtime_profile.cpp:232`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 12 | `DXVK_WAR3_DISABLE_SHADOW_CAPTURE` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_device.cpp:43225`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 13 | `DXVK_WAR3_DRAWTIME_VB_CACHE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:3369`；Legacy cross-frame cache/shortcut diagnosis gate. Disabling it is fail-closed for every consumer which may reuse a snapshot outside the exact current-frame producer. The separately gated Stage11 current-frame geometry path below may still capture owned GPU bytes, but it can only publish an entry who | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 14 | `DXVK_WAR3_FORCE_HOOKS` | false | 读取点 `src/d3d9/d3d9_war3_hook.cpp:400`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 15 | `DXVK_WAR3_FPS_UNLOCK_ONLY` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/core/war3_runtime_profile.cpp:220`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 16 | `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:204`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 17 | `DXVK_WAR3_GPU_SKIN_MODE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:202`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 18 | `DXVK_WAR3_GPU_SKIN_POISON_SIDECAR` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp:210`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 19 | `DXVK_WAR3_IMGUI_BEFORE_UI` | OFF（== "1"） | 读取点 `src/d3d9/d3d9_device.cpp:19312`；默认在 Present 阶段绘制 ImGui，确保 UI 不被后处理影响。 仅在显式启用开关时，才在 UI 渲染前插入 ImGui。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 20 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:121`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 21 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/shadow/war3_shadow_runtime_contract.cpp:94`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 22 | `DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/hooks/war3_native_capture.cpp:39`；No recording-owner route in the release-base integration. | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 23 | `DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/hooks/war3_native_capture.cpp:38`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 24 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER` | OFF（== "1"） | 读取点 `src/d3d9/d3d9_native_light.cpp:49`；Deferred by the author: retained only as an explicit research opt-in. | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 25 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_FORCE_FALLBACK` | OFF（== "1"） | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:11315`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 26 | `DXVK_WAR3_NATIVE_MODEL_LIGHTS` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/model/war3_native_light_bridge.cpp:517`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 27 | `DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW` | false | 读取点 `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:119`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 28 | `DXVK_WAR3_PALETTE_TREE_OPENING_SKIP` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/model/war3_model_hook.cpp:7105`；Phase 7.105：开局期 (RenderState::isInGame()==false) 大量 SpriteHostBind hook 触发，每次 fire 都会做一次 256-children 的 CollectRuntimeModelTree + per-child matrix palette 读取 + 多次 mutex 写入。12-玩家图实测每次 ~513ms × 57 次 = 30 秒级别同步阻塞 D3D9 主线程 → 用户看到 10s+ 卡顿。 修复策略：开局期跳过整个 palette tree publish。原因：palette 数据的 消费方（shadow pipe | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 29 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_` | 未提取到 | 读取点 `src/d3d9/d3d9_device.cpp:2458`；Content hashing is independently gated because it walks HOST_CACHED current-draw bytes. Release behavior remains zero-overhead and Consume is not accepted until recording and last-use authorities exist. | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 30 | `DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS` | 0u | 读取点 `src/d3d9/war3/render/war3_current_draw_contract.cpp:575`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 31 | `DXVK_WAR3_RUNTIME_TREE_MAX` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/model/war3_model_hook.cpp:6224`；Phase 7.105：原 256 nodes / 1024 link nodes 在 12-人地图上每次 tree walk 累计 ~500ms 同步阻塞 D3D9 主线程（4-人地图约 121ms）。这条 hook 的语义是 "把当前 sprite 绑定的 runtime model tree 预热到 metadata cache"，但实际 渲染不依赖整棵树都 cache 完毕——只要 root + 第一层 child 就够 shadow pipeline 用。后续帧的 RuntimeMatrixWrite/RangeCopy 会自然填充剩余 child。 修复：把 root 树深度降到 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 32 | `DXVK_WAR3_S1_EARLY_FALLBACK_BACKING` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2328`；A fallback draw usually references a frame-rotating ShadowArena/freeze slice. Retaining Rc<DxvkBuffer> keeps the VkBuffer alive, but BeginFrame reuses and overwrites the slice after the arena ring wraps. The early key also intentionally omits dynamic source offsets, so it cannot prove that the bytes | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 33 | `DXVK_WAR3_S1_PERSISTENT_UNSTABLE_SOURCE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2316`；UP/ring-backed S1 sources have no immutable content generation in the persistent key. Promoting them caused bridge/ramp cache churn, stale shadow reuse and hundreds of MiB of short-lived GPU allocations. Keep the historical broad admission only as an explicit rollback. ／ 记录到报告里的关键环境变量（schema v9 meta | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 34 | `DXVK_WAR3_S1_TERRAIN_CASTER_MASK` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:550`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 35 | `DXVK_WAR3_SEMANTIC_ALLOW_SINGLE_PRIM_FULL_INDEX` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:1190`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 36 | `DXVK_WAR3_SEMANTIC_AUGMENT_BATCH_LOOKUP` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:89`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 37 | `DXVK_WAR3_SEMANTIC_AUGMENT_COMPACT_SHADOW_VIEW` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:98`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 38 | `DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp:107`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 39 | `DXVK_WAR3_SEMANTIC_BOOTSTRAP_SUPPLEMENTED_BUILD` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2145`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 40 | `DXVK_WAR3_SEMANTIC_BYPASS_INLINE_REGISTRY_PUBLISH` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2615`；The direct draw-time producer consumes the write-side visible snapshot on the render thread. Publishing all semantic registries for every draw-time bypass candidate was useful as an early bootstrap fallback, but it now creates a large untracked semantic.data cost. Keep it as an opt-in escape hatch f | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 41 | `DXVK_WAR3_SEMANTIC_COVERAGE_DROP_TOLERANCE` | 0u | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:1085`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 42 | `DXVK_WAR3_SEMANTIC_DIRECT_EXPLICIT_BLEND_RESOLVE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:1209`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 43 | `DXVK_WAR3_SEMANTIC_DIRECT_SCAN_CAP` | defaultScanCap | 读取点 `src/d3d9/d3d9_device.cpp:2127`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 44 | `DXVK_WAR3_SEMANTIC_DIRECT_STATIC_SUPPLEMENT` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2133`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 45 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2557`；Correctness fail-closed. This shortcut publishes a caster before the generic canonical geometry/material/pose validation has completed. Dense unit/effect scenes proved that it can accept an input/layout tuple which the canonical append rejects, producing a one-frame giant shadow polygon anchored at | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 46 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2604`；This producer feeds only the unsafe fast append contract. It must not manufacture a Skinned eligible record when fast append is production-off. ／ 记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 47 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2091`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 48 | `DXVK_WAR3_SEMANTIC_MANIFEST_CMODEL_POSE_RESTORE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2666`；pose restore 开启时必须允许探测，否则 restore 路径没有判定依据。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 49 | `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:1225`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 50 | `DXVK_WAR3_SEMANTIC_RECEIVER_DISABLE_POINT_LIGHTS` | false | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:1097`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 51 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_MODE` | false | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:1013`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 52 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_PCF_RADIUS` | -1.0f | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:1061`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 53 | `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2974`；Phase 7.34 AlphaTest 链路已由 Claude heartbeat 2026-05-12 打通： stash（War3TryCaptureShadowCaster 抓 UV/diffuse/alphaRef）→ lookup（War3TryAppendSemanticShadowPacket 查 cache）→ apply（candidate.alphaTestEnabled + UV/diffuse 注入 draw）。 当 payload 存在时 reject helper 会放行合法 cutout；payload 缺失时仍安全拒绝。 真正的 AlphaBlend 由独立的 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 54 | `DXVK_WAR3_SEMANTIC_SHADOW_BUILD_RECORD_CAP` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp:8799`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 55 | `DXVK_WAR3_SEMANTIC_SHADOW_BYPASS_LEGACY_UNIT_CAPTURE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:90`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 56 | `DXVK_WAR3_SEMANTIC_SHADOW_DISABLE_LEGACY_CAPTURE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:99`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 57 | `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD` | false | 读取点 `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:71`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 58 | `DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK` | false | 读取点 `src/d3d9/war3/core/war3_semantic_shadow_gate.cpp:83`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 59 | `DXVK_WAR3_SEMANTIC_SHADOW_WEAK_BEFOREUI_COMMIT` | false | 读取点 `src/d3d9/d3d9_war3_pipeline.cpp:1226`；Semantic shadow has a stricter, explicit data path than the old receiver/capture heuristic. Once the device-side boundary classifier has accepted a non-unknown UI transition after the world phase, commit it directly instead of waiting for a later strong UI hook marker that may not exist on all launc | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 60 | `DXVK_WAR3_SEMANTIC_STEADY_SUPPLEMENTED_BUILD` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2139`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 61 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_BROAD_LEASE_PREFERENCE` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2514`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 62 | `DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP` | dxvk::war3::internal::kShadowSemanticCoreSceneSubmitDrawCap | 读取点 `src/d3d9/d3d9_device.cpp:2108`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 63 | `DXVK_WAR3_SHADOW_ADAPTIVE_MAP_UPDATE` | war3::internal::kShadowAdaptiveMapUpdateEnabled | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:1025`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 64 | `DXVK_WAR3_SHADOW_ARENA_FRAMES` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/memory/war3_shadow_arena.cpp:159`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 65 | `DXVK_WAR3_SHADOW_ARENA_MAX_MB` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/memory/war3_shadow_arena.cpp:142`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 66 | `DXVK_WAR3_SHADOW_ARENA_MB` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/memory/war3_shadow_arena.cpp:149`；兼容旧约定：原来的 DXVK_WAR3_SHADOW_ARENA_MB 是“单帧总容量”。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 67 | `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC` | war3::internal::kShadowDisableTaaForSemanticDynamicCasters | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:699`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 68 | `DXVK_WAR3_SHADOW_FALLBACK_BUDGET_MB` | dxvk::war3::render::IsShadowArenaCaptureEnabled | 读取点 `src/d3d9/d3d9_device.cpp:4700`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 69 | `DXVK_WAR3_SHADOW_STAGE_LIFECYCLE` | OFF（== "1"） | 读取点 `src/d3d9/war3/render/war3_shadow_producer_policy.cpp:70`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 70 | `DXVK_WAR3_SHADOW_TAA_DISABLE_ON_SUN_MOTION` | war3::internal::kShadowSunMotionAwareTaaDisable | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:706`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 71 | `DXVK_WAR3_SHADOW_TAA_MODE` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_war3_pipeline.cpp:63`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 72 | `DXVK_WAR3_SHADOW_TRIM_INDEXED` | OFF（== "1"） | 读取点 `src/d3d9/d3d9_device.cpp:48192`；注意：部分魔兽的 indexed draw 会传入不严格的 MinVertexIndex/NumVertices。 若直接用它裁剪 VB，阴影重放可能读取越界，表现为阴影缺失/闪烁。 因此默认仅对非 indexed 裁剪；如需强制启用可设置： DXVK_WAR3_SHADOW_TRIM_INDEXED=1 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 73 | `DXVK_WAR3_SHADOW_WORLD_UP` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_war3_csm.cpp:159`；... logic removed for conciseness as we default to 1 now ... But let's keep the env var check just in case but initialize to 1 | 守卫条件为 `s_forcedUp == -2`，但该文件内没有任何把 `s_forcedUp` 赋值为 -2 的路径（同文件 `static int s_forcedUp = 1;`，源码注释自述 "was -2"）→ 该环境变量在当前源码中不生效；三分类无"失效开关"一类，故列此并注明。 |
| 74 | `DXVK_WAR3_SKIN_PALETTE_CONTRACT` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/render/war3_skin_palette_selection.h:30`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 75 | `DXVK_WAR3_SPRITE_HOST_BIND_OPENING_SKIP` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/model/war3_model_hook.cpp:1741`；Phase 7.105：12-玩家图开局期间，War3 引擎在 Game.dll 主线程上批量 fire `Hook_CreateSpriteAndBindSourceObject` 这个 SpriteHostBind hook，每 fire 一次会触发： - TryResolveSourceObjectIdentity（多次 registry lookup + agent validation） - TryResolveCurrentRenderOwnerHint - NoteShadowRuntimeIdentity - ModelInstanceRegistry::not | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 76 | `DXVK_WAR3_SPRITE_UBER_DT_PROBE` | dxvk::war3::internal:: kWar3RuntimeConfigInstallSpriteUberDtProbeHooks | 读取点 `src/d3d9/war3/model/war3_model_hook.cpp:2424`；Phase 7.47 dt gate probe - only mode。 打开时 Hook_SpriteFrameUpdate 等入口只记一笔 dt 分桶就 return， 不触发 RecordSpriteFramePoseFromSprite 等重路径；用于短时间诊断。 | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 77 | `DXVK_WAR3_STAGE13_STATIC_RETENTION` | 0u | 读取点 `src/d3d9/d3d9_device.cpp:2177`；Emergency correctness default: the retained Stage13 path must remain opt-in until it has an exact write-time content generation. The former sparse content descriptor could alias two bridge/ramp submeshes and then combine the old position snapshot with the current draw state. Its miss path also perfo | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 78 | `DXVK_WAR3_STORM_NATIVE_SMALL_REPAIR` | 未提取到 | 读取点 `src/d3d9/war3/memory/war3_storm_native_small_repair.cpp:65`；（无邻近注释） | 读取站点形式未纳入已知 helper 模式；需人工确认 fallback 或生效默认值 |
| 79 | `DXVK_WAR3_STORM_TAKEOVER_MODE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/memory/war3_storm_hook.cpp:998`；生产 DLL 只编入稳定的大块模式；全尺寸接管留在独立实验分支。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 80 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_device.cpp:22549`；Phase 7.51：skinned 单位每帧无条件尝试 live rebuild，不再只在 lag >= 阈值时触发。 根因是 Resolve 会被 KeepReadySnapshotOnInvalidCurrentDraw 兜底保留上次 record， 导致"record 时间上是当前帧但 palette bytes 是旧的"的情况无法被 lag 检测到。 每帧做一次 rebuild 成本在 Phase 7.49 数据里约 100 次 / 帧，每次 60 字节级矩阵运算， 对 CPU 压力可忽略；收益是 FROZEN 8 帧段里都能拿到 PoseRegistry 的当前帧 pose。 若 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 81 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_device.cpp:22523`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 82 | `DXVK_WAR3_USER_EXAMPLE` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/war3_user_example.cpp:21`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 83 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX` | OFF（仅显式启用生效） | 读取点 `src/d3d9/war3/render/war3_visible_renderables.cpp:64`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 84 | `DXVK_WAR3_WIDGET_IDENTITY_HOOK` | OFF（未设置=关闭） | 读取点 `src/d3d9/war3/hooks/war3_hook_widget_identity.cpp:147`；（无邻近注释） | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |
| 85 | `DXVK_WAR3_WORKER_PREPARE` | OFF（未设置=关闭） | 读取点 `src/d3d9/d3d9_war3_shadow.cpp:5684`；记录到报告里的关键环境变量（schema v9 meta.env）。 | 缺该开关是否为生产回退/实验开关的权威证据（设计文档、提交记录或编译期门归属） |

## 7. 宏／注释专用开关名（9 项）

这 9 个名字**从不作为 C/C++ 字符串字面量出现**，因此不在 412 之内；它们控制的是编译期常量，或已被移除/仅留注释的运行时开关。

| # | 名称 | 形态 | 证据（file:line） | 判定 |
| ---: | --- | --- | --- | --- |
| 1 | `DXVK_WAR3_CPU_SKIN_HAS_SSE` | 宏定义、预处理条件 | `src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:22`、`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:24`、`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:41`、`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:49`、`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:63`、`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:75` 等 | 编译期宏（非运行时开关）；由同文件内 `#define` 决定 SSE 路径，玩家无法通过环境变量切换。 |
| 2 | `DXVK_WAR3_FORCE_OBJECT_TRACKING` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:896`、`src/d3d9/war3/render/war3_render_state.h:172`、`src/d3d9/war3/render/war3_scene_collector.cpp:406` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 3 | `DXVK_WAR3_NATIVE_QUEUE_SORT` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:57` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 4 | `DXVK_WAR3_NATIVE_RENDERER` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:46`、`src/d3d9/war3/core/war3_internal_test_config.h:50`、`src/d3d9/war3/render/war3_native_renderer_probe.h:9` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 5 | `DXVK_WAR3_NATIVE_RENDERER_REPORT_INTERVAL` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:50`、`src/d3d9/war3/render/war3_native_renderer_probe.h:9` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 6 | `DXVK_WAR3_OBJECT_TRACKING_FULL_RESOLVE` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:900`、`src/d3d9/war3/render/war3_scene_collector.cpp:720` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 7 | `DXVK_WAR3_OUTLINE_ALL` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:921` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 8 | `DXVK_WAR3_OUTLINE_FORCE` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:924` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |
| 9 | `DXVK_WAR3_UNIT_MISS_MARK` | 注释 | `src/d3d9/war3/core/war3_internal_test_config.h:915`、`src/d3d9/war3/render/war3_scene_collector.cpp:766` | 仅注释遗留名；对应能力现由 `war3_internal_test_config.h` 内的编译期常量控制（默认 false），不是可用的运行时开关。 |

代表性原文：

- `DXVK_WAR3_CPU_SKIN_HAS_SSE`：`src/d3d9/war3/gpu_skin/war3_cpu_skin_mt.cpp:22` — `#define DXVK_WAR3_CPU_SKIN_HAS_SSE 1`
- `DXVK_WAR3_FORCE_OBJECT_TRACKING`：`src/d3d9/war3/core/war3_internal_test_config.h:896` — `// 等价于 DXVK_WAR3_FORCE_OBJECT_TRACKING=1`
- `DXVK_WAR3_NATIVE_QUEUE_SORT`：`src/d3d9/war3/core/war3_internal_test_config.h:57` — `// DXVK_WAR3_NATIVE_QUEUE_SORT）`
- `DXVK_WAR3_NATIVE_RENDERER`：`src/d3d9/war3/core/war3_internal_test_config.h:46` — `// 是否强制启用 NativeRendererProbe（无需 DXVK_WAR3_NATIVE_RENDERER）`
- `DXVK_WAR3_NATIVE_RENDERER_REPORT_INTERVAL`：`src/d3d9/war3/core/war3_internal_test_config.h:50` — `// DXVK_WAR3_NATIVE_RENDERER_REPORT_INTERVAL）`
- `DXVK_WAR3_OBJECT_TRACKING_FULL_RESOLVE`：`src/d3d9/war3/core/war3_internal_test_config.h:900` — `// 等价于 DXVK_WAR3_OBJECT_TRACKING_FULL_RESOLVE=1`
- `DXVK_WAR3_OUTLINE_ALL`：`src/d3d9/war3/core/war3_internal_test_config.h:921` — `// 等价于 DXVK_WAR3_OUTLINE_ALL=1（注意：会对所有对象强制判定为描边目标）`
- `DXVK_WAR3_OUTLINE_FORCE`：`src/d3d9/war3/core/war3_internal_test_config.h:924` — `// 等价于 DXVK_WAR3_OUTLINE_FORCE=1（强制开启描边渲染开关）`
- `DXVK_WAR3_UNIT_MISS_MARK`：`src/d3d9/war3/core/war3_internal_test_config.h:915` — `// 等价于 DXVK_WAR3_UNIT_MISS_MARK=1`

## 8. 仅在文档中出现的孤立开关名（3 项，不计入 421）

以下三个名字在 `src/` 的全部 C/C++ 源码中**没有任何读取点**，只出现在 `src/d3d9/war3/render/war3_render_optimization_guide.md` 这一内部优化指南里；其中两个在指南中以双引号形式出现，因此会被"字符串字面量"型检索误捕，但源码并不实现它们。

| # | 名称 | 出现位置 | 说明 |
| ---: | --- | --- | --- |
| 1 | `DXVK_WAR3_DEBUG_LOG` | `src/d3d9/war3/render/war3_render_optimization_guide.md:267` | 无 `src/` C/C++ 读取点，属文档遗留名 |
| 2 | `DXVK_WAR3_ENABLE_CALLSITE_PATCH` | `src/d3d9/war3/render/war3_render_optimization_guide.md:126`、`src/d3d9/war3/render/war3_render_optimization_guide.md:261`、`src/d3d9/war3/render/war3_render_optimization_guide.md:290`、`src/d3d9/war3/render/war3_render_optimization_guide.md:297`、`src/d3d9/war3/render/war3_render_optimization_guide.md:304`、`src/d3d9/war3/render/war3_render_optimization_guide.md:311` | 无 `src/` C/C++ 读取点，属文档遗留名 |
| 3 | `DXVK_WAR3_ENABLE_STATE_CACHE` | `src/d3d9/war3/render/war3_render_optimization_guide.md:127`、`src/d3d9/war3/render/war3_render_optimization_guide.md:264`、`src/d3d9/war3/render/war3_render_optimization_guide.md:291`、`src/d3d9/war3/render/war3_render_optimization_guide.md:298`、`src/d3d9/war3/render/war3_render_optimization_guide.md:305`、`src/d3d9/war3/render/war3_render_optimization_guide.md:312` | 无 `src/` C/C++ 读取点，属文档遗留名 |

## 9. 复核方法与数字核对

### 9.1 检索方法

递归遍历 B 树，使用纯 Node.js `fs` 实现（不依赖 glob/grep 工具）。对 `src/` 下 782 个 C/C++ 源文件做逐行扫描：

- 先用一个保长度的掩码器把注释与字符串内容置空，用于预处理门、`if constexpr` 块、花括号深度与函数边界的结构分析；
- 再在原始行上以 `DXVK_WAR3_[A-Z0-9_]+` 提取名字，并用引号奇偶性判定该名字是否位于双引号字面量内；
- 每个读取点记录：所在预处理条件栈、最近的前置 `if constexpr` 开发策略门、所在函数起始行、函数内最近的注释块、所在运行时守卫条件，以及跨行拼接后的调用表达式（用于提取 fallback）。
- 玩家向文档面按任务定义独立扫描：`WarVK/**`、递归 `README*`/`CHANGELOG*`、`docs/RELEASE_*`，共 78 个文件（排除 `src/`、`AutoTest/`、`build32/`、`subprojects/`）；严格玩家向面另按"仅 `WarVK/` + 根 `README*`/`CHANGELOG*` + `docs/RELEASE_*`"复算。

### 9.2 数字核对

| 计数口径 | 结果 | 说明 |
| --- | ---: | --- |
| `src/` 下 `DXVK_WAR3_` 原文出现次数（任意上下文） | 692 | 含注释、预处理行、Meson 单引号串、Markdown |
| 其中位于双引号字符串内（C/C++ 源文件） | 580 | 即运行时读取点数量 |
| 唯一名字（任意上下文） | 424 | 含 8 个注释专用名 + `CPU_SKIN_HAS_SSE` |
| **唯一名字且至少在 C/C++ 源码中以双引号字面量出现** | **412** | **本报告定义域，与上一会话一致** |
| 唯一名字且以双引号字面量出现（把 `.md` 内双引号也计入） | 414 | 多出 `DXVK_WAR3_ENABLE_CALLSITE_PATCH`、`DXVK_WAR3_ENABLE_STATE_CACHE`，二者仅出现在 `war3_render_optimization_guide.md` |
| 424 减去 412 集合 | 12 | 8 个注释专用 + `CPU_SKIN_HAS_SSE` + `DEBUG_LOG` + `ENABLE_CALLSITE_PATCH` + `ENABLE_STATE_CACHE` |
| 本报告表格行数合计 | 156 + 152 + 19 + 85 + 9 = 421 | 与任务要求的 421 一致（上一会话口径 412 + 9） |

上一会话"412 个字符串字面量"经独立复核**成立**，但需明确其边界：该 412 **不包含** `DXVK_WAR3_ENABLE_CALLSITE_PATCH` 与 `DXVK_WAR3_ENABLE_STATE_CACHE`——这两个名字只在 Markdown 中以双引号出现，源码里不存在读取点（§8）。若把文档内的双引号也算作"字面量"，总数会是 414。

### 9.3 玩家向文档命中数：59 与 63 的差异

| corpus 口径 | 命中开关数 |
| --- | ---: |
| **严格玩家向面**：`WarVK/**` + 根 `README.md`/`README_CN.md`/`CHANGELOG.md` + `docs/RELEASE_*` | **0** |
| 任务字面 `README*`（递归基名，包含 `docs/research\|plan` 工程 README；不含 `AutoTest/`） | 59 |
| 上者再加 `AutoTest/**`（含子模块源码副本） | 69 |
| `docs/**` 全量（含全部研究/计划文档） | 446 |

我无法在本树复现 63。59 与 63 之间不存在由上述任一 corpus 口径产生的中间值；可能来源是上一会话把 `AutoTest/` 的部分文件，或某些非 README 的 `docs/**` 文件计入，但未留下可复核的文件清单。**结论以本报告的 59（含 `AutoTest` 则 69）为准**；59 全部落在内部工程文档，严格玩家向面为 0——按 Q4(b) 该 0 命中**属预期状态（内部面），不再是 A 类缺陷标记**（Q4(b)）。注意 `AutoTest/artifacts/.../original-package/source/` 下是本仓库的**源码副本**，不应作为本树的文档证据。

### 9.4 文档面文件大小与 SHA-256

严格玩家向面文件（全部 **0** 个开关名命中）：

| 文件 | 大小 (bytes) | SHA-256 |
| --- | ---: | --- |
| `README.md` | 10411 | `5974F0178A7E07E40DD5A9B8215A45341A1CBBEE6899D81A398B1423851F44FE` |
| `README_CN.md` | 9624 | `934E82153D323171C1CC810E01F0391D81B45C277CEABDE2A06D76CC0796547A` |
| `CHANGELOG.md` | 24664 | `81BD23129A1BB4A838329AF0AA896FED7884242A3551548A4C2FF16574CDE540` |
| `WarVK/README.md` | 14562 | `08FE3472E345AB6543F55AAA38D900BD2497665D3B781A42D74F6C177DCC5032` |
| `WarVK/jass/warvk_api.j` | 51650 | `0BB04E96D116C974CB165D1A3A962F88A0474146AF3C78844A811536A52ABF66` |
| `WarVK/action.txt` | 25840 | `82705A63FFCF916A6E192AB68FD2E549636AF7C64722E1009F400371E98435D8` |
| `docs/RELEASE_1.2.0.md` | 3864 | `490FFBBCB29510BA5F6C216D1957D5EE293B45ABBAD7DE3ED54BD039942BE6A5` |
| `docs/RELEASE_1.21.00.md` | 4145 | `90F02986A894EF86A1F5F91625B79CE1E0E6CC52A87363FEC6DDD289E4F0A6D8` |
| `docs/RELEASE_NOTES_1.2.0.md` | 2119 | `F930BA48B97FCBE3D1249436390FF3F0807CE9DF1654822B3A772141CE46EB6E` |
| `docs/RELEASE_NOTES_1.21.00.md` | 3036 | `2E7DDD19A101941C9620BC14BDDC3D6216DBC58D6D1B94F740E571A6CDF8419A` |
| `docs/RELEASE_NOTES_1.22.00_DRAFT.md` | 11624 | `B085DB9C1E94A68CC8AC5E28E36CF844321FDBF2124FAC03BDAA9A9EB683D986` |

命中开关名的全部文档文件（11 个，均为内部工程 README；`AutoTest/` 源码副本与仅有通配 `DXVK_WAR3_*` 的 `CHANGELOG.md` 不计入）：

| 文件 | 大小 (bytes) | SHA-256 | 命中开关数 |
| --- | ---: | --- | ---: |
| `docs/plan/automation_exchange/README.md` | 19825 | `76171E1CE8E9205E63B70356BDAF1ED1CBBEADD614D6038F0427BC6B2B3E70C9` | 5 |
| `docs/plan/merge_readiness_audit_2026_05_17/README.md` | 9078 | `E52DE9363CF0355EC8DFC20029BE4CBAD6B195A9E16E720A2364248531C1137D` | 1 |
| `docs/plan/shadow_pose_stutter_investigation_2026_05_11/README.md` | 8223 | `32AB4DE1AA42FFA6FF29C3D505AE6F685818CA47CAC36635F365E1FDF127EFB3` | 2 |
| `docs/research/war3_render_issues/16_phase738_pose_stutter_root_cause/README.md` | 6513 | `C5C66E2853C0F1A4A1C00875B512844A35C3A5EC528738CA3524825B99595DDC` | 1 |
| `docs/research/war3_render_issues/16_storm_memory_hook_reverse/README.md` | 9771 | `21CA81D89DE66CFAD066EF33B4C671895525C68A0C339482557483D7F1530572` | 1 |
| `docs/research/war3_render_issues/25_s1_terrain_csm_occluder/README.md` | 8567 | `F716411F335CD06AA8E31E39208482DF84CA4CFCF9A163EFE4B1034378C2E226` | 3 |
| `docs/research/war3_render_issues/26_volumetric_light_probe/README.md` | 4307 | `333AD3CB78A9F097E85E43215B1EB7EB5942D4A385BC3170B702E40E02A911B8` | 3 |
| `docs/research/war3_render_issues/27_dynamic_lights_and_volumetric_release/README.md` | 30386 | `A837C33A110EFD6C405ABE12A196E57CC78701575B3ACEA9EDADEF07325078D1` | 40 |
| `docs/research/war3_render_issues/28_gpu_skinning_takeover_feasibility/README.md` | 151307 | `752FCBA76AA8F353879B8CF4572FE06F33F4F6497F7A9FA642767EF2DCA57E24` | 4 |
| `docs/research/war3_render_issues/29_hybrid_ray_tracing/README.md` | 20849 | `1398A060892882C5F41D37F82799A011B2CF5342D910136606BA6694654F9B64` | 9 |
| `docs/research/war3_render_issues/32_2026_08_26_render_layer_coverage/README.md` | 32566 | `4C6061C48FF53E492CC6A023D3E99C69D0B2AD16AE936E9DB22172EAAAFA7843` | 3 |

### 9.5 编译期门清单（Meson 选项 → 宏）

| Meson 选项（均默认 false） | 宏 | 相关运行时开关（C 类） |
| --- | --- | --- |
| `warvk_shadow_observers_dev` | `WARVK_ENABLE_SHADOW_OBSERVERS_DEV` → `kDevelopmentShadowObserversEnabled` | `SEMANTIC_COMPACT_WORK_TABLE`、`CSM_TERRAIN_BOUNDS_MODE`、`STAGE11_ALLOC_OBSERVER`、`STAGE11_DIRECT_STATIC_SOURCE_MODE`、`STAGE11_DIRECT_UPLOAD_SOURCE_MODE`、`SEMANTIC_PRODUCER_CLAIM_LEDGER`、`UNION_CONSUMER_CULL_MODE` |
| `warvk_rts_shadow_candidate_dev` | `WARVK_ENABLE_RTS_SHADOW_CANDIDATE_DEV` → `kDevelopmentRtsShadowCandidateEnabled` | `RTS_SHADOW_CANDIDATE_MODE`、`RTS_SHADOW_RECEIVER_PLANE_HEIGHT`、`RTS_SHADOW_RECEIVER_BAND_HALF_HEIGHT`、`RTS_SHADOW_RECEIVER_PADDING`、`RTS_SHADOW_BASE_WORLD_TEXEL` |
| `warvk_coherent_up_index_trim_dev` | `WARVK_ENABLE_COHERENT_UP_INDEX_TRIM_DEV` → `kCoherentUpIndexTrimDevelopmentEnabled` | `COHERENT_UP_INDEX_TRIM_MODE` |
| `warvk_current_up_shadow_replay_dev` | `WARVK_ENABLE_CURRENT_UP_SHADOW_REPLAY_DEV` → `kCurrentUpShadowReplayDevelopmentEnabled` | `CURRENT_UP_SHADOW_REPLAY_MODE` |
| `warvk_coherent_real_index_trim_dev` | `WARVK_ENABLE_COHERENT_REAL_INDEX_TRIM_DEV` → `kCoherentRealIndexTrimDevelopmentEnabled` | `COHERENT_REAL_INDEX_TRIM_MODE` |
| `warvk_coherent_real_perf_candidate_dev` | `WARVK_ENABLE_COHERENT_REAL_PERF_CANDIDATE_DEV` → `kCoherentRealPerformanceCandidateEnabled` | （无独立运行时开关） |
| `warvk_data_collection_tree_dev` | `WARVK_DATA_COLLECTION_TREE_DEV` | `DATA_COLLECTION_TREE`、`DATA_COLLECTION_SAMPLE_PERIOD` |
| `warvk_internal_frame_recorder` | `WARVK_INTERNAL_FRAME_RECORDER_DEFAULT` | 记录器默认档（无独立运行时开关名） |
| `warvk_skin_palette_contract_candidate` | `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` | `SKIN_PALETTE_CONTRACT`（待判定，默认由宏决定） |
| `warvk_device_address_binding_report_dev` | `WARVK_ENABLE_DEVICE_ADDRESS_BINDING_REPORT_DEV` | （无运行时开关） |

另有两类只作用于测试夹具、不产生 `DXVK_WAR3_*` 开关的编译期门：`WARVK_ENABLE_LEGACY_JASS_COMMANDS_DEV`、`WARVK_ENABLE_RAW_SHADERPACK_DEV`（连同 `WARVK_EXPECT_*` 镜像与 `WARVK_JAPI_PROTOCOL_TEST`）。

### 9.6 已修正的判读陷阱与残留不确定性

- **注释误判（已修正 2 处）**：`DXVK_WAR3_BOOTSTRAP_NO_GAME_HOOKS` 的邻近注释含"默认路径不变"，`DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE` 的注释含"默认开时每帧……"，早期规则曾把它们误升为默认开。二者的代码 fallback 都是明确字面量（`false` / `0u`），按"代码字面量优先于注释"规则已改判为 B（诊断用途）。
- **编译期常量解析**：`DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE` 与 `DXVK_WAR3_S1_TERRAIN_PERSISTENT_GEOMETRY` 的 fallback 是符号常量，已解析到 `war3_internal_test_config.h` 的 `kShadowSemanticCoreSceneKeepS1TerrainLegacyCapture = true` 与 `kShadowS1TerrainPersistentGeometryEnabled = true`，判为默认开。
- **不可达开关**：`DXVK_WAR3_SHADOW_WORLD_UP` 的读取点被 `if (s_forcedUp == -2)` 包裹，而 `s_forcedUp` 在同文件初始化为 `1` 且全文件无任何赋值 -2 的路径（源码注释自述"was -2"）。该环境变量在当前源码中不生效，列入待判定并注明。
- 判为 C 的第 2 条依赖"同一函数内 `if constexpr` 提前返回"这一惯用法识别；若某开关在别的函数里还有 Release 可达的读取点，会被误判为 C。§5 的 19 项读取点已逐条人工核对为该开关的唯一有效读取点。
- A 类中"覆写型"条目的内置默认值由未纳入本报告的运行时设置逻辑决定，报告只声明"未设置时保留内置生产默认"；这些条目的**具体数值**属未提取项。
- 待判定中"缺默认值"条目（10 项）的读取站点使用了本报告已覆盖 helper 之外的间接包装；需人工阅读对应函数确认 fallback。


## 10. 扩展维度登记（Q4 裁定补充）

> 本章为 2026-09-16 Q4 裁定（方向 b）后追加的**扩展维度登记**：只补充维度，不改写第 1–9 章的既有分类、默认值与结论。第 3–6 章的 **412** 行在本章逐行对应（A 156／B 152／C 19／待判定 85）；读取点沿用各章已登记的 `file:line`，本章不重复列点。
>
> **口径**：Q4 定调“正式玩家入口以 Ctrl+F1 面板 + JAPI 为主，env 全属内部诊断/兼容/受控排障面”；本次**不删开关、不改默认值、不使既有启动配置失效**，所有已存在的 env 启动配置保持原语义。第 10 章为后补扩展章节。

### 10.1 两个必须分开的分类：默认开启·影响产品 ≠ 公开玩家接口

| 分类 | 判定依据 | 本章结果 |
| --- | --- | ---: |
| **默认开启／影响产品** | 第 3–6 章已登记的默认值：默认开＝是；仅覆盖内置默认或默认关＝否；角色未定＝待判定 | 是 102／否 225（覆写 54＋默认关 152＋Release 不可达 19）／待判定 85 |
| **公开玩家接口** | 是否为面向玩家/地图作者的公开入口 | **否 412／412** |

**412/412 的 `DXVK_WAR3_*` 环境变量都不是公开玩家接口。** 公开配置面是 Ctrl+F1 面板与 WarVK JAPI（`warvk:v1`）；env 名不出现在 `WarVK/`、根 `README*`、`CHANGELOG.md`、`docs/RELEASE_*`（第 1.1 节已证）。因此“默认开启·影响产品”（A 类 102 项）**不得**被当作“玩家可调”；`DXVK_WAR3_POINT_LIGHTS` 一类与 JAPI 同字段的开关，其 env 入口仍是内部面。

### 10.2 各维度判定规则（均可从源码复核）

| 维度 | 判定规则 | 证据来源 |
| --- | --- | --- |
| 所有者 | 读取点文件路径所属子系统：`src/d3d9/war3/<子系统>`，或顶层 `d3d9_*.cpp`/`.h` 主模块 | 读取点所在文件 |
| 读取时机 | ① 语句或 `static ... = []{...}()` IIFE 以 `static` 承接 env 读取 ⇒ `static 首次调用缓存`；② `static` 标志 + `if (!s_checked) s_checked=true;` ⇒ `lazy static 门首次调用`；③ 无 static 的 `getEnvVar`／`GetEnvironmentVariableA`／`ParseEnv*` 每次调用读取 ⇒ `每次调用读取（即时）`；④ 读取点仅位于构造/安装/Hook 安装/锁内初始化函数 ⇒ 归入对应“……一次” | 读取点语句形态与调用点上下文 |
| 重启需求 | `static` 缓存、lazy static 门、构造期/安装期/锁内初始化一次 ⇒ **是**；运行期每次调用读取 ⇒ **否**（下一次调用即取新值）；模式不可判 ⇒ 待判定 | 同上 |
| 与面板/JAPI 的实际优先级 | 读取点属于 `War3RenderPipeline` 构造函数 `m_settings` 覆盖块且该字段存在 `war3shader::Set*`（JASS/JAPI 入口）⇒ JAPI 后写覆盖 env；有同名字段但无 setter ⇒ 无同字段 setter；其余 ⇒ 面板/JAPI 无该开关入口 | `d3d9_war3_pipeline.cpp:258` 构造函数；`war3.h:33-90`（`War3SettingsWrite`）；`d3d9_war3_pipeline.h:208-211`；`war3_shader_api.cpp` 各 `Set*` |
| 发布可达 | 读取点位于 `#if` DEV 门或同函数 `if constexpr` 开发策略门内 ⇒ 否；否则 ⇒ 是（编译进 Release；是否默认生效见第 3–6 章） | 预处理条件栈与 `if constexpr` 门 |
| 支持范围·Q4 归属 | 由第 3–6 章分类映射：默认开/覆写 ⇒ 生产面（env 内部）；B ⇒ 内部诊断/取证；C ⇒ dev-only；待判定 ⇒ 待判定 | 第 3–6 章 |
| 诊断风险 | 高＝注释明示不安全/不得生产启用/会绕过生产正确性验证；中＝默认关的对照、A-B、回退或验证路径，启用后替换既有路径；低＝默认关且注释表明为输出/计数/采样（不参与渲染决策）或 Release 不可达；待判定＝缺运行期影响面证据 | 第 3–6 章注释证据＋读取点 |

两条有源码证据的优先级事实：

1. **面板/JAPI 与 env 的写入方向相反**：env 仅在 `War3RenderPipeline` 构造期写入 render-owner `m_settings`（`src/d3d9/d3d9_war3_pipeline.cpp:258` 起）；JAPI/面板经 `War3SettingsWrite` 写入作者邮箱，并在下一帧安全点提交给 render owner（`war3.h:33-42`、`d3d9_war3_pipeline.h:208-211`）。因此同一字段上**运行期 JAPI/面板写入后写覆盖构造期 env 值**；env 是“启动默认”，不是“最高优先级”。
2. **Ctrl+F1 面板本身不读 env**：`war3_imgui.cpp` 仅调用 `SetRenderScale`/`SetPassEnabled`/`SetParamVec4`，并在界面内注明“完全停用取证：启动前设置 `DXVK_WAR3_FRAME_EVIDENCE=0`，并重新启动游戏”（`war3_imgui.cpp:179-180`）。面板管运行期参数、env 管启动期开关。

### 10.3 维度统计

| 维度 | 取值 | 数量 |
| --- | --- | ---: |
| 读取时机（互斥分组，合计 412） | 全部读取点均为 static 缓存／lazy static 门（进程内一次） | 271 |
| 读取时机（互斥分组，合计 412） | 全部读取点均为每次调用读取 | 135 |
| 读取时机（互斥分组，合计 412） | 混合：同一开关同时存在 static 与每次调用读取点 | 6 |
| 读取时机·调用点（可重叠，排障用） | 调用点仅构造/安装/初始化期一次（非 static 承接） | 40 |
| 读取时机·调用点（可重叠，排障用） | 含运行期每次调用读取（即时） | 96 |
| 读取时机·调用点（单列） | 运行不可达（`s_forcedUp == -2` 守卫恒不成立） | 1 |
| 重启需求 | 是 | 386 |
| 重启需求 | 否 | 25 |
| 重启需求 | 待判定 | 1 |
| 发布可达 | 是（编译进 Release） | 393 |
| 发布可达 | 否（DEV／`if constexpr` 门内） | 19 |
| 与面板/JAPI 优先级 | JAPI 同字段后写覆盖 env | 27 |
| 与面板/JAPI 优先级 | 有同名字段但无 setter | 30 |
| 与面板/JAPI 优先级 | 面板/JAPI 无该开关入口 | 355 |
| 诊断风险 | 高 | 108 |
| 诊断风险 | 中 | 100 |
| 诊断风险 | 低 | 134 |
| 诊断风险 | 待判定 | 70 |
| Q4 归属·支持范围 | 默认开启·影响产品（env 内部面） | 102 |
| Q4 归属·支持范围 | 生产可调覆盖（env 内部面） | 54 |
| Q4 归属·支持范围 | 内部诊断/取证（env 内部面） | 152 |
| Q4 归属·支持范围 | dev-only／Release 不可达 | 19 |
| Q4 归属·支持范围 | 待判定 | 85 |
| — | **合计行数** | **412** |

### 10.4 按前缀分组登记（共 412 行，与第 3–6 章逐行一致）

**`DXVK_WAR3_AA_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | `DXVK_WAR3_AA` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetAaMode），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |

**`DXVK_WAR3_ASYNC_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | `DXVK_WAR3_ASYNC_SCREENSHOT` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_AUTOTEST_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 3 | `DXVK_WAR3_AUTOTEST_DISABLE_BACKGROUND_THROTTLE` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 4 | `DXVK_WAR3_AUTOTEST_DISABLE_GAME_PAUSE` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_BEFOREUI_*` 组（4 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 5 | `DXVK_WAR3_BEFOREUI_REQUIRE_STRONG_UI_MARKER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 6 | `DXVK_WAR3_BEFOREUI_REQUIRE_UIDISPATCH` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 7 | `DXVK_WAR3_BEFOREUI_TIER1` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 8 | `DXVK_WAR3_BEFOREUI_TIER6` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |

**`DXVK_WAR3_BLOCK_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 9 | `DXVK_WAR3_BLOCK_NATIVE_DOODAD_STATIC_SHADOW` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_BOOTSTRAP_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10 | `DXVK_WAR3_BOOTSTRAP_NO_GAME_HOOKS` | Hook 引导 (d3d9_war3_hook) | 启动/Hook 安装期一次（每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 11 | `DXVK_WAR3_BOOTSTRAP_MINIMAL` | Hook 引导 (d3d9_war3_hook) | 启动/Hook 安装期一次（每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_CASTER_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 12 | `DXVK_WAR3_CASTER_COMPOSITION` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_COHERENT_*` 组（4 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 13 | `DXVK_WAR3_COHERENT_REAL_DOMAIN_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 14 | `DXVK_WAR3_COHERENT_REAL_INDEX_TRIM_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 15 | `DXVK_WAR3_COHERENT_UP_INDEX_TRIM_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 16 | `DXVK_WAR3_COHERENT_REAL_HINT_DOMAIN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_CRASH_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 17 | `DXVK_WAR3_CRASH_FIRST_CHANCE_TRACE` | War3 运行时核心 (war3/core) | 崩溃处理器安装期一次（InstallCrashHandlerOnce） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 18 | `DXVK_WAR3_CRASH_HANDLER_SELFTEST` | War3 运行时核心 (war3/core) | 崩溃处理器安装期一次（InstallCrashHandlerOnce） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_CSM_*` 组（6 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 19 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 20 | `DXVK_WAR3_CSM_CONTINUITY_TRACE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 21 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 22 | `DXVK_WAR3_CSM_DESCRIPTOR_REUSE_VERIFY_ASSERT` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 23 | `DXVK_WAR3_CSM_TERRAIN_BOUNDS_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 24 | `DXVK_WAR3_CSM_DISABLE_FAR_CASCADE_CULL` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_CURRENT_*` 组（16 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 25 | `DXVK_WAR3_CURRENT_DRAW_ACTIVE_SLOT_SNAPSHOT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 26 | `DXVK_WAR3_CURRENT_DRAW_BATCHED_BOUNDED_SNAPSHOT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 27 | `DXVK_WAR3_CURRENT_DRAW_GROUP_RANGE_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 28 | `DXVK_WAR3_CURRENT_DRAW_INDEX_SLICE_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 29 | `DXVK_WAR3_CURRENT_DRAW_PUBLISH_AFTER_ORIGINAL` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 30 | `DXVK_WAR3_CURRENT_DRAW_REJECT_SMALL_VIEWPORT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 31 | `DXVK_WAR3_CURRENT_DRAW_SMALL_VIEWPORT_MIN` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 32 | `DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_TRACE_PERIOD` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 33 | `DXVK_WAR3_CURRENT_DRAW_GENERATION_INDEX_SLICE_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 34 | `DXVK_WAR3_CURRENT_DRAW_LAST_SAMPLE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 35 | `DXVK_WAR3_CURRENT_DRAW_REDUNDANT_ATOMICS` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 36 | `DXVK_WAR3_CURRENT_DRAW_SNAPSHOT_BREAKDOWN` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 37 | `DXVK_WAR3_CURRENT_UP_SHADOW_REPLAY_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 38 | `DXVK_WAR3_CURRENT_DRAW_GLOBAL_PUBLISH` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 39 | `DXVK_WAR3_CURRENT_DRAW_REQUIRE_MAIN_WORLD_STAGE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 40 | `DXVK_WAR3_CURRENT_DRAW_SAFE_PALETTE_READ` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_DATA_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 41 | `DXVK_WAR3_DATA_COLLECTION_SAMPLE_PERIOD` | War3 诊断工具 (war3/tools) | 运行期每次调用读取（DEV 门内，Release 不可达） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 42 | `DXVK_WAR3_DATA_COLLECTION_TREE` | War3 诊断工具 (war3/tools) | 运行期每次调用读取（DEV 门内，Release 不可达） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_DEBUG_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 43 | `DXVK_WAR3_DEBUG_CONSOLE` | 调试/日志 (d3d9_war3_debug) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_DESTRUCTIBLE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 44 | `DXVK_WAR3_DESTRUCTIBLE_SURVEY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_DISABLE_*` 组（4 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 45 | `DXVK_WAR3_DISABLE_SHADERPACK` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 46 | `DXVK_WAR3_DISABLE_PUBLISH_CONTRACT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 47 | `DXVK_WAR3_DISABLE` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |
| 48 | `DXVK_WAR3_DISABLE_SHADOW_CAPTURE` | D3D9 设备/主运行时 (d3d9_device) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_DRAWTIME_*` 组（5 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 49 | `DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 50 | `DXVK_WAR3_DRAWTIME_CURRENT_FRAME_GEOMETRY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 51 | `DXVK_WAR3_DRAWTIME_CACHE_ITERATOR_REUSE_VERIFY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 52 | `DXVK_WAR3_DRAWTIME_SOURCE_FINGERPRINT_REUSE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 53 | `DXVK_WAR3_DRAWTIME_VB_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_ENABLE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 54 | `DXVK_WAR3_ENABLE_HOOKS` | Hook 引导 (d3d9_war3_hook) | 启动/Hook 安装期一次（每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |

**`DXVK_WAR3_EXACT_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 55 | `DXVK_WAR3_EXACT_INDEX_DOMAIN_BULK_READ` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |

**`DXVK_WAR3_FORCE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 56 | `DXVK_WAR3_FORCE_HOOKS` | Hook 引导 (d3d9_war3_hook) | 启动/Hook 安装期一次（每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_FPS_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 57 | `DXVK_WAR3_FPS_UNLOCK_ONLY` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_FRAME_*` 组（7 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 58 | `DXVK_WAR3_FRAME_EVIDENCE` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 59 | `DXVK_WAR3_FRAME_EVIDENCE_CASTERS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 60 | `DXVK_WAR3_FRAME_EVIDENCE_DRAWS` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 61 | `DXVK_WAR3_FRAME_EVIDENCE_INPUTS` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 62 | `DXVK_WAR3_FRAME_EVIDENCE_OUTPUT` | War3 诊断工具 (war3/tools) | 运行期按需读取（取证写盘路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 63 | `DXVK_WAR3_FRAME_HISTORY_SELF_CONTAINED` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 64 | `DXVK_WAR3_FRAME_TIMELINE` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_FXAA_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 65 | `DXVK_WAR3_FXAA_SUBPIX` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetFxaaParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |

**`DXVK_WAR3_GPU_*` 组（8 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 66 | `DXVK_WAR3_GPU_SKIN_DIAG_PERIOD_FRAMES` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 67 | `DXVK_WAR3_GPU_SKIN_DIAGNOSTICS` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 68 | `DXVK_WAR3_GPU_SKIN_DIFF_EVERY_N` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 69 | `DXVK_WAR3_GPU_SKIN_DIFF_PERIOD` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 70 | `DXVK_WAR3_GPU_SKIN_DRAW_CHAIN_TIMING` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 71 | `DXVK_WAR3_GPU_SKIN_EXECUTION_ROUTE` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 72 | `DXVK_WAR3_GPU_SKIN_MODE` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 73 | `DXVK_WAR3_GPU_SKIN_POISON_SIDECAR` | War3 GPU skin (war3/gpu_skin) | GPU skin 运行时配置初始化一次（fromEnvironment） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_IMGUI_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 74 | `DXVK_WAR3_IMGUI_BEFORE_UI` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_INTERNAL_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 75 | `DXVK_WAR3_INTERNAL_EXIT_TEST` | War3 诊断工具 (war3/tools) | 内部测试 API 请求时读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 76 | `DXVK_WAR3_INTERNAL_TEST_API` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_KEEP_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 77 | `DXVK_WAR3_KEEP_S1_TERRAIN_LEGACY_CAPTURE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 78 | `DXVK_WAR3_KEEP_STAGE10_TERRAIN_DOODAD_LEGACY_CAPTURE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 79 | `DXVK_WAR3_KEEP_STAGE13_WORLDOBJECT_LEGACY_CAPTURE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |

**`DXVK_WAR3_LEGACY_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 80 | `DXVK_WAR3_LEGACY_MATERIAL_SIGNATURE_SCOPE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 81 | `DXVK_WAR3_LEGACY_PER_DRAW_SEMANTIC_SCOPES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |

**`DXVK_WAR3_MANIFEST_*` 组（6 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 82 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 83 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE_VERIFY_ASSERT` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 84 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 85 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_VERIFY_ASSERT` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 86 | `DXVK_WAR3_MANIFEST_MODEL_RESOURCE_CACHE` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 87 | `DXVK_WAR3_MANIFEST_SOURCE_BACKING_FAST_PATH` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_MODEL_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 88 | `DXVK_WAR3_MODEL_HOOK` | War3 模型/姿态桥 (war3/model) | 模型 Hook 安装期一次（Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 89 | `DXVK_WAR3_MODEL_POSE_HOOK` | War3 模型/姿态桥 (war3/model) | 模型 Hook 安装期一次（Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 90 | `DXVK_WAR3_MODEL_LOG` | War3 模型/姿态桥 (war3/model) | 模型 Hook 安装期一次（Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_NATIVE_*` 组（9 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 91 | `DXVK_WAR3_NATIVE_HINT_PRODUCERLESS_SKIP` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 92 | `DXVK_WAR3_NATIVE_DOODAD_STATIC_STAMP` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 93 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_DIAGNOSTIC_UNSHADOWED` | 原生光照 (d3d9_native_light) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 94 | `DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 95 | `DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 96 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_CONSUMER` | 原生光照 (d3d9_native_light) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 97 | `DXVK_WAR3_NATIVE_MODEL_LIGHT_FORCE_FALLBACK` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 98 | `DXVK_WAR3_NATIVE_MODEL_LIGHTS` | War3 模型/姿态桥 (war3/model) | 原生光照桥安装期一次（Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 99 | `DXVK_WAR3_NATIVE_SEMANTIC_SHADOW_PREVIEW` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_NETEVENT_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 100 | `DXVK_WAR3_NETEVENT_LOG` | War3 运行时核心 (war3/core) | 运行期每次日志调用读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_OBJECT_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 101 | `DXVK_WAR3_OBJECT_BOUNDS_CULL_CONSUME` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |

**`DXVK_WAR3_PALETTE_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 102 | `DXVK_WAR3_PALETTE_ARBITRATION_STRICT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 103 | `DXVK_WAR3_PALETTE_TREE_OPENING_SKIP` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_PERF_*` 组（26 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 104 | `DXVK_WAR3_PERF_SHADOW_PHASE_SAMPLE_PERIOD` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 105 | `DXVK_WAR3_PERF_AUTO_EXPORT_SEC` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 106 | `DXVK_WAR3_PERF_CURRENT_DRAW_BREAKDOWN` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 107 | `DXVK_WAR3_PERF_DRAW_SAMPLE_PERIOD` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 108 | `DXVK_WAR3_PERF_HISTORY_FRAMES` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 109 | `DXVK_WAR3_PERF_HISTORY_SEC` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 110 | `DXVK_WAR3_PERF_KEEP_RECORDING_ON_GAME_START` | 交换链/Present (d3d9_swapchain) | 运行期每次 Present 检查读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 111 | `DXVK_WAR3_PERF_LEVEL` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 112 | `DXVK_WAR3_PERF_MONITOR` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 113 | `DXVK_WAR3_PERF_OVERRIDE_GRAPH_BREAKDOWN_HOOKS` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 114 | `DXVK_WAR3_PERF_PENDING_MAX` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 115 | `DXVK_WAR3_PERF_PUBLISH_VISIBLE_BREAKDOWN` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 116 | `DXVK_WAR3_PERF_PUBLISH_VISIBLE_SAMPLE_PERIOD` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 117 | `DXVK_WAR3_PERF_RECORD_AFTER_GAME_START` | 交换链/Present (d3d9_swapchain) | 运行期每次 Present 检查读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 118 | `DXVK_WAR3_PERF_RECORD_ON_START` | 交换链/Present (d3d9_swapchain) | 运行期每次 Present 检查读取（即时）；性能监控单例构造期一次（首次 instance()） | 是（存在进程内一次性读取点） | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 119 | `DXVK_WAR3_PERF_RENDERQUEUE_DEEP_HOOKS` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 120 | `DXVK_WAR3_PERF_SHADOW_PHASE_BREAKDOWN` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 121 | `DXVK_WAR3_PERF_SPRITE_FRAME_HOOKS` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 122 | `DXVK_WAR3_PERF_SPRITE_NATIVE_BREAKDOWN_HOOKS` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 123 | `DXVK_WAR3_PERF_TRACE` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 124 | `DXVK_WAR3_PERF_TRACE_SAMPLE_PERIOD` | War3 诊断工具 (war3/tools) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 125 | `DXVK_WAR3_PERF_TRANSPARENT_DISPATCH_HOOKS` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 126 | `DXVK_WAR3_PERF_WINDOW_SEC` | War3 诊断工具 (war3/tools) | 性能监控单例构造期一次（首次 instance()） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 127 | `DXVK_WAR3_PERF_WORLD_PREPARE_CORE_HOOKS` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次）；性能 Hook 安装期一次（Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 128 | `DXVK_WAR3_PERF_WORLD_PREPARE_DEEP_HOOKS` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次）；性能 Hook 安装期一次（Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 129 | `DXVK_WAR3_PERF_WORLD_PREPARE_RESIDUAL_HOOKS` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次）；性能 Hook 安装期一次（Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |

**`DXVK_WAR3_PERSISTENT_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 130 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_D3D9_OWNER_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |
| 131 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_STAGE11_EVIDENCE_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 132 | `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_POINT_*` 组（22 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 133 | `DXVK_WAR3_POINT_LIGHTS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetPointLightShadowIntensity），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 134 | `DXVK_WAR3_POINT_RAY_SHADOW` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowEnabled） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 135 | `DXVK_WAR3_POINT_RAY_SHADOW_HIZ` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowHiZEnabled） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 136 | `DXVK_WAR3_POINT_RAY_SHADOW_MAX_DISTANCE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowMaxDistance） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 137 | `DXVK_WAR3_POINT_RAY_SHADOW_MAX_LIGHTS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowMaxLights） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 138 | `DXVK_WAR3_POINT_RAY_SHADOW_START_OFFSET` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowStartOffset） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 139 | `DXVK_WAR3_POINT_RAY_SHADOW_STEPS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowSteps） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 140 | `DXVK_WAR3_POINT_RAY_SHADOW_STRENGTH` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowStrength） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 141 | `DXVK_WAR3_POINT_RAY_SHADOW_THICKNESS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowThickness） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 142 | `DXVK_WAR3_POINT_SHADOW` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetPointLightShadowIntensity），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 143 | `DXVK_WAR3_POINT_SHADOW_BIAS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetPointShadowBias），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 144 | `DXVK_WAR3_POINT_SHADOW_MAX_FACES` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowMaxFacesPerFrame） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 145 | `DXVK_WAR3_POINT_SHADOW_MAX_LIGHTS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowMaxLights） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 146 | `DXVK_WAR3_POINT_SHADOW_PCF_FAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowPcfRadiusFar） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 147 | `DXVK_WAR3_POINT_SHADOW_PCF_NEAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowPcfRadiusNear） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 148 | `DXVK_WAR3_POINT_SHADOW_RANGE_FADE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowRangeFadeStart） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 149 | `DXVK_WAR3_POINT_SHADOW_RESOLUTION` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowResolution） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 150 | `DXVK_WAR3_POINT_SHADOW_TEXEL_BIAS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowTexelBiasScale） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 151 | `DXVK_WAR3_POINT_RAY_SHADOW_HIZ_VISITS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointRayShadowHiZMaxVisits） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 152 | `DXVK_WAR3_POINT_SHADOW_DEBUG_DISTANCE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 153 | `DXVK_WAR3_POINT_SHADOW_DEBUG_LIGHT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取）；static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.pointShadowDebugLightIndex） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 154 | `DXVK_WAR3_POINT_SHADOW_PERSISTENT_PREPARE_MODE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_POPULATE_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 155 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VIEW` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 156 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 157 | `DXVK_WAR3_POPULATE_SUBMIT_PERMUTATION_VERIFY_ASSERT` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_PREPARED_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 158 | `DXVK_WAR3_PREPARED_SLICE_PROBE` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 159 | `DXVK_WAR3_PREPARED_SLICE_PROBE_SAMPLE_RATE` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_PRESERVE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 160 | `DXVK_WAR3_PRESERVE_TRUSTED_PALETTE_ON_RAW_MISS` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_PROFILE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 161 | `DXVK_WAR3_PROFILE` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_PUBLISH_*` 组（5 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 162 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 163 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_SAMPLE_PERIOD` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 164 | `DXVK_WAR3_PUBLISH_PROBE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 165 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 166 | `DXVK_WAR3_PUBLISH_VISIBLE_SAFE_COPY_VERIFY_ASSERT` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |

**`DXVK_WAR3_REGISTRY_*` 组（2 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 167 | `DXVK_WAR3_REGISTRY_HEALTH_VERIFY` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 168 | `DXVK_WAR3_REGISTRY_HEALTH_VERIFY_ASSERT` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_RENDER_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 169 | `DXVK_WAR3_RENDER_LOG` | 调试/日志 (d3d9_war3_debug) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_RENDERABLE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 170 | `DXVK_WAR3_RENDERABLE_PART_PALETTE_SNAPSHOT` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |

**`DXVK_WAR3_RENDERER_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 171 | `DXVK_WAR3_RENDERER_CORE_TIMING_PROBE` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_RESOURCE_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 172 | `DXVK_WAR3_RESOURCE_CENSUS` | War3 诊断工具 (war3/tools) | 首次调用读取（Enabled() 内 static 缓存） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_RTS_*` 组（5 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 173 | `DXVK_WAR3_RTS_SHADOW_BASE_WORLD_TEXEL` | 阴影运行时 (d3d9_war3_shadow) | 运行期每次调用读取（阴影路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 174 | `DXVK_WAR3_RTS_SHADOW_CANDIDATE_MODE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 175 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_BAND_HALF_HEIGHT` | 阴影运行时 (d3d9_war3_shadow) | 运行期每次调用读取（阴影路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 176 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_PADDING` | 阴影运行时 (d3d9_war3_shadow) | 运行期每次调用读取（阴影路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 177 | `DXVK_WAR3_RTS_SHADOW_RECEIVER_PLANE_HEIGHT` | 阴影运行时 (d3d9_war3_shadow) | 运行期每次调用读取（阴影路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_RUNTIME_*` 组（4 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 178 | `DXVK_WAR3_RUNTIME_MATRIX_BATCH_CAPTURE` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 179 | `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 180 | `DXVK_WAR3_RUNTIME_MATRIX_RANGE_COPY_PUBLISH_DEDUP` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 181 | `DXVK_WAR3_RUNTIME_TREE_MAX` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_S1_*` 组（7 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 182 | `DXVK_WAR3_S1_PERSISTENT_BORROW_STATIC` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 183 | `DXVK_WAR3_S1_TERRAIN_CAPTURE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 184 | `DXVK_WAR3_S1_TERRAIN_PERSISTENT_GEOMETRY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 185 | `DXVK_WAR3_S1_FORCE_IDENTITY_WORLD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 186 | `DXVK_WAR3_S1_EARLY_FALLBACK_BACKING` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 187 | `DXVK_WAR3_S1_PERSISTENT_UNSTABLE_SOURCE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 188 | `DXVK_WAR3_S1_TERRAIN_CASTER_MASK` | 阴影运行时 (d3d9_war3_shadow) | 运行期每次调用读取（阴影路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_SEMANTIC_*` 组（96 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 189 | `DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_TRACE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 190 | `DXVK_WAR3_SEMANTIC_CONTRACT_CAPTURE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 191 | `DXVK_WAR3_SEMANTIC_COVERAGE_DROP_MAX_HOLD_FRAMES` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 192 | `DXVK_WAR3_SEMANTIC_CURRENT_DRAW_GRACE_FRAMES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 193 | `DXVK_WAR3_SEMANTIC_DIRECT_ONLY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 194 | `DXVK_WAR3_SEMANTIC_DIRECT_PART_PACKET_LEASE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 195 | `DXVK_WAR3_SEMANTIC_DIRECT_RECORD_CAP` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 196 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_DIRECT_PRODUCER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 197 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_REGION_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 198 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 199 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_TRACE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 200 | `DXVK_WAR3_SEMANTIC_GATE_VALUE_CACHE` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 201 | `DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 202 | `DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_ON_COVERAGE_DROP` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 203 | `DXVK_WAR3_SEMANTIC_HOLD_SHADOWMAP_UNTIL_STABLE_IDENTITY` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 204 | `DXVK_WAR3_SEMANTIC_IDENTITY_STABLE_FRAMES_BEFORE_REDRAW` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 205 | `DXVK_WAR3_SEMANTIC_KEEP_READY_ON_INVALID_CURRENT_DRAW` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 206 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_REFRESH` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 207 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_SAFE_COPY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 208 | `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_EPOCH_PLANNER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 209 | `DXVK_WAR3_SEMANTIC_MANIFEST_DEFER_PROVISIONAL_PARTS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 210 | `DXVK_WAR3_SEMANTIC_MANIFEST_GEOMETRY_CACHE_FRAMES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 211 | `DXVK_WAR3_SEMANTIC_MANIFEST_LEASE_PALETTE_REFRESH` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 212 | `DXVK_WAR3_SEMANTIC_MATERIAL_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 213 | `DXVK_WAR3_SEMANTIC_OBJECT_GROUPED_SELECTION` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 214 | `DXVK_WAR3_SEMANTIC_PALETTE_ATTRIBUTION_SNAPSHOT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 215 | `DXVK_WAR3_SEMANTIC_PALETTE_IN_PLACE_APPEND` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 216 | `DXVK_WAR3_SEMANTIC_POSE_ONLY_CAPTURE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 217 | `DXVK_WAR3_SEMANTIC_RECEIVER_FREEZE_LAST_GOOD_LIGHTING` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 218 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_STRENGTH_CLAMP` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 219 | `DXVK_WAR3_SEMANTIC_REJECT_ALPHA_BLEND_CASTER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 220 | `DXVK_WAR3_SEMANTIC_REQUIRE_AUTHORITATIVE_SKINNED` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 221 | `DXVK_WAR3_SEMANTIC_REQUIRE_DIRECT_UNIT_VISIBLE_BACKING` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 222 | `DXVK_WAR3_SEMANTIC_REQUIRE_VISIBLE_INDEX_SLICE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 223 | `DXVK_WAR3_SEMANTIC_SHADOW_BOOTSTRAP_CATCHUP` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 224 | `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_FLUSH` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 225 | `DXVK_WAR3_SEMANTIC_SHADOW_FORCE_BEFOREUI` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 每帧 OnFrameStart 读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 226 | `DXVK_WAR3_SEMANTIC_SHADOW_PRE_READY` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 227 | `DXVK_WAR3_SEMANTIC_SHADOW_PREVIEW` | War3 运行时核心 (war3/core) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 228 | `DXVK_WAR3_SEMANTIC_SHADOW_SCENE_SUBMISSION` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 229 | `DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 230 | `DXVK_WAR3_SEMANTIC_STICKY_PART_SELECTION_MIN_RECORDS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 231 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 232 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_FILL_MARGIN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 233 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 234 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_LEASE_FRAMES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 235 | `DXVK_WAR3_SEMANTIC_VALIDATE_UNIT_CORE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 236 | `DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE_STATS` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 237 | `DXVK_WAR3_SEMANTIC_BUILD_ELIGIBLE_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 238 | `DXVK_WAR3_SEMANTIC_DIRECT_OWNER_SCAN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 239 | `DXVK_WAR3_SEMANTIC_DIRECT_PHASE_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 240 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_POSE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 241 | `DXVK_WAR3_SEMANTIC_DYNAMIC_EVIDENCE_STATS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 242 | `DXVK_WAR3_SEMANTIC_FAST_APPEND_STATS_REUSE_VERIFY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 243 | `DXVK_WAR3_SEMANTIC_GENERIC_APPEND_STATS_REUSE_VERIFY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 244 | `DXVK_WAR3_SEMANTIC_MANIFEST_CORE_STALE_POSE_ONE_FRAME_RESTORE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 245 | `DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 246 | `DXVK_WAR3_SEMANTIC_MANIFEST_POSE_GENERATION_VERIFY_ASSERT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 247 | `DXVK_WAR3_SEMANTIC_MANIFEST_SIBLING_POSE_PROPAGATION` | War3 渲染层 (war3/render) | 运行期每次调用读取（逐 record 路径，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 248 | `DXVK_WAR3_SEMANTIC_OBJECT_FIRST_SNAPSHOT` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 249 | `DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 250 | `DXVK_WAR3_SEMANTIC_PERF_TRACK` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 251 | `DXVK_WAR3_SEMANTIC_SHADOW_MANIFEST_CMODEL_POSE_DIAG` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 252 | `DXVK_WAR3_SEMANTIC_SHADOW_TRACE` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 253 | `DXVK_WAR3_SEMANTIC_SLICE_TRACE` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 254 | `DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 255 | `DXVK_WAR3_SEMANTIC_COMPACT_WORK_TABLE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 256 | `DXVK_WAR3_SEMANTIC_PRODUCER_CLAIM_LEDGER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 257 | `DXVK_WAR3_SEMANTIC_ALLOW_SINGLE_PRIM_FULL_INDEX` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 258 | `DXVK_WAR3_SEMANTIC_AUGMENT_BATCH_LOOKUP` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 259 | `DXVK_WAR3_SEMANTIC_AUGMENT_COMPACT_SHADOW_VIEW` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 260 | `DXVK_WAR3_SEMANTIC_AUGMENT_TLS_CACHE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 261 | `DXVK_WAR3_SEMANTIC_BOOTSTRAP_SUPPLEMENTED_BUILD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 262 | `DXVK_WAR3_SEMANTIC_BYPASS_INLINE_REGISTRY_PUBLISH` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 263 | `DXVK_WAR3_SEMANTIC_COVERAGE_DROP_TOLERANCE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 264 | `DXVK_WAR3_SEMANTIC_DIRECT_EXPLICIT_BLEND_RESOLVE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 265 | `DXVK_WAR3_SEMANTIC_DIRECT_SCAN_CAP` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 266 | `DXVK_WAR3_SEMANTIC_DIRECT_STATIC_SUPPLEMENT` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 267 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_FAST_APPEND` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 268 | `DXVK_WAR3_SEMANTIC_DRAW_TIME_PREBUILD_BYPASS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |
| 269 | `DXVK_WAR3_SEMANTIC_LIVE_PALETTE_ALLOW_CMODEL_FALLBACK` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 270 | `DXVK_WAR3_SEMANTIC_MANIFEST_CMODEL_POSE_RESTORE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 271 | `DXVK_WAR3_SEMANTIC_PUBLISH_REGISTRIES_BEFORE_SCENE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 272 | `DXVK_WAR3_SEMANTIC_RECEIVER_DISABLE_POINT_LIGHTS` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 273 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_MODE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 274 | `DXVK_WAR3_SEMANTIC_RECEIVER_STABILITY_PCF_RADIUS` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 275 | `DXVK_WAR3_SEMANTIC_REJECT_UNSAFE_ALPHA_CASTER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 276 | `DXVK_WAR3_SEMANTIC_SHADOW_BUILD_RECORD_CAP` | War3 阴影契约层 (war3/shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 277 | `DXVK_WAR3_SEMANTIC_SHADOW_BYPASS_LEGACY_UNIT_CAPTURE` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 278 | `DXVK_WAR3_SEMANTIC_SHADOW_DISABLE_LEGACY_CAPTURE` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 279 | `DXVK_WAR3_SEMANTIC_SHADOW_ENDFRAME_BUILD` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 280 | `DXVK_WAR3_SEMANTIC_SHADOW_TAIL_FALLBACK` | War3 运行时核心 (war3/core) | 运行期每次调用读取（每帧/每事件语义门控，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 281 | `DXVK_WAR3_SEMANTIC_SHADOW_WEAK_BEFOREUI_COMMIT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 282 | `DXVK_WAR3_SEMANTIC_STEADY_SUPPLEMENTED_BUILD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 283 | `DXVK_WAR3_SEMANTIC_STICKY_SELECTION_BROAD_LEASE_PREFERENCE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 284 | `DXVK_WAR3_SEMANTIC_SUBMIT_DRAW_CAP` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_SHADOW_*` 组（59 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 285 | `DXVK_WAR3_SHADOW_ALPHA_HASH` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowAlphaHashed），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 286 | `DXVK_WAR3_SHADOW_ALPHA_MIP` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowAlphaUseMip），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 287 | `DXVK_WAR3_SHADOW_ALPHA_MIP_BIAS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowAlphaMipLodBias），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 288 | `DXVK_WAR3_SHADOW_ARENA_CAPTURE` | War3 内存/Arena (war3/memory) | Arena 初始化期一次（ShadowArena_Init）；static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 289 | `DXVK_WAR3_SHADOW_BIAS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowBias），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 290 | `DXVK_WAR3_SHADOW_CAPTURE_TRACE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 291 | `DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.cascadeBlendRange） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 292 | `DXVK_WAR3_SHADOW_CASCADES` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.csm.cascadeCount） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 293 | `DXVK_WAR3_SHADOW_FREEZE_DYNAMIC` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 294 | `DXVK_WAR3_SHADOW_GATE_TRACE_PERIOD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 295 | `DXVK_WAR3_SHADOW_METADATA_ALPHA` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 296 | `DXVK_WAR3_SHADOW_METADATA_BLOCKER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 297 | `DXVK_WAR3_SHADOW_METADATA_CAPTURE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 298 | `DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowNormalBiasScale），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 299 | `DXVK_WAR3_SHADOW_PCF` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowPcfRadius），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 300 | `DXVK_WAR3_SHADOW_PCF_KERNEL` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowPcfKernel），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 301 | `DXVK_WAR3_SHADOW_PERSISTENT_MAX_AGE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 302 | `DXVK_WAR3_SHADOW_PERSISTENT_MB` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 303 | `DXVK_WAR3_SHADOW_RECEIVER_MODE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowReceiverMode），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 304 | `DXVK_WAR3_SHADOW_RES` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.csm.shadowResolution） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 305 | `DXVK_WAR3_SHADOW_SPLIT_LAMBDA` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.csm.splitLambda） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 306 | `DXVK_WAR3_SHADOW_STABLE_SNAP` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.csm.stableSnap） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 307 | `DXVK_WAR3_SHADOW_STRENGTH` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetShadowStrength），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 308 | `DXVK_WAR3_SHADOW_TAA` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.shadowTaaEnabled） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 309 | `DXVK_WAR3_SHADOW_TAA_NEW_FRAME_WEIGHT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.shadowTaaBlendFactor） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 310 | `DXVK_WAR3_SHADOW_APPEND_SURVEY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 311 | `DXVK_WAR3_SHADOW_CAPTURE_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 312 | `DXVK_WAR3_SHADOW_DEBUG` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取）；static 首次调用缓存（进程内一次） | 是 | JAPI 可写同字段（SetShadowDebugMode），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 313 | `DXVK_WAR3_SHADOW_DEBUG_CASTER_STAGE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 314 | `DXVK_WAR3_SHADOW_DRAW_SURVEY` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 315 | `DXVK_WAR3_SHADOW_DRAWTIME_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 316 | `DXVK_WAR3_SHADOW_EXACT_INDEX_TRIM` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |
| 317 | `DXVK_WAR3_SHADOW_FAR_CASTER_DEPTH_EXTENSION` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 318 | `DXVK_WAR3_SHADOW_GATE_BREAKDOWN` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 319 | `DXVK_WAR3_SHADOW_PASS_TRACE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 320 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 321 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTER_SAMPLE_BYTES` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 322 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CASTERS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 323 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_CONTRACTS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 324 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 325 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CASTERS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 326 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_CONTRACTS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 327 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_OBJECTS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 328 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_POSES` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 329 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 330 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_OBJECTS` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 331 | `DXVK_WAR3_SHADOW_POSE_FULL_TRACE_POSES` | War3 渲染层 (war3/render) | 首次使用锁内初始化一次（InitializeShadowPoseFullTraceEnvLocked） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 332 | `DXVK_WAR3_SHADOW_STAGE_HISTOGRAM` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 333 | `DXVK_WAR3_SHADOW_ADAPTIVE_MAP_UPDATE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 334 | `DXVK_WAR3_SHADOW_ARENA_FRAMES` | War3 内存/Arena (war3/memory) | Arena 初始化期一次（ShadowArena_Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 335 | `DXVK_WAR3_SHADOW_ARENA_MAX_MB` | War3 内存/Arena (war3/memory) | Arena 初始化期一次（ShadowArena_Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 336 | `DXVK_WAR3_SHADOW_ARENA_MB` | War3 内存/Arena (war3/memory) | Arena 初始化期一次（ShadowArena_Init） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 337 | `DXVK_WAR3_SHADOW_DISABLE_TAA_FOR_SEMANTIC_DYNAMIC` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 338 | `DXVK_WAR3_SHADOW_FALLBACK_BUDGET_MB` | D3D9 设备/主运行时 (d3d9_device) | 设备初始化期一次（回退预算计算） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 339 | `DXVK_WAR3_SHADOW_STAGE_LIFECYCLE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 340 | `DXVK_WAR3_SHADOW_TAA_DISABLE_ON_SUN_MOTION` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 341 | `DXVK_WAR3_SHADOW_TAA_MODE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 shadows.shadowTaaEnabled） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 342 | `DXVK_WAR3_SHADOW_TRIM_INDEXED` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |
| 343 | `DXVK_WAR3_SHADOW_WORLD_UP` | CSM 级联 (d3d9_war3_csm) | 运行不可达（守卫 s_forcedUp==-2 恒不成立） | 待判定 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（编译进 Release，但运行守卫恒不成立） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（当前源码不可达，读取结果不被消费） |

**`DXVK_WAR3_SHADOWMAP_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 344 | `DXVK_WAR3_SHADOWMAP_REPLAY_LOG` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_SKIN_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 345 | `DXVK_WAR3_SKIN_PALETTE_CONTRACT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_SPRITE_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 346 | `DXVK_WAR3_SPRITE_HOST_BIND_DISABLE` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 347 | `DXVK_WAR3_SPRITE_HOST_BIND_OPENING_SKIP` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 348 | `DXVK_WAR3_SPRITE_UBER_DT_PROBE` | War3 模型/姿态桥 (war3/model) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_STAGE11_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 349 | `DXVK_WAR3_STAGE11_ALLOC_OBSERVER` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 350 | `DXVK_WAR3_STAGE11_DIRECT_STATIC_SOURCE_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |
| 351 | `DXVK_WAR3_STAGE11_DIRECT_UPLOAD_SOURCE_MODE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_STAGE13_*` 组（10 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 352 | `DXVK_WAR3_STAGE13_LATE_FULL_INDEX_FINGERPRINT` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 353 | `DXVK_WAR3_STAGE13_LATE_SAMPLE_COUNT` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 354 | `DXVK_WAR3_STAGE13_STATIC_RETENTION_CAP` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 355 | `DXVK_WAR3_STAGE13_STATIC_RETENTION_FRAMES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 356 | `DXVK_WAR3_STAGE13_COMPONENT_DIAGNOSTICS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 357 | `DXVK_WAR3_STAGE13_LATE_DESCRIPTOR_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |
| 358 | `DXVK_WAR3_STAGE13_SORT_UNIQUE_READS` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 359 | `DXVK_WAR3_STAGE13_SOURCE_GENERATION_VERIFY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 360 | `DXVK_WAR3_STAGE13_UNIQUE_SEMANTIC_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |
| 361 | `DXVK_WAR3_STAGE13_STATIC_RETENTION` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 高（注释明示为不安全/未证明路径，启用会绕过生产正确性验证或触发已知风险） |

**`DXVK_WAR3_STORM_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 362 | `DXVK_WAR3_STORM_THRESHOLD_KB` | War3 内存/Arena (war3/memory) | Storm hook 安装期一次（StormHook_Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 363 | `DXVK_WAR3_STORM_NATIVE_SMALL_REPAIR` | War3 内存/Arena (war3/memory) | 运行期按修复请求读取（即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 364 | `DXVK_WAR3_STORM_TAKEOVER_MODE` | War3 内存/Arena (war3/memory) | Storm hook 安装期一次（StormHook_Install） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_STREAM1_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 365 | `DXVK_WAR3_STREAM1_LAYOUT_PROBE` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_SUBMIT_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 366 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_LAG_THRESHOLD` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 中（默认关的对照/A-B/回退或验证路径；启用后替换既有路径，契约 fail-closed） |
| 367 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_EVERY_FRAME` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |
| 368 | `DXVK_WAR3_SUBMIT_LIVE_POSE_REBUILD_ON_LAG` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_TEST_*` 组（15 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 369 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_PRIMS` | D3D9 设备/主运行时 (d3d9_device) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 370 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_START` | D3D9 设备/主运行时 (d3d9_device) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 371 | `DXVK_WAR3_TEST_INDEX_OVERFLOW_VERTS` | D3D9 设备/主运行时 (d3d9_device) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 372 | `DXVK_WAR3_TEST_INDEX_OVERFLOW` | D3D9 设备/主运行时 (d3d9_device) | lazy static 门首次调用（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 373 | `DXVK_WAR3_TEST_POINT_LIGHT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 374 | `DXVK_WAR3_TEST_POINT_LIGHT_B` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 375 | `DXVK_WAR3_TEST_POINT_LIGHT_CLEAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 376 | `DXVK_WAR3_TEST_POINT_LIGHT_G` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 377 | `DXVK_WAR3_TEST_POINT_LIGHT_INTENSITY` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 378 | `DXVK_WAR3_TEST_POINT_LIGHT_R` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 379 | `DXVK_WAR3_TEST_POINT_LIGHT_RANGE` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 380 | `DXVK_WAR3_TEST_POINT_LIGHT_SHADOW` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 381 | `DXVK_WAR3_TEST_POINT_LIGHT_X` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 382 | `DXVK_WAR3_TEST_POINT_LIGHT_Y` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 383 | `DXVK_WAR3_TEST_POINT_LIGHT_Z` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_UNION_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 384 | `DXVK_WAR3_UNION_CONSUMER_CULL_MODE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 否（编译期 DEV/if constexpr 门内） | dev-only／Release 不可达 | 否（Release 不可达） | 否 | 低（Release 构建不可达，不改变产品行为） |

**`DXVK_WAR3_USER_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 385 | `DXVK_WAR3_USER_EXAMPLE` | 其他 (src/d3d9/war3) | 运行期每次调用读取（示例入口，即时） | 否 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_VB_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 386 | `DXVK_WAR3_VB_ALLOC_SPIKE_LOG` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_VISIBLE_*` 组（3 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 387 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 388 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX_VERIFY_ASSERT` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |
| 389 | `DXVK_WAR3_VISIBLE_SEMANTIC_MERGE_INDEX` | War3 渲染层 (war3/render) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 低（默认关的诊断/观察输出，未启用时不改变默认行为） |

**`DXVK_WAR3_VISUAL_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 390 | `DXVK_WAR3_VISUAL_API_DIAGNOSTICS` | 体积光 (d3d9_war3_volumetric_light) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 内部诊断/取证（默认关，env 属内部面） | 否（默认关） | 否 | 低（默认关的诊断/取证输出，未启用时不改变默认行为） |

**`DXVK_WAR3_VOLUMETRIC_*` 组（17 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 391 | `DXVK_WAR3_VOLUMETRIC_BACKEND` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricBackend），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 392 | `DXVK_WAR3_VOLUMETRIC_DECAY` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 393 | `DXVK_WAR3_VOLUMETRIC_DENSITY` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 394 | `DXVK_WAR3_VOLUMETRIC_EXTINCTION` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 postFx.volumetricLight.extinctionStrength） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 395 | `DXVK_WAR3_VOLUMETRIC_FADE_FAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightFade），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 396 | `DXVK_WAR3_VOLUMETRIC_FADE_NEAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightFade），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 397 | `DXVK_WAR3_VOLUMETRIC_FROXEL_FAR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 postFx.volumetricLight.froxelFar） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 398 | `DXVK_WAR3_VOLUMETRIC_HEIGHT_FOG` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricHeightFog），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 399 | `DXVK_WAR3_VOLUMETRIC_INTENSITY` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 400 | `DXVK_WAR3_VOLUMETRIC_LIGHT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightEnabled），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 401 | `DXVK_WAR3_VOLUMETRIC_MAX_RAY` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightFade），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 402 | `DXVK_WAR3_VOLUMETRIC_POINT_MAX_LIGHTS` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 postFx.volumetricLight.maxPointLights） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 403 | `DXVK_WAR3_VOLUMETRIC_RES_DIVISOR` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricResolutionDivisor），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 404 | `DXVK_WAR3_VOLUMETRIC_SAMPLES` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 405 | `DXVK_WAR3_VOLUMETRIC_SKY_THRESHOLD` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 postFx.volumetricLight.skyThreshold） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 406 | `DXVK_WAR3_VOLUMETRIC_UNSHADOWED` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | 面板/JAPI 无同字段 setter；env 仅构造期生效（字段 postFx.volumetricLight.unshadowedScattering） | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |
| 407 | `DXVK_WAR3_VOLUMETRIC_WEIGHT` | War3 渲染管线/效果设置 (d3d9_war3_pipeline) | 渲染管线构造期一次（ParseEnv* 每次调用读取） | 是 | JAPI 可写同字段（SetVolumetricLightParams），运行期后写覆盖 env；env 仅构造期生效 | 是（Release 编译并可达） | 生产可调覆盖（env 属内部面） | 否（仅覆盖内置默认） | 否 | 中（仅覆盖内置生产默认；未设置时行为不变） |

**`DXVK_WAR3_WIDGET_*` 组（4 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 408 | `DXVK_WAR3_WIDGET_NEGATIVE_CACHE_TTL_FRAMES` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 409 | `DXVK_WAR3_WIDGET_NEGATIVE_FRAME_CACHE` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 410 | `DXVK_WAR3_WIDGET_PROBE_SAFE_COPY` | D3D9 设备/主运行时 (d3d9_device) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 默认开启·影响产品（env 属内部面） | 是 | 否 | 高（默认开启，直接改变产品渲染/正确性路径；误设 0 会回退生产行为） |
| 411 | `DXVK_WAR3_WIDGET_IDENTITY_HOOK` | Game.dll 原生 Hook (war3/hooks) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

**`DXVK_WAR3_WORKER_*` 组（1 项）**

| # | 开关名 | 所有者 | 读取时机 | 重启需求 | 面板/JAPI 优先级 | 发布可达 | Q4 归属·支持范围 | 默认开启/影响产品 | 公开玩家接口 | 诊断风险 |
| ---: | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 412 | `DXVK_WAR3_WORKER_PREPARE` | 阴影运行时 (d3d9_war3_shadow) | static 首次调用缓存（进程内一次） | 是 | 面板/JAPI 无该开关入口（env 为内部唯一入口） | 是（Release 编译并可达） | 待判定（角色证据不足，env 属内部面） | 待判定 | 否 | 待判定（缺该开关运行期影响面的权威证据：设计文档、提交记录或编译期门归属） |

### 10.5 待判定维度与已知异常（禁止猜测项）

- **读取时机／重启需求 待判定：1 项。** `DXVK_WAR3_SHADOW_WORLD_UP` 的读取点被 `if (s_forcedUp == -2)` 包裹，而 `src/d3d9/d3d9_war3_csm.cpp` 内 `s_forcedUp` 初值为 `1` 且无任何赋值 `-2` 的路径（源码注释自述 was -2），读取结果不被消费，故两个维度均记待判定；其余 411 项读取模式（static 缓存／lazy static 门／每次调用）均可从读取点判定。
- **发布可达 待判定：0 项。** 19 项 dev-only 读点位于 `#if` DEV 门或同函数 `if constexpr` 开发策略门内（与第 5 章一致）；另有 1 项特殊：`DXVK_WAR3_SHADOW_WORLD_UP` 编译进 Release，但运行期守卫恒不成立。
- **诊断风险 待判定：70 项，全部落在第 6 章待判定条目。** 这些条目既无默认值角色证据，也无运行期影响面的权威证据（缺设计文档、提交记录或编译期门归属），本章按裁定不作推测。
- **与面板/JAPI 优先级 待判定：0 项。** 第 3–6 章给出读取点的行按「读取点字段 → `war3shader::Set*`」映射；无该字段的行按「无该开关入口」登记。
- **已知异常（订正记录，不改变 412 行定义域）：** `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_`（第 6 章 #29）是 C++ 相邻字符串字面量拼接产生的截断名。真实运行时名在 `src/d3d9/d3d9_device.cpp:2455-2459` 为 `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_` 与 `EQUIVALENCE_MODE` 拼接成的 `DXVK_WAR3_PERSISTENT_GPU_PACKAGE_CURRENT_DRAW_EQUIVALENCE_MODE`；该截断名在运行时并不存在。为与第 3–6 章保持逐行一致，本章仍按原名登记，读取模式按真实读取点的 `static const auto s_mode = []{…}()` 判为 static 首次调用缓存。
- **多读取点合并规则：** 同一开关存在多个读取点且模式不一致时，「读取时机」列以「；」并列全部模式；「重启需求」取并集——只要存在进程内一次性（static／构造期／安装期）读取点即记「是」。

### 10.6 关闭项（本工作流未完成边界）（Q4(b)）

- 本工作流在此关闭，**未完成边界只剩两条**：① **85 项待判定继续保持待判定**，不强行归入 A/B/C；② **不实施任何删除、默认值变更或玩家文档改动**。
- 其余维度（所有者／读取时机／重启需求／优先级／发布可达／支持范围／诊断风险）已在 §10.1–§10.5 逐行登记并闭合；第 3–6 章的 **412 行**与各章计数（156／152／19／85 等）**不因本裁定而改写**，第 10 章也不回写第 1–9 章的既有分类与默认值（Q4(b)）。

### 11. 2026-09-19 取证 profile 补录（不重算旧 412 行）

以下为后续已有生产 knob 的补录，不是本次新增环境变量，也不增加玩家面板/JAPI 入口。仍属于内部诊断面。

| 内部环境变量 | 默认/合法覆盖 | 所有者与生效边界 |
| --- | --- | --- |
| `DXVK_WAR3_FRAME_EVIDENCE_PRE_FRAMES` | 内部 profile 96、外部 256；仅接受 4..所选默认值的纯十进制整数 | 统一 `WithEnvOverrides`；CPU/input 准备与图像 ring 准备各自按调用时读取；诊断比较固定进程启动环境，不承诺运行中热改 |
| `DXVK_WAR3_FRAME_EVIDENCE_POST_FRAMES` | 两档均 4；仅接受 1..4 | 同上；不更改 CPU/input 容量、主开关或 preMilliseconds |

无值、非法/带尾随字符/溢出/超过所选值均保留所选 profile 默认。降低帧槽可能使前窗不足一秒，
仍须按实际 manifest 标明，不得据此宣称完全消除地址空间或显存成本。
源码与测试见 `war3_frame_recorder_profile.h` / `test_frame_recorder_defaults.cpp`，
本次验证边界见[首批 checkpoint](../agent-history/2026-09-19-memory-recovery-p0-p1a-checkpoint.md)。
