"""把 24 文档 v2 的 CUnit shadow 路径结论写回 IDA：
   - 命名 5 个 CUnit shadow 路径关键函数
   - 命名 5 个 CUnitUIManager record setter
   - 命名 1 个 CUnitUIManager 主 SLK loader
   - 给关键节点加中文注释
"""
import json, urllib.request

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read())


def use(name, args):
    return call("tools/call", {"name": name, "arguments": args})


# CUnit shadow path
RENAMES = [
    # CUnit emitter / dispatch / activation
    ("0x6F532420", "CUnit_FindShadowProjectorByObject"),
    ("0x6F543A90", "CUnit_DispatchEvent"),
    ("0x6F52F4D0", "CUnit_ActivateShadowProjector_Dispatch"),
    ("0x6F52F510", "CUnit_ActivateBuildingShadowProjector"),
    ("0x6F5449D0", "CUnit_ActivateGenericShadowProjector"),
    ("0x6F5457B0", "CUnit_RefreshAllShadowEmitters"),
    ("0x6F399220", "CShadowEmitter_GetTarget"),
    # CUnitUIManager record setters (from CUnitUIManager.cpp)
    ("0x6F327020", "CUnitUIManager_GetOrCreateRecord"),
    ("0x6F3358C0", "CUnitUIManager_RecordSetUnitShadow"),    # +0x4C
    ("0x6F335A00", "CUnitUIManager_RecordSetStructureShadow"),  # +0x50
    ("0x6F335930", "CUnitUIManager_RecordSetShadowOffset"),  # +0x80/+0x84
    ("0x6F335980", "CUnitUIManager_RecordSetShadowSize"),    # +0x88/+0x8C
    ("0x6F335960", "CUnitUIManager_RecordSetShadowOnWater"), # +0x90
    ("0x6F66BA00", "CUnitUIManager_LoadSlkRows"),
]

COMMENTS = [
    ("0x6F52F510",
     "【CUnit shadow 24】建筑物阴影投影器激活路径。\n"
     "读取 CUnitUIManager record 的 +0x48 (unitShadow color) / +0x4C (unitShadow texture) /\n"
     "+0x50 (buildingShadow texture)，调 CUnit_FindShadowProjectorByObject 找已存在 emitter，\n"
     "命中后调 ShadowPath_ObjectProjector_Runtime → ShadowProjector_Add_FromObject\n"
     "→ TerrainShadow_RegisterImageEntryWithParams → RegisterImageEntry(type=4)。\n"
     "源文件：War3\\Source\\UI/CUnitUIManager.cpp 周边。\n"
     "包含 PaidStructureColor / UnpaidStructureColor 字符串。\n"
     "详见 docs/research/war3_render_issues/24_cdoodads_static_shadow_upstream §5.5"),
    ("0x6F5449D0",
     "【CUnit shadow 24】通用单位阴影投影器激活路径（dispatch fallthrough 入口）。\n"
     "由 CUnit_DispatchEvent 在事件 0xD02A5 时调用。\n"
     "通过 vt[572] 取 emitter 资源后调 ShadowPath_ObjectProjector_Runtime。"),
    ("0x6F5457B0",
     "【CUnit shadow 24】每帧 prerender 时刷新 CUnit 所有 shadow emitter。\n"
     "遍历 CUnit + 0xA8 (this[42]) 的 emitter 数组（数量在 +0xA4 / this[41]），\n"
     "对未注册的（emitter[10] == 0）逐个 ShadowPath_ObjectProjector_Runtime。\n"
     "在 7 个 .data 槽位被引用，对应 7 张 CUnit 状态/种族 vtable。\n"
     "★ 这是关键：建筑阴影 hook 必须每次都 return -1，不能只拦第一次。"),
    ("0x6F543A90",
     "【CUnit shadow 24】CUnit 状态机事件分发器（在 7 个 .data 槽位被引用，对应 7 张 unit state vtable）。\n"
     "事件 0xD02A5 → CUnit_ActivateGenericShadowProjector\n"
     "事件 0xD02A6 → sub_6F543880\n"
     "默认 → CUnit_ActivateShadowProjector_Dispatch（建筑/普通分发，via vt[7] 比对）"),
    ("0x6F52F4D0",
     "【CUnit shadow 24】建筑/普通单位 shadow projector 激活分发器。\n"
     "通过 vt[7] 函数指针对比（byte_6F72642E / loc_6F726474）：\n"
     "  命中 → CUnit_ActivateBuildingShadowProjector (建筑路径)\n"
     "  否则 → sub_6F52F980 (其他单位路径)"),
    ("0x6F532420",
     "【CUnit shadow 24】在 CUnit + 0xA8 (this[42]) emitter 数组中查找 target = a2 的 emitter。\n"
     "数组长度在 CUnit + 0xA4 (this[41])。匹配键是 emitter[6] = emitter->GetTarget()。"),
    ("0x6F66BA00",
     "【CUnit shadow 24】CUnitUIManager 加载 UnitUI.slk：\n"
     "对每行 unit type 调 CUnitUIManager_RecordSet*（含 SetUnitShadow/SetStructureShadow/\n"
     "SetShadowOffset/SetShadowSize/SetShadowOnWater），把 SLK 字段写入 record。\n"
     "record 的关键偏移：+0x4C unitShadow tex, +0x50 buildingShadow tex,\n"
     "+0x80/+0x84 shadow offset XY, +0x88/+0x8C shadow size WH, +0x90 shadowOnWater flag。"),
    ("0x6F3358C0",
     "【CUnit shadow 24】写入 CUnitUIManager record +0x4C (unitShadow texture name)。\n"
     "对应 SLK 字段 'unitShadow'。"),
    ("0x6F335A00",
     "【CUnit shadow 24】写入 CUnitUIManager record +0x50 (buildingShadow texture name)。\n"
     "对应 SLK 字段 'buildingShadow' —— 这是用户痛点的核心字段（建筑预渲染贴花阴影名）。"),
]

print("== rename ==")
res = use("rename", {"batch": {"func": [{"addr": a, "name": n} for a, n in RENAMES]}})
text = json.dumps(res.get("result", res.get("error", {})))
print(text[:3000])

print("\n== comment ==")
res = use("set_comments", {"items": [{"addr": a, "comment": c} for a, c in COMMENTS]})
text = json.dumps(res.get("result", res.get("error", {})))
print(text[:2000])
