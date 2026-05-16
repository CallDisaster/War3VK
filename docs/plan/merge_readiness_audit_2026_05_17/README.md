# 合并提交准备度审查（2026-05-17 凌晨）

## 审查目的

用户提出"是否接近合并提交"，本文档基于当前 `codex/war3-stable-producer-baseline` 分支
（领先 `main` 69 个 commit、+12,322 / -1,022 行、87 个文件改动）做一次系统性审查，
回答：**距离合并还差什么？**

## 1. 性能护栏（PASS）

实测验证（`AutoTest/_phase800_dual_perf.py`，5 轮）：

| 场景 | 目标 | 实测均值 | 富裕度 | 结论 |
|---|---|---|---|---|
| 高压地图 (光影测试-高压) | ≥85 FPS | **86.4 FPS**（4 轮: 87.57 / 88.31 / 87.89 / 88.39） | +1.4 ~ +3.4 FPS | PASS（紧贴护栏） |
| 低压地图 (光影测试) | ≥120 FPS | **138.9 FPS**（4 轮: 139.51 / 139.38 / 138.63 / 139.37） | +18.6 ~ +19.5 FPS | PASS（充裕） |

**风险**：高压场景富裕度只有 ~3 FPS，性能波动易触底。建议合并前在前台环境（非
isolated desktop）验证一次基线。

## 2. 编译警告（PASS）

Phase 7.114 已清理 3 个 d3d9_device.cpp 内部 warning：
- `War3SemanticBuildWorldPaletteIfNeeded` (unused-function) → `[[maybe_unused]]`
- `War3SemanticRenderableHasBuildingFlags` (unused-function) → `[[maybe_unused]]`
- `alphaTestPayloadApplied` (unused-but-set-variable) → `[[maybe_unused]]`

**剩余**：`war3_game_struct.h:OPCode` 5 个 `-Wreorder` warning，与本分支无关
（jass 历史代码，初始化列表顺序与字段声明顺序不一致）。**非阻塞**。

## 3. 文件结构（屎山警告）

按 LOC 排序的关键文件：

| 文件 | 行数 | KB | 评级 |
|---|---|---|---|
| `src/d3d9/d3d9_device.cpp` | **26,685** | 1093 | 🔴 严重屎山 |
| `src/d3d9/war3/model/war3_model_hook.cpp` | **10,700** | 474 | 🔴 严重屎山 |
| `src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp` | **6,148** | 326 | 🔴 严重屎山 |
| `src/d3d9/d3d9_war3_shadow.cpp` | 4,541 | 188 | 🟡 偏大 |
| `src/d3d9/war3/tools/war3_control_plane.cpp` | 4,505 | 240 | 🟡 偏大 |
| `src/d3d9/war3/render/war3_current_draw_contract.cpp` | 2,334 | 110 | 🟡 偏大 |
| `src/d3d9/d3d9_device.h` | 2,203 | 83 | 🟡 偏大 |
| `src/d3d9/war3/hooks/war3_hook_shadow.cpp` | 1,373 | 58 | 🟢 合理 |
| `src/d3d9/war3/core/war3_internal_test_config.h` | 1,300 | 76 | 🟢 合理 |
| `src/d3d9/d3d9_war3_pipeline.cpp` | 886 | 40 | 🟢 合理 |

### 3.1 d3d9_device.cpp 拆分建议

26,685 行中包含 **108 个 D3D9DeviceEx 成员函数**。按主题拆分建议：

- `d3d9_device_war3_shadow_capture.cpp`（~6,000 行）：`War3TryCaptureShadowCaster` +
  `War3TryCaptureShadowCasterDrawIndexed` + `War3TryCaptureShadowCasterDrawNonIndexed` +
  v4 GPU copy + EarlyBypass。
- `d3d9_device_war3_semantic_scene.cpp`（~5,500 行）：`War3TryPopulateSemanticShadowScene` +
  `War3TryPopulateDirectCurrentDrawGrouped` + `War3TryAppendSemanticShadowPacket` +
  各路 producer/fast-append。
- `d3d9_device_war3_alpha_test.cpp`（~700 行）：alpha-test lane payload cache。
- `d3d9_device_war3_helpers.cpp`（~3,000 行）：`War3SemanticBuildShadowSemanticContext` +
  `War3ShadowIsLosBlocker` + 各种 helper 自由函数。
- `d3d9_device_war3_palette.cpp`（~3,000 行）：`War3GetOrCreateSemanticShadowPalette` +
  `War3TryBuildLiveRuntimeGroupPalette` + palette 相关。
- `d3d9_device.cpp`（剩余 ~8,500 行）：纯 D3D9 API 实现 + 主框架。

### 3.2 war3_model_hook.cpp 拆分建议

10,700 行中包含十几个 hook + writer + registry，按主题拆：

- `war3_model_hook_runtime_pose.cpp`：RuntimeMatrixWrite + RangeCopy + PoseRegistry publisher
- `war3_model_hook_palette_writers.cpp`：0x12FED0 / 0x12FF90 / 0x12E600 capture
- `war3_model_hook_sprite_uber.cpp`：4 个 PreRender 变体
- `war3_model_hook_widget_identity.cpp`：CWidget_RegisterFootprintAndShadowMask hook
- `war3_model_hook_lifecycle.cpp`：CreateSpriteAndBindSourceObject + CModel ctor

### 3.3 war3_shadow_runtime_bridge.cpp 拆分建议

6,148 行做的事情过多：
- shadow scene stats publish/aggregate
- full trace writer (shadow_pose_full_trace JSONL)
- control plane summary 数据透传
- shadow runtime bridge 主体

应至少拆为 `bridge_summary.cpp` / `bridge_full_trace.cpp` / `bridge_aggregate.cpp`。

## 4. 代码注释卫生（屎山病征）

`war3_current_draw_contract.cpp` 与 `d3d9_device.cpp` 包含大量 phase 历史注释，
例：

```cpp
// Phase 7.30 Action B：attribution-key 专用 palette snapshot 通路。
// Phase 7.34 第三轮：trusted snapshot 保留次数。
// Phase 7.55 v4 + Phase 7.57 producer 已闭环 stutter 根因。
```

**问题**：这些是开发过程的"考古挖掘记录"，不应该留在生产代码里。合并前应该
把这些注释精简成"当前实际行为"的描述（保留 1 行原因说明，不再罗列 7.30/7.34/7.55
phase 编号）。

**建议**：合并前做一次 `find . -name '*.cpp' -exec grep -l 'Phase 7\.' {} \;` 
扫描，逐文件清理。约 30+ 文件需要清理。

## 5. 临时诊断 atomic counter（应分级关闭）

当前代码中保留了大量"诊断 counter"：
- `g_paletteCaptureTrustedSourceHitCount`
- `g_paletteRejectedNoTrustedSourceCount`
- `g_submitPaletteFrameLag*`
- `g_writeMaskRegionFromBuildingStampCount`
- 等等

**问题**：每帧 enter hook 都会 `fetch_add` 这些 counter，即使 reject 路径默认关闭。
单 atomic add ~5ns，但 hook 入口 6300+ 次/帧 × 多个 counter = 累计 ~200μs/帧 占用。

**建议**：合并前把所有"phase 历史诊断 counter"按统一 env 开关收口
（`DXVK_WAR3_DIAGNOSTICS_DETAILED=1` 启用全部，默认 0）。Phase 7.113 已示范一例
（caller-aware diagnostics）。

## 6. AutoTest 临时脚本（待归档）

`AutoTest/` 下有 50+ 个 `_phase7XX_*.py` / `_analyze_*.py` 临时脚本，许多已经
不再使用。合并前应该：
- 保留 5-10 个稳定的 regression suite 脚本（dual_perf / append_survey / 
  static_shadow_probe）
- 把其他归档到 `AutoTest/_archive/` 子目录
- 写一个 `AutoTest/README.md` 说明哪些是发布级，哪些是历史调试

## 7. .git 仓库健康

- 当前分支 `codex/war3-stable-producer-baseline` 领先 main 69 个 commit
- 建议合并前 squash 成 ~10 个主题 commit：
  1. shadow caster pipeline foundation (Phase 7.30-7.50)
  2. draw-time VB GPU copy (Phase 7.55 v4)
  3. AlphaTest lane (Phase 7.55 alpha)
  4. path blocker filter (Phase 7.72-7.110)
  5. ManifestCopy early-skip + ResolveGeoset cache (Phase 7.99)
  6. RecordSpriteHostOwnerBinding disable (Phase 7.105)
  7. shared_mutex / atomic / thread_local 性能批 (Phase 7.76-7.86)
  8. shadow.frag / receiver.frag 视觉调整 (Phase 7.66-7.69)
  9. WriteMaskRegion + ShadowProjector caller-aware diagnostics (Phase 7.108-7.112)
  10. AGENTS.md / docs / autotest cleanup

## 8. 合并前必做 checklist

- [ ] 高压地图前台环境复测（确认 isolated desktop ~88 FPS 不是机器噪声）
- [ ] 至少 60s × 3 轮的 dual_perf 长窗口验证
- [ ] path blocker 视觉残留：用户在编辑器里确认实物 fourcc，加入 `kPathBlockerFourCCs`
- [ ] 静态阴影屏蔽：等待 `BoxFastpath/PolyFastpath` 拦截路线 IDA 复核完成
- [ ] 扫一遍 `Phase 7.X` 注释，把"考古记录"删掉，只留"为什么这么写"
- [ ] 收口所有诊断 counter 在统一 env 开关下
- [ ] 拆分 d3d9_device.cpp（最后一步，最大风险）
- [ ] AutoTest 归档 + README
- [ ] 69 commit squash 为 ~10 个主题 commit + 写 changelog

## 9. 距离合并的真实工程量评估

| 阶段 | 工作 | 工时 | 风险 |
|---|---|---|---|
| A. path blocker 视觉残留收尾 | 用户给具体 fourcc → 加黑名单 | 30 分钟 | 低 |
| B. 静态阴影 BoxFastpath 拦截 | IDA 逆向 + 实施 + 视觉验证 | 8-16 小时 | 中 |
| C. 注释清理 | 30+ 文件批量 | 4-8 小时 | 低 |
| D. 诊断 counter 收口 | 统一 env 开关 | 4-6 小时 | 低 |
| E. AutoTest 归档 | 移动 + README | 2 小时 | 极低 |
| F. d3d9_device.cpp 拆分 | 按主题切到 6 个文件 | 16-24 小时 | 高 |
| G. squash + changelog | 重写 commit history | 4-8 小时 | 中 |
| H. 多场景多机器复测 | 长窗口 baseline 锁定 | 8 小时（含等待） | 低 |

**乐观估计**：~50 工时（约 1 周全职）
**悲观估计**：~120 工时（约 2-3 周）

## 10. 可立即合并的"最小可行版本"

如果用户希望尽快合并一个**可工作版本**而非完美版本，最小工作清单：

1. ✅ 当前性能护栏（PASS）
2. ✅ 编译 0 warning（除 OPCode 历史 warning）
3. ⏳ path blocker 视觉残留（已知问题，可用 readme 备注遗留）
4. ⏳ 静态阴影屏蔽（research 完成 60%，实施 0%，备注遗留）
5. ⏳ commit squash（必做，否则 review 体验差）

**Squash 后合并**：约 8 工时（一个工作日）。

---

*生成时间：2026-05-17 05:30，基于 commit fba882c (Phase 7.113)。*
