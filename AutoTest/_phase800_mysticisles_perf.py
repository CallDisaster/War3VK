"""Phase 7.102 / Task 4：(4)MysticIsles 开局 perf 30s。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\(4)MysticIsles.w3m",
    sample_duration_sec=30,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
)
print(json.dumps(result, indent=2, ensure_ascii=False))
