"""Parse perf log and print top scopes by inclusive CPU."""
import json
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"AutoTest\_phase756_perf5.log"
with open(path, "r", encoding="utf-8") as f:
    j = json.load(f)

r = j["report"]
print(f"=== {path} ===")
print(f"avgFps={r['avgFps']:.2f}  frameMs={r['avgFrameTimeMs']:.2f}  gpuMs={r['avgGpuTimeMs']:.2f}  mainCpuMs={r['avgMainThreadCpuMs']:.2f}  procCpuMs={r['avgProcessCpuMs']:.2f}")
print(f"trackedActive={r['avgTrackedActiveCpuMs']:.2f}  untrackedActive={r['avgUntrackedActiveCpuMs']:.2f}")
print()

sb = r.get("sectionBreakdown")
if isinstance(sb, dict):
    blk = sb
elif isinstance(sb, list) and sb:
    blk = sb[0]
else:
    blk = None
if blk:
    print(f"=== Top by Inclusive CPU ({blk.get('count', 0)} scopes) ===")
    for entry in blk.get("topByInclusiveCpu", [])[:25]:
        scope = entry.get("name", entry.get("path", entry.get("scope", "")))
        avg = entry.get("avgCpuMs", entry.get("avgInclusiveCpuMs", 0))
        self_ms = entry.get("avgSelfCpuMs", 0)
        calls = entry.get("calls", 0)
        print(f"  {avg:7.3f}ms (self={self_ms:6.3f}ms) calls={calls:>7}  {scope}")
    print()
    print(f"=== Top by Self CPU ===")
    for entry in blk.get("topBySelfCpu", [])[:25]:
        scope = entry.get("name", entry.get("path", entry.get("scope", "")))
        self_ms = entry.get("avgSelfCpuMs", 0)
        calls = entry.get("calls", 0)
        print(f"  {self_ms:7.3f}ms calls={calls:>7}  {scope}")

mlst = r.get("mainLoopStagesTop", [])
if mlst:
    print()
    print(f"=== Main Loop Stages Top ===")
    for s in mlst[:15]:
        label = s.get("label", "")
        avg = s.get("avgMs", 0)
        calls = s.get("calls", 0)
        print(f"  {avg:7.3f}ms calls={calls:>7}  {label}")
