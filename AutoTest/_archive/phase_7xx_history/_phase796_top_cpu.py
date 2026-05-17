"""提取最近的 perf 报告里的 top CPU 段"""
import json
import re
import sys
from pathlib import Path

log_dir = Path(r"E:\Work\War3\WarVK\Log")
reports = sorted(log_dir.glob("war3_perf_report_auto_*.html"), key=lambda p: p.stat().st_mtime, reverse=True)
if not reports:
    print("no report")
    sys.exit(1)

target = reports[0]
print(f"report: {target.name}")
content = target.read_text(encoding="utf-8")

tops = []
for line in content.split("\n"):
    line = line.strip()
    if "avgCpuMs" not in line or "totalCpuMs" not in line:
        continue
    match = re.search(r"\{(.*)\}", line)
    if not match:
        continue
    try:
        data = json.loads("{" + match.group(1) + "}")
    except json.JSONDecodeError:
        continue
    tops.append(data)

tops.sort(key=lambda x: -x.get("avgCpuMs", 0))
for top in tops[:30]:
    avg = top.get("avgCpuMs", 0)
    if avg < 0.05:
        continue
    name = top.get("name", "?")
    calls = top.get("calls", 0)
    cpf = top.get("callsPerFrame", 0)
    print(f"  {name:60s}  avgCpuMs={avg:7.3f}  callsPerFrame={cpf:7.2f}")
