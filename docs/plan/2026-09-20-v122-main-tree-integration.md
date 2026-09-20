# v1.22 主树整合方案（integration-release-r1，只读设计）

> 2026-09-20执行状态：主线程已完成B-primary源码checkpoint及原主目录切换，见
> [正式执行回执](2026-09-20-formal-main-tree-integration.md)。下文保留r1/r2历史分析，
> 其中外移StormBreaker再update方案未执行；以回执的原位保护和实际验证为准。

## 0. 本 batch 边界

- taskId: `integration-release-r1`
- 本次只做只读复核、确定性审计工具、临时 fixture 测试和整合方案；未 commit / merge / reset / checkout / switch / worktree add / copy-over A。
- 无 build / Ninja / Meson / compiler / game / deploy / GPU / git 写权限；未改玩家 DLL、版本、AGENTS、Meson、device、诊断桥、总开发日志或任何 changelog。
- 玩家 DLL 保留要求成立：`E:/Work/Warcraft III/d3d9.dll` 在本次只读核验中为 36,412,825 B，SHA-256 `FAC75C10D640F011BA07482B1706E77223756095BCFFCA448046B2FA0289E529`；当前存在 `War3` 进程，未停止、未替换、未部署。FAC 正向反馈是玩家移动恢复反馈，不是完整发布验证。
- A/B 源码备份位于 `D:/WarVK-Backups/20260920-release-closeout-{A,B}`；work order 说明 root 已验证完整，本批未读取、未修改、未删除。

## 1. 本次独立只读复核（2026-09-20 daytime snapshot）

| 项目 | A（原主 worktree） | B（权威 v1.22 worktree） |
| --- | --- | --- |
| 路径 | `E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk` | `E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk-v1.22-integration-20260914` |
| branch | `codex/native-shadow-stable-baseline-20260830` | `codex/v1.22-release-integration-20260914` |
| HEAD | `88089cdf90f728e85b91bf45d75002348574b665` | `ae890542d766470d1703f5bea7f5b73636039733` |
| 备注 | A HEAD 只比 merge-base 多 1 个 commit | B HEAD = tag `v1.21.00` / `origin/main`；B 比 merge-base 多 221 个 commit |
| status 条目 | 175（55 tracked dirty + 120 untracked） | 915（113 tracked dirty + 802 untracked；并行 root 工作期间可能继续变化） |
| non-ignored inventory | 2041 | 2988 |
| ignored artifacts（未列入 inventory） | 11530 | 17176 |
| inventory 独有 | A-only 73 | B-only 1020 |

- common git dir：`E:/Mycode/Source/Repos/War3MapReforge/Core/Base/Graphics/dxvk/.git`
- refs：189 个；34 个 worktree；`origin` 存在但本批没有 fetch/push，且未使用上游 URL 或凭据。
- merge-base：`8f232cc7d5a69bb4cadc99d41795273e6900b9f1`。
- ancestry：A 独有 1 个 commit，B 独有 221 个 commit；A 不是 B 的 ancestor，B 也不是 A 的 ancestor。因此不能把当前 A 直接 fast-forward 到 B，也不能把当前 B 直接视为已含 A 的 checkpoint。
- A 独有 commit `88089cd` 含 35 个路径；其中 7 个路径不在 B HEAD，28 个路径在 B HEAD 有不同 blob。A 的旧 stage13 checkpoint 与 B 的 221 个后续提交之间有大量已演化内容，不能把 A 的 35 文件整包覆盖 B。

## 2. A-only 内容与 A 本地资产

### 2.1 A-only committed paths（7 个）

- 3 个测试：
  `AutoTest/test_shadow_path_blocker_five_source_parity_static.py`
  `AutoTest/test_stage13_content_persistent_exact_identity_model.py`
  `AutoTest/test_stage13_content_persistent_identity_static.py`
- 4 个文档：
  `docs/agent-history/2026-08-26-native-static-shadow-path-blocker-bridge-cache.md`
  `docs/agent-history/DEVELOPMENT_CHANGELOG.md`
  `docs/plan/2026-08-30-native-shadow-stage13-stable-baseline-gate.md`
  `docs/research/war3_render_issues/32_2026_08_26_render_layer_coverage/README.md`

这些路径必须显式 review，不能随主树 switch 隐式消失。`docs/agent-history/DEVELOPMENT_CHANGELOG.md` 属于 changelog，root 所有。

### 2.2 A 本地用户资产/证据

- `PlayerCrash/**`：6 个 crash/analysis 文件，只保留外部，不入 Git 整合。
- native model light 本地源码/测试：6 个文件，位于
  `src/d3d9/war3/render/war3_native_model_light_*.h` 和
  `src/d3d9/war3/render/tests/war3_native_model_light_*.cpp`。
- dxvk 本地 audit 头：2 个文件
  `src/dxvk/dxvk_image_origin_audit.h`
  `src/dxvk/dxvk_image_relocation_audit.h`。
- A-only `AutoTest/**` 约 53 个脚本/测试，包括 backbuffer/frame_sync/native_model_light/image audit 等。
- A-only 旧文档 1 个：`docs/plan/2026-09-16-tree-merge-diff-inventory.md`；这是历史 inventory，不能作为当前依据。
- 日志/构建输出：`*.log`、`_run_static_all.ps1` 等为本地/生成内容，整合排除但不得删除。

### 2.3 A-only dirty tracked（B status 中不存在，或为 mixed）

- 12 个 tracked 文件 + 1 个 gitlink dirty：
  `AutoTest/test_native_doodad_static_stamp_gate_static.py`
  `meson.build`
  `src/d3d9/d3d9_war3_aa.cpp`
  `src/d3d9/d3d9_war3_shadow_resources.cpp`
  `src/d3d9/d3d9_war3_ssao.cpp`
  `src/d3d9/war3/hooks/war3_hook_lifecycle.h`
  `src/dxvk/dxvk_context.cpp`
  `src/dxvk/dxvk_device_info.cpp`
  `src/dxvk/dxvk_device_info.h`
  `src/dxvk/dxvk_image.cpp`
  `src/dxvk/meson.build`
  `src/vulkan/vulkan_loader.h`
  `subprojects/StormBreaker`
- 另有 `docs/agent-history/DEVELOPMENT_CHANGELOG.md` 为 A tracked / B untracked 的 mixed dirty；归入 A 本地/changelog root 处理。
- 这些 A dirty 与当前 B worktree 均不同；其中 `meson.build`、`src/dxvk/meson.build`、device/诊断相关内容属于 root 所有权，不应由 integration lane 自行解决。

## 3. 冲突 dirty 与 B 侧状态

### 3.1 `review_conflict` 52 个的精确拆分

- 40 个 tracked-vs-tracked 真冲突，A/B 两侧都已 tracked dirty 且 A/B worktree blob 与双方 HEAD 均形成三方分歧，例如：
  `AGENTS.md`、`meson_options.txt`、`src/d3d9/d3d9_device.cpp`、`src/d3d9/d3d9_surface.cpp`、`src/d3d9/d3d9_swapchain.*`、`src/d3d9/d3d9_war3_scene.h`、`src/d3d9/d3d9_war3_shadow.*`、`src/d3d9/d3d9_war3_volumetric_light.*`、`src/d3d9/meson.build`、`src/d3d9/war3/hooks/*`、`src/d3d9/war3/model/*`、`src/d3d9/war3/render/*`、`src/d3d9/war3/tools/*`、`subprojects/war3fx/shaders/*`。
- 11 个 untracked-vs-untracked 分歧，例如 data_collection、frame_timeline、native_async_screenshot、geoset identity test、war3fx caster/shared shader header 等。
- 1 个 mixed：`docs/agent-history/DEVELOPMENT_CHANGELOG.md`（A tracked dirty，B untracked dirty）。
- 以上完整逐文件列表和 blob/status 在生成的 `v122-tree-integration-files.csv`；逐项 review 是进入主树整合前的硬门。

### 3.2 `review_a_change` 12 个

- 12 个 A dirty、B 对应路径 clean/untracked/缺失且内容不同的 tracked 文件，详见 CSV。
- 处理原则：默认 B；只有 review 证明 A 的变化仍适用且未被子提交覆盖时，才摘取 A hunk。

### 3.3 committed tree divergence

- A/B 双方 clean 但 HEAD blob 不同的路径有 87 个，审计 route 为 `review_head_divergence`；B primary，但需要结合 A 的 35 路径独有 commit 检查是否含被 B 漏掉的语义。
- B-only committed paths 268 个；这些是 B 221 个提交带来的权威内容，不能因为 A 不存在就回退。

### 3.4 submodule / gitlink

- `.gitmodules` 两侧相同，登记 9 个 submodule；index 中当前只有 4 个 gitlink。
- `smaa`、`src/minhook`、`subprojects/imgui` 在 A/B index 相同。
- `subprojects/StormBreaker`：
  - A index gitlink = `2b287f90cb9c9dac553a057e65904708161e3a88`。
  - A worktree HEAD = `375e82e3e4f901c52285c716bd090ea35fe8721c`，状态 `+`（worktree 与 index 不同），子仓另有 26 个 tracked 修改和 66 个 untracked 条目（含 build outputs）。
  - B index/worktree = `2b287f90cb9c9dac553a057e65904708161e3a88`，干净。
- 结论：B 的 gitlink 保持权威；A 的子仓 commit 和 dirty 内容必须单独导出/备份，不能作为 superproject 文件 transplant，也不能被 B 的 `2b287f9` 隐式覆盖。本批不修改任何 gitlink。

## 4. 推荐拓扑与 rollback-safe 事务

### 4.1 推荐：local checkpoint refs + file-reviewed transplant + 最后 switch 原主 worktree

理由：B 有 221 个后续 commit 和大量未提交 v1.22 源码；A 只有 1 个独有 checkpoint、A-only 资产/证据和 40 个 tracked 真冲突。直接把当前 B merge 到当前 A，会把 B 未提交 dirty 状态和 A dirty 状态混在一起，产生不可审查的冲突解决；直接把 A copy-over B 或 reset 则会丢失 A 本地状态。推荐按文件 review 后 transplant，再用新 branch 更新原主 worktree，同时保留 A/B 两个原 branch ref。

不推荐：
- wholesale ancestry merge（B HEAD 还不是完整 v1.22；40 tracked 真冲突需人工裁决）；
- `git reset --hard` / `git clean` / 整树 copy-over；
- 在 A dirty 状态未导出前 switch A；
- 把 A 子仓 commit 或 build outputs 带入主树；
- 删除或改写任一现有 ref、tag、remote、worktree。

若 root 最终强制要求保留 ancestry，fallback 方案是：A/B 各自先建 reviewed local checkpoint commit，再做 B-primary 的 reviewed merge commit；此方案冲突和回滚复杂度更高，不作为首选。

### 4.2 分阶段事务（本批只设计，不执行）

**Phase 0 — frozen evidence / preflight**
1. 确认 root 已停止 B 侧 Render Stats 写入和所有 build/game 进程；本 snapshot 期间 B status 仍在变化，最终交易前必须重新冻结。
2. 记录：
   `git show-ref`、`git worktree list --porcelain`、A/B `rev-parse HEAD`、A/B `status --porcelain=v2 -z`、A/B `diff --binary`、`ls-files -o --exclude-standard` 的 path/size/SHA-256 manifest。
3. 额外记录 A 子仓 `subprojects/StormBreaker` 的 HEAD、`git diff --binary`、untracked manifest；不提交、不删除。
4. 只读核对玩家 DLL size/SHA，禁止改玩家现场。
5. 确认 `/integration-audit` CSV/JSON 的 SHA，作为本 batch freeze 依据。

**Phase 1 — A local checkpoint（需 git mutation lease）**
1. 从 A HEAD 建 local checkpoint branch/ref（命名由 root 决定）。
2. 只 stage review 通过的 A-unique trackable paths：
   - 7 个 A-only committed docs/tests；
   - 12 个 A-only dirty tracked 文件中非 root-owned、非生成物；
   - root 批准的 A-only untracked source/test assets；
   - 不含 `AutoTest/artifacts/**`、`PlayerCrash/**`、`*.log`、DLL backup、build artifacts、submodule build outputs。
3. `git diff --check`，提交；记录 commit SHA。
4. A 的未提交剩余内容和子仓状态继续保留在外部 patch/manifest；不删原分支。

**Phase 2 — B authoritative checkpoint（需 git mutation lease）**
1. 从 B HEAD 建 local checkpoint branch/ref。
2. 将 B dirty 中 review 通过的 v1.22 source/test/docs 分组成可审查 commit；生成物/日志/证据/DLL backups 保持 untracked 外部保留。
3. 记录 B checkpoint commit SHA，确认原 B branch ref 仍指向 `ae890542...` 或 root 指定的新 checkpoint 由 root 审核。
4. B checkpoint 是 integration branch 的 source；不把 B dirty 全部 `git add -A`。

**Phase 3 — integration branch / transplant**
1. 从 B checkpoint 建 integration branch（例如 `codex/v1.22-main-tree-integration-20260920`，最终命名由 root 定）。
2. 对 A-unique patch series 先 `git apply --3way --check`，再按文件 apply；逐文件记录 source A blob、target pre/post blob、SHA-256。
3. 对 40 个 tracked 真冲突做 file-by-file review：
   - 默认取 B，除非确认 A hunk 仍适用且未被 B 后续 commit 覆盖；
   - 不用 `-X ours` / `-X theirs` 一把过；
   - root-owned `AGENTS.md`、changelogs、Meson、device、诊断桥、version 等路径遇到冲突即停，交 root 裁定。
4. 对 11 个 untracked-vs-untracked + 1 个 mixed 冲突按同一 review 规则；B-only 1020 个路径默认保留 B。
5. A-only file 新增时核对 exact bytes；确认不引入 ignored/生成物。
6. `subprojects/StormBreaker` 保持 B index gitlink `2b287f9`；A 的 `375e82e`/dirty 内容只存在于 export/backup。

**Phase 4 — verify before switch**
1. `git diff --check`、`git status --porcelain`、`git ls-files`，确认无 generated/evidence 被 staged。
2. 运行 `py AutoTest/test_audit_v122_tree_integration.py -v`（14 tests）和 integration audit（如 root 提供 checkpoint worktree）。
3. 核对：A 已批准路径 SHA 一致；B 原 ref/HEAD 不变；A/A 子仓 backup 存在；玩家 DLL SHA 不变；无 build/game 活动。
4. 任何失败留在 integration branch，不 switch 原主树。

**Phase 5 — switch original main worktree（需 root 明确批准）**
1. A dirty 状态已由 Phase 0/1 archive；A 的 unique untracked 资产先移入外部/临时保留区，避免 switch 冲突。
2. 在 `dxvk` worktree 执行 `git switch <integration-branch>`；禁止 `reset --hard` 或删除 A branch。
3. switch 后只恢复 root 批准的 A local assets；子仓按 root 决定，不默认递归更新。
4. A branch ref、B branch ref、原 worktree 列表、remote/tag 全部保留。

**Rollback**
- 保留 Phase 0 patch/manifest、A checkpoint ref、B checkpoint ref、integration branch ref。
- 若 switch 或验证失败，切回 A branch 并恢复 A dirty patch/untracked manifest；必要时使用 root 已验证的 `D:/WarVK-Backups/20260920-release-closeout-{A,B}`。
- 不删除 ref/backup；不 `gc`/`prune`。

## 5. 明确 blocker

| ID | severity | 内容 | 需要 root 裁定 |
| --- | --- | --- | --- |
| BLOCK-001 | blocker | A/B history 分歧：A 独有 1 commit，B 独有 221 commit；B 不是 A descendant | 选择首选 transplant + switch；若必须 ancestry，批准 fallback reviewed merge |
| BLOCK-002 | blocker | 40 tracked 真冲突 + 11 untracked/1 mixed dirty divergence | 逐文件默认 B；A hunk 需 review 批准 |
| BLOCK-003 | blocker | 73 个 A-only inventory、60 个 review_a_unique；含 native model light、dxvk audit headers、PlayerCrash、旧 inventory | 批准 A-unique 文件清单和外部保留清单 |
| BLOCK-004 | blocker | `subprojects/StormBreaker` A worktree `375e82e` + 26 mods + 66 untracked，B 为 `2b287f9` 干净 | 单独导出 A 子仓状态；B gitlink 保持权威 |
| BLOCK-005 | blocker | B 当前 HEAD `ae890542` = v1.21.00，B v1.22 源码仍在 dirty；B status 在并行 root 工作中继续变化 | 停止 B 写入并创建 B checkpoint commit 后重新 audit |
| BLOCK-006 | warning | 旧 2026-09-16 merge inventory 不是当前依据 | 使用本 batch JSON/CSV 与下次 frozen audit |
| BLOCK-007 | warning | 玩家 DLL FAC 正向反馈不是完整 release validation；当前有 `War3` 进程 | 本批不 build/deploy/game；后续独立 lease |
| BLOCK-008 | warning | root-owned AGENTS/Changelog/Meson/device/诊断桥/version 与 A dirty 交叉 | 由 root 直接裁决，integration lane 不修改 |

## 6. 可复现命令与产物

- 审计：
  `py AutoTest/audit_v122_tree_integration.py --output-dir AutoTest/artifacts/release-closeout-20260920/integration-audit --quiet`
- 临时 fixture 测试：
  `py AutoTest/test_audit_v122_tree_integration.py -v`（实际 14/14 OK）
- 当前 audit 产物（snapshot，最终报告会重新核对）：
  `v122-tree-integration-audit.json`
  `v122-tree-integration-files.csv`
- CSV 的 `route_hint`：`identical`、`keep_b`、`review_a_unique`、`review_a_change`、`review_conflict`、`review_head_divergence`、`exclude_generated`、`preserve_external`、`submodule_review`、`review_a_local` 等。

## 7. 未覆盖门与下一 bounded task

未覆盖：
- 未实际 commit / merge / reset / switch / worktree add；未修改 A 或 B dirty。
- 未逐 hunk review 40 个 tracked 冲突、11 个 untracked 冲突、87 个 committed head divergence。
- 未导出/验证 A 子仓 26 mods + 66 untracked。
- 未 clean build、未 Meson/static/CPU 重跑、未实机/GPU/视觉/性能验证；不声称 stable。
- 未做最终 main-tree integration、未部署 DLL、未 push/tag/release。

下一 bounded task（建议 `integration-release-r2`，需 root 授予相应 git mutation lease）：
1. root 停止 B 侧 Render Stats 写入并冻结 A/B；重新跑本审计并核对 SHA。
2. 只导出 A/B patch 与 untracked/submodule manifest，创建 local checkpoint refs。
3. 产出 40 tracked 冲突的逐文件裁决表和 A-unique patch series；root 审核后再创建 integration branch。
4. 本阶段仍不做 build/game/GPU、不部署 DLL、不 push/tag/release。
5. 注意：B 侧仍由 root 并行写入；本文件中的 B status/inventory/B-only 数字为较早 snapshot。最终以 `integration-audit/v122-tree-integration-audit.json` 的最新 frozen run 为准；本 r1 最终 audit snapshot 为 B status 922 / inventory 2991 / ignored 17179 / B-only 1023。

---

# R2 摘要：hunk 裁决、provenance 修正与 action list

- root UI build 完成、compiler lease 已释放；r2 在现有 work order 下恢复，仍无 production edit/Git write。
- Provenance 修正：A HEAD-only 共 7 个；5 个已存在于 B worktree（其中 3 个内容一致，`DEVELOPMENT_CHANGELOG.md` 为 root changelog 本地版本），2 个 absent from B worktree。审计工具新增 `summarize_a_head_provenance()`，测试 15/15。
- `d3d9_war3_aa.cpp` / `d3d9_war3_ssao.cpp` / `d3d9_war3_shadow_resources.cpp` / `d3d9_war3_volumetric_light.*`：A 的 barrier/初始化 hunk 不是 B 缺失，而是被 B `war3_owned_image_layout` 的 `plan/reset/commit` 事务式 layout state 覆盖；裁决 `A already ported / B supersedes`，Keep B。
- `war3_hook_lifecycle.h`：A 声明已迁移到 B `war3_native_capture.h` 并被 B 实际调用；裁决 `A already ported`。
- `dxvk_image.cpp`：historical candidate，未确认缺失 release fix。A 的 whole-image-init + transfer usage guard 可能拒绝 context 已处理的合法 partial-subresource migration；只登记 `do_not_apply_pending_root_gpu_contract_audit`，不应用。
- 52 个 `review_conflict` 的逐 path 裁决见 `integration-audit/r2-hunk-classification.json`；总体为 B-primary/root-owned/A-already-ported/deferred-research，不复制 A wholesale。
- StormBreaker nested archive manifest 已生成：`integration-audit/stormbreaker-nested-archive-manifest.json`（26 tracked modified + 66 untracked；A worktree `375e82e` vs index/B `2b287f9`）。建议 root 在 switch 前 CreateNew 外部 archive，逐字节保存 nested dirty state；本 lane 不执行 nested mutation。
- 详细证据、精确 hunk、root action list 见 `reports/integration-r2.md`。

**Root action list 摘要**
1. `src/dxvk/dxvk_image.cpp` relocation guard：先交 root GPU-contract audit；audit 前不应用、不 build。
2. 接受 AA/SSAO/shadow resources/volumetric `A already ported / B supersedes` 结论。
3. Root 自行处理 root-owned 冲突（AGENTS/changelog/Meson/device/version/诊断桥）。
4. A-only native model lights / image audit / research assets：仅 external backup，不导入生产。
5. CreateNew 执行 StormBreaker nested archive；本 lane 不执行。
6. B freeze 后重跑 audit；本 batch 不 checkpoint commit/switch。
- Root 已完成 CreateNew StormBreaker A 备份：`D:/WarVK-Backups/20260920-release-closeout-A-StormBreaker`；HEAD `375e82e3e4f901c52285c716bd090ea35fe8721c`；91 unique dirty/untracked nonignored paths；zip 490,864 B / SHA-256 `45DD3AF127D2C5475527A7E4558EF963D61494109860584242DE56282E4CB957`；root 已独立验证 CRC/all SHA/git status+head+diff/staged before-after、HEAD bundle。不得 recreate。
- Root mutation action manifest：`integration-audit/r2-root-mutation-action-manifest.json`（R2-ACT-001..005；R2-ACT-002 CreateNew A StormBreaker archive 已由 root 完成，不得 recreate）。

---

# R4 摘要：checkpoint 语义复核、switch 冲突矩阵与最小 root lease

- r4 独立复核 r3 的 794+6 路径：800 条全部存在、status 与当前 B 一致；无 binary/NUL/game asset/raw evidence/未复核 secret。审计前后仅 2 条白名单允许编辑文件变化，其余 798 条 SHA-256 不变。
- 语义决定：655 条 `checkpoint_repo_source_test_or_docs`、134 条 `checkpoint_with_root_owned_build_coordination`、5 条 `hold_autolight_pending_root_meson_decision`、6 条 `root_owned_do_not_stage`。
- Autolight hold：`src/d3d9/d3d9_native_light.cpp`、`war3_native_light_bridge.cpp/.h`、`war3_native_light_core.h`、`war3_native_light_policy.h`。当前 B dirty Meson 把前两者列入 `d3d9_src`，后三者通过 include 闭包进入 product；需 root 决定排除或明确接受 runtime opt-in。
- allocator risk：`dxvk_buffer.cpp`、`dxvk_memory.cpp`、`dxvk_buffer_allocation_guard.h` 属 failure-semantics product 依赖，需 root 确认不属 no allocator policy change 禁止项。
- A switch：当前 exact untracked collision 49 条（可由 `stash --include-untracked` 捕获）；ignored/case-fold/symlink/junction collision 当前均 0。合成测试证明 switch 会静默覆盖 ignored colliding file，stash 不保存 ignored 或 nested submodule dirty state。
- StormBreaker：A nested `.git` 是完整 directory，含 `index.lock`；A index gitlink 已等于 B target `2b287f9`。r3 的 external move + `submodule update` 未经离线证明，r4 建议不按原文 move；先保持 in place，clean nested checkout 另开 lease。
- Root backup 只读复核：`dirty-worktree.zip` 490,864 B / SHA-256 `45DD3AF127D2C5475527A7E4558EF963D61494109860584242DE56282E4CB957`；未 recreate。
- 最小 next lease：先由 root 裁定 R4-B1..B4，再授予单一 local Git mutation lease；preflight → B checkpoint/integration branch → A stash + switch without force → verify/rollback。禁 push/tag/release/force/reset/recursive move。
- 详细证据/命令/产物哈希见 `AutoTest/artifacts/release-closeout-20260920/reports/integration-r4.md` 与 `integration-audit/r4-audit/`。
