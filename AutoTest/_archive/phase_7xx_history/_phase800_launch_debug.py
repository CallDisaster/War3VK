"""Phase 7.116 launch debug"""
import sys, json
sys.path.insert(0, '.')
from war3_autotest_mcp import launch_war3_test, DEFAULT_WAR3_DIR

map_path = r"E:\Work\War3\Maps\ShadowTest\光影测试(高压).w3x"
print(f"DEFAULT_WAR3_DIR: {DEFAULT_WAR3_DIR}")
print(f"map_path: {map_path}")
launch = launch_war3_test(
    war3_dir=str(DEFAULT_WAR3_DIR),
    map_path=map_path,
    use_isolated_desktop=True,
)
print(json.dumps(launch, indent=2, default=str, ensure_ascii=False))
