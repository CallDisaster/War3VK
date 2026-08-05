# 子线程 F 任务卡 — FogMask grid + 静态阴影 type code 完整逆向

## 任务定位
渲染论文第 6 章 + **静态阴影主线**深化。
24 文档 v3 已经定位到 `TerrainShadow_WriteMaskRegion (0x234710)` 是真正的建筑阴影写入函数，
但 16-bit `typeCode` 的具体 bit 含义还没拆开。本子线程要把 type code 的每个 bit 逐一逆向。

## 已知锚点
| 地址 | 名字 |
|---|---|
| `0x6F234710` | `TerrainShadow_WriteMaskRegion` |
| `0x6F234620` | `TerrainShadow_WriteMaskRegion_ForObject` |
| `0x6F3DB260` | `TerrainShadow_WriteMaskRegion_FromActorRuntime` |
| `0x6F233E90` | `TerrainShadow_RebuildMaskFromObjectLists` |
| `0x6F232060` | `CFogMaskTable_GetOrCreateMask` |
| `0x6F230210` | `CFogMask_BuildNodeAndRangeTable` |
| `0x6F230F40` | `CFogOfWarMap_Ctor` |
| `0x6F231CD0` | `CFogOfWarMap_DumpDebugLog` |
| `0x6F2324F0` | `CFogOfWarMap_LoadState` |
| `0x6F232840` | `CFogOfWarMap_UpdateVisibilityMask` |
| `0x6F2330D0` | `CFogOfWarMap_SaveState` |
| `0x6F233DF0` | `CFogOfWarMap_BuildVisibilityMask` |
| `0x6F65A140` | `CWidget_RegisterFootprintAndShadowMask`（30+ caller） |
| `0x6F514F40` | `CUnit_StampBuildingShadowFootprint` |
| `0x6F66C930` | `CUnitUIManager_DispatchFootprintWrite` |
| `0x6F41B380` | `Actor_RuntimeShadowMaskWriter` |

## 已有研究（增补，不重写）
- `docs/research/war3_render_issues/24_cdoodads_static_shadow_upstream/README.md` v3
- AGENTS.md 第 14、43、57 条（历次 RegisterImage 拦截尝试）

## 必须搞清楚的问题（本任务的硬目标）

### 6.1 `WriteMaskRegion` 的 a3 typeCode 16-bit 含义表
按反编译 `0x234710` 内部对 `a3 / v123 / v116 / v117 / a5` 等参数的使用，
列出每个 bit 的含义。需要交叉对照：
- `CFogOfWarMap_BuildVisibilityMask (0x233DF0)` 看 visibility 用哪些 bit
- `CFogOfWarMap_UpdateVisibilityMask (0x232840)` 看 fog 更新用哪些 bit
- `CWidget_RegisterFootprintAndShadowMask (0x65A140)` 看 unit footprint 用哪些 bit
- `CUnit_StampBuildingShadowFootprint (0x514F40)` 看 building shadow 用哪些 bit
- `Actor_RuntimeShadowMaskWriter (0x3DB260)` 看 actor runtime 用哪些 bit

理想输出：
| bit | 含义（推断） | 证据来源 |
|---|---|---|
| 0 | fog-of-war | `0x232840` |
| 1 | line-of-sight | ? |
| ... | ... | ... |
| 9 | building shadow footprint | `0x514F40` 的 magic 0x2B5DB42C 后 OR 操作 |
| 10-15 | elevation / height（高 7 bit） | `WriteMaskRegion` 内 `((v23 + 8256) & 0xFF80) - 0x2000` |

### 6.2 4 个并行 mask layer（this+11/+12/+14/+15）的语义
`WriteMaskRegion` 反编译里 `v5[11/12/14/15]` 是 4 个 `_WORD*`，分别指向 4 个并行 mask grid。
确认每个 grid 的用途（一般 grid 0 是 fog mask、grid 1 是 visibility mask、grid 2 是 path/blocker mask、grid 3 是 shadow/extra mask）。

### 6.3 `magic 0x2B5DB42C` 的含义
`CUnit_StampBuildingShadowFootprint (0x514F40)` 验证 `*(widget+0x0C) == 0x2B5DB42C`。
这是什么 magic？是 CWidget 类型签名吗？哪些类型的对象会有这个 magic？
（如果只有 `Building` 类型会有这个 magic，那就是建筑阴影 footprint 的天然分流点）

### 6.4 整体重建路径 `RebuildMaskFromObjectLists (0x233E90)`
反编译这个函数，看它会遍历哪些对象列表 + 用什么 type code 重建 mask。
hook 必须考虑这条路径，否则 loadgame / 视野同步会覆盖单点拦截。

### 6.5 `CWidget_RegisterFootprintAndShadowMask (0x65A140)` 的 30+ caller 分析
列出所有 caller 的语义（建筑创建？销毁？移动？状态变化？升级？变形？）
按语义分桶，确认建筑 footprint 是否只在某几个 caller 路径写入。

### 6.6 静态阴影治理蓝图（最终交付物）
基于 type code bit 含义表，给出：
- **方案 A**（最干净）：hook `WriteMaskRegion`，在调 trampoline 前 `a3 &= ~kBuildingShadowBit`；
- **方案 B**（保险）：同时 hook `RebuildMaskFromObjectLists`，按对象类型过滤；
- **方案 C**（开关）：通过 `CWidget+0x0C` 的 magic 判定，仅对 Building widget 屏蔽；
- 给主线程一份**可直接落地的 hook 接线建议**（不要自己改源码）。

## 输出格式
写到 `docs/plan/overnight_render_paper_2026_05_15/06_fogmask_static_shadow.md`：

```
# 第 6 章 — FogMask 共享 mask grid 与静态阴影治理

## 6.1 数据结构（CFogMask / CFogMaskTable / CFogOfWarMap 字段表）
## 6.2 4 个并行 mask layer 的语义
## 6.3 16-bit typeCode 完整含义表
## 6.4 整体重建路径
## 6.5 中央 footprint registry (0x65A140) 的 caller 分桶
## 6.6 magic 0x2B5DB42C（CWidget 类型签名）
## 6.7 静态阴影治理蓝图（方案 A/B/C）
## 6.8 项目历史拦截尝试反证（为什么失败）
## 6.9 IDA rename / set_comments 建议
```

## 工具
- IDA MCP
- 项目代码（**只读**）：
  - `src/d3d9/war3/hooks/war3_hook_shadow.{h,cpp}`
  - `src/d3d9/war3/hooks/war3_shadow_filter_policy.{h,cpp}`

## 不能做的事
- 不动源码
- 不启动 War3
- IDA 写回交主线程

## 完成条件
- typeCode 16-bit 含义表必须**全部**填出（不能留 ?）
- 至少 1000 行 markdown
- 至少 30 处新命名建议
- 静态阴影治理蓝图要可直接落地
