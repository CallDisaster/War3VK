# AutoTest 自动化与性能验证套件

本目录包含 War3 自动化测试 / 性能基线 / shadow trace 控制等"发布级"工具。
历史调试脚本（按 phase 分组）已经归档到 `_archive/`，不参与日常验证。

## 发布级脚本（合并主线必用）

| 文件 | 作用 |
|---|---|
| `war3_autotest_mcp.py` | MCP 服务核心：启动/进图/采样/截图/读取报告等一体化 API。所有自动化流程的底层依赖。 |
| `run_mcp.ps1` | 启动 MCP 服务（PowerShell 入口）。 |
| `dual_perf_baseline.py` | 双图性能基线脚本：高压（光影测试-高压.w3x）+ 低压（光影测试.w3x），各 30s × 多轮，输出 FPS / mainThread / GPU 等核心指标。**合并前性能护栏验收必用**。 |
| `shadow_pose_full_trace_control.py` | 控制 plane 动态启停 shadow pose full trace（写盘 JSONL）。问题诊断时手动开 15-30s trace。 |
| `requirements.txt` | Python 依赖清单。 |
| `README.txt` | 历史 README（YDWE 启动链路 / DBWIN 监听等说明）。 |
| `ydwe_launch_notes.txt` | YDWE -loadfile 启动链路逆向笔记。 |

## 性能护栏（合并前必跑）

```pwsh
# 在前台环境运行（不要在 isolated desktop 里跑性能基线，否则受 GPU present 阻塞污染）
py AutoTest\dual_perf_baseline.py
```

护栏标准：
- 高压地图（带桥/斜坡/装饰物）≥ 85 FPS
- 低压地图（光影测试.w3x）≥ 120 FPS

如果某轮低于护栏，先在前台 / clean machine state 复测一次再判定。
后台 Defender 扫描、热节流、其他游戏运行 都会污染 isolated desktop 的基线。

## 归档目录（`_archive/`）

| 子目录 | 内容 |
|---|---|
| `phase_7xx_history/` | Phase 7.30-7.116 各轮一次性诊断 / 验证脚本（约 100+ 个）。 |
| `analysis/` | trace JSONL 分析脚本（_analyze_*）。 |
| `ida_scripts/` | IDA MCP 调用脚本（_ida_*）。已经回写完命名 / 注释，仅作历史参考。 |
| `probes/` | 一次性探针脚本（_probe_*, _check_*, _find_* 等）。 |
| `logs/` | 历史 perf 报告 / 调试 log。 |

归档原则：
- **不删除**：所有调试历史完整保留，便于 root-cause review。
- **从 PATH 移除**：不再出现在 `AutoTest/` 主目录，避免给新人造成"哪个是当前用的"困惑。
- 任何归档脚本若需要重新启用，直接从 `_archive/` 拉回来即可。

## 本目录文件命名约定

新增脚本规则（写在这里防止后人继续扔垃圾）：

- `_phaseXXX_*.py`：临时调试，写完即用，**不要进 release**。一旦完成 phase 就归档到 `_archive/phase_7xx_history/`。
- 发布级脚本：必须用语义命名（`dual_perf_baseline.py`、`shadow_pose_full_trace_control.py` 等），名字直接说明"做什么"，不带 phase 编号。
- 一次性 IDA 调用：`_ida_*.py` 写完后立即移到 `_archive/ida_scripts/`。

合并前自检：
```pwsh
# AutoTest 主目录里不应该有任何 _phaseXXX_* 文件
Get-ChildItem AutoTest -File -Filter "_phase*"
```
