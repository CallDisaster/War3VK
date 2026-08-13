from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
POLICY = ROOT / "src/d3d9/war3/tools/war3_perf_history_policy.h"
MONITOR_H = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.h"
MONITOR_CPP = ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp"
MCP = ROOT / "AutoTest/war3_autotest_mcp.py"


def main() -> int:
    policy = POLICY.read_text(encoding="utf-8")
    monitor_h = MONITOR_H.read_text(encoding="utf-8")
    monitor_cpp = MONITOR_CPP.read_text(encoding="utf-8")
    mcp = MCP.read_text(encoding="utf-8")

    assert re.search(r"kWar3PerfHistoryFrameLimit32\s*=\s*4000u", policy)
    assert "std::min(requested, kWar3PerfHistoryFrameLimit32)" in policy
    assert "m_maxHistorySize = ClampWar3PerfHistoryFrames(size);" in monitor_h
    assert monitor_cpp.count("ClampWar3PerfHistoryFrames(") >= 2
    assert 'setdefault("DXVK_WAR3_PERF_HISTORY_FRAMES", "4000")' in mcp
    assert 'setdefault("DXVK_WAR3_PERF_HISTORY_FRAMES", "7200")' not in mcp

    print("perf history x86 bound static contract: 6/6")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
