"""Phase 7.137: 双图性能基线（不用 isolated desktop，直接前台跑）"""
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import DEFAULT_SANDBOX_ROOT, run_quick_autotest

HIGH_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "ShadowTest" / "光影测试(高压).w3x"
LOW_MAP = DEFAULT_SANDBOX_ROOT / "Maps" / "ShadowTest" / "光影测试.w3x"

def get_perf(r):
    rep = r.get('report', {}) or {}
    return {
        'avgFps': rep.get('avgFps', 0),
        'avgFrameTimeMs': rep.get('avgFrameTimeMs', 0),
        'avgGpuTimeMs': rep.get('avgGpuTimeMs', 0),
        'avgMainThreadCpuMs': rep.get('avgMainThreadCpuMs', 0),
        'avgProcessCpuMs': rep.get('avgProcessCpuMs', 0),
        'frameCount': rep.get('frameCount', 0),
    }

print("\n========== HIGH PRESSURE (光影测试-高压) 30s [NO isolated desktop] ==========")
r1 = run_quick_autotest(
    map_path=str(HIGH_MAP),
    sample_duration_sec=30,
    use_isolated_desktop=False,  # 关键：不隔离桌面
    deploy_d3d9_before_launch=False,
)
p1 = get_perf(r1)
print(f"  ok={r1.get('ok')} stage={r1.get('stage')}")
for k,v in p1.items():
    print(f"  {k}={v}")

print("\n========== LOW PRESSURE (光影测试) 30s [NO isolated desktop] ==========")
r2 = run_quick_autotest(
    map_path=str(LOW_MAP),
    sample_duration_sec=30,
    use_isolated_desktop=False,
    deploy_d3d9_before_launch=False,
)
p2 = get_perf(r2)
print(f"  ok={r2.get('ok')} stage={r2.get('stage')}")
for k,v in p2.items():
    print(f"  {k}={v}")

print("\n========== TARGET ==========")
fps_h = p1.get('avgFps', 0)
fps_l = p2.get('avgFps', 0)
print(f"  High pressure: avgFps={fps_h:.2f}, target>=85: {'PASS' if fps_h >= 85 else 'FAIL'}")
print(f"  Low pressure:  avgFps={fps_l:.2f}, target>=120: {'PASS' if fps_l >= 120 else 'FAIL'}")
