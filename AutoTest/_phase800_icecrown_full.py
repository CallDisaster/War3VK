"""Phase 7.105: (12)IceCrown.w3m 启动卡顿 — 默认 profile (full enhancements)。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\(12)IceCrown.w3m",
    sample_duration_sec=20,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    record_after_game_started=False,
    auto_perf_export_sec=60,
)
print(json.dumps(result, indent=2, ensure_ascii=False))
