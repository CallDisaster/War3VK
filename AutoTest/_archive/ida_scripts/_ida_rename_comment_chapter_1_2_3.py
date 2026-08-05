"""把第 1 / 2 / 3 章（剔除→渲染过渡 / RenderQueue 分发 / CSprite 动画）研究结论写回 IDA。"""
import json
import urllib.request

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read())


def use(name, args):
    return call("tools/call", {"name": name, "arguments": args})


RENAMES = [
    # === Chapter 1: visibility -> renderqueue ===
    ("0x6F184F00", "CWorld_VisibilityOrPreRenderHook"),
    ("0x6F0CAA90", "WorldObjectList_QueryVisibleCandidates"),
    ("0x6F1854A0", "WorldObjectList_QueryVisibleCandidates_Inner"),
    ("0x6F0CB110", "WorldObjectList_AddEntry_ObjectSelfOwnerHint"),
    ("0x6F0CB480", "WorldObjectList_RenderAll"),
    ("0x6F186300", "CWorld_PreRenderRootStage0"),
    ("0x6F367980", "RenderSelectionManager_Stage15"),
    ("0x6F368A90", "WorldObjects_RenderGroup_Stage16Sub"),
    ("0x6F369560", "Stage16_FinalSubdispatch"),
    ("0x6F368D60", "CWorld_UpdateIndicatorAnchor"),
    ("0x6F35EBE0", "CWorld_PushFrameScope"),
    ("0x6F360270", "CWorld_PopFrameScope"),
    ("0x6F0A2300", "CWorld_FrameUpdateGate"),
    ("0x6F37C520", "GameUI_FrameSync"),
    ("0x6F0D0AF0", "Camera_LoadCurrentMatrix"),
    ("0x6F0D2370", "Camera_BuildFrustumPlanes8"),
    ("0x6F343150", "Camera_GetWorldPos"),
    ("0x6F3431C0", "Camera_GetWorldDir"),
    ("0x6F139860", "Scene_QueryFlushSync"),
    ("0x6F1398E0", "Scene_QueryFlushSync2"),
    ("0x6F76EF80", "TerrainShadow_FlushPass"),
    ("0x6F76D920", "ShadowProjector_FlushPass"),
    ("0x6F770670", "Terrain_RenderExtraPass"),
    ("0x6F0E3910", "RenderScene_PrepareViewProjPair"),
    ("0x6F0E3A00", "RenderScene_PrepareCameraConst"),
    ("0x6F361D00", "MapBoundary_QueryClipRange"),

    # === Chapter 2: RenderQueue dispatch ===
    ("0x6F139800", "RenderQueue_FlushAndReset"),
    ("0x6F139620", "SceneNode_RenderTransparentBatchPath"),
    ("0x6F1378D0", "AUCTransparent_ItemComparator"),
    ("0x6F137D50", "RenderQueue_ItemComparator"),
    ("0x6F13A0E0", "RenderQueue_TransparentDispatchType0"),
    ("0x6F198C00", "RenderQueue_TransparentDispatchType1"),
    ("0x6F19DFF0", "RenderQueue_TransparentDispatchType2"),
    ("0x6F19BC20", "RenderQueue_TransparentDispatchType3"),
    ("0x6F13A0B0", "RenderQueue_TransparentDispatchType4"),
    ("0x6F137BD0", "RenderQueue_PrepareLayerAlphaSnapshot"),
    ("0x6F0E36D0", "GxDevice_BeginPrimitiveBatch"),
    ("0x6F0E36C0", "GxDevice_EndPrimitiveBatch"),
    ("0x6F0E36E0", "GxDevice_SetAlphaMode"),
    ("0x6F0E3710", "GxDevice_PushAlphaSlotValue"),
    ("0x6F0E3540", "GxDevice_FlushPrimitiveBatch"),
    ("0x6F0E3520", "GxDevice_DrawIndexedRange"),
    ("0x6F0E2DA0", "GxDevice_QueryStageInitialState"),
    ("0x6F0A3D80", "RenderScene_BuildFlushParams"),
    ("0x6F0E39E0", "RenderScene_Flush"),
    ("0x6F777FE0", "LOSManager_QueryNodeVisible"),
    ("0x6F04F200", "RuntimeWrapper_AcquireRelease"),
    ("0x6F04F1A0", "RuntimeWrapper_Release"),

    # === Chapter 3: CSprite animation ===
    ("0x6F18F030", "CSpriteUber_FrameLogTraceUpdate"),
    ("0x6F183A30", "CSpriteUber_HandleSentinelReset"),
    ("0x6F137170", "CSpriteUber_LoadWorldMatrixToStack"),
    ("0x6F139AE0", "RenderQueue_PerfScopeAlloc"),
    ("0x6F133600", "CModel_GetAttachmentCount"),
    ("0x6F133540", "CModel_GetAttachmentByIndex"),
    ("0x6F12F4C0", "CSpriteUber_PropagateLinkedFlags"),
]


COMMENTS = [
    (
        "0x6F368480",
        "【Chapter 1】worldFrameUpdateAndPreparePasses\n"
        "每帧主入口（FrameUpdate 阶段）。主要工作：\n"
        "  1. dt 累加到 *(this+912)\n"
        "  2. push frame scope（sub_6F35EBE0）\n"
        "  3. early-return: sub_6F0A2300 false 时整帧跳过（暂停/加载）\n"
        "  4. Camera_LoadCurrentMatrix / GetWorldPos / GetWorldDir\n"
        "  5. Camera_BuildFrustumPlanes8 (0x0D2370) 构建 8 平面 frustum\n"
        "  6. Scene_QueryFlushSync (0x139860) 场景可见性预查询\n"
        "  7. TOD/fog 更新 + Shadow pre-pass（TerrainShadow_FlushPass / ShadowProjector_FlushPass）\n"
        "  8. WorldObjectList_QueryVisibleCandidates_ForcedGate 触发可见性查询\n"
        "  9. CWorld_VisibilityOrPreRenderHook (0x184F00) 调对象 vt[3] 推进动画\n"
        "  10. Particle/ribbon advance\n"
        "★ CWorld + 0x16C/0x170/0x174 = group 0/1/2 list head（装饰物/单位/飞行）",
    ),
    (
        "0x6F3681C0",
        "【Chapter 1】CWorld_RenderScene\n"
        "主渲染调度链：22 个 stage 顺序执行。\n"
        "stage 0 (PreRender) → 1+13 (TerrainShadow) → FlushAndReset →\n"
        "  19+9+2+3+8+17 (TerrainShadow) → 14+5+10 (TerrainShadow) →\n"
        "  12 (group 1 单位) → 11 (group 0 装饰物) →\n"
        "  FlushAndReset → 4+7+6+20 (TerrainShadow) →\n"
        "  [activeQueue==0] 15 (selection) + 18 + 21 (UI 收尾)\n"
        "★ 2 次 RenderQueue_FlushAndReset 切割主渲染期\n"
        "★ 项目主 hook 通常在 RenderQueue_FlushAndReset 前后插入 BeforeUi/Shadow/Outline/SSAO pass",
    ),
    (
        "0x6F363020",
        "【Chapter 1】RenderWorld_DispatchStage\n"
        "stageId 分发器（0..21）。每次入口先做：\n"
        "  - Category 切换（offset 0x664/field 409） RenderCategory_Disable/Enable\n"
        "  - Mode 切换（offset 0x660/field 408） sub_6F363350\n"
        "Stage 表（详细见 docs/plan/overnight_render_paper_2026_05_15/01_visibility_to_renderqueue.md §6）：\n"
        "  0:PreRender 1-10,14,17,19-21:TerrainShadow_Dispatch 各种\n"
        "  11/12/13: WorldObjects_RenderGroup(0/1/2)\n"
        "  15: SelectionManager 16: UI 子分发 18: post-process 21: 收尾",
    ),
    (
        "0x6F368E30",
        "【Chapter 1】WorldObjects_RenderGroup\n"
        "对象组渲染入口。a3 = 0/1/2 选择 group：\n"
        "  group 0 (CWorld+0x16C, field 91) : 装饰物 (Doodads) / 地面贴花 / 静态对象\n"
        "  group 1 (CWorld+0x170, field 92) : 单位主体 (CUnit) — 英雄/普通/建筑\n"
        "  group 2 (CWorld+0x174, field 93) : 飞行单位 / 特效 / 高度对象\n"
        "对每个 entry 调 WorldObjectEntry_Render，每 entry 24 字节。",
    ),
    (
        "0x6F184EE0",
        "【Chapter 1】WorldObjectEntry_Render\n"
        "单对象渲染入口。两步：\n"
        "  1. 调 a1->vt[5] (sub at vtable+20) — RenderPrepare/PreRender\n"
        "  2. 调 RenderQueue_AddBatch 把对象 batch 入主队列\n"
        "★ vt[5] 内部最终会触发 CSpriteUber_PreRenderAndUpdatePosePalette_*（详见 Chapter 3）",
    ),
    (
        "0x6F0CB110",
        "【Chapter 1】WorldObjectList_AddEntry_ObjectSelfOwnerHint\n"
        "给 WorldObjectList 追加 24B entry：\n"
        "  +0x00 = sub_6F04F200(object) runtime acquire wrapper\n"
        "  +0x14 = a3 ownerHint\n"
        "★ 在 model_runtime_probe 阶段 ownerHint 可能为 0，仅 diagnostic 用",
    ),
    (
        "0x6F139190",
        "【Chapter 2】RenderQueue_AddBatch\n"
        "递归处理一个 SceneNode 的所有 batch + 子 SceneNode：\n"
        "  1. 调 RenderBatch_Submit 提交本 SceneNode 所有 layer\n"
        "  2. 如果 flag & 0x10：处理 transparent batch 列表（4 种 type）\n"
        "  3. 递归子 SceneNode（按 LOS 分流：sub_6F777FE0 LOSManager_QueryNodeVisible）\n"
        "★ SceneNode 关键字段：+0x9C batchListHead, +0xC4 childCount, +0xC8 childArray, +0xD4 LOSStateBytes",
    ),
    (
        "0x6F1375C0",
        "【Chapter 2】RenderBatch_Submit\n"
        "把 RenderBatch 的每个 layer 提交到 opaque 主队列或 AUCTransparent 辅队列。\n"
        "对每个 layer：\n"
        "  - layerEntry+16 != 0 时跳过\n"
        "  - 检查 material visibility (batch->materialArray[meshData->materialIdx])\n"
        "  - CanEnqueueToMainQueue 返回 true → opaque 主队列（5 dword/entry）\n"
        "  - 否则 → AUCTransparent_AddEntry（按 camera 距离）\n"
        "主队列 entry 5 dword = 20B：\n"
        "  +0x00 batch ptr  +0x04 flags(0x1=special, 0x2=follow-state)\n"
        "  +0x08 layerIdx   +0x0C stageIdx   +0x10 layerState ptr",
    ),
    (
        "0x6F137AF0",
        "【Chapter 2】AUCTransparent_AddEntry\n"
        "把透明 layer 加到辅队列。entry 6 dword = 24B：\n"
        "  +0x00 transparentType (0..5)\n"
        "  +0x04 layerCtx\n"
        "  +0x08 distSq (camera 距离平方，作 sort key)\n"
        "  +0x0C batch_or_arg1\n"
        "  +0x10..+0x14 type=5 callback args",
    ),
    (
        "0x6F1380A0",
        "【Chapter 2】RenderQueue_FlushSortedItems\n"
        "opaque 主队列 sort + dispatch。\n"
        "  - g_NumOfElements 上限 10000\n"
        "  - qsort 用 RenderQueue_ItemComparator\n"
        "  - 排序：special > non-special；layerState ptr 升序；前 20B 字典序；meshData ptr 升序\n"
        "  - 遍历分发 Dispatch_Common (普通) 或 Dispatch_Special (特殊)\n"
        "  - State opt: 同 meshData/batch 时跳过 GxDevice_ApplyStateBlock\n"
        "  - 末尾若 StateCleanupPending → cleanup",
    ),
    (
        "0x6F138210",
        "【Chapter 2】RenderQueue_FlushTransparent\n"
        "AUCTransparent 辅队列 sort + dispatch。\n"
        "  - 上限 10000\n"
        "  - qsort 用 AUCTransparent_ItemComparator (先 type 后 distSq)\n"
        "  - switch 分发到 5 种 type:\n"
        "    type 0: TransparentDispatchType0 (普通透明 batch)\n"
        "    type 1: sub_6F198C00 (粒子 emitter, stride 104B)\n"
        "    type 2: RenderImageLikePrimitiveBatch (UI/billboard/decal)\n"
        "    type 3: sub_6F19BC20 (ribbon emitter, stride 356B)\n"
        "    type 4: TransparentDispatchType4 (callback 形式)\n"
        "    type 5: 任意 callback (entry->batch)(arg1, arg2)",
    ),
    (
        "0x6F139800",
        "【Chapter 2】RenderQueue_FlushAndReset\n"
        "顶层 flush 入口（每帧 BeforeUi 调一次）：\n"
        "  StageUpdate(force=1) → FlushSortedItems (opaque)\n"
        "    → FlushTransparent (5 种 type)\n"
        "    → StageUpdate(force=1) (清理)\n"
        "    → reset NumOfElements=0, AUCTransparent_Count=0\n"
        "★ 项目主 hook 通常在此函数前后插入自定义 pass",
    ),
    (
        "0x6F13A5E0",
        "【Chapter 2】RenderQueue_Dispatch_Common\n"
        "普通 opaque 路径：\n"
        "  - RenderQueue_UpdateItemWorldMatrix 读 paletteSlot\n"
        "  - 准备 alpha snapshot (sub_6F137BD0)\n"
        "  - BindDispatchBlock\n"
        "  - 调 GxDevice_BeginPrimitiveBatch / SetAlphaMode\n"
        "  - 视 needStateUpdate 调 GxDevice_ApplyStateBlock\n"
        "  - GxDevice_FlushPrimitiveBatch 实际画\n"
        "  - meshData+260 == 0 时调 RenderScene_Flush 一次",
    ),
    (
        "0x6F13A780",
        "【Chapter 2】RenderQueue_Dispatch_Special\n"
        "特殊 opaque 路径。先做 IsSpecialBatchStateConsistent 判定：\n"
        "  - 一致 → DispatchSpecialBatch 走快路径\n"
        "  - 不一致 → cleanup 当前状态，DispatchFallbackMultiPass 多 pass dispatch\n"
        "Fallback 路径慢 N 倍但兼容性高（每子批次重新 apply state）。",
    ),
    (
        "0x6F13A180",
        "【Chapter 2】RenderQueue_DispatchFallbackMultiPass\n"
        "Special batch 状态不一致时的降级路径。\n"
        "对每个 drawCommand 子批次：\n"
        "  - 重新计算 alpha\n"
        "  - ApplyTextureStageMode + ApplyDrawStateAndSamplerPair\n"
        "  - GxDevice_ApplyStateBlock\n"
        "  - 多 pass GxDevice_DrawIndexedRange\n"
        "  - GxDevice_StateCleanup78\n"
        "★ 比快路径 (DispatchSpecialBatch) 慢，仅在状态不一致时使用",
    ),
    (
        "0x6F139620",
        "【Chapter 2】SceneNode_RenderTransparentBatchPath\n"
        "SceneNode 透明 batch 调度（按 type 分发到 5 个 dispatcher）：\n"
        "  - 先 StageUpdate(1)\n"
        "  - type 0: 遍历 +168 list（type0 transparent）→ TransparentDispatchType0\n"
        "  - type 1: 遍历 +220 list 每 entry stride 104B → sub_6F198C00\n"
        "  - type 2: 遍历 +232 list → RenderImageLikePrimitiveBatch\n"
        "  - type 3: 遍历 +244 list 每 entry stride 356B → sub_6F19BC20\n"
        "  - type 4: 遍历 +168 list 满足 +32 != 0 → TransparentDispatchType4\n"
        "  - 递归子 SceneNode（按 LOS）",
    ),
    (
        "0x6F137D50",
        "【Chapter 2】RenderQueue_ItemComparator\n"
        "opaque 主队列排序 comparator。优先级：\n"
        "  1. special vs not-special：(flags & 3) == 3 排前\n"
        "  2. special-only: meshData ptr → flags → layerState ptr → 前 20B 字典序\n"
        "  3. non-special: layerState ptr → 前 20B 字典序 → meshData ptr\n"
        "★ 目标是把相同 state 的 batch 聚在一起，最大化 state opt 命中",
    ),
    (
        "0x6F1378D0",
        "【Chapter 2】AUCTransparent_ItemComparator\n"
        "透明辅队列排序 comparator：\n"
        "  - 不同 type → 按 type 升序（先 type 分桶）\n"
        "  - 同 type → 按 distSq 升序（同桶按距离从近到远）",
    ),
    (
        "0x6F13A510",
        "【Chapter 2】RenderQueue_UpdateItemWorldMatrix\n"
        "渲染前调用：读 RenderablePart + 0x08 的 paletteSlotIndex。\n"
        "  - 有效 slot → GetPaletteSlotAddress → 走 skinning 上传\n"
        "  - 无效 slot (0xFFFFFFFFu) → 走 identity fallback (zero matrix)\n"
        "★ Phase 7.47/7.55 关键判定：even with invalid slot 主渲染仍流畅，\n"
        "  说明 War3 不依赖 palette 做 skinning（CPU skinning 已写好 VB）",
    ),
    (
        "0x6F139060",
        "【Chapter 2】RenderQueue_GetPaletteSlotAddress\n"
        "读 paletteSlot index 对应的实际地址 = globalPaletteBuf + slotIndex * 48。",
    ),
    (
        "0x6F138FF0",
        "【Chapter 2】RenderQueue_AllocPaletteSlot\n"
        "全局 palette arena slot 分配器（输入 groupCount，输出 slot index）。\n"
        "  arena base: Game.dll + 0xBC6BD0\n"
        "  实际地址: arenaBase + slotIndex * 48",
    ),
]


print("== rename ==")
res = use("rename", {"batch": {"func": [{"addr": a, "name": n} for a, n in RENAMES]}})
print(json.dumps(res.get("result", res.get("error", {})), ensure_ascii=False)[:5000])

print("\n== set_comments ==")
res = use(
    "set_comments",
    {"items": [{"addr": a, "comment": c} for a, c in COMMENTS]},
)
print(json.dumps(res.get("result", res.get("error", {})), ensure_ascii=False)[:3000])
