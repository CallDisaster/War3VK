"""IDA MCP 探针：列出可用工具，确认服务可达。"""
import json, urllib.request

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=8) as resp:
        return json.loads(resp.read())


if __name__ == "__main__":
    out = call("tools/list")
    tools = out.get("result", {}).get("tools", [])
    print(f"tools={len(tools)}")
    for t in tools:
        print("-", t.get("name"))
