# 2026-09-16 — 会话交接（策略切换：主线程换 Kimi K3，子线程换 DeepSeek V4.1 Flash）

> 目的：新会话接手时不丢状态、不重复劳动、不破坏纪律。本文是**唯一入口**。

## 0. 一句话状态

两棵树的**分析阶段已完成**；执行阶段已完成：P8 shader 公共化、P5 六切片（采集树/帧时间线全量入库）、
P2 批次 0/2/3（观测层、五源闸、EntryGate 收窄）、**P0 Gap A + Gap B（palette 两处缝隙均已做路由式修复）**、
两道防回退门禁。**P0 的代码工作已收口**，剩余：新计数器导出接线 + 组合候选实机验证（需用户授权）。
全程未部署、未 commit、玩家现场未动。

## 1. 工作树与分支

| 树 | 路径 | 分支/HEAD | 角色 |
| --- | --- | --- | --- |
| **B（主）** | `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914` | `codex/v1.22-release-integration-20260914`，HEAD `ae89054` = 公开 tag v1.21.00 | **唯一集成主线，所有改动落这里** |
| A（只读） | `E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk` | `codex/native-shadow-stable-baseline-20260830`，HEAD `88089cd` | 只读参考；**禁止写入** |

两树是同一 git 仓库的 worktree，B 仓可直接 `git show 88089cd -- <path>` 读 A 的提交对象。
**所有工作未提交**（用户禁止 git 写操作）。

## 2. 不可破坏的硬约束

1. **禁止 git 写**（commit/checkout/reset/add）。
2. **禁止部署 DLL、启动游戏/编辑器**，除非用户明确请求；玩家在用 DLL 不得自动替换。
3. **本会话已禁用审批提示**：遇到沙盒拒绝不要设 `sandbox_permissions`，直接换方法。
4. GPU 资源 reset / Arena 回收 / receiver-skin 切换只能在 `PresentEx` 安全点。
5. 证明失败一律 fail-closed / fail-visible；候选不得冒充稳定。
6. **不得用百分比概括进度**；按交付项报告。**不得把"静态通过"说成"修复"**。
7. 修改图形算法若无法从现有合同证明，先查一手资料并记入 `docs/research/`。

## 3. 门禁基线（每次 checkpoint 全跑）

```powershell
cd E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk-v1.22-integration-20260914
.\build32_safe.cmd src/d3d9/d3d9.dll -j8      # 必须 exit 0
ninja -C build32 -n                            # 必须 no work to do
meson test -C build32                          # 当前 72/72
Get-ChildItem AutoTest -File -Filter 'test_*_static.py' | ForEach-Object { py $_.FullName }   # 当前 240/240
```
完成后必须更新 `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 与
`docs/plan/2026-09-16-merge-execution-ledger.md`。

**当前构建配置是 F855 诊断配置**（`warvk_internal_frame_recorder=true`、
`warvk_skin_palette_contract_candidate=true`）。发布候选必须**关闭诊断选项后重建**。
最近 DLL（Gap A 后，主线程已独立复核）：35,979,538 bytes /
SHA-256 `F73F49B0295A46DF230BC83DCED85CB9F5D40114BAB23335E94A2621C293C039`；
静态全量 **241/241**（主线程复核）。

## 4. 进行中的两件事（接手点）

### 4.1 P0 Gap A —— device 侧 palette 记忆槽位复核（**已完成**，主线程已独立验收）

**结论**：`useCachedEntry`（d3d9_device.cpp:8656-8686）三要素复核已落地——
producer 命中 && producer 槽位 == entry 记忆槽位 && producerGroupCount >= requiredPaletteCount
才供出记忆槽位；否则 rejected 计数 + `return 0xFFFFFFFFu`，调用方落 8860+ PoseFallback；
**无负缓存**（拒绝不写任何状态，下帧绑定恢复即可重新命中）。合同 ON 分支（8564 起）未改。
新增：`AutoTest/test_device_palette_slot_cache_producer_confirmation_static.py`；
`test_live_palette_slot_cache_accelerator_static.py` 仅锚点更新。
**遗留**：两个计数器（served/rejected）尚未导出报告。

（以下为原始接手说明，保留供追溯）
- 文件：`src/d3d9/d3d9_device.cpp`，`useCachedEntry` 约 8635-8647，fail-open 在 **8646**。
- 已定方案：扩展 `queryProducerBindingSlot`（约 8608）取回 groupCount/frameTag；
  **仅当** producer 命中 **且** producer 槽位 == entry 记忆槽位 **且** groupCount ≥ `requiredPaletteCount`
  才供出 `entry.paletteSlotIndex`；否则返回 `0xFFFFFFFF`，调用方落到 **8824+ PoseFallback**。
- 已完成的编辑（子代理自报）：在 `War3TryBuildLiveRuntimeGroupPalette` 前新增两个本地计数
  （`g_devicePaletteSlotCacheServedAfterConfirmCount` / `RejectedStaleCount`）；其余 4 处编辑待做。
- 已知测试锚点：`AutoTest/test_live_palette_slot_cache_accelerator_static.py` 断言
  `queryProducerBindingSlot()` 精确子串，扩展签名后需改为 `queryProducerBindingSlot(`（语义不变）；
  `return entry.paletteSlotIndex` 断言须保持成立。
- **接手动作**：先读 device.cpp 确认当前状态（可编译？改到哪一步？），按上述方案补完 → 跑门禁 →
  changelog/台账 → 若子代理已留下半成品且编译不过，先修到可编译。

### 4.2 运行时开关三分类（**已完成**，清册已写入）

**结论（主线程已抽查 4 行 + 独立验证）**：`docs/plan/2026-09-16-runtime-switch-triage.md`
（1319 行）；总数 412；**A 玩家/生产 = 0**、B 诊断/取证 = 24、C dev-only/实验 = 44、待判定 = 344。
A=0 的原因经独立验证：玩家向文档（`WarVK/`、`README*`）**未点名任何 `DXVK_WAR3_*`**，
玩家控制面实际是 Ctrl+F1 面板 + JAPI。
**需用户裁定（二选一）**：(a) 把有玩家价值的开关正式写入玩家文档并定义与面板的优先级；
(b) 明确声明 env 全属内部面、玩家入口只认面板并在发布门禁校验。裁定前不改默认值、不改文档口径。

（以下为原始接手说明，保留供追溯）
- 已完成的枚举：B 的 `src/` 有 **412** 个 `"DXVK_WAR3_*"` 字符串字面量；另有 **9** 个仅出现在宏/注释
  （CPU_SKIN_HAS_SSE、FORCE_OBJECT_TRACKING、NATIVE_QUEUE_SORT、NATIVE_RENDERER、
  NATIVE_RENDERER_REPORT_INTERVAL、OBJECT_TRACKING_FULL_RESOLVE、OUTLINE_ALL、OUTLINE_FORCE、UNIT_MISS_MARK）。
- 证据提取已完成：每个字面量的 file:line、邻近 helper/默认比较、`#if` 栈、config.h 提及、
  玩家向文档命中（**仅 63 个**开关在 `WarVK/`、`README*`、`CHANGELOG*`、`docs/RELEASE_*` 中出现）。
- 编译期门清单：`warvk_*_dev` / `WARVK_ENABLE_*_DEV` / `WARVK_SKIN_PALETTE_CONTRACT_DEFAULT` /
  `WARVK_INTERNAL_FRAME_RECORDER_DEFAULT` / `WARVK_DATA_COLLECTION_TREE_DEV`。
- 目标文件：`docs/plan/2026-09-16-runtime-switch-triage.md`（**尚未写入**）。
- 分类规则：A 玩家/生产（**必须有玩家向文档**）；B 诊断/取证（默认关）；C dev-only/实验（有编译期门）；
  证据不足 → 待判定并写明缺什么。**禁止猜测**。

## 5. 已落地交付（B 树）

| 项 | 内容 |
| --- | --- |
| P8 | shader 公共化：`subprojects/war3fx/shaders/war3_shadow_common.glsl` + `war3_shadow_caster_interface.h`；spirv-dis 逐行等价 |
| P5 六切片 | 采集树/帧时间线独立库（`war3/tools/war3_data_collection_tree*.{h,cpp}`、`war3_frame_timeline*.{h,cpp}`）+ meson 选项 `warvk_data_collection_tree_dev`(默认 false) + 2 个 C++ 测试 + perf_monitor JSON/录制门 + swapchain Present 切帧 + lifecycle 17 个 Scope + **114 个 WARVK_DATA_SCOPE** + 2 个静态守卫 + 2 个分析器 |
| P2 批次 0 | 14 个 `QueryShadowRegisterImage*`/`QueryShadowPathStaticStamp*` + filter policy 分支；**所有相关 constexpr 仍为 false（行为不变）** |
| P2 批次 2 | `src/d3d9/war3/war3_path_blocker_evidence.h`（唯一实现）+ canonical/semantic 四处提交闸 |
| P2 批次 3 | EntryGate 收窄（device.cpp 42986-43012 / 46604-46622）+ 7 项断言（含正向面） |
| **P0 Gap B** | `war3_shadow_renderer_core.cpp`：缓存供出前 producer 复核、`tryEngineDirectPosePalette` 快照优先、3 个本地计数、CPU 替代路径保留、会话隔离未动；测试 `test_palette_slot_cache_producer_confirmation_static.py` |
| 门禁 | `test_device_semantic_responsibility_budget_static.py`（device.cpp 语义符号白名单，37 个，只允许减少）；`test_diagnostic_build_options_default_off_static.py`（诊断/候选选项必须默认关） |

## 6. 用户裁定与纠正（必须遵守）

1. **doodad 贴花默认值**：保持 B 既有默认（透传），**不顺带改变玩家画面**；批次 1 不执行。
2. **P6（A 侧 Observe-only 原生灯）**：保留历史、**暂不移植、也不删除**。
3. **下一次实机**：单独组织**组合候选**验证，重点：高压低视角、往返移动、
   **压力解除后的阴影恢复**。需用户明确授权部署。
4. **"证明失败就不画"不能自动算修复**。五源闸/EntryGate 类改动必须同时证明：
   (a) 不该投影的 path-blocker 被拦住；(b) 正常单位/建筑/桥梁/装饰物**仍能投影**；
   (c) 失败后**能恢复**，不是持续遗漏。静态 ≠ 画面。
5. **palette 两处缝隙优先，但不能只改成返回失败**：验收须证明**错误矩阵不再被使用**
   **且合法对象仍有正确替代路径**；不得只凭撕裂减少/拒绝计数增加宣布通过。
6. **Stage13 常驻几何**涉及资源保留，**必须先验证占用与回收**，
   不得未经验证就认定它能改善**累积超预算**。
7. **不要让"搬完所有旧功能"挤掉眼前的发布问题**。
8. 主线程**不只是监管**，也要亲自推进；但需明确分工。

## 7. 优先级队列

| 优先级 | 事项 | 状态 |
| --- | --- | --- |
| **P0** | palette 两处缝隙（Gap A + Gap B） | **均已完成**（路由式，未实机） |
| **P0** | 接线新计数器到报告（Gap B 三个 + Gap A 两个，共 5 个） | **待做（下一步）** |
| **P0** | 组合候选实机验证（三证明 + 反例门） | **等用户授权** |
| P1 | 统一数据选择入口 S2/M2：合并 `TryBuildRuntimeGroupPalette` 同名双实现 | 设计已出 |
| P2 | P2 批次 4（registry domain 隔离）→ 5（alias 迁移） | 未开始 |
| P3 | P2 批次 6（Stage13） | 前置清单已出，须过占用/回收验证 |
| P4 | 语义职责迁出 device.cpp M1→M4 | 清册已出 |
| P4 | 运行时开关三分类清册 | **已完成**（A=0，待用户裁定文档口径） |

## 8. 文档索引（都在 B 树）

- 总台账：`docs/plan/2026-09-16-merge-execution-ledger.md`（**先读这个**）
- P2 移植审计：`docs/plan/2026-09-16-stage13-port-audit.md`
- P5 移植审计：`docs/plan/2026-09-16-p5-collection-timeline-port-audit.md`
- P1 palette 缝隙分析：`docs/research/2026-09-16-palette-provenance-gap-analysis.md`
- **P0 修复工单**：`docs/plan/2026-09-16-palette-gap-fix-workorder.md`
- **P0 实机方案**：`docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md`
- 统一入口设计：`docs/plan/2026-09-16-unified-data-selection-entry-design.md`
- Stage13 前置：`docs/plan/2026-09-16-stage13-retention-verification-prereq.md`
- 语义职责迁移：`docs/plan/2026-09-16-device-semantic-responsibility-migration.md`
- A 树差异清单（只读参考）：`A:docs/plan/2026-09-16-tree-merge-diff-inventory.md`
- 开发日志：`docs/agent-history/DEVELOPMENT_CHANGELOG.md`

## 9. 环境陷阱（血泪教训）

1. `glob`/`grep` 工具在 A 树会因 `.pytest_cache`、`subprojects/StormBreaker/...asi_backup*`
   权限受限目录**整体失败** → 用 Node `fs` 直读或 `pwsh Get-ChildItem` 定向扫描。
2. `Get-FileHash` 是**原始字节**；`git hash-object` 会做文本归一（CRLF 不敏感）。
   判断"文件是否一致"要用 `git hash-object`。
3. `pwsh Get-Content | Measure-Object -Line` 常返回 -1；`Select-String -Recurse` 在受限树上静默失败。
4. 子代理**可能"声称完成但没写文件"**（曾发生）。必须：要求**报告写在回复正文**，
   且主线程**独立复核产物**（哈希、门禁计数、字节一致性）。
5. 子代理可能**卡住多轮无产出** → 发状态请求；仍无进展则中断并由主线程接手。
6. 同一时刻只跑一个构建（build32 独占）；改动同一文件的两个任务必须串行。

## 10. 新会话建议的第一步

1. 读本文 + 台账；跑一次完整门禁确认基线（期望 72/72、静态 240/240、no-work）。
2. Gap A 已完成，只需按 §4.1 复核既有改动是否完好（可选）。
3. 做"新计数器导出"接线（见 §7 第二行，共 5 个计数器），让实机可以度量证明 (b)(c)。
4. 向用户申请实机授权，按 `docs/plan/2026-09-16-p0-combined-candidate-real-machine-plan.md` 执行。
5. 并行派子代理写"开关三分类清册"（§4.2）。
