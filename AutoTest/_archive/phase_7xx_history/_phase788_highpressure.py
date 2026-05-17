"""Phase 7.88: 高压地图基线 perf + full trace。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x",
    sample_duration_sec=30,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    env_overrides_json=json.dumps({
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE": "1",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC": "20",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES": "0",
    }),
)
print(json.dumps(result, indent=2, ensure_ascii=False))
