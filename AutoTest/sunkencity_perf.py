"""SunkenCity 30s 性能测试 — 看首帧暴降是否缓解。"""
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import DEFAULT_SANDBOX_ROOT, run_quick_autotest

SUNKEN_CITY_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "ShadowTest" / "SunkenCity.w3x"

print("\n========== SunkenCity 30s ==========")
r = run_quick_autotest(
    map_path=str(SUNKEN_CITY_MAP),
    sample_duration_sec=30,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
rep = r.get('report', {}) or {}
print(f"  ok={r.get('ok')} stage={r.get('stage')}")
for k in ['avgFps', 'maxFrameTimeMs', 'p99CpuMs', 'p95CpuMs', 'frameCount',
          'avgFrameTimeMs', 'avgGpuTimeMs', 'avgMainThreadCpuMs']:
    print(f"  {k}={rep.get(k, 0)}")

# Key indicator: maxFrameTimeMs - first frame stutter signature
mft = rep.get('maxFrameTimeMs', 0)
print()
print(f"  maxFrameTimeMs = {mft:.2f} ms")
if mft >= 50:
    print(f"    🚨 SPIKE detected (≥50ms)")
elif mft >= 30:
    print(f"    ⚠️  Moderate spike (30-50ms)")
else:
    print(f"    ✅ No major spike (<30ms)")
