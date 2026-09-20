# AutoTest 与 Shader 补充审核包（清单与边界）

> 配套：`2026-09-18-openai-review-brief.md`（主报告）、`2026-09-18-openai-review-source-inventory.md`（src/ 逐文件清单）
> 生成：2026-09-18 ｜ 树：`dxvk-v1.22-integration-20260914`

## 1. 两个源码包的分工

| 包 | 文件 | 大小 | SHA-256 | 内容 |
| --- | --- | --- | --- | --- |
| `WarVK-source-review-20260918.zip` | 全量 src | 4.3 MB | `634F7357CD24C5B8842EC0676338117A579276B6F730CB99E3ED97E890FC0F82` | `src/`（810 文件 / 457,374 行）+ `AGENTS.md` + `WAR3_LIFECYCLE.md` + 4 份审核/取证文档 |
| `WarVK-autotest-shaders-review-20260918.zip` | 692 | **1.96 MB** | `14D041A888B5BFC52356468BA0A3CB16420DB7F093A49DE98AFB3D770888F749` | `AutoTest/`（代码）+ 全部 shader 源码 + `MANIFEST.md` |

两个包按仓库内相对路径存放，可解压到同一目录合并为完整审核视图。

## 2. `AutoTest/` 内容（本包 558 文件 / 487 `.py` / 31 `.cpp` / 24 `.ps1`）

AutoTest 是"发布级自动化 + 性能基线 + 运行时取证"工具链。**全部平铺于顶层**（无功能子目录），另有 `_archive/` 存历史脚本。

**顶层 451 文件 + `_archive/` 107 文件 = 558 文件**（全包 `.py` 487、`.cpp` 31、`.ps1` 24）。下表除 `_archive/` 一行外均为**顶层**计数：

| 类别 | 数量 | 说明 |
| --- | --- | --- |
| `test_*.py`（静态门禁） | **296** | 静态/结构/合同检查脚本 |
| `test_*.cpp`（Win32 runnable） | **31** | 需要实机运行的宿主测试 |
| `analyze_*.py`（离线分析器） | **25** | 解析实机产出的 JSON/日志/性能报告 |
| 其它 `.py`（顶层） | 73 | 启动、打包、辅助工具 |
| `.ps1`（顶层） | 10 | Windows 启动与编排（另有 `_archive/` 内 `.ps1`） |
| `.j` / `.md` / 其它 | 8 | JASS 脚本与说明文档 |
| `_archive/`（子目录，非顶层） | 107 | 历史一次性脚本（保留以追溯） |

**与本次审核直接相关的关键文件**：

- `analyze_palette_object_evidence.py`（49 KB）—— 对象级证据链解析器；**其阶段顺序契约（`Rejected → ServedCandidate → Enqueued → Drawn → terminal`）是主报告 §6.1 所述设计缺陷的另一端**；
- `test_palette_object_evidence_analysis_static.py`、`test_palette_object_wire_roundtrip.py`、`test_palette_object_capture_points_static.py` —— 对应写方/读方的合同门禁；
- `autotest_sessions.py`（20.2 KB）—— 会话与沙箱根常量（本次已改指向真实安装目录）；
- `frame_evidence_control.py`、`analyze_frame_evidence.py`、`analyze_frame_history.py`、`analyze_frame_timeline.py`、`analyze_frame_inputs.py` —— 帧取证子系统的控制面与解析器；
- `test_*_static.py` 中与 shadow/palette/gpu_skin/registry/线程门相关的门禁。

## 3. Shader 源码（本包 134 文件）

| 路径 | 内容 |
| --- | --- |
| `subprojects/war3fx/`（30） | **WarVK 自有 shader 包**：16 `.frag`、7 `.comp`、4 `.vert`、1 `.glsl`（`shaders/war3_shadow_common.glsl`，即 2026-09-16 抽出 17 个重复函数的公共库）、`meson.build` |
| `src/dxvk/shaders/` + `src/dxvk/hud/`（68） | DXVK 上游 shader（含 `dxvk_*_common.glsl` 与 HUD HLSL） |
| `src/d3d9/shaders/`（11） | `d3d9_fixed_function_common.glsl` 等 D3D9 fixed-function 生成 shader |
| `src/d3d9/war3/shader/`（16） | WarVK 材质与 shader manager（C++ 侧） |
| `shaders/`（6） | War3 HLSL 入口：`war3_common/default/outline/postfx/sample/test_triangle.hlsl` |
| `docs/war3_shader_docs/`（11） | shader 文档与 Vulkan pack 模板 |
| `smaa/`（3） | 仅 `SMAA.hlsl` + `LICENSE.txt` + `README.md` |

## 4. 排除项与理由

| 排除 | 规模 | 理由 |
| --- | --- | --- |
| `AutoTest/artifacts/` | **10,378 文件 / ≈31,974 MB** | 实机采集产物（截图 `.tga`、JSON 转储、日志）。体量过大；如需可另行按需提供子集 |
| `AutoTest/__pycache__/` | 124 文件 / 4.8 MB | 字节码缓存 |
| `build32/` 与全部二进制/生成的 SPIR-V | — | 构建产物 |
| `smaa/Demo/`、`smaa/Textures/` | 47 文件 | 第三方 vendored 演示程序与贴图，非 WarVK 代码 |
| `subprojects/imgui/` | — | 第三方（其 GLSL backend 与 shader 相关，但非本项目代码） |

## 5. ⚠️ 验证状态声明（务必与主报告一致）

- 本树**全部工作未提交**（594 条未提交条目，HEAD `ae89054` 早于全部变更）；
- 该树在 **2026-09-18 诊断改动之前**有**全门禁全绿**记录（次班独立审核：全量静态 **256/256**、meson **84/84**、域测试 **46/46**、预算门禁 EXIT 0、M1/M2 等价门禁 EXIT 0）；
- **诊断改动之后**仅复验受影响门禁（palette object wire roundtrip **116/116**、evidence cost **38/38**、frame evidence runtime **723**、recorder memory **87**、`ninja -C build32 -n` no work）；
- ⇒ **不得**把本包或其中任何脚本的通过记录表述为"当前树上全门禁通过"，也**不得**表述为稳定版或已验收。

## 6. 提交建议

向外部审核端提交时，建议连同以下三份文档一并给出（均已在本树 `docs/plan/`）：

1. `2026-09-18-openai-review-brief.md` —— 主报告与 6 个待回答的架构问题；
2. `2026-09-18-openai-review-source-inventory.md` —— `src/` 逐文件清单；
3. 本文件 —— AutoTest 与 shader 包的边界说明；
4. （可选）`2026-09-18-mcp-driven-real-machine-feasibility.md` —— 实机取证全过程与根因推导（§0–§33），供审核方理解 §6.1 缺陷的实证来源。