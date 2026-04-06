# War3 Native 渲染链还原（ASM 基线）

## 说明
1. `src/d3d9/war3/native/` 仅用于原版链路还原参考，不参与当前功能渲染。
2. 还原标准为 ASM（IDA MCP），不以伪 C 作为事实来源。
3. 当前目录中的 `war3_native_renderer.cpp` 已更新为“可对照 ASM 的主链基线”，仍包含少量依赖全局单例的占位分支。

## 已校验主链地址（Game.dll 1.27，base=0x6F000000）
1. `CWorld_RenderScene` `0x6F3681C0`
2. `CWorld_DispatchStage` `0x6F363020`
3. `CWorld_WorldObjects_RenderGroup` `0x6F368E30`
4. `WorldObjectEntry_Render` `0x6F184EE0`
5. `RenderQueue_AddBatch` `0x6F139190`
6. `RenderBatch_Submit` `0x6F1375C0`
7. `RenderQueue_FlushSortedItems` `0x6F1380A0`
8. `RenderQueue_FlushAndReset` `0x6F139800`
9. `RenderQueue_Dispatch_Common` `0x6F13A5E0`
10. `RenderQueue_Dispatch_Special` `0x6F13A780`

## 当前还原进度
1. 已完成：
   - `Native_CWorld_RenderScene` 阶段顺序、两次 flush、shadow cast 开关时序按 ASM 重排。
   - `Native_RenderWorld_DispatchStage` 的主 switch（0..21）按 ASM 映射重建。
   - `DispatchStage case16/18/21` 已按 ASM 的真实调用链补全（RVA 解析函数 + 全局地址访问）。
   - `Native_WorldObjects_RenderGroup` 与 `Native_WorldObjectEntry_Render` 按实际 entry stride / 调用约定重建。
   - `war3_native_renderer_core.cpp` / `war3_native_symbols.cpp` 已纳入 `src/d3d9/war3/native/meson.build`。
2. 未完成：
   - `case16/21` 的运行态稳定性验证（不同版本地址可能漂移）。
   - `RenderQueue_AddBatch` / `RenderBatch_Submit` 仍是“部分还原 + 注释对照”状态。
3. 已做基础编译核验：
   - `war3_native_renderer.cpp`
   - `war3_native_renderer_core.cpp`
   - `war3_native_shadow.cpp`
   - `war3_native_symbols.cpp`
   已通过 `g++ -std=gnu++17 -fsyntax-only` 级别语法检查。

## 引擎域视角（2026-04-04）
1. 世界帧域：
   - `CWorldFrameWar3 -> RenderScene -> DispatchStage -> TerrainShadow / WorldObjects / Flush`
   - 状态：主链清晰，stage16/18/21 仍有尾链全局依赖待补齐
2. 提交队列域：
   - `SceneNode -> RenderBatch_Submit -> RenderQueue_FlushSortedItems -> Dispatch_Common/Special`
   - 状态：排序/flags/stage update 已确认；dispatch block 结构仍未完全落地
3. 模型动画域：
   - `CSprite / CSpriteUber -> CModelAnimController -> CAnimComplex -> SequenceProvider`
   - 状态：对象骨架已进入 public/native 头
4. 附着特效域：
   - `CreateAttachedEffect -> CAttachedEffect_Init -> CSprite_AttachModelToPoint`
   - 状态：主链清晰，骨骼/挂点表仍待继续展开
5. UI 3D 域：
   - `CGameUI / cursor_sprite / portrait_button / world_frame_war3`
   - 状态：组合关系明确，但仍缺一页独立 native 专题
6. 交付阅读顺序：
   - 推荐 `17_cworldframewar3_full_reverse -> 18_csprite_animation_attach_reverse -> 本页 -> 19_blizzard_native_rendering_engine_full_perspective`

## 主调度时序（已校验）
1. 预处理：`StateCleanup`(world+338/+33C/+354)，清空 world+660/+664。
2. 阶段序列：
   - 段1：`0(条件)` -> `1` -> `13` -> `FlushAndReset`
   - 段2：`19,9,2,3,8,(17条件),14,5,10,(12条件),11` -> `FlushAndReset`
   - 段3：`4,7,6,20`
   - `activeQueue==0` 追加：`15,18,21`
3. 末尾：恢复 category/mode 状态机并 flush cleanupContext。

## 后续建议
1. 在本目录维护 `address_book/README.md`（地址 + 调用约定 + 参数语义）并持续补全。
2. 先补 `RenderQueue_AddBatch` 的递归子节点与四类透明列表分流，再补 `RenderQueue_StageUpdate` 细节。
3. 任何推测字段必须标注“未校验”，并在文档里挂上对应 ASM 地址。

## 相关研究文档
1. `docs/research/war3_render_issues/native/README.md`
2. `docs/research/war3_render_issues/01_batch_merge/README.md`
3. `docs/research/war3_render_issues/02_los_blocker_shadow/README.md`
4. `docs/research/war3_render_issues/03_building_static_shadow/README.md`
