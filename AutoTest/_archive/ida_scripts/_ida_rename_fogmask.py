"""把 24 文档 v3 的 FogMask 写入路径关键发现写回 IDA。"""
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


# Rename newly understood functions
RENAMES = [
    ("0x6F65A140", "CWidget_RegisterFootprintAndShadowMask"),
    ("0x6F514F40", "CUnit_StampBuildingShadowFootprint"),
    ("0x6F66C930", "CUnitUIManager_DispatchFootprintWrite"),
    ("0x6F41B380", "Actor_RuntimeShadowMaskWriter"),
]

COMMENTS = [
    ("0x6F234710",
     "【FogMask shadow 24-v3】★★★ 真正的建筑预渲染贴花阴影写入函数。\n"
     "不通过 ListA/ListB/RegisterImage/ShadowProjector 任何注册池——直接修改\n"
     "CFogMaskTable + CFogOfWarMap 共享的 16-bit mask grid。\n"
     "a3 = type code（16 bit），高 7 bit elevation/height，低 9 bit 是各种 type 标志：\n"
     "  fog-of-war / line-of-sight / blight / path-blocker / building-shadow-footprint。\n"
     "项目历次拦 RegisterImage / StaticStamp / ListA/B 都不能消除建筑阴影，原因就在这里。\n"
     "正确治理路径是 hook 本函数并按 a3 bit 拆分屏蔽，但绝不能整体 return（会同时关 fog/视野）。\n"
     "详见 docs/research/war3_render_issues/24_cdoodads_static_shadow_upstream §4.5"),
    ("0x6F234620",
     "【FogMask shadow 24-v3】单对象写 mask 的 wrapper（在调 0x234710 前补充对象 flags）。\n"
     "由 CDoodads/CUnitUIManager 调度层调用。详见 §4.5"),
    ("0x6F3DB260",
     "【FogMask shadow 24-v3】Actor 运行时控制流写 mask 入口。\n"
     "ShadowPath_ActorController_Update 周边路径触发。详见 §4.5"),
    ("0x6F233E90",
     "【FogMask shadow 24-v3】整体从对象列表重建 mask grid。\n"
     "loadgame / 战争迷雾全图重置 / 视野同步时调用。\n"
     "重建会覆盖所有 hook 单写的清除工作，hook 必须考虑这条路径。详见 §4.5"),
    ("0x6F232060",
     "【FogMask shadow 24-v3】CFogMaskTable_GetOrCreateMask: 按半径取/造一个 footprint 模板。\n"
     "源文件: War3\\Source\\Game\\CFogMaskTable.h"),
    ("0x6F230210",
     "【FogMask shadow 24-v3】CFogMask_BuildNodeAndRangeTable: 构建 16-bit mask 节点表 + 范围表。\n"
     "初始化每个 cell 为 0xFFFF，后续 WriteMaskRegion 按 a3 type code 做 set/clear。"),
    ("0x6F65A140",
     "【FogMask shadow 24-v3】CWidget/CUnit 中央 sync 函数（30+ caller，几乎所有 unit 状态变化）。\n"
     "从 dword_6FBE4238 (gameWar3) + 0x34 (this+13) 拿 mask manager，调 WriteMaskRegion。\n"
     "建筑创建/销毁/移动/状态变化时都会经过这里。详见 §4.5"),
    ("0x6F514F40",
     "【FogMask shadow 24-v3】CUnit lifecycle helper：把建筑 footprint 写入 mask。\n"
     "验证 *(widget+0x0C) == 0x2B5DB42C magic（疑似 CWidget 类型签名）后调 WriteMaskRegion。\n"
     "调用前 *(unit+96) |= 0x400u（这是 ListA 共享 blob mask flag，连接两条系统的桥）。"),
]

print("== rename ==")
res = use("rename", {"batch": {"func": [{"addr": a, "name": n} for a, n in RENAMES]}})
print(json.dumps(res.get("result", res.get("error", {})))[:2500])

print("\n== comment ==")
res = use("set_comments", {"items": [{"addr": a, "comment": c} for a, c in COMMENTS]})
print(json.dumps(res.get("result", res.get("error", {})))[:2000])
