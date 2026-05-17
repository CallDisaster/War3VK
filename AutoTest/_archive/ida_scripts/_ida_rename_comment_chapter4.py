"""把 04_cmodel_pose_palette.md (第 4 章) 研究结论写回 IDA。

新增/补全：
- CModel anim advance 三个变体 (0x12EE90 / 0x12EF70 / 0x12FAA0)
- CModel pose stack helpers (0x12F3B0 / 0x12F7E0 / 0x12FB80 / 0x12F500)
- RenderQueue palette slot allocators (0x138FF0 / 0x139060 / 0x13A510)
- CModel sprite/runtime/visibility helpers
- CMatrixGroup / CModelData
"""
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
    # CSpriteUber dispatch entry
    ("0x6F12F0A0", "CModel_SetWorldMatrixAndBuildStagePresets"),
    ("0x6F12EB70", "CModel_BuildVisiblePartStagePresets_Simple"),
    ("0x6F12EC90", "CModel_RecurseChildPoseStack"),
    ("0x6F12F2F0", "CModel_VisibilityRecurseChild"),
    ("0x6F12EDE0", "CModel_VisibilityCacheClear"),
    # CModel anim advance variants
    ("0x6F12EE90", "CModel_AdvanceAnimSpriteSkip"),
    ("0x6F12EF70", "CModel_AdvanceAnimWithDeltaMs"),
    ("0x6F12FAA0", "CModel_AdvanceAnimByConstFlag"),
    ("0x6F12F500", "CModel_AdvanceAnimByMs"),
    # Pose stack helpers
    ("0x6F12F3B0", "CModel_BuildPoseStackRoot"),
    ("0x6F12F7E0", "CModel_SubtreePoseStablePoint"),
    ("0x6F12FB80", "CSpriteUber_AdvanceFrameTime400"),
    # Controller helpers
    ("0x6F12E7B0", "CModel_ControllerSlotInline"),
    ("0x6F12E8B0", "CModel_GetCurrentControllerOrSlot"),
    # MatrixGroup helper
    ("0x6F12E200", "CMatrixGroup_BlendOutputMatrix"),
    # RenderQueue palette
    ("0x6F138FF0", "RenderQueue_AllocPaletteSlot"),
    ("0x6F139060", "RenderQueue_GetPaletteSlotAddress"),
    ("0x6F13A510", "RenderQueue_UpdateItemWorldMatrix"),
    # Sprite / runtime model
    ("0x6F185250", "CSprite_BindRuntimeModel"),
    ("0x6F18EA90", "CSpriteUber_TryAttachAnchorScale"),
    ("0x6F1AB240", "CModel_TryNormalizeWorldScaleVec"),
    ("0x6F6BD110", "HostBindSourceSpriteRuntime"),
    ("0x6F130CD0", "CModelData_CloneIntoModel_A"),
    ("0x6F130D90", "CModelData_CloneIntoModel_B"),
]

COMMENTS = [
    (
        "0x6F12FED0",
        "【Pose 04】CModel_AllocAndFillGroupPalette\n"
        "Writer 1：主 palette writer。每个 RenderablePart 调一次。\n"
        "  - 读 `CGeosetData + 0xF0` 的 groupCount\n"
        "  - 调 RenderQueue_AllocPaletteSlot 拿一个 slot index\n"
        "  - 把 slot index 写回 `RenderablePart + 0x08`\n"
        "  - 调 0x12E600 BuildGroupBlendedPalette 把 groupCount*48 字节填到\n"
        "    globalPaletteBuf + slotIndex*48\n"
        "Phase 7.47 实测每帧 ~5849 次 fire；与 0x12FF90 / 0x12FDC0 互不冲突。",
    ),
    (
        "0x6F12E600",
        "【Pose 04】CGeosetData_BuildGroupBlendedPalette\n"
        "Writer 2：真正写 groupCount*48 字节的核函数。\n"
        "  - 输入: CGeosetData*, poseStackBase (current dword_6FBEE648 top), outPalette\n"
        "  - 用 CGeosetData->matrixGroupSizes / matrixIndices 把每 group 的 bone 矩阵\n"
        "    blend 成 1 个 3x4 matrix；输出连续写入 outPalette[group*48..]\n"
        "Phase 7.47 实测每帧 ~13650 次 fire（被 0x12FED0 在 RenderablePart loop 里重复调）",
    ),
    (
        "0x6F12FDC0",
        "【Pose 04】CModel_CopyPoseMatrixRangeFromStack\n"
        "Writer 3：把 pose stack 上的一段拷贝到 CModel +0x60..(+0x60+48*matrixCount)\n"
        "  - matrixCount 从 CModel +0x5C 读\n"
        "  - source 是 dword_6FBEE648 当前 top + offset\n"
        "  - 拷贝 12 float (1 matrix) * matrixCount 次\n"
        "Phase 7.34 A3 让此函数兼任 PoseRegistry publisher。\n"
        "Phase 7.47 实测每帧 ~370 次 fire。",
    ),
    (
        "0x6F12FF90",
        "【Pose 04】CModel_AllocAndFillSimpleFallbackPalette\n"
        "Writer 4：简单回退路径，仅在 groupCount == 0 时使用。\n"
        "  - 给每个 RenderablePart 分配 1-slot palette\n"
        "  - 直接从 dword_6FBEE648 top 拷 48B\n"
        "  - 把 slot index 写回 `RenderablePart + 0x08`\n"
        "Phase 7.47 实测每帧 ~751 次 fire；用于非动画或单矩阵模型。",
    ),
    (
        "0x6F12E900",
        "【Pose 04】CModel_EvalSingleGeosetAndRecurseChildren\n"
        "Pose 写入 dispatcher：根据 model->this[38] (controller 是否存在) 分流。\n"
        "  - this[38] != 0  : 调 0x12FED0 AllocAndFillGroupPalette + 0x12FDC0 CopyPoseMatrixRangeFromStack\n"
        "  - this[38] == 0  : 调 0x12FF90 AllocAndFillSimpleFallbackPalette\n"
        "之后 child loop (sub_6F12F2F0) 递归到子节点。\n"
        "本函数体内既有 0x12FED0 也有 0x12FDC0：两者不是替代而是先写 group palette\n"
        "再拷 final pose，是 same-frame 后续关系。Phase 7.55 v3 的关键判定。",
    ),
    (
        "0x6F12F0A0",
        "【Pose 04】CModel_SetWorldMatrixAndBuildStagePresets\n"
        "stage preset entry：每帧 PreRender 都调一次。\n"
        "  - 写 CModel +0x64..+0x84 当前 world 3x4 matrix\n"
        "  - push 一帧到 dword_6FBEE648 (pose stack top)\n"
        "  - 根据 flags & 0x10 (override graph) 决定:\n"
        "    True  → 调 0x12E900 EvalSingleGeoset (复杂动画路径)\n"
        "    False → 调 0x12EB70 BuildVisiblePartStagePresets_Simple\n"
        "★ 即使 anim 不 advance，本函数仍每帧跑，但 dt gate 控制 0x12E900 是否调。",
    ),
    (
        "0x6F12EE90",
        "【Pose 04】CModel_AdvanceAnimSpriteSkip\n"
        "anim advance 路径 1：flag & 0x20000 时使用。\n"
        "  - 在 dt > 0 但帧太短时跳过推进\n"
        "  - 用于低优先级 sprite，避免每帧都 anim 推进",
    ),
    (
        "0x6F12EF70",
        "【Pose 04】CModel_AdvanceAnimWithDeltaMs\n"
        "anim advance 路径 2 (默认)：标准路径。\n"
        "  - 输入 dt (秒)，转 ms (dt * 1000.0)\n"
        "  - 推进 anim controller，更新 pose stack top",
    ),
    (
        "0x6F12FAA0",
        "【Pose 04】CModel_AdvanceAnimByConstFlag\n"
        "anim advance 路径 3：flag & 0x40000 时使用。\n"
        "  - 用 dword_6FBE3D70 (常量 dt) 而非外部传入的 dt\n"
        "  - 用于固定速率动画如施法/UI/效果",
    ),
    (
        "0x6F138FF0",
        "【Pose 04】RenderQueue_AllocPaletteSlot\n"
        "全局 palette arena slot 分配器。\n"
        "  - 输入: groupCount\n"
        "  - 输出: slot index (写到 RenderablePart + 0x08)\n"
        "  - arena base: Game.dll + 0xBC6BD0 (g_globalPaletteArena)\n"
        "  - 实际地址: arenaBase + slotIndex * 48\n"
        "  - 每帧前 RenderQueue_FlushAndReset 清零 g_paletteAllocOffsetThisFrame",
    ),
    (
        "0x6F139060",
        "【Pose 04】RenderQueue_GetPaletteSlotAddress\n"
        "读 paletteSlot index 对应的实际地址。\n"
        "  - 输入: paletteSlotIndex\n"
        "  - 输出: globalPaletteBuf + slotIndex * 48 (12-float matrix base)",
    ),
    (
        "0x6F13A510",
        "【Pose 04】RenderQueue_UpdateItemWorldMatrix\n"
        "渲染前调用：读 `RenderablePart + 0x08` 的 paletteSlotIndex。\n"
        "  - 有效 slot (< 0x3A98u) → GetPaletteSlotAddress → 走 skinning 上传\n"
        "  - 无效 slot (= 0xFFFFFFFFu) → 走 identity fallback (zero matrix)\n"
        "★ Phase 7.47 / 7.55 的关键判定：even with invalid slot 主渲染仍然流畅，\n"
        "  说明 War3 不依赖 palette 做 skinning（CPU skinning 已写好 VB）",
    ),
    (
        "0x6F12FE10",
        "【Pose 04】RenderQueue_ResizePaletteBuffer\n"
        "palette arena 容量扩张。\n"
        "三元组同步: g_globalPaletteArenaCapacity / Size / Base (BC6B58/5C/60)",
    ),
]


print("== rename ==")
res = use("rename", {"batch": {"func": [{"addr": a, "name": n} for a, n in RENAMES]}})
print(json.dumps(res.get("result", res.get("error", {})), ensure_ascii=False)[:3000])

print("\n== set_comments ==")
res = use(
    "set_comments",
    {"items": [{"addr": a, "comment": c} for a, c in COMMENTS]},
)
print(json.dumps(res.get("result", res.get("error", {})), ensure_ascii=False)[:3000])
