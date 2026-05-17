"""Check cascade cull counters from trace."""
import json, sys
from pathlib import Path

p = Path(sys.argv[1])
frames = [json.loads(l) for l in p.open("r", encoding="utf-8") if '"shadowPoseFullTraceFrame"' in l]
ks = [f.get("keyStats", {}) for f in frames]
fields = [
    "semanticSceneShadowMapCascade0DrawnCount",
    "semanticSceneShadowMapCascade1DrawnCount",
    "semanticSceneShadowMapCascade2DrawnCount",
    "semanticSceneShadowMapCascade3DrawnCount",
    "semanticSceneShadowMapCascade0CulledCount",
    "semanticSceneShadowMapCascade1CulledCount",
    "semanticSceneShadowMapCascade2CulledCount",
    "semanticSceneShadowMapCascade3CulledCount",
    "semanticSceneShadowMapPreparedDrawCount",
]
n = max(1, len(ks))
for f in fields:
    v = sum(int(k.get(f, 0) or 0) for k in ks) / n
    print(f"  {f:<55s} = {v:.1f}")
