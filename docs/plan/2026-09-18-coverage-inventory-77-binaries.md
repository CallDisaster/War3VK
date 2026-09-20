# 覆盖基线清点：**77 个测试二进制**，74 独立绿 + 3 驱动型 — 2026-09-18

## 1. 动机

round 264 发现：我一直在用「**我记得的**覆盖范围」评估目标，而不是去查**仓库里实际有什么**
（`war3_live_palette_selection_test` 的 4021 万次等价性检查一直存在，我却在 `render/tests/` 里找了很久，
它在 `semantic/tests/`）。

⇒ 本轮做一次**穷举清点**，建立真实基线。

## 2. 方法

```powershell
$exes = Get-ChildItem build32 -Recurse -Filter *_test.exe | Sort-Object Name
foreach ($e in $exes) { $out = & $e.FullName 2>&1; 记录 exit code + 含 SUMMARY/failure 的最后一行 }
```

（逐行写入 `E:\Work\alltests.txt` 后整体读取 —— 遵守「先落盘再检索」，避免流式截断丢行。）

## 3. 结果

```
总测试二进制 : 77
非零退出     : 3   —— 全部是「需要驱动」，不是失败
```

| 非零项 | 退出信息 | 性质 |
| --- | --- | --- |
| `war3_frame_recorder_defaults_0_test.exe` | `argc==3` | 需 Python 驱动传参（驱动已在 259 个静态脚本内） |
| `war3_frame_recorder_defaults_1_test.exe` | `argc==3` | 同上 |
| `war3_palette_object_wire_roundtrip_test.exe` | `FAIL: DXVK_WAR3_FRAME_EVIDENCE_PALETTE_OBJECT=1 is required` | 需 env + Python 驱动；**驱动已跑绿**：`CHECKS=1137 FAILURES=0` |

⇒ **74/77 独立运行即绿；3 个驱动型的两套驱动均已验证绿。**

## 4. 与目标直接相关的项（全部绿）

| 二进制 | 结果 |
| --- | --- |
| `war3_live_palette_selection_test` | **40,211,653 / 40,211,653 checks passed** |
| `war3_palette_object_evidence_test` | `SUMMARY: 24 passed, 0 failed` |
| `war3_palette_object_evidence_cost_test` | `all checks passed` |
| `war3_skin_palette_admission_test` | `SUMMARY: 6 passed, 0 failed` |
| `war3_runtime_group_palette_kernel_test` | `all T1-T17 passed` |
| `war3_runtime_group_palette_kernel_diff_test` | `coreMismatches=0 upperMismatches=0`（各 30 万次输入） |

## 5. 加上静态与 meson

```
AutoTest 全量静态 : 259 scripts, 0 failed
meson test       : Ok: 85   Fail: 0
```

## 6. ⚠️ 这份基线的边界（不得越过）

1. **它证明的是"当时那份源码"**（候选 DLL `5ADEDD5F…`，源码即当前工作树）。
   仓库此后若有改动，本基线**不再代表当前状态** —— 这与硬约束「不得声称全门禁通过」一致：
   我记录的是**某次运行的完整结果**，不是"当前状态永远通过"。
2. **它不证明实机行为** —— 上述全部是单元/契约/等价性层面的检查；
   实机部分只有：两次实机 A/B（修正①、修正②）与 ④ 的隔离桌面采集。
3. **驱动型的 3 个我没有在"裸跑"下通过** —— 它们的通过性依赖两个 Python 驱动，
   而驱动本身是静态测试的一部分（259/0）。**两者不可互相替代**，但合起来构成完整证据。

## 7. 现场

```
站点     : E:\Work\Warcraft III\d3d9.dll = F275545B…（基线）✅  War3 进程: none
候选 DLL : 36,289,280 B  SHA-256 5ADEDD5FE98228E193B1FB8203BE4F216650BCA3A3508D9C6F9710829A47918D
本轮未改任何文件（只读 + 运行既有测试二进制）。未提交、未部署。
```