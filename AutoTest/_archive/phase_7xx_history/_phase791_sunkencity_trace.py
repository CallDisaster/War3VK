"""Phase 7.91: SunkenCity.w3x 全追踪诊断 — 用户报告某些视角 1FPS 卡顿。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\SunkenCity.w3x",
    sample_duration_sec=60,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    env_overrides_json=json.dumps({
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE": "1",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC": "45",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES": "0",
        "DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN": "1",
    }),
)
print(json.dumps(result, indent=2, ensure_ascii=False))
