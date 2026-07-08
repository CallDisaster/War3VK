# 阴影管线整体讲解（2026-05-11）

本目录用于向用户系统性讲解：
- 项目整体架构（d3d9.dll 替身 + Warcraft III 逆向注入的是什么东西）
- 阴影管线 caster 侧的完整数据流（Warcraft III 游戏内存 → 我们捕获 → 我们自建 manifest → DXVK 真渲染）
- 当前视觉问题（大门闪、英雄/火凤凰/紫色单位阴影几乎不可见、pose 10FPS 感）的根因在管线的哪一段
- 为什么 Phase 7.20–7.31 这么多轮重构仍然没彻底修好
- Codex 最新裁决的 Producer Packet Takeover 方案为什么才是第一次真正对准根因的方案
- 多 Agent 协作规则

阅读顺序建议按编号来，每篇都是独立可读的：

| 顺序 | 文件 | 讲什么 |
|---|---|---|
| 01 | [01_project_overview.md](./01_project_overview.md) | 项目整体是一个什么东西，DXVK + War3 mod 是怎么长在一起的 |
| 02 | [02_shadow_pipeline_data_flow.md](./02_shadow_pipeline_data_flow.md) | 阴影管线 caster 侧完整数据流，附 Mermaid 图 |
| 03 | [03_palette_sources_deep_dive.md](./03_palette_sources_deep_dive.md) | Skinned palette 的五大来源，为什么 87% 走了不可信分支 |
| 04 | [04_manifest_lease_lifecycle.md](./04_manifest_lease_lifecycle.md) | VisibleRenderable → ShadowManifest → Core Set → Part Lease 的完整生命周期 |
| 05 | [05_root_cause_why_three_rounds_failed.md](./05_root_cause_why_three_rounds_failed.md) | 三轮重构为什么没修好，每轮的偏差和纠偏表 |
| 06 | [06_producer_packet_plan.md](./06_producer_packet_plan.md) | Codex 最新裁决：Producer Packet Takeover 为什么是真正的解 |
| 07 | [07_multi_agent_coordination.md](./07_multi_agent_coordination.md) | Kiro 的多 Agent 能力与本项目建议的任务切分 |

---

**一句话版本**：我们一直在 *消费端（shadow manifest / lease / core TTL）* 反复修表面，但真正坏掉的是 *生产端（skinned palette 数据源）* ——引擎把每个 renderablePart 完整的 48×N 字节 group-blended palette 写进 `0xBC6BD0` 全局调色板 arena，而我们的 `Hook_RuntimeMatrixWrite` 长期只缓存了**第一个矩阵**。这一个粒度错误，产生了连锁的三类视觉症状。
