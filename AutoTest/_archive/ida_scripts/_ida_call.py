"""IDA MCP 通用调用 helper。

用法：
    py AutoTest/_ida_call.py decompile 0x74D500
    py AutoTest/_ida_call.py disasm 0x74D500 80
    py AutoTest/_ida_call.py callees 0x74D500
    py AutoTest/_ida_call.py xrefs_to 0x713250
    py AutoTest/_ida_call.py lookup_funcs 0x74D500
    py AutoTest/_ida_call.py find_regex .*Shadow.*
"""

import json
import sys
import urllib.request

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.loads(resp.read())


def parse_addr(s: str) -> int:
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)

    tool = sys.argv[1]
    args = sys.argv[2:]

    arguments = {}
    if tool == "decompile":
        arguments = {"addr": args[0]}
    elif tool == "disasm":
        arguments = {"addr": args[0]}
        if len(args) > 1:
            arguments["max_instructions"] = int(args[1])
    elif tool == "callees":
        arguments = {"addrs": list(args)}
    elif tool == "xrefs_to":
        arguments = {"addrs": list(args)}
        # default limit raised
        arguments.setdefault("limit", 200)
    elif tool == "lookup_funcs":
        arguments = {"queries": list(args)}
    elif tool == "find_regex":
        arguments = {"pattern": args[0]}
        if len(args) > 1:
            arguments["limit"] = int(args[1])
    elif tool == "get_string":
        arguments = {"addrs": list(args)}
    elif tool == "find_bytes":
        arguments = {"patterns": [args[0]] if not isinstance(args[0], list) else args[0]}
    else:
        # generic: pass remaining args as JSON keyword pairs
        for kv in args:
            k, _, v = kv.partition("=")
            try:
                arguments[k] = json.loads(v)
            except Exception:
                arguments[k] = v

    out = call(
        "tools/call", {"name": tool, "arguments": arguments}
    )
    if "error" in out:
        print(json.dumps(out, indent=2))
        sys.exit(1)

    result = out.get("result", {})
    content = result.get("content", [])
    for item in content:
        if item.get("type") == "text":
            print(item.get("text", ""))


if __name__ == "__main__":
    main()
