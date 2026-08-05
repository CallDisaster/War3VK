"""仅查询当前 War3 实例的 DispatchToShape counter"""
import sys
import subprocess
sys.path.insert(0, '.')
from war3_autotest_mcp import _control_plane_request

# Find War3 pid via tasklist
out = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq War3.exe', '/NH'], text=True, errors='ignore')
pid = 0
for line in out.splitlines():
    line = line.strip()
    if line.lower().startswith('war3.exe'):
        parts = line.split()
        if len(parts) >= 2:
            try:
                pid = int(parts[1])
                break
            except ValueError:
                pass
if pid == 0:
    print("[probe] no War3 process running")
    sys.exit(1)
print(f"[probe] War3 pid={pid}")

resp = _control_plane_request(pid=pid, command="get_shadow_runtime_summary", timeout_sec=10.0)
summary = resp.get("result", resp.get("summary", resp))
if not isinstance(summary, dict):
    print(f"unexpected summary: {type(summary)}")
    print(resp)
    sys.exit(1)

print("\n=== DispatchToShape (Phase 7.116) ===")
keys = [
    "dispatchToShapeEnterCount",
    "dispatchToShapeRejectedCount",
    "dispatchToShapeFromRebuildMaskCount",
    "dispatchToShapeFromShadowSetupCount",
    "dispatchToShapeFromOtherCallerCount",
]
for k in keys:
    v = summary.get(k, "MISSING")
    print(f"  {k:55s} {v}")

print("\n=== Sanity (其他 mask 写入仍正常) ===")
keys2 = [
    "writeMaskRegionEnterCount",
    "writeMaskRegionPassFogCount",
    "writeMaskRegionPassLosCount",
    "writeMaskRegionPassPathCount",
    "writeMaskRegionPassOtherCount",
    "semanticSceneSubmittedSkinned",
    "semanticSceneShadowMapDrawnCasters",
    "semanticSceneRejectedPathBlockerCount",
]
for k in keys2:
    v = summary.get(k, "MISSING")
    print(f"  {k:55s} {v}")
