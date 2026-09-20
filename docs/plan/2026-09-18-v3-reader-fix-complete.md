# ✅ 版本 3 读方注册缺口已修复（三处联动 + 端到端见证）— 2026-09-18

> 承接 `2026-09-18-v3-reader-not-registered.md`（缺口发现与更正）。本文记录修复与验证。

## 1. 最终状态

```
写方（C++）  : result["version"] = firstSightUsed() ? 3 : 2;   -> 会发 v3
读方（Python）: PALETTE_OBJECT_BLOCK_FIELDS 注册 {1, 2, 3}      -> 现在接受 v3
```

实测（同一探针，修复前后对比）：

```
修复前  v3 generic-reader -> REJECTED  unknown paletteObject extension version 3 (registered: [1,2])
修复后  v3 generic-reader -> ACCEPTED
        v3 palette-analyzer -> 已越过版本判定，改为报 fixture 自身问题（见 §3）
```

## 2. 实际改了四处（比原计划的"三处"多一处）

| # | 文件 | 改动 |
| --- | --- | --- |
| 1 | `AutoTest/analyze_frame_evidence.py` | 新增 `PALETTE_OBJECT_FIRST_SIGHT_VERSION=3`；把 3 加入 `PALETTE_OBJECT_BLOCK_FIELDS`（顶层字段集与 v2 相同） |
| 2 | `AutoTest/analyze_palette_object_evidence.py` | 新增 `FIRST_SIGHT_COUNTER_FIELDS`；counters 校验由**精确集合相等**改为**按版本**（v1/v2 用原集合，v3 允许追加两个首见计数器） |
| 3 | `AutoTest/test_palette_object_evidence_analysis_static.py` | ①非法版本列表移除 3（改为 `0,4,7,99,-1,"2",2.0,True`）；②新增 **`test_explicit_version_3_is_registered_and_accepted_by_both_readers`** 正向端到端用例；③fixture 构造器 `envelope()` 在 v3 时补齐首见计数器（**照生产形状**） |
| 4 | `AutoTest/test_semantic_build_thread_gate_static.py` | **计划外的第二处同类冲突**：line 1975 也把 3 当非法版本 ⇒ 移除 3，并新增 `version=3 必须被接受并上报` 的正向断言 |

### 2.1 第 4 处是"边改边发现"的
修完前三处后全量静态出现 **1 个失败**，正是该门禁。这印证了我上一轮判断"三处必须同时改"的方向 ——
但**同时改的范围比我估计的大一处**：同类冲突有**两个**测试文件，不止一个。

## 3. 过程中修掉的两个自身缺陷（如实记录）

| 缺陷 | 现象 | 修正 |
| --- | --- | --- |
| fixture 补丁未做类型判断 | `format_version="2"`（非法版本用例）上执行 `>=` ⇒ `TypeError: '>=' not supported between str and int` | 加 `isinstance(int) and not isinstance(bool)` 守卫 |
| 探针脚本使用 segment=0 的旧 fixture | 修复后 palette 分析器报 `palette window segment must be >= 1 (got 0)`，一度看似仍有问题 | 该报错属**探针自身**的形状问题（真实用例用 `segment_chain_events(segment=1)`）⇒ 判据以真实测试为准，不以探针为准 |

## 4. 验证

```
analyze 静态测试          -> Ran 95 tests ... OK
全量静态脚本               -> 259 scripts, 0 failed
meson test                -> 85 Ok / 0 Fail
```

**关键区别**：这次的"全绿"与上一轮不同 —— 上一轮全绿**掩盖**了 v3 缺口（无用例覆盖），
本轮全绿是**包含**一条 v3 端到端用例（两个读方都断言接受 v3）之后的结果。

## 5. 临时探针已清理

`_check_v3_acceptance.py`、`_probe_desktop_api.py`、`_try_kill_war3.py` 三个临时脚本已删除，
不留在仓库里。

## 6. 目标完成度（更正后）

| 项 | 状态 |
| --- | --- |
| ① 检查点（c0 + c1 + 归档 + 回退信息 + 环境区分） | ✅ |
| ② 三项缺陷（R1/R2/R3） | ✅ |
| ③ 有版本正常观察链 | **✅ 本轮补齐** —— 写方 + 读方注册 + 按版本 counters + 双向测试（含 v3 端到端正向见证） |
| ④ 严格契约确认 + 真实入口对照 | 前三子问题 ✅（读码 + 纯谓词）；**第四子问题（是否误伤）缺运行期证据** |
| 套件 | 静态 259/0、meson 85/0、runnable 74/76；未跑 TDR/ABBA、前台门、**实机** |

## 7. 仍未闭合

- **实机非零 palette 导出**：仍无。v3 现在**可被读方接受**了，但仍**没有任何实机 v3 导出**被验证过；
- 批次 4 第④问第四子问题：仍只有读码 + 纯谓词证据；
- 阻塞：`War3.exe` pid 42212 仍存活，本会话无权终止（`taskkill /F /PID 42212` 可解）；
- 现场 DLL：基线 `F275545B…5CF07FF3`，未触碰；未提交、未部署。