# 根因突破：静态阴影/path blocker 阴影的真正渲染消费点（2026-05-30）

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

## 根因（IDA 决定性证据）
`CWorld_TerrainShadow_Dispatch (0x6F7369B4)` 的 **case 0（主渲染路径）** 直接调用：
```c
Terrain_ShadowListA_Prepare(t);                  // 0x7376E0 排序/准备
TerrainShadow_ListA_RenderPreparedGroups(t);     // 0x7370A0 ★画 ListA 分组 stamp
TerrainShadow_RenderListB(t, 4, 0);              // 0x737400（已 hook）
TerrainShadow_RenderLayer(t, 1, 1, 0);           // 0x737620（已 hook，仅这个）
Terrain_ShadowListA_PrepareResources(t);
Terrain_ShadowListA_RenderAllEntries(t);         // 0x737110 ★画所有 ListA stamp
```

**关键发现**：
- 项目历史只 hook 了 `Terrain_RenderShadowLayer (0x737620)`，mode=1 只把它的
  `a3`(ListB) 关掉，`a2`(ListA) 保留。
- 但**真正画 doodad/建筑/path blocker 地面贴花阴影 stamp 的是**：
  - `TerrainShadow_ListA_RenderPreparedGroups (0x7370A0)`
  - `Terrain_ShadowListA_RenderAllEntries (0x737110)`
- 这两个在 case 0 里**直接调用**，**完全绕过我们所有的 hook**。
- 它们消费的是 `RegisterImageEntry`（ToggleStaticStampFromObject/EmitterStamp）
  注册进 stamp 池的条目，对每个 entry 调 `Terrain_ShadowListA_RenderEntryComplex`。

这解释了为什么：
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

## 治理方案（理论成立）
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

## 下一步
1. 加 hook `0x737110` + `0x7370A0`，mode>=1 时 return（配置开关 + Logger 确认）。
2. 保留 producer 端 ToggleStaticStampFromObject hook 作为兜底。
3. 实机验证：静态阴影 + path blocker 阴影是否消失，fog/border 是否正常。
