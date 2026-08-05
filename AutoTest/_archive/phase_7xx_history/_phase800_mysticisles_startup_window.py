"""Phase 7.102 / Task 4: 用 record_after_game_started=False（从 launch 第 0 帧开始 perf）
跑 MysticIsles，看启动 + 进图整段过程的 FPS 分布是否有 4-5 秒卡顿"""
import json
import sys
sys.path.insert(0, '.')
from war3_autotest_mcp import run_quick_autotest

result = run_quick_autotest(
    map_path=r"E:\Work\War3\Maps\(4)MysticIsles.w3m",
    sample_duration_sec=20,        # 控制后采样时长
    use_isolated_desktop=True,
    deploy_d3d9_before_launch=False,
    record_after_game_started=False,  # 从 launch 第 0 帧开始
    auto_perf_export_sec=45,          # 把启动期 + 后续 20s 都包含
)
print(json.dumps(result, indent=2, ensure_ascii=False))
