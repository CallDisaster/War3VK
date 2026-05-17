"""把 06_fogmask_static_shadow.md 研究结论写回 IDA。

新增/补全：
- 中央 footprint registry helpers (5 个)
- WriteMaskRegion fastpath helpers (2 个)
- CFogOfWarMap helpers (1 个)
- CWidget shadow-relevant setters (10+ 个)
- CFogMask 字段语义注释
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
    # FogMask helpers
    ("0x6F231880", "CFogMaskTable_AdjustGrowHint"),
    ("0x6F2328E0", "CFogMaskTable_ResizeEntryArray"),
    ("0x6F232AF0", "CFogMaskTable_AllocNodeBuffer"),
    ("0x6F232B80", "CFogMaskTable_AllocCellBuffer"),
    ("0x6F231EA0", "CFogMask_ComputeAxisRange"),
    ("0x6F232C20", "CFogMask_SetBitDepth"),

    # WriteMaskRegion fastpath / dispatch helpers
    ("0x6F1F5180", "TerrainShadow_BoxFastpath"),
    ("0x6F1F4DD0", "TerrainShadow_PolyFastpath"),
    ("0x6F233570", "TerrainShadow_ScanlineFastpath"),

    # WriteMaskRegion 入口 wrapper / dispatcher
    ("0x6F234420", "TerrainShadow_DispatchToShape"),
    ("0x6F234060", "TerrainShadow_RebuildMask_PlayerSlotHelper"),
    ("0x6F234055", "TerrainShadow_RebuildMask_WidgetWriter"),

    # CWidget / CUnit shadow-relevant setters (来源于 5.3 caller 表)
    ("0x6F60D860", "CUnit_SetShadowVisible"),
    ("0x6F60D730", "CUnit_SetBuildingFlag"),
    ("0x6F60E860", "CUnit_SetVisibility"),
    ("0x6F611DD0", "CUnit_SetOwnerForVision"),
    ("0x6F52F980", "CUnit_OnBuildComplete"),
    ("0x6F530260", "CUnit_OnConstructionStart"),
    ("0x6F4BB710", "CUnit_OnDestroy"),
    ("0x6F5B2A50", "CUnit_SetPosition_Shadow"),
    ("0x6F5B8AB0", "CUnit_Teleport_Shadow"),
    ("0x6F5B93A0", "CUnit_OnKill_Shadow"),
    ("0x6F5B94B0", "CUnit_OnDecay_Shadow"),
    ("0x6F4ACA40", "CUnit_SetFlag_Shadow"),
    ("0x6F52CE40", "CUnit_MorphTo_Shadow"),
    ("0x6F4EF520", "CUnit_ChangeOwner_Shadow"),
    ("0x6F568130", "CUnit_SetVisible_Shadow"),
    ("0x6F56B520", "CUnit_SetPathing_Shadow"),
    ("0x6F56E500", "CUnit_SetLifecycleState_Shadow"),
    ("0x6F56E890", "CUnit_SetHidden_Shadow"),
    ("0x6F588800", "CUnit_Load_Shadow"),
    ("0x6F5B9A70", "CUnit_SetOwnerSlot_Shadow"),
    ("0x6F4A57E0", "CUnit_create_Shadow"),
    ("0x6F407F90", "CDestructable_create_Shadow"),
    ("0x6F629BA0", "CDestructable_SetHidden_Shadow"),
    ("0x6F630F20", "CDestructable_SetKilled_Shadow"),

    # 辅助 helper
    ("0x6F76FAB0", "Map_QueryGroundElevation"),
    ("0x6F668F40", "CWidget_GetShadowMaskFlags"),
    ("0x6F678230", "CWidget_GetExtendedVisibilityMask"),
    ("0x6F215590", "CPlayer_GetSharedVisionMask"),
    ("0x6F2412C0", "CWidget_ComputeFootprintMaskBits"),
    ("0x6F66EA60", "Map_IsFogEnabled"),
    ("0x6F66E5F0", "CWidget_HasShadowFlag"),
]

COMMENTS = [
    (
        "0x6F234710",
        "【FogMask shadow 06】WriteMaskRegion 真正的 mask 写入函数。\n"
        "签名: this=CFogMask*, a2=对象(CWidget*/Actor*), a3=type code (16-bit), a4=可选 footprint 矩形, a5=OR-only 标志\n"
        "\n"
        "核心：操作 4 个并行 mask layer (CFogMask 内连续字段):\n"
        "  v5[11] (+0x2C) clearMaskBase  : &= ~a3   (清除 player slot 的 bit)\n"
        "  v5[12] (+0x30) setMaskBase    : |= a3    (设置 player slot 的 bit)\n"
        "  v5[14] (+0x38) elevationMaskBase : 高度等级 (((h+8256)&0xFF80)-0x2000)\n"
        "  v5[15] (+0x3C) aboveCurrentMaskBase : LOS-阻挡判定\n"
        "\n"
        "type code (a3) 拆解:\n"
        "  bit 0..11 : 12 个 player slot 共享视野 mask 写入位\n"
        "  bit 12-14 : shape (0=点, 1=scanline, 2=矩形短边, 4=矩形)\n"
        "  bit 15    : reserved\n"
        "\n"
        "★ 决定写哪份 mask 的不是 a3，而是对象 +0x10C 的 mask idx 字段:\n"
        "  idx=0 : 战争迷雾\n"
        "  idx=1 : LOS 可见性\n"
        "  idx=2 : 路径阻挡\n"
        "  idx=3 : 阴影 footprint  ← 项目要屏蔽建筑预渲染贴花阴影应在此 idx 处过滤\n"
        "  idx=4 : flying mask (a4!=NULL 时强制)\n"
        "\n"
        "项目治理蓝图（方案 A）:\n"
        "  hook 此函数入口；当对象 +0x10C == 3 且 a4==NULL 时，return 0 跳过即可。\n"
        "  不影响 fog/LOS/path。\n"
        "详见 docs/plan/overnight_render_paper_2026_05_15/06_fogmask_static_shadow.md",
    ),
    (
        "0x6F232060",
        "【FogMask shadow 06】CFogMaskTable::GetOrCreateMask\n"
        "按 idx 从 entryArray 拿对应 CFogMask*；若不存在则用 CFogMask_BuildNodeAndRangeTable 创建。\n"
        "idx 含义见 0x234710 注释。",
    ),
    (
        "0x6F230210",
        "【FogMask shadow 06】CFogMask::BuildNodeAndRangeTable\n"
        "构造 CFogMask 实例并填充 nodeArray / cellArray。\n"
        "字段布局（dword index）：\n"
        "  +0x04 inputDepth (a2)\n"
        "  +0x18 nodeCount\n"
        "  +0x1C nodeArrayPtr (8B per node entry)\n"
        "  +0x2C clearMaskBase  (短整型 mask grid 第 1 份: 写入时 &= ~a3)\n"
        "  +0x30 setMaskBase    (短整型 mask grid 第 2 份: 写入时 |= a3)\n"
        "  +0x38 elevationMaskBase\n"
        "  +0x3C aboveCurrentMaskBase",
    ),
    (
        "0x6F230F40",
        "【FogMask shadow 06】CFogOfWarMap::ctor\n"
        "vftable=0x97156C；单例由 dword_6FBE4238+0x34 (即 *((_DWORD*)gameWar3 + 13)) 索引。\n"
        "包含主 CFogMaskTable 与 axis range 等字段。",
    ),
    (
        "0x6F233E90",
        "【FogMask shadow 06】TerrainShadow_RebuildMaskFromObjectLists\n"
        "整体重建 mask grid 的入口（loadgame / 视野同步 / 画质切换时触发）。\n"
        "\n"
        "三段式重建:\n"
        "  段 1: 扫 dword_6FBE4798[] 待重建列表，对 widget+0x80 & 0x40 (建筑/可破坏物)\n"
        "        且 0x100==0 调 sub_6F234420 (DispatchToShape)\n"
        "  段 2: 扫主 widget table dword_6FBE40A8，对所有 visible widget (0x100==0||0x20)\n"
        "        调 TerrainShadow_WriteMaskRegion (主路径)\n"
        "  段 3: 再扫 dword_6FBE4798[] 列表，对 0x40 && 0x100 widget (飞行/特殊)\n"
        "        调 sub_6F234420\n"
        "\n"
        "type code 通过 word_6FBE47B8/BA/BC/BE 4 个 player slot mask 表 ROL 累加。\n"
        "★ 项目治理时若只 hook WriteMaskRegion 即可同时覆盖此重建路径。",
    ),
    (
        "0x6F233DF0",
        "【FogMask shadow 06】CFogOfWarMap::BuildVisibilityMask\n"
        "根据 player slot 计算可见性 mask；不直接写阴影 mask，但 mask 值会通过\n"
        "WriteMaskRegion 间接写入到 idx=1 的 CFogMask 实例。\n"
        "返回值: 0xFFFF (全开) / 0x0FFF (12-bit 全开) / 1<<bit (单 player)。",
    ),
    (
        "0x6F65A140",
        "【FogMask shadow 06】CWidget_RegisterFootprintAndShadowMask\n"
        "30+ caller 的中央 sync 入口；CUnit/CWidget 任意 lifecycle 状态变化都会调它。\n"
        "\n"
        "签名: a2=widget, a3=posXYZ, a4=posY(float), a6=32-bit flag pack\n"
        "  a6 & 0x00001 : CREATE\n"
        "  a6 & 0x00002 : DESTROY/decay\n"
        "  a6 & 0x00004 : MOVE\n"
        "  a6 & 0x00100 : RESTORE/loadgame\n"
        "  a6 & 0x00400 : ★ SHADOW-LAYER write (建筑阴影 footprint 写入)\n"
        "  a6 & 0x10000 : INVULN/permanent footprint\n"
        "  a6 & 0x20000 : SHARED-VISION\n"
        "\n"
        "末端调 TerrainShadow_WriteMaskRegion(*((_DWORD**)gameWar3+13), v9, ..., 1, 1)\n"
        "★ 项目治理时若只 hook WriteMaskRegion 即可覆盖所有 30+ caller。",
    ),
    (
        "0x6F514F40",
        "【FogMask shadow 06】CUnit_StampBuildingShadowFootprint\n"
        "CUnit lifecycle helper：把建筑 footprint 写入 mask。\n"
        "★ 验证 *(widget+0x0C) == 0x2B5DB42C (727803756) — CWidget 类型签名。\n"
        "  仅注册的 CWidget 派生类才有此 magic，CDoodads 不带。\n"
        "*(unit+96) |= 0x400  → 标记 ListA-shared blob mask flag (两条系统的桥)\n"
        "  *(_WORD*)(v3+44) |= v6 & 0xFFF  → 累加 type code 的 12 个 player slot bit",
    ),
    (
        "0x6F1F5180",
        "【FogMask shadow 06】TerrainShadow_BoxFastpath\n"
        "矩形 footprint 的 SIMD 快路径（128-bit 对齐写入）。\n"
        "case 1/2/4 区分是否同时写 clearMask + setMask, 还是只写 clearMask。",
    ),
    (
        "0x6F233570",
        "【FogMask shadow 06】TerrainShadow_ScanlineFastpath\n"
        "单行 footprint 写入；处理 4-byte 对齐 + 残留 16-bit。",
    ),
    (
        "0x6F234620",
        "【FogMask shadow 06】TerrainShadow_WriteMaskRegion_ForObject\n"
        "单对象 wrapper：先按对象 flag 累加 type code，再调 WriteMaskRegion 主路径。\n"
        "如果 (_WORD)a4 == 0xFFFF 表示需要重新计算 type code。",
    ),
    (
        "0x6F3DB260",
        "【FogMask shadow 06】TerrainShadow_WriteMaskRegion_FromActorRuntime\n"
        "Actor runtime（动态对象）写入 mask 的入口；标记 +0x5C |= 0x8000000，最后调 WriteMaskRegion。",
    ),
    (
        "0x6F234420",
        "【FogMask shadow 06】TerrainShadow_DispatchToShape\n"
        "形状分发：根据 v17 = a2[8] & 0x38 选择 fastpath:\n"
        "  case 8  : sub_6F1F5180 BoxFastpath (矩形)\n"
        "  case 16 : sub_6F1F4DD0 PolyFastpath (多边形)\n"
        "  case 32 : sub_6F1F4DD0 (含特殊 mask)\n"
        "如果 a3 == 0xFFFF 先用 player slot loop 重新计算 type code。",
    ),
    (
        "0x6F60D860",
        "【FogMask shadow 06】CUnit_SetShadowVisible\n"
        "★ 直接控制单位/建筑阴影 mask 的 setter。\n"
        "项目方案 C 可以 hook 此处，但只覆盖一个 caller，不彻底；\n"
        "推荐方案 A: 在 0x234710 入口按 maskIdx==3 拦截，覆盖 30+ caller 全部。",
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
