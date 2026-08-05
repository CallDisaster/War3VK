"""按名字前缀列函数。"""
import json, urllib.request, sys

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def use(name, args):
    return call("tools/call", {"name": name, "arguments": args})


pattern = sys.argv[1] if len(sys.argv) > 1 else "TerrainShadow"
res = use("list_funcs", {"queries": {"filter": f"*{pattern}*", "count": 0}})
text = res["result"]["content"][0]["text"]
print(text)
