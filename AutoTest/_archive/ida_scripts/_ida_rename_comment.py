"""把 24_cdoodads_static_shadow_upstream 研究结论写回 IDA：
   - 给 5 个 CDoodads 调度器命名 + 中文注释
   - 给 0x74D500 的 a6 mask bit1/bit2 写注释
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


# 1. rename
RENAMES = [
    ("0x6F74D500", "CDoodads_CreateDoodadAndActivate"),
    ("0x6F751290", "CDoodads_DestroyDoodadAtIndex"),
    ("0x6F759880", "CDoodads_EnableFeatures"),
    ("0x6F7599F0", "CDoodads_DisableFeatures"),
    ("0x6F75C5F0", "CDoodads_SetTodAndRefreshStamp"),
]

# 2. comments
COMMENTS = [
    ("0x6F74D500",
     "【CDoodads 24】对象创建并激活整套阴影状态。\n"
     "char a6 是 War3 自带的“跳过 stamp 路径”位掩码：\n"
     "  bit 0 (a6&1) -> 跳过 vt[13] basic init\n"
     "  bit 1 (a6&2) -> 跳过 ShadowPath_StaticStamp_Toggle + ToggleStaticStampFromObject\n"
     "  bit 2 (a6&4) -> 跳过 ToggleEmitterStamp\n"
     "  bit 3 (a6&8) -> 走 vt+ pose 标记\n"
     "  bit 4 (a6&16)-> 跳过 vt+248\n"
     "现有 5 处调用全部传 a6=0；项目应在 mode>=1 时 hook 注入 a6 |= 0x06，\n"
     "即可在不动 RegisterImage / ListA / ListB 任何末端的前提下，\n"
     "干净地关闭装饰物/可破坏物/树木的静态贴花阴影。\n"
     "详见 docs/research/war3_render_issues/24_cdoodads_static_shadow_upstream"),
    ("0x6F751290",
     "【CDoodads 24】销毁 doodad 并反向关闭 StaticStamp/Emitter/StaticStampFromObject。"),
    ("0x6F759880",
     "【CDoodads 24】按 mask 重新激活 doodad 阴影/lifecycle 子集。\n"
     "mask: bit0=StaticStamp, bit1=basic, bit2=RefreshStampScale, bit3=full reactivate, bit4=vt[256]"),
    ("0x6F7599F0",
     "【CDoodads 24】按 mask 关闭 doodad 阴影/lifecycle 子集；与 EnableFeatures 配对。"),
    ("0x6F75C5F0",
     "【CDoodads 24】TOD 变化驱动 stamp 重写入口：\n"
     "Disable -> ShadowPath_StaticStamp_Toggle(0) -> 改写 alpha -> ShadowPath_StaticStamp_Toggle(1) -> Enable.\n"
     "项目要根治静态阴影必须连同 0x74D500 一起 hook。"),
    ("0x6F74E420",
     "【CDoodads 24】静态阴影按贴图名直写：ReplaceableTextures\\Shadows\\<name>\n"
     "走 ShadowStamp_WriteByName -> WriteCore -> 字节级 ListA 网格修改 + dirty rect"),
    ("0x6F74DB30",
     "【CDoodads 24】对象自带阴影矩形（vt[48..51]）-> RegisterImage(type=0) -> stamp index 写到 entry+136"),
    ("0x6F74DE40",
     "【CDoodads 24】emitter 半径 + 资源路径 -> RegisterImage(type=4) -> stamp index 写到 entry+144"),
]

print("== rename ==")
res = use("rename", {"batch": {"func": [{"addr": a, "name": n} for a, n in RENAMES]}})
print(json.dumps(res.get("result", res.get("error", {})))[:2000])

print("\n== comment ==")
res = use("set_comments", {"items": [{"addr": a, "comment": c} for a, c in COMMENTS]})
print(json.dumps(res.get("result", res.get("error", {})))[:2000])
