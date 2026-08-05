"""Phase 7.56 性能基线测试。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\光影测试.w3x",
    sample_duration_sec=30,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    include_sections_in_report=True,
    section_top_n=30,
    env_overrides_json=json.dumps({}),  # no full trace, focus on perf
)
print(json.dumps(result, indent=2, ensure_ascii=False))
