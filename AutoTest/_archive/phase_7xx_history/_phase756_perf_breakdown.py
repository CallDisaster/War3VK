"""开 SubmitBreakdown 看 Populate 内部分布。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x",
    sample_duration_sec=20,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    include_sections_in_report=True,
    section_top_n=50,
    env_overrides_json=json.dumps({
        "DXVK_WAR3_SEMANTIC_SUBMIT_BREAKDOWN": "1",
    }),
)
print(json.dumps(result, indent=2, ensure_ascii=False))
