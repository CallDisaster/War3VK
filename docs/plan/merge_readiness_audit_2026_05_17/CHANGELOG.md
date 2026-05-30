# DXVK War3 fork — Phase 7 累计变更清单（2026-05-18 凌晨整理）

本文件按主题汇总 `codex/war3-stable-producer-baseline` 分支自 `main` 分叉以来的
所有重要变更，作为 squash 前的"预排版"。每个主题下列出对应 phase 编号 + 一句话
描述，便于后续撰写 squash commit message。

---

## 主题 1 — Shadow Caster 渲染管线骨架（Phase 7.30 ~ 7.50）

引入 semantic-driven shadow producer/consumer 路线，替代原 legacy ShadowCapture
作为视觉主路径。

- 7.30 ~ 7.34：Manifest / lease / palette provenance 仲裁、phantom shrink
- 7.35：Submit-side live palette rebuild 探针（PoseRegistry fallback）
- 7.46 ~ 7.49：renderablePart palette snapshot + per-publish provenance probe
- 7.50：Live rebuild on Resolve fail（最终被 7.51-7.55 v4 替代）

## 主题 2 — Draw-time VB/IB GPU copy（Phase 7.55 v4）

CPU skinned 顶点直接走 GPU copy 给 shadow caster，绕开 palette cadence 问题。
后续追加 dedup（7.70）、UV/AlphaTest 接入（7.55+）。

- 7.55：v4 GPU copy 通路打通 + frustum cull 修复
- 7.70：同帧 capture fingerprint dedup（avoid 重复 EmitCs(copyBuffer)）
- 7.123：per-frame GPU buffer alloc budget（首帧暴降修复）

## 主题 3 — AlphaTest Lane（Phase 7.55 alpha）

让 `alpha-blend + UV + diffuseTexture` 物体 promote 成 alpha-test shadow，
解决"树叶/栅栏在阴影里变方块"。

- 7.55 alpha：promote logic + UV stream 复制 + descriptor 派生
- 7.67 ~ 7.69：tree shadow TAA stabilization（hash anchor + history weight）

## 主题 4 — Path Blocker 视觉屏蔽（Phase 7.72 ~ 7.124）

8 个 YT?? fourcc 黑名单 + 多层入口 reject + 视觉/统计验证。

- 7.72：unitPtr 兜底 + widget identity cache write-through
- 7.99：dxvk log reject 日志（前 30 个 unique fourcc 各 1 行）
- 7.107：destructible v4 capture path hard-read fallback
- 7.108 ~ 7.110：fourcc 黑名单统一到 `kPathBlockerFourCCs`
- 7.112 ~ 7.116：caller-aware WriteMaskRegion / DispatchToShape 诊断（实测推翻）
- 7.122：path blocker entry gate at `War3TryCaptureShadowCaster` 函数最入口
- 7.124：entry gate fast-path（廉价 currentObj/handle 优先，慢路径只对 rawcode=0）

## 主题 4b — 静态阴影 / Path Blocker 真正根因（Phase 7.143，2026-05-30）⭐

**这是 2026-05-30 的决定性突破，纠正了主题 4 + 历史所有静态阴影拦截的方向。**

用户反馈"所有静态阴影/path blocker 拦截全部无效"。根因（IDA 反编译 `CWorld_
TerrainShadow_Dispatch 0x6F7369B4` case0 确认）：

- 真正画 doodad/建筑/path blocker 地面贴花阴影 stamp 的是
  `TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)` 和
  `Terrain_ShadowListA_RenderAllEntries (0x737110)`，它们在 dispatch case0 里
  **直接调用，绕过项目所有现有 hook**（历史只 hook 了 Terrain_RenderShadowLayer
  0x737620，那只是另一个子调用）。
- 历史拦截全在 producer（RegisterImage/StaticStamp）或错误 consumer（ListB/
  RenderLayer/Projector），从没拦到这两个真正的 stamp 渲染消费点。
- xrefs 确认这两个函数仅被 Dispatch 调用，`RenderEntryComplex` 内部纯
  `GxDevice_DrawIndexedRange` 画 stamp 几何，不涉及 fog/visibility/border。

修复：mode>=1 时 hook 这两个函数直接 return。
- 配置：`kNativeShadowListARenderHookEnabled` / `kNativeShadowBlockListARenderWhenMode1`
- 兜底：保留 CDoodads `ToggleStaticStampFromObject (0x74DB30)` producer hook
- 安装日志用 `Logger::info` 写进 war3_d3d9.log（验证 hook 安装的正确通道）

**经验教训**：(1) `war3dbg::Print` 走 DebugView 不进日志文件，验证 hook 必须用
`Logger::info`；(2) 拦 producer 注册 ≠ 阻止 consumer 渲染（预置对象已注册）；
(3) 一个渲染系统多个 render 入口，必须 xrefs 找全所有 consumer。

## 主题 5 — 12-人对战图开局卡顿修复（Phase 7.99 + 7.105）

map load 期间引擎重 widget create cadence 导致 main thread 阻塞 4-10 秒。

- 7.99：ManifestCopy early-skip（99.9% skip 率，从 2493 → 2 / 30s）
- 7.99：ResolveGeoset cache（256 槽 thread_local）
- 7.105：默认禁用 `RecordSpriteHostOwnerBinding`（可 env=0 恢复）

效果：4-人 96 → 138 FPS, 12-人 25.5 → 126 FPS。

## 主题 6 — Performance: shared_mutex / atomic / thread_local（Phase 7.76 ~ 7.86）

把全局 mutex 改 shared_mutex（reader 走 shared_lock），把 frame counter 改
atomic，把 per-frame scratch 改 thread_local 复用。

- 7.76：DirectPacketGeosetCache shared_mutex
- 7.79：runtimePoseArrayRange shared_mutex
- 7.80：HydrateVisibleSnapshot partCache thread_local
- 7.81：SnapshotPublishedCurrentDrawContracts dedupe index thread_local
- 7.82：StaticMeshDataResourceCacheMutex shared_mutex
- 7.83：5 个 registry 全部 shared_mutex
- 7.84：frameTag/frameNumber 双读合并
- 7.85：previousSubmittedObjectIdentityKeys / PartIdentityKeys 改 const ref
- 7.86：shadow scene stats shared_mutex

## 主题 7 — Performance: hot-path 数据结构合批（Phase 7.117 ~ 7.121）

- 7.117：AutoTest 130+ phase 临时脚本归档到 `_archive/`
- 7.118：BuildShadowReplayDraws thread_local + scene-tuple cache
- 7.119：previousSubmittedSelectionKeys 改 const ref
- 7.120：stableEligibleRecords swap-copy 改 in-place `std::remove_if`
- 7.121：m_war3SemanticPaletteCache hash index（O(N) → O(1)）

## 主题 8 — Diagnostics 三剑客（Phase 7.42 ~ 7.51）

shadow_pose_full_trace + 各路 atomic counter + control plane summary 透传。

## 主题 9 — Visual Sharpness（shader 调整）

- CSM maxDistance 8000 → 4000（视野内细节锐化）
- PCF radius 0.95 → 0.70
- AlphaShadow hashed/mip 默认关，硬阈值

## 主题 10 — Documentation

- 论文 `docs/plan/overnight_render_paper_2026_05_15/` 5 章主链
- IDA rename + comment 累计 80+ 处
- AGENTS.md 完整历史记录

---

## Squash 建议（10 个主题 commit）

按上面 10 个主题各做 1 个 squash commit + 1 个收尾 docs commit。

每个 squash commit 的 description 应包含：
1. 一句话目标
2. 关键代码修改清单（3-5 个文件）
3. 性能/视觉验证证据（FPS 数字、trace 路径）
4. 可回退路径（env / runtime config flag）

---

## 合并前必跑验证

```pwsh
py AutoTest\dual_perf_baseline.py
```

护栏：
- 高压地图 ≥ 85 FPS
- 低压地图 ≥ 120 FPS

最近 4 轮记录（凌晨 03:00 - 04:30）：
| Phase | 高压 FPS | 低压 FPS |
|---|---|---|
| 7.119 | 88.33 | 137.06 |
| 7.120 | 90.98 | 142.84 |
| 7.121 | 90.77 | 141.58 |
| 7.122/7.123 | 85.66 | 145.97 |
| 7.124 | 86.59 | 141.69 |

---

*生成时间: 2026-05-18 04:30*
