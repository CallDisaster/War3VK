#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""2026-07-22 性能计时体系重建验证（P6 协议 + P4-1 crash-gate）。

用法（必须在仓库根目录运行，先部署新 DLL 到 E:\\Work\\War3\\d3d9.dll）：
    py -X utf8 AutoTest/run_perf_rebuild_validation.py gate       # P4-1 hot-shadow crash-gate
    py -X utf8 AutoTest/run_perf_rebuild_validation.py monitor    # P6-1 monitor on/off
    py -X utf8 AutoTest/run_perf_rebuild_validation.py monitor-reverse # P6-1 off/on 反序复核
    py -X utf8 AutoTest/run_perf_rebuild_validation.py detail     # P6-1 detail(2) / frame(1)
    py -X utf8 AutoTest/run_perf_rebuild_validation.py detail-reverse # P6-1 反序
    py -X utf8 AutoTest/run_perf_rebuild_validation.py r08        # P6-3 修复后的 R08（shadow+postfx 关）
    py -X utf8 AutoTest/run_perf_rebuild_validation.py resource   # P6-2 semantic off vs on
    py -X utf8 AutoTest/run_perf_rebuild_validation.py all
"""
import json
import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
os.chdir(REPO_ROOT)
sys.path.insert(0, str(REPO_ROOT / "AutoTest"))
from war3_autotest_mcp import run_quick_autotest

WAR3_DIR = r"E:\Work\War3"
HIGH_MAP = str(Path(WAR3_DIR) / "Maps" / "ShadowTest" / "光影测试(高压).w3x")
LOW_MAP = str(Path(WAR3_DIR) / "Maps" / "ShadowTest" / "光影测试.w3x")

OUT = str(REPO_ROOT / "AutoTest" / "artifacts" / "perf_rebuild_validation_20260722.json")


def perf_of(r):
    rep = r.get("report", {}) or {}
    return {
        "avgFps": rep.get("avgFps", 0),
        "avgFrameTimeMs": rep.get("avgFrameTimeMs", 0),
        "avgGpuTimeMs": rep.get("avgGpuTimeMs", 0),
        "avgMainThreadCpuMs": rep.get("avgMainThreadCpuMs", 0),
        "avgProcessCpuMs": rep.get("avgProcessCpuMs", 0),
        "avgUnattributedCpuMs": rep.get("avgUnattributedCpuMs", None),
        "frameCount": rep.get("frameCount", 0),
    }


def run(label, **kw):
    print(f"\n========== {label} ==========")
    defaults = dict(
        war3_dir=WAR3_DIR,
        sample_duration_sec=30,
        use_isolated_desktop=True,
        deploy_d3d9_before_launch=False,
    )
    defaults.update(kw)
    t0 = time.time()
    r = run_quick_autotest(**defaults)
    p = perf_of(r)
    print(f"  ok={r.get('ok')} stage={r.get('stage')} elapsed={time.time()-t0:.0f}s")
    if not r.get("ok"):
        failure = r.get("detail", r.get("error", ""))
        print("  failure=" + json.dumps(failure, ensure_ascii=False, default=str))
    for k, v in p.items():
        print(f"  {k}={v}")
    return {"label": label, "ok": r.get("ok"), "stage": r.get("stage"), "perf": p,
            "report": (r.get("report", {}) or {}).get("reportPath", ""),
            "failure": r.get("detail", r.get("error", "")) if not r.get("ok") else ""}


def gate(results):
    # P4-1 crash-gate：阴影全开高压图 + hot-shadow 门，验证门控修复无回归。
    results.append(run(
        "GATE high-pressure hot-shadow",
        map_path=HIGH_MAP,
        require_hot_shadow_frame=True,
    ))


def monitor(results):
    # P6-1：PerfMonitor 本身保持开启以便两侧都能导出同口径报告；仅关闭
    # Hook timing 层。DXVK_WAR3_PERF_MONITOR=0 不会生成报告，不能把缺失值 0
    # 误当成真实帧时。
    results.append(run(
        "P6-1a Hook trace ON (PERF_LEVEL=1)",
        map_path=LOW_MAP,
    ))
    results.append(run(
        "P6-1b Hook trace OFF (PERF_LEVEL=0)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "0"}),
    ))


def monitor_reverse(results):
    # 与 monitor 完全相同，仅倒置运行顺序，用于排除首轮预热、温度和地图
    # 稳态漂移被误算成 Hook trace 自耗。
    results.append(run(
        "P6-1b Hook trace OFF (PERF_LEVEL=0)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "0"}),
    ))
    results.append(run(
        "P6-1a Hook trace ON (PERF_LEVEL=1)",
        map_path=LOW_MAP,
    ))


def detail(results):
    # PERF_LEVEL=1 是默认低频帧级账本；完整动态 Hook 树必须显式使用 2。
    # 两侧都生成报告，差值是 detail 观察器税，不得混入业务模块结论。
    results.append(run(
        "P6-1c Frame trace (PERF_LEVEL=1)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "1"}),
    ))
    results.append(run(
        "P6-1d Full Hook tree (PERF_LEVEL=2)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "2"}),
    ))


def detail_reverse(results):
    results.append(run(
        "P6-1d Full Hook tree (PERF_LEVEL=2)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "2"}),
    ))
    results.append(run(
        "P6-1c Frame trace (PERF_LEVEL=1)",
        map_path=LOW_MAP,
        env_overrides_json=json.dumps({"DXVK_WAR3_PERF_LEVEL": "1"}),
    ))


def r08(results):
    # P6-3：修复后的 R08——接管核心、阴影/后处理关闭。
    # 预期：SemanticScene/Populate 不再出现 22-24ms 孤儿慢路径（P4-1 修复）。
    results.append(run(
        "P6-3 R08-fixed takeover-core-only (shadow+postfx off)",
        map_path=LOW_MAP,
        disable_modules="shadow,postfx",
    ))


def resource(results):
    # P6-2：R05 semantic 全关 vs R07 资源层全开，重新锁定资源层成本。
    results.append(run(
        "P6-2a R05 semantic.data OFF",
        map_path=LOW_MAP,
        disable_modules="semantic.data",
    ))
    results.append(run(
        "P6-2b R07 resource layer ON (default)",
        map_path=LOW_MAP,
    ))


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    results = []
    if which in ("gate", "all"):
        gate(results)
    if which in ("monitor", "all"):
        monitor(results)
    if which == "monitor-reverse":
        monitor_reverse(results)
    if which == "detail":
        detail(results)
    if which == "detail-reverse":
        detail_reverse(results)
    if which in ("r08", "all"):
        r08(results)
    if which in ("resource", "all"):
        resource(results)
    with open(OUT, "w", encoding="utf-8") as fp:
        json.dump(results, fp, ensure_ascii=False, indent=2)
    print(f"\nresults written to {OUT}")
    # 快速结论
    by_label = {r["label"]: r for r in results}
    if "P6-1a Hook trace ON (PERF_LEVEL=1)" in by_label and \
       "P6-1b Hook trace OFF (PERF_LEVEL=0)" in by_label:
        a = by_label["P6-1a Hook trace ON (PERF_LEVEL=1)"]["perf"]["avgFrameTimeMs"]
        b = by_label["P6-1b Hook trace OFF (PERF_LEVEL=0)"]["perf"]["avgFrameTimeMs"]
        print(f"hook trace self-cost ~= {a - b:+.3f} ms/frame (on={a:.3f} off={b:.3f})")
    if "P6-1c Frame trace (PERF_LEVEL=1)" in by_label and \
       "P6-1d Full Hook tree (PERF_LEVEL=2)" in by_label:
        frame = by_label["P6-1c Frame trace (PERF_LEVEL=1)"]["perf"]["avgFrameTimeMs"]
        detail_ms = by_label["P6-1d Full Hook tree (PERF_LEVEL=2)"]["perf"]["avgFrameTimeMs"]
        print(f"detail tree observer tax ~= {detail_ms - frame:+.3f} ms/frame "
              f"(detail={detail_ms:.3f} frame={frame:.3f})")


if __name__ == "__main__":
    main()
