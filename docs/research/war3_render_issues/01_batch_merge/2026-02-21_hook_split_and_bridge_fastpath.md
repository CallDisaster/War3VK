# 2026-02-21：Hook 拆分与 Bridge 快路径实验记录

## 1. 背景
- `d3d9_war3_hook.cpp` 仍承担大量阴影 Hook 逻辑，编译和定位成本高。
- `ExecBatchProcessor::Begin` 在高频路径仍有多级查找（SceneNode -> Tracker -> Registry -> TLS Fallback）。

## 2. 本轮落地
### 2.1 阴影域物理拆分
- 新增：
  - `src/d3d9/war3/hooks/war3_hook_shadow.h`
  - `src/d3d9/war3/hooks/war3_hook_shadow.cpp`
- 迁移内容：
  - `Terrain_RenderShadowLayer / RenderListA / RenderListB`
  - `ShadowUpdate_WriteEntry`
  - `ShadowProjector_Add_FromObject / Add_Simple`
  - 阴影域所需 helper 与 trampoline/original 状态
- 主流程改动：
  - `War3Hook::InstallGameHooks` 改为构造 `ShadowHookAddresses`，统一调用 `InstallShadowHooks()`。

### 2.2 Bridge 侵入式句柄槽（默认关闭）
- 新增编译期开关（`war3_internal_test_config.h`）：
  - `kNativeBridgeInlineHandleSlotEnabled`
  - `kNativeBridgeInlineHandleSlotOffset`
  - `kNativeBridgeInlineHandleSlotWriteBackEnabled`
- 新增快路径（`war3_render_exec_batch.cpp`）：
  - `TryReadInlineHandleSlot()`：可选从 `sceneNode + offset` 直读句柄；
  - `TryWriteInlineHandleSlot()`：可选回写句柄（默认关闭）；
  - 命中后可跳过一部分 SceneNode->Registry 查询链。

## 3. 安全边界
- 该直读/回写路径默认全关闭，不影响当前功能行为。
- 仅在确认 ASM 偏移后才建议开启 `kNativeBridgeInlineHandleSlotEnabled`。
- 回写开关单独控制，避免在偏移未完全确认时污染原生对象内存。

## 4. IDA MCP 校验（ASM）
- 已通过 IDA MCP 在线验证关键函数：
  - `0x6F76D800` `ShadowProjector_Add_FromObject`
  - `0x6F76D790` `ShadowProjector_Add_Simple`
  - `0x6F13A5E0` `RenderQueue_Dispatch_Common`
  - `0x6F13A510` `RenderQueue_UpdateItemWorldMatrix`
- 结论：
  - `Add_FromObject` 与 `Add_Simple` 最终均收敛到 `sub_6F713CA0`（投影写入核心），说明在该层拦截是有效的；
  - `Dispatch_Common` 首段仍保留 `RenderQueue_UpdateItemWorldMatrix` 前置更新，Single Dispatch 需要继续围绕“减少外层 dispatch 次数”推进。

## 5. 下一步
1. 通过 IDA ASM 确认可复用字段偏移（优先 CSceneNode 的稳定 user-data/保留位）。
2. 先只开“直读不回写”验证命中率和稳定性，再评估是否开启回写。
3. 在 `FQ_Dispatch_Opaque` 下补充分项统计，观察快路径命中后 CPU 占比变化。
