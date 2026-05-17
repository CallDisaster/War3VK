"""批量搜索关键 RTTI 类，输出名字+地址列表。"""
import json, urllib.request, sys

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def search(pattern, limit=200):
    out = call("tools/call", {"name": "find_regex",
                              "arguments": {"pattern": pattern, "limit": limit}})
    if "error" in out:
        return []
    txt = out["result"]["content"][0]["text"]
    j = json.loads(txt)
    return j.get("matches", [])


KEYWORDS = [
    "Doodad", "Destruct", "Blight", "Decal", "Shadow", "Splat", "Stamp",
    "WorldObjects", "Clip", "Tree", "Decor", "Building", "Item", "Effect",
    "Sprite", "Platform", "Bridge", "Wall", "Mine", "Footprint",
]
seen = set()
allmatches = []
for kw in KEYWORDS:
    for m in search(f"AVC{kw}"):
        if m["addr"] not in seen:
            seen.add(m["addr"])
            allmatches.append(m)

allmatches.sort(key=lambda x: int(x["addr"], 16))
for m in allmatches:
    print(m["addr"], m["string"])
print(f"\nTotal: {len(allmatches)}")
