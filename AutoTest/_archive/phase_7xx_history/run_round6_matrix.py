#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
第六轮组合兼容矩阵测试：
1) 自动切换渲染层/逻辑层关键开关；
2) 每个组合执行编译 + AutoTest；
3) 输出结构化结果，定位不兼容组合。
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from war3_autotest_mcp import run_quick_autotest


ROOT = Path(__file__).resolve().parents[1]
CONFIG_H = ROOT / "src/d3d9/war3/core/war3_internal_test_config.h"
OUT_DIR = ROOT / "AutoTest" / "artifacts" / "round6_matrix"
OUT_DIR.mkdir(parents=True, exist_ok=True)


@dataclass
class Combo:
  name: str
  desc: str
  flags: Dict[str, Any]


def _replace_inline_constant(text: str, key: str, value: Any) -> str:
  if isinstance(value, bool):
    repl_value = "true" if value else "false"
  else:
    repl_value = str(value)

  pattern = re.compile(
      rf"(inline\s+constexpr\s+(?:bool|uint32_t|int32_t|int)\s+{re.escape(key)}\s*=\s*)([^;]+)(;)"
  )

  def _sub(m: re.Match[str]) -> str:
    return f"{m.group(1)}{repl_value}{m.group(3)}"

  new_text, n = pattern.subn(_sub, text, count=1)
  if n == 0:
    raise RuntimeError(f"未找到常量: {key}")
  return new_text


def apply_flags(flags: Dict[str, Any]) -> None:
  text = CONFIG_H.read_text(encoding="utf-8")
  for k, v in flags.items():
    text = _replace_inline_constant(text, k, v)
  CONFIG_H.write_text(text, encoding="utf-8")


def run_ninja() -> Dict[str, Any]:
  p = subprocess.run(
      ["ninja", "-C", "build32"],
      cwd=str(ROOT),
      capture_output=True,
      text=True,
  )
  return {
      "ok": p.returncode == 0,
      "returncode": p.returncode,
      "stdout": p.stdout[-4000:],
      "stderr": p.stderr[-4000:],
  }


def run_autotest() -> Dict[str, Any]:
  return run_quick_autotest(
      war3_dir=r"E:\Work\War3",
      map_path=r"E:\Work\War3\Maps\光影测试.w3x",
      ready_timeout_sec=150,
      sample_duration_sec=18,
      windowed=False,
      opengl=False,
      auto_perf_record=True,
      auto_perf_export_sec=8,
      deploy_d3d9_before_launch=True,
      build_d3d9_path=r"build32/src/d3d9/d3d9.dll",
      enforce_video_baseline=True,
      baseline_width=2560,
      baseline_height=1440,
      baseline_refresh_rate=59,
      include_sections_in_report=True,
      section_top_n=40,
      avoid_focus_on_stop=True,
  )


def compact_result(res: Dict[str, Any]) -> Dict[str, Any]:
  report = res.get("report", {}) if isinstance(res, dict) else {}
  shot = res.get("screenshotSize", {}) if isinstance(res, dict) else {}
  return {
      "ok": bool(res.get("ok", False)),
      "stage": res.get("stage"),
      "reportPath": report.get("reportPath"),
      "avgFps": report.get("avgFps"),
      "avgFrameTimeMs": report.get("avgFrameTimeMs"),
      "avgGpuTimeMs": report.get("avgGpuTimeMs"),
      "avgMainThreadCpuMs": report.get("avgMainThreadCpuMs"),
      "avgProcessCpuMs": report.get("avgProcessCpuMs"),
      "activeFrameTimeMs": report.get("activeFrameTimeMs"),
      "cpuCoveragePct": report.get("cpuCoveragePct"),
      "jank16": report.get("jank16"),
      "jank33": report.get("jank33"),
      "screenshotSize": shot,
      "warnings": res.get("warnings", []),
      "ready": res.get("ready"),
      "stop": res.get("stop"),
  }


def main() -> int:
  original = CONFIG_H.read_text(encoding="utf-8")

  # 默认渲染优化包（与第五轮一致）
  render_base = {
      "kNativeQueueTakeoverConservativeEnabled": True,
      "kNativeDispatchTagStageCacheEnabled": True,
      "kShadowAdaptiveMapUpdateEnabled": True,
      "kNativeDispatchLocalContextMergeEnabled": False,
      "kNativeRenderQueueDiagnosticStatsEnabled": False,
  }

  # 默认逻辑优化包（性能模式）
  logic_base = {
      "kNativeMainLoopDeepPhaseHookEnabled": False,
      "kNativeMainThreadWaitHookEnabled": False,
      "kNativeMainThreadWaitDeepHookEnabled": False,
      "kNativeJassVmPerfTrackingEnabled": False,
      "kNativeJassVmDeepHooksEnabled": False,
      "kNativeJassOpBudgetAdaptiveEnabled": False,
  }

  combos: List[Combo] = [
      Combo(
          name="C0_base",
          desc="渲染优化基础包 + 逻辑优化关闭",
          flags={**render_base, **logic_base},
      ),
      Combo(
          name="C1_render_local_merge",
          desc="在基础包上开启 DispatchLocalContextMerge",
          flags={**render_base, **logic_base, "kNativeDispatchLocalContextMergeEnabled": True},
      ),
      Combo(
          name="C2_logic_jass_adaptive",
          desc="在基础包上开启 JASS 自适应预算",
          flags={**render_base, **logic_base, "kNativeJassOpBudgetAdaptiveEnabled": True},
      ),
      Combo(
          name="C3_render_local_merge_plus_jass_adaptive",
          desc="渲染局部合并 + JASS 自适应预算",
          flags={
              **render_base,
              **logic_base,
              "kNativeDispatchLocalContextMergeEnabled": True,
              "kNativeJassOpBudgetAdaptiveEnabled": True,
          },
      ),
      Combo(
          name="C4_logic_mainloop_deep",
          desc="基础包 + MainLoop 深层阶段 Hook",
          flags={**render_base, **logic_base, "kNativeMainLoopDeepPhaseHookEnabled": True},
      ),
      Combo(
          name="C5_logic_wait_hooks",
          desc="基础包 + 主线程 Wait Hook(含 deep)",
          flags={
              **render_base,
              **logic_base,
              "kNativeMainThreadWaitHookEnabled": True,
              "kNativeMainThreadWaitDeepHookEnabled": True,
          },
      ),
      Combo(
          name="C6_logic_jass_deep_hooks",
          desc="基础包 + JASS 深层 Hook（高风险）",
          flags={**render_base, **logic_base, "kNativeJassVmDeepHooksEnabled": True},
      ),
      Combo(
          name="C7_all_optimizations",
          desc="渲染局部合并 + JASS 自适应 + MainLoop 深层 + Wait Hook",
          flags={
              **render_base,
              **logic_base,
              "kNativeDispatchLocalContextMergeEnabled": True,
              "kNativeJassOpBudgetAdaptiveEnabled": True,
              "kNativeMainLoopDeepPhaseHookEnabled": True,
              "kNativeMainThreadWaitHookEnabled": True,
              "kNativeMainThreadWaitDeepHookEnabled": True,
          },
      ),
  ]

  results: List[Dict[str, Any]] = []
  try:
    for combo in combos:
      apply_flags(combo.flags)
      build = run_ninja()
      row: Dict[str, Any] = {
          "name": combo.name,
          "desc": combo.desc,
          "flags": combo.flags,
          "build": build,
      }
      if build["ok"]:
        run = run_autotest()
        row["autotest"] = compact_result(run)
      else:
        row["autotest"] = {"ok": False, "stage": "build_failed"}
      results.append(row)
      print(
          json.dumps(
              {
                  "name": combo.name,
                  "build_ok": build["ok"],
                  "autotest_ok": row["autotest"].get("ok", False),
                  "avgFps": row["autotest"].get("avgFps"),
                  "reportPath": row["autotest"].get("reportPath"),
              },
              ensure_ascii=False,
          )
      )
  finally:
    # 恢复原始配置，避免矩阵中间状态残留。
    CONFIG_H.write_text(original, encoding="utf-8")

  out_json = OUT_DIR / "round6_matrix_results.json"
  out_json.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
  print(f"[round6] wrote {out_json}")
  return 0


if __name__ == "__main__":
  raise SystemExit(main())

