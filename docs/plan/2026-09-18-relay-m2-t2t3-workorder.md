# 2026-09-18 — 接力工单：M2 切片实施 + T2/T3 可批准证据方案（本轮执行计划）

> 本工单是接力会话的执行计划与范围界定，不代表任何代码已改动。
> 交接来源：上一 agent 的《WarVK 当前状态与待办交接》（A–E 节）；本工单把其中 C/E 节落到可执行切片。

## 进度状态（2026-09-18 更新）

- **T2/T3 方案文档：已完成**（`2026-09-18-t2-slot-ownership-minimal-evidence-plan.md`、`2026-09-18-t3-thread-writer-runtime-proof-plan.md`，均为「未实施/待上级批准」状态，待复审）。
- **M2-1：已闭合**（8 符号迁出 + 等价证据 + 2 变异真跑；meson 83/83、静态 249/249；DLL 36,271,659 B / `D82AFF08…`；证据 `2026-09-18-m2-1-migration-equivalence-record.md`）。
- **M2-2：已闭合**（选择链本体 621 行 + 枚举/类型/slot 缓存/Gap A 计数器迁出；链差分三矩阵 10,357,597 / 11,167,085 / 6,869,485 全过；2 变异真跑；meson 83/83、静态 249/249；DLL 36,270,708 B / `EB9D51DE…`；证据 `2026-09-18-m2-2-migration-equivalence-record.md`）。
- **下一步**：M2-3（motion 诊断三函数）→ M2-5（taxonomy 对照门禁设计）。T1/T8 仍待用户实机授权。

## 0. 起点状态（2026-09-18 已逐项核验，非转述）

| 项 | 核验结果 |
| --- | --- |
| 写入树 | B 树 `dxvk-v1.22-integration-20260914`，分支 `codex/v1.22-release-integration-20260914`，HEAD `ae89054`（与交接 `ae890542` 前缀一致） |
| 工作树 | 509 个未提交改动（与交接"未提交"一致） |
| 候选 DLL | `build32/src/d3d9/d3d9.dll` = **36,271,456 B / `CB62253417CEC91ADC9A16F7D5BAC8E758370C62DFBFEB63D84906FF031750AE`**（实测一致） |
| `ninja -C build32 -n` | **no work to do**（实测） |
| 工具链 | ninja/meson 位于 `D:\Environment\Python3.13.11\Scripts\`（git-bash/cmd PATH 不可见，须用显式路径）；`py` = Python 3.13.11 |
| 交接证据文档 | `docs/plan/2026-09-17-*.md` 全部在树；CHANGELOG 顶部已含 09-17 条目（纪律被遵守） |

## 1. 本轮范围裁定（对照交接 C/E 节）

| 交接任务 | 本轮处置 | 理由 |
| --- | --- | --- |
| T1 实机取证 ×2 + ④ 三项判定 | **不执行，待用户实机** | 需启动游戏且每会话仅一次导出；纪律 D3「不启动游戏除非有明确授权」；已部署 DLL（`E:\Work\Warcraft III\d3d9.dll` = `A0A51AF2…`）保持不动，用户随时可跑 |
| T8 性能护栏 + 老问题 | **不执行** | `dual_perf_baseline.py` 须前台实机，同属实机授权范围 |
| T2 槽位所有权（路线 A） | **出方案文档**（`2026-09-18-t2-slot-ownership-minimal-evidence-plan.md`） | 交接原文「需要先出可批准的最小证据方案」；路线 A 改准入未获上级批准，本轮只写方案不写代码 |
| T3 线程/写者运行时证明 | **出方案文档**（`2026-09-18-t3-thread-writer-runtime-proof-plan.md`） | 同上；bits[13] wire 变更需上级裁定，方案中列为待裁定项 |
| **T4 = M2** | **本轮实施 M2-1 切片**（见 §2） | 交接 E 节顺序中 T1/T8 被实机阻塞后，M2 是下一个可离线推进项；用户明确要求「继续重构」 |
| T5(M3)/T6(M4)/T7 | **不动** | 交接明确：风险高，放实机反馈之后 |

## 2. M2-1 切片范围（M2 的第一片，不是 M2 全部）

**依据**：`2026-09-16-device-semantic-responsibility-migration.md` §3 M2 行 + `2026-09-16-unified-data-selection-entry-design.md`。
**切片理由**：M2 整体（选择链本体 621 行 + slot 缓存 + 计数器 + 等价差分扩展）过大，按迁移文档 §4-4「每步独立可回退」切三片：

| 片 | 内容 | 状态 |
| --- | --- | --- |
| **M2-1（本轮）** | 纯计算 helper ×4（`War3SemanticHashMatrixPalette`、`War3DecodeRuntimePoseMatrix48`、`War3TryReadRuntimePoseArray`、`War3ResolveLivePoseRuntimeAlias`）+ env getter ×4（`War3SemanticLivePaletteSafeCopyRuntime`、`…RefreshRuntime`、`…AllowCModelFallbackRuntime`、`War3SemanticPaletteDiagnosticsRuntime`）→ 新模块 `src/d3d9/war3/semantic/war3_live_palette_selection.h/.cpp`（命名空间 `dxvk::war3::semantic`，与 M1 模块同，保证调用点机械改写） | 本轮实施 |
| M2-2 | 选择链本体 `War3TryBuildLiveRuntimeGroupPalette` + `War3SemanticPaletteSource` enum + `War3LivePaletteBuild*` 类型 + slot 缓存（含两个计数器按 M1 D 类先例改外部链接） | 下一轮；差分替身世界（SafeCopy/PoseRegistry/producer 绑定表/Game.dll arena）设计先行 |
| M2-3 | motion 诊断三函数（`War3NoteLivePaletteMotion`/`…DrawTimePoseMotion`/`…SubmittedPaletteMotion` + Entry 结构） | 下一轮 |

**M2-1 明确不做**：不迁选择链本体/枚举/类型/计数器；不动 `war3_shadow_renderer_core.cpp:835` 与 `war3_model_hook.cpp:927` 的两份 `HashMatrixPalette` 姊妹（S2 设计 §5-9：仅登记不抽取）；不改任何判定结果；taxonomy 发射全部留在 device.cpp 调用侧；不开新 env/编译开关。

**M2-1 门禁扩展**（M1 范式，全部加法式、不削弱任何 M1 断言）：
1. 预算门禁 FROZEN：7 个命名族符号 budget→0；`War3SemanticHashMatrixPalette` 不在命名族盲区须**先补表**（baseline 1, budget 0）；`MODULE_HOME_PATHS` 加新模块文件。
2. 等价证据：迁移**前**先捕获 pre-M2 工作树 device.cpp 的 SHA-256 作 provenance（工作树未提交，git 无此 blob，与 M1 的 git provenance 不同，须在等价记录中显式说明）；**生成器脚本入库**（M1 生成器未入库，本轮补齐该缺口）；新 legacy 参考 `.inc` 逐字节抽取 8 个定义 + 命名空间改写；新差分测试目标（`sem::` vs `legacy::`，确定性大输入空间，0 mismatch，含 4 个新 env getter 的 probe 场景）；等价门禁 COVERED +8、新 SHA 双钉死。
3. 变异验证 ≥2 条**真跑**（如翻转哈希常量、改模块侧 env 默认值），必须被差分/等价门禁杀死再回退。

**复跑门禁全量清单**（Below Normal + `-j2`，构建只此一家）：
构建 exit 0 → `ninja -n` no work → 预算门禁 → 等价门禁 → 新差分测试（默认 + probe env 矩阵）→ `meson test -C build32 --num-processes 2`（M1 时点 82/82）→ 全量静态 `test_*_static.py`（M1 时点 248/248）→ 记录器 23/23、成本 PASS(38/38)、生命周期 187/187、解析器静态 94/94、根读方 55/55、往返 CERTIFIED。

**证据文档**：`docs/plan/2026-09-18-m2-1-migration-equivalence-record.md`（M1 记录同构：对照表、provenance、差分设计、变异验证、未覆盖项）。

## 3. T2/T3 方案文档要求（只写文档，不动代码）

共同纪律：两份文档都是**待批准方案**，头部标「未实施 / 待上级批准」；所有 file:line 引用以函数名锚（行号会漂移）；明确区分 render-host SlotLedger 合同与 palette 槽位所有权（同名不同物）；措辞上限按上级 2026-09-17 03:05 裁定（不得写运行时肯定表述）。

- **T2**（`2026-09-18-t2-slot-ownership-minimal-evidence-plan.md`）：命题分层 P1（同帧未被第二来源域重写）/P2（与 A5 通过样本不相交，线索级）/P3（native 生产者即 owner，**本轮不可证**）；路线 A/B 历史与上级裁定 5；`slotAllocationGeneration` 恒 0 是结构性事实（全树无写入点）须静态钉死「0 永不授权」；最小改动集 = 路线 B 已批准的写者见证记录点（`CaptureBlendedPaletteSlotRange` 两调用点 + A5 站点 + 2 个导出计数 + 离线判读 + 静态测试），零准入改动、env 默认关；验收口径「未观察到 ≠ 已证明」。
- **T3**（`2026-09-18-t3-thread-writer-runtime-proof-plan.md`）：命题 Q1（消费线程逐轮是谁）/Q2（全部写者线程 id 集合）/Q3（措辞上限「已观察到同线程」，永不升级「已消除竞争」）；写者戳候选（per-entry 原子 vs 最后写者+位掩码）；mismatch 计数 + 7 处导出链先例；`bits[13]` 填充 vs 新 label `palette-writer/v1` 二选一（**涉 wire/label 登记，列为待上级裁定**）；宿主并发单测；逐轮原始 id 列表口径；mismatch≠0 时 A5 结论一律 fail-visible。
- 两份文档互相交叉引用：写者见证记录点同址（`CaptureBlendedPaletteSlotRange`），但所有权（T2）与线程关系（T3）判定口径独立。

## 4. 纪律复述（本轮全程有效）

同一时刻只一个构建（共享 build32）；实机只在 `E:\Work\Warcraft III` 且本轮不实机；不 git 写、不部署、不启动游戏；候选绝不称稳定版；每条结论同 checkpoint 更新 `docs/agent-history/DEVELOPMENT_CHANGELOG.md`；fail-visible/fail-closed；CPU 侧证据 ≠ GPU 提交 ≠ 像素证明；M2-1 落地后按冻结纪律产出新候选身份记录（新字节数/SHA-256 + 门禁原始输出）。
