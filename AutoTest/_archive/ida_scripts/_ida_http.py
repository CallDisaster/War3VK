"""手动调 IDA MCP HTTP JSON-RPC（替代 mcp_ida_pro_mcp_* 失联）"""
import json
import sys
import urllib.request

URL = "http://127.0.0.1:13337/mcp"
HEADERS = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}

def call(method, params=None, req_id=1):
    payload = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}
    req = urllib.request.Request(URL, data=json.dumps(payload).encode("utf-8"),
                                  headers=HEADERS, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            # SSE response: "data: {...}\n\n"
            if body.startswith("event:") or "\ndata:" in body or body.startswith("data:"):
                # parse SSE - take last data line
                for line in body.splitlines():
                    if line.startswith("data:"):
                        data = line[5:].strip()
                        try:
                            return json.dumps(json.loads(data), indent=2, ensure_ascii=False)
                        except Exception:
                            return data
                return body
            return body
    except Exception as e:
        return f"ERR: {e}"

def tool_call(name, args=None):
    return call("tools/call", {"name": name, "arguments": args or {}})

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: _ida_http.py <method-or-tool> [json-args]")
        print("  list_tools                   - list available tools")
        print("  decompile {\"addr\":\"0x6F234710\"}")
        print("  lookup_funcs {\"queries\":\"0x6F234710\"}")
        sys.exit(1)
    method = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    if method == "list_tools":
        print(call("tools/list"))
    elif method == "raw":
        # raw: arg2 = method name, arg3 = params json
        print(call(args.get("method", ""), args.get("params") or {}))
    else:
        print(tool_call(method, args))
