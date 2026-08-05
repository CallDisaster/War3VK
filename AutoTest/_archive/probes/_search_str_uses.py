"""在所有函数里 disasm 找出某段字符串地址被 push 的位置。
直接用 find_bytes 搜 little-endian 地址。
"""
import json, urllib.request, sys, struct

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def find_bytes(pattern):
    out = call("tools/call", {"name": "find_bytes", "arguments": {"patterns": pattern, "limit": 200}})
    if "error" in out:
        print("ERR:", out["error"])
        return []
    txt = out["result"]["content"][0]["text"]
    j = json.loads(txt)
    if isinstance(j, list):
        # array of {pattern, matches}
        out = []
        for entry in j:
            out.extend(entry.get("matches", []))
        return out
    return j.get("matches", [])


for name, addr in [("buildingShadow", 0x6FA4E29C), ("unitShadow", 0x6FA4E290)]:
    le = struct.pack("<I", addr).hex().upper()
    pattern = " ".join(le[i:i+2] for i in range(0, 8, 2))
    print(f"\n=== {name} @ 0x{addr:08X}  pattern={pattern} ===")
    matches = find_bytes(pattern)
    for m in matches:
        print(json.dumps(m))
