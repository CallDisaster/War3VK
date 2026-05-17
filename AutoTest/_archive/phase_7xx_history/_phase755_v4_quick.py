"""Phase 7.55 v4 quick AutoTest: 验证 CPU memcpy + lazy upload 是否消除"全图抽"。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x",
    sample_duration_sec=20,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,  # 已手动部署
    env_overrides_json=json.dumps({
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE": "1",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MAX_SEC": "15",
        "DXVK_WAR3_SHADOW_POSE_FULL_TRACE_MATRIX_BYTES": "0",
    }),
)
print(json.dumps(result, indent=2, ensure_ascii=False))
