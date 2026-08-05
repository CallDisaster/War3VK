"""IDA MCP schema 探针。"""
import json, urllib.request, sys

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


if __name__ == "__main__":
    out = call("tools/list")
    tools = out.get("result", {}).get("tools", [])
    targets = sys.argv[1:] or [
        "decompile", "disasm", "callees", "xrefs_to",
        "lookup_funcs", "find_regex", "get_string", "find_bytes",
        "rename", "set_comments", "infer_types", "set_type",
    ]
    for t in tools:
        if t.get("name") in targets:
            print("=", t.get("name"), "=")
            print(json.dumps(t.get("inputSchema", {}), indent=2))
