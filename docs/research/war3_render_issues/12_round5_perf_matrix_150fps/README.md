# 第五轮配置矩阵与 150FPS 目标验证（2026-02-25）

## 目标
1. 在不改渲染语义底线（不崩溃）的前提下，完成第五轮“配置矩阵 + 稳定性”验证。  
2. 评估测试图是否可达 `150 FPS+`。  
3. 形成可回滚、可复验的配置结论与证据路径。

## 执行内容
### 1) 主矩阵（8 组，60s）
- 脚本：`AutoTest/run_round5_perf_matrix.py`
- 产物：`AutoTest/artifacts/round5_matrix/round5_matrix_results.json`
- 方法：每组执行 `ninja -C build32` + `run_quick_autotest(sample=60s, 2K全屏)`。

### 2) 上限探索矩阵（4 组，60s）
- 脚本：`AutoTest/run_round5_extra_matrix.py`
- 产物：`AutoTest/artifacts/round5_matrix/round5_extra_matrix_results.json`
- 方法：在主矩阵最佳配置基础上，重点隔离 mode1 阴影链路开销。

## 结果
### 主矩阵最佳
- 组合：`C2_perf_full_no_local_merge`
- 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_05_02_14.html`
- 指标：`avgFps=122.804`

### 上限探索最佳
- 组合：`E1_disable_shadow_capture_mode1`
- 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_05_16_23.html`
- 指标：`avgFps=209.268`

### 最终落地配置验证
- 报告：`E:\\Work\\War3\\WarVK\\Log\\war3_perf_report_auto_2026_02_25_05_23_00.html`
- 指标：`avgFps=196.917`, `avgFrameTimeMs=5.078`, `avgGpuTimeMs=1.168`
- 结论：`150 FPS+` 在实验档可达成，且自动化流程未出现崩溃。

## 关键配置（当前落地）
文件：`src/d3d9/war3/core/war3_internal_test_config.h`
- `kNativeMainLoopCoverageAnalysisMode=false`
- `kNativeDispatchLocalContextMergeEnabled=false`
- `kNativeShadowDisableShadowCaptureWhenMode1=false`（画质默认）

## 技术结论
1. 当前性能上限的决定性瓶颈在 `mode1 ShadowCapture` 链路。  
2. 在关闭 mode1 ShadowCapture 时，FPS 可显著突破 150（实测可到 200+），但该档仅用于上限分析。  
3. 若后续要在“保留 mode1 完整阴影语义”前提下继续提 FPS，优先方向应是 ShadowCapture 采集/回放分级与增量化，而不是继续堆开关。

## 已知限制
1. `AutoTest` 截图原链路基于 `CopyFromScreen`，窗口被覆盖时可能截到桌面。  
2. 已在 `AutoTest/war3_autotest_mcp.py` 改为 `PrintWindow` 优先；需 MCP 服务重启后生效。

## 回归建议
1. 收敛版本建议固定 2 套 profile：  
   - `Perf`: 关闭深层分析 + mode1 shadow capture 关闭。  
   - `Analysis`: 开启 MainLoop 深层追踪，仅用于诊断。  
2. 每次改动至少做 60s AutoTest，并记录：
   - `avgFps / avgFrameTimeMs / avgGpuTimeMs`
   - `ok / 崩溃与否`
   - 报告路径与 commit 对应关系
