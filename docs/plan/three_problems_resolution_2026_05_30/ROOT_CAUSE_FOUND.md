# 根因突破：静态阴影/path blocker 阴影的真正渲染消费点（2026-05-30）

> ⚠️ **2026-05-31 / 2026-07-08 复核勘误**：
> 本文的 `0x7370A0` / `0x737110` “ListA 是静态阴影消费点”结论已被 Phase 7.143
> 用户实机验证和 IDA 复核证伪。直接 hook 这两个函数会让所有悬崖/地形 tile
> 消失，并不能解决静态阴影/path blocker 残留。IDA 证据是
> `Terrain_ShadowListA_RenderAllEntries -> sub_6F725F80` 按
> `148 * (tileX + tileY * stride)` 取地形 tile 几何，随后提交
> `GxDevice_DrawIndexedRange`。因此本文仅作为失败路线归档；生产方向继续以
> `RegisterImageEntry` / `ToggleStaticStampFromObject` / `ShadowPath_StaticStamp_Toggle`
> 等 producer 端精确治理为准，避免再启用 ListA consumer 粗拦截。

## 用户反馈
"你的修复没有一个是生效的，问题全都是老问题。"

## 诊断过程（拿硬证据，不再猜）
1. 确认 DLL 被加载：`war3_d3d9.log` 时间 23:49 > 部署时间 23:28，DLL 是我的版本
   （`DXVK 1.1.0 x86 gcc 15.2.0`），含我的新字符串（`DoodadStaticStamp` 等）。
2. 确认 hook 安装：日志有 `DispatchToShape install result=ok`、`WidgetIdentity
   install result=ok`。
3. 确认 path blocker CSM 拦截在跑：EntryGate/Producer/FastAppend/AppendEntry
   全部命中 YTfb/YTpb/YTab 并 `return false`（不进 shadowCasters）。
4. **但用户仍看到 path blocker + 静态阴影。**

## 已证伪的旧根因判断（2026-05-30 当时结论）
`CWorld_TerrainShadow_Dispatch (0x6F7369B4)` 的 **case 0（主渲染路径）** 直接调用：
```c
Terrain_ShadowListA_Prepare(t);                  // 0x7376E0 排序/准备
TerrainShadow_ListA_RenderPreparedGroups(t);     // 0x7370A0 ★画 ListA 分组 stamp
TerrainShadow_RenderListB(t, 4, 0);              // 0x737400（已 hook）
TerrainShadow_RenderLayer(t, 1, 1, 0);           // 0x737620（已 hook，仅这个）
Terrain_ShadowListA_PrepareResources(t);
Terrain_ShadowListA_RenderAllEntries(t);         // 0x737110 ★画所有 ListA stamp
```

**当时的关键误判**：
- 项目历史只 hook 了 `Terrain_RenderShadowLayer (0x737620)`，mode=1 只把它的
  `a3`(ListB) 关掉，`a2`(ListA) 保留。
- 当时误判为“真正画 doodad/建筑/path blocker 地面贴花阴影 stamp”的是：
  - `TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)`
  - `Terrain_ShadowListA_RenderAllEntries (0x737110)`
- 2026-05-31 实机和 2026-07-08 IDA 复核已确认：这两个函数会渲染地形/悬崖
  tile 几何，不是可安全屏蔽的阴影专用 consumer。

这个旧解释已经废弃：
- 历史拦 RegisterImage/ListB/Projector 全无效——它们不是这条渲染消费路径；
- 我新加的 ToggleStaticStampFromObject hook 即使拦了 NEW 注册，地图预置 doodad
  在 hook 之前已经注册进池，ListA render 仍然把池里的全画出来。

## 验证的渲染链
```
producer（注册）: CDoodads → ToggleStaticStampFromObject/EmitterStamp
                   → RegisterImageEntry (0x713250) → 写入 ListA stamp 池
consumer（渲染）: CWorld_TerrainShadow_Dispatch case0
                   → ListA_RenderPreparedGroups (0x7370A0)  ← 没人 hook
                   → ShadowListA_RenderAllEntries (0x737110) ← 没人 hook
                   → Terrain_ShadowListA_RenderEntryComplex（逐条画 stamp）
```

## 已废弃治理方案（实测失败）
**在 consumer 端拦截**，比 producer 端更彻底（覆盖预置 + 运行时所有 stamp）：
- hook `CWorld_TerrainShadow_Dispatch (0x6F7369B4)`，mode>=1 时在 case 0 跳过
  `ListA_RenderPreparedGroups` + `ShadowListA_RenderAllEntries`（doodad 贴花阴影），
  保留 fog/border/visibility 路径。
- 或更简单：直接 hook `Terrain_ShadowListA_RenderAllEntries (0x737110)` +
  `TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)`，mode>=1 时直接 return
  （不画任何 ListA stamp）。这是最干净的单点拦截。

风险：ListA 是否含 fog/border？从 dispatch 看，fog/visibility 走独立的
FogMask grid（terrain tile 着色消费），ListA 是专门的 shadow stamp 池。
但 doc 24 §8.2 历史提到"拦 ListA 出现边界条纹"——需要实测确认拦哪个粒度安全。
先用最干净的 0x737110 + 0x7370A0 双 hook + 配置开关，可一键 A/B。

## 历史下一步（禁止照做）
1. 加 hook `0x737110` + `0x7370A0`，mode>=1 时 return（配置开关 + Logger 确认）。
2. 保留 producer 端 ToggleStaticStampFromObject hook 作为兜底。
3. 实机验证：静态阴影 + path blocker 阴影是否消失，fog/border 是否正常。
