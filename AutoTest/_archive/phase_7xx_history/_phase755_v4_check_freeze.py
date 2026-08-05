"""检查 CombinedHash 是否还冻结。"""
import json
import sys

trace = sys.argv[1]
hashes = []
with open(trace, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            evt = json.loads(line)
        except Exception:
            continue
        if evt.get("type") != "shadowPoseFullTraceFrame":
            continue
        ks = evt.get("keyStats", {})
        h = ks.get("semanticSceneSubmittedSkinnedPaletteCombinedHash")
        if h is not None:
            hashes.append(h)

print(f"Total samples: {len(hashes)}")
print(f"Distinct: {len(set(hashes))}")

# 找连续 run
runs = []
if hashes:
    cur = 0
    for i in range(1, len(hashes)):
        if hashes[i] != hashes[i-1]:
            runs.append((cur, i - cur, hashes[i-1]))
            cur = i
    runs.append((cur, len(hashes) - cur, hashes[-1]))

runs.sort(key=lambda r: -r[1])
print("\nTop 10 longest runs (start, length, hash):")
for start, length, h in runs[:10]:
    h_str = str(h) if not isinstance(h, str) else h
    print(f"  [{start}..{start+length-1}] len={length} hash={h_str[:32]}")

frozen3 = sum(1 for r in runs if r[1] >= 3)
frozen6 = sum(1 for r in runs if r[1] >= 6)
print(f"\nRuns len>=3: {frozen3}")
print(f"Runs len>=6: {frozen6}")
