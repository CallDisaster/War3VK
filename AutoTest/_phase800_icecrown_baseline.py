"""Phase 7.105: (12)IceCrown.w3m 启动卡顿 A/B 测试。
profile=dxvk_only 关闭所有 War3 渲染增强，看启动期 maxFrameTimeMs 是否变小。"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\(12)IceCrown.w3m",
    sample_duration_sec=20,
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    record_after_game_started=False,    # 从 launch 第 0 帧开始 perf
    auto_perf_export_sec=60,             # 把启动期 + 25s 都包含
    profile="dxvk_only",                 # 关掉 War3 渲染增强
)
print(json.dumps(result, indent=2, ensure_ascii=False))
