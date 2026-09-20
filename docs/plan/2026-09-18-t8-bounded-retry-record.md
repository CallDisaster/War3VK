# T8 有界重试记录（2026-09-18 主线程）

## 判定：**未覆盖**（两个压力段均在 launch 阶段失败，无任何性能数据）

## 1. 执行

- 命令：`py AutoTest/dual_perf_baseline.py`（脚本存在；**进程退出码 = 0**）
- 脚本期望的两张图（源码 `dual_perf_baseline.py:6-7`）：
  ```
  HIGH_MAP = <sandbox>/Maps/ShadowTest/光影测试(高压).w3x
  LOW_MAP  = <sandbox>/Maps/ShadowTest/光影测试.w3x
  ```

## 2. 原始输出（摘录，未改写）

```
========== HIGH PRESSURE 30s ==========
  ok=False stage=launch
  avgFps=0
  avgFrameTimeMs=0
  avgGpuTimeMs=0
  avgMainThreadCpuMs=0
  avgProcessCpuMs=0
  frameCount=0

========== LOW PRESSURE 30s ==========
  ok=False stage=launch
```

## 3. 根因（与 ① 同一条前置）

- 沙箱 `E:\Work\War3_AutoTestSandbox` **没有游戏安装**：`war3.exe` / `Game.dll` / `d3d9.dll` / `Frozen Throne.exe` 实测全部 **False**；
- 同一事实在 `preflight_instance_pool` 上表现为 **`ok=false`**；
- 沙箱 `Maps\` 下只有 `光影测试.w3x`（**22,716 B**），而 T8 需要 `Maps/ShadowTest/` 下的两张图（其中低压应为 **5,859,935 B** 那张真实大图）。

## 4. 本记录确立的两条事实（对后续重要）

1. **脚本不伪造数据**：失败时指标全 0 且 `ok=False`，这是正确行为 —— 后续引用时**不得**把 0 当作"性能为 0"；
2. **该脚本失败时仍返回 `EXIT=0`** ⇒ **判成功必须看 `ok=True` 与各指标，绝不能只看退出码**。（与之成对的反面教训：runner 类脚本缺位置参数时 argparse 返回 `EXIT=2`，那不是回归。）

## 5. 解锁前置

- **(A) 人工**：按 `2026-09-18-option-a-manual-evidence-procedure.md` 进图（注意 T8 与 ① 用的不是同一张图）；
- **(B) 供给沙箱**：按 `2026-09-18-option-b-sandbox-provisioning-checklist.md`（含两张图的精确来源与目标路径）；供给完成后本脚本即可直接重跑。

## 6. 边界声明

本文档只记录一次**失败的有界重试**；**不包含任何性能结论**，**不得**被引用为基线数据。执行过程未替换现场 DLL（`A0A51AF2…`）、未部署、未 git 写。
