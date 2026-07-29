#!/usr/bin/env python3
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from war3_autotest_mcp import run_quick_autotest
name = sys.argv[1]
profile = sys.argv[2] if len(sys.argv)>2 else ""
disable = sys.argv[3] if len(sys.argv)>3 else ""
print("RUN", name, profile, disable, flush=True)
r = run_quick_autotest(
    map_path=r"E:\\Work\\War3\\Maps\\ShadowTest\\光影测试(高压).w3x",
    sample_duration_sec=12,
    ready_timeout_sec=60,
    use_isolated_desktop=False,
    deploy_d3d9_before_launch=False,
    profile=profile,
    disable_modules=disable,
)
rep = r.get("report") or {}
row = {
  "name": name, "ok": r.get("ok"), "stage": r.get("stage"),
  "avgFps": rep.get("avgFps"), "avgMt": rep.get("avgMainThreadCpuMs"),
  "avgGpu": rep.get("avgGpuTimeMs"), "avgUntracked": rep.get("avgUntrackedActiveCpuMs"),
  "error": r.get("error"),
}
print(json.dumps(row, ensure_ascii=False), flush=True)
Path("artifacts/phaseB_one_"+name+".json").write_text(json.dumps(row, indent=2), encoding="utf-8")
