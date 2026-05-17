"""搜 mov reg, [reg+0x50] / mov reg, [reg+0x4C] 风格的 8B XX 50 / 8B XX 4C 字节序列。

x86 指令格式：mov r32, m32
  opcode = 0x8B
  ModR/M:
    reg field 任意（目标寄存器，3 bits）
    mod=01 表示 disp8（[reg+disp8]）
    rm 任意（基址寄存器，3 bits，但 rm=4 表示 SIB）
  → ModR/M byte 取值范围：
      mod=01 (bits 7-6 = 01)，所以 0x40 ~ 0x7F
      但若 rm=0b100 (4)，下一字节是 SIB；rm=0b101 (5) 时仍是 ebp 基
  读 [edi+disp8]: ModR/M = mod=01, reg=任, rm=7 → 高 2 位 01, 低 3 位 111
      一字节后跟 disp8
  常见的：
    [eax+disp8]: ModR/M low3=000, high2=01 → byte=0x40+8*reg → 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78
    [edi+disp8]: 0x47, 0x4F, 0x57, 0x5F, 0x67, 0x6F, 0x77, 0x7F
    [esi+disp8]: 0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x76, 0x7E
    [ecx+disp8]: 0x41, 0x49, 0x51, 0x59, 0x61, 0x69, 0x71, 0x79
  目标是任意 reg(0..7)，所以掩码 reg 字段。
"""
import json, urllib.request

URL = "http://127.0.0.1:13337/mcp"


def call(method, params=None):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}}
    req = urllib.request.Request(
        URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read())


def find_bytes(pattern, limit=2000):
    out = call("tools/call", {"name": "find_bytes",
                              "arguments": {"patterns": pattern, "limit": limit}})
    txt = out["result"]["content"][0]["text"]
    j = json.loads(txt)
    if isinstance(j, list):
        out = []
        for entry in j:
            out.extend(entry.get("matches", []))
        return out
    return j.get("matches", [])


# 我们要找：mov reg, [edi+0x4C]，即字节序列 8B 47/4F/57/5F/67/6F/77/7F  4C
# 用通配 ?? 匹配第二字节
PATTERNS = [
    "8B ?? 4C",  # [reg+0x4C] = +76 (unitShadow)
    "8B ?? 50",  # [reg+0x50] = +80 (buildingShadow)
]
results = {}
for p in PATTERNS:
    matches = find_bytes(p, limit=500)
    results[p] = matches
    print(f"=== {p} : {len(matches)} matches ===")

# 把结果交叉过滤：只保留 ModR/M low3=4..7 (eax/ecx/edx/ebx) 不太具备指示性，
# 暂且全部输出，但只看每条所在函数。
# 输出按函数聚合
def summarize(matches):
    by_fn = {}
    for m in matches:
        addr = m.get("addr", "?")
        # 没有 fn 直接跳过
        fn = m.get("fn") or {}
        name = fn.get("name", "?") if fn else "?"
        by_fn.setdefault(name, []).append(addr)
    return sorted(by_fn.items(), key=lambda x: -len(x[1]))


for p, ms in results.items():
    print(f"\n### {p} top fns ###")
    for fn, addrs in summarize(ms)[:30]:
        print(f"  {fn}: {len(addrs)} sites — first {addrs[:3]}")
