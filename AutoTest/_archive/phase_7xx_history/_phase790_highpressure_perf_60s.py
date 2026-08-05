"""Phase 7.90 60s 高压 perf 减少噪声。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x",
    sample_duration_sec=60,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
print(json.dumps(result, indent=2, ensure_ascii=False))
