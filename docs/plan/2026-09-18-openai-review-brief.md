# WarVK 源码现状与「相较上一次重构的进展」审核包

> 用途：提交给外部研究端（OpenAI Research）审核本项目的架构演进与代码质量。
> 生成：2026-09-18 ｜ 源码树：`dxvk-v1.22-integration-20260914`
> 配套文件：`2026-09-18-openai-review-source-inventory.md`（src/ 全量 810 文件逐文件行数清单）

---

## 0. Abstract (EN)

WarVK is a DXVK-derived 32-bit `d3d9.dll` that renders **Warcraft III 1.27a** through Vulkan and augments it with shadow mapping, volumetric lighting, post-processing, runtime diagnostics and a map-author JASS API. The project deliberately does **not** change map logic or gameplay.

This package documents the **current, uncommitted state** of the v1.22 integration line and its delta relative to the last documented architecture-review checkpoint (2026-09-16). Headline facts:

- `src/` = **810 files / 457,374 lines**; the War3-specific subtree `src/d3d9/war3/` is the dominant growth area.
- **594 uncommitted working-tree entries**: 103 modified (tracked) + 491 new (untracked), 0 deleted. Tracked diff = **+4,792 / −3,452 lines across 103 files**.
- A **new subsystem cluster** has appeared since the baseline: frame evidence / frame history / frame inputs / frame timeline / recorder (memory, profile, session, control), async screenshot, data-collection tree, object-level palette-slot evidence, shadow-build lifecycle & thread gate, native light bridge, native capture & frame-sync ownership, runtime group palette kernel.
- One **correctness-critical finding** is unresolved and is the main reason for requesting review: the object-level palette evidence chain is **structurally blind to healthy paths** — see §6.

The reviewer is invited to focus on §5 (architecture invariants), §6 (known gaps and the unresolved design issue) and §8 (specific questions).

---

## 1. 项目与问题域

**目标**：把 Warcraft III 1.27a 的 D3D9 渲染路径接到 Vulkan，并在此之上补阴影/体积光/后处理/运行时诊断/地图作者 API。

**关键约束（不可破坏）**：

1. GPU 资源的 reset、Arena 回收、receiver/skin 状态切换**必须**由渲染所有者在 `PresentEx` 安全点合并执行；地图退出或 JASS/Hook 线程只能提请求与清 CPU 侧状态。
2. 阴影的生产/接收/replay 都必须验证 map/device epoch、资源身份、范围、索引/顶点来源与数据有限性；**任何证明失败 fail-closed**（不发布 candidate，必要时显示无阴影，绝不跨地图复用）。
3. Arena、冻结几何、persistent package、GPU-skin 的 fence/所有权**不得互相冒充**。
4. WarVK JAPI 是有界外部输入面（wire 长度、参数数、句柄、有限性、溢出、生命周期检查）。
5. 改动图形算法时，若实现者不能从现有合同完整证明公式与 Vulkan 行为，**必须先查一手资料**（论文 / MSDN / Khronos / 厂商研究）并在 `docs/research/` 记录公式、适用条件、来源与到 WarVK 的映射。
6. `AutoTest` 的 isolated-desktop 数据**不得**宣称为玩家前台性能。

---

## 2. 源码树与提交状态（实测事实）

| 项 | 值 |
| --- | --- |
| 主审核树 | `Core/Base/Graphics/dxvk-v1.22-integration-20260914` |
| 分支 | `codex/v1.22-release-integration-20260914` |
| HEAD | `ae89054`（2026-08-14，`release: finalize 1.21.00 publication text`） |
| `src/` 规模 | **810 文件 / 457,374 行**（55 个目录） |
| 工作树未提交条目 | **594**（103 modified + 491 untracked + 0 deleted） |
| 已跟踪文件 diff | **103 文件，+4,792 / −3,452 行** |
| 变更分布（前四） | `src/d3d9` 132、`docs/plan` 95、`docs/research` 31、`docs/agent-history` 23 |
| 新增源码文件 | `src/d3d9/war3/` 下 **63 个**（其中 `tools/` 31、`render/` 17） |

**注意：本树所有工作均未提交**，HEAD 早于全部变更。因此本次"进展"描述基于：文件清单 + 变更状态 + 模块结构 + 文档记录，**而非** commit diff。**不存在**可用的"基线 commit → 当前 commit"的差异数字。

同目录另有一棵姊妹树（不在本审核包内，仅供理解上下文）：

| 树 | 分支 | HEAD | 未提交 | src 文件数 |
| --- | --- | --- | --- | --- |
| `dxvk` | `codex/native-shadow-stable-baseline-20260830` | `88089cd`（2026-08-30） | 170 | 697 |
| `dxvk-v1.22-integration-20260914` | `codex/v1.22-release-integration-20260914` | `ae89054`（2026-08-14） | **594** | **810** |

→ 两棵树并行演进、各自带未提交工作，这本身是可维护性风险（见 §7）。

---

## 3. 模块规模（实测，按行数）

`src/d3d9/war3/` 子树（含 tests）：

| 模块 | 行数 | 职责 |
| --- | --- | --- |
| `render/` | 55,817 | 阴影 Map/receiver、最终 replay 验证、渲染阶段与资源发布 |
| `gpu_skin/` | 42,922 | GPU 蒙皮、Arena/静态包、模型与资源生命周期 |
| `tools/` | 34,599 | 运行时取证、性能监控、控制面、帧记录器（**本阶段增长最快**） |
| `hooks/` | 21,949 | Game.dll/JASS Hook、逻辑层到渲染层桥 |
| `model/` | 19,629 | 模型注册表、资源缓存、原生光照桥 |
| `shadow/` | 18,496 | 阴影渲染核心、运行时契约、构建生命周期 |
| `semantic/` | 9,252 | 语义场景/调色板 |
| `memory/` | 5,945 | 分配器与内存合同 |
| `native/` | 5,532 | 原生接口层 |
| `core/` | 5,286 | 内部测试配置与核心契约 |
| `japi/` | 2,821 | WarVK JAPI v1 |

**最大的单个文件**：

| 行数 | 文件 |
| --- | --- |
| **50,106** | `src/d3d9/d3d9_device.cpp` |
| 14,397 | `war3/gpu_skin/war3_gpu_skin_native_bridge.cpp` |
| 12,500 | `war3/tools/war3_perf_monitor.cpp` |
| 12,461 | `src/d3d9/d3d9_war3_shadow.cpp` |
| 11,434 | `war3/model/war3_model_hook.cpp` |
| 10,093 | `war3/render/war3_shadow_runtime_bridge.cpp` |
| 10,018 | `war3/shadow/war3_shadow_renderer_core.cpp` |

→ `d3d9_device.cpp`（5.0 万行）与多个 1 万行级文件是可维护性审核的重点对象。

---

## 4. 相较「上一次重构」的进展

### 4.1 基线定义

"上一次重构"= `docs/agent-history/2026-09-16-architecture-review-followup.md` 记录的架构复审后续整理 checkpoint。其自述成果（引自 `AGENTS.md` 当前状态节）：

- AutoTest 258 个 tracked 删除已恢复；静态全量 **88/88**、meson test **28/28**、DLL 构建与 no-work 通过；
- **palette slot 缓存身份证明复核已接入**（fail-visible 收紧，**实机未验证**）；
- **17 个重复 shader 函数**已抽入 `war3_shadow_common.glsl`；
- **caster GPU-skin 常量已单一来源化**（spirv-dis 证明行为不变）。

### 4.2 此后出现的**新子系统**（`src/d3d9/war3/` 下 63 个新文件）

| # | 子系统 | 代表文件 | 说明 |
| --- | --- | --- | --- |
| 1 | **帧取证 / 帧历史 / 帧输入 / 帧时间线** | `tools/war3_frame_evidence.{h,cpp}`、`frame_history*.h`、`frame_inputs.*`、`frame_timeline.*`、`frame_evidence_core.h` | 新增的完整取证子系统，含控制面与导出 |
| 2 | **帧记录器** | `tools/war3_frame_recorder_{config,control,memory,profile,session}.h`、`frame_recorder_memory.cpp`、`frame_recorder_build.h.in` | 内存预算、会话、配置、构建期版本注入 |
| 3 | **对象级调色板证据** | `tools/war3_palette_object_evidence.{h}`、`_sink.{h,cpp}`、`_capture.h` | 对象级 palette 链证据（wire 载体 + 解析器契约） |
| 4 | **阴影构建生命周期 / 进度 / 线程门 / 几何域** | `shadow/war3_shadow_build_{lifecycle,progress,thread_gate}.h`、`shadow_geometry_domain.h` | 把"构建"提升为一等状态机，带线程准入 |
| 5 | **原生光照桥** | `model/war3_native_light_bridge.{h,cpp}`、`_core.h`、`_policy.h` | 从原生运行时可取得的光照语义 |
| 6 | **原生捕获 / 帧同步所有权** | `hooks/war3_native_capture.{h,cpp}`、`native_frame_sync_{owner,policy}.h` | 原生截图/同步的 owner 与策略分离 |
| 7 | **运行时组调色板内核** | `render/war3_runtime_group_palette_kernel.h` | 组调色板求值内核 |
| 8 | **皮肤调色板选择** | `render/war3_skin_palette_selection.h` | palette slot 选择规则 |
| 9 | **太阳光策略** | `render/war3_sun_light_policy.h` | 方向光/admission 策略 |
| 10 | **路径阻断证据** | `war3_path_blocker_evidence.h` | 静态阴影 stamp 阻断证据 |

### 4.3 既有模块的改造（103 个 modified 文件中 `src/` 部分）

核心设备与生命周期：`d3d9_device.{cpp,h}`、`d3d9_swapchain.{cpp,h}`、`d3d9_window.{cpp,h}`、`d3d9_surface.cpp`、`d3d9_war3_pipeline.{cpp,h}`、`d3d9_war3_scene.h`、`d3d9_war3_shadow.{cpp,h}`、`d3d9_war3_hook.cpp`、`d3d9_war3_volumetric_light.{cpp,h}`、`d3d9_fixed_function.cpp`、`d3d9_state.h`、`meson.build`。

Shadow/渲染：`render/war3_shadow_object_registry.cpp`、`war3_shadow_runtime_bridge.{cpp,h}`、`war3_renderer.cpp`、`war3_scene_collector.cpp`、`war3_canonical_draw.{cpp,h}`、`war3_current_draw_contract.{cpp,h}`、`war3_render_exec_batch.cpp`、`war3_render_identity_bridge.cpp`、`war3_render_objects.cpp`、`war3_upper_layer_shadow.cpp`、`war3_visible_renderables.cpp`、`shadow/war3_shadow_native_runtime.cpp`。

Hooks/模型/JAPI/平台：`hooks/war3_hook_{lifecycle,render,shadow}.cpp`、`hooks/war3_jass_command_bridge.{cpp,h}`、`hooks/war3_shadow_filter_policy.cpp`、`model/war3_model_hook.{cpp,h}`、`model/war3_model_registry.cpp`、`model/war3_model_resource_cache.{cpp,h}`、`japi/war3_japi_v1.cpp`、`platform/war3_module_api.{cpp,h}`、`core/war3_internal_test_config.h`。

### 4.4 测试与文档体系的扩张

- `src/d3d9/war3/render/tests/` 新增 **13 个**测试文件：palette object evidence（**3 个**：evidence / cost / wire roundtrip）、palette slot recheck evidence、frame timeline、data collection tree、geoset identity view、runtime group palette kernel（**2 个**：kernel + kernel diff）、shadow build（**3 个**：lifecycle / progress / thread gate）、shadow geometry domain、sun light policy。
- 文档：`docs/plan` 新增 95、`docs/research` 新增 31、`docs/agent-history` 新增 23。近期 plan 覆盖：active-path palette instrumentation、candidate freeze、Gap B 确认强化与资源键修复、M1 迁移等价性、P0 对象级证据采集点核验、registry domain isolation、T2/T3 slot ownership、T3 thread-writer runtime proof、T7 registry stage13 survey、T8 bounded retry、relay M2 工单。

**趋势判断**：基线侧重"整理与去重"（shader 抽取、常量单一来源、删除恢复）；此后转向**新增可观测性与准入状态机**（取证、记录器、构建生命周期、线程门、原生桥），并配套大量合同测试。代码体量增长主要在 `tools/`（34.6k 行）与测试目录。

---

## 5. 关键架构不变式（供审核方评估是否成立）

1. **Fail-closed 证据链**：任何身份/范围/代际证明失败 ⇒ 整份 candidate 不发布。
2. **单一所有权**：Arena / 冻结几何 / persistent package / GPU-skin 的 fence 与所有权不可互冒；producer 完成 fence ≠ 消费者 last-use 权限。
3. **安全点合并执行**：GPU 资源重置只在 `PresentEx` 安全点，由渲染所有者执行。
4. **epoch 隔离**：地图/设备 epoch 贯穿资源与缓存；跨地图禁止复用旧资源与旧身份。
5. **有界外部输入**：JAPI 与 wire 载体（含 `words32` 位分配）受登记表约束，读方解析器与写方必须逐项一致。
6. **证据语义受裁定**：证据阶段/位分配**不得由实现者发明**（代码中多处注释引自"上级裁定"）。

---

## 6. 已知缺口与风险（诚实清单）

### 6.1 未解决的设计问题（**本次请求审核的首要原因**）

**对象级 palette 证据链对"健康路径"结构性失明。**

- 取证模型要求每个被观察对象**先经历一次拒绝**：条目仅由 `NoteReject → Insert` 创建；`NoteServed` / `NoteEnqueued` / `NoteDrawn` 在 `Find(key) == nullptr` 时**静默 return**（`war3_palette_object_evidence.h:346-354`、`:392-401`）。
- 解析器契约要求链形如 `Rejected → ServedCandidate → Enqueued → Drawn → terminal`，并对乱序报 `servedCandidateBeforeRejected` / `enqueuedBeforeServedCandidate`（`AutoTest/analyze_palette_object_evidence.py:10-11,463-478`）。
- 后果：在**没有拒绝发生**的正常会话中，观察表恒空 ⇒ `emitted` / `watchCount` / 全部 drop 计数**精确为 0**。已用实机数据确认（生产路径采集块实测执行 `insertReached ≈ 48 万次`、`noteCalled ≈ 36.8 万次`，而全部输出计数为 0）。
- 三条修复路径均被既有约束关闭：① 伪造 `NoteReject`（头文件明令禁止 `NotChecked` 冒充 R0）；② 首次观测直接发 `Enqueued`（解析器判 orderViolation）；③ 保持现状（恒为零）。
- ⇒ 需在**证据 wire 契约**层面决策（新增"首次观测"阶段并同步改解析器，或改以"拒绝"为取证对象）。**这是设计层缺陷，不是实现层缺陷。**

### 6.2 验证状态

- **本树未提交**（594 条目），HEAD 早于全部变更；不存在"基线 commit → 当前 commit"的可审计差异。
- **全门禁未在当前树上复跑**。本轮仅验证了受影响门禁：palette object wire roundtrip **116/116 PASS**、evidence cost **38/38 PASS**、frame evidence runtime **723 PASS**、recorder memory **87 PASS**，以及 `ninja -C build32 -n` = no work。**静态全量、meson、生命周期、线程门、taxonomy、成本门禁等未复跑。**
- **但需区分**：本仓库 `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 记载，**次班独立审核**曾对**本轮诊断改动之前**的同一棵树复跑全门禁并全绿：全量静态 **256/256**、meson **84/84**、域测试 **46/46**、预算门禁 EXIT 0（冻结 157 / 站点 112 / 已迁出 46 / device.cpp 567）、M1/M2 等价门禁 EXIT 0。
- ⇒ 正确表述是：**该树在诊断改动前有全门禁全绿记录**；诊断改动后**只验证了受影响门禁**，**未**复跑全门禁。二者不可混为一谈。
- `AGENTS.md` 中记录的验证数字（如 88/88、474/474、28/28）分属不同时间点与不同树，**不能**直接当作当前树的结果。

### 6.3 结构与工程风险

1. **两棵并行树 + 全部工作未提交**：`dxvk`(170) 与 `dxvk-v1.22-integration-20260914`(594) 各自带未提交变更，跨树同步无机制保障。
2. **超大翻译单元**：`d3d9_device.cpp` 5.0 万行；4 个文件 >1.1 万行。
3. **取证/记录器子系统膨胀**：`tools/` 已达 34.6k 行，且与渲染核心共享进程状态（同一 DLL、同一 `PaletteObjectRecorder` 全局）。
4. **诊断插桩滞留**：本轮为定位上述问题加入了 5 类诊断计数与生产路径采集块，**仍在树中**，冻结前需决定去留。
5. **已删除的验证前提**：用于实机自动化的隔离沙箱目录已被移除，取证脚本已改指向玩家真实安装目录 —— 这提高了"误动玩家环境"的风险面（当前以备份 + 只读采集约束控制）。

---

## 7. 本轮（2026-09-18）新增改动明细

| 文件 | 改动 | 性质 |
| --- | --- | --- |
| `src/d3d9/d3d9_device.cpp` | ① append 函数入口计数；② **生产路径新增对象级采集块**（`PaletteObjectEvidenceEnabled() && ActiveSession()!=0` 短路下 `NoteServed`/`NoteEnqueued`，`source=Unknown`、`hitKey=0`、native 帧记 unknown）；③ 采集点入口到达计数 | 诊断 + 功能 |
| `src/d3d9/war3/tools/war3_palette_object_evidence_sink.{h,cpp}` | 新增 5 组只读诊断 API/计数（appendEntered、enqueueBlockReached、productionInsertReached、productionNoteCalled、resetOrClear） | 诊断 |
| `src/d3d9/war3/tools/war3_frame_evidence.cpp` | 头块新增对应 5 个只读字段 | 诊断 |
| `AutoTest/autotest_sessions.py` | 沙箱根常量改指向 `E:\Work\Warcraft III`（单一来源） | 基础设施 |
| `docs/plan/2026-09-18-mcp-driven-real-machine-feasibility.md` | §0–§33 完整实机取证记录与根因推导 | 文档 |

**语义诚实性声明**：新增采集块在无法证明语义的字段上**一律记 unknown/0**（`runtimeModelPtr` 传 `nullptr`、native 帧标 `unknown`、来源记 `Unknown`），未伪造任何证据。诊断计数仅计数、不改变 caster 准入或渲染行为。

---

## 8. 建议审核方回答的问题

1. **证据链契约**：对于"阴影链健康、无拒绝"的对象，对象级证据应当如何建模？应扩展链首阶段（新增"首次观测"）并接受解析器契约变更，还是应把取证目标改为"拒绝"本身？
2. **模块边界**：`render/`(55.8k) 与 `gpu_skin/`(42.9k) 的职责划分是否已过度耦合？GPU-skin 的 fence/所有权抽象是否应在更底层统一？
3. **上帝对象拆分**：`d3d9_device.cpp` 5.0 万行应如何按"职责"而非"文件"拆分（例如把 War3 语义、设备生命周期、draw-time 缓存分成独立 TU）而不破坏"安全点合并执行"不变式？
4. **取证子系统与渲染核心的隔离**：`tools/` 34.6k 行与渲染共享进程状态，是否应引入明确的"只读观察面"接口（类似 PImpl/端口），使取证代码无法影响渲染决策？
5. **未提交超大工作树的审查策略**：594 条目、+4.8k/−3.5k 已跟踪变更 + 491 新文件，如何分片审查与冻结？
6. **验证充分性**：在"全门禁未复跑"的前提下，哪些门禁是发布前**不可豁免**的最小集？

---

## 9. 附录：度量方法与文件索引

- 规模统计口径：`Get-ChildItem src -Recurse -File -Include *.cpp,*.h,*.hpp,*.c`，行数按物理行计（`Get-Content -ReadCount 0`）。
- 变更状态口径：`git status --porcelain`（`M` = 已跟踪已改，`??` = 未跟踪）；diff 口径：`git diff --shortstat HEAD`（**不含**未跟踪新文件）。
- 逐文件清单（810 文件 + 按目录汇总）：见 `2026-09-18-openai-review-source-inventory.md`。
- 取证全过程记录：见 `2026-09-18-mcp-driven-real-machine-feasibility.md`（§0–§33）、`2026-09-18-overnight-progress-log.md`。
- 项目入口与当前状态摘要：仓库根 `AGENTS.md`；生命周期 `docs/WAR3_LIFECYCLE.md`；自动化边界 `AutoTest/README.md`。

### 附：提交的两个源码包

| 包 | 文件 | 大小 | SHA-256 | 内容 |
| --- | --- | --- | --- | --- |
| `WarVK-source-review-20260918.zip` | src 全量 | 4.3 MB | `634F7357CD24C5B8842EC0676338117A579276B6F730CB99E3ED97E890FC0F82` | `src/`（810 文件 / 457,374 行）+ `AGENTS.md` + `WAR3_LIFECYCLE.md` + 4 份审核/取证文档 |
| `WarVK-autotest-shaders-review-20260918.zip` | **692** | **1.96 MB** | `14D041A888B5BFC52356468BA0A3CB16420DB7F093A49DE98AFB3D770888F749` | `AutoTest/` 代码（558 文件，含 296 个 `test_*.py` 静态门禁与 25 个 `analyze_*.py` 解析器）+ 全部 shader 源码（WarVK war3fx 包、DXVK/D3D9 shader、War3 HLSL 入口、`war3_shadow_common.glsl`）+ `MANIFEST.md` |

两包按仓库相对路径存放，可解压到同一目录合并。AutoTest 包**排除了 `AutoTest/artifacts/`**（10,378 文件 / ≈31,974 MB 实机采集产物）与 `__pycache__`，理由见 `2026-09-18-openai-review-autotest-shaders-inventory.md`。

> **提交说明**：本报告描述的是**工作树当前状态**，全部变更**未提交**、**未部署到稳定通道**、**未通过全门禁**。任何引用本报告的结论都不得表述为"稳定版"或"已验收"。
